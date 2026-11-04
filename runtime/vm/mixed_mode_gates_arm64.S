// Copyright (c) 2026, the Dart project authors.  Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

// Gates for mixed-mode execution (see vm/mixed_mode.h).
//
// Every gate is two instructions: it records its own address in x16 (IP0,
// which the Dart and C ABIs leave dead at call and return boundaries) and
// jumps to DartMixedSimEnter. DartMixedSimEnter saves all CPU registers in
// the current Thread's MixedRegs area, switches to the side stack, and calls
// DartMixedRun(area), which simulates from the gate's patch pc until the
// simulated code branches to native code. The registers are then reloaded
// from the area (now holding the simulated state) and the CPU branches to
// the native target.

#if defined(__aarch64__) && defined(DART_MIXED_MODE)

#if defined(__APPLE__)
#define SYM(x) _##x
#else
#define SYM(x) x
#endif


#define GATE_COUNT 131072

// reg = THR->mixed_area_, through the offset in DartMixedAreaOffset (set by
// MixedMode::RegisterImages before any gate can be bound).
.macro LOAD_AREA reg
#if defined(__APPLE__)
  adrp \reg, SYM(DartMixedAreaOffset)@PAGE
  ldr \reg, [\reg, SYM(DartMixedAreaOffset)@PAGEOFF]
#else
  adrp \reg, DartMixedAreaOffset
  ldr \reg, [\reg, :lo12:DartMixedAreaOffset]
#endif
  ldr \reg, [x26, \reg]
.endm

// reg = &sym
.macro ADDR reg, sym
#if defined(__APPLE__)
  adrp \reg, SYM(\sym)@PAGE
  add \reg, \reg, SYM(\sym)@PAGEOFF
#else
  adrp \reg, \sym
  add \reg, \reg, :lo12:\sym
#endif
.endm

// reg = sym (a pointer-sized global)
.macro LOAD_GLOBAL reg, sym
#if defined(__APPLE__)
  adrp \reg, SYM(\sym)@PAGE
  ldr \reg, [\reg, SYM(\sym)@PAGEOFF]
#else
  adrp \reg, \sym
  ldr \reg, [\reg, :lo12:\sym]
#endif
.endm

  .text
  .p2align 4
  .globl SYM(DartMixedGatesStart)
SYM(DartMixedGatesStart):
  .rept GATE_COUNT
  adr x16, .
  b SYM(DartMixedSimEnter)
  .endr
  .globl SYM(DartMixedGatesEnd)
SYM(DartMixedGatesEnd):

  .p2align 4
  .globl SYM(DartMixedSimEnter)
SYM(DartMixedSimEnter):
  LOAD_AREA x17
  stp x0, x1, [x17, #0]
  stp x2, x3, [x17, #16]
  stp x4, x5, [x17, #32]
  stp x6, x7, [x17, #48]
  stp x8, x9, [x17, #64]
  stp x10, x11, [x17, #80]
  stp x12, x13, [x17, #96]
  stp x14, x15, [x17, #112]
  str x16, [x17, #128]
  stp x18, x19, [x17, #144]
  stp x20, x21, [x17, #160]
  stp x22, x23, [x17, #176]
  stp x24, x25, [x17, #192]
  stp x26, x27, [x17, #208]
  stp x28, x29, [x17, #224]
  str x30, [x17, #240]
  mov x16, sp
  str x16, [x17, #248]
  add x16, x17, #272
  stp q0, q1, [x16, #0]
  stp q2, q3, [x16, #32]
  stp q4, q5, [x16, #64]
  stp q6, q7, [x16, #96]
  // v8..v31 are saved unless the gate's flag says they are dead from its
  // target (DartMixedGateFlags, set by the interpreter's liveness analysis;
  // 0 = save). x0..x15 are free here: they are saved and reloaded at exit.
  ldr x0, [x17, #128]
  ADDR x1, DartMixedGatesStart
  sub x0, x0, x1
  lsr x0, x0, #3
  LOAD_GLOBAL x1, DartMixedGateFlags
  ldrb w0, [x1, x0]
  cbnz w0, 1f
  stp q8, q9, [x16, #128]
  stp q10, q11, [x16, #160]
  stp q12, q13, [x16, #192]
  stp q14, q15, [x16, #224]
  stp q16, q17, [x16, #256]
  stp q18, q19, [x16, #288]
  stp q20, q21, [x16, #320]
  stp q22, q23, [x16, #352]
  stp q24, q25, [x16, #384]
  stp q26, q27, [x16, #416]
  stp q28, q29, [x16, #448]
  stp q30, q31, [x16, #480]
1:
  // Switch to the side stack and run the simulator.
  ldr x16, [x17, #784]
  mov sp, x16
  stp x29, x30, [sp, #-16]!
  mov x29, sp
  mov x0, x17
  bl SYM(DartMixedRun)
  // x26 (THR) is callee-saved in the C ABI.
  LOAD_AREA x17
  add x16, x17, #272
  ldp q0, q1, [x16, #0]
  ldp q2, q3, [x16, #32]
  ldp q4, q5, [x16, #64]
  ldp q6, q7, [x16, #96]
  // Bit 0 of DartMixedRun's result: the exiting code cannot have written
  // v8..v31 (Simulator EntryMayWriteHighV), so they are not reloaded.
  tbnz x0, #0, 2f
  ldp q8, q9, [x16, #128]
  ldp q10, q11, [x16, #160]
  ldp q12, q13, [x16, #192]
  ldp q14, q15, [x16, #224]
  ldp q16, q17, [x16, #256]
  ldp q18, q19, [x16, #288]
  ldp q20, q21, [x16, #320]
  ldp q22, q23, [x16, #352]
  ldp q24, q25, [x16, #384]
  ldp q26, q27, [x16, #416]
  ldp q28, q29, [x16, #448]
  ldp q30, q31, [x16, #480]
2:
  ldp x0, x1, [x17, #0]
  ldp x2, x3, [x17, #16]
  ldp x4, x5, [x17, #32]
  ldp x6, x7, [x17, #48]
  ldp x8, x9, [x17, #64]
  ldp x10, x11, [x17, #80]
  ldp x12, x13, [x17, #96]
  ldp x14, x15, [x17, #112]
  ldp x18, x19, [x17, #144]
  ldp x20, x21, [x17, #160]
  ldp x22, x23, [x17, #176]
  ldp x24, x25, [x17, #192]
  ldp x26, x27, [x17, #208]
  ldp x28, x29, [x17, #224]
  ldr x30, [x17, #240]
  ldr x16, [x17, #248]
  mov sp, x16
  // The area is not on the stack, so it is safe to read after moving sp.
  ldr x16, [x17, #792]
  br x16

#endif  // defined(__aarch64__) && defined(DART_MIXED_MODE)
