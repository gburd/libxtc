/*
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/os/asm/fctx_sparc64_sysv.S
 *	make_fcontext / jump_fcontext for SPARC64 (UltraSPARC, SPARC V9)
 *	SysV ELF ABI.  No Boost.Context dependency.
 *
 * Status: written AND runtime-verified on a real UltraSPARC sun4u
 *	host (OpenIndiana/illumos, gcc 13.4.0 and clang 21.1.8, 64-bit),
 *	round-trip fiber harness: the transfer arg threads through every
 *	jump, a coroutine yields and is resumed several times, and its
 *	saved state survives each switch.  The first hand-written draft
 *	(flush-then-save with a 16-byte-aligned raw %sp and a "ret jumps
 *	to fn+8" bug) bus-errored on the very first store in
 *	make_fcontext; this revision follows the proven SPARC V9 idiom.
 *
 *	SPARC V9 register-window model.  Integer state lives in
 *	overlapping register windows, not in fixed callee-saved
 *	registers.  A "save" rotates to a fresh window (old %out become
 *	new %in, old %sp becomes new %fp); "restore" rotates back, so
 *	the restored window's %sp comes from the %fp (%i6) we load.  The
 *	"flushw" instruction spills every active window's 16 in+local
 *	registers to that window's on-stack save area at
 *	[%sp + BIAS .. %sp + BIAS + 127].  A context switch therefore
 *	does NOT copy the integer file by hand: it flushes the windows,
 *	swaps %sp, and lets window-underflow reload the target's
 *	in/local on demand.  Only the resumed window's %sp (threaded via
 *	%fp) and its resume address %i7 need to be set explicitly.
 *
 *	Order matters: store %fp and %i7 into the new window's save
 *	slots FIRST, then "flushw", so the spilled window image left on
 *	the outgoing stack is self-consistent before we abandon it.
 *
 *	Stack BIAS.  SPARC V9 keeps a biased stack pointer: the real
 *	address of a frame's save area is %sp + 2047.  The hardware
 *	requires the BIASED address to be 16-byte aligned, i.e. %sp
 *	itself congruent to -2047 (== 1) modulo 16.  A naively
 *	16-aligned %sp makes every [%sp + BIAS + k] access misaligned
 *	and bus-errors -- the defect in the first draft.  make_fcontext
 *	below computes a biased-aligned %sp.  The opaque handle we pass
 *	around IS that real (loadable) %sp; jump_fcontext "mov"s it
 *	straight into %sp and addresses slots as [%sp + BIAS + k].
 *
 *	Floating point.  The SPARC V9 SysV ABI has NO callee-saved
 *	floating-point registers -- all of %f0..%f31 are caller-saved,
 *	so a C caller already spills live FP values to its own frame
 *	across a call and preserving the whole stack preserves them.
 *	We nonetheless save/restore the even doubles %f2..%f30 across
 *	the switch for defence in depth; it is harmless.
 *
 *	Context-save frame layout (offsets are BIASED, from %sp + 2047):
 *	  +0  .. +127   register-window save area (16 xwords, the ABI
 *	                mandated minimum; flushw / window underflow use
 *	                it -- must not be clobbered).
 *	  +128          saved %fp  (= the %sp the resumed window runs on)
 *	  +136          saved %i7  (resume address; "ret" lands at +8)
 *	  +144 .. +271  saved FP doubles %f2..%f30 (16 xwords).
 *	CC64FSZ is the reserved frame size, a 16-byte multiple.
 */

#if defined(__sparc__) && defined(__arch64__) && !defined(_WIN32)

#define STACK_BIAS 2047
#define CC64FSZ    288         /* frame size, multiple of 16          */
#define O_FP       128         /* biased offset of saved %fp / run-sp */
#define O_I7       136         /* biased offset of saved %i7          */
#define O_FP0      144         /* biased offset of first saved double */

	.text
	.register %g2, #scratch
	.register %g3, #scratch

