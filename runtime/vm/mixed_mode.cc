// Copyright (c) 2026, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

#include "vm/mixed_mode.h"

#if defined(DART_MIXED_MODE)

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/syslog.h"
#include "vm/image_snapshot.h"
#include "vm/os.h"
#include "vm/os_thread.h"
#include "vm/simulator.h"
#include "vm/stub_code.h"
#include "vm/thread.h"
#include "vm/virtual_memory.h"

extern "C" {
extern char DartMixedGatesStart[];
extern char DartMixedGatesEnd[];
// Offset of Thread::mixed_area_, read by DartMixedSimEnter (gates assembly).
// A variable rather than an assembler constant, so one source works for
// every build configuration.
__attribute__((visibility("hidden"))) uintptr_t DartMixedAreaOffset = 0;
// One byte per gate, read by DartMixedSimEnter: nonzero when v8..v31 are dead
// at the gate's target, so the gate need not save them. Written by the
// interpreter (simulator_arm64_mixed.cc).
__attribute__((visibility("hidden"))) uint8_t* DartMixedGateFlags = nullptr;
}

namespace dart {

bool MixedMode::active_ = false;
bool MixedMode::count_transitions_ = false;
uword MixedMode::link_generation_ = 1;
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

// Instructions-table entries of the patch, and their link state. Written once
// by LinkImage, read-only afterwards.
intptr_t entry_count = 0;
uword* entry_start = nullptr;  // entry_count + 1 elements (last = end).
uint8_t* entry_linked = nullptr;
intptr_t* entry_delta = nullptr;  // base address - patch address (linked).
// End of entry i's code: entry_start[i + 1], or less when a link record
// names a payload followed by alignment padding.
uword* entry_end = nullptr;

// Linked release ranges sorted by address, for BaseToPatch.
struct BaseRange {
  uword lo;
  uword hi;
  intptr_t delta;  // base - patch
};
BaseRange* base_ranges = nullptr;
intptr_t base_range_count = 0;

// Link records given by the embedder (copied), or nullptr for identity mode.
MixedLink* table_links = nullptr;
intptr_t table_link_count = 0;

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
  if (entry_count == 0 || pc < entry_start[0] ||
      pc >= entry_start[entry_count]) {
    return -1;
  }
  intptr_t lo = 0, hi = entry_count - 1;
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

// Pc-relative instructions. kind: 0 = none, 1 = b, 2 = bl, 3 = any other
// pc-relative branch, literal load or address computation.
int PcRelative(uword pc, uint32_t instr, uword* target) {
  if ((instr & 0x7C000000) == 0x14000000) {  // b / bl imm26
    const int64_t imm26 = static_cast<int32_t>(instr << 6) >> 6;
    *target = pc + (imm26 << 2);
    return (instr & 0x80000000) != 0 ? 2 : 1;
  }
  int64_t imm;
  if ((instr & 0xFF000010) == 0x54000000 ||  // b.cond
      (instr & 0x7E000000) == 0x34000000 ||  // cbz / cbnz
      (instr & 0x3B000000) == 0x18000000) {  // ldr (literal)
    imm = static_cast<int32_t>(((instr >> 5) & 0x7FFFF) << 13) >> 13;
    *target = pc + (imm << 2);
    return 3;
  }
  if ((instr & 0x7E000000) == 0x36000000) {  // tbz / tbnz
    imm = static_cast<int32_t>(((instr >> 5) & 0x3FFF) << 18) >> 18;
    *target = pc + (imm << 2);
    return 3;
  }
  if ((instr & 0x1F000000) == 0x10000000) {  // adr / adrp
    imm = ((instr >> 5) & 0x7FFFF) << 2 | ((instr >> 29) & 3);
    imm = (imm << 43) >> 43;
    if ((instr & 0x80000000) != 0) {
      // adrp: always treated as leaving the entry (the linker refuses it).
      *target = ~static_cast<uword>(0);
    } else {
      *target = pc + imm;
    }
    return 3;
  }
  return 0;
}

// Whether linked entry i may run from the release: every instruction equals
// the release copy, except direct branches (b/bl) leaving the entry, which in
// the release must reach the release copy of a linked patch entry at the same
// offset (the displacement differs when the layout differs). Other
// pc-relative references leaving the entry are not allowed.
bool VerifyEntry(intptr_t i) {
  const uword lo = entry_start[i], hi = entry_end[i];
  const intptr_t delta = entry_delta[i];
  for (uword pc = lo; pc + 4 <= hi; pc += 4) {
    const uint32_t pw = *reinterpret_cast<const uint32_t*>(pc);
    const uint32_t bw = *reinterpret_cast<const uint32_t*>(pc + delta);
    uword pt = 0;
    const int kind = PcRelative(pc, pw, &pt);
    if (kind == 0 || (pt >= lo && pt < hi)) {
      if (pw != bw) return false;
      continue;
    }
    if (kind == 3) return false;
    const intptr_t j = FindEntryIndex(pt);
    if (j < 0 || !entry_linked[j]) return false;
    uword bt = 0;
    if (PcRelative(pc + delta, bw, &bt) != kind) return false;
    if (bt != pt + entry_delta[j]) return false;
  }
  return true;
}

bool IsCall(uint32_t instr) {
  return (instr & 0xFC000000) == 0x94000000 ||  // bl
         (instr & 0xFFFFFC1F) == 0xD63F0000;    // blr
}

void FreeLinkState() {
  free(entry_start);
  free(entry_linked);
  free(entry_delta);
  free(entry_end);
  free(base_ranges);
  entry_start = nullptr;
  entry_linked = nullptr;
  entry_delta = nullptr;
  entry_end = nullptr;
  base_ranges = nullptr;
  entry_count = 0;
  base_range_count = 0;
}

}  // namespace

