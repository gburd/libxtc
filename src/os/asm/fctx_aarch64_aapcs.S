/*
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/os/asm/fctx_aarch64_aapcs.S
 *	make_fcontext / jump_fcontext for AArch64 (ARM64) AAPCS64.
 *	Linux, *BSD, illumos ELF.  No Boost.Context dependency.
 *
 * Status: runtime verified natively on Linux aarch64 (glibc; make
 *	check with --with-coro=fctx) and earlier under qemu-aarch64
 *	user-mode (round-trip fiber harness, callee-saved int + d8-d15 FP
 *	preserved across switches).
 *
 *	AAPCS64 callee-saved state that must survive a switch:
 *	  x19-x28 (10 integer regs), x29 (frame pointer), x30 (link reg),
 *	  and the LOW 64 bits of v8-v15 (d8-d15).  Only the low 8 bytes
 *	  of v8-v15 are callee-saved per the ABI; the upper bits are
 *	  caller-saved, so saving d8-d15 (8 bytes each) is sufficient.
 *
 *	Stack alignment: AAPCS64 requires sp to be 16-byte aligned at
 *	any public interface (function entry).  We keep the saved
 *	context block a multiple of 16 bytes so a switched-to sp lands
 *	16-aligned.
 *
 *	Layout of a saved context (offsets from the saved sp, growing up):
 *	  +0    d8        +8    d9
 *	  +16   d10       +24   d11
 *	  +32   d12       +40   d13
 *	  +48   d14       +56   d15      (64 bytes of FP)
 *	  +64   x19       +72   x20
 *	  +80   x21       +88   x22
 *	  +96   x23       +104  x24
 *	  +112  x25       +120  x26
 *	  +128  x27       +136  x28
 *	  +144  x29 (fp)  +152  x30/pc  (resume address)
 *	  +160  FPCR      +168  FPSR
 *	Total: 176 bytes (multiple of 16).
 *
 *	FPCR and FPSR are per fiber, as glibc's swapcontext keeps them
 *	(it saves and restores both) and as the x86-64 variant keeps
 *	MXCSR: AAPCS64 makes FPCR's rounding mode, FZ, DN and trap
 *	enables global state that only support functions (fesetround)
 *	change, so they belong to the thread of control that set them.
 *	Before, a fiber that set FE_UPWARD or flush-to-zero and yielded
 *	handed it to every fiber the scheduler ran next, and got theirs
 *	back (test/m4/test_async /C10_fp_env_per_fiber).  FPSR's
 *	cumulative exception flags travel with the fiber for the same
 *	reason (fetestexcept after a yield reports the fiber's own
 *	operations), matching ucontext and MXCSR's flag bits on x86-64.
 *	A fresh context starts with its creator's FPCR / FPSR, as
 *	getcontext + makecontext does.  Each msr is skipped when the
 *	target's value equals the live one (the usual case): a raw round
 *	trip costs 22.6 ns that way against 24.9 ns writing both always
 *	(22.0 ns saving neither; 16-vCPU aarch64 host).
 *
 *	On a resumed jump_fcontext the saved x30 is the return site in
 *	the caller of jump_fcontext (a "ret" lands there).  For a fresh
 *	make_fcontext the saved x30 is the start trampoline and the x19
 *	slot holds `fn`: the first jump's "ret" lands in the trampoline,
 *	which calls fn with the transfer value already loaded into x0, so
 *	fn has a real caller frame that ends the unwind.
 */

#if defined(__aarch64__) && !defined(__APPLE__) && !defined(_WIN32)

	.text

/*
 * The outermost frame of every fcontext fiber, as glibc's __startcontext
 * is for makecontext.  Entered by the first jump's "ret" with x19 = fn,
 * x29 = 0, x0 = transfer; calls fn, which never returns (__coro_entry
 * jumps away).  CFI marks x30 undefined, so an unwinder stops here.
 * Before, the first jump entered fn with x30 = fn itself: the frame
 * above fn claimed a return address of fn+0, the DWARF unwinder looked
 * up the FDE of fn-1 -- the function laid out before fn -- and applied
 * its CFI to memory above the stack top (clang -O2: SIGSEGV in
 * _Unwind_Backtrace from xtc_dump; test_fctx /fctx_unwind_ends).
 */
	.type  __xtc_fctx_start,%function
	.align 4
__xtc_fctx_start:
	.cfi_startproc
	.cfi_undefined x30
	blr     x19
	brk     #0                       /* fn returned: unreachable */
	.cfi_endproc
	.size __xtc_fctx_start, .-__xtc_fctx_start

/*
 * void *__xtc_make_fcontext(void *stack_top, size_t size,
 *                           void (*fn)(void *transfer));
 *   x0 = stack_top, x1 = size (advisory), x2 = fn
 *   returns: x0 = saved-sp; pass to __xtc_jump_fcontext as `to`.
 */
	.globl __xtc_make_fcontext
	.type  __xtc_make_fcontext,%function
	.align 4
__xtc_make_fcontext:
	/* Align the stack top down to 16, then reserve a 176-byte
	 * context block. */
	and     x0, x0, #~15
	sub     x0, x0, #176

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

	/* The creator's FPCR and FPSR, as getcontext + makecontext (and
	 * a new thread) would start with. */
	mrs     x9,  fpcr
	mrs     x10, fpsr
	stp     x9,  x10, [x0, #160]

	/* The first jump "ret"s into the start trampoline, which
	 * calls fn (kept in the x19 slot) with x0 = transfer. */
	str     x2, [x0, #64]            /* x19 = fn */
	adr     x9, __xtc_fctx_start
	str     x9, [x0, #152]           /* x30 = start trampoline */
	ret
	.size __xtc_make_fcontext, .-__xtc_make_fcontext


/*
 * void *__xtc_jump_fcontext(void **from, void *to, void *transfer);
 *   x0 = &from, x1 = to, x2 = transfer
 *   returns: x0 = the `transfer` value passed by whoever later
 *            jumps back to us.
 */
	.globl __xtc_jump_fcontext
	.type  __xtc_jump_fcontext,%function
	.align 4
__xtc_jump_fcontext:
	/* Reserve and fill our context block below the current sp. */
	sub     sp, sp, #176

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
	mrs     x9,  fpcr
	mrs     x10, fpsr
	stp     x9,  x10, [sp, #160]

	/* *from = current sp. */
	mov     x11, sp
	str     x11, [x0]

	/* sp = to. */
	mov     sp, x1

	/* Restore the target's FPCR / FPSR, writing each only when it
	 * differs from the live value still in x9 / x10 (the usual case
	 * is equal, and an msr costs more than the compare). */
	ldp     x11, x12, [sp, #160]
	cmp     x11, x9
	b.eq    1f
	msr     fpcr, x11
1:	cmp     x12, x10
	b.eq    2f
	msr     fpsr, x12
2:
	/* Then the target's callee-saved registers. */
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

	add     sp, sp, #176

	/* Hand the transfer arg to the resumed code.  x0 serves both as
	 * arg0 for the first-jump-into-fn case and as the return value
	 * for the resumed-jump_fcontext case; only one matters per jump. */
	mov     x0, x2

	ret
	.size __xtc_jump_fcontext, .-__xtc_jump_fcontext

	.section .note.GNU-stack,"",%progbits

#endif  /* __aarch64__ && !_WIN32 */