/*
 * void *__xtc_make_fcontext(void *stack_top, size_t size,
 *                           void (*fn)(void *transfer));
 *   %o0 = stack_top, %o1 = size (advisory), %o2 = fn
 *   returns: %o0 = saved-sp handle; pass as `to` to jump_fcontext.
 *
 * We carve two frames out of the top of the stack: an upper frame for
 * fn to execute in, and below it the context-save frame (the handle).
 * The first jump restores into a window whose %i7 is fn-8 (so "ret"
 * == "jmpl %i7+8" enters fn) and whose %sp, after the jump's trailing
 * "restore", is the upper frame -- a valid stack for fn to run on.
 */
	.globl __xtc_make_fcontext
	.type  __xtc_make_fcontext,#function
	.align 4
__xtc_make_fcontext:
	/* Align the stack top down to 16 bytes, then bias-align it so
	 * (sp + BIAS) % 16 == 0 as the hardware requires. */
	and     %o0, -16, %o0
	add     %o0, STACK_BIAS, %o0
	and     %o0, -16, %o0
	sub     %o0, STACK_BIAS, %o0

	/* Upper frame for fn (%g2), then the context-save frame (%o0,
	 * the handle we return).  Both stay bias-aligned. */
	sub     %o0, CC64FSZ, %g2        /* fn's run-sp                */
	sub     %g2, CC64FSZ, %o0        /* context-save sp (handle)   */

	/* Resume window's run-sp = upper frame; restore makes %sp = %fp. */
	stx     %g2, [%o0 + STACK_BIAS + O_FP]

	/* Resume address slot = fn - 8 (ret jumps to %i7 + 8 -> fn). */
	sub     %o2, 8, %g1
	stx     %g1, [%o0 + STACK_BIAS + O_I7]

	/* Zero the saved FP doubles %f2..%f30 (15 slots, 8 bytes each). */
	stx     %g0, [%o0 + STACK_BIAS + O_FP0 + 0]
	stx     %g0, [%o0 + STACK_BIAS + O_FP0 + 8]
	stx     %g0, [%o0 + STACK_BIAS + O_FP0 + 16]
	stx     %g0, [%o0 + STACK_BIAS + O_FP0 + 24]
	stx     %g0, [%o0 + STACK_BIAS + O_FP0 + 32]
	stx     %g0, [%o0 + STACK_BIAS + O_FP0 + 40]
	stx     %g0, [%o0 + STACK_BIAS + O_FP0 + 48]
	stx     %g0, [%o0 + STACK_BIAS + O_FP0 + 56]
	stx     %g0, [%o0 + STACK_BIAS + O_FP0 + 64]
	stx     %g0, [%o0 + STACK_BIAS + O_FP0 + 72]
	stx     %g0, [%o0 + STACK_BIAS + O_FP0 + 80]
	stx     %g0, [%o0 + STACK_BIAS + O_FP0 + 88]
	stx     %g0, [%o0 + STACK_BIAS + O_FP0 + 96]
	stx     %g0, [%o0 + STACK_BIAS + O_FP0 + 104]
	stx     %g0, [%o0 + STACK_BIAS + O_FP0 + 112]

	retl
	 nop
	.size __xtc_make_fcontext, .-__xtc_make_fcontext


/*
 * void *__xtc_jump_fcontext(void **from, void *to, void *transfer);
 *   %o0 = &from, %o1 = to, %o2 = transfer
 *   returns: %o0 = the `transfer` value passed by whoever jumps back.
 */
	.globl __xtc_jump_fcontext
	.type  __xtc_jump_fcontext,#function
	.align 4
