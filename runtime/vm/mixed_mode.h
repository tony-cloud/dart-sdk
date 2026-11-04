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
// with mixed_mode_gates_arm64.S. The simulator works on this area directly
// (its register file aliases it), so a transition copies no registers beyond
// the gate's own save and restore.
struct MixedRegs {
  // x0..x30; x[31] is CSP; x[32] reads as zero (the interpreter's XZR source
  // slot, never written); x[33] absorbs writes to XZR.     offset 0
  int64_t x[34];
  int64_t v[32][2];      // q0..q31.                         offset 272
  uword c_stack_top;     // Top of the side stack.           offset 784
  uword exit_target;     // Native address to resume at.     offset 792
  Simulator* simulator;  // Per-thread simulator instance.   offset 800
  // Profiler support: the pre-decoded op the interpreter last branched to
  // from another function, i.e. a pc in the function being simulated;
  // kMixedSimEntering while no op has run yet (x[16] still holds the gate);
  // nullptr when unknown (the generic simulator). DartMixedSimEnter sets
  // kMixedSimEntering before it switches to the side stack; see
  // MixedMode::SampleSimulatedState.                    offset 808
  std::atomic<const void*> sim_op;
};
static_assert(sizeof(MixedRegs) == 816, "Keep in sync with the gate asm");
static_assert(offsetof(MixedRegs, sim_op) == 808, "Keep in sync with the asm");
static constexpr uword kMixedSimEntering = 1;  // A sim_op value.
static constexpr intptr_t kMixedZeroSlot = 32;
static constexpr intptr_t kMixedSinkSlot = 33;
static constexpr intptr_t kMixedRegisterSlots = 34;

// One record of a vmcode v1 link table (docs/ios-linker/COMPILER_LINKER.md
// section 6): the patch instructions entry whose payload starts at
// `patch_offset` is linked to the identical release code at
// `release_offset`. Offsets are relative to the respective instructions
// image (_kDartSnapshotText). Entries without a record are unlinked.
struct MixedLink {
  uint32_t patch_offset;
  uint32_t release_offset;
  uint32_t size;
  uint32_t flags;  // Must be 0 in version 1.
};
static_assert(sizeof(MixedLink) == 16, "vmcode v1 link record");

class MixedMode : public AllStatic {
 public:
  // Called by the embedder before the patch snapshot is deserialized.
  // `patch_text` is the instructions image of the patch (readable, not
  // executable); `base_text` is the instructions image of the release
  // (executable), or nullptr if none is mapped. `links` are the linker's
  // records (the embedder has already checked the vmcode hashes). If `links`
  // is nullptr the runtime links by identity (equal bytes at equal offsets),
  // which is only valid for two compiles that share one layout (testing).
  //
  // Returns false (and sets *error) if the patch must not be used; the
  // embedder must then run the release unpatched. Mixed mode stays inactive.
  static bool RegisterImages(const uint8_t* patch_text,
                             const uint8_t* base_text,
                             const MixedLink* links,
                             intptr_t link_count,
                             const char** error);

  // Undoes RegisterImages after a failure (before any code was entered).
  static void Deactivate();

  // True once a patch image has been linked.
  static bool active() { return active_; }

  // Is `instructions_image` the registered patch image?
  static bool IsPatchImage(const uint8_t* instructions_image) {
    return instructions_image != nullptr &&
           instructions_image == patch_image_;
  }

  // Decides, for every entry of the patch's instructions table, whether it
  // is linked, and validates the link table against it. Called by the
  // deserializer before any object is allocated. Returns nullptr on success,
  // otherwise an error message; mixed mode is then deactivated.
  static const char* LinkImage(uword start_pc,
                               uword end_pc,
                               intptr_t length,
                               const uint32_t* pc_offsets,
                               intptr_t pc_offset_stride);

  // After deserialization: rejects the patch if a stub that C++ enters
  // directly (without THR in its register) would be entered through a gate.
  static const char* CheckNativeOnlyEntries();

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
      return BaseToPatch(pc);
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
  // As above, also setting *delta (release - patch address, if linked).
  static bool LookupEntry(uword pc,
                          uword* lo,
                          uword* hi,
                          bool* linked,
                          intptr_t* delta);
  // Changes whenever the link state changes (simulator lookup caches).
  static uword link_generation() { return link_generation_; }

