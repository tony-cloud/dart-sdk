// Copyright (c) 2026, the Dart project authors. Please see the AUTHORS file
// for details. All rights reserved. Use of this source code is governed by a
// BSD-style license that can be found in the LICENSE file.

// Release-linked native macOS SysV callback capture. Call-outs use the existing
// FfiCallTrampoline stub already present in the release snapshot.
#if defined(__APPLE__) && defined(__x86_64__) && \
    defined(DART_PRECOMPILED_RUNTIME) && defined(DART_BYTECODE_INTERPRETER) && \
    defined(DART_SHOREBIRD_INTERPRETER) && defined(TARGET_ARCH_X64) && \
    defined(DART_TARGET_OS_MACOS)
.text
.p2align 4
.globl _InterpreterFfiCallbackEntry
.private_extern _InterpreterFfiCallbackEntry
_InterpreterFfiCallbackEntry:
  .cfi_startproc
  pushq %rbp
  .cfi_def_cfa_offset 16
  .cfi_offset %rbp, -16
  movq %rsp, %rbp
  .cfi_def_cfa_register %rbp
  subq $256, %rsp  // InterpreterFfiCallbackFrame, aligned to 16 bytes.

  // Architectural CPU indices (not argument ordinals); no managed pointers.
  movq %rcx, 8(%rsp)
  movq %rdx, 16(%rsp)
  movq %rsi, 48(%rsp)
  movq %rdi, 56(%rsp)
  movq %r8, 64(%rsp)
  movq %r9, 72(%rsp)
  movsd %xmm0, 128(%rsp)
  movsd %xmm1, 136(%rsp)
  movsd %xmm2, 144(%rsp)
  movsd %xmm3, 152(%rsp)
  movsd %xmm4, 160(%rsp)
  movsd %xmm5, 168(%rsp)
  movsd %xmm6, 176(%rsp)
  movsd %xmm7, 184(%rsp)
  // Shared callback stub contract: six-word frame, two epilogue cookies,
  // then CALL's return PC. Native stack arguments are entry RSP + 72.
  // Our standard prologue moves RBP down another word: RBP + 80.
  leaq 80(%rbp), %rax
  movq %rax, 192(%rsp)

  movq %r14, %rdi  // THR installed by the shared callback stub.
  movq %rsp, %rsi
  callq _DLRT_InterpreterFfiCallback
  movq 200(%rsp), %rax
  movq 208(%rsp), %rdx
  movsd 216(%rsp), %xmm0
  movsd 224(%rsp), %xmm1
  movq %rbp, %rsp
  popq %rbp
  .cfi_def_cfa %rsp, 8
  retq
  .cfi_endproc
#endif