bool MixedMode::RegisterImages(const uint8_t* patch_text,
                               const uint8_t* base_text,
                               const MixedLink* links,
                               intptr_t link_count,
                               const char** error) {
  DartMixedAreaOffset = Thread::mixed_area_offset();
  count_transitions_ = EnvFlag("DART_MIXED_VERBOSE");
  *error = nullptr;
  if (patch_text == nullptr) {
    *error = "no patch image";
    return false;
  }
  if (links == nullptr && base_text == nullptr) {
    *error = "identity linking needs the release image";
    return false;
  }
  if (links != nullptr) {
    if (link_count > 0 && base_text == nullptr) {
      *error = "patch links release code but no release image is mapped";
      return false;
    }
    const uword patch_size =
        ImageEnd(patch_text) - reinterpret_cast<uword>(patch_text);
    const uword base_size =
        base_text == nullptr
            ? 0
            : ImageEnd(base_text) - reinterpret_cast<uword>(base_text);
    uint64_t previous_end = 0;
    for (intptr_t i = 0; i < link_count; i++) {
      const MixedLink& l = links[i];
      if (l.flags != 0 || l.size == 0 || (l.size & 3) != 0 ||
          (l.patch_offset & 3) != 0 || (l.release_offset & 3) != 0) {
        *error = "malformed link record";
        return false;
      }
      if (l.patch_offset < previous_end) {
        *error = "link records are not sorted or overlap";
        return false;
      }
      previous_end = static_cast<uint64_t>(l.patch_offset) + l.size;
      if (previous_end > patch_size ||
          static_cast<uint64_t>(l.release_offset) + l.size > base_size) {
        *error = "link record outside the patch or release image";
        return false;
      }
    }
    table_link_count = link_count;
    table_links = reinterpret_cast<MixedLink*>(
        malloc(Utils::Maximum<intptr_t>(link_count, 1) * sizeof(MixedLink)));
    memcpy(table_links, links, link_count * sizeof(MixedLink));
  }

  patch_image_ = patch_text;
  base_image_ = base_text;
  gates_start_ = reinterpret_cast<uword>(DartMixedGatesStart);
  gate_count =
      (reinterpret_cast<uword>(DartMixedGatesEnd) - gates_start_) >>
      kGateSizeLog2;
  if (const char* limit = getenv("DART_MIXED_GATE_LIMIT")) {
    // Testing hook: pretend the engine has fewer gates.
    gate_count = Utils::Minimum<uword>(gate_count, strtoull(limit, nullptr, 0));
  }
  gates_size_ = gate_count << kGateSizeLog2;
  if (gate_target_ == nullptr) {
    gate_target_ = reinterpret_cast<uword*>(calloc(
        (reinterpret_cast<uword>(DartMixedGatesEnd) - gates_start_) >>
            kGateSizeLog2,
        sizeof(uword)));
    gate_map =
        reinterpret_cast<GateSlot*>(calloc(kGateMapSize, sizeof(GateSlot)));
    DartMixedGateFlags = reinterpret_cast<uint8_t*>(
        calloc((reinterpret_cast<uword>(DartMixedGatesEnd) - gates_start_) >>
                   kGateSizeLog2,
               1));
    gate_mutex = new Mutex();
  }
  RELEASE_ASSERT(gate_count * 2 < kGateMapSize);
  return true;
}

