/*
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/os/asm/fctx_arm_aapcs.S
 *	make_fcontext / jump_fcontext for 32-bit ARM (AArch32), AAPCS
 *	EABI.  ELF.  No Boost.Context dependency.
 *
 * Status: written, assembles with arm cross gcc.  NOT yet
 *	runtime-verified under qemu (no arm32 user-mode run was
 *	performed in this round); the integer save/restore mirrors the
 *	verified aarch64 path and the VFP block is guarded on hard-float
 *	builds.  Treat as compile-only until a qemu-arm run confirms it,
 *	in the spirit of src/io/io_aix.c's documented untested status.
 *
 *	AAPCS callee-saved state that must survive a switch:
 *	  r4-r11 (8 integer regs), lr (r14, return address).  r13 (sp)
 *	  is the context handle itself.  On hard-float (VFP) builds the
 *	  double-precision regs d8-d15 (s16-s31) are also callee-saved
 *	  and must be preserved; soft-float builds skip them.
 *
 *	Stack alignment: AAPCS requires sp to be 8-byte aligned at a
 *	public interface.  We keep the saved context block an 8-byte
 *	multiple so a switched-to sp lands 8-aligned.  (We over-align
 *	the make_fcontext start to 16 for headroom; the ABI only
 *	requires 8.)
 *
 *	Layout of a saved context (offsets from the saved sp, growing up):
 *	  +0    lr (resume address)
 *	  +4    r4    +8    r5
 *	  +12   r6    +16   r7
 *	  +20   r8    +24   r9
 *	  +28   r10   +32   r11    (lr + 8 GPRs = 36 bytes)
 *	  +40   d8 .. +96   d15    (8 doubles = 64 bytes, hard-float)
 *	The integer area occupies +0..+35; we pad to +40 (8-byte
 *	alignment for the VFP block) and reserve 104 bytes total
 *	(multiple of 8).  On soft-float the VFP slots are simply
 *	unused.
 */

#if defined(__arm__) && !defined(_WIN32)

	.text
	.syntax unified
#if defined(__thumb2__)
	.thumb
#else
	.arm
#endif

/*
 * void *__xtc_make_fcontext(void *stack_top, size_t size,
 *                           void (*fn)(void *transfer));
 *   r0 = stack_top, r1 = size (advisory), r2 = fn
 *   returns: r0 = saved-sp; pass to __xtc_jump_fcontext as `to`.
 */
	.globl __xtc_make_fcontext
	.type  __xtc_make_fcontext,%function
	.align 2
#if defined(__thumb2__)
	.thumb_func
#endif
__xtc_make_fcontext:
	/* Align the stack top down to 16, then reserve a 104-byte
	 * context block. */
	bic     r0, r0, #15
	sub     r0, r0, #104

	/* Zero the callee-saved integer slots (r4-r11). */
	mov     r3, #0
	str     r3, [r0, #4]             /* r4  */
	str     r3, [r0, #8]             /* r5  */
	str     r3, [r0, #12]            /* r6  */
	str     r3, [r0, #16]            /* r7  */
	str     r3, [r0, #20]            /* r8  */
	str     r3, [r0, #24]            /* r9  */
	str     r3, [r0, #28]            /* r10 */
	str     r3, [r0, #32]            /* r11 */

#if defined(__ARM_PCS_VFP) || defined(__VFP_FP__)
	/* Zero the VFP slots d8-d15 (hard-float only). */
	str     r3, [r0, #40]
	str     r3, [r0, #44]
	str     r3, [r0, #48]
	str     r3, [r0, #52]
	str     r3, [r0, #56]
	str     r3, [r0, #60]
	str     r3, [r0, #64]
	str     r3, [r0, #68]
	str     r3, [r0, #72]
	str     r3, [r0, #76]
	str     r3, [r0, #80]
	str     r3, [r0, #84]
	str     r3, [r0, #88]
	str     r3, [r0, #92]
	str     r3, [r0, #96]
	str     r3, [r0, #100]
#endif

	/* Saved resume address (lr) = fn.  The first jump_fcontext
	 * loads this into pc with the transfer value already in r0. */
	str     r2, [r0, #0]
	bx      lr
	.size __xtc_make_fcontext, .-__xtc_make_fcontext


/*
 * void *__xtc_jump_fcontext(void **from, void *to, void *transfer);
 *   r0 = &from, r1 = to, r2 = transfer
 *   returns: r0 = the `transfer` value passed by whoever later
 *            jumps back to us.
 */
	.globl __xtc_jump_fcontext
	.type  __xtc_jump_fcontext,%function
	.align 2
#if defined(__thumb2__)
	.thumb_func
#endif
__xtc_jump_fcontext:
	/* Reserve our context block below the current sp. */
	sub     sp, sp, #104

	/* Save lr and callee-saved integer regs r4-r11. */
	str     lr, [sp, #0]
	str     r4, [sp, #4]
	str     r5, [sp, #8]
	str     r6, [sp, #12]
	str     r7, [sp, #16]
	str     r8, [sp, #20]
	str     r9, [sp, #24]
	str     r10, [sp, #28]
	str     r11, [sp, #32]

#if defined(__ARM_PCS_VFP) || defined(__VFP_FP__)
	/* Save callee-saved VFP regs d8-d15 (hard-float only). */
	vstr    d8,  [sp, #40]
	vstr    d9,  [sp, #48]
	vstr    d10, [sp, #56]
	vstr    d11, [sp, #64]
	vstr    d12, [sp, #72]
	vstr    d13, [sp, #80]
	vstr    d14, [sp, #88]
	vstr    d15, [sp, #96]
#endif

	/* *from = current sp. */
	str     sp, [r0]

	/* sp = to. */
	mov     sp, r1

	/* Restore lr and callee-saved integer regs. */
	ldr     lr, [sp, #0]
	ldr     r4, [sp, #4]
	ldr     r5, [sp, #8]
	ldr     r6, [sp, #12]
	ldr     r7, [sp, #16]
	ldr     r8, [sp, #20]
	ldr     r9, [sp, #24]
	ldr     r10, [sp, #28]
	ldr     r11, [sp, #32]

#if defined(__ARM_PCS_VFP) || defined(__VFP_FP__)
	/* Restore callee-saved VFP regs d8-d15. */
	vldr    d8,  [sp, #40]
	vldr    d9,  [sp, #48]
	vldr    d10, [sp, #56]
	vldr    d11, [sp, #64]
	vldr    d12, [sp, #72]
	vldr    d13, [sp, #80]
	vldr    d14, [sp, #88]
	vldr    d15, [sp, #96]
#endif

	/* Drop the context block. */
	add     sp, sp, #104

	/* Hand the transfer arg to the resumed code.  r0 is arg0 for
	 * the first-jump-into-fn case and the return value for the
	 * resumed-jump_fcontext case; only one matters per jump. */
	mov     r0, r2

	bx      lr
	.size __xtc_jump_fcontext, .-__xtc_jump_fcontext

	.section .note.GNU-stack,"",%progbits

#endif  /* __arm__ && !_WIN32 */
