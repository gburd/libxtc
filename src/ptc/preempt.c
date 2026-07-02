/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * src/ptc/preempt.c
 *	Per-worker preemption timer seam (M_PREEMPTION Phase 0).
 *
 *	A per-thread interval timer (POSIX timer_create + SIGVTALRM,
 *	counting CPU time so a busy fiber is timed, not wall time) that a
 *	worker thread arms.  In Phase 0 the handler does the MINIMUM: it
 *	increments a per-thread tick counter and sets a per-thread
 *	"tick pending" flag.  No preemption happens yet -- later phases
 *	(cooperative-assisted, then signal-context involuntary yield)
 *	build on this seam.
 *
 *	OFF BY DEFAULT: nothing arms the timer unless xtc_preempt_arm is
 *	called (the executor opts in via config), so the cooperative fast
 *	path is byte-unchanged in a normal build/run.
 *
 *	Portability: POSIX timers (Linux/BSD/illumos).  On a platform
 *	without them (Windows, macOS lacks CPU-time POSIX timers cleanly)
 *	the arm/disarm are no-ops and xtc_preempt_supported() returns 0;
 *	a later phase adds a timer-queue / watchdog-thread variant.
 */

#include "xtc_int.h"
#include "xtc_preempt.h"

#include <signal.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>

#if defined(__linux__) || defined(__FreeBSD__) || defined(__NetBSD__) || \
    defined(__OpenBSD__) || defined(__DragonFly__) || defined(__sun) || \
    defined(_AIX)
# define XTC_HAVE_POSIX_TIMERS 1
#endif

/*
 * Per-thread preemption state.  Thread-local because each worker owns
 * its own timer and its own tick bookkeeping; a signal delivered to a
 * worker thread reads/writes only that thread's state.
 */
typedef struct {
	int              armed;        /* 1 if this thread has a live timer */
#if defined(XTC_HAVE_POSIX_TIMERS)
	timer_t          timer;        /* the per-thread timer */
	int              have_timer;   /* 1 if `timer` is created */
#endif
	_Atomic uint64_t ticks;        /* total timer ticks observed */
	_Atomic int      pending;      /* a tick fired, not yet consumed */
} preempt_tls_t;

static XTC_THREAD_LOCAL preempt_tls_t g_pt;

/* The timer signal.  SIGVTALRM pairs with ITIMER_VIRTUAL / a
 * CLOCK_THREAD_CPUTIME timer: it fires on CPU time consumed by THIS
 * thread, so a busy fiber accrues ticks and an idle worker does not.
 * Distinct from the fault signals (SIGSEGV/BUS/FPE/ILL) so the two
 * handlers never collide. */
#define XTC_PREEMPT_SIGNAL SIGVTALRM

static _Atomic int g_handler_installed;

/* Phase 2 (signal-context involuntary yield) opt-in.  When 0 (the
 * default, and Phase 1) the handler only records a tick and the yield
 * happens cooperatively at the next xtc_yield_if_due.  When 1, the
 * handler ALSO performs a resumable involuntary yield when it is safe.
 * A separate flag so Phase 1 users are never exposed to signal-context
 * stack redirection. */
static _Atomic int g_involuntary_on;

/* Provided by the active coroutine substrate (coro_uctx.c does the real
 * resumable redirect; coro_fctx.c / coro_winfiber.c decline and return
 * 0) and by proc.c.  Forward-declared to keep preempt.c (L3) from
 * pulling the substrate headers. */
extern int __xtc_coro_preempt(void *uctx);
extern int __xtc_proc_crit_depth(void);

/*
 * Per-thread async-signal-unsafe-region nesting depth (see the
 * __xtc_unsafe_* section below).  Declared here because the timer
 * handler reads it.  volatile so the handler's read is not hoisted;
 * plain int because only this thread writes it and the handler runs on
 * this same thread (a transient value only ever errs toward "defer"). */
static XTC_THREAD_LOCAL volatile int g_unsafe_depth;

#if defined(XTC_HAVE_POSIX_TIMERS)

/*
 * The timer handler.  Async-signal-safe: it only does relaxed atomic
 * stores to this thread's own state (atomics on lock-free types are on
 * the async-signal-safe list; no allocation, no non-reentrant libc).
 * Phase 0: record the tick.  Later phases consult crit_depth /
 * unsafe_depth here and either defer (set pending) or perform the
 * involuntary yield.
 */
