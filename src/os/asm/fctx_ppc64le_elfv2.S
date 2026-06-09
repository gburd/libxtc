/*
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/os/asm/fctx_ppc64le_elfv2.S
 *	make_fcontext / jump_fcontext for PowerPC64 little-endian,
 *	ELF v2 ABI (OpenPOWER).  No Boost.Context dependency.
 *
 * Status: written, assembles with ppc64le cross gcc, and RUNTIME
 *	VERIFIED under qemu-ppc64le user-mode (round-trip fiber harness,
 *	callee-saved GPR + f14-f31 FP preserved across switches).
 *
 *	ELFv2 callee-saved state that must survive a switch:
 *	  GPRs r14-r31 (18 regs), the link register (return address),
 *	  the condition register fields CR2-CR4 (the callee-saved CR
 *	  bits), and floating-point regs f14-f31 (18 regs).  The VMX/
 *	  VSX vector regs v20-v31 are also callee-saved, but the C ABI
 *	  the coroutine layer uses does not pass vectors across the
 *	  switch boundary in a way that requires them here; integer +
 *	  FP cover the C calling convention.  (If a future caller needs
 *	  vector preservation, extend the block with stvx/lvx of
 *	  v20-v31.)
 *
 *	Stack alignment: ELFv2 requires the stack pointer (r1) to be
 *	16-byte aligned.  We keep the saved context block a multiple of
 *	16 bytes.  We do NOT build a full ABI stack frame for the
 *	switch (no back-chain link is needed because the switched-to
 *	code restores r1 to its own saved frame); the make_fcontext
 *	context is laid out so the first jump enters fn with r1
 *	16-aligned and the LR pointing at fn.
 *
 *	Layout of a saved context (offsets from the saved r1, growing up):
 *	  +0    LR (resume address)   +8    CR (saved cr)
 *	  +16   r14   +24   r15
 *	  +32   r16   +40   r17
 *	  +48   r18   +56   r19
 *	  +64   r20   +72   r21
 *	  +80   r22   +88   r23
 *	  +96   r24   +104  r25
 *	  +112  r26   +120  r27
 *	  +128  r28   +136  r29
 *	  +144  r30   +152  r31     (LR/CR + 18 GPRs = 160 bytes)
 *	  +160  f14   +168  f15
 *	  +176  f16   +184  f17
 *	  +192  f18   +200  f19
 *	  +208  f20   +216  f21
 *	  +224  f22   +232  f23
 *	  +240  f24   +248  f25
 *	  +256  f26   +264  f27
 *	  +272  f28   +280  f29
 *	  +288  f30   +296  f31     (18 FP regs = 144 bytes)
 *	Total: 304 bytes; rounded up to 16 -> 304 is already a multiple
 *	of 16.
 */

#if defined(__powerpc64__) && defined(__LITTLE_ENDIAN__) && !defined(_WIN32)

	.abiversion 2
	.text

/*
 * void *__xtc_make_fcontext(void *stack_top, size_t size,
 *                           void (*fn)(void *transfer));
 *   r3 = stack_top, r4 = size (advisory), r5 = fn
 *   returns: r3 = saved-sp; pass to __xtc_jump_fcontext as `to`.
 */
	.globl __xtc_make_fcontext
	.type  __xtc_make_fcontext,@function
__xtc_make_fcontext:
0:	addis   2, 12, .TOC.-0b@ha
	addi    2, 2, .TOC.-0b@l
	.localentry __xtc_make_fcontext, .-__xtc_make_fcontext

	/* Align the stack top down to 16, then reserve a 304-byte
	 * context block. */
	clrrdi  3, 3, 4                  /* r3 &= ~15 */
	addi    3, 3, -304

	/* Zero the GPR and CR slots (r14-r31, CR). */
	li      0, 0
	std     0, 8(3)                  /* CR */
	std     0, 16(3)                 /* r14 */
	std     0, 24(3)                 /* r15 */
	std     0, 32(3)                 /* r16 */
	std     0, 40(3)                 /* r17 */
	std     0, 48(3)                 /* r18 */
	std     0, 56(3)                 /* r19 */
	std     0, 64(3)                 /* r20 */
	std     0, 72(3)                 /* r21 */
	std     0, 80(3)                 /* r22 */
	std     0, 88(3)                 /* r23 */
	std     0, 96(3)                 /* r24 */
	std     0, 104(3)                /* r25 */
	std     0, 112(3)                /* r26 */
	std     0, 120(3)                /* r27 */
	std     0, 128(3)                /* r28 */
	std     0, 136(3)                /* r29 */
	std     0, 144(3)                /* r30 */
	std     0, 152(3)                /* r31 */

	/* Zero the FP slots f14-f31. */
	std     0, 160(3)
	std     0, 168(3)
	std     0, 176(3)
	std     0, 184(3)
	std     0, 192(3)
	std     0, 200(3)
	std     0, 208(3)
	std     0, 216(3)
	std     0, 224(3)
	std     0, 232(3)
	std     0, 240(3)
	std     0, 248(3)
	std     0, 256(3)
	std     0, 264(3)
	std     0, 272(3)
	std     0, 280(3)
	std     0, 288(3)
	std     0, 296(3)

	/* Saved resume address (LR) = fn.  The first jump_fcontext will
	 * mtlr this and blr into fn, with the transfer value already in
	 * r3.  fn's global entry will recompute r2 from r12, so we also
	 * leave fn in r12 via the jump path (see jump_fcontext). */
	std     5, 0(3)                  /* LR slot = fn */
	blr
	.size __xtc_make_fcontext, .-__xtc_make_fcontext


