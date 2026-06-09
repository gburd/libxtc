/*
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/os/asm/fctx_sparc64_sysv.S
 *	make_fcontext / jump_fcontext for SPARC64 (UltraSPARC, SPARC V9)
 *	SysV ELF ABI.  No Boost.Context dependency.
 *
 * Status: written, assembles with a sparc64 cross assembler.  NOT
 *	yet runtime-verified: nixpkgs has no sparc64 cross gcc and the
 *	standalone harness could not be linked for this round.  The
 *	register-window flush + FP save logic follows the SPARC V9 ABI
 *	and the well-known Boost.Context sparc64 pattern, but it has not
 *	been run.  Treat as compile-only until a qemu-sparc64 run with
 *	a working sparc64 toolchain confirms it, in the spirit of
 *	src/io/io_aix.c's documented untested status.
 *
 *	SPARC V9 callee-saved state:
 *	  The integer registers are managed by register windows: the
 *	  "local" (l0-l7) and "in" (i0-i7) registers of every active
 *	  window are saved to the stack by the "flushw" instruction, so
 *	  a switch only needs to flush windows, swap the stack pointer
 *	  (%sp / %o6), and the new stack's windows restore on demand.
 *	  The return address lives in %i7 of the current window.  The
 *	  floating-point registers %f0-%f62 -- here we preserve the
 *	  even double-precision set %f0,%f2,...,%f30 used by the C ABI
 *	  for caller/callee state across the switch.
 *
 *	Stack bias: SPARC V9 uses a stack BIAS of 2047 -- the real
 *	memory address of the register save area is %sp + 2047.  All
 *	stack-relative loads/stores below the bias use the biased
 *	offset; the saved context handle we return is the RAW %sp
 *	(unbiased), matching what makecontext-style users expect.
 *
 *	Layout of a saved context (offsets are biased, i.e. from
 *	%sp + 2047):
 *	  +0 .. +127   register-window save area (16 xwords, ABI
 *	               mandated minimum frame; flushw / window restore
 *	               use this).
 *	  +128         saved %i7 (resume address) for a fresh context.
 *	  +136 .. +263 saved FP doubles %f2..%f30 (callee state).
 *	The make_fcontext block is sized to a 16-byte multiple.
 */

#if defined(__sparc__) && defined(__arch64__) && !defined(_WIN32)

#define STACK_BIAS 2047
#define CTX_SIZE   304          /* multiple of 16, room for frame + fp */

	.text
	.register %g2, #scratch
	.register %g3, #scratch

/*
 * void *__xtc_make_fcontext(void *stack_top, size_t size,
 *                           void (*fn)(void *transfer));
 *   %o0 = stack_top, %o1 = size (advisory), %o2 = fn
 *   returns: %o0 = saved-sp (raw, unbiased); pass as `to`.
 */
	.globl __xtc_make_fcontext
	.type  __xtc_make_fcontext,#function
	.align 4
