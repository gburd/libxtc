/*
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/os/asm/preempt_trampoline_aarch64.S
 *	Involuntary-preemption resume trampoline for AArch64 (ARM64)
 *	AAPCS64.  Linux, *BSD ELF.  Phase 2b-arch of the preemption
 *	facility, the aarch64 counterpart of preempt_trampoline_x86_64.S.
 *
 *	Go's async-preemption mechanism.  When the per-thread CPU timer
 *	fires inside a running fiber at a SAFE point (crit_depth == 0,
 *	unsafe_depth == 0), __xtc_coro_preempt rewrites the interrupted
 *	mcontext so that on sigreturn the fiber resumes -- on its own
 *	stack, with its own real registers -- at this trampoline, rather
 *	than switching stacks inside the async-signal handler (unsound).
 *
 *	The trampoline runs at an ARBITRARY instruction boundary, so
 *	every register is potentially live: it saves x0-x30, the NZCV
 *	flags, and the full v0-v31 SIMD/FP file plus FPSR/FPCR, does a
 *	normal cooperative xtc_yield(), then restores everything and
 *	branches back to the interrupted PC with the interrupted SP
 *	exactly restored.  Invisible to the interrupted code, save for one
 *	intra-procedure-call scratch register, x16 (IP0) -- see the caveat
 *	below.
 *
 *	Entry contract (from __xtc_coro_preempt in coro_uctx.c):
 *	  sp  = orig_sp - 16   (16-aligned scratch below the interrupted
 *	        sp; AAPCS64 has no red zone, so memory below the
 *	        interrupted sp is free for us to use while resuming).
 *	  [orig_sp - 16] = orig_pc
 *	  all registers hold their interrupted values.
 *
 *	CAVEAT (x16): aarch64 has no instruction that restores every
 *	general register AND redirects PC+SP with zero free registers, so
 *	exactly one register is unavoidably consumed by the terminal
 *	indirect branch.  We sacrifice only x16 (IP0) and restore all
 *	other 30 GP registers (including x17), NZCV, and the whole FP/SIMD
 *	file exactly.  x16 is the ABI's intra-procedure-call temporary:
 *	the compiler treats it as clobbered across any call and uses it
 *	only transiently inside linker/veneer sequences, so at essentially
 *	every C instruction boundary it is dead.  This is the theoretical
 *	minimum (one scratch) and matches Go's arm64 async preemption.
 *
 *	Frame layout (bytes, from the trampoline's working sp, growing
 *	down; total 16-aligned):
 *	  0    x0  x1        16   x2  x3   ... 224  x28 x29
 *	  240  x30 (+pad)    256  NZCV(+pad)
 *	  272..                   v0..v31 (16 bytes each = 512)
 *	  784  FPSR FPCR
 *	We reserve 800 bytes (16-aligned).
 */

#if defined(__aarch64__) && !defined(__APPLE__) && !defined(_WIN32)

	.text

	.globl __xtc_preempt_trampoline
	.type  __xtc_preempt_trampoline,%function
	.align 4