static void
preempt_handler(int sig, siginfo_t *si, void *uctx)
{
	(void)sig;
	(void)si;
	atomic_fetch_add_explicit(&g_pt.ticks, 1, memory_order_relaxed);
	/*
	 * Phase 2: a signal-context involuntary yield, but ONLY when it is
	 * safe -- not inside a critical section (crit_depth) and not inside
	 * an async-signal-unsafe region (unsafe_depth: the allocator, a
	 * latch's internal lock).  This is libas-safe's discipline realized
	 * via counters: we never redirect the stack out of malloc or a
	 * lock.  __xtc_coro_preempt does the resumable redirect on the
	 * ucontext substrate (Go's mechanism -- the kernel swaps context on
	 * sigreturn) and declines on fctx/winfiber, in which case we fall
	 * back to the cooperative pending flag.  g_unsafe_depth is read
	 * directly (this handler runs on the same thread that maintains
	 * it). */
	if (atomic_load_explicit(&g_involuntary_on, memory_order_relaxed) &&
	    g_unsafe_depth == 0 &&
	    __xtc_proc_crit_depth() == 0 &&
	    __xtc_coro_preempt(uctx)) {
		/* The involuntary yield is armed: on sigreturn the scheduler
		 * resumes and re-queues this fiber.  No pending flag needed. */
		return;
	}
	/* Unsafe, disabled, or no fiber to preempt: defer to the next
	 * cooperative yield-check (Phase 1). */
	atomic_store_explicit(&g_pt.pending, 1, memory_order_relaxed);
}

static void
ensure_handler_installed(void)
{
	struct sigaction sa;
	if (atomic_exchange_explicit(&g_handler_installed, 1,
	    memory_order_acq_rel))
		return;
	memset(&sa, 0, sizeof sa);
	sa.sa_sigaction = preempt_handler;
	/* No SA_ONSTACK: the involuntary-yield redirect only rewrites the
	 * kernel-saved *uctx and returns; it needs no altstack, and running
	 * the tiny handler on the interrupted fiber's stack is fine (its
	 * frame is below the resume point that c->ctx captured, and is
	 * abandoned when sigreturn restores the scheduler context). */
	sa.sa_flags = SA_SIGINFO | SA_RESTART;
	(void)sigemptyset(&sa.sa_mask);
	(void)sigaction(XTC_PREEMPT_SIGNAL, &sa, NULL);
}

/* PUBLIC: int xtc_preempt_arm __P((int64_t)); */
int
xtc_preempt_arm(int64_t interval_ns)
{
	struct sigevent sev;
	struct itimerspec its;

	if (interval_ns <= 0)
		return XTC_E_INVAL;
	if (g_pt.armed)
		(void)xtc_preempt_disarm();

	ensure_handler_installed();

	/* A CPU-time (per-thread) timer: SIGEV_THREAD_ID would target this
	 * thread, but the portable form is a per-thread CLOCK using
	 * SIGEV_SIGNAL delivered to the process and steered by the timer
	 * being thread-CPU-clocked.  We use CLOCK_THREAD_CPUTIME_ID so the
	 * timer measures THIS thread's CPU time. */
	memset(&sev, 0, sizeof sev);
	sev.sigev_notify = SIGEV_SIGNAL;
	sev.sigev_signo = XTC_PREEMPT_SIGNAL;
	if (timer_create(CLOCK_THREAD_CPUTIME_ID, &sev, &g_pt.timer) != 0)
		return XTC_E_NOSYS;
	g_pt.have_timer = 1;

	memset(&its, 0, sizeof its);
	its.it_interval.tv_sec = interval_ns / 1000000000LL;
	its.it_interval.tv_nsec = interval_ns % 1000000000LL;
	its.it_value = its.it_interval;   /* first fire after one interval */
	if (timer_settime(g_pt.timer, 0, &its, NULL) != 0) {
		(void)timer_delete(g_pt.timer);
		g_pt.have_timer = 0;
		return XTC_E_NOSYS;
	}
	g_pt.armed = 1;
	atomic_store_explicit(&g_pt.pending, 0, memory_order_relaxed);
	return XTC_OK;
}

/* PUBLIC: int xtc_preempt_disarm __P((void)); */
int
xtc_preempt_disarm(void)
{
	if (g_pt.have_timer) {
		struct itimerspec its;
		memset(&its, 0, sizeof its);
		(void)timer_settime(g_pt.timer, 0, &its, NULL);  /* stop */
		(void)timer_delete(g_pt.timer);
		g_pt.have_timer = 0;
	}
	g_pt.armed = 0;
	atomic_store_explicit(&g_pt.pending, 0, memory_order_relaxed);
	return XTC_OK;
}

/* PUBLIC: int xtc_preempt_supported __P((void)); */
int
xtc_preempt_supported(void)
{
	return 1;
}

#else  /* no POSIX timers: no-op seam */

/* PUBLIC: int xtc_preempt_arm __P((int64_t)); */
int
xtc_preempt_arm(int64_t interval_ns)
{
	(void)interval_ns;
	return XTC_E_NOSYS;
}

/* PUBLIC: int xtc_preempt_disarm __P((void)); */
int
xtc_preempt_disarm(void)
{
	g_pt.armed = 0;
	return XTC_OK;
}

/* PUBLIC: int xtc_preempt_supported __P((void)); */
int
xtc_preempt_supported(void)
{
	return 0;
}

