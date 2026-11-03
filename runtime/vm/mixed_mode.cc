// Copyright (c) 2026, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/mixed_mode.h"

#if defined(DART_MIXED_MODE)

#include <stdlib.h>
#include <string.h>

#include "platform/syslog.h"
#include "vm/image_snapshot.h"
#include "vm/os.h"
#include "vm/os_thread.h"
#include "vm/simulator.h"
#include "vm/thread.h"
#include "vm/virtual_memory.h"

extern "C" {
extern char DartMixedGatesStart[];
extern char DartMixedGatesEnd[];
}

namespace dart {

bool MixedMode::active_ = false;
const uint8_t* MixedMode::patch_image_ = nullptr;
const uint8_t* MixedMode::base_image_ = nullptr;
uword MixedMode::gates_start_ = 0;
uword MixedMode::gates_size_ = 0;
uword* MixedMode::gate_target_ = nullptr;
uword MixedMode::base_start_ = 0;
uword MixedMode::base_size_ = 0;
uword MixedMode::patch_start_ = 0;
uword MixedMode::patch_size_ = 0;
std::atomic<int64_t> MixedMode::native_to_sim_ = {0};
std::atomic<int64_t> MixedMode::sim_to_native_ = {0};

namespace {

// Sorted entry starts of the patch instructions table, and whether each
// entry is linked. Written once by LinkImage, read-only afterwards.
intptr_t entry_count = 0;
uword* entry_start = nullptr;  // entry_count + 1 elements (last = end).
uint8_t* entry_linked = nullptr;
uword gate_count = 0;
std::atomic<uword> next_gate = {0};
Mutex* gate_mutex = nullptr;

// Open-addressing map from patch pc to gate index. Readers do not lock:
// the gate index is published before the key.
struct GateSlot {
  std::atomic<uword> pc;
  uword gate;
};
constexpr uword kGateMapSize = 1 << 19;  // Power of two, > 2 * max gates.
GateSlot* gate_map = nullptr;

inline uword HashPc(uword pc) {
  return ((pc >> 2) * 0x9E3779B97F4A7C15ULL) >> (64 - 19);
}

bool EnvFlag(const char* name) {
  const char* value = getenv(name);
  return value != nullptr && value[0] != '\0' && value[0] != '0';
}

uword ImageEnd(const uint8_t* image) {
  TextImage text(image);
  return reinterpret_cast<uword>(text.object_start()) + text.object_size();
}

intptr_t FindEntryIndex(uword pc) {
  // Largest i with entry_start[i] <= pc.
  intptr_t lo = 0, hi = entry_count - 1;
  if (entry_count == 0 || pc < entry_start[0] || pc >= entry_start[entry_count])
    return -1;
  while (lo < hi) {
    const intptr_t mid = (lo + hi + 1) / 2;
    if (entry_start[mid] <= pc) {
      lo = mid;
    } else {
      hi = mid - 1;
    }
  }
  return lo;
}

}  // namespace

void MixedMode::RegisterImages(const uint8_t* patch_text,
                               const uint8_t* base_text) {
  if (Thread::mixed_area_offset() != DART_MIXED_AREA_OFFSET) {
    FATAL("Mixed mode: rebuild with dart_mixed_area_offset=%" Pd
          " (built with %d)",
          Thread::mixed_area_offset(), DART_MIXED_AREA_OFFSET);
  }
  patch_image_ = patch_text;
  base_image_ = base_text;
  gates_start_ = reinterpret_cast<uword>(DartMixedGatesStart);
  gates_size_ = reinterpret_cast<uword>(DartMixedGatesEnd) - gates_start_;
  gate_count = gates_size_ >> kGateSizeLog2;
  gate_target_ = reinterpret_cast<uword*>(calloc(gate_count, sizeof(uword)));
  gate_map = reinterpret_cast<GateSlot*>(calloc(kGateMapSize, sizeof(GateSlot)));
  RELEASE_ASSERT(gate_count * 2 < kGateMapSize);
  gate_mutex = new Mutex();
}

uword MixedMode::AllocateGate(uword patch_pc) {
  MutexLocker ml(gate_mutex);
  uword h = HashPc(patch_pc);
  while (true) {
    const uword key = gate_map[h].pc.load(std::memory_order_relaxed);
    if (key == patch_pc) return gate_map[h].gate;
    if (key == 0) break;
    h = (h + 1) & (kGateMapSize - 1);
  }
  const uword index = next_gate.fetch_add(1);
  if (index >= gate_count) {
    FATAL("Mixed mode: out of gates (%" Pd ")", gate_count);
  }
  gate_target_[index] = patch_pc;
  const uword gate = gates_start_ + (index << kGateSizeLog2);
  gate_map[h].gate = gate;
  gate_map[h].pc.store(patch_pc, std::memory_order_release);
  return gate;
}

uword MixedMode::ReturnGateFor(uword return_pc) {
  uword h = HashPc(return_pc);
  while (true) {
    const uword key = gate_map[h].pc.load(std::memory_order_acquire);
    if (key == return_pc) return gate_map[h].gate;
    if (key == 0) break;
    h = (h + 1) & (kGateMapSize - 1);
  }
  return AllocateGate(return_pc);
}

uword MixedMode::ToNativePc(uword patch_pc) {
  if (!active_ || !InPatchText(patch_pc)) return patch_pc;
  uword lo, hi;
  bool linked;
  if (LookupEntry(patch_pc, &lo, &hi, &linked) && linked) {
    return PatchToBase(patch_pc);
  }
  return ReturnGateFor(patch_pc);
}

bool MixedMode::LookupEntry(uword pc, uword* lo, uword* hi, bool* linked) {
  const intptr_t i = FindEntryIndex(pc);
  if (i < 0) return false;
  *lo = entry_start[i];
  *hi = entry_start[i + 1];
  *linked = entry_linked[i] != 0;
  return true;
}

void MixedMode::LinkImage(uword start_pc,
                          uword end_pc,
                          intptr_t length,
                          const uint32_t* pc_offsets,
                          intptr_t pc_offset_stride) {
  const bool verbose = EnvFlag("DART_MIXED_VERBOSE");
  const bool unlink_all = EnvFlag("DART_MIXED_UNLINK_ALL");

  patch_start_ = reinterpret_cast<uword>(patch_image_);
  patch_size_ = end_pc - patch_start_;
  if (base_image_ != nullptr) {
    base_start_ = reinterpret_cast<uword>(base_image_);
    base_size_ = ImageEnd(base_image_) - base_start_;
  }

  entry_count = length;
  entry_start = reinterpret_cast<uword*>(malloc((length + 1) * sizeof(uword)));
  entry_linked = reinterpret_cast<uint8_t*>(calloc(length, 1));
  for (intptr_t i = 0; i < length; i++) {
    entry_start[i] = start_pc + pc_offsets[i * pc_offset_stride];
  }
  entry_start[length] = end_pc;
  if (verbose) {
    for (intptr_t i = 0; i < 4 && i < length; i++) {
      OS::PrintErr("mixed: entry %" Pd " at offset %#" Px "\n", i,
                   entry_start[i] - start_pc);
    }
  }

  // Pass 1: an entry may be linked if the release has identical bytes at the
  // same image offset. (Spike: identity mapping between two compiles. The
  // real link table comes from the compiler-side linker.)
  intptr_t identical = 0;
  for (intptr_t i = 0; i < length; i++) {
    const uword lo = entry_start[i], hi = entry_start[i + 1];
    if (unlink_all || base_image_ == nullptr) continue;
    const uword base_lo = PatchToBase(lo);
    if (base_lo + (hi - lo) > base_start_ + base_size_) continue;
    if (memcmp(reinterpret_cast<void*>(lo), reinterpret_cast<void*>(base_lo),
               hi - lo) == 0) {
      entry_linked[i] = 1;
      identical++;
    }
  }

  // Pass 2: a linked entry must not reach unlinked code by a pc-relative
  // branch or address computation, since in the release that instruction
  // reaches the release's stale copy. Iterate to a fixed point.
  intptr_t cascaded = 0;
  bool changed = true;
  while (changed) {
    changed = false;
    for (intptr_t i = 0; i < length; i++) {
      if (!entry_linked[i]) continue;
      const uword lo = entry_start[i], hi = entry_start[i + 1];
      for (uword pc = lo; pc + 4 <= hi; pc += 4) {
        const uint32_t instr = *reinterpret_cast<const uint32_t*>(pc);
        uword target = 0;
        int kind = 0;
        if ((instr & 0x7C000000) == 0x14000000) {  // b / bl imm26
          kind = 1;
          const int64_t imm26 = static_cast<int32_t>(instr << 6) >> 6;
          target = pc + (imm26 << 2);
        } else if ((instr & 0x9F000000) == 0x10000000) {  // adr
          kind = 2;
          int64_t imm = ((instr >> 5) & 0x7FFFF) << 2 | ((instr >> 29) & 3);
          imm = (imm << 43) >> 43;
          target = pc + imm;
        } else if ((instr & 0x9F000000) == 0x90000000) {  // adrp
          kind = 3;
          int64_t imm = ((instr >> 5) & 0x7FFFF) << 2 | ((instr >> 29) & 3);
          imm = (imm << 43) >> 43;
          target = (pc & ~static_cast<uword>(0xFFF)) + (imm << 12);
        } else {
          continue;
        }
        if (target >= lo && target < hi) continue;
        const intptr_t j = FindEntryIndex(target);
        if (j < 0) {
          // Leaves the instructions: not an expected code reference.
          if (verbose) {
            OS::PrintErr("mixed: entry %" Pd " kind %d at +%#" Px
                         " targets outside text: %#" Px " (instr %08x)\n",
                         i, kind, pc - lo, target, instr);
          }
          entry_linked[i] = 0;
          changed = true;
          cascaded++;
          break;
        }
        if (!entry_linked[j]) {
          entry_linked[i] = 0;
          changed = true;
          cascaded++;
          break;
        }
      }
    }
  }

  intptr_t linked = 0;
  uword linked_bytes = 0, total_bytes = 0;
  for (intptr_t i = 0; i < length; i++) {
    const uword size = entry_start[i + 1] - entry_start[i];
    total_bytes += size;
    if (entry_linked[i]) {
      linked++;
      linked_bytes += size;
    }
  }
  active_ = true;
  if (verbose) {
    OS::PrintErr(
        "mixed: entries=%" Pd " identical=%" Pd " cascade-unlinked=%" Pd
        " linked=%" Pd " unlinked=%" Pd " linked-bytes=%" Pu "/%" Pu
        " (%.3f%%) patch=[%#" Px ",%#" Px ") base=%#" Px " gates=%" Pu "\n",
        length, identical, cascaded, linked, length - linked, linked_bytes,
        total_bytes, total_bytes == 0 ? 0.0 : 100.0 * linked_bytes / total_bytes,
        patch_start_, patch_start_ + patch_size_, base_start_, gate_count);
    for (intptr_t i = 0; i < length && !unlink_all; i++) {
      if (!entry_linked[i]) {
        OS::PrintErr("mixed: unlinked entry %" Pd " offset %#" Px " size %" Pu
                     "\n",
                     i, entry_start[i] - patch_start_,
                     entry_start[i + 1] - entry_start[i]);
      }
    }
  }
}

uword MixedMode::NativeEntry(intptr_t index, uword patch_entry) {
  ASSERT(active_);
  if (entry_linked[index]) return PatchToBase(patch_entry);
  return ReturnGateFor(patch_entry);
}

MixedRegs* MixedMode::NewThreadArea() {
  auto* area = reinterpret_cast<MixedRegs*>(calloc(1, sizeof(MixedRegs)));
  // The side stack only holds the simulator's own C++ frames (and signal
  // handler frames while simulating); Dart frames stay on the thread stack.
  constexpr intptr_t kSideStackSize = 512 * KB;
  VirtualMemory* stack =
      VirtualMemory::Allocate(kSideStackSize, false, "dart-mixed-sim-stack");
  RELEASE_ASSERT(stack != nullptr);
  area->c_stack_top = stack->end() - 64;
  return area;
}

void MixedMode::DeleteThreadArea(MixedRegs* area) {
  // The side stack and simulator are leaked in this spike.
  free(area);
}

void MixedMode::PrintStats() {
  if (!EnvFlag("DART_MIXED_VERBOSE")) return;
  OS::PrintErr("mixed: native->sim %" Pd64 " sim->native %" Pd64
               " gates-used %" Pu "\n",
               native_to_sim_.load(), sim_to_native_.load(), next_gate.load());
}

extern "C" uword DartMixedRun(MixedRegs* regs) {
  Simulator* sim = regs->simulator;
  if (sim == nullptr) {
    sim = new Simulator();
    regs->simulator = sim;
  }
  MixedMode::native_to_sim_.fetch_add(1, std::memory_order_relaxed);
  return sim->RunMixed(regs);
}

}  // namespace dart

#endif  // defined(DART_MIXED_MODE)
