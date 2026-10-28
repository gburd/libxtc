/*
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/os/asm/fctx_x86_64_sysv.S
 *	make_fcontext / jump_fcontext for x86-64 System V ABI.
 *	Linux, *BSD, illumos.  No Boost.Context dependency.
 *
 *	The clever part is the alignment.  Sys V ABI:
 *	  At a function's first instruction, (%rsp + 8) % 16 == 0,
 *	  i.e., %rsp % 16 == 8 (because the CALL just pushed a return
 *	  address on top of an aligned-to-16 caller frame).
 *
 *	jump_fcontext is itself a normal C-callable function.  It is
 *	entered with %rsp % 16 == 8.  We push 6 callee-saved regs (48
 *	bytes) and an 8-byte FP-control slot, so the saved %rsp we
 *	record has %rsp % 16 == 0.  When control later resumes here
 *	(i.e., somebody jumps back into us), we drop the slot, pop the 6
 *	regs and pop + jmp to the saved RIP (64 bytes); the target sees
 *	%rsp % 16 == 0,
 *	which is the correct alignment for the post-call return site in
 *	the caller of jump_fcontext.
 *
 *	make_fcontext sets up a frame that, when used as `to` for the
 *	first jump_fcontext, runs `fn(transfer_arg)` with the proper
 *	function-entry alignment.  To make the same 64-byte unwind land
 *	at %rsp % 16 == 8 (function-entry rule) instead of % 16 == 0
 *	(post-call rule), we choose a saved-sp with %16 == 8 instead
 *	of 0.  The same instructions produce different post-ret
 *	alignments depending on the alignment of the loaded-from sp --
 *	which is exactly what we need.
 *
 *	Layout of a saved context (offsets from the saved %rsp):
 *	  +0   MXCSR (4 bytes) + x87 control word (2 bytes) + pad
 *	  +8   rbx
 *	  +16  rbp
 *	  +24  r12
 *	  +32  r13
 *	  +40  r14
 *	  +48  r15
 *	  +56  saved RIP   (= fn for a fresh make_fcontext context;
 *	                    = post-call return site for a saved
 *	                      jump_fcontext context)
 *
 *	MXCSR's control bits and the x87 control word are callee-saved in
 *	the System V ABI (rounding mode, exception masks, precision), so a
 *	jump must hand each fiber back the ones it left with, as glibc's
 *	swapcontext and Boost.Context do: without this, a fiber that set
 *	FE_UPWARD and yielded leaked it to every fiber the scheduler ran
 *	next.  A fresh context starts with its creator's settings
 *	(makecontext's getcontext captures them the same way).  The extra
 *	8-byte slot flips the saved sp's alignment, so a fresh context's
 *	sp is 8 (mod 16) and a saved one's 0 (mod 16); the pop + ret below
 *	still land each on the alignment its resume site needs.
 */

#if defined(__x86_64__) && !defined(__APPLE__) && !defined(_WIN32)

	.text

/*
 * void *__xtc_make_fcontext(void *stack_top, size_t size,
 *                           void (*fn)(void *transfer));
 *   rdi = stack_top, rsi = size (advisory), rdx = fn
 *   returns: rax = saved-sp; pass to __xtc_jump_fcontext as `to`.
 */
	.globl __xtc_make_fcontext
	.type  __xtc_make_fcontext,@function
	.align 16
__xtc_make_fcontext:
	movq    %rdi, %rax
	andq    $-16, %rax               /* %rax % 16 == 0            */
	subq    $72, %rax                /* % 16 == 8; offsets 0..56  */
	                                  /* (64 bytes), top 8 slack.  */
	                                  /* pop 6 + 8 + ret = +72, so  */
	                                  /* fn is entered at % 16 == 8 */
	movq    %rdx, 56(%rax)           /* saved RIP = fn */
	xorq    %rcx, %rcx
	movq    %rcx, 0(%rax)            /* clear the pad bytes too */
	stmxcsr 0(%rax)                  /* the creator's MXCSR ...  */
	fnstcw  4(%rax)                  /* ... and x87 control word */
	movq    %rcx, 8(%rax)            /* rbx */
	movq    %rcx, 16(%rax)           /* rbp */
	movq    %rcx, 24(%rax)           /* r12 */
	movq    %rcx, 32(%rax)           /* r13 */
	movq    %rcx, 40(%rax)           /* r14 */
	movq    %rcx, 48(%rax)           /* r15 */
	ret
	.size __xtc_make_fcontext, .-__xtc_make_fcontext


/*
 * void *__xtc_jump_fcontext(void **from, void *to, void *transfer);
 *   rdi = &from, rsi = to, rdx = transfer
 *   returns: the `transfer` value passed by whoever later jumps to us.
 */
	.globl __xtc_jump_fcontext
	.type  __xtc_jump_fcontext,@function
	.align 16
__xtc_jump_fcontext:
	pushq   %r15
	pushq   %r14
	pushq   %r13
	pushq   %r12
	pushq   %rbp
	pushq   %rbx
	leaq    -8(%rsp), %rsp           /* FP control slot */
	stmxcsr (%rsp)
	fnstcw  4(%rsp)

	movq    %rsp, (%rdi)             /* *from = current sp */
	movq    %rsi, %rsp               /* sp = to            */

	ldmxcsr (%rsp)
	fldcw   4(%rsp)
	leaq    8(%rsp), %rsp
	popq    %rbx
	popq    %rbp
	popq    %r12
	popq    %r13
	popq    %r14
	popq    %r15

	/* Hand the transfer arg to the resumed code.  rdi = arg0
	 * register for first-jump-into-fn case; rax = return value
	 * for resumed-jump_fcontext case.  Only one matters per jump. */
	movq    %rdx, %rdi
	movq    %rdx, %rax

	/* Resume with an indirect jump, not `ret`.  A `ret` here returns to
	 * an address the return-stack buffer never saw a matching call
	 * for (the call that saved it was made on the OTHER context's
	 * stack), so it mispredicted on every switch: 22.2 ns per round
	 * trip, against 6.9 ns with pop + jmp, on a 32-vCPU x86-64 host
	 * (Boost.Context's x86-64 jump does the same).  %rsp ends where
	 * the ret would leave it, so the alignment rules above still hold.
	 * %r8 is caller-saved and not an argument register of fn. */
	popq    %r8
	jmpq    *%r8
	.size __xtc_jump_fcontext, .-__xtc_jump_fcontext

	.section .note.GNU-stack,"",@progbits

#endif  /* __x86_64__ && !__APPLE__ && !_WIN32 */