  // Release address of the linked patch pc `patch_pc`.
  static uword PatchToBase(uword patch_pc);
  // Patch pc for a pc inside linked release code.
  static uword BaseToPatch(uword base_pc);

  // Instructions-table queries for the simulator's pre-decoder (valid while
  // active; the table is immutable after LinkImage).
  static intptr_t EntryCount();
  // Index of the entry containing `pc`, or -1.
  static intptr_t EntryIndex(uword pc);
  static uword EntryStart(intptr_t i);
  // End of entry i, including trailing alignment padding.
  static uword EntryLimit(intptr_t i);
  static bool EntryLinked(intptr_t i);
  static intptr_t EntryDelta(intptr_t i);
  static uword GateCount();
  // Whether transitions are counted (DART_MIXED_VERBOSE).
  static bool count_transitions() { return count_transitions_; }

  // Allocates the per-thread register area and side stack.
  static constexpr intptr_t kSideStackSize = 512 * KB;
  static MixedRegs* NewThreadArea();

  // Is `sp` on the side stack of `area`, i.e. is the thread inside
  // DartMixedSimEnter / DartMixedRun (simulating, or about to)?
  static bool OnSideStack(const MixedRegs* area, uword sp) {
    if (area == nullptr || area->c_stack_top == 0) return false;
    const uword end = area->c_stack_top + kSideStackEndGap;
    return sp - (end - kSideStackSize) < static_cast<uword>(kSideStackSize);
  }
  static constexpr intptr_t kSideStackEndGap = 64;

  // Profiler support. Called while the thread owning `area` is interrupted
  // at an arbitrary instruction (suspended, or in a signal handler on that
  // thread); reads only, takes no locks, never dereferences the simulated
  // stack. When `csp` (the interrupted CPU stack pointer) is not on the side
  // stack, returns kNotSimulating. Otherwise returns kSimulated and the top
  // simulated frame's state (pc is a patch pc; fp/sp/lr are the simulated
  // registers, still to be validated by the caller against the thread's
  // stack bounds) when that state is known to be consistent, or
  // kSimulatedUnknown when it is not (the generic simulator, an op table of
  // an old link generation).
  enum SampleState { kNotSimulating, kSimulated, kSimulatedUnknown };
  static SampleState SampleSimulatedState(const MixedRegs* area,
                                          uword csp,
                                          uword* pc,
                                          uword* fp,
                                          uword* sp,
                                          uword* lr);
  static std::atomic<int64_t> profiler_simulated_;
  static std::atomic<int64_t> profiler_unknown_;
  static void DeleteThreadArea(MixedRegs* area);

  // Statistics.
  static std::atomic<int64_t> native_to_sim_;
  static std::atomic<int64_t> sim_to_native_;
  static void PrintStats();

  static constexpr intptr_t kGateSizeLog2 = 3;
  // Gates reserved for exception-handler pcs and other lazily bound targets
  // that the link-time budget cannot count exactly.
  static constexpr intptr_t kGateReserve = 1024;

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
  static bool count_transitions_;
  static uword link_generation_;
  static const uint8_t* patch_image_;
  static const uint8_t* base_image_;
};

// Statistics of the pre-decoded interpreter (simulator_arm64_mixed.cc).
void MixedInterpPrintStats();

// The patch pc of a pre-decoded op published in MixedRegs::sim_op, or 0 if
// `op` is not in an op table of the current link generation. Lock-free and
// async-signal safe (simulator_arm64_mixed.cc).
uword MixedInterpOpPc(const void* op);

// Entry point called from the gate assembly with the saved registers. Runs
// the simulator and returns the native address at which to resume.
extern "C" uword DartMixedRun(MixedRegs* regs);

// Links the registered patch image from its isolate snapshot before any
// isolate exists (app_snapshot.cc). Returns nullptr or a static error.
const char* MixedModeLinkSnapshot(const uint8_t* snapshot_data,
                                  const uint8_t* snapshot_instructions);

}  // namespace dart

#endif  // defined(DART_MIXED_MODE)

#endif  // RUNTIME_VM_MIXED_MODE_H_
