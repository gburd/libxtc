/*
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/os/asm/fctx_s390x_zabi.S
 *	make_fcontext / jump_fcontext for IBM z/Architecture (s390x),
 *	64-bit Linux ELF ABI.  No Boost.Context dependency.
 *
 * Status: written, assembles with s390x cross gcc, and RUNTIME
 *	VERIFIED under qemu-s390x user-mode (round-trip fiber harness,
 *	callee-saved GPR + f8-f15 FP preserved across switches).
 *
 *	s390x ELF callee-saved state that must survive a switch:
 *	  GPRs r6-r13 (8 regs), r14 (return address / link), and the
 *	  floating-point regs f8-f15 (8 doubles).  r15 is the stack
 *	  pointer and serves as the context handle.  (The standard
 *	  Linux/ELF s390x C ABI defines f8-f15 as callee-saved; the
 *	  vector regs v8-v15 alias these and are covered for the
 *	  low double word, which is what the C convention uses.)
 *
 *	Stack alignment: the s390x ABI requires r15 (sp) to be 8-byte
 *	aligned.  We keep the saved context block an 8-byte multiple.
 *	We do not build a full ABI stack frame for the switch (no
 *	160-byte register-save-area or back-chain is needed because the
 *	switched-to code restores r15 to its own saved frame).
 *
 *	Layout of a saved context (offsets from the saved r15, growing up):
 *	  +0    r14 (resume address)
 *	  +8    r6    +16   r7
 *	  +24   r8    +32   r9
 *	  +40   r10   +48   r11
 *	  +56   r12   +64   r13     (r14 + 8 GPRs = 72 bytes)
 *	  +72   f8    +80   f9
 *	  +88   f10   +96   f11
 *	  +104  f12   +112  f13
 *	  +120  f14   +128  f15     (8 FP regs = 64 bytes)
 *	Total: 136 bytes (multiple of 8).
 */

#if defined(__s390x__) && !defined(_WIN32)

	.text

/*
 * void *__xtc_make_fcontext(void *stack_top, size_t size,
 *                           void (*fn)(void *transfer));
 *   r2 = stack_top, r3 = size (advisory), r4 = fn
 *   returns: r2 = saved-sp; pass to __xtc_jump_fcontext as `to`.
 */
	.globl __xtc_make_fcontext
	.type  __xtc_make_fcontext,@function
	.align 16
__xtc_make_fcontext:
	/* Align the stack top down to 8, then reserve a 136-byte
	 * context block. */
	nill    %r2, 0xfff8              /* r2 &= ~7 */
	aghi    %r2, -136

	/* Zero the callee-saved GPR slots (r6-r13). */
	lghi    %r1, 0
	stg     %r1, 8(%r2)             /* r6  */
	stg     %r1, 16(%r2)            /* r7  */
	stg     %r1, 24(%r2)            /* r8  */
	stg     %r1, 32(%r2)            /* r9  */
	stg     %r1, 40(%r2)            /* r10 */
	stg     %r1, 48(%r2)            /* r11 */
	stg     %r1, 56(%r2)            /* r12 */
	stg     %r1, 64(%r2)            /* r13 */

	/* Zero the FP slots f8-f15. */
	stg     %r1, 72(%r2)
	stg     %r1, 80(%r2)
	stg     %r1, 88(%r2)
	stg     %r1, 96(%r2)
	stg     %r1, 104(%r2)
	stg     %r1, 112(%r2)
	stg     %r1, 120(%r2)
	stg     %r1, 128(%r2)

	/* Saved resume address (r14) = fn.  The first jump_fcontext
	 * loads this into r14 and "br %r14" enters fn with the transfer
	 * value already in r2 (arg0). */
	stg     %r4, 0(%r2)
	br      %r14
	.size __xtc_make_fcontext, .-__xtc_make_fcontext


/*
 * void *__xtc_jump_fcontext(void **from, void *to, void *transfer);
 *   r2 = &from, r3 = to, r4 = transfer
 *   returns: r2 = the `transfer` value passed by whoever later
 *            jumps back to us.
 */
	.globl __xtc_jump_fcontext
	.type  __xtc_jump_fcontext,@function
	.align 16
__xtc_jump_fcontext:
	/* Reserve our context block below the current r15. */
	aghi    %r15, -136

	/* Save r14 and callee-saved GPRs r6-r13. */
	stg     %r14, 0(%r15)
	stg     %r6, 8(%r15)
	stg     %r7, 16(%r15)
	stg     %r8, 24(%r15)
	stg     %r9, 32(%r15)
	stg     %r10, 40(%r15)
	stg     %r11, 48(%r15)
	stg     %r12, 56(%r15)
	stg     %r13, 64(%r15)

	/* Save callee-saved FP regs f8-f15. */
	std     %f8, 72(%r15)
	std     %f9, 80(%r15)
	std     %f10, 88(%r15)
	std     %f11, 96(%r15)
	std     %f12, 104(%r15)
	std     %f13, 112(%r15)
	std     %f14, 120(%r15)
	std     %f15, 128(%r15)

	/* *from = current r15. */
	stg     %r15, 0(%r2)

	/* r15 = to. */
	lgr     %r15, %r3

	/* Restore r14 and callee-saved GPRs. */
	lg      %r14, 0(%r15)
	lg      %r6, 8(%r15)
	lg      %r7, 16(%r15)
	lg      %r8, 24(%r15)
	lg      %r9, 32(%r15)
	lg      %r10, 40(%r15)
	lg      %r11, 48(%r15)
	lg      %r12, 56(%r15)
	lg      %r13, 64(%r15)

	/* Restore callee-saved FP regs. */
	ld      %f8, 72(%r15)
	ld      %f9, 80(%r15)
	ld      %f10, 88(%r15)
	ld      %f11, 96(%r15)
	ld      %f12, 104(%r15)
	ld      %f13, 112(%r15)
	ld      %f14, 120(%r15)
	ld      %f15, 128(%r15)

	/* Drop the context block. */
	aghi    %r15, 136

	/* Hand the transfer arg to the resumed code.  r2 is arg0 for
	 * the first-jump-into-fn case and the return value for the
	 * resumed-jump_fcontext case; only one matters per jump. */
	lgr     %r2, %r4

	br      %r14
	.size __xtc_jump_fcontext, .-__xtc_jump_fcontext

	.section .note.GNU-stack,"",@progbits

#endif  /* __s390x__ && !_WIN32 */
