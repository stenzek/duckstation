// SPDX-FileCopyrightText: 2021-2024 Connor McLaughlin <stenzek@gmail.com>
// SPDX-License-Identifier: (GPL-3.0 OR CC-BY-NC-ND-4.0)

// Win32 uses Fastjmp.asm, because MSVC doesn't support inline asm.
#if !defined(_WIN32) || defined(_M_ARM64)

#include "fastjmp.h"

#if defined(__APPLE__)
#define PREFIX "_"
#else
#define PREFIX ""
#endif

#if defined(__x86_64__)

asm("\t.global " PREFIX "fastjmp_set\n"
    "\t.global " PREFIX "fastjmp_jmp\n"
    "\t.text\n"
    "\t" PREFIX "fastjmp_set:"
    R"(
	movq 0(%rsp), %rax
	movq %rsp, %rdx			# fixup stack pointer, so it doesn't include the call to fastjmp_set
	addq $8, %rdx
	movq %rax, 0(%rdi)	# actually rip
	movq %rbx, 8(%rdi)
	movq %rdx, 16(%rdi)	# actually rsp
	movq %rbp, 24(%rdi)
	movq %r12, 32(%rdi)
	movq %r13, 40(%rdi)
	movq %r14, 48(%rdi)
	movq %r15, 56(%rdi)
	xorl %eax, %eax
	ret
)"
    "\t" PREFIX "fastjmp_jmp:"
    R"(
	movl %esi, %eax
	movq 0(%rdi), %rdx	# actually rip
	movq 8(%rdi), %rbx
	movq 16(%rdi), %rsp	# actually rsp
	movq 24(%rdi), %rbp
	movq 32(%rdi), %r12
	movq 40(%rdi), %r13
	movq 48(%rdi), %r14
	movq 56(%rdi), %r15
	jmp *%rdx
)");

#elif defined(__aarch64__)

asm(
	"\t.global " PREFIX "fastjmp_set\n"
	"\t.global " PREFIX "fastjmp_jmp\n"
	"\t.text\n"
	"\t.align 16\n"
	"\t" PREFIX "fastjmp_set:" R"(
	mov x16, sp
	stp x16, x30, [x0]
	stp x19, x20, [x0, #16]
	stp x21, x22, [x0, #32]
	stp x23, x24, [x0, #48]
	stp x25, x26, [x0, #64]
	stp x27, x28, [x0, #80]
	str x29, [x0, #96]
	stp d8, d9, [x0, #112]
	stp d10, d11, [x0, #128]
	stp d12, d13, [x0, #144]
	stp d14, d15, [x0, #160]
	mov w0, wzr
	br x30
)"
".align 16\n"
"\t" PREFIX "fastjmp_jmp:" R"(
	ldp x16, x30, [x0]
	mov sp, x16
	ldp x19, x20, [x0, #16]
	ldp x21, x22, [x0, #32]
	ldp x23, x24, [x0, #48]
	ldp x25, x26, [x0, #64]
	ldp x27, x28, [x0, #80]
	ldr x29, [x0, #96]
	ldp d8, d9, [x0, #112]
	ldp d10, d11, [x0, #128]
	ldp d12, d13, [x0, #144]
	ldp d14, d15, [x0, #160]
	mov w0, w1
	br x30
)");

#elif defined(__arm__)

asm(
	"\t.global " PREFIX "fastjmp_set\n"
	"\t.global " PREFIX "fastjmp_jmp\n"
	"\t.text\n"
	"\t" PREFIX "fastjmp_set:" R"(
        vstmia r0!, {d8-d15}
        stmia r0!, {r4-r14}
        fmrx r1, fpscr
        str r1, [r0]
        mov r0, #0
        bx lr
)"

"\t" PREFIX "fastjmp_jmp:" R"(
        vldmia r0!, {d8-d15}
        ldmia r0!, {r4-r14}
	ldr r0, [r0]
	fmxr fpscr, r0
        mov r0, r1
	bx lr
)");

#elif defined(__riscv) && __riscv_xlen == 64

asm(
	"\t.global " PREFIX "fastjmp_set\n"
	"\t.global " PREFIX "fastjmp_jmp\n"
	"\t.text\n"
	"\t.align 16\n"
	"\t" PREFIX "fastjmp_set:" R"(
  sd sp, 0(a0)
  sd s0, 8(a0)
  sd s1, 16(a0)
  sd s2, 24(a0)
  sd s3, 32(a0)
  sd s4, 40(a0)
  sd s5, 48(a0)
  sd s6, 56(a0)
  sd s7, 64(a0)
  sd s8, 72(a0)
  sd s9, 80(a0)
  sd s10, 88(a0)
  sd s11, 96(a0)
  fsd fs0, 104(a0)
  fsd fs1, 112(a0)
  fsd fs2, 120(a0)
  fsd fs3, 128(a0)
  fsd fs4, 136(a0)
  fsd fs5, 144(a0)
  fsd fs6, 152(a0)
  fsd fs7, 160(a0)
  fsd fs8, 168(a0)
  fsd fs9, 176(a0)
  fsd fs10, 184(a0)
  fsd fs11, 192(a0)
  sd ra, 208(a0)
  li a0, 0
  jr ra
)"
".align 16\n"
"\t" PREFIX "fastjmp_jmp:" R"(
  ld ra, 208(a0)
  fld fs11, 192(a0)
  fld fs10, 184(a0)
  fld fs9, 176(a0)
  fld fs8, 168(a0)
  fld fs7, 160(a0)
  fld fs6, 152(a0)
  fld fs5, 144(a0)
  fld fs4, 136(a0)
  fld fs3, 128(a0)
  fld fs2, 120(a0)
  fld fs1, 112(a0)
  fld fs0, 104(a0)
  ld s11, 96(a0)
  ld s10, 88(a0)
  ld s9, 80(a0)
  ld s8, 72(a0)
  ld s7, 64(a0)
  ld s6, 56(a0)
  ld s5, 48(a0)
  ld s4, 40(a0)
  ld s3, 32(a0)
  ld s2, 24(a0)
  ld s1, 16(a0)
  ld s0, 8(a0)
  ld sp, 0(a0)
  mv a0, a1
  jr ra
)");


#else

#error Unknown platform.

#endif

#endif // __WIN32
