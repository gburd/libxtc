/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/coro_preempt.h
 *	The involuntary-preemption pieces both mmap-stack coroutine
 *	substrates share (coro_uctx.c, coro_fctx.c): the per-thread
 *	in-switch guard, the x86-64 vector-state probe, and the x86-64 and
 *	AArch64 Linux signal-context PC redirects into the substrate-
 *	agnostic trampolines (src/os/asm/preempt_trampoline_*.S, which only
 *	preserve the interrupted state, call xtc_yield and resume it).
 *	Moved here verbatim from coro_uctx.c.  Exactly one substrate is
 *	compiled per build, so the `static` definitions are instantiated
 *	once.  Include after coro_int.h.
 */

#ifndef XTC_CORO_PREEMPT_H
#define XTC_CORO_PREEMPT_H

#include <stdatomic.h>
#include <stdint.h>

/*
 * XTC_CORO_REDIRECT_X86_64: this build implements the x86-64 Linux
 * signal-context redirect (Phase 2b-arch).  ucontext: as before, every
 * x86-64 Linux build outside the amalgamation.  fcontext: glibc only --
 * the code is libc-independent (the kernel's signal ucontext, the
 * trampoline, xtc_yield), but it has been verified on glibc alone; musl
 * keeps declining (cooperative-assisted preemption) until it is.
 */
#if defined(__x86_64__) && defined(__linux__) && !defined(__APPLE__) && \
    !defined(XTC_AMALGAMATION) && \
    (!defined(XTC_CORO_FCTX) || defined(__GLIBC__))
# define XTC_CORO_REDIRECT_X86_64 1
#endif
/*
 * XTC_CORO_REDIRECT_AARCH64: the AArch64 Linux redirect (rt_sigreturn
 * on the kernel's in-place signal frame; preempt_trampoline_aarch64.S).
 * Any libc: it reads only the kernel's frame (offsets asserted below
 * against the libc's <ucontext.h>) and the trampoline makes raw
 * syscalls.  Tested on glibc (both substrates) and on musl (fcontext,
 * its only substrate there): test/m14/test_preempt*.
 */
#if defined(__aarch64__) && defined(__linux__) && !defined(__APPLE__) && \
    !defined(XTC_AMALGAMATION)
# define XTC_CORO_REDIRECT_AARCH64 1
#endif
#if defined(XTC_CORO_REDIRECT_X86_64) || defined(XTC_CORO_REDIRECT_AARCH64)
#include <signal.h>
#include <ucontext.h>    /* the signal-delivered ucontext_t (mcontext) */
/* 1 if an involuntary yield may land at pc (preempt.c, SAFE POINTS). */
extern int __xtc_preempt_pc_ok(uintptr_t pc);
#endif

/*
 * Per-thread guard for the involuntary-preemption redirect (Phase 2b).
 * It marks the brief windows during which the running context's
 * machine state is being saved/restored by a ucontext swapcontext (or
 * the trampoline is mid-save) -- exactly the windows in which an
 * involuntary redirect would corrupt a half-written context (verified:
 * a rare SIGSEGV in the scheduler's swapcontext resume under
 * aggressive slicing).
 *
 * It is a FLAG set immediately before each swapcontext and cleared
 * immediately after it returns -- on whichever side runs next, since a
 * swapcontext hands control to a context that itself returns just
 * after its own swapcontext.  So the flag is 1 only while a
 * swapcontext instruction is actually executing, NOT for the whole
 * time a fiber is parked (that earlier, depth-held-across-park design
 * wrongly blocked ALL preemption whenever any fiber sat parked).
 * __xtc_coro_preempt declines while the flag is set, deferring to the
 * cooperative pending flag.  volatile: the timer handler reads it on
 * the same thread that maintains it; a stale read only errs toward
 * "decline", which is safe.
 */
static XTC_THREAD_LOCAL volatile int g_in_preempt_tls;
/*
 * Every access goes through a non-inlinable accessor so it addresses the
 * CURRENT thread's slot.  A migratable fiber can resume from swapcontext
 * on another OS thread, and clang keeps a thread-local's address
 * (%fs:0 + off or a __tls_get_addr result) live across the call: the
 * post-swap `g_in_preempt = 0` then cleared the OLD thread's guard.
 * Same hazard and fix as __current_proc in src/ptc/proc.c.
 */
