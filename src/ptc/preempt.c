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
 *	without them (Windows) the arm/disarm are no-ops and
 *	xtc_preempt_supported() returns 0.
 *
 *	macOS (__APPLE__): there is no per-thread CPU-time timer_create, so
 *	Phase 1 (the tick source) is instead driven by a dedicated kqueue
 *	EVFILT_TIMER on a private per-thread kqueue (see
 *	XTC_HAVE_KQUEUE_TIMER below): xtc_preempt_arm spins up one small
 *	helper thread that blocks in kevent() and, on every timer fire,
 *	pthread_kill()s the ARMING thread with XTC_PREEMPT_SIGNAL -- so the
 *	EXISTING preempt_handler below (unchanged, already fully portable
 *	POSIX signal-handler code) runs on the arming thread exactly as it
 *	does on Linux, updating the same g_pt.ticks / g_pt.pending state.
 *	Every consumer above this file (exec.c's xtc_yield_check,
 *	xtc_preempt_ticks/tick_pending) therefore needs zero macOS-specific
 *	code.  One real, documented behavior difference: kqueue has no
 *	CPU-time filter, so the macOS tick source is WALL-CLOCK, not
 *	CPU-time -- an idle macOS worker still ticks, where an idle
 *	Linux/BSD worker does not (see xtc_preempt.3).  Phase 2 (the
 *	signal-context involuntary yield) stays Linux-only: it is gated
 *	inside the coroutine substrate (coro_uctx.c's __xtc_coro_preempt,
 *	owned by a different layer than this file), which already declines
 *	on __APPLE__, so a tick delivered this way on macOS always falls
 *	back to Phase 1 cooperative-assisted preemption -- correct, and no
 *	new gating needed here.
 */

/* Define the Darwin feature macro before any system header (including
 * xtc_int.h, which transitively pulls in stddef.h/stdint.h and thus
 * <sys/cdefs.h> -- the macOS feature-macro gate only takes effect if
 * set before THAT first inclusion) so the full BSD <sys/event.h>
 * surface (NOTE_NSECONDS et al.) is visible later in this file.  Same
 * pattern, same rationale, as src/io/io_kqueue.c. */
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE 1
#endif

/* Linux: the SIGEV_THREAD_ID target LWP id (used below to deliver the
 * preemption signal to the exact arming worker thread) is the sigevent
 * union member _sigev_un._tid, a glibc extension gated behind
 * _GNU_SOURCE, which must be set before the first system header. */
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE 1
#endif

#include "xtc_int.h"
#include "xtc_preempt.h"
#include "preempt_int.h"

#include <signal.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <time.h>
#if defined(__linux__)
#include <sys/syscall.h>   /* SYS_gettid for SIGEV_THREAD_ID targeting */
#include <unistd.h>
#endif

#if defined(__linux__) || defined(__FreeBSD__) || defined(__NetBSD__) || \
    defined(__OpenBSD__) || defined(__DragonFly__) || defined(__sun) || \
    defined(_AIX)
# define XTC_HAVE_POSIX_TIMERS 1
#endif

#if defined(__linux__) && defined(SIGEV_THREAD_ID)
# define XTC_HAVE_SIGEV_THREAD_ID 1
/* The public POSIX-ish accessor sigev_notify_thread_id exists on musl
 * and newer glibc; older glibc (e.g. 2.40) omits the macro but exposes
 * the same field as the union member _sigev_un._tid.  Prefer the macro,
 * fall back to the glibc-internal spelling. */
# ifdef sigev_notify_thread_id
#  define XTC_SIGEV_SET_TID(sev, tid) ((sev).sigev_notify_thread_id = (tid))
# else
#  define XTC_SIGEV_SET_TID(sev, tid) ((sev)._sigev_un._tid = (tid))
# endif
#endif

