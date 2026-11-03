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

// Offset of Thread::mixed_area_. Checked at startup by
// MixedMode::CheckThreadOffset.
#ifndef DART_MIXED_AREA_OFFSET
#define DART_MIXED_AREA_OFFSET 0
#endif

#define GATE_COUNT 131072

.macro LOAD_AREA reg
  add \reg, x26, #(DART_MIXED_AREA_OFFSET >> 12), lsl #12
  ldr \reg, [\reg, #(DART_MIXED_AREA_OFFSET & 0xfff)]
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
  add x16, x17, #256
  stp q0, q1, [x16, #0]
  stp q2, q3, [x16, #32]
  stp q4, q5, [x16, #64]
  stp q6, q7, [x16, #96]
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
  // Switch to the side stack and run the simulator.
  ldr x16, [x17, #768]
  mov sp, x16
  stp x29, x30, [sp, #-16]!
  mov x29, sp
  mov x0, x17
  bl SYM(DartMixedRun)
  // x26 (THR) is callee-saved in the C ABI.
  LOAD_AREA x17
  add x16, x17, #256
  ldp q0, q1, [x16, #0]
  ldp q2, q3, [x16, #32]
  ldp q4, q5, [x16, #64]
  ldp q6, q7, [x16, #96]
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
  ldr x16, [x17, #776]
  br x16

#endif  // defined(__aarch64__) && defined(DART_MIXED_MODE)