__attribute__((noinline))
static volatile int *
__in_preempt_slot(void)
{
	volatile int *volatile slot = &g_in_preempt_tls;
	return slot;
}
#define g_in_preempt (*__in_preempt_slot())

#if defined(XTC_CORO_REDIRECT_X86_64)
#include <cpuid.h>
/*
 * The vector state the x86-64 trampoline saves (preempt_trampoline_x86_64.S
 * reads both words): an XSAVE feature mask and the XSAVE area size, or a
 * zero mask for its FXSAVE path.  FXSAVE alone covers only x87 and
 * XMM0-15's low halves, so a fiber preempted inside AVX code (glibc's
 * memcpy on an AVX-512 host) resumed with another fiber's YMM upper
 * halves, ZMM16-31 and opmask registers (test/m14/test_preempt_p2
 * /vector_state: 100 of 69k checks corrupted per 0.4 s run before).
 *
 * The mask is XCR0 limited to x87, SSE, AVX, opmask, the two ZMM
 * components and PKRU: the state a fiber's own code can hold live across
 * an arbitrary instruction.  PKRU (the protection-key rights, xtc_pkey)
 * is per thread, so a fiber that ran in the slice could leave the
 * preempted code a different set of rights between its own wrpkru and
 * the access it guarded (test_preempt_p2 /tick_pkru: 64 of 64 ticks
 * resumed with the peer's PKRU before).  AMX tile data (8 KiB) is left out on purpose
 * -- it would put ~11 KiB on a 64 KiB fiber stack, and Linux gates AMX
 * behind an explicit per-process permission request.  Probed once (CPUID
 * and XGETBV are plain instructions, safe in the signal handler that
 * calls this); the size is published before the mask so a trampoline
 * that sees the mask also sees the size (x86 store order).
 */
extern uint64_t __xtc_preempt_xsave_mask __attribute__((visibility("hidden")));
extern uint64_t __xtc_preempt_xsave_size __attribute__((visibility("hidden")));
static _Atomic int g_xsave_probed;

/*
 * The interrupted PC, handed from the signal handler to the trampoline
 * on the same thread (sigreturn resumes the thread that took the
 * signal, and the trampoline reads it, via __xtc_preempt_take_pc,
 * before its first yield).  It used to be stored on the fiber stack at
 * orig_sp - 8 -- inside the interrupted code's red zone, which the ABI
 * promises a signal handler never touches.  A nested tick cannot
 * overwrite it: g_in_preempt stays raised until the trampoline's
 * xtc_yield, and the handler declines while it is.  The slot and its
 * reader live in preempt.c (see there for why).
 */
extern XTC_THREAD_LOCAL uintptr_t __xtc_preempt_pc
    __attribute__((visibility("hidden")));

#define XTC_XSAVE_WANT 0x2E7ull  /* x87 SSE AVX opmask ZMM_Hi256 Hi16_ZMM PKRU */

static void
__preempt_xsave_probe(void)
{
	unsigned a, b, c, d, i, lo, hi;
	uint64_t mask, size = 576;       /* legacy area + XSAVE header */

	if (atomic_load_explicit(&g_xsave_probed, memory_order_acquire))
		return;
	mask = 0;
	if (__get_cpuid(1, &a, &b, &c, &d) && (c & bit_OSXSAVE) != 0 &&
	    __get_cpuid_max(0, NULL) >= 0xD) {
		__asm__ volatile("xgetbv" : "=a"(lo), "=d"(hi) : "c"(0));
		mask = (((uint64_t)hi << 32) | lo) & XTC_XSAVE_WANT;
		for (i = 2; i < 10; i++) {
			if ((mask & (1ull << i)) == 0)
				continue;
			__cpuid_count(0xD, i, a, b, c, d);   /* a size, b offset */
			if ((uint64_t)a + b > size)
				size = (uint64_t)a + b;
		}
		size = (size + 63) & ~63ull;
	}
	__xtc_preempt_xsave_size = size;
	atomic_signal_fence(memory_order_release);
	__xtc_preempt_xsave_mask = mask;
	atomic_store_explicit(&g_xsave_probed, 1, memory_order_release);
}
#endif

