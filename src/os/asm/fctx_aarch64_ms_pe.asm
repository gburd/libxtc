; Copyright (c) 2026, The XTC Project
; Use of this source code is governed by the ISC License,
; a copy of which is in the file LICENSE in the top-level directory
; of this distribution.
;
; src/os/asm/fctx_aarch64_ms_pe.asm
;       make_fcontext / jump_fcontext for AArch64 (ARM64) on Windows,
;       armasm64.exe syntax.  This is the MSVC sibling of
;       fctx_aarch64_ms_pe.S (GNU-assembler syntax, used by Clang on
;       Windows ARM64); both implement the identical frame layout and
;       calling contract so the two toolchains are interchangeable.
;
;       The Windows ARM64 ABI matches AAPCS64 for the callee-saved set
;       this code preserves:
;         x19-x28 (10 integer regs), x29 (frame pointer), x30 (link
;         reg), and the LOW 64 bits of v8-v15 (d8-d15).
;
;       CRITICAL: x18 is the Windows RESERVED PLATFORM REGISTER (TEB
;       pointer).  It is neither caller- nor callee-saved; it belongs
;       to the platform and MUST NOT be touched.  This routine never
;       reads or writes x18, so the running thread's TEB pointer
;       survives a fiber switch unchanged.  Do not add x18 to the
;       saved set and do not use it as scratch.
;
;       Frame layout (160 bytes), saved sp at offset +0:
;         +0    d8        +8    d9
;         +16   d10       +24   d11
;         +32   d12       +40   d13
;         +48   d14       +56   d15      (64 bytes of FP)
;         +64   x19       +72   x20
;         +80   x21       +88   x22
;         +96   x23       +104  x24
;         +112  x25       +120  x26
;         +128  x27       +136  x28
;         +144  x29 (fp)  +152  x30/pc  (resume address)
;       Total: 160 bytes (multiple of 16, so a switched-to sp lands
;       16-aligned as the ABI requires at a public interface).

	AREA	|.text|, CODE, READONLY

	EXPORT	__xtc_make_fcontext
	EXPORT	__xtc_jump_fcontext

; void *__xtc_make_fcontext(void *stack_top, size_t size,
;                           void (*fn)(void *transfer));
;   x0 = stack_top, x1 = size (advisory), x2 = fn
;   returns: x0 = saved-sp; pass to __xtc_jump_fcontext as `to`.
__xtc_make_fcontext PROC
	; Align stack top down to 16, reserve a 160-byte context block.
	and	x0, x0, #0xFFFFFFFFFFFFFFF0
	sub	x0, x0, #160

	; Zero the callee-saved integer slots (x19-x28, x29).
	stp	xzr, xzr, [x0, #64]	; x19, x20
	stp	xzr, xzr, [x0, #80]	; x21, x22
	stp	xzr, xzr, [x0, #96]	; x23, x24
	stp	xzr, xzr, [x0, #112]	; x25, x26
	stp	xzr, xzr, [x0, #128]	; x27, x28
	str	xzr, [x0, #144]		; x29 (fp)

	; Zero the FP slots d8-d15.
	stp	xzr, xzr, [x0, #0]	; d8, d9
	stp	xzr, xzr, [x0, #16]	; d10, d11
	stp	xzr, xzr, [x0, #32]	; d12, d13
	stp	xzr, xzr, [x0, #48]	; d14, d15

	; Saved resume address = fn.
	str	x2, [x0, #152]
	ret
	ENDP

; void *__xtc_jump_fcontext(void **from, void *to, void *transfer);
;   x0 = &from, x1 = to, x2 = transfer
;   returns: x0 = the `transfer` value passed by whoever later jumps
;            back to us.
;
; x18 (the Windows platform/TEB register) is deliberately untouched.
__xtc_jump_fcontext PROC
	; Reserve and fill our context block below the current sp.
	sub	sp, sp, #160

	stp	d8,  d9,  [sp, #0]
	stp	d10, d11, [sp, #16]
	stp	d12, d13, [sp, #32]
	stp	d14, d15, [sp, #48]

	stp	x19, x20, [sp, #64]
	stp	x21, x22, [sp, #80]
	stp	x23, x24, [sp, #96]
	stp	x25, x26, [sp, #112]
	stp	x27, x28, [sp, #128]
	stp	x29, x30, [sp, #144]	; fp, resume address (lr)

	; *from = current sp.
	mov	x9, sp
	str	x9, [x0]

	; sp = to.
	mov	sp, x1

	; Restore callee-saved state from the target context.
	ldp	d8,  d9,  [sp, #0]
	ldp	d10, d11, [sp, #16]
	ldp	d12, d13, [sp, #32]
	ldp	d14, d15, [sp, #48]

	ldp	x19, x20, [sp, #64]
	ldp	x21, x22, [sp, #80]
	ldp	x23, x24, [sp, #96]
	ldp	x25, x26, [sp, #112]
	ldp	x27, x28, [sp, #128]
	ldp	x29, x30, [sp, #144]	; fp, resume address

	add	sp, sp, #160

	; Hand the transfer arg to the resumed code.  x0 serves both as
	; arg0 for the first-jump-into-fn case and as the return value
	; for the resumed-jump_fcontext case; only one matters per jump.
	mov	x0, x2

	ret
	ENDP

	END