#endif

/* PUBLIC: void xtc_preempt_set_involuntary __P((int)); */
/*
 * Enable (on != 0) or disable signal-context involuntary yield (Phase
 * 2).  When enabled AND a per-worker timer is armed, a tick preempts
 * the running fiber in the handler -- resumably, and only when safe
 * (crit_depth == 0, unsafe_depth == 0) -- on the ucontext substrate;
 * on the fctx/winfiber substrate the involuntary yield declines and
 * the timer falls back to Phase 1 cooperative-assisted preemption.
 * Process-global (it flips the shared handler's behavior).  Off by
 * default; Phase 1 users are never exposed to signal-context stack
 * redirection unless they opt in here. */
void
xtc_preempt_set_involuntary(int on)
{
	atomic_store_explicit(&g_involuntary_on, on ? 1 : 0,
	    memory_order_relaxed);
}

/* PUBLIC: uint64_t xtc_preempt_ticks __P((void)); */
/* Total timer ticks this thread has observed since arming.  The Phase 0
 * signal-that-the-seam-works metric. */
uint64_t
xtc_preempt_ticks(void)
{
	return atomic_load_explicit(&g_pt.ticks, memory_order_relaxed);
}

/* PUBLIC: int xtc_preempt_tick_pending __P((void)); */
/* 1 if a timer tick fired and has not been consumed; clears the flag.
 * Phase 1 will call this at safe points to decide whether to yield. */
int
xtc_preempt_tick_pending(void)
{
	return atomic_exchange_explicit(&g_pt.pending, 0, memory_order_relaxed);
}

/* ---- async-signal-unsafe-region counter (Phase 2 prerequisite) ----
 *
 * A per-thread nesting depth that is > 0 while this thread is inside an
 * async-signal-UNSAFE region -- the allocator (malloc/free) and the
 * brief internal-lock windows of the latches.  The preemption timer
 * handler (Phase 2) MUST NOT perform a signal-context involuntary yield
 * while this depth is > 0, because jumping the stack out of malloc's
 * arena lock or a latch's pthread_mutex would corrupt state; it defers
 * (leaves the tick pending) instead.  This realizes libas-safe's
 * discipline via a counter rather than rewriting the unsafe code.
 *
 * g_unsafe_depth is declared near the top of the file (the handler
 * needs it).  It is a plain volatile int (not atomic): only THIS thread
 * writes it, and the timer signal handler -- which runs ON this same
 * thread -- reads it; enter increments BEFORE the unsafe op and leave
 * decrements AFTER, so the handler seeing a transient value only ever
 * errs toward "unsafe" (defer), never toward a wrongful yield.  The
 * fault handler (proc.c) can consult it too, so a SIGSEGV inside malloc
 * no longer siglongjmps out of a corrupt arena.
 */

/* PUBLIC: void __xtc_unsafe_enter __P((void)); */
void
__xtc_unsafe_enter(void)
{
	g_unsafe_depth++;
}

/* PUBLIC: void __xtc_unsafe_leave __P((void)); */
void
__xtc_unsafe_leave(void)
{
	if (g_unsafe_depth > 0)
		g_unsafe_depth--;
}

/* PUBLIC: int __xtc_unsafe_depth __P((void)); */
int
__xtc_unsafe_depth(void)
{
	return g_unsafe_depth;
}

/*
 * __xtc_mtx_lock / __xtc_mtx_unlock (declared in xtc_preempt.h):
 *
 * Preemption-safe raw-pthread mutex acquire.  A fiber that holds a
 * mutex must not be involuntarily preempted (M_PREEMPTION Phase 2b): a
 * loop runs many fibers on one OS thread, so a holder preempted
 * mid-hold plus another same-loop fiber blocking on the same mutex
 * deadlocks the thread.  Bracketing with __xtc_unsafe_enter/leave
 * makes the preemption timer defer to the cooperative path while the
 * lock is held.  This is the raw-pthread counterpart of the
 * __os_mutex_* brackets, for the many internal subsystems that embed a
 * bare pthread_mutex_t in their structures.  It must ONLY be used for
 * short critical sections that never yield/park while the lock is held
 * (a yield inside the bracket would strand the elevated unsafe depth on
 * a different fiber).
 */
int
__xtc_mtx_lock(pthread_mutex_t *m)
{
	int e;
	__xtc_unsafe_enter();
	e = pthread_mutex_lock(m);
	if (e != 0)
		__xtc_unsafe_leave();   /* not held; balance the enter */
	return e;
}

/*
 * __xtc_mtx_unlock: preemption-safe raw-pthread mutex release; balances
 * __xtc_mtx_lock.
 */
int
__xtc_mtx_unlock(pthread_mutex_t *m)
{
	int e = pthread_mutex_unlock(m);
	__xtc_unsafe_leave();
	return e;
}