/* macOS: no per-thread CPU-time timer_create.  The tick source is a
 * kqueue EVFILT_TIMER on a private per-thread kqueue instead (see the
 * big block comment above and the XTC_HAVE_KQUEUE_TIMER branch below).
 * kqueue is already the L1 io backend on macOS/BSD (src/io/io_kqueue.c)
 * -- this reuses that same OS facility rather than introducing GCD or
 * any other second event mechanism.  Deliberately Apple-only: the
 * other kqueue platforms (FreeBSD/NetBSD/OpenBSD/DragonFly) already
 * have real per-thread CPU-time POSIX timers via XTC_HAVE_POSIX_TIMERS
 * above, so they never need this fallback.  (_DARWIN_C_SOURCE, which
 * exposes NOTE_NSECONDS and the rest of the BSD <sys/event.h> surface
 * on the macOS SDK headers, is defined above -- before xtc_int.h's
 * transitive first system-header pull -- exactly as io_kqueue.c does
 * it.) */
#if defined(__APPLE__)
# define XTC_HAVE_KQUEUE_TIMER 1
# include <sys/event.h>
# include <sys/time.h>
# include <errno.h>
# include <unistd.h>
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

#if defined(XTC_HAVE_POSIX_TIMERS) || defined(XTC_HAVE_KQUEUE_TIMER)

/*
 * preempt_handler / ensure_handler_installed are SHARED by every tick
 * source (the POSIX per-thread CPU-time timer below, and the macOS
 * kqueue EVFILT_TIMER branch further down): whichever source delivers
 * XTC_PREEMPT_SIGNAL to this thread, it runs this same handler and
 * updates the same g_pt state.  Widening this guard to also compile
 * under XTC_HAVE_KQUEUE_TIMER (Apple-only) does not change what is
 * compiled on Linux/BSD/illumos/AIX at all -- XTC_HAVE_KQUEUE_TIMER is
 * never defined there, so the condition reduces to exactly
 * XTC_HAVE_POSIX_TIMERS, unchanged.
 */

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

#endif /* XTC_HAVE_POSIX_TIMERS || XTC_HAVE_KQUEUE_TIMER */

#if defined(XTC_HAVE_POSIX_TIMERS)

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

	/* Unblock the preemption signal on THIS thread.  Runtime threads are
	 * created with all signals blocked (__os_pthread_create_masked) so a
	 * process-directed signal never lands on them; but a thread that
	 * EXPLICITLY arms preemption wants its own per-thread timer signal
	 * delivered, so it opts back in here.  Without this the SIGVTALRM
	 * from the CPU-time timer would stay blocked and preemption never
	 * fires. */
#if !defined(_WIN32)
	{
		sigset_t s;
		sigemptyset(&s);
		sigaddset(&s, XTC_PREEMPT_SIGNAL);
		(void)pthread_sigmask(SIG_UNBLOCK, &s, NULL);
	}
#endif

	/* A CPU-time (per-thread) timer measuring THIS thread's CPU time
	 * (CLOCK_THREAD_CPUTIME_ID).  On Linux, deliver via SIGEV_THREAD_ID
	 * so the signal is guaranteed to land on the ARMING worker thread --
	 * the one actually burning the CPU time -- not on an arbitrary
	 * thread the kernel picks for a process-directed SIGEV_SIGNAL.  The
	 * old SIGEV_SIGNAL form set the thread-local `pending` flag on
	 * whichever thread the kernel happened to deliver to, so on a busy
	 * single-loop run the compute thread's xtc_yield_if_due could never
	 * see "due" (observed as test_preempt_p1's g_yields==0 flaking on
	 * some VMs while passing on others -- a real thread-targeting bug,
	 * not merely an environment quirk).  SIGEV_THREAD_ID makes it
	 * deterministic.  Non-Linux keeps the portable SIGEV_SIGNAL form. */
	memset(&sev, 0, sizeof sev);