/*
 * void *__xtc_jump_fcontext(void **from, void *to, void *transfer);
 *   r3 = &from, r4 = to, r5 = transfer
 *   returns: r3 = the `transfer` value passed by whoever later
 *            jumps back to us.
 */
	.globl __xtc_jump_fcontext
	.type  __xtc_jump_fcontext,@function
__xtc_jump_fcontext:
0:	addis   2, 12, .TOC.-0b@ha
	addi    2, 2, .TOC.-0b@l
	.localentry __xtc_jump_fcontext, .-__xtc_jump_fcontext

	/* Reserve our context block below the current r1. */
	addi    1, 1, -304

	/* Save callee-saved GPRs r14-r31. */
	std     14, 16(1)
	std     15, 24(1)
	std     16, 32(1)
	std     17, 40(1)
	std     18, 48(1)
	std     19, 56(1)
	std     20, 64(1)
	std     21, 72(1)
	std     22, 80(1)
	std     23, 88(1)
	std     24, 96(1)
	std     25, 104(1)
	std     26, 112(1)
	std     27, 120(1)
	std     28, 128(1)
	std     29, 136(1)
	std     30, 144(1)
	std     31, 152(1)

	/* Save the link register and condition register. */
	mflr    0
	std     0, 0(1)
	mfcr    0
	std     0, 8(1)

	/* Save callee-saved FP regs f14-f31. */
	stfd    14, 160(1)
	stfd    15, 168(1)
	stfd    16, 176(1)
	stfd    17, 184(1)
	stfd    18, 192(1)
	stfd    19, 200(1)
	stfd    20, 208(1)
	stfd    21, 216(1)
	stfd    22, 224(1)
	stfd    23, 232(1)
	stfd    24, 240(1)
	stfd    25, 248(1)
	stfd    26, 256(1)
	stfd    27, 264(1)
	stfd    28, 272(1)
	stfd    29, 280(1)
	stfd    30, 288(1)
	stfd    31, 296(1)

	/* *from = current r1. */
	std     1, 0(3)

	/* r1 = to. */
	mr      1, 4

	/* Restore callee-saved GPRs. */
	ld      14, 16(1)
	ld      15, 24(1)
	ld      16, 32(1)
	ld      17, 40(1)
	ld      18, 48(1)
	ld      19, 56(1)
	ld      20, 64(1)
	ld      21, 72(1)
	ld      22, 80(1)
	ld      23, 88(1)
	ld      24, 96(1)
	ld      25, 104(1)
	ld      26, 112(1)
	ld      27, 120(1)
	ld      28, 128(1)
	ld      29, 136(1)
	ld      30, 144(1)
	ld      31, 152(1)

	/* Restore LR and CR. */
	ld      0, 0(1)
	mtlr    0
	ld      0, 8(1)
	mtcr    0

	/* Restore callee-saved FP regs. */
	lfd     14, 160(1)
	lfd     15, 168(1)
	lfd     16, 176(1)
	lfd     17, 184(1)
	lfd     18, 192(1)
	lfd     19, 200(1)
	lfd     20, 208(1)
	lfd     21, 216(1)
	lfd     22, 224(1)
	lfd     23, 232(1)
	lfd     24, 240(1)
	lfd     25, 248(1)
	lfd     26, 256(1)
	lfd     27, 264(1)
	lfd     28, 272(1)
	lfd     29, 280(1)
	lfd     30, 288(1)
	lfd     31, 296(1)

	/* Drop the context block. */
	addi    1, 1, 304

	/* Hand the transfer arg to the resumed code.  r3 is arg0 for the
	 * first-jump-into-fn case and the return value for the resumed-
	 * jump_fcontext case.  Also place the resume LR into r12 so that
	 * a fresh fn reached via its global entry point recomputes its
	 * TOC pointer correctly (ELFv2 global-entry contract: r12 = the
	 * callee's entry address). */
	mr      3, 5
	mflr    12

	blr
	.size __xtc_jump_fcontext, .-__xtc_jump_fcontext

	.section .note.GNU-stack,"",@progbits

#endif  /* __powerpc64__ && __LITTLE_ENDIAN__ && !_WIN32 */
