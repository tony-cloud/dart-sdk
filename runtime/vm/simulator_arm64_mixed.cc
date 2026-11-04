// Copyright (c) 2026, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

// Pre-decoded interpreter for mixed-mode execution (vm/mixed_mode.h).
//
// The generic simulator (simulator_arm64.cc) decodes every instruction each
// time it runs it. Here each unlinked patch entry is decoded once, on first
// use, into an array of FastOp records, one per instruction word:
//   * a handler pointer and pre-extracted operands (register slots with XZR
//     and SP already resolved, immediates already expanded, branch targets as
//     op-array displacements, call return gates already allocated);
//   * handlers are small functions that end in a guaranteed tail call
//     ([[clang::musttail]]) to the next op's handler, so dispatch is one
//     load and one indirect branch per instruction, and the interpreter's
//     state (op pointer, register file, lazily evaluated NZCV flags, the
//     simulator) stays in machine registers across instructions;
//   * a flag-setting compare followed by b.cond is fused into one op.
// Instructions without a fast handler (SIMD, atomics, exclusives, rare
// forms) run through the generic decoder, one instruction at a time
// (H_Slow), so coverage equals the generic simulator's.
//
// Nothing here generates machine code: the op arrays are ordinary heap data
// and every handler is compiled into the runtime. The patch text is read as
// data only.
//
// Modes (DART_MIXED_INTERP): unset = this interpreter; "simulator" = the
// generic simulator; "verify" = this interpreter, re-executing every
// instruction it handles with the generic decoder and comparing registers,
// flags and control flow (testing).

#include "vm/globals.h"
#if defined(TARGET_ARCH_ARM64) && defined(DART_INCLUDE_SIMULATOR) &&          \
    defined(DART_MIXED_MODE)

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <array>
#include <atomic>

#include "platform/utils.h"
#include "vm/constants.h"
#include "vm/lockers.h"
#include "vm/mixed_mode.h"
#include "vm/os.h"
#include "vm/os_thread.h"
#include "vm/simulator.h"

#if !defined(__has_cpp_attribute) || !__has_cpp_attribute(clang::musttail)
#error "The mixed-mode interpreter requires [[clang::musttail]] (clang)."
#endif

extern "C" uint8_t* DartMixedGateFlags;  // mixed_mode.cc