#if defined(XTC_HAVE_SIGEV_THREAD_ID)
	sev.sigev_notify = SIGEV_THREAD_ID;
	sev.sigev_signo = XTC_PREEMPT_SIGNAL;
	/* Deliver to the exact arming worker thread (its LWP id); the field
	 * spelling differs across libcs -- XTC_SIGEV_SET_TID bridges it. */
	XTC_SIGEV_SET_TID(sev, (int)syscall(SYS_gettid));
#else
	sev.sigev_notify = SIGEV_SIGNAL;
	sev.sigev_signo = XTC_PREEMPT_SIGNAL;
#endif
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
	/* Runtime probe, cached: the POSIX-timer headers may be present
	 * (XTC_HAVE_POSIX_TIMERS) yet timer_create(CLOCK_THREAD_CPUTIME_ID)
	 * still fail on a given host -- e.g. some FreeBSD configurations do
	 * not back a per-thread CPU-time timer.  Probe once so
	 * xtc_preempt_supported() and xtc_preempt_arm() agree: a compile-
	 * time yes that arm() then refuses with NOSYS made a caller (and
	 * test_preempt) assert on the mismatch. */
	static _Atomic int cached = -1;   /* -1 unknown, 0 no, 1 yes */
	int c = atomic_load_explicit(&cached, memory_order_relaxed);
	if (c < 0) {
		struct sigevent sev;
		timer_t t;
		memset(&sev, 0, sizeof sev);
#if defined(XTC_HAVE_SIGEV_THREAD_ID)
		sev.sigev_notify = SIGEV_THREAD_ID;
		sev.sigev_signo = XTC_PREEMPT_SIGNAL;
		XTC_SIGEV_SET_TID(sev, (int)syscall(SYS_gettid));
#else
		sev.sigev_notify = SIGEV_SIGNAL;
		sev.sigev_signo = XTC_PREEMPT_SIGNAL;
#endif
		if (timer_create(CLOCK_THREAD_CPUTIME_ID, &sev, &t) == 0) {
			(void)timer_delete(t);
			c = 1;
		} else {
			c = 0;
		}
		atomic_store_explicit(&cached, c, memory_order_relaxed);
	}
	return c;
}

#elif defined(XTC_HAVE_KQUEUE_TIMER)  /* macOS: kqueue EVFILT_TIMER tick source */

/*
 * macOS has no per-thread CPU-time timer_create, so the Phase 1 tick
 * source is a kqueue EVFILT_TIMER instead (see the file-header comment
 * for the full rationale).  Design, spelled out:
 *
 *   - xtc_preempt_arm creates a PRIVATE kqueue (kqueue(2)) plus one
 *     periodic EVFILT_TIMER registered on it (NOTE_NSECONDS so the
 *     interval matches the ns-granularity public API exactly), then
 *     spawns one small helper pthread that just blocks in kevent() and
 *     pthread_kill()s the ARMING thread with XTC_PREEMPT_SIGNAL every
 *     time the timer fires.
 *   - The signal lands on the arming thread and runs the EXACT SAME
 *     preempt_handler() defined above (shared, unchanged): it bumps
 *     g_pt.ticks and (Phase 2, if enabled) tries __xtc_coro_preempt.
 *     coro_uctx.c's __xtc_coro_preempt already declines on __APPLE__
 *     (see its own #if), so this always falls back to the Phase 1
 *     pending flag on macOS -- correct, no new gating needed here.
 *   - A dedicated kqueue per armed thread (not the loop's own L1 io
 *     kqueue) keeps this seam decoupled from xtc_io_t's lifecycle: the
 *     timer thread outlives individual io-poll calls and is torn down
 *     independently by xtc_preempt_disarm.  This is the same choice
 *     the file already makes on Linux (its own timer_t, not tied to
 *     any io object).
 *
 *   Behavior difference from the POSIX-timer path (documented in the
 *   file header and xtc_preempt.3): EVFILT_TIMER counts WALL-CLOCK
 *   time, not this thread's CPU time (kqueue has no CPU-time filter).
 *   An idle macOS worker still ticks; ticks are therefore a weaker
 *   "time slice elapsed" signal there than on Linux/BSD's "CPU time
 *   consumed" signal.  Phase 1 (xtc_yield_check) treats a tick the
 *   same way regardless of source, so this is purely a documented
 *   fairness/telemetry nuance, not a correctness gap.
 */