__xtc_make_fcontext:
	/* Align stack top down to 16, then reserve the context block.
	 * The returned handle is the raw (unbiased) %sp. */
	and     %o0, -16, %o0
	sub     %o0, CTX_SIZE, %o0

	/* Store fn as the resume address.  The first jump_fcontext will
	 * place it into %i7 of the restored window; "ret" (jmpl %i7+8)
	 * enters fn.  We store it at the biased +128 slot. */
	stx     %o2, [%o0 + STACK_BIAS + 128]

	/* Zero the saved FP slots %f2..%f30 (8 bytes each, +136..+263). */
	stx     %g0, [%o0 + STACK_BIAS + 136]
	stx     %g0, [%o0 + STACK_BIAS + 144]
	stx     %g0, [%o0 + STACK_BIAS + 152]
	stx     %g0, [%o0 + STACK_BIAS + 160]
	stx     %g0, [%o0 + STACK_BIAS + 168]
	stx     %g0, [%o0 + STACK_BIAS + 176]
	stx     %g0, [%o0 + STACK_BIAS + 184]
	stx     %g0, [%o0 + STACK_BIAS + 192]
	stx     %g0, [%o0 + STACK_BIAS + 200]
	stx     %g0, [%o0 + STACK_BIAS + 208]
	stx     %g0, [%o0 + STACK_BIAS + 216]
	stx     %g0, [%o0 + STACK_BIAS + 224]
	stx     %g0, [%o0 + STACK_BIAS + 232]
	stx     %g0, [%o0 + STACK_BIAS + 240]
	stx     %g0, [%o0 + STACK_BIAS + 248]

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
	/* Flush all register windows to their stack save areas so the
	 * outgoing context's locals/ins are safely in memory. */
	flushw

	/* Save the resume address (current %o7 return address + 8 so a
	 * plain "jmpl %g1, %g0" lands at the post-call site) and the
	 * callee FP doubles into the OUTGOING context's save block.
	 * The outgoing context handle is our own current %sp; we carve
	 * the block just below it. */
	save    %sp, -CTX_SIZE, %sp

	/* %i7 now holds the caller's return address; store it. */
	stx     %i7, [%sp + STACK_BIAS + 128]

	/* Save callee FP doubles %f2..%f30. */
	std     %f2,  [%sp + STACK_BIAS + 136]
	std     %f4,  [%sp + STACK_BIAS + 144]
	std     %f6,  [%sp + STACK_BIAS + 152]
	std     %f8,  [%sp + STACK_BIAS + 160]
	std     %f10, [%sp + STACK_BIAS + 168]
	std     %f12, [%sp + STACK_BIAS + 176]
	std     %f14, [%sp + STACK_BIAS + 184]
	std     %f16, [%sp + STACK_BIAS + 192]
	std     %f18, [%sp + STACK_BIAS + 200]
	std     %f20, [%sp + STACK_BIAS + 208]
	std     %f22, [%sp + STACK_BIAS + 216]
	std     %f24, [%sp + STACK_BIAS + 224]
	std     %f26, [%sp + STACK_BIAS + 232]
	std     %f28, [%sp + STACK_BIAS + 240]
	std     %f30, [%sp + STACK_BIAS + 248]

	/* *from = current %sp (raw, unbiased).  &from arrived in the
	 * caller's %o0, which after "save" is our %i0. */
	stx     %sp, [%i0]

	/* %sp = to (raw, unbiased).  `to` arrived in %i1. */
	mov     %i1, %sp

	/* Restore the resume address and FP doubles from the target. */
	ldx     [%sp + STACK_BIAS + 128], %i7

	ldd     [%sp + STACK_BIAS + 136], %f2
	ldd     [%sp + STACK_BIAS + 144], %f4
	ldd     [%sp + STACK_BIAS + 152], %f6
	ldd     [%sp + STACK_BIAS + 160], %f8
	ldd     [%sp + STACK_BIAS + 168], %f10
	ldd     [%sp + STACK_BIAS + 176], %f12
	ldd     [%sp + STACK_BIAS + 184], %f14
	ldd     [%sp + STACK_BIAS + 192], %f16
	ldd     [%sp + STACK_BIAS + 200], %f18
	ldd     [%sp + STACK_BIAS + 208], %f20
	ldd     [%sp + STACK_BIAS + 216], %f22
	ldd     [%sp + STACK_BIAS + 224], %f24
	ldd     [%sp + STACK_BIAS + 232], %f26
	ldd     [%sp + STACK_BIAS + 240], %f28
	ldd     [%sp + STACK_BIAS + 248], %f30

	/* Deliver the transfer value (%i2) as the return value (%o0 of
	 * the resumed code).  ret performs "jmpl %i7 + 8, %g0" and
	 * restore copies %i0 -> the caller's %o0; place transfer there. */
	mov     %i2, %i0

	ret
	 restore
	.size __xtc_jump_fcontext, .-__xtc_jump_fcontext

	.section .note.GNU-stack,"",@progbits

#endif  /* __sparc__ && __arch64__ && !_WIN32 */
