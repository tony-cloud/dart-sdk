// Copyright (c) 2026, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

// Mixed-mode execution of an AOT patch snapshot (feasibility spike).
//
// The patch snapshot's data (objects, pools, tables) is used as the program,
// but its instructions are mapped read-only and are never executed by the
// CPU. Each patch code entry is either
//   * linked: its machine code is identical to the code at the same place in
//     the release ("base") image, and so the CPU runs the base's signed copy;
//   * unlinked: it is executed by the ARM64 simulator, reading the patch's
//     instructions as data.
//
// Native code can only reach unlinked code through "gates": a fixed table of
// tiny trampolines compiled into the runtime binary. Each gate is bound to a
// patch pc; branching to the gate saves the CPU registers into a per-thread
// area, runs the simulator from that pc on a side stack, and resumes the CPU
// at the native address the simulated code eventually branches to. Simulated
// and native Dart frames share the ordinary thread stack, so a frame chain
// is an ordinary frame chain; return addresses that are gate addresses or base
// addresses are translated to patch pcs whenever the VM inspects a frame.
//
// See docs/ios-linker/RUNTIME.md in the workspace for the design.

#ifndef RUNTIME_VM_MIXED_MODE_H_
#define RUNTIME_VM_MIXED_MODE_H_

#include "platform/globals.h"

#if defined(DART_MIXED_MODE)

#include <atomic>

#include "vm/allocation.h"

namespace dart {

class Simulator;

// Saved CPU state at a native <-> simulator transition. The layout is shared
// with mixed_mode_gates_arm64.S.
struct MixedRegs {
  int64_t x[32];         // x0..x30; x[31] is CSP.          offset 0
  int64_t v[32][2];      // q0..q31.                         offset 256
  uword c_stack_top;     // Top of the side stack.           offset 768
  uword exit_target;     // Native address to resume at.     offset 776
  Simulator* simulator;  // Per-thread simulator instance.   offset 784
  uword padding;
};
static_assert(sizeof(MixedRegs) == 800, "Keep in sync with the gate asm");

class MixedMode : public AllStatic {
 public:
  // Called by the embedder before the patch snapshot is deserialized.
  // `patch_text` is the instructions image of the patch (readable, not
  // executable); `base_text` is the instructions image of the release
  // (executable). Pass base_text == nullptr to simulate everything.
  static void RegisterImages(const uint8_t* patch_text,
                             const uint8_t* base_text);

  // True once a patch image has been linked.
  static bool active() { return active_; }

  // Is `instructions_image` the registered patch image?
  static bool IsPatchImage(const uint8_t* instructions_image) {
    return instructions_image != nullptr &&
           instructions_image == patch_image_;
  }

  // Decides, for every entry of the patch's instructions table, whether it
  // is linked. Called by the deserializer once the table exists.
  static void LinkImage(uword start_pc,
                        uword end_pc,
                        intptr_t length,
                        const uint32_t* pc_offsets,
                        intptr_t pc_offset_stride);

  // Native-callable address for the patch entry point `patch_entry`, which
  // belongs to instructions table entry `index`.
  static uword NativeEntry(intptr_t index, uword patch_entry);

  // Maps a native return address / pc found in a frame to the patch pc that
  // describes it. Identity for pcs that are neither gates nor base pcs.
  static inline uword ToPatchPc(uword pc) {
    if (pc - gates_start_ < gates_size_) {
      return gate_target_[(pc - gates_start_) >> kGateSizeLog2];
    }
    if (pc - base_start_ < base_size_) {
      return pc - base_start_ + patch_start_;
    }
    return pc;
  }

  // Maps a patch pc to an address the CPU may branch to: the base copy for
  // linked code, otherwise a gate bound to the pc.
  static uword ToNativePc(uword patch_pc);

  static inline bool InPatchText(uword pc) {
    return pc - patch_start_ < patch_size_;
  }
  static inline bool IsGate(uword pc) { return pc - gates_start_ < gates_size_; }
  static inline uword GateTarget(uword gate) {
    return gate_target_[(gate - gates_start_) >> kGateSizeLog2];
  }

  // Gate the CPU should return to when native code called from the
  // simulated instruction before `return_pc` returns.
  static uword ReturnGateFor(uword return_pc);

  // Looks up the instructions-table entry containing patch pc `pc`.
  // Returns false if `pc` is not inside the patch text. On success, sets
  // [*lo, *hi) to the entry's range and *linked.
  static bool LookupEntry(uword pc, uword* lo, uword* hi, bool* linked);

  static inline uword PatchToBase(uword patch_pc) {
    return patch_pc - patch_start_ + base_start_;
  }

  // Allocates the per-thread register area and side stack.
  static MixedRegs* NewThreadArea();
  static void DeleteThreadArea(MixedRegs* area);

  // Statistics.
  static std::atomic<int64_t> native_to_sim_;
  static std::atomic<int64_t> sim_to_native_;
  static void PrintStats();

  static constexpr intptr_t kGateSizeLog2 = 3;

  static uword gates_start_;
  static uword gates_size_;
  static uword* gate_target_;
  static uword base_start_;
  static uword base_size_;
  static uword patch_start_;
  static uword patch_size_;

 private:
  static uword AllocateGate(uword patch_pc);

  static bool active_;
  static const uint8_t* patch_image_;
  static const uint8_t* base_image_;
};

// Entry point called from the gate assembly with the saved registers. Runs
// the simulator and returns the native address at which to resume.
extern "C" uword DartMixedRun(MixedRegs* regs);

}  // namespace dart

#endif  // defined(DART_MIXED_MODE)

#endif  // RUNTIME_VM_MIXED_MODE_H_
