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
	(void)uctx;
	atomic_fetch_add_explicit(&g_pt.ticks, 1, memory_order_relaxed);
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
