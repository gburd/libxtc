/*
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/os/asm/fctx_x86_64_ms_pe.S
 *	make_fcontext / jump_fcontext for x86-64 Microsoft x64 ABI
 *	(MinGW MS PE/COFF).  Sister of fctx_x86_64_sysv.S.
 *
 *	Round-2 final: NO XMM6-XMM15 save/restore (round 3 work; user
 *	coroutines don't currently rely on SSE state across yields).
 *	Just 8 callee-saved general regs + saved RIP.  Asm uses the
 *	natural push/pop/ret pattern (mirrors boost.context); resume
 *	alignment is selected by how saved_sp aligns.
 *
 *	Win64 ABI: at function entry, rsp === 8 (mod 16) (post-CALL).
 *
 *	Frame layout (72 bytes), saved sp at offset +0:
 *	  +0    rbx
 *	  +8    rbp
 *	  +16   rdi
 *	  +24   rsi
 *	  +32   r12
 *	  +40   r13
 *	  +48   r14
 *	  +56   r15
 *	  +64   saved RIP   (fn for fresh; post-call return for resumed)
 *
 *	Two alignment cases:
 *	  - jump_fcontext-saved: 8 pushes from rsp%16==8 land at
 *	    saved_sp%16==8.  After resume (8 pops + ret = +72),
 *	    rsp%16 = (8+72)%16 = 0.  This is the CALLER's post-call
 *	    alignment, which is correct for returning into the
 *	    original caller of jump_fcontext.
 *	  - make_fcontext-prepared: rax%16==0 (16-aligned chunk at
 *	    `(stack_top & -16) - 80`).  After resume, rsp%16 =
 *	    (0+72)%16 = 8.  This is the FUNCTION-ENTRY alignment,
 *	    correct for entering `fn` for the first time.
 *
 *	Same asm, different starting alignment, different final
 *	alignment -- exactly what we need for both call-paths.
 */

#if defined(_WIN32) && defined(__x86_64__)

	.text

/*
 * void *__xtc_make_fcontext(void *stack_top, size_t size,
 *                           void (*fn)(void *transfer));
 *   rcx = stack_top, rdx = size (advisory), r8 = fn
 *   returns: rax = saved sp; pass to __xtc_jump_fcontext as `to`.
 */
	.globl __xtc_make_fcontext
	.align 16
__xtc_make_fcontext:
	movq    %rcx, %rax
	andq    $-16, %rax              /* rax % 16 == 0          */
	subq    $80, %rax               /* still rax % 16 == 0;    */
	                                 /* layout uses rax+0..+71 */
	                                 /* with 8 bytes slack     */
	                                 /* between rax+72 and the */
	                                 /* original 16-aligned tip */

	/* Saved RIP = fn at +64. */
	movq    %r8, 64(%rax)

	/* Zero the 8 callee-saved-reg slots. */
	xorq    %rcx, %rcx
	movq    %rcx,  0(%rax)           /* rbx */
	movq    %rcx,  8(%rax)           /* rbp */
	movq    %rcx, 16(%rax)           /* rdi */
	movq    %rcx, 24(%rax)           /* rsi */
	movq    %rcx, 32(%rax)           /* r12 */
	movq    %rcx, 40(%rax)           /* r13 */
	movq    %rcx, 48(%rax)           /* r14 */
	movq    %rcx, 56(%rax)           /* r15 */
	ret

/*
 * void *__xtc_jump_fcontext(void **from, void *to, void *transfer);
 *   rcx = &from, rdx = to, r8 = transfer
 *
 * Natural push/pop/ret: 8 pushes save state below the CALL-pushed
 * return address; resume pops + ret returns to that saved RIP.
 */
	.globl __xtc_jump_fcontext
	.align 16
__xtc_jump_fcontext:
	pushq   %r15
	pushq   %r14
	pushq   %r13
	pushq   %r12
	pushq   %rsi
	pushq   %rdi
	pushq   %rbp
	pushq   %rbx

	/* Frame now matches make_fcontext layout from rsp+0:
	 *   rsp+0  = rbx
	 *   rsp+8  = rbp
	 *   ...
	 *   rsp+56 = r15
	 *   rsp+64 = saved RIP (return addr from CALL into us) */

	movq    %rsp, (%rcx)             /* *from = rsp */
	movq    %rdx, %rsp               /* rsp = to    */

	popq    %rbx
	popq    %rbp
	popq    %rdi
	popq    %rsi
	popq    %r12
	popq    %r13
	popq    %r14
	popq    %r15

	/* Hand transfer arg as rcx (Win64 first arg) and rax (return
	 * value).  Only one is meaningful per resume:
	 *   - First resume into a fresh make_fcontext: fn(rcx) is
	 *     called with rcx = transfer.
	 *   - Resume of a paused jump_fcontext call: the original
	 *     jump_fcontext returns with rax = transfer. */
	movq    %r8, %rcx
	movq    %r8, %rax

	ret

#endif  /* _WIN32 && __x86_64__ */