/* Per-thread kqueue-timer state, alongside preempt_tls_t's `armed`
 * flag.  Kept in its own struct (not folded into preempt_tls_t) so the
 * POSIX-timer branch's layout above is completely untouched -- that
 * struct's #if defined(XTC_HAVE_POSIX_TIMERS) member is compiled out
 * here since XTC_HAVE_POSIX_TIMERS is never defined together with
 * XTC_HAVE_KQUEUE_TIMER. */
typedef struct {
	int        kq;          /* the private kqueue fd, or -1 */
	pthread_t  helper;      /* the timer-poll helper thread */
	int        have_helper; /* 1 if `helper` was created */
	pthread_t  target;      /* the arming thread: pthread_kill's target */
} kq_timer_tls_t;

static XTC_THREAD_LOCAL kq_timer_tls_t g_kqt = { -1, 0, 0, 0 };

/* Two disjoint EVFILT_TIMER/EVFILT_USER idents on the SAME private
 * kqueue -- the pattern io_kqueue.c also uses (a fixed EVFILT_USER
 * ident alongside per-fd EVFILT_READ/WRITE registrations) to fold a
 * cross-thread wakeup into one kevent() namespace: EVFILT_TIMER and
 * EVFILT_USER are separate filter namespaces in kqueue, so these two
 * idents never collide even though both equal small integers. */
#define XTC_KQ_TIMER_IDENT ((uintptr_t)1)
#define XTC_KQ_STOP_IDENT  ((uintptr_t)2)

/*
 * The helper thread: blocks in kevent() on the private kqueue for
 * EITHER the periodic EVFILT_TIMER or the EVFILT_USER stop event:
 *   - on a timer fire, pthread_kill()s the arming thread;
 *   - on the stop event (posted by xtc_preempt_disarm via NOTE_TRIGGER,
 *     the same live-kqueue wakeup idiom __xtc_io_kqueue_wakeup_post
 *     uses in io_kqueue.c), returns.
 * Waking via an EVFILT_USER NOTE_TRIGGER rather than closing the
 * kqueue out from under a blocked kevent() call avoids a close-vs-
 * blocked-syscall race: the fd stays open and valid for the helper's
 * entire lifetime, and disarm only closes it AFTER the join.
 */
static void *
kq_timer_helper(void *arg)
{
	kq_timer_tls_t *st = (kq_timer_tls_t *)arg;
	for (;;) {
		struct kevent ev;
		int n = kevent(st->kq, NULL, 0, &ev, 1, NULL);
		if (n < 0) {
			if (errno == EINTR)
				continue;
			break;              /* kqueue gone: exit defensively */
		}
		if (n == 0)
			continue;
		if (ev.filter == EVFILT_USER && ev.ident == XTC_KQ_STOP_IDENT)
			break;
		if (ev.filter == EVFILT_TIMER)
			(void)pthread_kill(st->target, XTC_PREEMPT_SIGNAL);
	}
	return NULL;
}