void MixedMode::Deactivate() {
  active_ = false;
  link_generation_++;
  patch_image_ = nullptr;
  base_image_ = nullptr;
  base_start_ = base_size_ = 0;
  patch_start_ = patch_size_ = 0;
  gates_size_ = 0;
  free(table_links);
  table_links = nullptr;
  table_link_count = 0;
  FreeLinkState();
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
    // The link-time budget (LinkImage) makes this unreachable unless the
    // reserve for lazily bound targets is exhausted.
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
  const intptr_t i = FindEntryIndex(patch_pc);
  if (i >= 0 && entry_linked[i]) return patch_pc + entry_delta[i];
  return ReturnGateFor(patch_pc);
}

uword MixedMode::PatchToBase(uword patch_pc) {
  const intptr_t i = FindEntryIndex(patch_pc);
  RELEASE_ASSERT(i >= 0 && entry_linked[i]);
  return patch_pc + entry_delta[i];
}

uword MixedMode::BaseToPatch(uword base_pc) {
  intptr_t lo = 0, hi = base_range_count - 1;
  while (lo <= hi) {
    const intptr_t mid = (lo + hi) / 2;
    if (base_pc < base_ranges[mid].lo) {
      hi = mid - 1;
    } else if (base_pc >= base_ranges[mid].hi) {
      lo = mid + 1;
    } else {
      return base_pc - base_ranges[mid].delta;
    }
  }
  return base_pc;  // Not linked release code; leave it untranslated.
}

bool MixedMode::LookupEntry(uword pc,
                            uword* lo,
                            uword* hi,
                            bool* linked,
                            intptr_t* delta) {
  const intptr_t i = FindEntryIndex(pc);
  if (i < 0) return false;
  *lo = entry_start[i];
  *hi = entry_start[i + 1];
  *linked = entry_linked[i] != 0;
  *delta = entry_delta[i];
  return true;
}

intptr_t MixedMode::EntryCount() {
  return entry_count;
}

intptr_t MixedMode::EntryIndex(uword pc) {
  return FindEntryIndex(pc);
}

uword MixedMode::EntryStart(intptr_t i) {
  return entry_start[i];
}

uword MixedMode::EntryLimit(intptr_t i) {
  return entry_start[i + 1];
}

bool MixedMode::EntryLinked(intptr_t i) {
  return entry_linked[i] != 0;
}

intptr_t MixedMode::EntryDelta(intptr_t i) {
  return entry_delta[i];
}

uword MixedMode::GateCount() {
  return gate_count;
}

bool MixedMode::LookupEntry(uword pc, uword* lo, uword* hi, bool* linked) {
  const intptr_t i = FindEntryIndex(pc);
  if (i < 0) return false;
  *lo = entry_start[i];
  *hi = entry_start[i + 1];
  *linked = entry_linked[i] != 0;
  return true;
}

