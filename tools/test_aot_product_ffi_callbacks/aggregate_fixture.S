// Test-only size-aware ABI bridge for guarded indirect results. ARM64
// receives an argument pointer in x0 and sret in x8; SysV receives an exact
// by-value stack copy and sret in RDI. No storage is a borrowed moving Dart ptr.
#if defined(__APPLE__)
#define SYMBOL(name) _##name
#else
#define SYMBOL(name) name
#endif
.text
.p2align 2
.globl SYMBOL(CallbackIndirectAt)
#if !defined(__APPLE__)
.type SYMBOL(CallbackIndirectAt), %function
#endif
SYMBOL(CallbackIndirectAt):
  .cfi_startproc
#if defined(__x86_64__)
  pushq %rbp
  .cfi_def_cfa_offset 16
  .cfi_offset %rbp, -16
  movq %rsp, %rbp
  .cfi_def_cfa_register %rbp
  pushq %rbx
  pushq %r12
  pushq %r13
  pushq %r14
  movq %rdi, %rbx  // callback
  movq %rdx, %r12  // sret storage
  movq %rcx, %r14  // exact argument size, not its rounded ABI container
  // SysV large/unaligned by-value aggregates live on the argument stack.
  // Copy exactly size bytes so a source ending at a guard page stays valid.
  leaq 15(%rcx), %rax
  andq $-16, %rax
  subq %rax, %rsp
  movq %rsp, %rdi
  rep movsb
  movq %r12, %rdi
  callq *%rbx
  // Returning the actual RAX lets C check sret pointer identity on success
  // and exceptional fallback, including when input and output alias.
  leaq -32(%rbp), %rsp
  popq %r14
  popq %r13
  popq %r12
  popq %rbx
  popq %rbp
  .cfi_def_cfa %rsp, 8
  retq
#else
  stp x29, x30, [sp, #-16]!
  .cfi_def_cfa_offset 16
  .cfi_offset x29, -16
  .cfi_offset x30, -8
  mov x29, sp
  mov x16, x0
  mov x0, x1
  mov x8, x2
  blr x16
  ldp x29, x30, [sp], #16
  .cfi_def_cfa_offset 0
  ret
#endif
  .cfi_endproc
#if !defined(__APPLE__)
.size SYMBOL(CallbackIndirectAt), .-SYMBOL(CallbackIndirectAt)
.section .note.GNU-stack,"",%progbits
#endif
