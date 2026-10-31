// Test-only ABI bridge. Invoke an indirect-argument/indirect-result callback
// using caller-provided stable native buffers. x8 is the AAPCS64 result address;
// x0 carries the by-value argument copy. Neither is a borrowed moving Dart ptr.
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
  .cfi_endproc
#if !defined(__APPLE__)
.size SYMBOL(CallbackIndirectAt), .-SYMBOL(CallbackIndirectAt)
.section .note.GNU-stack,"",%progbits
#endif