__xtc_jump_fcontext:
	/* Rotate to a fresh window and reserve our save area below %sp. */
	save    %sp, -CC64FSZ, %sp

	/* Save the outgoing resume state into the new window's slots:
	 *   %fp -> the caller's %sp (the window-below's stack pointer,
	 *          which is what the resumed code must run on),
	 *   %i7 -> jump_fcontext's own return address (ret lands at +8).
	 * Save the callee FP doubles too. */
	stx     %fp,  [%sp + STACK_BIAS + O_FP]
	stx     %i7,  [%sp + STACK_BIAS + O_I7]

	std     %f2,  [%sp + STACK_BIAS + O_FP0 + 0]
	std     %f4,  [%sp + STACK_BIAS + O_FP0 + 8]
	std     %f6,  [%sp + STACK_BIAS + O_FP0 + 16]
	std     %f8,  [%sp + STACK_BIAS + O_FP0 + 24]
	std     %f10, [%sp + STACK_BIAS + O_FP0 + 32]
	std     %f12, [%sp + STACK_BIAS + O_FP0 + 40]
	std     %f14, [%sp + STACK_BIAS + O_FP0 + 48]
	std     %f16, [%sp + STACK_BIAS + O_FP0 + 56]
	std     %f18, [%sp + STACK_BIAS + O_FP0 + 64]
	std     %f20, [%sp + STACK_BIAS + O_FP0 + 72]
	std     %f22, [%sp + STACK_BIAS + O_FP0 + 80]
	std     %f24, [%sp + STACK_BIAS + O_FP0 + 88]
	std     %f26, [%sp + STACK_BIAS + O_FP0 + 96]
	std     %f28, [%sp + STACK_BIAS + O_FP0 + 104]
	std     %f30, [%sp + STACK_BIAS + O_FP0 + 112]

	/* Remember the outgoing handle (this window's raw %sp). */
	mov     %sp, %g1

	/* &from arrived in the caller's %o0 -> our %i0 after save;
	 * `to` in %i1, transfer in %i2. */

	/* Flush every active register window to its stack save area so
	 * the outgoing context is fully materialised.  Must come AFTER
	 * the %fp/%i7 stores above. */
	flushw

	/* *from = outgoing handle. */
	stx     %g1, [%i0]

	/* Switch to the target stack. */
	mov     %i1, %sp

	/* Restore the target's resume state. */
	ldx     [%sp + STACK_BIAS + O_FP], %fp
	ldx     [%sp + STACK_BIAS + O_I7], %i7

	ldd     [%sp + STACK_BIAS + O_FP0 + 0],   %f2
	ldd     [%sp + STACK_BIAS + O_FP0 + 8],   %f4
	ldd     [%sp + STACK_BIAS + O_FP0 + 16],  %f6
	ldd     [%sp + STACK_BIAS + O_FP0 + 24],  %f8
	ldd     [%sp + STACK_BIAS + O_FP0 + 32],  %f10
	ldd     [%sp + STACK_BIAS + O_FP0 + 40],  %f12
	ldd     [%sp + STACK_BIAS + O_FP0 + 48],  %f14
	ldd     [%sp + STACK_BIAS + O_FP0 + 56],  %f16
	ldd     [%sp + STACK_BIAS + O_FP0 + 64],  %f18
	ldd     [%sp + STACK_BIAS + O_FP0 + 72],  %f20
	ldd     [%sp + STACK_BIAS + O_FP0 + 80],  %f22
	ldd     [%sp + STACK_BIAS + O_FP0 + 88],  %f24
	ldd     [%sp + STACK_BIAS + O_FP0 + 96],  %f26
	ldd     [%sp + STACK_BIAS + O_FP0 + 104], %f28
	ldd     [%sp + STACK_BIAS + O_FP0 + 112], %f30

	/* Deliver transfer (%i2) as the resumed code's return value.
	 * "restore" rotates the window back, sets the new %sp from the
	 * %fp we loaded, and copies its first source operand into the
	 * destination %o0 -- so feed transfer through it. */
	ret
	 restore %i2, %g0, %o0
	.size __xtc_jump_fcontext, .-__xtc_jump_fcontext

	.section .note.GNU-stack,"",@progbits

#endif  /* __sparc__ && __arch64__ && !_WIN32 */
