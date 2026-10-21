/*
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/os/asm/preempt_trampoline_x86_64.S
 *	Involuntary-preemption resume trampoline for x86-64 System V
 *	(Linux, *BSD, illumos).  Phase 2b-arch of the preemption facility.
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
 *	RFLAGS, and the whole enabled vector state (XSAVE: x87, SSE, the
 *	AVX upper halves, AVX-512 opmask and ZMM; FXSAVE where the OS has
 *	not enabled XSAVE), does a normal cooperative xtc_yield(), then
 *	restores everything and returns to the interrupted PC with the
 *	interrupted RSP EXACTLY restored.  Invisible to the interrupted
 *	code.
 *
 *	FXSAVE alone saves only x87 and XMM0-15's low 128 bits: a fiber
 *	preempted inside AVX code (glibc's memcpy on an AVX-512 host)
 *	resumed with another fiber's YMM upper halves, ZMM16-31 and k
 *	registers -- silently corrupted copies (test/m14/test_preempt_p2
 *	/vector_state; sqlxtc's log lost acknowledged commits that way).
 *	__xtc_coro_preempt (coro_uctx.c) probes the XSAVE mask and area
 *	size once (CPUID/XGETBV) into the two words below before it arms
 *	the first redirect; a zero mask selects the FXSAVE path.
 *
 *	Entry contract (from __xtc_coro_preempt in coro_uctx.c):
 *	  RSP = orig_sp - 136 (below the interrupted code's 128-byte red
 *	        zone, plus the 8-byte return slot); orig_pc is in a
 *	        per-thread slot, read back via __xtc_preempt_take_pc().  All
 *	        other registers, RFLAGS included, hold the interrupted
 *	        values.
 *
 *	The handler used to store orig_pc at orig_sp - 8 and the trampoline
 *	skipped the red zone with `subq $128, %rsp` -- but the store was
 *	itself INSIDE the red zone (an ABI-conforming leaf's -8(%rsp) value
 *	was overwritten: ~100 of ~70k probes per run), and the subq ran
 *	before pushfq, so the RFLAGS restored at the end were the subq's,
 *	not the interrupted code's (a preemption between a cmp and its jcc
 *	took the wrong branch).  Now nothing touches the red zone and the
 *	first instruction is pushfq.
 */

#if defined(__x86_64__) && !defined(__APPLE__) && !defined(_WIN32)

	.text

/*
 * void __xtc_preempt_trampoline(void);
 *   Entered via sigreturn, not a normal call.  The terminal `ret`
 *   resumes the interrupted fiber at orig_pc.
 */
/* XSAVE feature mask (0 = use FXSAVE) and area size in bytes, written
 * by __xtc_coro_preempt before the first redirect. */
	.data
	.balign 8
	.globl __xtc_preempt_xsave_mask
	.hidden __xtc_preempt_xsave_mask
	.type  __xtc_preempt_xsave_mask,@object
	.size  __xtc_preempt_xsave_mask,8
__xtc_preempt_xsave_mask:
	.quad 0
	.globl __xtc_preempt_xsave_size
	.hidden __xtc_preempt_xsave_size
	.type  __xtc_preempt_xsave_size,@object
	.size  __xtc_preempt_xsave_size,8
__xtc_preempt_xsave_size:
	.quad 0
	.text
	.globl __xtc_preempt_trampoline
	.type  __xtc_preempt_trampoline,@function
	.align 16
__xtc_preempt_trampoline:
	/*
	 * Entry RSP == orig_sp - 136: [orig_sp - 136, orig_sp - 128) is the
	 * return slot, filled below; [orig_sp - 128, orig_sp) is the
	 * interrupted red zone, never touched.  Save RFLAGS first (nothing
	 * before it may change a flag), then every GP register, then fetch
	 * orig_pc into the return slot (a call clobbers caller-saved
	 * registers, all saved by then).
	 */
	pushfq
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
	movq    %rsp, %rbp                /* rbp = post-GP-save RSP (temp) */
	andq    $-16, %rsp               /* SysV call alignment */
	call    __xtc_preempt_take_pc
	movq    %rax, 128(%rbp)           /* the return slot: 16 saves up */

	/*
	 * The vector save area needs a 64-aligned (XSAVE) or 16-aligned
	 * (FXSAVE) destination and RSP has arbitrary alignment (orig_sp
	 * was arbitrary): rbp holds the post-GP-save RSP (its own value is
	 * saved on the stack and xtc_yield preserves it); reserve, align
	 * down, save; restore RSP from rbp.  Every GP register is saved, so
	 * rax/rcx/rdx are free scratch.
	 */
	movq    %rbp, %rsp
	movq    __xtc_preempt_xsave_mask(%rip), %rax
	testq   %rax, %rax
	jz      .Lfxsave

	subq    __xtc_preempt_xsave_size(%rip), %rsp
	andq    $-64, %rsp               /* XSAVE area: 64-aligned */
	/* XRSTOR faults on a nonzero reserved header byte, and XSAVE
	 * writes only XSTATE_BV: zero the 64-byte header first. */
	xorl    %ecx, %ecx
	movq    %rcx, 512(%rsp)
	movq    %rcx, 520(%rsp)
	movq    %rcx, 528(%rsp)
	movq    %rcx, 536(%rsp)
	movq    %rcx, 544(%rsp)
	movq    %rcx, 552(%rsp)
	movq    %rcx, 560(%rsp)
	movq    %rcx, 568(%rsp)
	movq    %rax, %rdx
	shrq    $32, %rdx                 /* edx:eax = the feature mask */
	xsave   (%rsp)

	call    xtc_yield                 /* cooperative switch to scheduler */

	/*
	 * Fiber resumed.  Restore the vector state and the GP file and
	 * return to orig_pc.  The whole trampoline body -- including this
	 * epilogue, where g_in_preempt is already back to 0 -- is protected
	 * against a nested involuntary redirect by an instruction-pointer
	 * range check in __xtc_coro_preempt (it declines when the
	 * interrupted PC lies within
	 * [__xtc_preempt_trampoline, __xtc_preempt_trampoline_end)), so no
	 * g_in_preempt store is needed here and there is no window in which
	 * a tick could redirect a half-restored register file.
	 */
	movq    __xtc_preempt_xsave_mask(%rip), %rax
	movq    %rax, %rdx
	shrq    $32, %rdx
	xrstor  (%rsp)
	jmp     .Lrestore_gp

.Lfxsave:
	subq    $512, %rsp                /* fxsave area */
	andq    $-16, %rsp               /* 16-align it */
	fxsave  (%rsp)
	call    xtc_yield                 /* cooperative switch to scheduler */
	fxrstor (%rsp)

.Lrestore_gp:
	movq    %rbp, %rsp                /* undo the reserve + align exactly */

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
	popfq
	/* RSP == orig_sp - 136, top of stack == orig_pc: pop it and drop the
	 * 128-byte gap in one flag-neutral instruction. */
	ret     $128                      /* -> orig_pc, RSP == orig_sp */
	.size __xtc_preempt_trampoline, .-__xtc_preempt_trampoline

/* End marker so __xtc_coro_preempt can range-check the interrupted PC
 * and decline to redirect a fiber that is executing inside the
 * trampoline itself (its prologue, xtc_yield call, or epilogue). */
	.globl __xtc_preempt_trampoline_end
__xtc_preempt_trampoline_end:

	.section .note.GNU-stack,"",@progbits

#endif /* __x86_64__ && !__APPLE__ && !_WIN32 */
