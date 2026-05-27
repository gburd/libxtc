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
 *	bytes), so the saved %rsp we record has %rsp % 16 == 8.  When
 *	control later resumes here (i.e., somebody jumps back into us),
 *	we pop the 6 regs and ret; the ret-target sees %rsp % 16 == 0,
 *	which is the correct alignment for the post-call return site
 *	in the caller of jump_fcontext.
 *
 *	make_fcontext sets up a frame that, when used as `to` for the
 *	first jump_fcontext, runs `fn(transfer_arg)` with the proper
 *	function-entry alignment.  To make the pop+ret sequence land
 *	at %rsp % 16 == 8 (function-entry rule) instead of % 16 == 0
 *	(post-call rule), we choose a saved-sp with %16 == 0 instead
 *	of 8.  The same pop+ret instructions produce different
 *	post-ret alignments depending on the alignment of the
 *	loaded-from sp -- which is exactly what we need.
 *
 *	Layout of a saved context (offsets from the saved %rsp):
 *	  +0   rbx
 *	  +8   rbp
 *	  +16  r12
 *	  +24  r13
 *	  +32  r14
 *	  +40  r15
 *	  +48  saved RIP   (= fn for a fresh make_fcontext context;
 *	                    = post-call return site for a saved
 *	                      jump_fcontext context)
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
	andq    $-16, %rax               /* %rax % 16 == 0           */
	subq    $64, %rax                /* still % 16 == 0; we use  */
	                                  /* offsets 0..48 (56 bytes) */
	                                  /* and leave the top 8 as  */
	                                  /* slack.                  */
	movq    %rdx, 48(%rax)           /* saved RIP = fn */
	xorq    %rcx, %rcx
	movq    %rcx, 0(%rax)            /* rbx */
	movq    %rcx, 8(%rax)            /* rbp */
	movq    %rcx, 16(%rax)           /* r12 */
	movq    %rcx, 24(%rax)           /* r13 */
	movq    %rcx, 32(%rax)           /* r14 */
	movq    %rcx, 40(%rax)           /* r15 */
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

	movq    %rsp, (%rdi)             /* *from = current sp */
	movq    %rsi, %rsp               /* sp = to            */

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

	ret
	.size __xtc_jump_fcontext, .-__xtc_jump_fcontext

	.section .note.GNU-stack,"",@progbits

#endif  /* __x86_64__ && !__APPLE__ && !_WIN32 */