__xtc_preempt_trampoline:
	/* Reserve the save frame below the entry sp (== orig_sp - 16). */
	sub     sp, sp, #800

	/* Save x0-x30 (pairs). */
	stp     x0,  x1,  [sp, #0]
	stp     x2,  x3,  [sp, #16]
	stp     x4,  x5,  [sp, #32]
	stp     x6,  x7,  [sp, #48]
	stp     x8,  x9,  [sp, #64]
	stp     x10, x11, [sp, #80]
	stp     x12, x13, [sp, #96]
	stp     x14, x15, [sp, #112]
	stp     x16, x17, [sp, #128]
	stp     x18, x19, [sp, #144]
	stp     x20, x21, [sp, #160]
	stp     x22, x23, [sp, #176]
	stp     x24, x25, [sp, #192]
	stp     x26, x27, [sp, #208]
	stp     x28, x29, [sp, #224]
	str     x30,      [sp, #240]

	/* Save NZCV (condition flags). */
	mrs     x9, nzcv
	str     x9,       [sp, #256]

	/* Save v0-v31 (128-bit each) at offset 272. */
	stp     q0,  q1,  [sp, #272]
	stp     q2,  q3,  [sp, #304]
	stp     q4,  q5,  [sp, #336]
	stp     q6,  q7,  [sp, #368]
	stp     q8,  q9,  [sp, #400]
	stp     q10, q11, [sp, #432]
	stp     q12, q13, [sp, #464]
	stp     q14, q15, [sp, #496]
	stp     q16, q17, [sp, #528]
	stp     q18, q19, [sp, #560]
	stp     q20, q21, [sp, #592]
	stp     q22, q23, [sp, #624]
	stp     q24, q25, [sp, #656]
	stp     q26, q27, [sp, #688]
	stp     q28, q29, [sp, #720]
	stp     q30, q31, [sp, #752]

	/* Save FPSR/FPCR.  Offset 784 exceeds the 64-bit stp range
	 * ([-512,504]); use an intermediate base register. */
	mrs     x9,  fpsr
	mrs     x10, fpcr
	add     x11, sp, #768
	stp     x9,  x10, [x11, #16]      /* [sp+784] */

	bl      xtc_yield                 /* cooperative switch to scheduler */

	/* Restore FPSR/FPCR. */
	add     x11, sp, #768
	ldp     x9,  x10, [x11, #16]
	msr     fpsr, x9
	msr     fpcr, x10

	/* Restore v0-v31. */
	ldp     q0,  q1,  [sp, #272]
	ldp     q2,  q3,  [sp, #304]
	ldp     q4,  q5,  [sp, #336]
	ldp     q6,  q7,  [sp, #368]
	ldp     q8,  q9,  [sp, #400]
	ldp     q10, q11, [sp, #432]
	ldp     q12, q13, [sp, #464]
	ldp     q14, q15, [sp, #496]
	ldp     q16, q17, [sp, #528]
	ldp     q18, q19, [sp, #560]
	ldp     q20, q21, [sp, #592]
	ldp     q22, q23, [sp, #624]
	ldp     q24, q25, [sp, #656]
	ldp     q26, q27, [sp, #688]
	ldp     q28, q29, [sp, #720]
	ldp     q30, q31, [sp, #752]

	/* Restore NZCV. */
	ldr     x9, [sp, #256]
	msr     nzcv, x9

	/*
	 * Recover the resume target BEFORE restoring the GP file.  The
	 * entry sp was orig_sp - 16, and the handler stored orig_pc at
	 * [orig_sp - 16] == [sp + 800].  Load it into x16 (a scratch we
	 * are allowed to clobber) and compute orig_sp into x17.
	 */
	/*
	 * Recover orig_pc before restoring the GP file, using x16 (IP0) as
	 * the sole scratch.  The entry sp was orig_sp - 16 and the handler
	 * stored orig_pc there == [sp + 800].  We restore EVERY other GP
	 * register (including x17) exactly; only x16 is consumed for the
	 * terminal indirect branch.  aarch64 cannot set SP and branch to an
	 * arbitrary PC with zero free registers, so exactly one register is
	 * unavoidably sacrificed; x16 (IP0) is the ABI's intra-procedure
	 * scratch, which the compiler treats as clobbered across any call
	 * and holds live only transiently inside linker/veneer sequences.
	 * This is the theoretical minimum and matches Go's arm64 async
	 * preemption.
	 */
	ldr     x16, [sp, #800]           /* x16 = orig_pc */

	/* Restore x0-x15 and x17-x30 exactly (only x16 is our scratch). */
	ldp     x0,  x1,  [sp, #0]
	ldp     x2,  x3,  [sp, #16]
	ldp     x4,  x5,  [sp, #32]
	ldp     x6,  x7,  [sp, #48]
	ldp     x8,  x9,  [sp, #64]
	ldp     x10, x11, [sp, #80]
	ldp     x12, x13, [sp, #96]
	ldp     x14, x15, [sp, #112]
	ldr     x17,      [sp, #136]      /* interrupted x17 restored */
	ldp     x18, x19, [sp, #144]
	ldp     x20, x21, [sp, #160]
	ldp     x22, x23, [sp, #176]
	ldp     x24, x25, [sp, #192]
	ldp     x26, x27, [sp, #208]
	ldp     x28, x29, [sp, #224]
	ldr     x30,      [sp, #240]

	/*
	 * Switch to the interrupted stack pointer (sp + 816 == orig_sp)
	 * and branch to orig_pc.  The interrupted code resumes with its
	 * exact sp and all GP registers restored except x16 (documented
	 * above).  We compute orig_sp into sp via a scratch-free add on sp
	 * itself, so no extra register is needed.
	 */
	add     sp, sp, #816              /* sp = orig_sp */
	br      x16                       /* -> orig_pc */
	.size __xtc_preempt_trampoline, .-__xtc_preempt_trampoline

/* End marker for the interrupted-PC range check in __xtc_coro_preempt. */
	.globl __xtc_preempt_trampoline_end
__xtc_preempt_trampoline_end:

	.section .note.GNU-stack,"",%progbits

#endif /* __aarch64__ && !__APPLE__ && !_WIN32 */
