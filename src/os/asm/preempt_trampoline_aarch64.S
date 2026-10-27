/*
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/os/asm/preempt_trampoline_aarch64.S
 *	Involuntary-preemption resume trampoline for AArch64 Linux, the
 *	counterpart of preempt_trampoline_x86_64.S.  Substrate-agnostic:
 *	the ucontext and fcontext substrates both use it, through the
 *	redirect in src/inc/coro_preempt.h.
 *
 *	The kernel already saved the interrupted thread state EXACTLY when
 *	it delivered the preemption signal: its signal frame (struct
 *	rt_sigframe) holds x0-x30, sp, pc, PSTATE (NZCV, BTYPE), the
 *	FP/SIMD file with FPSR/FPCR, and -- on an SVE core -- the full
 *	Z, P and FFR registers.  The frame is built on the fiber's own
 *	stack (the preemption handler has no SA_ONSTACK).  So the redirect
 *	does not copy registers at all: it leaves that frame in place,
 *	stashes the interrupted pc/sp/pstate in the frame's siginfo (which
 *	rt_sigreturn never reads), and aims the frame at this trampoline
 *	with sp = the frame.  The handler's own sigreturn lands here with
 *	every register still holding its interrupted value and the frame
 *	just above sp, where the fiber's later calls cannot reach it.  We
 *	do a normal cooperative xtc_yield, aim the frame back at the
 *	interrupted pc/sp/pstate, and issue rt_sigreturn on the same
 *	frame: the kernel restores the whole state, so the preempted code
 *	cannot tell.  The frame stays on the parked fiber's stack (about
 *	4.6 KiB with 256-bit SVE; the kernel sizes it to the vector length).
 *
 *	The earlier trampoline saved the registers itself, and that cannot
 *	be done exactly in user mode: AArch64 has no instruction that
 *	restores every general register AND branches to an arbitrary
 *	address, so it gave up x16 (gcc and glibc's string functions do
 *	keep live data there), and it saved only the low 128 bits of each
 *	vector (glibc's memcpy/memset are SVE code on an SVE core).
 *	test/m14/test_preempt_p2 /a64_x16_preserved and /a64_sve_state
 *	caught both on every run.  rt_sigreturn also enters the target with
 *	the interrupted PSTATE.BTYPE, where a `br` would have needed a BTI
 *	landing pad at an arbitrary instruction.
 *
 *	Clobbering registers here is fine: the closing rt_sigreturn
 *	restores all of them from the frame.  The redirect declines while
 *	the interrupted pc is inside [__xtc_preempt_trampoline,
 *	__xtc_preempt_trampoline_end), and before the yield g_in_preempt
 *	(raised by the redirect) declines it too.  Frame offsets (siginfo
 *	128 bytes, then the ucontext: uc_stack +16, uc_sigmask +40,
 *	uc_mcontext sp/pc/pstate +432/+440/+448) are the kernel ABI,
 *	asserted against <ucontext.h> in coro_preempt.h.
 */

#if defined(__aarch64__) && defined(__linux__)

	.text
	.globl __xtc_preempt_trampoline
	.type  __xtc_preempt_trampoline,%function
	.align 4
__xtc_preempt_trampoline:
	/* Entered by the handler's sigreturn: every register holds its
	 * interrupted value, sp = the kernel's signal frame, the frame's
	 * siginfo holds the interrupted pc, sp, pstate (coro_preempt.h). */
	mov     x19, sp                   /* the frame (sigreturn restores x19) */
	/*
	 * Hand the scheduler a call-boundary state; the closing rt_sigreturn
	 * restores the interrupted one from the frame.
	 *  - SME (only where HWCAP2_SME, probed by the redirect): leave
	 *    streaming mode and turn ZA off, and clear TPIDR2_EL0.  The
	 *    interrupted code may be in streaming mode or hold ZA live, and
	 *    xtc_yield is a normal function: in streaming mode the scheduler
	 *    and its fibers ran with the streaming vector length and, without
	 *    FEAT_SME_FA64, faulted on their first Advanced SIMD instruction;
	 *    with ZA live they ran in a state the ABI forbids at a call to a
	 *    private-ZA function.  The frame holds SVCR, ZA and TPIDR2.
	 *  - errno is per thread: keep the interrupted value in callee-saved
	 *    x20 across the yield (the next fiber on this thread may set it;
	 *    a migrated fiber resumes on another thread).
	 */
	adrp    x9, __xtc_preempt_a64_sme
	ldr     w9, [x9, :lo12:__xtc_preempt_a64_sme]
	cbz     w9, 1f
	.inst   0xd503467f                /* smstop (SM and ZA) */
	mrs     x21, S3_3_C13_C0_5        /* TPIDR2_EL0 (kernels before 6.3 */
	msr     S3_3_C13_C0_5, xzr        /*   do not restore it: by hand) */
1:	bl      __xtc_preempt_errno_get
	mov     w20, w0
	bl      xtc_yield                 /* cooperative switch to scheduler */
	mov     w0, w20
	bl      __xtc_preempt_errno_set
	adrp    x9, __xtc_preempt_a64_sme
	ldr     w9, [x9, :lo12:__xtc_preempt_a64_sme]
	cbz     w9, 2f
	msr     S3_3_C13_C0_5, x21
2:

	/*
	 * Resumed, possibly on another OS thread.  rt_sigreturn installs
	 * the frame's signal mask and alternate stack as the CURRENT
	 * thread's, so overwrite them with this thread's own (raw
	 * syscalls: everything up to the sigreturn stays inside this
	 * function's PC range, where the redirect declines).  Then aim the
	 * frame back at the interrupted pc / sp / pstate.
	 */
	mov     x0, #0                    /* how: ignored (set == NULL) */
	mov     x1, #0                    /* set = NULL: query only */
	add     x2, x19, #(128 + 40)      /* &uc.uc_sigmask */
	mov     x3, #8                    /* sizeof the kernel sigset */
	mov     x8, #135                  /* __NR_rt_sigprocmask */
	svc     #0
	mov     x0, #0                    /* ss = NULL: query only */
	add     x1, x19, #(128 + 16)      /* &uc.uc_stack */
	mov     x8, #132                  /* __NR_sigaltstack */
	svc     #0
	ldp     x9, x10, [x19]            /* stashed pc, sp */
	ldr     x11, [x19, #16]           /* stashed pstate */
	add     x12, x19, #(128 + 432)    /* &uc.uc_mcontext.sp */
	stp     x10, x9, [x12]            /* sp, pc */
	str     x11, [x12, #16]           /* pstate */

	mov     sp, x19
	mov     x8, #139                  /* __NR_rt_sigreturn */
	svc     #0
	brk     #0                        /* not reached */
	.size __xtc_preempt_trampoline, .-__xtc_preempt_trampoline

/* End marker for the interrupted-PC range check in the redirect. */
	.globl __xtc_preempt_trampoline_end
__xtc_preempt_trampoline_end:

	.section .note.GNU-stack,"",%progbits

#endif /* __aarch64__ && __linux__ */