#if defined(XTC_CORO_REDIRECT_X86_64)
/*
 * The x86-64 System V redirect, called from __xtc_coro_preempt with the
 * interrupted (signal-delivered) ucontext.  Returns 1 if it armed the
 * redirect, 0 if it declined.
 */
static int
__coro_preempt_redirect_x86_64(void *uctx)
{
	/*
	 * Phase 2b-arch, x86-64 System V: Go's async-preemption PC
	 * redirect.  We do NOT copy the (unsound-to-reuse) signal
	 * ucontext; we rewrite it so that on sigreturn the fiber resumes
	 * -- on its own stack, with its own real registers -- at
	 * __xtc_preempt_trampoline, which does a normal cooperative
	 * xtc_yield() and later returns to the interrupted PC.
	 *
	 * Only a real fiber can be preempted (the scheduler thread itself
	 * has no coro; nothing to yield).  The handler already checked
	 * crit_depth == 0 and unsafe_depth == 0.
	 *
	 * RE-ENTRANCY GUARD: a second timer tick that fires after this
	 * arms the redirect but before the trampoline has finished saving
	 * the interrupted register file would rewrite the same fiber's
	 * RIP/RSP a second time and corrupt c->ctx (verified: a rare
	 * SIGSEGV in the scheduler's swapcontext resume under aggressive
	 * slicing).  g_in_preempt closes that window: it is set here and
	 * cleared by the trampoline the instant its saves are complete
	 * (see __xtc_preempt_armed_clear), so a nested tick in the
	 * vulnerable window declines and falls back to the pending flag.
	 */
	extern void __xtc_preempt_trampoline(void);
	extern void __xtc_preempt_trampoline_end(void);
	ucontext_t *uc = (ucontext_t *)uctx;
	greg_t orig_pc, orig_sp, new_sp;

	if (__xtc_current_coro == NULL)
		return 0;   /* not in a fiber; leave the tick pending */
	if (g_in_preempt > 0)
		return 0;   /* inside a swapcontext or a redirect in flight */
	__preempt_xsave_probe();   /* once: what the trampoline must save */

	orig_pc = uc->uc_mcontext.gregs[REG_RIP];
	orig_sp = uc->uc_mcontext.gregs[REG_RSP];

	/*
	 * Decline if the fiber is executing inside the trampoline itself
	 * (prologue saving the register file, the xtc_yield call, or the
	 * epilogue restoring it): a nested redirect there would corrupt a
	 * half-saved/half-restored register file.  The epilogue runs with
	 * g_in_preempt already back to 0, so the flag alone cannot cover
	 * it -- this instruction-pointer range check does.
	 */
	if (orig_pc >= (greg_t)(uintptr_t)&__xtc_preempt_trampoline &&
	    orig_pc <  (greg_t)(uintptr_t)&__xtc_preempt_trampoline_end)
		return 0;
	/* Not a safe point: libc, another library, libxtc (preempt.c). */
	if (!__xtc_preempt_pc_ok((uintptr_t)orig_pc))
		return 0;

	/*
	 * Enter the trampoline BELOW the interrupted code's 128-byte red
	 * zone plus an 8-byte slot the trampoline fills with orig_pc, so its
	 * terminal `ret $128` resumes at orig_pc with RSP == orig_sp
	 * exactly.  Nothing at or above orig_sp - 128 is written: a leaf
	 * function may keep live data there across any instruction,
	 * asynchronous events included (SysV x86-64 ABI 3.2.2).
	 */
	new_sp = orig_sp - 136;
	__xtc_preempt_pc = (uintptr_t)orig_pc;

	g_in_preempt++;
	uc->uc_mcontext.gregs[REG_RSP] = new_sp;
	uc->uc_mcontext.gregs[REG_RIP] =
	    (greg_t)(uintptr_t)&__xtc_preempt_trampoline;
	return 1;   /* armed: sigreturn lands in the trampoline */
}
#endif

