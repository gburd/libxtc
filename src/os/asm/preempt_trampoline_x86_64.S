/*
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/os/asm/preempt_trampoline_x86_64.S
 *	Involuntary-preemption resume trampoline for x86-64 System V
 *	(Linux, *BSD, illumos).  Phase 2b-arch of docs/M_PREEMPTION.md.
 *
 *	Go's async-preemption mechanism.  When the per-thread CPU timer
 *	fires inside a running fiber at a SAFE point (crit_depth == 0,
 *	unsafe_depth == 0), the signal handler rewrites the interrupted
 *	context so that, on sigreturn, the fiber resumes -- on its own
 *	stack, with its own real registers -- at this trampoline, rather
 *	than switching stacks inside the async-signal handler (unsound).
 *
 *	The trampoline runs at an ARBITRARY instruction boundary, so
 *	every register is potentially live: it saves the full GP file,
 *	RFLAGS, and a 512-byte FXSAVE area, does a normal cooperative
 *	xtc_yield(), then restores everything and returns to the
 *	interrupted PC with the interrupted RSP EXACTLY restored.
 *	Invisible to the interrupted code.
 *
 *	Entry contract (from __xtc_coro_preempt in coro_uctx.c):
 *	  RSP = orig_sp - 8, and [orig_sp - 8] = orig_pc, so a terminal
 *	        `ret` from this exact RSP lands at orig_pc with RSP ==
 *	        orig_sp.  All other registers hold interrupted values.
 *
 *	Because the trampoline runs on the interrupted fiber's own stack
 *	just below orig_sp, it first drops RSP past the 128-byte red zone
 *	(which the interrupted leaf function may still be using) BEFORE
 *	any push, so its saves never clobber live red-zone data.  No
 *	scratch register is needed: the terminal `ret` alone restores PC
 *	and RSP, so all 15 GP registers + RFLAGS are restored cleanly.
 */

#if defined(__x86_64__) && !defined(__APPLE__) && !defined(_WIN32)

	.text

/*
 * void __xtc_preempt_trampoline(void);
 *   Entered via sigreturn, not a normal call.  The terminal `ret`
 *   resumes the interrupted fiber at orig_pc.
 */
	.globl __xtc_preempt_trampoline
	.type  __xtc_preempt_trampoline,@function
	.align 16
__xtc_preempt_trampoline:
	/*
	 * Entry RSP == orig_sp - 8 (holds orig_pc).  Skip the interrupted
	 * leaf's 128-byte red zone before saving anything.  orig_sp-8 has
	 * unknown alignment; after "sub 128 ; and ~15" the save area is
	 * 16-aligned, but we must not lose the orig_pc slot -- so we do
	 * NOT realign RSP itself (that would strand the ret slot).
	 * Instead: leave RSP at orig_sp-8, subtract a red-zone gap that
	 * makes the fxsave area 16-aligned, and account for the odd
	 * orig_pc slot with the push count.
	 *
	 * We reserve 128 (red zone) + up to 8 to reach a 16-aligned point
	 * for the fxsave area.  Simpler and robust: save GP regs first
	 * (their alignment does not matter), then align only for fxsave.
	 */
	subq    $128, %rsp                /* clear the red zone */

	pushq   %rax
	pushq   %rbx
	pushq   %rcx
	pushq   %rdx
	pushq   %rsi
	pushq   %rdi
	pushq   %rbp
	pushq   %r8
	pushq   %r9
	pushq   %r10
	pushq   %r11
	pushq   %r12
	pushq   %r13
	pushq   %r14
	pushq   %r15
	pushfq

	/*
	 * fxsave needs a 16-aligned destination.  RSP here has arbitrary
	 * alignment (orig_sp-8 was arbitrary).  Save the current RSP in a
	 * callee-saved register (rbp is already preserved on the stack),
	 * align down, fxsave, and restore RSP afterward from the saved
	 * copy.  Use r15's slot?  No -- use a local: push the pre-align
	 * RSP, align, reserve 512.
	 */
	movq    %rsp, %rbp                /* rbp = post-GP-save RSP (temp) */
	subq    $512, %rsp                /* fxsave area */
	andq    $-16, %rsp               /* 16-align it */
	fxsave  (%rsp)

	call    xtc_yield                 /* cooperative switch to scheduler */

	/*
	 * Fiber resumed.  Restore FP/SSE and the GP file and return to
	 * orig_pc.  The whole trampoline body -- including this epilogue,
	 * where g_in_preempt is already back to 0 -- is protected against a
	 * nested involuntary redirect by an instruction-pointer range
	 * check in __xtc_coro_preempt (it declines when the interrupted PC
	 * lies within [__xtc_preempt_trampoline, __xtc_preempt_trampoline_end)),
	 * so no g_in_preempt store is needed here and there is no window in
	 * which a tick could redirect a half-restored register file.
	 */
	fxrstor (%rsp)
	movq    %rbp, %rsp                /* undo align+512 exactly */

	popfq
	popq    %r15
	popq    %r14
	popq    %r13
	popq    %r12
	popq    %r11
	popq    %r10
	popq    %r9
	popq    %r8
	popq    %rbp
	popq    %rdi
	popq    %rsi
	popq    %rdx
	popq    %rcx
	popq    %rbx
	popq    %rax
	addq    $128, %rsp                /* undo the red-zone gap */
	/* RSP == orig_sp - 8, top of stack == orig_pc. */
	ret                               /* -> orig_pc, RSP == orig_sp */
	.size __xtc_preempt_trampoline, .-__xtc_preempt_trampoline

/* End marker so __xtc_coro_preempt can range-check the interrupted PC
 * and decline to redirect a fiber that is executing inside the
 * trampoline itself (its prologue, xtc_yield call, or epilogue). */
	.globl __xtc_preempt_trampoline_end
__xtc_preempt_trampoline_end:

	.section .note.GNU-stack,"",@progbits

#endif /* __x86_64__ && !__APPLE__ && !_WIN32 */
