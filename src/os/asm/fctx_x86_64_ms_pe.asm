; Copyright (c) 2026, The XTC Project
; Use of this source code is governed by the ISC License,
; a copy of which is in the file LICENSE in the top-level directory
; of this distribution.
;
; src/os/asm/fctx_x86_64_ms_pe.asm
;       make_fcontext / jump_fcontext for x86-64 Microsoft x64 ABI,
;       MASM (ml64.exe) syntax.  This is the MASM sibling of
;       fctx_x86_64_ms_pe.S (GNU-assembler syntax, used by MinGW and
;       Clang64); both implement the identical frame layout and
;       calling contract so the two toolchains are interchangeable.
;
;       NO XMM6-XMM15 save/restore: user coroutines do not rely on
;       SSE state across a yield.  Eight callee-saved general regs
;       plus the saved RIP, with the natural push/pop/ret pattern.
;
;       Win64 ABI: at function entry rsp == 8 (mod 16) (post-CALL).
;
;       Frame layout (72 bytes), saved sp at offset +0:
;         +0    rbx
;         +8    rbp
;         +16   rdi
;         +24   rsi
;         +32   r12
;         +40   r13
;         +48   r14
;         +56   r15
;         +64   saved RIP   (fn for fresh; post-call return for resumed)
;
;       Alignment cases (see the .S sibling for the full derivation):
;         - jump_fcontext-saved: 8 pushes from rsp%16==8 land at
;           saved_sp%16==8; after resume (+72) rsp%16==0, the caller's
;           post-call alignment.
;         - make_fcontext-prepared: rax%16==0 at a 16-aligned chunk;
;           after resume rsp%16==8, the function-entry alignment for
;           entering fn the first time.

_TEXT SEGMENT

; void *__xtc_make_fcontext(void *stack_top, size_t size,
;                           void (*fn)(void *transfer));
;   rcx = stack_top, rdx = size (advisory), r8 = fn
;   returns: rax = saved sp; pass to __xtc_jump_fcontext as `to`.
PUBLIC __xtc_make_fcontext
ALIGN 16
__xtc_make_fcontext PROC
        mov     rax, rcx
        and     rax, -16                ; rax % 16 == 0
        sub     rax, 80                 ; still rax % 16 == 0; layout
                                        ; uses rax+0..+71 with 8 bytes
                                        ; slack to the 16-aligned tip

        mov     QWORD PTR [rax+64], r8  ; saved RIP = fn

        xor     rcx, rcx                ; zero the 8 callee-saved slots
        mov     QWORD PTR [rax+0],  rcx ; rbx
        mov     QWORD PTR [rax+8],  rcx ; rbp
        mov     QWORD PTR [rax+16], rcx ; rdi
        mov     QWORD PTR [rax+24], rcx ; rsi
        mov     QWORD PTR [rax+32], rcx ; r12
        mov     QWORD PTR [rax+40], rcx ; r13
        mov     QWORD PTR [rax+48], rcx ; r14
        mov     QWORD PTR [rax+56], rcx ; r15
        ret
__xtc_make_fcontext ENDP

; void *__xtc_jump_fcontext(void **from, void *to, void *transfer);
;   rcx = &from, rdx = to, r8 = transfer
;
; Natural push/pop/ret: 8 pushes save state below the CALL-pushed
; return address; resume pops + ret returns to that saved RIP.
PUBLIC __xtc_jump_fcontext
ALIGN 16
__xtc_jump_fcontext PROC
        push    r15
        push    r14
        push    r13
        push    r12
        push    rsi
        push    rdi
        push    rbp
        push    rbx

        ; Frame now matches make_fcontext layout from rsp+0:
        ;   rsp+0 = rbx ... rsp+56 = r15, rsp+64 = saved RIP.
        mov     QWORD PTR [rcx], rsp    ; *from = rsp
        mov     rsp, rdx                ; rsp = to

        pop     rbx
        pop     rbp
        pop     rdi
        pop     rsi
        pop     r12
        pop     r13
        pop     r14
        pop     r15

        ; Hand transfer as rcx (Win64 first arg, for a fresh fn) and
        ; rax (return value, for a resumed jump_fcontext).  Only one
        ; is meaningful per resume.
        mov     rcx, r8
        mov     rax, r8
        ret
__xtc_jump_fcontext ENDP

_TEXT ENDS
END
