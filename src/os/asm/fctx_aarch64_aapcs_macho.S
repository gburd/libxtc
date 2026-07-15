/*
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/os/asm/fctx_aarch64_aapcs_macho.S
 *	make_fcontext / jump_fcontext for AArch64 (ARM64) AAPCS64 on the
 *	Mach-O ABI (macOS / Apple Silicon).  Byte-for-byte the same
 *	register discipline as fctx_aarch64_aapcs.S (the ELF variant);
 *	only the assembler dialect differs:
 *	  - Mach-O prepends an underscore to C symbols, so the C names
 *	    __xtc_make_fcontext / __xtc_jump_fcontext are the assembler
 *	    symbols ___xtc_make_fcontext / ___xtc_jump_fcontext.
 *	  - no .type / .size / .note.GNU-stack (ELF-only directives).
 *	  - .p2align (log2) instead of ELF .align.
 *
 *	Callee-saved state preserved across a switch (AAPCS64): x19-x28,
 *	x29 (fp), x30 (lr), and d8-d15 (low 64 bits).  160-byte context
 *	block, 16-byte aligned.  See fctx_aarch64_aapcs.S for the full
 *	layout and rationale.
 *
 *	Selected only when a build forces the fcontext substrate on Apple
 *	arm64 (XTC_CORO_FORCE_FCTX); the default substrate on macOS remains
 *	ucontext until this path is promoted.  Empty TU elsewhere.
 */

#if defined(__aarch64__) && defined(__APPLE__)

	.text

/*
 * void *__xtc_make_fcontext(void *stack_top, size_t size,
 *                           void (*fn)(void *transfer));
 *   x0 = stack_top, x1 = size (advisory), x2 = fn
 *   returns: x0 = saved-sp; pass to __xtc_jump_fcontext as `to`.
 */
	.globl ___xtc_make_fcontext
	.p2align 2
___xtc_make_fcontext:
	/* Align the stack top down to 16, then reserve a 160-byte
	 * context block. */
	and     x0, x0, #~15
	sub     x0, x0, #160

	/* Zero the callee-saved integer slots (x19-x28, x29). */
	stp     xzr, xzr, [x0, #64]      /* x19, x20 */
	stp     xzr, xzr, [x0, #80]      /* x21, x22 */
	stp     xzr, xzr, [x0, #96]      /* x23, x24 */
	stp     xzr, xzr, [x0, #112]     /* x25, x26 */
	stp     xzr, xzr, [x0, #128]     /* x27, x28 */
	str     xzr, [x0, #144]          /* x29 (fp) */

	/* Zero the FP slots d8-d15. */
	stp     xzr, xzr, [x0, #0]       /* d8, d9   */
	stp     xzr, xzr, [x0, #16]      /* d10, d11 */
	stp     xzr, xzr, [x0, #32]      /* d12, d13 */
	stp     xzr, xzr, [x0, #48]      /* d14, d15 */

	/* Saved resume address = fn. */
	str     x2, [x0, #152]
	ret


/*
 * void *__xtc_jump_fcontext(void **from, void *to, void *transfer);
 *   x0 = &from, x1 = to, x2 = transfer
 *   returns: x0 = the `transfer` value passed by whoever later
 *            jumps back to us.
 */
	.globl ___xtc_jump_fcontext
	.p2align 2
___xtc_jump_fcontext:
	/* Reserve and fill our context block below the current sp. */
	sub     sp, sp, #160

	stp     d8,  d9,  [sp, #0]
	stp     d10, d11, [sp, #16]
	stp     d12, d13, [sp, #32]
	stp     d14, d15, [sp, #48]

	stp     x19, x20, [sp, #64]
	stp     x21, x22, [sp, #80]
	stp     x23, x24, [sp, #96]
	stp     x25, x26, [sp, #112]
	stp     x27, x28, [sp, #128]
	stp     x29, x30, [sp, #144]     /* fp, resume address (lr) */

	/* *from = current sp. */
	mov     x9, sp
	str     x9, [x0]

	/* sp = to. */
	mov     sp, x1

	/* Restore callee-saved state from the target context. */
	ldp     d8,  d9,  [sp, #0]
	ldp     d10, d11, [sp, #16]
	ldp     d12, d13, [sp, #32]
	ldp     d14, d15, [sp, #48]

	ldp     x19, x20, [sp, #64]
	ldp     x21, x22, [sp, #80]
	ldp     x23, x24, [sp, #96]
	ldp     x25, x26, [sp, #112]
	ldp     x27, x28, [sp, #128]
	ldp     x29, x30, [sp, #144]     /* fp, resume address */

	add     sp, sp, #160

	/* Hand the transfer arg to the resumed code. */
	mov     x0, x2

	ret

#endif  /* __aarch64__ && __APPLE__ */
