/*
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/os/asm/fctx_riscv64_lp64d.S
 *	make_fcontext / jump_fcontext for RISC-V 64-bit, lp64d ABI
 *	(hard-float double).  ELF.  No Boost.Context dependency.
 *
 * Status: written, assembles with riscv64 cross gcc, and RUNTIME
 *	VERIFIED under qemu-riscv64 user-mode (round-trip fiber harness,
 *	callee-saved s-regs + fs0-fs11 FP preserved across switches).
 *
 *	lp64d callee-saved state that must survive a switch:
 *	  ra (x1, return address), s0/fp (x8), s1 (x9), s2-s11
 *	  (x18-x27) -- 12 saved integer regs plus ra -- and the
 *	  floating-point saved regs fs0/fs1 (f8/f9) and fs2-fs11
 *	  (f18-f27), 12 in total.
 *
 *	Stack alignment: the RISC-V psABI requires the stack pointer to
 *	be 16-byte aligned at a procedure call.  We keep the saved
 *	context block a multiple of 16 bytes.
 *
 *	Layout of a saved context (offsets from the saved sp, growing up):
 *	  +0    ra (resume address)
 *	  +8    s0/fp     +16   s1
 *	  +24   s2        +32   s3
 *	  +40   s4        +48   s5
 *	  +56   s6        +64   s7
 *	  +72   s8        +80   s9
 *	  +88   s10       +96   s11    (ra + 13 GPRs = 104 bytes)
 *	  +104  fs0       +112  fs1
 *	  +120  fs2       +128  fs3
 *	  +136  fs4       +144  fs5
 *	  +152  fs6       +160  fs7
 *	  +168  fs8       +176  fs9
 *	  +184  fs10      +192  fs11   (12 FP regs = 96 bytes)
 *	Total: 200 bytes; rounded up to a multiple of 16 -> 208 bytes.
 */

#if defined(__riscv) && (__riscv_xlen == 64) && !defined(_WIN32)

	.text

/*
 * void *__xtc_make_fcontext(void *stack_top, size_t size,
 *                           void (*fn)(void *transfer));
 *   a0 = stack_top, a1 = size (advisory), a2 = fn
 *   returns: a0 = saved-sp; pass to __xtc_jump_fcontext as `to`.
 */
	.globl __xtc_make_fcontext
	.type  __xtc_make_fcontext,@function
	.align 2
__xtc_make_fcontext:
	/* Align the stack top down to 16, then reserve a 208-byte
	 * context block. */
	andi    a0, a0, -16
	addi    a0, a0, -208

	/* Zero the saved integer slots (s0-s11). */
	sd      zero, 8(a0)              /* s0/fp */
	sd      zero, 16(a0)             /* s1  */
	sd      zero, 24(a0)             /* s2  */
	sd      zero, 32(a0)             /* s3  */
	sd      zero, 40(a0)             /* s4  */
	sd      zero, 48(a0)             /* s5  */
	sd      zero, 56(a0)             /* s6  */
	sd      zero, 64(a0)             /* s7  */
	sd      zero, 72(a0)             /* s8  */
	sd      zero, 80(a0)             /* s9  */
	sd      zero, 88(a0)             /* s10 */
	sd      zero, 96(a0)             /* s11 */

	/* Zero the FP slots fs0-fs11. */
	sd      zero, 104(a0)
	sd      zero, 112(a0)
	sd      zero, 120(a0)
	sd      zero, 128(a0)
	sd      zero, 136(a0)
	sd      zero, 144(a0)
	sd      zero, 152(a0)
	sd      zero, 160(a0)
	sd      zero, 168(a0)
	sd      zero, 176(a0)
	sd      zero, 184(a0)
	sd      zero, 192(a0)

	/* Saved resume address (ra) = fn.  The first jump_fcontext
	 * loads ra from here and "ret" enters fn with the transfer
	 * value already in a0. */
	sd      a2, 0(a0)
	ret
	.size __xtc_make_fcontext, .-__xtc_make_fcontext


/*
 * void *__xtc_jump_fcontext(void **from, void *to, void *transfer);
 *   a0 = &from, a1 = to, a2 = transfer
 *   returns: a0 = the `transfer` value passed by whoever later
 *            jumps back to us.
 */
	.globl __xtc_jump_fcontext
	.type  __xtc_jump_fcontext,@function
	.align 2
__xtc_jump_fcontext:
	/* Reserve our context block below the current sp. */
	addi    sp, sp, -208

	/* Save ra and callee-saved integer regs. */
	sd      ra, 0(sp)
	sd      s0, 8(sp)
	sd      s1, 16(sp)
	sd      s2, 24(sp)
	sd      s3, 32(sp)
	sd      s4, 40(sp)
	sd      s5, 48(sp)
	sd      s6, 56(sp)
	sd      s7, 64(sp)
	sd      s8, 72(sp)
	sd      s9, 80(sp)
	sd      s10, 88(sp)
	sd      s11, 96(sp)

	/* Save callee-saved FP regs fs0-fs11. */
	fsd     fs0, 104(sp)
	fsd     fs1, 112(sp)
	fsd     fs2, 120(sp)
	fsd     fs3, 128(sp)
	fsd     fs4, 136(sp)
	fsd     fs5, 144(sp)
	fsd     fs6, 152(sp)
	fsd     fs7, 160(sp)
	fsd     fs8, 168(sp)
	fsd     fs9, 176(sp)
	fsd     fs10, 184(sp)
	fsd     fs11, 192(sp)

	/* *from = current sp. */
	sd      sp, 0(a0)

	/* sp = to. */
	mv      sp, a1

	/* Restore ra and callee-saved integer regs. */
	ld      ra, 0(sp)
	ld      s0, 8(sp)
	ld      s1, 16(sp)
	ld      s2, 24(sp)
	ld      s3, 32(sp)
	ld      s4, 40(sp)
	ld      s5, 48(sp)
	ld      s6, 56(sp)
	ld      s7, 64(sp)
	ld      s8, 72(sp)
	ld      s9, 80(sp)
	ld      s10, 88(sp)
	ld      s11, 96(sp)

	/* Restore callee-saved FP regs. */
	fld     fs0, 104(sp)
	fld     fs1, 112(sp)
	fld     fs2, 120(sp)
	fld     fs3, 128(sp)
	fld     fs4, 136(sp)
	fld     fs5, 144(sp)
	fld     fs6, 152(sp)
	fld     fs7, 160(sp)
	fld     fs8, 168(sp)
	fld     fs9, 176(sp)
	fld     fs10, 184(sp)
	fld     fs11, 192(sp)

	/* Drop the context block. */
	addi    sp, sp, 208

	/* Hand the transfer arg to the resumed code.  a0 is arg0 for
	 * the first-jump-into-fn case and the return value for the
	 * resumed-jump_fcontext case; only one matters per jump. */
	mv      a0, a2

	ret
	.size __xtc_jump_fcontext, .-__xtc_jump_fcontext

	.section .note.GNU-stack,"",@progbits

#endif  /* __riscv && __riscv_xlen == 64 && !_WIN32 */