namespace dart {

struct FastOp;
typedef uword (*FastHandler)(const FastOp* ip,
                             int64_t* x,
                             uint64_t fa,
                             uint64_t fb,
                             uint64_t fk,
                             Simulator* sim);

// One decoded instruction. Field use depends on the handler; rd/rn/rm/ra are
// register-file slots (0-30, 31 = CSP, kMixedZeroSlot = XZR as a source,
// kMixedSinkSlot = XZR as a destination) or V register numbers.
struct FastOp {
  FastHandler h;
  uint8_t rd;
  uint8_t rn;
  uint8_t rm;
  uint8_t ra;
  int32_t i32;
  int64_t imm;
  uint64_t aux;
};
static_assert(sizeof(FastOp) == 32, "FastOp layout");

// Accessors the handlers need from the simulator (friend of Simulator).
class MixedInterp {
 public:
  static uint32_t OldStep(Simulator* sim,
                          uword pc,
                          uint32_t nzcv,
                          uword* next,
                          bool* exited) {
    uword next_pc = 0;
    const uint32_t out =
        sim->MixedOldStep(reinterpret_cast<Instr*>(pc), nzcv, &next_pc);
    *exited = next_pc == Simulator::kEndSimulatingPC;
    *next = *exited ? sim->mixed_exit_target_ : next_pc;
    return out;
  }
  static void*& State(Simulator* sim) { return sim->mixed_interp_state_; }
  // The simulator's register file becomes the thread's area (one simulator
  // and one area per thread).
  static void Attach(Simulator* sim, MixedRegs* regs) {
    sim->registers_ = regs->x;
    sim->vregisters_ = reinterpret_cast<simd_value_t*>(regs->v);
    // The generic decoder (H_Slow) must not trust a stale entry range.
    sim->sim_lo_ = 0;
    sim->sim_hi_ = 0;
  }
};

namespace {

constexpr intptr_t kZR = kMixedZeroSlot;
constexpr intptr_t kSink = kMixedSinkSlot;
constexpr intptr_t kLinkSlot = 30;

// Lazily evaluated flags: the handler that last set NZCV records its inputs
// (fa, fb) and kind (fk); conditions are evaluated from them when needed.
enum : uint64_t {
  kFNzcv = 0,  // fa holds NZCV in bits 3..0.
  kFSub64,     // fa - fb (subs / cmp), 64 bits.
  kFSub32,
  kFAdd64,  // fa + fb (adds / cmn).
  kFAdd32,
  kFLogic64,  // fa is the result (ands / tst); C = V = 0.
  kFLogic32,
};

inline uint32_t Nzcv(uint64_t a, uint64_t b, uint64_t k) {
  switch (k) {
    case kFSub64: {
      const uint64_t r = a - b;
      return ((r >> 63) << 3) | ((r == 0 ? 1u : 0u) << 2) |
             ((a >= b ? 1u : 0u) << 1) | (((a ^ b) & (a ^ r)) >> 63);
    }
    case kFSub32: {
      const uint32_t a32 = static_cast<uint32_t>(a);
      const uint32_t b32 = static_cast<uint32_t>(b);
      const uint32_t r = a32 - b32;
      return ((r >> 31) << 3) | ((r == 0 ? 1u : 0u) << 2) |
             ((a32 >= b32 ? 1u : 0u) << 1) | (((a32 ^ b32) & (a32 ^ r)) >> 31);
    }
    case kFAdd64: {
      const uint64_t r = a + b;
      return ((r >> 63) << 3) | ((r == 0 ? 1u : 0u) << 2) |
             ((r < a ? 1u : 0u) << 1) | ((~(a ^ b) & (a ^ r)) >> 63);
    }
    case kFAdd32: {
      const uint32_t a32 = static_cast<uint32_t>(a);
      const uint32_t b32 = static_cast<uint32_t>(b);
      const uint32_t r = a32 + b32;
      return ((r >> 31) << 3) | ((r == 0 ? 1u : 0u) << 2) |
             ((r < a32 ? 1u : 0u) << 1) | ((~(a32 ^ b32) & (a32 ^ r)) >> 31);
    }
    case kFLogic64:
      return ((a >> 63) << 3) | ((a == 0 ? 1u : 0u) << 2);
    case kFLogic32: {
      const uint32_t a32 = static_cast<uint32_t>(a);
      return ((a32 >> 31) << 3) | ((a32 == 0 ? 1u : 0u) << 2);
    }
    default:
      return static_cast<uint32_t>(a);
  }
}

constexpr uint16_t CondMaskFor(int cond) {
  uint16_t mask = 0;
  for (int f = 0; f < 16; f++) {
    const bool n = (f & 8) != 0, z = (f & 4) != 0, c = (f & 2) != 0,
               v = (f & 1) != 0;
    bool r = true;
    switch (cond) {
      case EQ: r = z; break;
      case NE: r = !z; break;
      case CS: r = c; break;
      case CC: r = !c; break;
      case MI: r = n; break;
      case PL: r = !n; break;
      case VS: r = v; break;
      case VC: r = !v; break;
      case HI: r = c && !z; break;
      case LS: r = !c || z; break;
      case GE: r = n == v; break;
      case LT: r = n != v; break;
      case GT: r = !z && (n == v); break;
      case LE: r = z || (n != v); break;
      default: r = true; break;
    }
    if (r) mask |= static_cast<uint16_t>(1u << f);
  }
  return mask;
}

constexpr uint16_t kCondMask[16] = {
    CondMaskFor(0),  CondMaskFor(1),  CondMaskFor(2),  CondMaskFor(3),
    CondMaskFor(4),  CondMaskFor(5),  CondMaskFor(6),  CondMaskFor(7),
    CondMaskFor(8),  CondMaskFor(9),  CondMaskFor(10), CondMaskFor(11),
    CondMaskFor(12), CondMaskFor(13), CondMaskFor(14), CondMaskFor(15),
};

inline bool CondRT(int cond, uint64_t a, uint64_t b, uint64_t k) {
  return ((kCondMask[cond] >> Nzcv(a, b, k)) & 1) != 0;
}

// Condition kCond, with direct evaluation for the common flag producers.
template <int kCond>
DART_FORCE_INLINE bool Cond(uint64_t a, uint64_t b, uint64_t k) {
  if (k == kFSub64) {
    const int64_t sa = static_cast<int64_t>(a), sb = static_cast<int64_t>(b);
    int64_t r;
    switch (kCond) {
      case EQ: return a == b;
      case NE: return a != b;
      case CS: return a >= b;
      case CC: return a < b;
      case MI: return static_cast<int64_t>(a - b) < 0;
      case PL: return static_cast<int64_t>(a - b) >= 0;
      case VS: return __builtin_sub_overflow(sa, sb, &r);
      case VC: return !__builtin_sub_overflow(sa, sb, &r);
      case HI: return a > b;
      case LS: return a <= b;
      case GE: return sa >= sb;
      case LT: return sa < sb;
      case GT: return sa > sb;
      case LE: return sa <= sb;
      default: return true;
    }
  }
  if (k == kFLogic64) {
    const int64_t r = static_cast<int64_t>(a);
    switch (kCond) {
      case EQ: return r == 0;
      case NE: return r != 0;
      case CS: return false;
      case CC: return true;
      case MI: return r < 0;
      case PL: return r >= 0;
      case VS: return false;
      case VC: return true;
      case HI: return false;
      case LS: return true;
      case GE: return r >= 0;
      case LT: return r < 0;
      case GT: return r > 0;
      case LE: return r <= 0;
      default: return true;
    }
  }
  return ((kCondMask[kCond] >> Nzcv(a, b, k)) & 1) != 0;
}

template <typename T>
DART_FORCE_INLINE T Load(uword addr) {
  T value;
  memcpy(&value, reinterpret_cast<const void*>(addr), sizeof(T));
  return value;
}

template <typename T>
DART_FORCE_INLINE void Store(uword addr, T value) {
  memcpy(reinterpret_cast<void*>(addr), &value, sizeof(T));
}

DART_FORCE_INLINE simd_value_t* VReg(int64_t* x, intptr_t i) {
  return reinterpret_cast<simd_value_t*>(x + kMixedRegisterSlots) + i;
}

DART_FORCE_INLINE double VD(int64_t* x, intptr_t i) {
  return bit_cast<double, int64_t>(VReg(x, i)->bits.i64[0]);
}

DART_FORCE_INLINE void SetVD(int64_t* x, intptr_t i, int64_t bits) {
  simd_value_t* v = VReg(x, i);
  v->bits.i64[0] = bits;
  v->bits.i64[1] = 0;
}

// ------------------------------------------------------------ global tables

// Per link generation: decoded op arrays per instructions-table entry, and
// the op each gate resumes at. Both are filled lazily and published with
// release stores; entries and gates are immutable once bound, so a published
// pointer never changes.
std::atomic<uword> g_generation = {0};
intptr_t g_entry_count = 0;
std::atomic<const FastOp*>* g_entry_ops = nullptr;
uword g_gates_start = 0;
uword g_gate_count = 0;
std::atomic<const FastOp*>* g_gate_ip = nullptr;
// Per entry: one byte per instruction, nonzero when v8..v31 may be read
// (before being written) on some path from that instruction. Published
// before the entry's ops.
uint8_t** g_entry_high_live = nullptr;
std::atomic<int> g_mode = {-1};
std::atomic<int64_t> g_decoded_entries = {0};
std::atomic<int64_t> g_decoded_bytes = {0};

enum { kModeDecoder = 0, kModeSimulator = 1, kModeVerify = 2 };

int Mode() {
  int mode = g_mode.load(std::memory_order_relaxed);
  if (mode < 0) {
    const char* value = getenv("DART_MIXED_INTERP");
    mode = kModeDecoder;
    if (value != nullptr && strcmp(value, "simulator") == 0) {
      mode = kModeSimulator;
    } else if (value != nullptr && strcmp(value, "verify") == 0) {
      mode = kModeVerify;
    }
    g_mode.store(mode, std::memory_order_relaxed);
  }
  return mode;
}

Mutex* DecodeMutex() {
  static Mutex* mutex = new Mutex();
  return mutex;
}

void EnsureTables() {
  const uword generation = MixedMode::link_generation();
  if (g_generation.load(std::memory_order_acquire) == generation) return;
  MutexLocker ml(DecodeMutex());
  if (g_generation.load(std::memory_order_relaxed) == generation) return;
  // Tables of an earlier generation are leaked: a patch is linked once per
  // process (testing re-links at most once).
  g_entry_count = MixedMode::EntryCount();
  // calloc: zero pages stay untouched until used (std::atomic<T*> of a null
  // pointer is all zero bits).
  g_entry_ops = reinterpret_cast<std::atomic<const FastOp*>*>(
      calloc(Utils::Maximum<intptr_t>(g_entry_count, 1), sizeof(void*)));
  g_entry_high_live = reinterpret_cast<uint8_t**>(
      calloc(Utils::Maximum<intptr_t>(g_entry_count, 1), sizeof(uint8_t*)));
  g_gates_start = MixedMode::gates_start_;
  g_gate_count = MixedMode::GateCount();
  g_gate_ip = reinterpret_cast<std::atomic<const FastOp*>*>(
      calloc(Utils::Maximum<uword>(g_gate_count, 1), sizeof(void*)));
  g_generation.store(generation, std::memory_order_release);
}

const FastOp* DecodeEntry(intptr_t j);

DART_FORCE_INLINE const FastOp* EntryOps(intptr_t j) {
  const FastOp* ops = g_entry_ops[j].load(std::memory_order_acquire);
  return ops != nullptr ? ops : DecodeEntry(j);
}

// Where branching to `target` continues: an op (simulated), or nullptr with
// *native set (linked release code, stubs, C, anything outside the patch).
const FastOp* ResolveSlow(uword target, uword* native) {
  uword pc = target;
  intptr_t gate = -1;
  if (MixedMode::IsGate(target)) {
    gate = (target - g_gates_start) >> MixedMode::kGateSizeLog2;
    pc = MixedMode::GateTarget(target);
    if (pc == 0) FATAL("Mixed mode: branch to unbound gate %#" Px, target);
  }
  if (!MixedMode::InPatchText(pc)) {
    *native = pc;
    return nullptr;
  }
  const intptr_t j = MixedMode::EntryIndex(pc);
  if (j < 0) {
    FATAL("Mixed mode: branch into patch text outside any entry %#" Px, pc);
  }
  if (MixedMode::EntryLinked(j)) {
    // Linked code runs natively from the release's copy.
    *native = pc + MixedMode::EntryDelta(j);
    return nullptr;
  }
  const intptr_t index = (pc - MixedMode::EntryStart(j)) >> 2;
  const FastOp* ip = EntryOps(j) + index;
  if (gate >= 0 && static_cast<uword>(gate) < g_gate_count) {
    if (g_entry_high_live[j][index] == 0) {
      // Entries through this gate need not save v8..v31 (see
      // ComputeHighVLiveness).
      DartMixedGateFlags[gate] = 1;
    }
    g_gate_ip[gate].store(ip, std::memory_order_release);
  }
  return ip;
}

// The op a gate resumes at, or nullptr when not yet resolved / not a gate.
DART_FORCE_INLINE const FastOp* GateOp(uword target) {
  const uword g = (target - g_gates_start) >> MixedMode::kGateSizeLog2;
  if (g < g_gate_count) return g_gate_ip[g].load(std::memory_order_acquire);
  return nullptr;
}

// Returns the native target, with bit 0 set when the exiting entry cannot
// have written v8..v31 (ip->ra = 1 on exit ops of such entries; see
// EntryMayWriteHighV): DartMixedSimEnter then skips reloading them.
DART_FORCE_INLINE uword ExitTo(const FastOp* ip, int64_t* x, uword target) {
  reinterpret_cast<MixedRegs*>(x)->exit_target = target;
  if (MixedMode::count_transitions()) {
    MixedMode::sim_to_native_.fetch_add(1, std::memory_order_relaxed);
  }
  return target | (ip->ra & 1);
}

// A non-linking branch to native code: the CPU must never see a patch pc as
// its return address (see Simulator::MixedBranch).
DART_FORCE_INLINE void FixLinkRegister(int64_t* x) {
  const uword lr = static_cast<uword>(x[kLinkSlot]);
  if (MixedMode::InPatchText(lr)) {
    x[kLinkSlot] = static_cast<int64_t>(MixedMode::ToNativePc(lr));
  }
}

// ----------------------------------------------------------------- handlers

#define HARGS                                                                  \
  const FastOp *ip, int64_t *x, uint64_t fa, uint64_t fb, uint64_t fk,         \
      Simulator *sim
#define DISPATCH(target)                                                       \
  do {                                                                         \
    const FastOp* next_op_ = (target);                                         \
    [[clang::musttail]] return next_op_->h(next_op_, x, fa, fb, fk, sim);      \
  } while (0)
#define NEXT() DISPATCH(ip + 1)
// Straight-line instructions are bodies (B_*); H_Simple runs one, H_Seq2
// (superinstructions) runs two in a single dispatch.
#define BARGS                                                                  \
  const FastOp *ip, int64_t *x, uint64_t &fa, uint64_t &fb, uint64_t &fk

template <class B>
uword H_Simple(HARGS) {
  B::Exec(ip, x, fa, fb, fk);
  NEXT();
}

uword H_Slow(HARGS) {
  const uword pc = static_cast<uword>(ip->imm);
  uword next;
  bool exited;
  fa = MixedInterp::OldStep(sim, pc, Nzcv(fa, fb, fk), &next, &exited);
  fb = 0;
  fk = kFNzcv;
  if (exited) {
    reinterpret_cast<MixedRegs*>(x)->exit_target = next;
    return next;
  }
  if (next == pc + Instr::kInstrSize) NEXT();
  uword native = 0;
  const FastOp* target = ResolveSlow(next, &native);
  if (target != nullptr) DISPATCH(target);
  FixLinkRegister(x);
  return ExitTo(ip, x, native);
}

// Falling off the end of an entry continues at the next patch pc.
uword H_End(HARGS) {
  uword native = 0;
  const FastOp* target = ResolveSlow(static_cast<uword>(ip->imm), &native);
  if (target != nullptr) DISPATCH(target);
  FixLinkRegister(x);
  return ExitTo(ip, x, native);
}

struct B_Nop {
  static DART_FORCE_INLINE void Exec(BARGS) {

  }
};

// ---- moves and immediates

struct B_MovImm {
  static DART_FORCE_INLINE void Exec(BARGS) {
    x[ip->rd] = ip->imm;
  }
};

struct B_MovK {
  static DART_FORCE_INLINE void Exec(BARGS) {
    x[ip->rd] = (x[ip->rd] & static_cast<int64_t>(ip->aux)) | ip->imm;
  }
};

struct B_Mov64 {
  static DART_FORCE_INLINE void Exec(BARGS) {
    x[ip->rd] = x[ip->rm];
  }
};

struct B_Mov32 {
  static DART_FORCE_INLINE void Exec(BARGS) {
    x[ip->rd] = static_cast<uint32_t>(x[ip->rm]);
  }
};

// ---- add/sub immediate (sub as a negated immediate when flags are not set)

struct B_AddImm64 {
  static DART_FORCE_INLINE void Exec(BARGS) {
    x[ip->rd] = static_cast<int64_t>(static_cast<uint64_t>(x[ip->rn]) +
                                     static_cast<uint64_t>(ip->imm));
  }
};

struct B_AddImm32 {
  static DART_FORCE_INLINE void Exec(BARGS) {
    x[ip->rd] = static_cast<uint32_t>(static_cast<uint64_t>(x[ip->rn]) +
                                      static_cast<uint64_t>(ip->imm));
  }
};

template <bool kSub, bool k64>
struct B_AddSImm {
  static DART_FORCE_INLINE void Exec(BARGS) {
    if (k64) {
      const uint64_t a = static_cast<uint64_t>(x[ip->rn]);
      const uint64_t b = static_cast<uint64_t>(ip->imm);
      x[ip->rd] = static_cast<int64_t>(kSub ? a - b : a + b);
      fa = a;
      fb = b;
      fk = kSub ? kFSub64 : kFAdd64;
    } else {
      const uint32_t a = static_cast<uint32_t>(x[ip->rn]);
      const uint32_t b = static_cast<uint32_t>(ip->imm);
      x[ip->rd] = static_cast<uint32_t>(kSub ? a - b : a + b);
      fa = a;
      fb = b;
      fk = kSub ? kFSub32 : kFAdd32;
    }
  }
};

// ---- shifted / extended register operands

template <int kShift>
DART_FORCE_INLINE uint64_t Shift64(uint64_t v, int amount) {
  switch (kShift) {
    case LSL: return v << amount;
    case LSR: return v >> amount;
    case ASR: return static_cast<uint64_t>(static_cast<int64_t>(v) >> amount);
    default:
      return amount == 0 ? v : (v >> amount) | (v << (64 - amount));
  }
}

template <int kShift>
DART_FORCE_INLINE uint32_t Shift32(uint32_t v, int amount) {
  switch (kShift) {
    case LSL: return v << amount;
    case LSR: return v >> amount;
    case ASR: return static_cast<uint32_t>(static_cast<int32_t>(v) >> amount);
    default:
      return amount == 0 ? v : (v >> amount) | (v << (32 - amount));
  }
}

DART_FORCE_INLINE int64_t ExtendValue(int64_t v, int extend) {
  switch (extend) {
    case UXTB: return v & 0xff;
    case UXTH: return v & 0xffff;
    case UXTW: return static_cast<uint32_t>(v);
    case SXTB: return static_cast<int8_t>(v);
    case SXTH: return static_cast<int16_t>(v);
    case SXTW: return static_cast<int32_t>(v);
    default: return v;
  }
}

// add/sub (shifted register); rm shifted by i32.
template <bool kSub, bool kFlags, bool k64, int kShift>
struct B_AddSubShift {
  static DART_FORCE_INLINE void Exec(BARGS) {
    if (k64) {
      const uint64_t a = static_cast<uint64_t>(x[ip->rn]);
      const uint64_t b = Shift64<kShift>(static_cast<uint64_t>(x[ip->rm]),
                                         ip->i32);
      x[ip->rd] = static_cast<int64_t>(kSub ? a - b : a + b);
      if (kFlags) {
        fa = a;
        fb = b;
        fk = kSub ? kFSub64 : kFAdd64;
      }
    } else {
      const uint32_t a = static_cast<uint32_t>(x[ip->rn]);
      const uint32_t b = Shift32<kShift>(static_cast<uint32_t>(x[ip->rm]),
                                         ip->i32);
      x[ip->rd] = static_cast<uint32_t>(kSub ? a - b : a + b);
      if (kFlags) {
        fa = a;
        fb = b;
        fk = kSub ? kFSub32 : kFAdd32;
      }
    }
  }
};

// add/sub (extended register); extend type in ra, shift in i32.
template <bool kSub, bool kFlags, bool k64>
struct B_AddSubExt {
  static DART_FORCE_INLINE void Exec(BARGS) {
    uint64_t b = static_cast<uint64_t>(ExtendValue(x[ip->rm], ip->ra))
                 << ip->i32;
    if (k64) {
      const uint64_t a = static_cast<uint64_t>(x[ip->rn]);
      x[ip->rd] = static_cast<int64_t>(kSub ? a - b : a + b);
      if (kFlags) {
        fa = a;
        fb = b;
        fk = kSub ? kFSub64 : kFAdd64;
      }
    } else {
      const uint32_t a = static_cast<uint32_t>(x[ip->rn]);
      const uint32_t b32 = static_cast<uint32_t>(b);
      x[ip->rd] = static_cast<uint32_t>(kSub ? a - b32 : a + b32);
      if (kFlags) {
        fa = a;
        fb = b32;
        fk = kSub ? kFSub32 : kFAdd32;
      }
    }
  }
};

// ---- logical

enum LogicOp { kAnd = 0, kBic, kOrr, kOrn, kEor, kEon, kAnds, kBics };

template <int kOp, typename T>
DART_FORCE_INLINE T Logic(T a, T b) {
  switch (kOp) {
    case kAnd:
    case kAnds: return a & b;
    case kBic:
    case kBics: return a & ~b;
    case kOrr: return a | b;
    case kOrn: return a | ~b;
    case kEor: return a ^ b;
    default: return a ^ ~b;  // kEon
  }
}

template <int kOp, bool k64, int kShift>
struct B_LogicShift {
  static DART_FORCE_INLINE void Exec(BARGS) {
    if (k64) {
      const uint64_t r = Logic<kOp, uint64_t>(
          static_cast<uint64_t>(x[ip->rn]),
          Shift64<kShift>(static_cast<uint64_t>(x[ip->rm]), ip->i32));
      x[ip->rd] = static_cast<int64_t>(r);
      if (kOp == kAnds || kOp == kBics) {
        fa = r;
        fk = kFLogic64;
      }
    } else {
      const uint32_t r = Logic<kOp, uint32_t>(
          static_cast<uint32_t>(x[ip->rn]),
          Shift32<kShift>(static_cast<uint32_t>(x[ip->rm]), ip->i32));
      x[ip->rd] = r;
      if (kOp == kAnds || kOp == kBics) {
        fa = r;
        fk = kFLogic32;
      }
    }
  }
};

// Logical immediate; the 32-bit forms have a 32-bit immediate.
template <int kOp, bool k64>
struct B_LogicImm {
  static DART_FORCE_INLINE void Exec(BARGS) {
    const uint64_t r = Logic<kOp, uint64_t>(static_cast<uint64_t>(x[ip->rn]),
                                            static_cast<uint64_t>(ip->imm));
    if (k64) {
      x[ip->rd] = static_cast<int64_t>(r);
      if (kOp == kAnds) {
        fa = r;
        fk = kFLogic64;
      }
    } else {
      x[ip->rd] = static_cast<uint32_t>(r);
      if (kOp == kAnds) {
        fa = static_cast<uint32_t>(r);
        fk = kFLogic32;
      }
    }
  }
};

// ---- bitfield

struct B_Lsl64 {
  static DART_FORCE_INLINE void Exec(BARGS) {
    x[ip->rd] = static_cast<int64_t>(static_cast<uint64_t>(x[ip->rn])
                                     << ip->i32);
  }
};

struct B_Lsr64 {
  static DART_FORCE_INLINE void Exec(BARGS) {
    x[ip->rd] =
        static_cast<int64_t>(static_cast<uint64_t>(x[ip->rn]) >> ip->i32);
  }
};

struct B_Asr64 {
  static DART_FORCE_INLINE void Exec(BARGS) {
    x[ip->rd] = x[ip->rn] >> ip->i32;
  }
};

// Unsigned bitfield move: ((rn & imm) >> i32) << ra. Also covers the 32-bit
// forms (the mask keeps the result in 32 bits).
struct B_Ubfm {
  static DART_FORCE_INLINE void Exec(BARGS) {
    x[ip->rd] = static_cast<int64_t>(
        ((static_cast<uint64_t>(x[ip->rn]) & static_cast<uint64_t>(ip->imm)) >>
         ip->i32)
        << ip->ra);
  }
};

// Signed bitfield move: as H_Ubfm, then sign-extended from bit 63 - rm.
template <bool k64>
struct B_Sbfm {
  static DART_FORCE_INLINE void Exec(BARGS) {
    const uint64_t u =
        ((static_cast<uint64_t>(x[ip->rn]) & static_cast<uint64_t>(ip->imm)) >>
         ip->i32)
        << ip->ra;
    const int64_t r = static_cast<int64_t>(u << ip->rm) >> ip->rm;
    x[ip->rd] = k64 ? r : static_cast<uint32_t>(r);
  }
};

// ---- conditional select (condition in i32)

enum CselKind { kCsel = 0, kCsinc, kCsinv, kCsneg };

template <int kKind, bool k64>
struct B_Csel {
  static DART_FORCE_INLINE void Exec(BARGS) {
    int64_t r;
    if (CondRT(ip->i32, fa, fb, fk)) {
      r = x[ip->rn];
    } else {
      const int64_t m = x[ip->rm];
      switch (kKind) {
        case kCsel: r = m; break;
        case kCsinc: r = static_cast<int64_t>(static_cast<uint64_t>(m) + 1); break;
        case kCsinv: r = ~m; break;
        default: r = static_cast<int64_t>(0 - static_cast<uint64_t>(m)); break;
      }
    }
    x[ip->rd] = k64 ? r : static_cast<uint32_t>(r);
  }
};

// ---- multiply / divide / variable shifts

template <bool kSub, bool k64>
struct B_Madd {
  static DART_FORCE_INLINE void Exec(BARGS) {
    if (k64) {
      const uint64_t p = static_cast<uint64_t>(x[ip->rn]) *
                         static_cast<uint64_t>(x[ip->rm]);
      const uint64_t a = static_cast<uint64_t>(x[ip->ra]);
      x[ip->rd] = static_cast<int64_t>(kSub ? a - p : a + p);
    } else {
      const uint32_t p = static_cast<uint32_t>(x[ip->rn]) *
                         static_cast<uint32_t>(x[ip->rm]);
      const uint32_t a = static_cast<uint32_t>(x[ip->ra]);
      x[ip->rd] = static_cast<uint32_t>(kSub ? a - p : a + p);
    }
  }
};

struct B_Smulh {
  static DART_FORCE_INLINE void Exec(BARGS) {
    const __int128 p = static_cast<__int128>(x[ip->rn]) *
                       static_cast<__int128>(x[ip->rm]);
    x[ip->rd] = static_cast<int64_t>(p >> 64);
  }
};

struct B_Umulh {
  static DART_FORCE_INLINE void Exec(BARGS) {
    const unsigned __int128 p =
        static_cast<unsigned __int128>(static_cast<uint64_t>(x[ip->rn])) *
        static_cast<unsigned __int128>(static_cast<uint64_t>(x[ip->rm]));
    x[ip->rd] = static_cast<int64_t>(static_cast<uint64_t>(p >> 64));
  }
};

template <bool kSigned>
struct B_Maddl {
  static DART_FORCE_INLINE void Exec(BARGS) {
    if (kSigned) {
      const int64_t p = static_cast<int64_t>(static_cast<int32_t>(x[ip->rn])) *
                        static_cast<int64_t>(static_cast<int32_t>(x[ip->rm]));
      x[ip->rd] = static_cast<int64_t>(static_cast<uint64_t>(x[ip->ra]) +
                                       static_cast<uint64_t>(p));
    } else {
      const uint64_t p = static_cast<uint64_t>(static_cast<uint32_t>(x[ip->rn])) *
                         static_cast<uint64_t>(static_cast<uint32_t>(x[ip->rm]));
      x[ip->rd] = static_cast<int64_t>(static_cast<uint64_t>(x[ip->ra]) + p);
    }
  }
};

template <bool kSigned, bool k64>
struct B_Div {
  static DART_FORCE_INLINE void Exec(BARGS) {
    // ARM64 does not trap: x / 0 = 0, INT_MIN / -1 = INT_MIN.
    if (k64) {
      const int64_t n = x[ip->rn], d = x[ip->rm];
      int64_t r;
      if (d == 0) {
        r = 0;
      } else if (kSigned) {
        r = (n == INT64_MIN && d == -1) ? INT64_MIN : n / d;
      } else {
        r = static_cast<int64_t>(static_cast<uint64_t>(n) /
                                 static_cast<uint64_t>(d));
      }
      x[ip->rd] = r;
    } else {
      const int32_t n = static_cast<int32_t>(x[ip->rn]);
      const int32_t d = static_cast<int32_t>(x[ip->rm]);
      int32_t r;
      if (d == 0) {
        r = 0;
      } else if (kSigned) {
        r = (n == INT32_MIN && d == -1) ? INT32_MIN : n / d;
      } else {
        r = static_cast<int32_t>(static_cast<uint32_t>(n) /
                                 static_cast<uint32_t>(d));
      }
      x[ip->rd] = static_cast<uint32_t>(r);
    }
  }
};

template <int kShift, bool k64>
struct B_ShiftV {
  static DART_FORCE_INLINE void Exec(BARGS) {
    if (k64) {
      x[ip->rd] = static_cast<int64_t>(Shift64<kShift>(
          static_cast<uint64_t>(x[ip->rn]), static_cast<int>(x[ip->rm] & 63)));
    } else {
      x[ip->rd] = Shift32<kShift>(static_cast<uint32_t>(x[ip->rn]),
                                  static_cast<int>(x[ip->rm] & 31));
    }
  }
};

// ---- loads and stores

enum AddrMode { kAImm = 0, kAPre, kAPost, kARegX, kARegExt };

template <int kMode>
DART_FORCE_INLINE uword Address(const FastOp* ip, int64_t* x) {
  switch (kMode) {
    case kAImm:
    case kAPre:
      return static_cast<uword>(x[ip->rn]) + static_cast<uword>(ip->imm);
    case kAPost:
      return static_cast<uword>(x[ip->rn]);
    case kARegX:
      return static_cast<uword>(x[ip->rn]) +
             (static_cast<uword>(x[ip->rm]) << ip->i32);
    default:
      return static_cast<uword>(x[ip->rn]) +
             (static_cast<uword>(ExtendValue(x[ip->rm], ip->ra)) << ip->i32);
  }
}

template <int kMode>
DART_FORCE_INLINE void WriteBack(const FastOp* ip, int64_t* x, uword addr) {
  if (kMode == kAPre) {
    x[ip->rn] = static_cast<int64_t>(addr);
  } else if (kMode == kAPost) {
    x[ip->rn] = static_cast<int64_t>(addr + static_cast<uword>(ip->imm));
  }
}

// Integer load of M, sign- or zero-extended per M; kW: to a W register.
template <typename M, bool kW, int kMode>
struct B_Ld {
  static DART_FORCE_INLINE void Exec(BARGS) {
    const uword a = Address<kMode>(ip, x);
    int64_t v = static_cast<int64_t>(Load<M>(a));
    if (kW) v = static_cast<uint32_t>(v);
    x[ip->rd] = v;
    WriteBack<kMode>(ip, x, a);
  }
};

template <typename M, int kMode>
struct B_St {
  static DART_FORCE_INLINE void Exec(BARGS) {
    const uword a = Address<kMode>(ip, x);
    Store<M>(a, static_cast<M>(x[ip->rd]));
    WriteBack<kMode>(ip, x, a);
  }
};

// FP/SIMD load or store of 4, 8 or 16 bytes (V register rd).
template <int kBytes, bool kLoad, int kMode>
struct B_VLdSt {
  static DART_FORCE_INLINE void Exec(BARGS) {
    const uword a = Address<kMode>(ip, x);
    simd_value_t* v = VReg(x, ip->rd);
    if (kLoad) {
      if (kBytes == 4) {
        v->bits.i64[0] = static_cast<int64_t>(Load<uint32_t>(a));
        v->bits.i64[1] = 0;
      } else if (kBytes == 8) {
        v->bits.i64[0] = Load<int64_t>(a);
        v->bits.i64[1] = 0;
      } else {
        const int64_t lo = Load<int64_t>(a);
        const int64_t hi = Load<int64_t>(a + 8);
        v->bits.i64[0] = lo;
        v->bits.i64[1] = hi;
      }
    } else {
      if (kBytes == 4) {
        Store<uint32_t>(a, static_cast<uint32_t>(v->bits.i64[0]));
      } else if (kBytes == 8) {
        Store<int64_t>(a, v->bits.i64[0]);
      } else {
        Store<int64_t>(a, v->bits.i64[0]);
        Store<int64_t>(a + 8, v->bits.i64[1]);
      }
    }
    WriteBack<kMode>(ip, x, a);
  }
};

// Load/store pair; first register rd, second rm; offset in imm.
enum PairKind { kPairX = 0, kPairW, kPairS, kPairD, kPairQ };

template <int kKind, bool kLoad, int kMode>
struct B_Pair {
  static DART_FORCE_INLINE void Exec(BARGS) {
    const uword a = Address<kMode>(ip, x);
    if (kKind == kPairX || kKind == kPairW) {
      constexpr int kSize = kKind == kPairX ? 8 : 4;
      if (kLoad) {
        int64_t v1, v2;
        if (kKind == kPairX) {
          v1 = Load<int64_t>(a);
          v2 = Load<int64_t>(a + kSize);
        } else {
          v1 = Load<uint32_t>(a);
          v2 = Load<uint32_t>(a + kSize);
        }
        x[ip->rd] = v1;
        x[ip->rm] = v2;
      } else {
        if (kKind == kPairX) {
          Store<int64_t>(a, x[ip->rd]);
          Store<int64_t>(a + kSize, x[ip->rm]);
        } else {
          Store<uint32_t>(a, static_cast<uint32_t>(x[ip->rd]));
          Store<uint32_t>(a + kSize, static_cast<uint32_t>(x[ip->rm]));
        }
      }
    } else {
      constexpr int kSize = kKind == kPairS ? 4 : (kKind == kPairD ? 8 : 16);
      simd_value_t* v1 = VReg(x, ip->rd);
      simd_value_t* v2 = VReg(x, ip->rm);
      if (kLoad) {
        if (kSize == 4) {
          v1->bits.i64[0] = Load<uint32_t>(a);
          v1->bits.i64[1] = 0;
          v2->bits.i64[0] = Load<uint32_t>(a + 4);
          v2->bits.i64[1] = 0;
        } else if (kSize == 8) {
          v1->bits.i64[0] = Load<int64_t>(a);
          v1->bits.i64[1] = 0;
          v2->bits.i64[0] = Load<int64_t>(a + 8);
          v2->bits.i64[1] = 0;
        } else {
          const int64_t a0 = Load<int64_t>(a), a1 = Load<int64_t>(a + 8);
          const int64_t b0 = Load<int64_t>(a + 16), b1 = Load<int64_t>(a + 24);
          v1->bits.i64[0] = a0;
          v1->bits.i64[1] = a1;
          v2->bits.i64[0] = b0;
          v2->bits.i64[1] = b1;
        }
      } else {
        if (kSize == 4) {
          Store<uint32_t>(a, static_cast<uint32_t>(v1->bits.i64[0]));
          Store<uint32_t>(a + 4, static_cast<uint32_t>(v2->bits.i64[0]));
        } else if (kSize == 8) {
          Store<int64_t>(a, v1->bits.i64[0]);
          Store<int64_t>(a + 8, v2->bits.i64[0]);
        } else {
          Store<int64_t>(a, v1->bits.i64[0]);
          Store<int64_t>(a + 8, v1->bits.i64[1]);
          Store<int64_t>(a + 16, v2->bits.i64[0]);
          Store<int64_t>(a + 24, v2->bits.i64[1]);
        }
      }
    }
    WriteBack<kMode>(ip, x, a);
  }
};

// ---- branches

uword H_B(HARGS) {
  DISPATCH(ip + ip->i32);
}

uword H_BLocalLink(HARGS) {
  x[kLinkSlot] = ip->imm;
  DISPATCH(ip + ip->i32);
}

// Slow paths are separate handlers reached by tail calls, so the fast paths
// below need no stack frame.

// Direct branch to another unlinked entry: aux = its g_entry_ops slot, i32 =
// op index in it.
uword H_BSimDecode(HARGS) {
  const auto* slot = reinterpret_cast<std::atomic<const FastOp*>*>(ip->aux);
  const FastOp* ops = DecodeEntry(slot - g_entry_ops);
  DISPATCH(ops + ip->i32);
}

uword H_BSim(HARGS) {
  const auto* slot = reinterpret_cast<std::atomic<const FastOp*>*>(ip->aux);
  const FastOp* ops = slot->load(std::memory_order_acquire);
  if (ops == nullptr) [[unlikely]] {
    [[clang::musttail]] return H_BSimDecode(ip, x, fa, fb, fk, sim);
  }
  DISPATCH(ops + ip->i32);
}

uword H_BlSim(HARGS) {
  x[kLinkSlot] = ip->imm;  // Return gate.
  const auto* slot = reinterpret_cast<std::atomic<const FastOp*>*>(ip->aux);
  const FastOp* ops = slot->load(std::memory_order_acquire);
  if (ops == nullptr) [[unlikely]] {
    [[clang::musttail]] return H_BSimDecode(ip, x, fa, fb, fk, sim);
  }
  DISPATCH(ops + ip->i32);
}

uword H_BNativeFixLink(HARGS) {
  FixLinkRegister(x);
  return ExitTo(ip, x, static_cast<uword>(ip->aux));
}

uword H_BNative(HARGS) {
  if (MixedMode::InPatchText(static_cast<uword>(x[kLinkSlot]))) [[unlikely]] {
    [[clang::musttail]] return H_BNativeFixLink(ip, x, fa, fb, fk, sim);
  }
  return ExitTo(ip, x, static_cast<uword>(ip->aux));
}

uword H_BlNative(HARGS) {
  x[kLinkSlot] = ip->imm;  // Return gate.
  return ExitTo(ip, x, static_cast<uword>(ip->aux));
}

// br / ret to a target that is not a resolved gate.
uword H_BrSlow(HARGS) {
  const uword target = static_cast<uword>(x[ip->rn]);
  uword native = 0;
  const FastOp* next = ResolveSlow(target, &native);
  if (next != nullptr) DISPATCH(next);
  FixLinkRegister(x);
  return ExitTo(ip, x, native);
}

// br / ret (register rn).
uword H_Br(HARGS) {
  const uword target = static_cast<uword>(x[ip->rn]);
  const FastOp* next = GateOp(target);
  if (next != nullptr) DISPATCH(next);
  if (MixedMode::InPatchText(target) || MixedMode::IsGate(target) ||
      MixedMode::InPatchText(static_cast<uword>(x[kLinkSlot]))) [[unlikely]] {
    [[clang::musttail]] return H_BrSlow(ip, x, fa, fb, fk, sim);
  }
  return ExitTo(ip, x, target);
}

// blr to a target that is not a resolved gate (the link register is not yet
// written: rn may be x30).
uword H_BlrSlow(HARGS) {
  const uword target = static_cast<uword>(x[ip->rn]);
  x[kLinkSlot] = ip->imm;
  uword native = 0;
  const FastOp* next = ResolveSlow(target, &native);
  if (next != nullptr) DISPATCH(next);
  return ExitTo(ip, x, native);
}

// blr (register rn); imm = return gate.
uword H_Blr(HARGS) {
  const uword target = static_cast<uword>(x[ip->rn]);
  const FastOp* next = GateOp(target);
  if (next == nullptr) [[unlikely]] {
    if (MixedMode::InPatchText(target) || MixedMode::IsGate(target)) {
      [[clang::musttail]] return H_BlrSlow(ip, x, fa, fb, fk, sim);
    }
    x[kLinkSlot] = ip->imm;
    return ExitTo(ip, x, target);
  }
  x[kLinkSlot] = ip->imm;
  DISPATCH(next);
}

template <int kCond>
uword H_BCond(HARGS) {
  if (Cond<kCond>(fa, fb, fk)) DISPATCH(ip + ip->i32);
  NEXT();
}

template <bool kNonZero, bool k64>
uword H_Cbz(HARGS) {
  const uint64_t v = k64 ? static_cast<uint64_t>(x[ip->rn])
                         : static_cast<uint32_t>(x[ip->rn]);
  if ((v != 0) == kNonZero) DISPATCH(ip + ip->i32);
  NEXT();
}

// tbz / tbnz: imm = bit mask.
template <bool kNonZero>
uword H_Tbz(HARGS) {
  if (((x[ip->rn] & ip->imm) != 0) == kNonZero) DISPATCH(ip + ip->i32);
  NEXT();
}

// Fused flag-setting compare + b.cond (the next op): the compare's result
// (if any) goes to rd; i32 is the branch displacement from this op.
enum FuseKind {
  kFuseSubImm64 = 0,
  kFuseSubReg64,
  kFuseSubImm32,
  kFuseSubReg32,
  kFuseAddImm64,
  kFuseAddReg64,
  kFuseAndImm64,
  kFuseAndReg64,
  kFuseAndImm32,
  kFuseKinds,
};

template <int kKind, int kCond>
uword H_Fused(HARGS) {
  switch (kKind) {
    case kFuseSubImm64:
    case kFuseSubReg64: {
      const uint64_t a = static_cast<uint64_t>(x[ip->rn]);
      const uint64_t b = kKind == kFuseSubImm64
                             ? static_cast<uint64_t>(ip->imm)
                             : static_cast<uint64_t>(x[ip->rm]);
      x[ip->rd] = static_cast<int64_t>(a - b);
      fa = a;
      fb = b;
      fk = kFSub64;
      break;
    }
    case kFuseSubImm32:
    case kFuseSubReg32: {
      const uint32_t a = static_cast<uint32_t>(x[ip->rn]);
      const uint32_t b = kKind == kFuseSubImm32
                             ? static_cast<uint32_t>(ip->imm)
                             : static_cast<uint32_t>(x[ip->rm]);
      x[ip->rd] = static_cast<uint32_t>(a - b);
      fa = a;
      fb = b;
      fk = kFSub32;
      break;
    }
    case kFuseAddImm64:
    case kFuseAddReg64: {
      const uint64_t a = static_cast<uint64_t>(x[ip->rn]);
      const uint64_t b = kKind == kFuseAddImm64
                             ? static_cast<uint64_t>(ip->imm)
                             : static_cast<uint64_t>(x[ip->rm]);
      x[ip->rd] = static_cast<int64_t>(a + b);
      fa = a;
      fb = b;
      fk = kFAdd64;
      break;
    }
    case kFuseAndImm64:
    case kFuseAndReg64: {
      const uint64_t r = static_cast<uint64_t>(x[ip->rn]) &
                         (kKind == kFuseAndImm64
                              ? static_cast<uint64_t>(ip->imm)
                              : static_cast<uint64_t>(x[ip->rm]));
      x[ip->rd] = static_cast<int64_t>(r);
      fa = r;
      fk = kFLogic64;
      break;
    }
    default: {
      const uint32_t r = static_cast<uint32_t>(x[ip->rn]) &
                         static_cast<uint32_t>(ip->imm);
      x[ip->rd] = r;
      fa = r;
      fk = kFLogic32;
      break;
    }
  }
  if (Cond<kCond>(fa, fb, fk)) DISPATCH(ip + ip->i32);
  DISPATCH(ip + 2);
}

// ---- floating point (double precision, as the generic decoder)

struct B_VMovImm {
  static DART_FORCE_INLINE void Exec(BARGS) {
    SetVD(x, ip->rd, ip->imm);
  }
};

struct B_FMovDD {
  static DART_FORCE_INLINE void Exec(BARGS) {
    SetVD(x, ip->rd, VReg(x, ip->rn)->bits.i64[0]);
  }
};

struct B_FMovXD {
  static DART_FORCE_INLINE void Exec(BARGS) {
    x[ip->rd] = VReg(x, ip->rn)->bits.i64[0];
  }
};

struct B_FMovDX {
  static DART_FORCE_INLINE void Exec(BARGS) {
    SetVD(x, ip->rd, x[ip->rn]);
  }
};

struct B_FMovWS {
  static DART_FORCE_INLINE void Exec(BARGS) {
    x[ip->rd] = static_cast<uint32_t>(VReg(x, ip->rn)->bits.i64[0]);
  }
};

struct B_FMovSW {
  static DART_FORCE_INLINE void Exec(BARGS) {
    SetVD(x, ip->rd, static_cast<uint32_t>(x[ip->rn]));
  }
};

template <bool k64>
struct B_Scvtf {
  static DART_FORCE_INLINE void Exec(BARGS) {
    const double d = k64 ? static_cast<double>(x[ip->rn])
                         : static_cast<double>(static_cast<int32_t>(x[ip->rn]));
    SetVD(x, ip->rd, bit_cast<int64_t, double>(d));
  }
};

// fcvtzs / fcvtps / fcvtms (i32 = rounding: 0 zero, 1 +inf, 2 -inf), as the
// generic decoder (saturating).
template <bool k64>
struct B_Fcvt {
  static DART_FORCE_INLINE void Exec(BARGS) {
    const int64_t max = k64 ? INT64_MAX : INT32_MAX;
    const int64_t min = k64 ? INT64_MIN : INT32_MIN;
    double v = VD(x, ip->rn);
    if (ip->i32 == 1) {
      v = ceil(v);
    } else if (ip->i32 == 2) {
      v = floor(v);
    }
    int64_t r;
    if (v >= static_cast<double>(max)) {
      r = max;
    } else if (v <= static_cast<double>(min)) {
      r = min;
    } else {
      r = static_cast<int64_t>(v);
    }
    x[ip->rd] = k64 ? r : (r & 0xffffffffll);
  }
};

enum FpOp1 { kFAbs = 1, kFNeg = 2, kFSqrt = 3 };

template <int kOp>
struct B_Fp1 {
  static DART_FORCE_INLINE void Exec(BARGS) {
    const double v = VD(x, ip->rn);
    double r;
    switch (kOp) {
      case kFAbs: r = fabs(v); break;
      case kFNeg: r = -v; break;
      default: r = sqrt(v); break;
    }
    SetVD(x, ip->rd, bit_cast<int64_t, double>(r));
  }
};

enum FpOp2 { kFMul = 0, kFDiv = 1, kFAdd = 2, kFSub = 3 };

template <int kOp>
struct B_Fp2 {
  static DART_FORCE_INLINE void Exec(BARGS) {
    const double a = VD(x, ip->rn), b = VD(x, ip->rm);
    double r;
    switch (kOp) {
      case kFMul: r = a * b; break;
      case kFDiv: r = a / b; break;
      case kFAdd: r = a + b; break;
      default: r = a - b; break;
    }
    SetVD(x, ip->rd, bit_cast<int64_t, double>(r));
  }
};

// fcmp d(rn), d(rm) or #0.0 (kZero).
template <bool kZero>
struct B_Fcmp {
  static DART_FORCE_INLINE void Exec(BARGS) {
    const double a = VD(x, ip->rn);
    const double b = kZero ? 0.0 : VD(x, ip->rm);
    uint32_t nzcv;
    if (isnan(a) || isnan(b)) {
      nzcv = 0x3;  // C, V
    } else if (a == b) {
      nzcv = 0x6;  // Z, C
    } else if (a < b) {
      nzcv = 0x8;  // N
    } else {
      nzcv = 0x2;  // C
    }
    fa = nzcv;
    fb = 0;
    fk = kFNzcv;
  }
};

enum VLogicOp { kVAnd = 0, kVOrr, kVEor };

// and / orr (mov) / eor of 16-byte vectors.
template <int kOp>
struct B_VLogic {
  static DART_FORCE_INLINE void Exec(BARGS) {
    const simd_value_t* n = VReg(x, ip->rn);
    const simd_value_t* m = VReg(x, ip->rm);
    int64_t lo, hi;
    switch (kOp) {
      case kVAnd:
        lo = n->bits.i64[0] & m->bits.i64[0];
        hi = n->bits.i64[1] & m->bits.i64[1];
        break;
      case kVOrr:
        lo = n->bits.i64[0] | m->bits.i64[0];
        hi = n->bits.i64[1] | m->bits.i64[1];
        break;
      default:
        lo = n->bits.i64[0] ^ m->bits.i64[0];
        hi = n->bits.i64[1] ^ m->bits.i64[1];
        break;
    }
    simd_value_t* d = VReg(x, ip->rd);
    d->bits.i64[0] = lo;
    d->bits.i64[1] = hi;
  }
};

// ---- generic pairs: two straight-line instructions in one dispatch. Both
// ops keep their own fields (the second op stays in place for branches to
// it), so any pair of the bodies below composes without repacking.

template <class A, class B>
uword H_Seq2(HARGS) {
  A::Exec(ip, x, fa, fb, fk);
  B::Exec(ip + 1, x, fa, fb, fk);
  DISPATCH(ip + 2);
}

template <class... Bs>
struct BodyList {};

// The most frequent straight-line instructions in AOT code (profiles of the
// mixed-mode tests); kPairStores marks the stores.
using PairBodies = BodyList<B_Mov64,
                            B_MovImm,
                            B_AddImm64,
                            B_AddImm32,
                            B_AddSubShift<false, false, true, LSL>,
                            B_AddSubShift<true, false, true, LSL>,
                            B_AddSubShift<false, false, false, LSL>,
                            B_LogicImm<kAnd, true>,
                            B_LogicImm<kAnd, false>,
                            B_LogicImm<kEor, true>,
                            B_LogicShift<kAnd, true, LSL>,
                            B_LogicShift<kOrr, true, LSL>,
                            B_LogicShift<kEor, true, LSL>,
                            B_Lsl64,
                            B_Asr64,
                            B_Ubfm,
                            B_Madd<false, true>,
                            B_Ld<uint64_t, false, kAImm>,
                            B_Ld<uint8_t, false, kAImm>,
                            B_St<uint64_t, kAImm>>;
constexpr intptr_t kPairBodyCount = 20;
constexpr intptr_t kPairStoreBody = 19;

template <class A, class... Bs>
constexpr std::array<FastHandler, sizeof...(Bs)> PairRow(BodyList<Bs...>) {
  return {{&H_Seq2<A, Bs>...}};
}

template <class... As>
constexpr std::array<std::array<FastHandler, sizeof...(As)>, sizeof...(As)>
PairTable(BodyList<As...> list) {
  return {{PairRow<As>(list)...}};
}

template <class... Bs>
constexpr std::array<FastHandler, sizeof...(Bs)> SingleRow(BodyList<Bs...>) {
  return {{&H_Simple<Bs>...}};
}

constexpr auto kPairTable = PairTable(PairBodies{});
constexpr auto kPairSingles = SingleRow(PairBodies{});
static_assert(kPairSingles.size() == kPairBodyCount, "PairBodies");

intptr_t PairKind(FastHandler h) {
  for (intptr_t k = 0; k < kPairBodyCount; k++) {
    if (kPairSingles[k] == h) return k;
  }
  return -1;
}

// ---- superinstructions (built by the fusion pass in DecodeEntry; each
// covers several consecutive instructions, which keep their own ops for
// branches into the middle)

// mov rd, rm ; mov rn, ra
uword H_MovMov(HARGS) {
  x[ip->rd] = x[ip->rm];
  x[ip->rn] = x[ip->ra];
  DISPATCH(ip + 2);
}

// mov rd, rm ; mov rn, ra ; b (i32)
uword H_MovMovB(HARGS) {
  x[ip->rd] = x[ip->rm];
  x[ip->rn] = x[ip->ra];
  DISPATCH(ip + ip->i32);
}

// mov rd, rm ; b (i32)
uword H_MovB(HARGS) {
  x[ip->rd] = x[ip->rm];
  DISPATCH(ip + ip->i32);
}

// mov rd, rm ; ubfx rd, rd, #0, #32
uword H_MovUxtw(HARGS) {
  x[ip->rd] = static_cast<uint32_t>(x[ip->rm]);
  DISPATCH(ip + 2);
}

// mov rm, #imm ; mul rd, rn, rm
uword H_MovImmMul(HARGS) {
  x[ip->rm] = ip->imm;
  x[ip->rd] = static_cast<int64_t>(static_cast<uint64_t>(x[ip->rn]) *
                                   static_cast<uint64_t>(ip->imm));
  DISPATCH(ip + 2);
}

// Two 64-bit loads/stores with immediate offsets: first rd, [rn, #imm];
// second rm, [ra, #aux].
template <bool kLoad1, bool kLoad2>
uword H_Mem2(HARGS) {
  const uword a1 = static_cast<uword>(x[ip->rn]) + static_cast<uword>(ip->imm);
  if (kLoad1) {
    x[ip->rd] = Load<int64_t>(a1);
  } else {
    Store<int64_t>(a1, x[ip->rd]);
  }
  const uword a2 = static_cast<uword>(x[ip->ra]) + static_cast<uword>(ip->aux);
  if (kLoad2) {
    x[ip->rm] = Load<int64_t>(a2);
  } else {
    Store<int64_t>(a2, x[ip->rm]);
  }
  DISPATCH(ip + 2);
}

// ldr rd, [rn, #imm] ; cmp ra, rd ; b.cond (i32): the stack overflow check
// (ldr x16, [THR, #limit] ; cmp SP, x16 ; b.ls) and similar.
template <int kCond>
uword H_LdCmpBr(HARGS) {
  x[ip->rd] =
      Load<int64_t>(static_cast<uword>(x[ip->rn]) + static_cast<uword>(ip->imm));
  fa = static_cast<uint64_t>(x[ip->ra]);
  fb = static_cast<uint64_t>(x[ip->rd]);
  fk = kFSub64;
  if (Cond<kCond>(fa, fb, fk)) DISPATCH(ip + ip->i32);
  DISPATCH(ip + 3);
}

// The generational/incremental write barrier check (rn = value, rm = object):
//   ldurb w16, [rn, #-1] ; ldurb w17, [rm, #-1]
//   and x16, x17, x16, lsr #2 ; tst x16, x28, lsr #32 ; b.eq (i32)
uword H_BarrierCheck(HARGS) {
  x[16] = Load<uint8_t>(static_cast<uword>(x[ip->rn]) - 1);
  x[17] = Load<uint8_t>(static_cast<uword>(x[ip->rm]) - 1);
  const uint64_t r = static_cast<uint64_t>(x[17]) &
                     (static_cast<uint64_t>(x[16]) >> 2);
  x[16] = static_cast<int64_t>(r);
  fa = r & (static_cast<uint64_t>(x[28]) >> 32);
  fb = 0;
  fk = kFLogic64;
  if (fa == 0) DISPATCH(ip + ip->i32);
  DISPATCH(ip + 5);
}

#undef NEXT

// ------------------------------------------------------------------ decoder

// Register slot for a field in which 31 is XZR, as a source / destination.
inline uint8_t Src(int r) {
  return r == 31 ? kZR : static_cast<uint8_t>(r);
}
inline uint8_t Dst(int r) {
  return r == 31 ? kSink : static_cast<uint8_t>(r);
}
// A field in which 31 is CSP.
inline uint8_t Sp(int r) {
  return static_cast<uint8_t>(r);
}

// What the fusion pass needs to know about an op.
struct DecodeTag {
  int8_t fuse = -1;   // FuseKind of a flag-setting compare.
  int8_t bcond = -1;  // Condition of a b.cond (local target).
};

template <int kMode>
FastHandler LoadHandlerForMode(int size, bool is_signed, bool to_w) {
  switch (size) {
    case 0:
      if (!is_signed) return &H_Simple<B_Ld<uint8_t, false, kMode>>;
      return to_w ? &H_Simple<B_Ld<int8_t, true, kMode>> : &H_Simple<B_Ld<int8_t, false, kMode>>;
    case 1:
      if (!is_signed) return &H_Simple<B_Ld<uint16_t, false, kMode>>;
      return to_w ? &H_Simple<B_Ld<int16_t, true, kMode>> : &H_Simple<B_Ld<int16_t, false, kMode>>;
    case 2:
      return is_signed ? &H_Simple<B_Ld<int32_t, false, kMode>>
                       : &H_Simple<B_Ld<uint32_t, false, kMode>>;
    default:
      return &H_Simple<B_Ld<uint64_t, false, kMode>>;
  }
}

template <int kMode>
FastHandler StoreHandlerForMode(int size) {
  switch (size) {
    case 0: return &H_Simple<B_St<uint8_t, kMode>>;
    case 1: return &H_Simple<B_St<uint16_t, kMode>>;
    case 2: return &H_Simple<B_St<uint32_t, kMode>>;
    default: return &H_Simple<B_St<uint64_t, kMode>>;
  }
}

template <int kMode>
FastHandler VHandlerForMode(int bytes, bool load) {
  switch (bytes) {
    case 4: return load ? &H_Simple<B_VLdSt<4, true, kMode>> : &H_Simple<B_VLdSt<4, false, kMode>>;
    case 8: return load ? &H_Simple<B_VLdSt<8, true, kMode>> : &H_Simple<B_VLdSt<8, false, kMode>>;
    default:
      return load ? &H_Simple<B_VLdSt<16, true, kMode>> : &H_Simple<B_VLdSt<16, false, kMode>>;
  }
}

FastHandler LoadStoreHandler(int mode, bool vector, int size, bool load,
                             bool is_signed, bool to_w) {
#define BY_MODE(F, ...)                                                        \
  switch (mode) {                                                              \
    case kAImm: return F<kAImm>(__VA_ARGS__);                                  \
    case kAPre: return F<kAPre>(__VA_ARGS__);                                  \
    case kAPost: return F<kAPost>(__VA_ARGS__);                                \
    case kARegX: return F<kARegX>(__VA_ARGS__);                                \
    default: return F<kARegExt>(__VA_ARGS__);                                  \
  }
  if (vector) {
    BY_MODE(VHandlerForMode, 1 << size, load)
  } else if (load) {
    BY_MODE(LoadHandlerForMode, size, is_signed, to_w)
  } else {
    BY_MODE(StoreHandlerForMode, size)
  }
#undef BY_MODE
}

template <int kKind>
FastHandler PairHandlerFor(bool load, int mode) {
  switch (mode) {
    case kAImm: return load ? &H_Simple<B_Pair<kKind, true, kAImm>>
                            : &H_Simple<B_Pair<kKind, false, kAImm>>;
    case kAPre: return load ? &H_Simple<B_Pair<kKind, true, kAPre>>
                            : &H_Simple<B_Pair<kKind, false, kAPre>>;
    default: return load ? &H_Simple<B_Pair<kKind, true, kAPost>>
                         : &H_Simple<B_Pair<kKind, false, kAPost>>;
  }
}

bool DecodeLoadStoreReg(Instr* instr, FastOp* op) {
  const bool vector = instr->Bit(26) == 1;
  const int size = vector ? ((instr->Bit(23) << 2) | instr->SzField())
                          : instr->SzField();
  const int opc = instr->Bits(22, 2);
  bool load;
  bool is_signed = false;
  bool to_w = false;
  if (vector) {
    if (size < 2 || size > 4) return false;
    load = instr->Bit(22) == 1;
  } else {
    if (opc == 0) {
      load = false;
    } else {
      if (size == 3 && opc == 3) return false;  // Unallocated / prefetch.
      if (size == 3 && opc == 2) return false;  // prfm.
      if (size == 2 && opc == 3) return false;
      load = true;
      is_signed = instr->Bit(23) == 1;
      to_w = is_signed && instr->Bit(22) == 1;
    }
  }
  int mode;
  op->rn = Sp(instr->RnField());
  if (instr->Bit(24) == 1) {
    mode = kAImm;
    op->imm = static_cast<int64_t>(instr->Imm12Field()) << size;
  } else if (instr->Bit(21) == 0 && instr->Bits(10, 2) == 0) {
    mode = kAImm;  // Unscaled.
    op->imm = instr->SImm9Field();
  } else if (instr->Bit(21) == 0 && instr->Bit(10) == 1) {
    mode = instr->Bit(11) == 1 ? kAPre : kAPost;
    op->imm = instr->SImm9Field();
  } else if (instr->Bit(21) == 1 && instr->Bits(10, 2) == 2) {
    const int ext = instr->ExtendTypeField();
    op->rm = Src(instr->RmField());
    op->i32 = instr->Bit(12) == 1 ? size : 0;
    if (ext == UXTX || ext == SXTX) {
      mode = kARegX;
    } else if (ext == UXTW || ext == SXTW) {
      mode = kARegExt;
      op->ra = static_cast<uint8_t>(ext);
    } else {
      return false;
    }
  } else {
    return false;
  }
  if (vector) {
    op->rd = static_cast<uint8_t>(instr->VtField());
  } else {
    op->rd = load ? Dst(instr->RtField()) : Src(instr->RtField());
  }
  op->h = LoadStoreHandler(mode, vector, size, load, is_signed, to_w);
  return true;
}

bool DecodeLoadStorePair(Instr* instr, FastOp* op) {
  if (instr->Bits(30, 2) == 3) return false;
  int mode;
  switch (instr->Bits(23, 3)) {
    case 1: mode = kAPost; break;
    case 2: mode = kAImm; break;
    case 3: mode = kAPre; break;
    default: return false;  // Non-temporal and others.
  }
  const bool vector = instr->Bit(26) == 1;
  const bool load = instr->Bit(22) != 0;
  const int shift = vector ? 2 + instr->SzField() : 2 + instr->SFField();
  op->rn = Sp(instr->RnField());
  op->imm = static_cast<int64_t>(instr->SImm7Field()) * (1 << shift);
  if (vector) {
    op->rd = static_cast<uint8_t>(instr->VtField());
    op->rm = static_cast<uint8_t>(instr->Vt2Field());
    switch (shift) {
      case 2: op->h = PairHandlerFor<kPairS>(load, mode); break;
      case 3: op->h = PairHandlerFor<kPairD>(load, mode); break;
      case 4: op->h = PairHandlerFor<kPairQ>(load, mode); break;
      default: return false;
    }
    return true;
  }
  if (instr->Bit(30) == 1) return false;  // ldpsw: generic decoder.
  if (load) {
    op->rd = Dst(instr->RtField());
    op->rm = Dst(instr->Rt2Field());
  } else {
    op->rd = Src(instr->RtField());
    op->rm = Src(instr->Rt2Field());
  }
  op->h = instr->Bit(31) == 1 ? PairHandlerFor<kPairX>(load, mode)
                              : PairHandlerFor<kPairW>(load, mode);
  return true;
}

bool DecodeDPImmediate(Instr* instr, uword pc, FastOp* op, DecodeTag* tag) {
  const bool sf = instr->SFField() == 1;
  if (instr->IsMoveWideOp()) {
    const int opc = instr->Bits(29, 2);
    const int hw = instr->HWField();
    if (!sf && (hw & 2) != 0) return false;
    const int shift = hw << 4;
    const uint64_t shifted = static_cast<uint64_t>(instr->Imm16Field())
                             << shift;
    op->rd = Dst(instr->RdField());
    if (opc == 0 || opc == 2) {
      uint64_t value = opc == 0 ? ~shifted : shifted;
      if (!sf) value &= 0xffffffffull;
      op->h = &H_Simple<B_MovImm>;
      op->imm = static_cast<int64_t>(value);
    } else if (opc == 3) {
      op->h = &H_Simple<B_MovK>;
      op->imm = static_cast<int64_t>(shifted);
      uint64_t keep = ~(0xffffull << shift);
      if (!sf) keep &= 0xffffffffull;
      op->aux = keep;
    } else {
      return false;
    }
    return true;
  }
  if (instr->IsAddSubImmOp()) {
    const bool sub = instr->Bit(30) == 1;
    const bool flags = instr->HasS();
    int64_t imm = instr->Bit(22) == 1 ? (instr->Imm12Field() << 12)
                                      : instr->Imm12Field();
    op->rn = Sp(instr->RnField());
    op->rd = flags ? Dst(instr->RdField()) : Sp(instr->RdField());
    if (!flags) {
      op->h = sf ? &H_Simple<B_AddImm64> : &H_Simple<B_AddImm32>;
      op->imm = sub ? -imm : imm;
      return true;
    }
    op->imm = imm;
    if (sub) {
      op->h = sf ? &H_Simple<B_AddSImm<true, true>> : &H_Simple<B_AddSImm<true, false>>;
      tag->fuse = sf ? kFuseSubImm64 : kFuseSubImm32;
    } else {
      op->h = sf ? &H_Simple<B_AddSImm<false, true>> : &H_Simple<B_AddSImm<false, false>>;
      if (sf) tag->fuse = kFuseAddImm64;
    }
    return true;
  }
  if (instr->IsBitfieldOp()) {
    const int opc = instr->Bits(29, 2);
    if (opc == 1 || opc == 3) return false;  // bfm: generic decoder.
    if (instr->NField() != instr->SFField()) return false;
    const int width = sf ? 64 : 32;
    const int r = instr->ImmRField();
    const int s = instr->ImmSField();
    if (r >= width || s >= width) return false;
    op->rd = Dst(instr->RdField());
    op->rn = Src(instr->RnField());
    uint64_t mask;
    int right, left;
    if (s >= r) {
      mask = Utils::NBitMask<uint64_t>(s + 1);
      right = r;
      left = 0;
    } else {
      mask = Utils::NBitMask<uint64_t>(s + 1);
      right = 0;
      left = width - r;
    }
    if (opc == 2) {
      if (sf && s == 63) {
        op->h = &H_Simple<B_Lsr64>;
        op->i32 = r;
      } else if (sf && s + 1 == r) {
        op->h = &H_Simple<B_Lsl64>;
        op->i32 = 64 - r;
      } else if (right == 0 && left == 0) {
        op->h = &H_Simple<B_LogicImm<kAnd, true>>;
        op->imm = static_cast<int64_t>(mask);
      } else {
        op->h = &H_Simple<B_Ubfm>;
        op->imm = static_cast<int64_t>(mask);
        op->i32 = right;
        op->ra = static_cast<uint8_t>(left);
      }
      return true;
    }
    // sbfm: sign-extend from bit (s - r) mod width of the extracted field.
    const int high = (s - r) & (width - 1);
    if (sf && s == 63) {
      op->h = &H_Simple<B_Asr64>;
      op->i32 = r;
      return true;
    }
    op->h = sf ? &H_Simple<B_Sbfm<true>> : &H_Simple<B_Sbfm<false>>;
    op->imm = static_cast<int64_t>(mask);
    op->i32 = right;
    op->ra = static_cast<uint8_t>(left);
    op->rm = static_cast<uint8_t>(63 - high);
    return true;
  }
  if (instr->IsLogicalImmOp()) {
    const int opc = instr->Bits(29, 2);
    const uint64_t imm = instr->ImmLogical();
    if (imm == 0) return false;
    if (!sf && instr->NField() != 0) return false;
    op->rn = Src(instr->RnField());
    op->rd = opc == 3 ? Dst(instr->RdField()) : Sp(instr->RdField());
    op->imm = static_cast<int64_t>(sf ? imm : (imm & 0xffffffffull));
    if (opc == 1 && op->rn == kZR) {
      op->h = &H_Simple<B_MovImm>;  // mov rd, #imm (orr rd, zr, #imm).
      return true;
    }
    switch (opc) {
      case 0: op->h = sf ? &H_Simple<B_LogicImm<kAnd, true>> : &H_Simple<B_LogicImm<kAnd, false>>;
        break;
      case 1: op->h = sf ? &H_Simple<B_LogicImm<kOrr, true>> : &H_Simple<B_LogicImm<kOrr, false>>;
        break;
      case 2: op->h = sf ? &H_Simple<B_LogicImm<kEor, true>> : &H_Simple<B_LogicImm<kEor, false>>;
        break;
      default:
        op->h = sf ? &H_Simple<B_LogicImm<kAnds, true>> : &H_Simple<B_LogicImm<kAnds, false>>;
        tag->fuse = sf ? kFuseAndImm64 : kFuseAndImm32;
        break;
    }
    return true;
  }
  if (instr->IsPCRelOp()) {
    if (instr->Bit(31) != 0) return false;  // adrp: generic decoder.
    const uint64_t immhi = instr->SImm19Field();
    const uint64_t immlo = instr->Bits(29, 2);
    op->h = &H_Simple<B_MovImm>;
    op->rd = Dst(instr->RdField());
    op->imm = static_cast<int64_t>(pc + ((immhi << 2) | immlo));
    return true;
  }
  return false;
}

template <bool kSub, bool kFlags, bool k64>
FastHandler AddSubShiftHandler(int shift) {
  switch (shift) {
    case LSL: return &H_Simple<B_AddSubShift<kSub, kFlags, k64, LSL>>;
    case LSR: return &H_Simple<B_AddSubShift<kSub, kFlags, k64, LSR>>;
    default: return &H_Simple<B_AddSubShift<kSub, kFlags, k64, ASR>>;
  }
}

template <int kOp, bool k64>
FastHandler LogicShiftHandler(int shift) {
  switch (shift) {
    case LSL: return &H_Simple<B_LogicShift<kOp, k64, LSL>>;
    case LSR: return &H_Simple<B_LogicShift<kOp, k64, LSR>>;
    case ASR: return &H_Simple<B_LogicShift<kOp, k64, ASR>>;
    default: return &H_Simple<B_LogicShift<kOp, k64, ROR>>;
  }
}

template <int kOp>
FastHandler LogicShiftHandler(bool sf, int shift) {
  return sf ? LogicShiftHandler<kOp, true>(shift)
            : LogicShiftHandler<kOp, false>(shift);
}

bool DecodeDPRegister(Instr* instr, FastOp* op, DecodeTag* tag) {
  const bool sf = instr->SFField() == 1;
  if (instr->IsAddSubShiftExtOp()) {
    const bool sub = instr->Bit(30) == 1;
    const bool flags = instr->HasS();
    if (instr->IsShift()) {
      const int shift = instr->ShiftTypeField();
      if (shift == ROR) return false;
      const int amount = instr->Imm6Field();
      if (!sf && amount >= 32) return false;
      op->rd = Dst(instr->RdField());
      op->rn = Src(instr->RnField());
      op->rm = Src(instr->RmField());
      op->i32 = amount;
#define PICK(S, F)                                                             \
  (sf ? AddSubShiftHandler<S, F, true>(shift)                                 \
      : AddSubShiftHandler<S, F, false>(shift))
      if (sub) {
        op->h = flags ? PICK(true, true) : PICK(true, false);
      } else {
        op->h = flags ? PICK(false, true) : PICK(false, false);
      }
#undef PICK
      if (flags && shift == LSL && amount == 0) {
        if (sub) {
          tag->fuse = sf ? kFuseSubReg64 : kFuseSubReg32;
        } else if (sf) {
          tag->fuse = kFuseAddReg64;
        }
      }
      return true;
    }
    // Extended register.
    const int amount = instr->Imm3Field();
    if (amount > 4) return false;
    op->rd = flags ? Dst(instr->RdField()) : Sp(instr->RdField());
    op->rn = Sp(instr->RnField());
    op->rm = Src(instr->RmField());
    op->ra = static_cast<uint8_t>(instr->ExtendTypeField());
    op->i32 = amount;
    if (sub) {
      op->h = flags ? (sf ? &H_Simple<B_AddSubExt<true, true, true>>
                          : &H_Simple<B_AddSubExt<true, true, false>>)
                    : (sf ? &H_Simple<B_AddSubExt<true, false, true>>
                          : &H_Simple<B_AddSubExt<true, false, false>>);
    } else {
      op->h = flags ? (sf ? &H_Simple<B_AddSubExt<false, true, true>>
                          : &H_Simple<B_AddSubExt<false, true, false>>)
                    : (sf ? &H_Simple<B_AddSubExt<false, false, true>>
                          : &H_Simple<B_AddSubExt<false, false, false>>);
    }
    return true;
  }
  if (instr->IsLogicalShiftOp()) {
    const int op_bits = (instr->Bits(29, 2) << 1) | instr->Bit(21);
    const int shift = instr->ShiftTypeField();
    const int amount = instr->Imm6Field();
    if (!sf && amount >= 32) return false;
    op->rd = Dst(instr->RdField());
    op->rn = Src(instr->RnField());
    op->rm = Src(instr->RmField());
    op->i32 = amount;
    if (op_bits == kOrr && op->rn == kZR && amount == 0) {
      op->h = sf ? &H_Simple<B_Mov64> : &H_Simple<B_Mov32>;
      return true;
    }
    switch (op_bits) {
      case kAnd: op->h = LogicShiftHandler<kAnd>(sf, shift); break;
      case kBic: op->h = LogicShiftHandler<kBic>(sf, shift); break;
      case kOrr: op->h = LogicShiftHandler<kOrr>(sf, shift); break;
      case kOrn: op->h = LogicShiftHandler<kOrn>(sf, shift); break;
      case kEor: op->h = LogicShiftHandler<kEor>(sf, shift); break;
      case kEon: op->h = LogicShiftHandler<kEon>(sf, shift); break;
      case kAnds:
        op->h = LogicShiftHandler<kAnds>(sf, shift);
        if (sf && shift == LSL && amount == 0) tag->fuse = kFuseAndReg64;
        break;
      default: op->h = LogicShiftHandler<kBics>(sf, shift); break;
    }
    return true;
  }
  if (instr->IsMiscDP2SourceOp()) {
    if (instr->Bit(29) != 0) return false;
    op->rd = Dst(instr->RdField());
    op->rn = Src(instr->RnField());
    op->rm = Src(instr->RmField());
    switch (instr->Bits(10, 5)) {
      case 2: op->h = sf ? &H_Simple<B_Div<false, true>> : &H_Simple<B_Div<false, false>>; break;
      case 3: op->h = sf ? &H_Simple<B_Div<true, true>> : &H_Simple<B_Div<true, false>>; break;
      case 8: op->h = sf ? &H_Simple<B_ShiftV<LSL, true>> : &H_Simple<B_ShiftV<LSL, false>>; break;
      case 9: op->h = sf ? &H_Simple<B_ShiftV<LSR, true>> : &H_Simple<B_ShiftV<LSR, false>>; break;
      case 10:
        op->h = sf ? &H_Simple<B_ShiftV<ASR, true>> : &H_Simple<B_ShiftV<ASR, false>>;
        break;
      default: return false;
    }
    return true;
  }
  if (instr->IsMiscDP3SourceOp()) {
    op->rd = Dst(instr->RdField());
    op->rn = Src(instr->RnField());
    op->rm = Src(instr->RmField());
    op->ra = Src(instr->RaField());
    const int op54 = instr->Bits(29, 2);
    const int op31 = instr->Bits(21, 3);
    const int o0 = instr->Bit(15);
    if (op54 == 0 && op31 == 0) {
      op->h = o0 == 0 ? (sf ? &H_Simple<B_Madd<false, true>> : &H_Simple<B_Madd<false, false>>)
                      : (sf ? &H_Simple<B_Madd<true, true>> : &H_Simple<B_Madd<true, false>>);
      return true;
    }
    if (!sf || op54 != 0 || o0 != 0) return false;
    switch (op31) {
      case 1: op->h = &H_Simple<B_Maddl<true>>; return true;
      case 5: op->h = &H_Simple<B_Maddl<false>>; return true;
      case 2: op->h = &H_Simple<B_Smulh>; return true;
      case 6: op->h = &H_Simple<B_Umulh>; return true;
      default: return false;
    }
  }
  if (instr->IsConditionalSelectOp()) {
    const int op2 = instr->Bits(10, 2);
    const int o = instr->Bits(29, 2);
    if (op2 > 1 || (o != 0 && o != 2)) return false;
    op->rd = Dst(instr->RdField());
    op->rn = Src(instr->RnField());
    op->rm = Src(instr->RmField());
    op->i32 = instr->SelectConditionField();
    const int kind = (o == 2 ? 2 : 0) + op2;
    switch (kind) {
      case kCsel: op->h = sf ? &H_Simple<B_Csel<kCsel, true>> : &H_Simple<B_Csel<kCsel, false>>;
        break;
      case kCsinc:
        op->h = sf ? &H_Simple<B_Csel<kCsinc, true>> : &H_Simple<B_Csel<kCsinc, false>>;
        break;
      case kCsinv:
        op->h = sf ? &H_Simple<B_Csel<kCsinv, true>> : &H_Simple<B_Csel<kCsinv, false>>;
        break;
      default:
        op->h = sf ? &H_Simple<B_Csel<kCsneg, true>> : &H_Simple<B_Csel<kCsneg, false>>;
        break;
    }
    return true;
  }
  return false;
}

FastHandler BCondHandler(int cond) {
  switch (cond) {
#define C(c) case c: return &H_BCond<c>;
    C(EQ) C(NE) C(CS) C(CC) C(MI) C(PL) C(VS) C(VC)
    C(HI) C(LS) C(GE) C(LT) C(GT) C(LE)
#undef C
    default: return &H_B;  // al / nv
  }
}

template <int kKind>
FastHandler FusedFor(int cond) {
  switch (cond) {
#define C(c) case c: return &H_Fused<kKind, c>;
    C(EQ) C(NE) C(CS) C(CC) C(MI) C(PL) C(VS) C(VC)
    C(HI) C(LS) C(GE) C(LT) C(GT) C(LE)
#undef C
    default: return nullptr;
  }
}

FastHandler FusedHandler(int kind, int cond) {
  switch (kind) {
    case kFuseSubImm64: return FusedFor<kFuseSubImm64>(cond);
    case kFuseSubReg64: return FusedFor<kFuseSubReg64>(cond);
    case kFuseSubImm32: return FusedFor<kFuseSubImm32>(cond);
    case kFuseSubReg32: return FusedFor<kFuseSubReg32>(cond);
    case kFuseAddImm64: return FusedFor<kFuseAddImm64>(cond);
    case kFuseAddReg64: return FusedFor<kFuseAddReg64>(cond);
    case kFuseAndImm64: return FusedFor<kFuseAndImm64>(cond);
    case kFuseAndReg64: return FusedFor<kFuseAndReg64>(cond);
    default: return FusedFor<kFuseAndImm32>(cond);
  }
}

FastHandler LdCmpBrHandler(int cond) {
  switch (cond) {
#define C(c) case c: return &H_LdCmpBr<c>;
    C(EQ) C(NE) C(CS) C(CC) C(MI) C(PL) C(VS) C(VC)
    C(HI) C(LS) C(GE) C(LT) C(GT) C(LE)
#undef C
    default: return nullptr;
  }
}

// Superinstructions: replaces ops[i] with an op covering ops[i..i+k) and
// returns k, or returns 0. Later ops are left alone (branch targets).
intptr_t Superinstruction(FastOp* ops,
                          const DecodeTag* tags,
                          const bool* fused,
                          intptr_t i,
                          intptr_t n,
                          uword pc) {
  const FastHandler kLd = &H_Simple<B_Ld<uint64_t, false, kAImm>>;
  const FastHandler kSt = &H_Simple<B_St<uint64_t, kAImm>>;
  FastOp& a = ops[i];
  auto at = [&](intptr_t k) -> const FastOp* {
    return i + k < n ? &ops[i + k] : nullptr;
  };
  // Write barrier check (exact words, see H_BarrierCheck).
  if (i + 4 < n) {
    const uint32_t* w = reinterpret_cast<const uint32_t*>(pc);
    if ((w[0] & 0xFFFFFC1F) == 0x385FF010 &&
        (w[1] & 0xFFFFFC1F) == 0x385FF011 && w[2] == 0x8a500a30 &&
        w[3] == 0xea5c821f && ops[i + 4].h == &H_BCond<EQ>) {
      const uint8_t value = static_cast<uint8_t>((w[0] >> 5) & 31);
      const uint8_t object = static_cast<uint8_t>((w[1] >> 5) & 31);
      if (value != 31 && object != 31) {
        const int32_t delta = ops[i + 4].i32 + 4;
        a = FastOp();
        a.h = &H_BarrierCheck;
        a.rn = value;
        a.rm = object;
        a.i32 = delta;
        return 5;
      }
    }
  }
  const FastOp* b = at(1);
  if (b == nullptr) return 0;
  // ldr rd, [rn, #imm] ; (fused) cmp ra, rd ; b.cond
  if (a.h == kLd && fused[i + 1] && tags[i + 1].fuse == kFuseSubReg64 &&
      b->rd == kSink && b->rm == a.rd && b->rn != a.rd && a.rd != kSink) {
    FastHandler h = LdCmpBrHandler(tags[i + 2].bcond);
    if (h != nullptr) {
      const uint8_t compared = b->rn;
      const int32_t delta = b->i32 + 1;
      a.h = h;
      a.ra = compared;
      a.i32 = delta;
      return 3;
    }
  }
  if (a.h == &H_Simple<B_Mov64>) {
    const FastOp* c = at(2);
    if (b->h == &H_Simple<B_Mov64> && c != nullptr && c->h == &H_B) {
      const int32_t delta = c->i32 + 2;
      a.h = &H_MovMovB;
      a.rn = b->rd;
      a.ra = b->rm;
      a.i32 = delta;
      return 3;
    }
    if (b->h == &H_Simple<B_Mov64>) {
      a.h = &H_MovMov;
      a.rn = b->rd;
      a.ra = b->rm;
      return 2;
    }
    if (b->h == &H_B) {
      a.h = &H_MovB;
      a.i32 = b->i32 + 1;
      return 2;
    }
    if (b->h == &H_Simple<B_LogicImm<kAnd, true>> && b->imm == 0xffffffffll &&
        b->rd == a.rd && b->rn == a.rd) {
      a.h = &H_MovUxtw;
      return 2;
    }
    return 0;
  }
  if (a.h == &H_Simple<B_MovImm> && b->h == &H_Simple<B_Madd<false, true>> && b->ra == kZR &&
      a.rd != kSink) {
    uint8_t other;
    if (b->rm == a.rd && b->rn != a.rd) {
      other = b->rn;
    } else if (b->rn == a.rd && b->rm != a.rd) {
      other = b->rm;
    } else {
      return 0;
    }
    const uint8_t product = b->rd;
    a.h = &H_MovImmMul;
    a.rm = a.rd;
    a.rd = product;
    a.rn = other;
    return 2;
  }
  if ((a.h == kLd || a.h == kSt) && (b->h == kLd || b->h == kSt)) {
    const bool load1 = a.h == kLd, load2 = b->h == kLd;
    a.rm = b->rd;
    a.ra = b->rn;
    a.aux = static_cast<uint64_t>(b->imm);
    a.h = load1 ? (load2 ? &H_Mem2<true, true> : &H_Mem2<true, false>)
                : (load2 ? &H_Mem2<false, true> : &H_Mem2<false, false>);
    return 2;
  }
  return 0;
}

// Static branch target outside the current entry.
void DecodeFarBranch(uword target, bool link, uword ret, FastOp* op) {
  const intptr_t j = MixedMode::EntryIndex(target);
  if (link) op->imm = static_cast<int64_t>(MixedMode::ReturnGateFor(ret));
  if (j >= 0 && !MixedMode::EntryLinked(j)) {
    op->h = link ? &H_BlSim : &H_BSim;
    op->aux = reinterpret_cast<uword>(&g_entry_ops[j]);
    op->i32 = static_cast<int32_t>((target - MixedMode::EntryStart(j)) >> 2);
    return;
  }
  op->h = link ? &H_BlNative : &H_BNative;
  op->aux = j >= 0 ? target + MixedMode::EntryDelta(j) : target;
}

bool DecodeBranch(Instr* instr,
                  uword pc,
                  uword lo,
                  uword hi,
                  FastOp* op,
                  DecodeTag* tag) {
  auto local = [&](int64_t offset, int32_t* delta) {
    const uword target = pc + offset;
    if (target < lo || target >= hi) return false;
    *delta = static_cast<int32_t>((static_cast<intptr_t>(target) -
                                   static_cast<intptr_t>(pc)) >>
                                  2);
    return true;
  };
  if (instr->IsConditionalBranchOp()) {
    if (instr->Bit(24) != 0 || instr->Bit(4) != 0) return false;
    if (!local(instr->SImm19Field() << 2, &op->i32)) return false;
    const int cond = instr->ConditionField();
    op->h = BCondHandler(cond);
    if (cond < AL) tag->bcond = static_cast<int8_t>(cond);
    return true;
  }
  if (instr->IsCompareAndBranchOp()) {
    if (!local(instr->SImm19Field() << 2, &op->i32)) return false;
    op->rn = Src(instr->RtField());
    const bool nz = instr->Bit(24) == 1;
    const bool sf = instr->SFField() == 1;
    op->h = nz ? (sf ? &H_Cbz<true, true> : &H_Cbz<true, false>)
               : (sf ? &H_Cbz<false, true> : &H_Cbz<false, false>);
    return true;
  }
  if (instr->IsTestAndBranchOp()) {
    if (!local(instr->SImm14Field() << 2, &op->i32)) return false;
    const int bit = instr->Bits(19, 5) | (instr->Bit(31) << 5);
    op->rn = Src(instr->RtField());
    op->imm = static_cast<int64_t>(1ull << bit);
    op->h = instr->Bit(24) == 1 ? &H_Tbz<true> : &H_Tbz<false>;
    return true;
  }
  if (instr->IsUnconditionalBranchOp()) {
    const bool link = instr->Bit(31) == 1;
    const uword target = pc + (instr->SImm26Field() << 2);
    const uword ret = pc + Instr::kInstrSize;
    int32_t delta;
    if (local(instr->SImm26Field() << 2, &delta)) {
      op->i32 = delta;
      if (link) {
        op->h = &H_BLocalLink;
        op->imm = static_cast<int64_t>(MixedMode::ReturnGateFor(ret));
      } else {
        op->h = &H_B;
      }
      return true;
    }
    DecodeFarBranch(target, link, ret, op);
    return true;
  }
  if (instr->IsUnconditionalBranchRegOp()) {
    if (instr->Bits(0, 5) != 0 || instr->Bits(10, 6) != 0 ||
        instr->Bits(16, 5) != 0x1f) {
      return false;
    }
    op->rn = Src(instr->RnField());
    switch (instr->Bits(21, 4)) {
      case 0:  // br
      case 2:  // ret
        op->h = &H_Br;
        return true;
      case 1:  // blr
        op->h = &H_Blr;
        op->imm = static_cast<int64_t>(
            MixedMode::ReturnGateFor(pc + Instr::kInstrSize));
        return true;
      default:
        return false;
    }
  }
  if (instr->IsSystemOp()) {
    // hint (including nop); barriers and clrex use the generic decoder.
    if (instr->Bits(12, 20) == 0xD5032 && instr->Bits(0, 5) == 0x1f) {
      op->h = &H_Simple<B_Nop>;
      return true;
    }
    return false;
  }
  return false;
}

bool DecodeFP(Instr* instr, FastOp* op) {
  if (instr->IsFPImmOp()) {
    if (instr->Bit(31) != 0 || instr->Bit(29) != 0 || instr->Bit(23) != 0 ||
        instr->Bits(5, 5) != 0 || instr->Bit(22) != 1) {
      return false;
    }
    op->h = &H_Simple<B_VMovImm>;
    op->rd = static_cast<uint8_t>(instr->VdField());
    op->imm = Instr::VFPExpandImm(instr->Imm8Field());
    return true;
  }
  if (instr->IsFPIntCvtOp()) {
    if (instr->Bit(29) != 0) return false;
    const bool sf = instr->SFField() == 1;
    const int type = instr->Bits(22, 2);
    const int opcode = instr->Bits(16, 5);
    if (!sf && type == 0) {
      if (opcode == 6) {
        op->h = &H_Simple<B_FMovWS>;
        op->rd = Dst(instr->RdField());
        op->rn = static_cast<uint8_t>(instr->VnField());
        return true;
      }
      if (opcode == 7) {
        op->h = &H_Simple<B_FMovSW>;
        op->rd = static_cast<uint8_t>(instr->VdField());
        op->rn = Src(instr->RnField());
        return true;
      }
      return false;
    }
    if (type != 1) return false;
    switch (opcode) {
      case 2:
        op->h = sf ? &H_Simple<B_Scvtf<true>> : &H_Simple<B_Scvtf<false>>;
        op->rd = static_cast<uint8_t>(instr->VdField());
        op->rn = Src(instr->RnField());
        return true;
      case 6:
        if (!sf) return false;
        op->h = &H_Simple<B_FMovXD>;
        op->rd = Dst(instr->RdField());
        op->rn = static_cast<uint8_t>(instr->VnField());
        return true;
      case 7:
        if (!sf) return false;
        op->h = &H_Simple<B_FMovDX>;
        op->rd = static_cast<uint8_t>(instr->VdField());
        op->rn = Src(instr->RnField());
        return true;
      case 8:
      case 16:
      case 24:
        op->h = sf ? &H_Simple<B_Fcvt<true>> : &H_Simple<B_Fcvt<false>>;
        op->rd = Dst(instr->RdField());
        op->rn = static_cast<uint8_t>(instr->VnField());
        op->i32 = opcode == 8 ? 1 : (opcode == 16 ? 2 : 0);
        return true;
      default:
        return false;
    }
  }
  if (instr->IsFPOneSourceOp()) {
    if (instr->Bit(22) != 1) return false;
    op->rd = static_cast<uint8_t>(instr->VdField());
    op->rn = static_cast<uint8_t>(instr->VnField());
    switch (instr->Bits(15, 6)) {
      case 0: op->h = &H_Simple<B_FMovDD>; return true;
      case 1: op->h = &H_Simple<B_Fp1<kFAbs>>; return true;
      case 2: op->h = &H_Simple<B_Fp1<kFNeg>>; return true;
      case 3: op->h = &H_Simple<B_Fp1<kFSqrt>>; return true;
      default: return false;
    }
  }
  if (instr->IsFPTwoSourceOp()) {
    if (instr->Bits(22, 2) != 1) return false;
    op->rd = static_cast<uint8_t>(instr->VdField());
    op->rn = static_cast<uint8_t>(instr->VnField());
    op->rm = static_cast<uint8_t>(instr->VmField());
    switch (instr->Bits(12, 4)) {
      case 0: op->h = &H_Simple<B_Fp2<kFMul>>; return true;
      case 1: op->h = &H_Simple<B_Fp2<kFDiv>>; return true;
      case 2: op->h = &H_Simple<B_Fp2<kFAdd>>; return true;
      case 3: op->h = &H_Simple<B_Fp2<kFSub>>; return true;
      default: return false;
    }
  }
  if (instr->IsFPCompareOp()) {
    if (instr->Bit(22) != 1) return false;
    op->rn = static_cast<uint8_t>(instr->VnField());
    op->rm = static_cast<uint8_t>(instr->VmField());
    if (instr->Bits(3, 2) == 0) {
      op->h = &H_Simple<B_Fcmp<false>>;
      return true;
    }
    if (instr->Bits(3, 2) == 1 && instr->VmField() == V0) {
      op->h = &H_Simple<B_Fcmp<true>>;
      return true;
    }
    return false;
  }
  return false;
}

// Decodes the instruction at pc (in entry [lo, hi)). Instructions without a
// fast handler run through the generic decoder (H_Slow).
void DecodeOne(uword pc, uword lo, uword hi, FastOp* op, DecodeTag* tag) {
  Instr* instr = reinterpret_cast<Instr*>(pc);
  bool ok = false;
  if (instr->IsLoadStoreOp()) {
    if (instr->IsAtomicMemoryOp()) {
      ok = false;
    } else if (instr->IsLoadStoreRegOp()) {
      ok = DecodeLoadStoreReg(instr, op);
    } else if (instr->IsLoadStoreRegPairOp()) {
      ok = DecodeLoadStorePair(instr, op);
    }
  } else if (instr->IsDPImmediateOp()) {
    ok = DecodeDPImmediate(instr, pc, op, tag);
  } else if (instr->IsCompareBranchOp()) {
    ok = DecodeBranch(instr, pc, lo, hi, op, tag);
  } else if (instr->IsDPRegisterOp()) {
    ok = DecodeDPRegister(instr, op, tag);
  } else if (instr->IsDPSimd1Op()) {
    // Bitwise and / orr / eor of 16-byte vectors (as the generic decoder:
    // Q = 1, opcode 3, size 00 / 10 / 00 with U = 0 / 0 / 1).
    if (instr->IsSIMDThreeSameOp() && instr->Bit(30) == 1 &&
        instr->Bits(11, 5) == 3 && instr->Bit(22) == 0) {
      const int u = instr->Bit(29), b23 = instr->Bit(23);
      op->rd = static_cast<uint8_t>(instr->VdField());
      op->rn = static_cast<uint8_t>(instr->VnField());
      op->rm = static_cast<uint8_t>(instr->VmField());
      if (u == 0) {
        op->h = b23 == 0 ? &H_Simple<B_VLogic<kVAnd>> : &H_Simple<B_VLogic<kVOrr>>;
        ok = true;
      } else if (b23 == 0) {
        op->h = &H_Simple<B_VLogic<kVEor>>;
        ok = true;
      }
    }
  } else if (instr->IsDPSimd2Op()) {
    ok = instr->IsFPOp() && DecodeFP(instr, op);
  }
  if (!ok) {
    *op = FastOp();
    *tag = DecodeTag();
    op->h = &H_Slow;
    op->imm = static_cast<int64_t>(pc);
  }
}

// ------------------------------------------------------------ verify mode

uword H_Verify(HARGS);

struct VerifyBlock {
  const FastOp* ops;
  intptr_t n;
  uword lo;
  FastHandler* real;
  uint8_t* length;  // Instructions covered by the op (2 when fused).
  uint64_t* count;  // Executions (DART_MIXED_PROFILE_OUT).
};

VerifyBlock* verify_blocks = nullptr;
intptr_t verify_block_count = 0;
intptr_t verify_block_capacity = 0;
std::atomic<int64_t> verify_ops = {0};
std::atomic<int64_t> verify_instructions = {0};
std::atomic<int64_t> verify_mismatches = {0};

// Caller holds DecodeMutex() (registration) or not (lookup; blocks are never
// removed and the array only grows under the mutex).
const VerifyBlock* FindVerifyBlock(const FastOp* ip) {
  MutexLocker ml(DecodeMutex());
  for (intptr_t i = 0; i < verify_block_count; i++) {
    const VerifyBlock& b = verify_blocks[i];
    if (ip >= b.ops && ip < b.ops + b.n + 1) return &b;
  }
  return nullptr;
}

// ------------------------------------------------------------ entry decode

// Liveness of v8..v31 (one bit) for the gates' register save
// (DartMixedSimEnter saves v0..v7 always, v8..v31 unless the gate's flag is
// set). The Dart ABI passes and returns floating-point values only in v0..v7
// (arguments in v0..v5, returns in v0..v3, FFI arguments in v0..v7) and no
// FPU value lives across a Dart call; stubs that preserve FPU registers only
// do so for their caller. So v8..v31 can only be read by FP/SIMD code of the
// function itself, or by code outside the entry. Conservatively:
//   * any FP/SIMD instruction or FP/SIMD load/store reads all V registers;
//   * a branch to a register, a branch leaving the entry, a fall-through past
//     its end and an unrecognized branch read all V registers;
//   * calls (bl, blr) and ret read none of v8..v31 (arguments and results
//     are in v0..v7), and calls do not kill anything (callee-preserving
//     stubs keep values live across them);
//   * no instruction kills: a register written before it is read is still
//     considered live (less precise, never unsound).
// high_live[i] = 1 when v8..v31 may be read on some path from instruction i.
void ComputeHighVLiveness(uword lo, intptr_t n, uint8_t* high_live) {
  // Successors: -1 = none; next = i + 1 unless control does not fall through.
  intptr_t* target = reinterpret_cast<intptr_t*>(
      malloc(Utils::Maximum<intptr_t>(n, 1) * sizeof(intptr_t)));
  bool* falls = reinterpret_cast<bool*>(malloc(Utils::Maximum<intptr_t>(n, 1)));
  for (intptr_t i = 0; i < n; i++) {
    const uint32_t w = *reinterpret_cast<const uint32_t*>(lo + i * 4);
    bool use = false;
    bool fall = true;
    intptr_t t = -1;
    int64_t offset = 0;
    bool branch = false;
    if ((w & 0x0A000000) == 0x08000000) {
      use = (w & 0x04000000) != 0;  // FP/SIMD load or store.
    } else if ((w & 0x0E000000) == 0x0E000000) {
      use = true;  // FP/SIMD data processing.
    } else if ((w & 0xFF000010) == 0x54000000 ||  // b.cond
               (w & 0x7E000000) == 0x34000000) {  // cbz / cbnz
      offset = static_cast<int64_t>(static_cast<int32_t>(w << 8) >> 13) * 4;
      branch = true;
    } else if ((w & 0x7E000000) == 0x36000000) {  // tbz / tbnz
      offset = static_cast<int64_t>(static_cast<int32_t>(w << 13) >> 18) * 4;
      branch = true;
    } else if ((w & 0xFC000000) == 0x14000000) {  // b
      offset = static_cast<int64_t>(static_cast<int32_t>(w << 6) >> 6) * 4;
      branch = true;
      fall = false;
    } else if ((w & 0xFC000000) == 0x94000000 ||  // bl
               (w & 0xFFFFFC1F) == 0xD63F0000) {  // blr
      // A call: falls through to the return point.
    } else if ((w & 0xFFFFFC1F) == 0xD65F0000) {  // ret
      fall = false;
    } else if ((w & 0xFE000000) == 0xD6000000) {  // br and other branches
      use = true;
      fall = false;
    } else if ((w & 0xFF000000) == 0xD4000000) {  // brk, hlt, svc
      fall = false;
    }
    if (branch) {
      const intptr_t k = i + offset / 4;
      if (k >= 0 && k < n) {
        t = k;
      } else {
        use = true;  // Leaves the entry.
      }
    }
    if (fall && i + 1 == n) use = true;  // Falls off the end.
    high_live[i] = use ? 1 : 0;
    target[i] = t;
    falls[i] = fall;
  }
  bool changed = true;
  while (changed) {
    changed = false;
    for (intptr_t i = n - 1; i >= 0; i--) {
      if (high_live[i] != 0) continue;
      if ((falls[i] && i + 1 < n && high_live[i + 1] != 0) ||
          (target[i] >= 0 && high_live[target[i]] != 0)) {
        high_live[i] = 1;
        changed = true;
      }
    }
  }
  high_live[n] = 1;  // Past the end.
  free(target);
  free(falls);
}

// Whether the entry may write any of v8..v31 (the exit-side counterpart of
// ComputeHighVLiveness). Exits from an entry that cannot write them return
// with bit 0 of the native target set and DartMixedSimEnter does not reload
// v8..v31 from the area. That is sound because a value in v8..v31 that native
// code may read after the exit (a value live across a call to a
// register-preserving stub, or a C callee-saved d8..d15 restored by an FFI
// callback's epilogue before returning to C) must have been written by the
// exiting function itself: the Dart ABI passes and returns FP values only in
// v0..v7, and no FPU value lives across a Dart call. Conservatively, every
// FP/SIMD data-processing instruction and FP/SIMD load/store counts as a write
// of its register field(s) (stores and FP-to-GPR moves included), and
// load/store structure instructions count as writing all of them.
bool EntryMayWriteHighV(uword lo, intptr_t n) {
  for (intptr_t i = 0; i < n; i++) {
    const uint32_t w = *reinterpret_cast<const uint32_t*>(lo + i * 4);
    const uint32_t rt = w & 31;
    if ((w & 0x0A000000) == 0x08000000) {
      if ((w & 0x04000000) == 0) continue;     // GPR load/store.
      if ((w & 0x3A000000) == 0x08000000) {    // SIMD structure ld/st.
        return true;
      }
      if (rt >= 8) return true;
      if ((w & 0x38000000) == 0x28000000 && ((w >> 10) & 31) >= 8) {
        return true;                           // Pair: Rt2.
      }
    } else if ((w & 0x0E000000) == 0x0E000000) {
      if ((w & 0xFF20FC07) == 0x1E202000) continue;  // fcmp / fcmpe.
      if (rt >= 8) return true;                // FP/SIMD data processing.
    }
  }
  return false;
}

bool IsExitHandler(FastHandler h) {
  return h == &H_BNative || h == &H_BlNative || h == &H_Br || h == &H_Blr ||
         h == &H_End;
}

const FastOp* DecodeEntry(intptr_t j) {
  MutexLocker ml(DecodeMutex());
  const FastOp* existing = g_entry_ops[j].load(std::memory_order_relaxed);
  if (existing != nullptr) return existing;
  RELEASE_ASSERT(!MixedMode::EntryLinked(j));
  const uword lo = MixedMode::EntryStart(j);
  const uword hi = MixedMode::EntryLimit(j);
  const intptr_t n = static_cast<intptr_t>((hi - lo) >> 2);
  FastOp* ops = reinterpret_cast<FastOp*>(calloc(n + 1, sizeof(FastOp)));
  DecodeTag* tags = reinterpret_cast<DecodeTag*>(malloc(
      Utils::Maximum<intptr_t>(n, 1) * sizeof(DecodeTag)));
  for (intptr_t i = 0; i < n; i++) {
    tags[i] = DecodeTag();
    DecodeOne(lo + i * Instr::kInstrSize, lo, hi, &ops[i], &tags[i]);
  }
  // Running past the end continues at the next patch pc.
  ops[n].h = &H_End;
  ops[n].imm = static_cast<int64_t>(hi);
  uint8_t* length = nullptr;
  const bool verify = Mode() == kModeVerify;
  if (verify) {
    length = reinterpret_cast<uint8_t*>(malloc(n + 1));
    memset(length, 1, n + 1);
  }
  // Fuse compare + b.cond. The b.cond keeps its own op for branches to it.
  bool* fused = reinterpret_cast<bool*>(calloc(n + 1, sizeof(bool)));
  for (intptr_t i = 0; i + 1 < n; i++) {
    if (tags[i].fuse < 0 || tags[i + 1].bcond < 0) continue;
    FastHandler h = FusedHandler(tags[i].fuse, tags[i + 1].bcond);
    if (h == nullptr) continue;
    ops[i].h = h;
    ops[i].i32 = ops[i + 1].i32 + 1;
    fused[i] = true;
    if (length != nullptr) length[i] = 2;
  }
  // Superinstructions for common AOT idioms (DART_MIXED_SUPER=0 disables).
  static const bool super = getenv("DART_MIXED_SUPER") == nullptr ||
                            getenv("DART_MIXED_SUPER")[0] != '0';
  for (intptr_t i = 0; super && i < n; i++) {
    const intptr_t k =
        Superinstruction(ops, tags, fused, i, n, lo + i * Instr::kInstrSize);
    if (k > 0 && length != nullptr) length[i] = static_cast<uint8_t>(k);
  }
  // Generic pairs of straight-line ops. Verify mode leaves out pairs with a
  // store: its generic replay could not undo the decoder's store.
  for (intptr_t i = 0; super && i + 1 < n; i++) {
    const intptr_t a = PairKind(ops[i].h);
    if (a < 0) continue;
    const intptr_t b = PairKind(ops[i + 1].h);
    if (b < 0) continue;
    if (verify && (a == kPairStoreBody || b == kPairStoreBody)) continue;
    ops[i].h = kPairTable[a][b];
    if (length != nullptr) length[i] = 2;
  }
  free(fused);
  free(tags);
  // Exits that may skip reloading v8..v31 (ra = 1; see EntryMayWriteHighV).
  if (!EntryMayWriteHighV(lo, n)) {
    for (intptr_t i = 0; i <= n; i++) {
      if (IsExitHandler(ops[i].h)) ops[i].ra = 1;
    }
  }
  if (verify) {
    FastHandler* real =
        reinterpret_cast<FastHandler*>(malloc((n + 1) * sizeof(FastHandler)));
    for (intptr_t i = 0; i <= n; i++) {
      real[i] = ops[i].h;
      ops[i].h = &H_Verify;
    }
    if (verify_block_count == verify_block_capacity) {
      verify_block_capacity = Utils::Maximum<intptr_t>(64, 2 * verify_block_capacity);
      verify_blocks = reinterpret_cast<VerifyBlock*>(realloc(
          verify_blocks, verify_block_capacity * sizeof(VerifyBlock)));
    }
    uint64_t* count =
        reinterpret_cast<uint64_t*>(calloc(n + 1, sizeof(uint64_t)));
    verify_blocks[verify_block_count++] = {ops, n, lo, real, length, count};
  }
  uint8_t* high_live = reinterpret_cast<uint8_t*>(malloc(n + 1));
  ComputeHighVLiveness(lo, n, high_live);
  g_entry_high_live[j] = high_live;
  g_decoded_entries.fetch_add(1, std::memory_order_relaxed);
  g_decoded_bytes.fetch_add((n + 1) * sizeof(FastOp),
                            std::memory_order_relaxed);
  g_entry_ops[j].store(ops, std::memory_order_release);
  return ops;
}

// ------------------------------------------------------------ verify mode

struct VerifyRegs {
  int64_t x[kMixedRegisterSlots];
  simd_value_t v[kNumberOfVRegisters];
};

struct VerifyState {
  bool stepping = false;
  const FastOp* next = nullptr;
  uint64_t fa = 0, fb = 0, fk = 0;
};

constexpr uword kStepMarker = 1;

void SaveRegs(int64_t* x, VerifyRegs* r) {
  memcpy(r->x, x, sizeof(r->x));
  memcpy(r->v, VReg(x, 0), sizeof(r->v));
}

void LoadRegs(int64_t* x, const VerifyRegs& r) {
  memcpy(x, r.x, sizeof(r.x));
  memcpy(VReg(x, 0), r.v, sizeof(r.v));
}

uword OpPc(const FastOp* ip) {
  const VerifyBlock* b = FindVerifyBlock(ip);
  if (b == nullptr) return 0;
  return b->lo + (ip - b->ops) * Instr::kInstrSize;
}

uword H_Verify(HARGS) {
  void*& state_slot = MixedInterp::State(sim);
  if (state_slot == nullptr) state_slot = new VerifyState();
  VerifyState* vs = reinterpret_cast<VerifyState*>(state_slot);
  if (vs->stepping) {
    // The op being verified continued here: stop and report.
    vs->stepping = false;
    vs->next = ip;
    vs->fa = fa;
    vs->fb = fb;
    vs->fk = fk;
    return kStepMarker;
  }
  const VerifyBlock* block = FindVerifyBlock(ip);
  RELEASE_ASSERT(block != nullptr);
  const intptr_t index = ip - block->ops;
  const FastHandler real = block->real[index];
  const int length = block->length[index];
  const uword pc = block->lo + index * Instr::kInstrSize;
  const bool replay = real != &H_Slow && real != &H_End;
  block->count[index]++;

  VerifyRegs before;
  SaveRegs(x, &before);
  const uint32_t nzcv_before = Nzcv(fa, fb, fk);
  // A memory pair that stores: the generic replay must see memory as it
  // was before the decoder's stores (e.g. ldur x1, [fp, #-8] ; stur x0,
  // [fp, #-8]). Store addresses are computable before execution.
  uword store_at[2] = {0, 0};
  int64_t store_before[2] = {0, 0};
  int stores = 0;
  if (real == &H_Mem2<true, false> || real == &H_Mem2<false, true> ||
      real == &H_Mem2<false, false>) {
    const uword a1 = static_cast<uword>(x[ip->rn]) + ip->imm;
    if (real != &H_Mem2<true, false>) store_at[stores++] = a1;
    if (real != &H_Mem2<false, true>) {
      const int64_t base = (real == &H_Mem2<true, false> && ip->ra == ip->rd)
                               ? Load<int64_t>(a1)
                               : x[ip->ra];
      store_at[stores++] = static_cast<uword>(base) + ip->aux;
    }
    for (int k = 0; k < stores; k++) store_before[k] = Load<int64_t>(store_at[k]);
  }

  vs->stepping = true;
  const uword result = real(ip, x, fa, fb, fk, sim);
  const bool exited = vs->stepping;  // Returned without stepping on.
  vs->stepping = false;
  verify_ops.fetch_add(1, std::memory_order_relaxed);
  verify_instructions.fetch_add(length, std::memory_order_relaxed);

  VerifyRegs after;
  SaveRegs(x, &after);
  const uint64_t nfa = vs->fa, nfb = vs->fb, nfk = vs->fk;
  const uword new_exit = reinterpret_cast<MixedRegs*>(x)->exit_target;
  const uword new_next_pc = exited ? 0 : OpPc(vs->next);

  if (replay) {
    // Re-execute with the generic decoder from the same state.
    int64_t store_after[2] = {0, 0};
    for (int k = stores - 1; k >= 0; k--) {
      store_after[k] = Load<int64_t>(store_at[k]);
    }
    for (int k = stores - 1; k >= 0; k--) {
      Store<int64_t>(store_at[k], store_before[k]);
    }
    LoadRegs(x, before);
    uint32_t nzcv = nzcv_before;
    uword old_next = pc;
    bool old_exited = false;
    for (int k = 0; k < length && !old_exited; k++) {
      const uword at = old_next;
      nzcv = MixedInterp::OldStep(sim, at, nzcv, &old_next, &old_exited);
      if (k + 1 < length && !old_exited && old_next != at + 4) break;
    }
    VerifyRegs old_regs;
    SaveRegs(x, &old_regs);
    bool same = true;
    char why[256] = "";
    for (intptr_t r = 0; r <= kZR && same; r++) {
      if (after.x[r] != old_regs.x[r]) {
        same = false;
        snprintf(why, sizeof(why), "x%" Pd ": decoder %#" Px64
                 " generic %#" Px64, r, after.x[r], old_regs.x[r]);
      }
    }
    for (intptr_t r = 0; r < kNumberOfVRegisters && same; r++) {
      if (memcmp(&after.v[r], &old_regs.v[r], sizeof(simd_value_t)) != 0) {
        same = false;
        snprintf(why, sizeof(why), "v%" Pd " differs", r);
      }
    }
    for (int k = 0; k < stores && same; k++) {
      if (Load<int64_t>(store_at[k]) != store_after[k]) {
        same = false;
        snprintf(why, sizeof(why), "memory at %#" Px " differs", store_at[k]);
      }
    }
    if (same && exited != old_exited) {
      same = false;
      snprintf(why, sizeof(why), "exit: decoder %d generic %d", exited,
               old_exited);
    }
    if (same && exited && new_exit != old_next) {
      same = false;
      snprintf(why, sizeof(why), "exit target: decoder %#" Px
               " generic %#" Px, new_exit, old_next);
    }
    if (same && !exited) {
      if (new_next_pc != old_next) {
        same = false;
        snprintf(why, sizeof(why), "next pc: decoder %#" Px " generic %#" Px,
                 new_next_pc, old_next);
      } else if (Nzcv(nfa, nfb, nfk) != nzcv) {
        same = false;
        snprintf(why, sizeof(why), "nzcv: decoder %x generic %x",
                 Nzcv(nfa, nfb, nfk), nzcv);
      }
    }
    if (!same) {
      const int64_t count = verify_mismatches.fetch_add(1) + 1;
      if (count <= 50) {
        OS::PrintErr("mixed: verify mismatch at patch+%#" Px " [%08x %08x]: %s\n",
                     pc - MixedMode::patch_start_,
                     *reinterpret_cast<const uint32_t*>(pc),
                     length > 1 ? *reinterpret_cast<const uint32_t*>(pc + 4)
                                : 0u,
                     why);
      }
    }
    // Continue from the decoder's state.
    LoadRegs(x, after);
    reinterpret_cast<MixedRegs*>(x)->exit_target = new_exit;
  }
  if (exited) return result;
  fa = nfa;
  fb = nfb;
  fk = nfk;
  DISPATCH(vs->next);
}

#undef DISPATCH
#undef HARGS

}  // namespace

uword Simulator::RunMixedFast(MixedRegs* regs) {
  EnsureTables();
  MixedInterp::Attach(this, regs);
  int64_t* x = regs->x;
  const uword gate = static_cast<uword>(x[16]);
  const FastOp* ip = GateOp(gate);
  if (ip == nullptr) [[unlikely]] {
    if (!MixedMode::IsGate(gate)) {
      FATAL("Mixed mode: entered from %#" Px, gate);
    }
    uword native = 0;
    ip = ResolveSlow(gate, &native);
    if (ip == nullptr) {
      FATAL("Mixed mode: gate %#" Px " bound to non-simulated pc %#" Px, gate,
            MixedMode::GateTarget(gate));
    }
  }
  if (Mode() == kModeVerify) [[unlikely]] {
    const uword g = (gate - g_gates_start) >> MixedMode::kGateSizeLog2;
    if (g < g_gate_count && DartMixedGateFlags[g] != 0) {
      // The gate did not save v8..v31: poison them, so that code reading
      // them (an unsound liveness result) computes garbage.
      for (intptr_t r = 8; r < kNumberOfVRegisters; r++) {
        VReg(x, r)->bits.i64[0] = 0x7ff4dead0000dead;
        VReg(x, r)->bits.i64[1] = 0x7ff4dead0000dead;
      }
    }
  }
  return ip->h(ip, x, 0, 0, kFLogic64, this);
}

// Entry point from the gate assembly (DartMixedSimEnter) with the saved
// registers: simulates from the gate's patch pc until the code branches to
// native code, and returns that native address (MixedRegs::exit_target), with
// bit 0 set when v8..v31 need not be reloaded (see EntryMayWriteHighV).
extern "C" uword DartMixedRun(MixedRegs* regs) {
  Simulator* sim = regs->simulator;
  if (sim == nullptr) [[unlikely]] {
    sim = new Simulator();
    regs->simulator = sim;
  }
  if (MixedMode::count_transitions()) {
    MixedMode::native_to_sim_.fetch_add(1, std::memory_order_relaxed);
  }
  if (Mode() == kModeSimulator) [[unlikely]] {
    return sim->RunMixed(regs);
  }
  const uword target = sim->RunMixedFast(regs);
  if (Mode() == kModeVerify && (target & 1) != 0) [[unlikely]] {
    // Instead of leaving v8..v31 as the interpreter left them, reload them
    // as poison, so that native code reading them computes garbage.
    for (intptr_t r = 8; r < kNumberOfVRegisters; r++) {
      VReg(regs->x, r)->bits.i64[0] = 0x7ff4beef0000beef;
      VReg(regs->x, r)->bits.i64[1] = 0x7ff4beef0000beef;
    }
    return target & ~static_cast<uword>(1);
  }
  return target;
}

void MixedInterpPrintStats() {
  if (Mode() == kModeSimulator) return;
  OS::PrintErr("mixed: interpreter=%s decoded-entries=%" Pd64
               " decoded-bytes=%" Pd64 "\n",
               Mode() == kModeVerify ? "verify" : "decoder",
               g_decoded_entries.load(), g_decoded_bytes.load());
  if (Mode() == kModeVerify) {
    if (const char* path = getenv("DART_MIXED_PROFILE_OUT")) {
      // Testing aid: per-instruction execution counts of decoded code, as
      // "patch-offset count word slow" lines.
      if (FILE* f = fopen(path, "w")) {
        MutexLocker ml(DecodeMutex());
        for (intptr_t i = 0; i < verify_block_count; i++) {
          const VerifyBlock& b = verify_blocks[i];
          for (intptr_t k = 0; k < b.n; k++) {
            if (b.count[k] == 0) continue;
            const uword pc = b.lo + k * Instr::kInstrSize;
            fprintf(f, "%" Px " %" Pu64 " %08x %d\n",
                    pc - MixedMode::patch_start_, b.count[k],
                    *reinterpret_cast<const uint32_t*>(pc),
                    b.real[k] == &H_Slow ? 1 : 0);
          }
        }
        fclose(f);
      }
    }
    OS::PrintErr("mixed: verify ops=%" Pd64 " instructions=%" Pd64
                 " mismatches=%" Pd64 "\n",
                 verify_ops.load(), verify_instructions.load(),
                 verify_mismatches.load());
  }
}

}  // namespace dart

#endif  // defined(TARGET_ARCH_ARM64) && defined(DART_INCLUDE_SIMULATOR) && ...