const char* MixedMode::LinkImage(uword start_pc,
                                 uword end_pc,
                                 intptr_t length,
                                 const uint32_t* pc_offsets,
                                 intptr_t pc_offset_stride) {
  const bool verbose = EnvFlag("DART_MIXED_VERBOSE");
  const bool unlink_all = EnvFlag("DART_MIXED_UNLINK_ALL");
  // Stress mode: simulate every entry whose image offset is below this value
  // (e.g. all Dart code, leaving the trailing stubs native).
  const char* below_env = getenv("DART_MIXED_UNLINK_BELOW");
  const uword unlink_below =
      below_env != nullptr ? strtoull(below_env, nullptr, 0) : 0;

  patch_start_ = reinterpret_cast<uword>(patch_image_);
  patch_size_ = end_pc - patch_start_;
  if (base_image_ != nullptr) {
    base_start_ = reinterpret_cast<uword>(base_image_);
    base_size_ = ImageEnd(base_image_) - base_start_;
  }

  entry_count = length;
  entry_start = reinterpret_cast<uword*>(malloc((length + 1) * sizeof(uword)));
  entry_linked = reinterpret_cast<uint8_t*>(calloc(length, 1));
  entry_delta = reinterpret_cast<intptr_t*>(calloc(length, sizeof(intptr_t)));
  entry_end = reinterpret_cast<uword*>(malloc(length * sizeof(uword)));
  for (intptr_t i = 0; i < length; i++) {
    entry_start[i] = start_pc + pc_offsets[i * pc_offset_stride];
  }
  entry_start[length] = end_pc;
  for (intptr_t i = 0; i < length; i++) {
    entry_end[i] = entry_start[i + 1];
  }

  auto fail = [&](const char* message) -> const char* {
    if (verbose) OS::PrintErr("mixed: link failed: %s\n", message);
    Deactivate();
    return message;
  };

  intptr_t identical = 0;
  intptr_t cascaded = 0;
  if (table_links != nullptr) {
    // Map every record to the entry it names, then verify every linked
    // entry against the release (word-wise, see VerifyEntry).
    for (intptr_t r = 0; r < table_link_count; r++) {
      const MixedLink& l = table_links[r];
      const uword lo = patch_start_ + l.patch_offset;
      const intptr_t i = FindEntryIndex(lo);
      if (i < 0 || entry_start[i] != lo) {
        return fail("link record does not start a patch code entry");
      }
      // The record names the payload; the entry may also hold the padding
      // (brk #0 words) that aligns the next payload or the image end.
      const uword end = lo + l.size;
      bool size_ok = end <= entry_start[i + 1] && end % 4 == 0 &&
                     entry_start[i + 1] - end < kObjectAlignment;
      for (uword pc = end; size_ok && pc < entry_start[i + 1]; pc += 4) {
        size_ok = *reinterpret_cast<const uint32_t*>(pc) == 0xD4200000;
      }
      if (!size_ok) {
        if (verbose) {
          OS::PrintErr("mixed: record %" Pd " patch %#x release %#x size %u,"
                       " entry %" Pd " of %" Pd " spans %" Pu "\n",
                       r, l.patch_offset, l.release_offset, l.size, i, length,
                       entry_start[i + 1] - lo);
        }
        return fail("link record size differs from the patch code entry");
      }
      entry_linked[i] = 1;
      entry_delta[i] = (base_start_ + l.release_offset) - lo;
      entry_end[i] = end;
    }
    for (intptr_t i = 0; i < length; i++) {
      if (entry_linked[i] && !VerifyEntry(i)) {
        return fail("linked code does not match the release copy");
      }
      if (entry_linked[i]) identical++;
    }
  } else {
    // Identity linking (testing): start from every entry that fits in the
    // release at the same image offset and remove entries that fail
    // VerifyEntry until stable (greatest fixpoint, as the linker's rule 3).
    const intptr_t identity_delta = base_start_ - patch_start_;
    for (intptr_t i = 0; i < length; i++) {
      const uword lo = entry_start[i], hi = entry_start[i + 1];
      if (unlink_all || base_image_ == nullptr) continue;
      if (lo - start_pc < unlink_below) continue;
      if (lo + identity_delta + (hi - lo) > base_start_ + base_size_) continue;
      entry_linked[i] = 1;
      entry_delta[i] = identity_delta;
    }
    bool changed = true;
    bool first = true;
    while (changed) {
      changed = false;
      for (intptr_t i = 0; i < length; i++) {
        if (entry_linked[i] && !VerifyEntry(i)) {
          entry_linked[i] = 0;
          changed = true;
          if (!first) cascaded++;
        }
      }
      first = false;
    }
    for (intptr_t i = 0; i < length; i++) {
      if (entry_linked[i]) identical++;
    }
  }

  // Gate budget: every unlinked entry may need up to four entry gates, every
  // call site in unlinked code one return gate; the reserve covers
  // exception-handler pcs. Refuse the patch rather than run out later.
  intptr_t linked = 0, unlinked = 0, call_sites = 0;
  uword linked_bytes = 0, total_bytes = 0;
  for (intptr_t i = 0; i < length; i++) {
    const uword size = entry_end[i] - entry_start[i];
    total_bytes += size;
    if (entry_linked[i]) {
      linked++;
      linked_bytes += size;
      continue;
    }
    unlinked++;
    for (uword pc = entry_start[i]; pc + 4 <= entry_start[i + 1]; pc += 4) {
      if (IsCall(*reinterpret_cast<const uint32_t*>(pc))) call_sites++;
    }
  }
  const intptr_t gates_needed = 4 * unlinked + call_sites + kGateReserve;
  if (gates_needed > static_cast<intptr_t>(gate_count)) {
    if (verbose) {
      OS::PrintErr("mixed: needs %" Pd " gates, has %" Pu "\n", gates_needed,
                   gate_count);
    }
    return fail("patch needs more gates than the runtime provides");
  }

  // Reverse map for stack walking: linked release ranges sorted by address.
  base_ranges =
      reinterpret_cast<BaseRange*>(malloc((linked + 1) * sizeof(BaseRange)));
  base_range_count = 0;
  for (intptr_t i = 0; i < length; i++) {
    if (!entry_linked[i]) continue;
    base_ranges[base_range_count++] = {entry_start[i] + entry_delta[i],
                                       entry_end[i] + entry_delta[i],
                                       entry_delta[i]};
  }
  qsort(base_ranges, base_range_count, sizeof(BaseRange),
        [](const void* a, const void* b) -> int {
          const uword x = static_cast<const BaseRange*>(a)->lo;
          const uword y = static_cast<const BaseRange*>(b)->lo;
          return x < y ? -1 : (x > y ? 1 : 0);
        });

  if (const char* path = getenv("DART_MIXED_DUMP_LINK_TABLE")) {
    // Testing aid: write the vmcode v1 link records this run used.
    if (FILE* f = fopen(path, "wb")) {
      for (intptr_t i = 0; i < length; i++) {
        if (!entry_linked[i]) continue;
        MixedLink l;
        l.patch_offset = static_cast<uint32_t>(entry_start[i] - patch_start_);
        l.release_offset = static_cast<uint32_t>(entry_start[i] +
                                                 entry_delta[i] - base_start_);
        l.size = static_cast<uint32_t>(entry_end[i] - entry_start[i]);
        l.flags = 0;
        fwrite(&l, sizeof(l), 1, f);
      }
      fclose(f);
    }
  }

  active_ = true;
  link_generation_++;
  if (verbose) {
    OS::PrintErr(
        "mixed: table=%s entries=%" Pd " identical=%" Pd
        " cascade-unlinked=%" Pd " linked=%" Pd " unlinked=%" Pd
        " linked-bytes=%" Pu "/%" Pu " (%.3f%%) gates-budget=%" Pd "/%" Pu
        " patch=[%#" Px ",%#" Px ") base=%#" Px "\n",
        table_links != nullptr ? "vmcode" : "identity", length, identical,
        cascaded, linked, unlinked, linked_bytes, total_bytes,
        total_bytes == 0 ? 0.0 : 100.0 * linked_bytes / total_bytes,
        gates_needed, gate_count, patch_start_, patch_start_ + patch_size_,
        base_start_);
    for (intptr_t i = 0; i < length && !unlink_all && unlink_below == 0; i++) {
      if (!entry_linked[i]) {
        OS::PrintErr("mixed: unlinked entry %" Pd " offset %#" Px " size %" Pu
                     "\n",
                     i, entry_start[i] - patch_start_,
                     entry_start[i + 1] - entry_start[i]);
      }
    }
  }
  return nullptr;
}

const char* MixedMode::CheckNativeOnlyEntries() {
  // Gates find the thread's register area through THR (x26), which C++ does
  // not set when it calls these stubs; they must run from the release.
  const uword entries[] = {
      StubCode::InvokeDartCode().EntryPoint(),
      StubCode::JumpToFrame().EntryPoint(),
      StubCode::RunExceptionHandler().EntryPoint(),
      StubCode::FfiCallbackTrampoline().EntryPoint(),
  };
  for (uword entry : entries) {
    if (IsGate(entry)) {
      return "a stub entered from C++ is not linked to the release";
    }
  }
  return nullptr;
}

uword MixedMode::NativeEntry(intptr_t index, uword patch_entry) {
  ASSERT(active_);
  if (entry_linked[index]) return patch_entry + entry_delta[index];
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
  MixedInterpPrintStats();
}

// DartMixedRun is defined with the interpreter (simulator_arm64_mixed.cc).

}  // namespace dart

#endif  // defined(DART_MIXED_MODE)