/* PUBLIC: int xtc_preempt_arm __P((int64_t)); */
int
xtc_preempt_arm(int64_t interval_ns)
{
	struct kevent kev[2];

	if (interval_ns <= 0)
		return XTC_E_INVAL;
	if (g_pt.armed)
		(void)xtc_preempt_disarm();

	ensure_handler_installed();

	/* Same rationale as the POSIX-timer branch: opt this thread's mask
	 * back in for XTC_PREEMPT_SIGNAL, since runtime threads start with
	 * every signal blocked. */
	{
		sigset_t s;
		sigemptyset(&s);
		sigaddset(&s, XTC_PREEMPT_SIGNAL);
		(void)pthread_sigmask(SIG_UNBLOCK, &s, NULL);
	}

	g_kqt.kq = kqueue();
	if (g_kqt.kq < 0)
		return XTC_E_NOSYS;

	/* NOTE_NSECONDS: register the interval in the same ns unit the
	 * public API takes, no lossy us/ms rounding.  EV_ADD with no
	 * EV_ONESHOT/EV_DISPATCH -- the default kqueue timer is periodic,
	 * re-firing every `interval_ns` until EV_DELETE or the kqueue is
	 * closed.  The EVFILT_USER stop event is armed alongside it
	 * (EV_CLEAR: it auto-resets after delivery -- the same shape
	 * io_kqueue.c's own wakeup event uses). */
	EV_SET(&kev[0], XTC_KQ_TIMER_IDENT, EVFILT_TIMER, EV_ADD | EV_ENABLE,
	    NOTE_NSECONDS, (intptr_t)interval_ns, NULL);
	EV_SET(&kev[1], XTC_KQ_STOP_IDENT, EVFILT_USER, EV_ADD | EV_CLEAR,
	    0, 0, NULL);
	if (kevent(g_kqt.kq, kev, 2, NULL, 0, NULL) < 0) {
		(void)close(g_kqt.kq);
		g_kqt.kq = -1;
		return XTC_E_NOSYS;
	}

	g_kqt.target = pthread_self();
	if (__os_pthread_create_masked(&g_kqt.helper, kq_timer_helper, &g_kqt)
	    != 0) {
		(void)close(g_kqt.kq);
		g_kqt.kq = -1;
		return XTC_E_NOSYS;
	}
	g_kqt.have_helper = 1;

	g_pt.armed = 1;
	atomic_store_explicit(&g_pt.pending, 0, memory_order_relaxed);
	return XTC_OK;
}

/* PUBLIC: int xtc_preempt_disarm __P((void)); */
int
xtc_preempt_disarm(void)
{
	if (g_kqt.have_helper) {
		/* Wake the helper's blocked kevent() with the EVFILT_USER stop
		 * event (NOTE_TRIGGER) rather than closing the kqueue fd out from
		 * under it -- the same live-kqueue wakeup idiom
		 * __xtc_io_kqueue_wakeup_post uses in io_kqueue.c, and it avoids a
		 * close()-races-with-a-blocked-syscall hazard entirely. */
		struct kevent kev;
		EV_SET(&kev, XTC_KQ_STOP_IDENT, EVFILT_USER, 0, NOTE_TRIGGER,
		    0, NULL);
		(void)kevent(g_kqt.kq, &kev, 1, NULL, 0, NULL);
		(void)pthread_join(g_kqt.helper, NULL);  /* XTC_BLOCKING_OK: joining the kqueue timer helper thread on teardown, not on a loop */
		g_kqt.have_helper = 0;
	}
	if (g_kqt.kq >= 0) {
		(void)close(g_kqt.kq);
		g_kqt.kq = -1;
	}
	g_pt.armed = 0;
	atomic_store_explicit(&g_pt.pending, 0, memory_order_relaxed);
	return XTC_OK;
}

/* PUBLIC: int xtc_preempt_supported __P((void)); */
int
xtc_preempt_supported(void)
{
	/* Runtime probe, cached, mirroring the POSIX-timer branch: kqueue()
	 * itself can fail (e.g. an fd-table limit), so probe once rather
	 * than assume the compile-time XTC_HAVE_KQUEUE_TIMER guarantees a
	 * live kqueue at runtime. */
	static _Atomic int cached = -1;
	int c = atomic_load_explicit(&cached, memory_order_relaxed);
	if (c < 0) {
		int kq = kqueue();
		if (kq >= 0) {
			(void)close(kq);
			c = 1;
		} else {
			c = 0;
		}
		atomic_store_explicit(&cached, c, memory_order_relaxed);
	}
	return c;
}

#else  /* no POSIX timers and no kqueue timer: no-op seam */

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