#if defined(XTC_CORO_REDIRECT_AARCH64)
#include <stddef.h>   /* offsetof */
/*
 * The AArch64 Linux redirect.  The kernel built the signal frame
 * (struct rt_sigframe: siginfo_t, then the ucontext_t `uctx` whose
 * uc_mcontext carries the FP/SIMD and SVE records) on the interrupted
 * fiber's stack, just below its sp, holding the WHOLE interrupted
 * state.  Leave the frame there: the handler's sigreturn then enters
 * __xtc_preempt_trampoline with sp = the frame; it calls xtc_yield and
 * issues a second rt_sigreturn on the same frame, which restores
 * everything exactly (preempt_trampoline_aarch64.S says why registers
 * are not saved by hand, and what it refreshes in the frame first).
 * The interrupted pc / sp / pstate wait in the frame's siginfo, which
 * rt_sigreturn never reads (stash[0..2]).  The trampoline is
 * entered with PSTATE.BTYPE cleared (a tick between an indirect branch
 * and its landing pad must not demand one at the trampoline); its
 * second sigreturn puts the interrupted pstate back.  It addresses the
 * frame with the offsets asserted here.
 */
_Static_assert(sizeof(siginfo_t) == 128, "rt_sigframe: siginfo size");
_Static_assert(offsetof(ucontext_t, uc_stack) == 16, "uc_stack");
_Static_assert(offsetof(ucontext_t, uc_sigmask) == 40, "uc_sigmask");
_Static_assert(offsetof(ucontext_t, uc_mcontext.sp) == 432, "mc sp");
_Static_assert(offsetof(ucontext_t, uc_mcontext.pc) == 440, "mc pc");
_Static_assert(offsetof(ucontext_t, uc_mcontext.pstate) == 448, "mc pstate");

#define CORO_A64_PSR_BTYPE 0xC00ull   /* PSTATE.BTYPE, bits 10-11 */

static int
__coro_preempt_redirect_aarch64(void *uctx)
{
	extern void __xtc_preempt_trampoline(void);
	extern void __xtc_preempt_trampoline_end(void);
	ucontext_t *uc = (ucontext_t *)uctx;
	struct xtc_coro *c = __xtc_current_coro;
	unsigned long long pc, frame, lo, hi, *stash;

	if (c == NULL)
		return 0;   /* not in a fiber; leave the tick pending */
	if (g_in_preempt > 0)
		return 0;   /* inside a switch or a redirect in flight */
	pc = uc->uc_mcontext.pc;
	if (pc >= (unsigned long long)(uintptr_t)&__xtc_preempt_trampoline &&
	    pc < (unsigned long long)(uintptr_t)&__xtc_preempt_trampoline_end)
		return 0;
	if (!__xtc_preempt_pc_ok((uintptr_t)pc))
		return 0;   /* not a safe point (preempt.c) */
	/* The frame starts with the siginfo; the ucontext follows it.  It
	 * must be the kernel's frame on THIS fiber's stack, below the
	 * interrupted sp (anything else, say a handler on an alternate
	 * stack, declines). */
	frame = (unsigned long long)(uintptr_t)uctx - sizeof(siginfo_t);
	lo = (unsigned long long)(uintptr_t)c->stack;
	hi = lo + c->stack_sz + 65536;   /* + the guard: at most a 64 KiB page */
	if ((frame & 15) != 0 || frame >= uc->uc_mcontext.sp || frame <= lo ||
	    frame >= hi)
		return 0;
	stash = (unsigned long long *)(uintptr_t)frame;
	stash[0] = pc;
	stash[1] = uc->uc_mcontext.sp;
	stash[2] = uc->uc_mcontext.pstate;

	g_in_preempt++;   /* the trampoline's xtc_yield clears it on resume */
	uc->uc_mcontext.pstate &= ~CORO_A64_PSR_BTYPE;
	uc->uc_mcontext.sp = frame;
	uc->uc_mcontext.pc =
	    (unsigned long long)(uintptr_t)&__xtc_preempt_trampoline;
	return 1;   /* armed: sigreturn lands in the trampoline */
}
#endif

#endif /* XTC_CORO_PREEMPT_H */
