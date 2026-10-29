/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/ptc/sync.c
 *	M9 sync primitives.  All of these compose with the existing
 *	xtc_proc receive infrastructure: a waiter parks via an internal
 *	mailbox-shaped signal and is woken either by an explicit signal
 *	or by a timeout.  We piggyback on xtc_recv when called from
 *	inside a process, and on a thread-condvar for callers from
 *	outside any process (test main threads, tooling).
 */

#include "xtc_int.h"
#include "preempt_int.h"   /* __xtc_unsafe_* / __xtc_mtx_*: internal preemption brackets */
#include "xtc_sync.h"
#include "xtc_proc.h"
#include "proc_int.h"   /* __xtc_proc_ctx_save/restore (internal) */
#include "xtc_sim.h"       /* XTC_SIM_BUGGIFY / xtc_sim_fault (DST) */
#include "loop_int.h"
#include "coro_int.h"
#include "xtc_probes.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* -------------------------------------------------------------------------
 * Off-loop condvar wait helpers.
 *
 * Every blocking primitive here (notify, semaphore, mutex, rwlock,
 * latch, ...) shares the same timed-wait shape for callers outside any
 * process: compute an absolute deadline from a relative timeout, then
 * loop pthread_cond_timedwait while the wake predicate is unmet,
 * returning XTC_E_AGAIN once the deadline passes.  Factor the two
 * mechanical pieces out so each primitive keeps only its own predicate.
 *
 * deadline_from_timeout fills *ts with now + timeout_ns (CLOCK_REALTIME,
 * the clock pthread_cond_timedwait uses).  cv_wait_until does one
 * bounded wait against that deadline and returns 0 if the cond was
 * signaled (the caller re-tests its predicate) or 1 if the deadline
 * has passed (the caller maps that to XTC_E_AGAIN).
 * ----------------------------------------------------------------------- */
static void
deadline_from_timeout(int64_t timeout_ns, struct timespec *ts)
{
	int64_t now = 0;
	(void)__os_clock_real(&now);
	now += timeout_ns;
	ts->tv_sec  = (time_t)(now / 1000000000LL);
	ts->tv_nsec = (long)(now % 1000000000LL);
}

static int
cv_wait_until(pthread_cond_t *cv, pthread_mutex_t *lock,
    const struct timespec *ts)
{
	/* 0 == signaled (re-test the predicate); 1 == deadline reached. */
	return pthread_cond_timedwait(cv, lock, ts) == 0 ? 0 : 1;
}

/* -------------------------------------------------------------------------
 * Off-loop spin-then-park (xtc_amutex, xtc_arwlock).
 *
 * A plain thread that finds the lock held used to sleep on the condvar at
 * once.  With short holds that is a convoy: the lock is free again long
 * before the sleeper is scheduled, so it is handed only to threads that
 * pay a futex sleep + wake each (measured, bench_sync_offloop, 16
 * threads: 6 context switches per arwlock acquire, 10x slower than a
 * pthread rwlock).  Now a waiter first polls the lock's state words
 * without taking the inner mutex (test-and-test-and-set: a spinner never
 * writes the line the holder needs), for at most sync_spin_rounds()
 * polls (about 5.6 us; measured best of 1.4..22 us for holds of 50 ns to
 * 5 us), then parks.  At most sync_spin_cap() threads spin on one lock at a time
 * (a quarter of the CPUs): more would only steal CPU from the holder when
 * threads outnumber CPUs.  No spinning on a single CPU or under the
 * simulator (one thread: nothing can release the lock while we poll).
 * The spin reads no clock and no RNG, so it is deterministic.
 *
 * The bound is a TIME, converted once per process into a poll count.
 * SYNC_SPIN_ROUNDS / SYNC_NOTIFY_SPIN_ROUNDS were tuned as counts on an
 * x86-64 host whose PAUSE took 11 ns (SYNC_REF_POLL_NS), but a poll's
 * cost differs widely between cores: PAUSE + load measured 20 ns on
 * another x86-64 core (and Intel documents PAUSE at about 140 cycles
 * from Skylake on, ~10 before), while aarch64's YIELD retires as a NOP
 * (0.4 ns on a 16-vCPU Neoverse-V1 host).  There 512 polls lasted about
 * a microsecond, so every 2 us hold parked its waiter (test_sync
 * /offloop_spin: 39,581 context switches for 40,000 acquires).  The
 * first spin times the poll loop (sync_spin_calibrate) and scales both
 * counts by SYNC_REF_POLL_NS / measured, so the spin lasts the same time
 * on every core.  The calibration reads the monotonic clock once per
 * process, and only where spinning is allowed: never under the simulator
 * (sync_spin_cap() is 0 there), so DST stays clock-free.
 *
 * shortcut: the spin is bounded by a count, not by "the holder is still
 * running" (the lock does not record which thread holds it); upgrade to
 * an owner-on-CPU check if holders that sleep while latched show up as
 * wasted spin in a profile.
 * ----------------------------------------------------------------------- */
#ifndef SYNC_SPIN_ROUNDS
#define SYNC_SPIN_ROUNDS 512
#endif
#define SYNC_REF_POLL_NS 11     /* ns per poll where the counts were tuned */
#define SYNC_CAL_POLLS   256    /* polls per calibration trial */
#define SYNC_CAL_TRIALS  3
/*
 * Floor of the scale, x1000.  x86-64 keeps at least the tuned counts: a
 * slower PAUSE there spins longer, not fewer polls (the shorter spin cost
 * 4-9% at 4 threads on a 64-vCPU Icelake host, where PAUSE + load takes
 * ~20 ns).  Elsewhere the spin may shrink to 1/16 of the counts.
 */
#if defined(__x86_64__) || defined(__i386__)
#define SYNC_SCALE_MIN_PM 1000
#else
#define SYNC_SCALE_MIN_PM (1000 / 16)
#endif

/*
 * The spin hint.  aarch64: ISB, not YIELD.  YIELD is a hint for SMT
 * cores; on Neoverse-V1 it retires at once (0.4 ns), so for the same
 * time a YIELD loop polls the lock word 7x as often as an ISB loop
 * (1.9 vs 13.7 ns per hint + load), which drains the pipeline like x86
 * PAUSE.  Measured with the calibrated budget on a 16-vCPU aarch64 host
 * (bench_sync_offloop, 5 reps): ISB is ahead on every exclusive
 * short-hold case (amutex 4 threads 1.29M vs 1.09M acquires/s, arwlock
 * 1.37M vs 1.10M), and only ISB kept test_sync /offloop_spin passing
 * (YIELD: 2 of 12 runs parked every amutex acquire).  Rust's
 * core::hint::spin_loop is ISB on aarch64 too.  Elsewhere the shared
 * hint.
 */
#if defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
#define sync_spin_hint() __asm__ __volatile__("isb" ::: "memory")
#else
#define sync_spin_hint() __os_pause()
#endif

static _Atomic int g_sync_spin_cap = -1;
static _Atomic int g_sync_spin_scale_pm;   /* polls per tuned poll, x1000 */
static _Atomic int g_sync_cal_word;

static int
sync_spin_cap(void)
{
	int v;
	if (__xtc_sim_active())
		return 0;
	v = atomic_load_explicit(&g_sync_spin_cap, memory_order_relaxed);
	if (v < 0) {
		v = __os_ncpus();
		v = v > 1 ? (v / 4 > 0 ? v / 4 : 1) : 0;
		atomic_store_explicit(&g_sync_spin_cap, v, memory_order_relaxed);
	}
	return v;
}

/*
 * Time SYNC_CAL_POLLS polls of the spin loop's shape (hint + relaxed
 * load), best of SYNC_CAL_TRIALS so an interrupt in one trial cannot
 * shorten every later spin, and set the scale.  One-off cost about
 * 3 x 256 polls (10 us at 13 ns, 110 us at 140 ns).  Racing first
 * spinners may each calibrate; any of their results is fine.
 */
static int
sync_spin_calibrate(void)
{
	int64_t t0, t1, ns, best = INT64_MAX;
	int i, k, sink = 0, pm;

	for (k = 0; k < SYNC_CAL_TRIALS; k++) {
		if (__os_clock_mono(&t0) != XTC_OK)
			return 1000;
		for (i = 0; i < SYNC_CAL_POLLS; i++) {
			sync_spin_hint();
			sink += atomic_load_explicit(&g_sync_cal_word,
			    memory_order_relaxed);
		}
		if (__os_clock_mono(&t1) != XTC_OK)
			return 1000;
		ns = t1 - t0 + (sink != 0);
		if (ns < best)
			best = ns;
	}
	if (best < 1)
		best = 1;
	/* scale = REF / (best / POLLS), x1000, kept within [MIN, 64]. */
	pm = (int)((int64_t)SYNC_REF_POLL_NS * SYNC_CAL_POLLS * 1000 / best);
	if (pm < SYNC_SCALE_MIN_PM)
		pm = SYNC_SCALE_MIN_PM;
	if (pm > 64 * 1000)
		pm = 64 * 1000;
	atomic_store_explicit(&g_sync_spin_scale_pm, pm, memory_order_relaxed);
	return pm;
}

/* `tuned` polls of the reference core, in polls of this one. */
static int
sync_spin_rounds(int tuned)
{
	int pm = atomic_load_explicit(&g_sync_spin_scale_pm,
	    memory_order_relaxed);
	if (pm == 0)
		pm = sync_spin_calibrate();
	return (int)((int64_t)tuned * pm / 1000);
}

/* Enter the spin if fewer than the cap are spinning; 1 if admitted. */
static int
sync_spin_enter(_Atomic int *spinners)
{
	int cap = sync_spin_cap();
	if (cap == 0)
		return 0;
	if (atomic_fetch_add_explicit(spinners, 1, memory_order_relaxed) >= cap) {
		atomic_fetch_sub_explicit(spinners, 1, memory_order_relaxed);
		return 0;
	}
	return 1;
}

static void
sync_spin_leave(_Atomic int *spinners)
{
	atomic_fetch_sub_explicit(spinners, 1, memory_order_relaxed);
}

/*
 * amutex / arwlock objects start on a cache line and own whole lines:
 * sqlxtc allocates one arwlock per buffer frame, and with calloc'd
 * 136-232 byte objects the hot words of neighbouring frames' latches
 * shared lines, so threads reading DIFFERENT pages bounced the same
 * lines.  Measured (test_snap_registry, 8 threads, read-only
 * transactions, 5 reps): 1.75-1.87M txns/s packed, 2.58-2.70M aligned.
 * Costs up to one line of padding per object.
 */
static int
sync_obj_alloc(size_t sz, void **out)
{
	int rc;
	sz = (sz + XTC_CACHE_LINE - 1) & ~(size_t)(XTC_CACHE_LINE - 1);
	if ((rc = __os_aligned_alloc(XTC_CACHE_LINE, sz, out)) != XTC_OK)
		return rc;
	memset(*out, 0, sz);
	return XTC_OK;
}

/* __xtc_mtx_lock's try form: 1 with *lk held (bracket balanced by
 * __xtc_mtx_unlock), 0 if busy.  A spinner never blocks on the inner
 * mutex: a busy one means another thread is about to take the lock. */
static int
sync_mtx_trylock(pthread_mutex_t *lk)
{
	__xtc_unsafe_enter();
	if (pthread_mutex_trylock(lk) == 0)
		return 1;
	__xtc_unsafe_leave();
	return 0;
}

/* -------------------------------------------------------------------------
 * Fiber-park path for the blocking primitives (notify, semaphore, amutex,
 * arwlock, barrier, gate).
 *
 * A caller running inside a fiber on a loop (__xtc_current_task() != NULL)
 * MUST NOT block the OS thread on a condvar: under the single-thread DST
 * scheduler that would freeze every other fiber (no other thread can
 * signal it), and in production it wedges the loop.  Instead such a caller
 * PARKS the fiber -- it links a stack waiter on the primitive's fiber wait
 * queue, drops the primitive's lock and xtc_yield()s to the loop -- and is
 * re-woken by a post / signal / unlock / leave / close (or the barrier's
 * last arrival).  On wake it re-takes the lock and re-tests; a wake that
 * does not satisfy it (spurious, timer, lost race) re-parks.
 *
 * A caller NOT on a loop (OS threads, the blocking pool, tooling) takes
 * the pthread_cond_wait / pthread_cond_timedwait path.  A waker serves
 * both kinds (condvar broadcast/signal for threads, the queue for fibers),
 * so mixed waiters are correct.
 *
 * The semaphore, gate and barrier use wake-and-recheck: the shared state
 * (a count, a generation) is not a single ownable token, so a woken fiber
 * re-evaluates its predicate under the lock.  A waiter stays queued until
 * it stops waiting, and wakers wake in place without detaching the queue:
 * a woken waiter that loses the re-check (a sibling took the count)
 * re-parks still queued, so the next post reaches it.  Detaching the
 * queue on wake (1.55.0) stranded exactly such a loser.
 *
 * notify, amutex and arwlock hand off directly: the waker dequeues the
 * head waiter(s) and sets `granted`, so one signal / unlock reaches
 * exactly one waiter and the rest stay queued.
 *
 * Every wake fires UNDER the primitive's lock.  To return, a woken waiter
 * must re-take that lock first, so its stack frame (which holds the
 * waker) outlives xtc_waker_wake.  The wake only posts to the target
 * loop, so it orders against no other lock.
 *
 * Every exit from a timed wait cancels the park timer.  A timer left in
 * the task slot turns the fiber's next plain xtc_yield into a park until
 * the stale deadline.
 *
 * Kill delivery.  The resume hook inside xtc_yield may unwind a pending
 * kill before any code after the yield runs, which would leave a stack
 * waiter linked (or a lock granted to a dead fiber).  So every wait runs
 * under the cancellation mask (a kill is latched, not delivered, at the
 * resume point); after each resume the loop checks xtc_cancel_requested(),
 * unlinks itself, gives back anything it was granted, cancels its park
 * timer and drops the lock, and only then delivers the kill.  If an
 * enclosing mask of the caller still defers it, the wait resumes as if
 * nothing happened.
 * ----------------------------------------------------------------------- */
struct fiber_waiter {
	xtc_waker_t          waker;
	struct fiber_waiter *next;
	struct fiber_waiter *prev;
	unsigned             need;     /* semaphore: units wanted */
	int                  mode;     /* arwlock: ARW_SHARED / ARW_EXCL */
	int                  granted;  /* notify/amutex/arwlock hand-off */
	int                  queued;
	/* notify: granted by a signaler on this pinned waiter's own loop
	 * thread, which therefore finished signalling (and unlocked) before
	 * the waiter could resume; the waiter may return without the lock.
	 * Only that same-thread signaler sets it (a remote one never writes
	 * it), so it is never accessed by two threads. */
	int                  handed;
	int                  lost;     /* amutex: woken, found it re-taken */
};

struct fw_queue {
	struct fiber_waiter *head;     /* FIFO */
	struct fiber_waiter *tail;
};

/* Link w at the tail unless already queued (under the lock). */
static void
fw_enqueue(struct fw_queue *q, struct fiber_waiter *w)
{
	if (w->queued) return;
	w->next = NULL;
	w->prev = q->tail;
	if (q->tail != NULL) q->tail->next = w;
	else q->head = w;
	q->tail = w;
	w->queued = 1;
}

/* Unlink w if queued, O(1) (under the lock). */
static void
fw_remove(struct fw_queue *q, struct fiber_waiter *w)
{
	if (!w->queued) return;
	if (w->prev != NULL) w->prev->next = w->next;
	else q->head = w->next;
	if (w->next != NULL) w->next->prev = w->prev;
	else q->tail = w->prev;
	w->next = w->prev = NULL;
	w->queued = 0;
}

/* Wake every queued waiter in place (under the lock). */
static void
fw_wake_all(struct fw_queue *q)
{
	struct fiber_waiter *w;
	for (w = q->head; w != NULL; w = w->next)
		(void)xtc_waker_wake(&w->waker);
}

/* Absolute mono deadline for a relative timeout; -1 for none. */
static int64_t
fw_deadline(int64_t timeout_ns)
{
	int64_t now = 0;
	if (timeout_ns < 0) return -1;
	(void)__os_clock_mono(&now);
	return now + timeout_ns;
}

/* 1 once a deadline has passed; *now is set when there is a deadline. */
static int
fw_expired(int64_t deadline, int64_t *now)
{
	if (deadline < 0) return 0;
	(void)__os_clock_mono(now);
	return *now >= deadline;
}

/*
 * Park the calling fiber once.  Called with *lk held, returns WITHOUT it
 * (fw_park re-takes it).  The waker is re-armed each time so it names
 * the loop the fiber is on now (a migratable fiber may have been stolen
 * since the last park), and a stale park timer is replaced, never left
 * to fire into a later park.
 */
static void
fw_park_unlocked(pthread_mutex_t *lk, xtc_task_t *cur, xtc_waker_t *wk,
    int64_t deadline, int64_t now)
{
	void *proc_ctx;
	(void)xtc_task_waker(cur, wk);
	if (deadline >= 0) {
		__xtc_task_cancel_park_timer(cur);
		(void)xtc_task_park_on_timer(cur, deadline - now);
	} else {
		cur->park_requested = 1;
	}
	(void)__xtc_mtx_unlock(lk);
	proc_ctx = __xtc_proc_ctx_save();
	XTC_PROBE_FIBER_PARK((uintptr_t)cur, XTC_PROBE_PARK_SYNC, (uintptr_t)lk);
	xtc_yield();
	XTC_PROBE_FIBER_RESUME((uintptr_t)cur, XTC_PROBE_PARK_SYNC,
	    (uintptr_t)lk);
	__xtc_proc_ctx_restore(proc_ctx);
}

/* Park once; called and returns with *lk held. */
static void
fw_park(pthread_mutex_t *lk, xtc_task_t *cur, xtc_waker_t *wk,
    int64_t deadline, int64_t now)
{
	fw_park_unlocked(lk, cur, wk, deadline, now);
	(void)__xtc_mtx_lock(lk);
}

/*
 * Wait-scope cancellation mask.  The state word is 0 off a proc (nothing
 * can kill the caller), 1 while kills are checked, 2 once an enclosing
 * mask of the caller is known to defer them (stop checking).
 */
static int
fw_mask_enter(void)
{
	return xtc_mask_enter() == XTC_OK ? 1 : 0;
}

/* Drop the wait's mask.  Every resume checks fw_kill_pending before
 * taking a grant, so a kill seen here arrived after the wait succeeded
 * and is delivered as it would be at the caller's next yield. */
static void
fw_mask_leave(int masked)
{
	if (masked != 0) (void)xtc_mask_leave();
}

static int
fw_kill_pending(int masked)
{
	return masked == 1 && xtc_cancel_requested();
}

/*
 * Deliver a pending kill.  Call with no lock held and nothing linked or
 * granted.  Does not return unless an enclosing mask of the caller
 * defers the kill; then the wait's mask is re-raised and *masked becomes
 * 2.  xtc_mask_leave delivers a kill the resume hook latched; the poll
 * delivers one it did not latch.
 */
static void
fw_kill_deliver(int *masked)
{
	(void)xtc_mask_leave();
#ifdef XTC_HAVE_PROC_KILL_WAKE
	__xtc_proc_kill_poll();
#else
	if (__xtc_fiber_kill_check != NULL) __xtc_fiber_kill_check();
#endif
	(void)xtc_mask_enter();
	*masked = 2;
}

/* ----- notify ----------------------------------------------------- */

struct xtc_notify {
	pthread_mutex_t lock;
	pthread_cond_t  cv;
	_Atomic int     stored;       /* one-shot stored signal; under lock,
	                               * peeked by off-loop spinners */
	struct fw_queue wq;           /* fiber waiters */
};

#define NT_STORED(n) atomic_load_explicit(&(n)->stored, memory_order_relaxed)
#define NT_SET(n, v) \
	atomic_store_explicit(&(n)->stored, (v), memory_order_relaxed)

/*
 * An off-loop thread that waits on a notify without a deadline first polls
 * `stored` (inner mutex dropped) for up to SYNC_NOTIFY_SPIN_ROUNDS pauses,
 * then sleeps on the condvar.  sqlxtc's committers wait here for the WAL
 * group-commit leader's fdatasync and for the visibility horizon; the
 * signal usually comes within tens of microseconds, and a futex sleep +
 * wake on a CPU that has dropped into a deep idle state costs ~100 us
 * (measured, 64-vCPU x86-64 host with C6 enabled: wake-to-run 80-130 us
 * from C6, 2-3 us otherwise).  4096 polls of the reference core is about
 * 45 us (scaled per core; see sync_spin_rounds).  The spinners share one process-wide cap (sync_spin_cap(),
 * which is 0 on one CPU and under the simulator); timed waits do not spin.
 */
#ifndef SYNC_NOTIFY_SPIN_ROUNDS
#define SYNC_NOTIFY_SPIN_ROUNDS 4096
#endif
static _Atomic int g_notify_spinners;

/* Hand one signal to the oldest fiber waiter, else store it for the next
 * wait (under the lock).  Direct hand-off, so N signals reach N parked
 * fibers; with a stored bit alone two signals before either waiter ran
 * collapsed into one and the second waiter stayed parked. */
static void
notify_give_locked(xtc_notify_t *n)
{
	struct fiber_waiter *w = n->wq.head;
	if (w == NULL) {
		NT_SET(n, 1);
		(void)pthread_cond_broadcast(&n->cv);
		return;
	}
	fw_remove(&n->wq, w);
	w->granted = 1;
	/* Same-thread hand-off to a pinned waiter: it cannot run until this
	 * step yields, and a pinned task is never stolen meanwhile.  A
	 * remote or migratable waiter re-takes the lock to return, so a
	 * notify freed right after the wait never sees our unlock. */
	if (__xtc_current_loop == w->waker.loop && w->waker.task->pinned)
		w->handed = 1;
	(void)xtc_waker_wake(&w->waker);
}

int
xtc_notify_create(xtc_notify_t **out)
{
	xtc_notify_t *n;
	int rc;
	if (out == NULL) return XTC_E_INVAL;
	if ((rc = __os_calloc(1, sizeof *n, (void **)&n)) != XTC_OK)
		return rc;
	(void)pthread_mutex_init(&n->lock, NULL);
	(void)pthread_cond_init(&n->cv, NULL);
	*out = n;
	return XTC_OK;
}

void
xtc_notify_destroy(xtc_notify_t *n)
{
	if (n == NULL) return;
	(void)pthread_cond_destroy(&n->cv);
	(void)pthread_mutex_destroy(&n->lock);
	__os_free(n);
}

int
xtc_notify_signal(xtc_notify_t *n)
{
	if (n == NULL) return XTC_E_INVAL;
	(void)__xtc_mtx_lock(&n->lock);
	notify_give_locked(n);
	(void)__xtc_mtx_unlock(&n->lock);
	return XTC_OK;
}

int
xtc_notify_wait(xtc_notify_t *n, int64_t timeout_ns)
{
	int rc = XTC_OK;
	xtc_task_t *cur;
	struct fiber_waiter w;
	int64_t deadline, now = 0;
	int masked, i, rounds;

	if (n == NULL) return XTC_E_INVAL;
	(void)__xtc_mtx_lock(&n->lock);
	if (NT_STORED(n)) {
		NT_SET(n, 0);
		(void)__xtc_mtx_unlock(&n->lock);
		return XTC_OK;
	}
	if (timeout_ns == 0) {
		(void)__xtc_mtx_unlock(&n->lock);
		return XTC_E_AGAIN;
	}

	cur = __xtc_current_task();
	if (cur != NULL) {
		/* Fiber waiter: park and re-check on wake. */
		memset(&w, 0, sizeof w);
		deadline = fw_deadline(timeout_ns);
		masked = fw_mask_enter();
		for (;;) {
			if (fw_kill_pending(masked)) {
				fw_remove(&n->wq, &w);
				if (w.granted) {        /* pass it on */
					w.granted = 0;
					notify_give_locked(n);
				}
				__xtc_task_cancel_park_timer(cur);
				(void)__xtc_mtx_unlock(&n->lock);
				fw_kill_deliver(&masked);
				(void)__xtc_mtx_lock(&n->lock);
				continue;
			}
			if (w.granted) { rc = XTC_OK; break; }
			if (NT_STORED(n)) { NT_SET(n, 0); rc = XTC_OK; break; }
			if (fw_expired(deadline, &now)) { rc = XTC_E_AGAIN; break; }
			fw_enqueue(&n->wq, &w);
			fw_park_unlocked(&n->lock, cur, &w.waker, deadline, now);
			if (w.handed && !fw_kill_pending(masked)) {
				/* Dequeued and granted by a same-thread signal
				 * that has already unlocked: done, lock-free. */
				__xtc_task_cancel_park_timer(cur);
				fw_mask_leave(masked);
				return XTC_OK;
			}
			w.handed = 0;   /* a kill: the locked path passes it on */
			(void)__xtc_mtx_lock(&n->lock);
		}
		fw_remove(&n->wq, &w);
		__xtc_task_cancel_park_timer(cur);
		(void)__xtc_mtx_unlock(&n->lock);
		fw_mask_leave(masked);
		return rc;
	}

	if (timeout_ns < 0) {
		/* Spin first (see g_notify_spinners); the locked re-test
		 * below decides, so a signal taken by another waiter meanwhile
		 * only means we sleep. */
		if (sync_spin_enter(&g_notify_spinners)) {
			(void)__xtc_mtx_unlock(&n->lock);
			rounds = sync_spin_rounds(SYNC_NOTIFY_SPIN_ROUNDS);
			for (i = 0; i < rounds && !NT_STORED(n); i++)
				sync_spin_hint();
			sync_spin_leave(&g_notify_spinners);
			(void)__xtc_mtx_lock(&n->lock);
		}
		while (!NT_STORED(n))
			(void)pthread_cond_wait(&n->cv, &n->lock);
		NT_SET(n, 0);
	} else {
		struct timespec ts;
		deadline_from_timeout(timeout_ns, &ts);
		while (!NT_STORED(n)) {
			if (cv_wait_until(&n->cv, &n->lock, &ts)) {
				rc = XTC_E_AGAIN;
				break;
			}
		}
		if (NT_STORED(n)) { NT_SET(n, 0); rc = XTC_OK; }
	}
	(void)__xtc_mtx_unlock(&n->lock);
	return rc;
}

/* ----- semaphore -------------------------------------------------- */

struct xtc_sem {
	pthread_mutex_t lock;
	pthread_cond_t  cv;
	unsigned        count;
	struct fw_queue wq;           /* fiber waiters */
};

/*
 * Wake the fiber waiters the current count can satisfy, oldest first: a
 * post of n units wakes waiters for up to n units, not the whole queue
 * (under the lock).  Every woken waiter re-runs this when it stops
 * waiting (whether it took units, timed out or was killed) and before it
 * re-parks after losing its units to a barging acquirer, so units never
 * sit unclaimed beside a parked waiter they would satisfy.  A waiter
 * whose need exceeds the remaining budget is skipped, not waited on, so
 * the scan is O(waiters) in the worst case (mixed sizes) and O(woken)
 * for the usual all-ones pattern.
 */
static void
sem_wake_locked(xtc_sem_t *s)
{
	struct fiber_waiter *w;
	unsigned budget = s->count;
	for (w = s->wq.head; w != NULL && budget > 0; w = w->next) {
		if (w->need <= budget) {
			(void)xtc_waker_wake(&w->waker);
			budget -= w->need;
		}
	}
}

int
xtc_sem_create(unsigned initial, xtc_sem_t **out)
{
	xtc_sem_t *s;
	int rc;
	if (out == NULL) return XTC_E_INVAL;
	if ((rc = __os_calloc(1, sizeof *s, (void **)&s)) != XTC_OK)
		return rc;
	(void)pthread_mutex_init(&s->lock, NULL);
	(void)pthread_cond_init(&s->cv, NULL);
	s->count = initial;
	*out = s;
	return XTC_OK;
}

void
xtc_sem_destroy(xtc_sem_t *s)
{
	if (s == NULL) return;
	(void)pthread_cond_destroy(&s->cv);
	(void)pthread_mutex_destroy(&s->lock);
	__os_free(s);
}

int
xtc_sem_post(xtc_sem_t *s, unsigned n)
{
	if (s == NULL) return XTC_E_INVAL;
	(void)__xtc_mtx_lock(&s->lock);
	s->count += n;
	(void)pthread_cond_broadcast(&s->cv);
	sem_wake_locked(s);
	(void)__xtc_mtx_unlock(&s->lock);
	return XTC_OK;
}

int
xtc_sem_try_acquire(xtc_sem_t *s, unsigned n)
{
	int rc = XTC_E_AGAIN;
	if (s == NULL) return XTC_E_INVAL;
	(void)__xtc_mtx_lock(&s->lock);
	if (s->count >= n) { s->count -= n; rc = XTC_OK; }
	(void)__xtc_mtx_unlock(&s->lock);
	return rc;
}

int
xtc_sem_acquire(xtc_sem_t *s, unsigned n, int64_t timeout_ns)
{
	int rc = XTC_OK;
	xtc_task_t *cur;
	struct fiber_waiter w;
	int64_t deadline, now = 0;
	int masked;

	if (s == NULL) return XTC_E_INVAL;
	(void)__xtc_mtx_lock(&s->lock);
	if (s->count >= n) { s->count -= n; goto out; }
	if (timeout_ns == 0) { rc = XTC_E_AGAIN; goto out; }

	cur = __xtc_current_task();
	if (cur != NULL) {
		/* Fiber waiter: park (yield to the loop) and re-check on wake,
		 * never blocking the OS thread. */
		memset(&w, 0, sizeof w);
		w.need = n;
		deadline = fw_deadline(timeout_ns);
		masked = fw_mask_enter();
		for (;;) {
			if (fw_kill_pending(masked)) {
				fw_remove(&s->wq, &w);
				sem_wake_locked(s);
				__xtc_task_cancel_park_timer(cur);
				(void)__xtc_mtx_unlock(&s->lock);
				fw_kill_deliver(&masked);
				(void)__xtc_mtx_lock(&s->lock);
				continue;
			}
			if (s->count >= n) {
				/* Buggify: under DST, occasionally decline a
				 * satisfiable acquire and report a spurious
				 * timeout instead -- a legal pessimal outcome
				 * ONLY when the caller passed a finite timeout
				 * (XTC_E_AGAIN is documented there and the
				 * caller must retry).  A fresh per-call fault
				 * draw so a retrying caller eventually
				 * succeeds; the site coin gates liveness. */
				if (deadline >= 0 &&
				    XTC_SIM_BUGGIFY("sync.sem.spurious_timeout") &&
				    xtc_sim_fault(250)) {
					rc = XTC_E_AGAIN;
					break;
				}
				s->count -= n; rc = XTC_OK; break;
			}
			if (fw_expired(deadline, &now)) { rc = XTC_E_AGAIN; break; }
			/* Beaten to the units: pass what is left to a waiter
			 * it does satisfy before parking again. */
			sem_wake_locked(s);
			fw_enqueue(&s->wq, &w);
			fw_park(&s->lock, cur, &w.waker, deadline, now);
		}
		fw_remove(&s->wq, &w);
		sem_wake_locked(s);
		__xtc_task_cancel_park_timer(cur);
		(void)__xtc_mtx_unlock(&s->lock);
		fw_mask_leave(masked);
		return rc;
	}

	/* Off-loop: block the OS thread on the condvar, as before. */
	if (timeout_ns < 0) {
		while (s->count < n)
			(void)pthread_cond_wait(&s->cv, &s->lock);
		s->count -= n;
	} else {
		struct timespec ts;
		deadline_from_timeout(timeout_ns, &ts);
		while (s->count < n) {
			if (cv_wait_until(&s->cv, &s->lock, &ts)) {
				rc = XTC_E_AGAIN;
				goto out;
			}
		}
		s->count -= n;
	}
out:
	(void)__xtc_mtx_unlock(&s->lock);
	return rc;
}

int
xtc_sem_count(const xtc_sem_t *s)
{
	int v;
	if (s == NULL) return 0;
	(void)__xtc_mtx_lock((pthread_mutex_t *)&s->lock);
	v = (int)s->count;
	(void)__xtc_mtx_unlock((pthread_mutex_t *)&s->lock);
	return v;
}

/* ----- abort_source ---------------------------------------------- */

struct xtc_abort_source {
	_Atomic int  fired;
	_Atomic int  reason;
};

int
xtc_abort_source_create(xtc_abort_source_t **out)
{
	xtc_abort_source_t *s;
	int rc;
	if (out == NULL) return XTC_E_INVAL;
	if ((rc = __os_calloc(1, sizeof *s, (void **)&s)) != XTC_OK)
		return rc;
	atomic_store_explicit(&s->fired, 0, memory_order_relaxed);
	atomic_store_explicit(&s->reason, 0, memory_order_relaxed);
	*out = s;
	return XTC_OK;
}

void
xtc_abort_source_destroy(xtc_abort_source_t *s)
{
	__os_free(s);
}

int
xtc_abort_source_fire(xtc_abort_source_t *s, int reason)
{
	if (s == NULL) return XTC_E_INVAL;
	atomic_store_explicit(&s->reason, reason, memory_order_relaxed);
	atomic_store_explicit(&s->fired, 1, memory_order_release);
	return XTC_OK;
}

int
xtc_abort_source_token(xtc_abort_source_t *s, xtc_abort_token_t *out)
{
	if (s == NULL || out == NULL) return XTC_E_INVAL;
	out->src = s;
	return XTC_OK;
}

int
xtc_abort_token_is_aborted(const xtc_abort_token_t *t)
{
	if (t == NULL || t->src == NULL) return 0;
	return atomic_load_explicit(&t->src->fired, memory_order_acquire);
}

int
xtc_abort_token_reason(const xtc_abort_token_t *t)
{
	if (t == NULL || t->src == NULL) return 0;
	return atomic_load_explicit(&t->src->reason, memory_order_relaxed);
}

/* ----- amutex (parking mutex) -----------------------------------
 *
 * Contended waiters that are running inside a coroutine / process
 * park the fiber (yield to the loop) instead of blocking the OS
 * thread, so a process can hold the lock across its own park (e.g.
 * a blocking-pool offload) without wedging the loop the moment
 * another process on that loop contends.  Off a loop (cur == NULL)
 * the caller blocks on a condvar, as before.
 *
 * Fiber waiters use a FIFO queue with direct hand-off: unlock grants
 * the lock to the head waiter (held stays 1) and wakes it, so there
 * is no thundering herd and ordering is fair.  Thread waiters share
 * the same `held` flag via the condvar: they spin briefly, then sleep
 * (see "Off-loop spin-then-park").  When both kinds wait, unlock
 * prefers a fiber waiter; a release with no fiber waiters wakes ONE
 * sleeping thread (cond_signal), unless a thread is spinning on the
 * mutex (see amutex_wake_thread_locked).  An earlier version held the
 * wake back while a previously woken thread had not run yet (at most
 * one wake in flight per mutex); on a host with deep CPU idle states
 * the woken thread takes ~100 us to run, and the mutex sat free with
 * sleepers for that long (measured, 64-vCPU x86-64 host: 22 us mean
 * free-with-sleepers interval, sqlxtc ledger transfer at 64 clients).
 * A spinner cannot leave it free that way: it polls `held` and takes
 * the mutex within its ~5 us spin.
 */

/* Field order: lock .. wq.head (everything an uncontended lock/unlock
 * touches) fit the first cache line on LP64 glibc; see struct
 * xtc_arwlock and sync_obj_alloc. */
struct xtc_amutex {
	pthread_mutex_t       lock;
	_Atomic int           held;      /* written under lock; spinners peek */
	int                   recursive; /* XTC_AMUTEX_RECURSIVE */
	int                   cv_waiters; /* threads asleep on cv */
	struct fw_queue       wq;        /* fiber waiters */
	_Atomic int           spinners;  /* threads in the off-loop spin */
	int                   count;     /* recursion depth */
	int                   wake_skips; /* releases gated since a wake */
	int                   handoff;   /* next release grants the head */
	struct amutex_owner {
		int       kind;          /* 0 none, 1 fiber, 2 thread */
		void     *task;          /* fiber identity (kind 1) */
		xtc_pid_t pid;           /* its proc, or XTC_PID_NONE (kind 1) */
		void     *ploop;         /* that proc's owner loop (kind 1) */
		pthread_t thr;           /* thread identity (kind 2) */
	}                     owner;     /* valid while held; under lock */
	pthread_cond_t        cv;        /* thread (non-fiber) waiters */
};

#define AM_HELD(m) atomic_load_explicit(&(m)->held, memory_order_relaxed)
#define AM_SET(m, v) \
	atomic_store_explicit(&(m)->held, (v), memory_order_relaxed)

/* Wake ONE sleeping thread waiter (under lock).  The woken thread
 * re-tests: a running thread may have taken the lock first (barging,
 * which keeps the lock busy while the sleeper is being scheduled); then
 * it sleeps again and that thread's release wakes the next one.
 *
 * The spinner gate: a release that frees the mutex while a thread is
 * spinning on it does not wake a sleeper.  The spinner takes the free
 * mutex within its spin, so a sleeper woken now would only find it
 * re-taken and sleep again: on a hot mutex 1.59 woke one on EVERY
 * release, 3.2 context switches per acquire and half the 1.58 rate
 * (bench_sync_offloop amutex w 64, 64-vCPU x86-64 host: 247k vs 486k
 * acquires/s; with the gate 523k).  At most SYNC_AM_SKIP_MAX releases
 * in a row skip, so a sleeper still gets a turn on a mutex that always
 * has a spinner (2..32 measured: 8 keeps 64-thread acquire p99 at the
 * ungated 1.2 ms, unbounded let it grow to 1.6 ms).  Nothing is
 * stranded: `spinners` is raised under m->lock, so a counted spinner
 * either is still polling or has stopped and not yet re-taken m->lock;
 * either way it re-tests `held` after this release, takes the free
 * mutex or sleeps, and the next release decides again.  The gate reads
 * no clock: deterministic, and spinners is 0 under the simulator. */
#ifndef SYNC_AM_SKIP_MAX
#define SYNC_AM_SKIP_MAX 8
#endif
static void
amutex_wake_thread_locked(xtc_amutex_t *m)
{
	if (m->cv_waiters == 0)
		return;
	if (SYNC_AM_SKIP_MAX > 0 &&
	    atomic_load_explicit(&m->spinners, memory_order_relaxed) > 0 &&
	    m->wake_skips < SYNC_AM_SKIP_MAX) {
		m->wake_skips++;
		return;
	}
	m->wake_skips = 0;
	(void)pthread_cond_signal(&m->cv);
}

/* The mutex is free (under lock): wake the head fiber waiter, which
 * stays queued and re-competes, else a thread waiter. */
static void
amutex_wake_next_locked(xtc_amutex_t *m)
{
	if (m->wq.head != NULL)
		(void)xtc_waker_wake(&m->wq.head->waker);
	else
		amutex_wake_thread_locked(m);
}

/*
 * Release a held (non-recursive-nested) mutex.  Under m->lock.
 *
 * Normally the mutex is FREED and the head fiber waiter woken; it stays
 * queued and re-competes when it runs, so a running fiber or thread may
 * take the mutex first (barging).  Handing it to the parked head instead
 * (held stays 1 until the head runs) kept the mutex idle for the whole
 * wake latency of the head's loop, every release: one hot amutex shared
 * by fibers on 4 loops did 185k acquires/s with hand-off and 2.13M
 * without (bench_amutex_fibers 4 64, 32-vCPU x86-64 host); on one loop
 * the two are equal.  A head that loses AM_HANDOFF_LOSSES races in a row
 * sets `handoff`, and the next release grants it the mutex directly, so
 * a parked waiter is not starved by a fiber that re-locks in a loop.
 * Counted, not timed: deterministic under the simulator.
 *
 * The waker fires before m->lock is dropped: the waiter cannot return
 * (and release its stack frame) without re-taking m->lock.
 */
#ifndef AM_HANDOFF_LOSSES
#define AM_HANDOFF_LOSSES 4
#endif
static void
amutex_release_locked(xtc_amutex_t *m)
{
	struct fiber_waiter *w = m->wq.head;
	if (m->recursive) {
		m->count = 0;
		m->owner.kind = 0;         /* next owner claims it */
	}
	if (w != NULL && m->handoff) {
		m->handoff = 0;
		fw_remove(&m->wq, w);
		w->granted = 1;
		(void)xtc_waker_wake(&w->waker);
		return;
	}
	AM_SET(m, 0);
	amutex_wake_next_locked(m);
}

/* Identity of the current execution context: the running fiber's task
 * on a loop, else the OS thread.  Tracked by FIBER on a loop so two
 * fibers sharing one OS thread are distinct owners.  A finished proc's
 * task struct is recycled for the next proc on its loop, so the task
 * pointer alone would let that proc re-enter a lock the dead one still
 * holds; the proc's (pid, owner loop) pair -- generation-tagged, and
 * re-anchored on the running fiber so it is right after a migration --
 * tells them apart.  A non-proc fiber keeps the task-pointer identity:
 * only proc tasks are recycled at DONE. */
static struct amutex_owner
amutex_self_owner(void)
{
	struct amutex_owner o;
	xtc_task_t *t = __xtc_current_task();
	o.task = NULL;
	o.pid = XTC_PID_NONE;
	o.ploop = NULL;
	if (t != NULL) {
		o.kind = 1;
		o.task = t;
		o.ploop = __xtc_proc_self_loop(&o.pid);
	} else {
		o.kind = 2;
		o.thr = pthread_self();
	}
	return o;
}

static int
amutex_owner_eq(const struct amutex_owner *a, const struct amutex_owner *b)
{
	if (a->kind != b->kind || a->kind == 0) return 0;
	if (a->kind == 1)
		return a->task == b->task && a->ploop == b->ploop &&
		    xtc_pid_eq(a->pid, b->pid);
	return pthread_equal(a->thr, b->thr);
}

int
xtc_amutex_create_ex(xtc_amutex_t **out, unsigned flags)
{
	xtc_amutex_t *m;
	int rc;
	if (out == NULL) return XTC_E_INVAL;
	if ((rc = sync_obj_alloc(sizeof *m, (void **)&m)) != XTC_OK)
		return rc;
	(void)pthread_mutex_init(&m->lock, NULL);
	(void)pthread_cond_init(&m->cv, NULL);
	m->recursive = (flags & XTC_AMUTEX_RECURSIVE) ? 1 : 0;
	*out = m;
	return XTC_OK;
}

int
xtc_amutex_create(xtc_amutex_t **out)
{
	return xtc_amutex_create_ex(out, 0);
}

/* Process-global recursive static mutexes, lazily created. */
static xtc_amutex_t   *g_static_amutex[XTC_AMUTEX_STATIC_MAX];
static pthread_mutex_t g_static_amutex_lock = PTHREAD_MUTEX_INITIALIZER;

xtc_amutex_t *
xtc_amutex_static(unsigned slot)
{
	xtc_amutex_t *m;
	if (slot >= XTC_AMUTEX_STATIC_MAX) return NULL;
	(void)__xtc_mtx_lock(&g_static_amutex_lock);
	m = g_static_amutex[slot];
	if (m == NULL) {
		if (xtc_amutex_create_ex(&m, XTC_AMUTEX_RECURSIVE) == XTC_OK)
			g_static_amutex[slot] = m;
		else
			m = NULL;
	}
	(void)__xtc_mtx_unlock(&g_static_amutex_lock);
	return m;
}

void
xtc_amutex_destroy(xtc_amutex_t *m)
{
	if (m == NULL) return;
	(void)pthread_cond_destroy(&m->cv);
	(void)pthread_mutex_destroy(&m->lock);
	__os_aligned_free(m);
}

int
xtc_amutex_try_lock(xtc_amutex_t *m)
{
	int rc = XTC_E_AGAIN;
	if (m == NULL) return XTC_E_INVAL;
	(void)__xtc_mtx_lock(&m->lock);
	if (m->recursive) {
		struct amutex_owner self = amutex_self_owner();
		if (AM_HELD(m) && amutex_owner_eq(&m->owner, &self)) {
			m->count++;
			rc = XTC_OK;
		} else if (!AM_HELD(m)) {
			AM_SET(m, 1);
			m->owner = self;
			m->count = 1;
			rc = XTC_OK;
		}
	} else if (!AM_HELD(m)) {
		AM_SET(m, 1);
		rc = XTC_OK;
	}
	(void)__xtc_mtx_unlock(&m->lock);
	return rc;
}

/* Thread-waiter (off-loop) slow path, called and returning with m->lock
 * held: spin (TTAS on `held`, the inner mutex dropped) once per sleep,
 * then sleep on the condvar.  Barging is allowed, as before: a running
 * thread may take a free lock ahead of a sleeper, which re-sleeps; the
 * barger's release wakes the next one.  A recursive lock records the
 * winning thread as its owner, as the fast and fiber paths do: without
 * that its nested lock found the mutex "held by another" and waited on
 * itself forever. */
static int
__amutex_lock_thread(xtc_amutex_t *m, int64_t timeout_ns)
{
	struct timespec ts;
	int i, got, spun = 0, expired, rounds;

	if (timeout_ns > 0)
		deadline_from_timeout(timeout_ns, &ts);
	while (AM_HELD(m)) {
		if (!spun && sync_spin_enter(&m->spinners)) {
			spun = 1;
			got = 0;
			(void)__xtc_mtx_unlock(&m->lock);
			rounds = sync_spin_rounds(SYNC_SPIN_ROUNDS);
			for (i = 0; i < rounds && !got; i++) {
				sync_spin_hint();
				if (AM_HELD(m) || !sync_mtx_trylock(&m->lock))
					continue;
				if (!AM_HELD(m))
					got = 1;        /* exit with m->lock held */
				else
					(void)__xtc_mtx_unlock(&m->lock);
			}
			sync_spin_leave(&m->spinners);
			if (!got)
				(void)__xtc_mtx_lock(&m->lock);
			continue;
		}
		m->cv_waiters++;
		expired = 0;
		XTC_PROBE_LOCK_WAIT_START((uintptr_t)m, XTC_PROBE_LOCK_AMUTEX, 0,
		    (uintptr_t)0);
		if (timeout_ns < 0)
			(void)pthread_cond_wait(&m->cv, &m->lock);
		else
			expired = cv_wait_until(&m->cv, &m->lock, &ts);
		XTC_PROBE_LOCK_WAIT_DONE((uintptr_t)m, XTC_PROBE_LOCK_AMUTEX, 0,
		    (uintptr_t)0);
		m->cv_waiters--;
		spun = 0;
		if (expired && AM_HELD(m))
			return XTC_E_AGAIN; /* the holder's release wakes the next */
	}
	AM_SET(m, 1);
	if (m->recursive) {
		m->owner = amutex_self_owner();
		m->count = 1;
	}
	return XTC_OK;
}

int
xtc_amutex_lock(xtc_amutex_t *m, int64_t timeout_ns)
{
	xtc_task_t *cur;
	struct fiber_waiter w;
	int64_t deadline, now = 0;
	int masked, rc;

	if (m == NULL) return XTC_E_INVAL;

	(void)__xtc_mtx_lock(&m->lock);
	if (m->recursive) {
		struct amutex_owner self = amutex_self_owner();
		if (AM_HELD(m) && amutex_owner_eq(&m->owner, &self)) {
			m->count++;                 /* recursive re-entry */
			(void)__xtc_mtx_unlock(&m->lock);
			return XTC_OK;
		}
		if (!AM_HELD(m)) {
			AM_SET(m, 1);
			m->owner = self;
			m->count = 1;
			(void)__xtc_mtx_unlock(&m->lock);
			return XTC_OK;
		}
	} else if (!AM_HELD(m)) {
		AM_SET(m, 1);
		(void)__xtc_mtx_unlock(&m->lock);
		return XTC_OK;
	}
	if (timeout_ns == 0) {
		(void)__xtc_mtx_unlock(&m->lock);
		return XTC_E_AGAIN;
	}

	cur = __xtc_current_task();
	if (cur == NULL) {
		/* Not on a loop: block the thread on the condvar. */
		rc = __amutex_lock_thread(m, timeout_ns);
		(void)__xtc_mtx_unlock(&m->lock);
		return rc;
	}

	/* Fiber waiter: queue and park until unlock hands the lock over. */
	memset(&w, 0, sizeof w);
	deadline = fw_deadline(timeout_ns);
	masked = fw_mask_enter();
	for (;;) {
		if (fw_kill_pending(masked)) {
			int was_head = m->wq.head == &w;
			fw_remove(&m->wq, &w);
			if (w.granted) {               /* hand it on */
				w.granted = 0;
				amutex_release_locked(m);
			} else if (was_head) {
				m->handoff = 0;
				/* A release woke us, not the next waiter. */
				if (!AM_HELD(m))
					amutex_wake_next_locked(m);
			}
			__xtc_task_cancel_park_timer(cur);
			(void)__xtc_mtx_unlock(&m->lock);
			fw_kill_deliver(&masked);
			(void)__xtc_mtx_lock(&m->lock);
			continue;
		}
		if (!w.granted && !AM_HELD(m)) {
			/* Free: take it (first pass, a release woke us as the
			 * head, or back from a deferred kill). */
			if (m->wq.head == &w)
				m->handoff = 0;   /* our hand-off is moot */
			fw_remove(&m->wq, &w);
			AM_SET(m, 1);
			w.granted = 1;
		} else if (!w.granted) {
			if (!w.queued)
				fw_enqueue(&m->wq, &w);
			else if (m->wq.head == &w &&
			    ++w.lost >= AM_HANDOFF_LOSSES)
				m->handoff = 1;   /* the next release is ours */
		}
		if (w.granted) { rc = XTC_OK; break; }
		if (fw_expired(deadline, &now)) {
			if (m->wq.head == &w)
				m->handoff = 0;
			fw_remove(&m->wq, &w);
			rc = XTC_E_AGAIN;
			break;
		}
		XTC_PROBE_LOCK_WAIT_START((uintptr_t)m, XTC_PROBE_LOCK_AMUTEX, 0,
		    (uintptr_t)cur);
		fw_park(&m->lock, cur, &w.waker, deadline, now);
		XTC_PROBE_LOCK_WAIT_DONE((uintptr_t)m, XTC_PROBE_LOCK_AMUTEX, 0,
		    (uintptr_t)cur);
	}
	if (rc == XTC_OK && m->recursive) {
		m->owner = amutex_self_owner();
		m->count = 1;
	}
	/* Every exit cancels the park timer: a stale one would turn the
	 * fiber's next plain yield into a park until the old deadline. */
	__xtc_task_cancel_park_timer(cur);
	(void)__xtc_mtx_unlock(&m->lock);
	fw_mask_leave(masked);
	return rc;
}

int
xtc_amutex_unlock(xtc_amutex_t *m)
{
	if (m == NULL) return XTC_E_INVAL;
	(void)__xtc_mtx_lock(&m->lock);
	if (m->recursive && m->count > 1) {
		m->count--;                    /* still held by this owner */
		(void)__xtc_mtx_unlock(&m->lock);
		return XTC_OK;
	}
	amutex_release_locked(m);
	(void)__xtc_mtx_unlock(&m->lock);
	return XTC_OK;
}

/* ----- arwlock (parking reader/writer latch) --------------------
 *
 * A shared/exclusive latch whose contended waiters PARK the fiber
 * (yield to the loop) rather than blocking the OS thread, so a holder
 * may park on I/O (e.g. a buffer-manager page fetch) while latched
 * without wedging the loop, and lock coupling can hold a parent latch
 * across a child fix.  Off a loop (cur == NULL) waiters block on a
 * condvar.
 *
 * Fiber waiters use a FIFO queue with direct hand-off: a release grants
 * the latch to the head waiter(s) -- a run of consecutive shared
 * waiters together, or one exclusive waiter -- and wakes them, so
 * ordering is fair and writers do not starve.  New acquirers that find
 * any waiter queued ahead of them queue too (FIFO), so a steady read
 * stream cannot starve a waiting writer.  Off-loop (condvar) waiters
 * re-check the grant condition themselves and yield to queued fiber
 * waiters; under heavy fiber contention an off-loop waiter may wait
 * (the documented tradeoff -- real contention here is between procs).
 *
 * Off-loop waiters spin briefly, then sleep: shared ones on cv, exclusive
 * ones on wcv.  A release wakes only waiters that can enter.  (An earlier
 * version also kept at most one wake per class in flight; on a host with
 * deep CPU idle states the latch then sat free with sleepers ~20-38 us
 * per release while the one woken thread left C6 -- measured, 64-vCPU
 * x86-64 host, sqlxtc ledger transfer at 16-64 clients -- so every
 * release that frees the latch now wakes.)  It used to broadcast to
 * every off-loop waiter on every release, even while readers still held
 * the latch; with N threads queued on a hot B-tree leaf each release
 * woke all N, one won and the rest slept again (about 6 context switches
 * per acquire at 16 threads).  The phases alternate: a free latch after
 * an exclusive release wakes the readers (else one writer); after a
 * shared release, one writer (else the readers); a read-held latch wakes
 * sleeping readers, who can join, only while no writer sleeps (so the
 * readers drain and the last one out wakes the writer).  A woken waiter
 * that finds the latch taken (a running thread got there first) sleeps
 * again, and that holder's release runs the wake again, so a sleeper is
 * never left behind a latch nobody releases.
 */
#define ARW_SHARED 0
#define ARW_EXCL   1

/* Field order matters: an uncontended lock/unlock touches only the inner
 * mutex, readers / writer, the waiter counts and wq.head, so they fit the
 * first cache line (LP64 glibc: offsets 0..63); the condvars (48 bytes
 * each) go last.  With the condvars ahead of the state words, an 8-thread
 * read-only sqlxtc workload lost 20% (test_snap_registry: 1.89M -> 1.51M
 * txns/s); with this order and line-aligned objects it gains 38%. */
struct xtc_arwlock {
	pthread_mutex_t        lock;
	_Atomic int            readers;   /* active shared holders */
	_Atomic int            writer;    /* 1 if exclusive held */
	int                    rd_waiters; /* threads asleep on cv */
	int                    wr_waiters; /* threads asleep on wcv */
	struct fw_queue        wq;        /* fiber waiters */
	int                    last_excl; /* the last release was exclusive */
	_Atomic int            spinners;  /* threads in the off-loop spin */
	pthread_cond_t         cv;        /* off-loop shared waiters */
	pthread_cond_t         wcv;       /* off-loop exclusive waiters */
};

/* readers / writer are written under lock; spinners peek without it. */
#define ARW_R(r) atomic_load_explicit(&(r)->readers, memory_order_relaxed)
#define ARW_W(r) atomic_load_explicit(&(r)->writer, memory_order_relaxed)

/* Take one hold in `mode` (under r->lock). */
static void
__arw_take(xtc_arwlock_t *r, int mode)
{
	if (mode == ARW_EXCL)
		atomic_store_explicit(&r->writer, 1, memory_order_relaxed);
	else
		atomic_store_explicit(&r->readers, ARW_R(r) + 1,
		    memory_order_relaxed);
}

int
xtc_arwlock_create(xtc_arwlock_t **out)
{
	xtc_arwlock_t *r;
	int rc;
	if (out == NULL) return XTC_E_INVAL;
	if ((rc = sync_obj_alloc(sizeof *r, (void **)&r)) != XTC_OK)
		return rc;
	(void)pthread_mutex_init(&r->lock, NULL);
	(void)pthread_cond_init(&r->cv, NULL);
	(void)pthread_cond_init(&r->wcv, NULL);
	*out = r;
	return XTC_OK;
}

void
xtc_arwlock_destroy(xtc_arwlock_t *r)
{
	if (r == NULL) return;
	(void)pthread_cond_destroy(&r->wcv);
	(void)pthread_cond_destroy(&r->cv);
	(void)pthread_mutex_destroy(&r->lock);
	__os_aligned_free(r);
}

/* Can `mode` be granted right now, ignoring queue order? */
static int
__arw_compatible(xtc_arwlock_t *r, int mode)
{
	return mode == ARW_EXCL ? (!ARW_W(r) && ARW_R(r) == 0)
	                        : (!ARW_W(r));
}

/* Wake the off-loop waiters that can enter now (see the arwlock
 * comment).  Under r->lock. */
static void
__arw_wake_threads_locked(xtc_arwlock_t *r)
{
	int writer_next;
	if ((r->rd_waiters | r->wr_waiters) == 0 || r->wq.head != NULL ||
	    ARW_W(r))
		return;   /* none asleep; fiber waiters first; or still held X */
	if (ARW_R(r) > 0) {
		/* Read-held: sleeping readers may join, unless a writer
		 * sleeps (the last reader out wakes it). */
		if (r->wr_waiters == 0 && r->rd_waiters > 0)
			(void)pthread_cond_broadcast(&r->cv);
		return;
	}
	writer_next = r->wr_waiters > 0 && (r->rd_waiters == 0 || !r->last_excl);
	if (writer_next)
		(void)pthread_cond_signal(&r->wcv);
	else if (r->rd_waiters > 0)
		(void)pthread_cond_broadcast(&r->cv);
}

/* Grant as many head fiber waiters as the current state allows and wake
 * them (under r->lock: a granted waiter cannot return and release its
 * stack frame until it re-takes r->lock).  Wakes off-loop waiters too
 * when no fiber waiter is queued ahead. */
static void
__arw_grant_locked(xtc_arwlock_t *r)
{
	struct fiber_waiter *w;
	while ((w = r->wq.head) != NULL) {
		if (!__arw_compatible(r, w->mode))
			break;
		fw_remove(&r->wq, w);
		__arw_take(r, w->mode);
		w->granted = 1;
		(void)xtc_waker_wake(&w->waker);
		if (w->mode == ARW_EXCL)
			break;          /* exclusive: nothing after can join */
	}
	__arw_wake_threads_locked(r);
}

/* Drop one hold of the latch (under r->lock). */
static void
__arw_release_locked(xtc_arwlock_t *r)
{
	if ((r->rd_waiters | r->wr_waiters) != 0)
		r->last_excl = ARW_W(r);   /* only the wake phase reads it */
	if (ARW_W(r))
		atomic_store_explicit(&r->writer, 0, memory_order_relaxed);
	else if (ARW_R(r) > 0)
		atomic_store_explicit(&r->readers, ARW_R(r) - 1,
		    memory_order_relaxed);
}

/* Can an off-loop waiter take `mode` now (under r->lock)? */
#define ARW_THREAD_OK(r, mode) \
	((r)->wq.head == NULL && __arw_compatible((r), (mode)))

/* Off-loop slow path, called and returning with r->lock held: spin (TTAS
 * on readers / writer, the inner mutex dropped) once per sleep, then
 * sleep on the mode's condvar.  A timed-out waiter leaves only while the
 * latch is held (else it takes it), so the holder's release passes on
 * any wake it consumed. */
static int
__arw_lock_thread(xtc_arwlock_t *r, int mode, int64_t timeout_ns)
{
	struct timespec ts;
	pthread_cond_t *cv = mode == ARW_EXCL ? &r->wcv : &r->cv;
	int *nwait = mode == ARW_EXCL ? &r->wr_waiters : &r->rd_waiters;
	int i, got, spun = 0, expired, rounds;

	if (timeout_ns > 0)
		deadline_from_timeout(timeout_ns, &ts);
	while (!ARW_THREAD_OK(r, mode)) {
		if (!spun && r->wq.head == NULL &&
		    sync_spin_enter(&r->spinners)) {
			spun = 1;
			got = 0;
			(void)__xtc_mtx_unlock(&r->lock);
			rounds = sync_spin_rounds(SYNC_SPIN_ROUNDS);
			for (i = 0; i < rounds && !got; i++) {
				sync_spin_hint();
				if (!__arw_compatible(r, mode) ||
				    !sync_mtx_trylock(&r->lock))
					continue;
				if (ARW_THREAD_OK(r, mode))
					got = 1;        /* exit with r->lock held */
				else
					(void)__xtc_mtx_unlock(&r->lock);
			}
			sync_spin_leave(&r->spinners);
			if (!got)
				(void)__xtc_mtx_lock(&r->lock);
			continue;
		}
		(*nwait)++;
		expired = 0;
		XTC_PROBE_LOCK_WAIT_START((uintptr_t)r, XTC_PROBE_LOCK_ARWLOCK,
		    mode, (uintptr_t)0);
		if (timeout_ns < 0)
			(void)pthread_cond_wait(cv, &r->lock);
		else
			expired = cv_wait_until(cv, &r->lock, &ts);
		XTC_PROBE_LOCK_WAIT_DONE((uintptr_t)r, XTC_PROBE_LOCK_ARWLOCK,
		    mode, (uintptr_t)0);
		(*nwait)--;
		spun = 0;
		if (expired && !ARW_THREAD_OK(r, mode))
			return XTC_E_AGAIN;
	}
	__arw_take(r, mode);
	/* A reader that got in lets in readers that fell asleep after the
	 * broadcast that woke it (no-op if a writer sleeps). */
	if (mode == ARW_SHARED)
		__arw_wake_threads_locked(r);
	return XTC_OK;
}

static int
__arwlock_lock(xtc_arwlock_t *r, int mode, int64_t timeout_ns)
{
	xtc_task_t *cur;
	struct fiber_waiter w;
	int64_t deadline, now = 0;
	int masked, rc;

	if (r == NULL) return XTC_E_INVAL;

	(void)__xtc_mtx_lock(&r->lock);
	/* Fast path: compatible AND nobody queued ahead (FIFO fairness). */
	if (r->wq.head == NULL && __arw_compatible(r, mode)) {
		__arw_take(r, mode);
		(void)__xtc_mtx_unlock(&r->lock);
		return XTC_OK;
	}
	if (timeout_ns == 0) {
		(void)__xtc_mtx_unlock(&r->lock);
		return XTC_E_AGAIN;
	}

	cur = __xtc_current_task();
	if (cur == NULL) {
		/* Off-loop: spin, then condvar wait, yielding to any queued
		 * fiber waiter. */
		rc = __arw_lock_thread(r, mode, timeout_ns);
		(void)__xtc_mtx_unlock(&r->lock);
		return rc;
	}

	/* Fiber waiter: queue and park until granted. */
	memset(&w, 0, sizeof w);
	w.mode = mode;
	deadline = fw_deadline(timeout_ns);
	masked = fw_mask_enter();
	for (;;) {
		if (fw_kill_pending(masked)) {
			fw_remove(&r->wq, &w);
			if (w.granted) {               /* give the hold back */
				w.granted = 0;
				__arw_release_locked(r);
			}
			/* Leaving may unblock waiters behind us. */
			__arw_grant_locked(r);
			__xtc_task_cancel_park_timer(cur);
			(void)__xtc_mtx_unlock(&r->lock);
			fw_kill_deliver(&masked);
			(void)__xtc_mtx_lock(&r->lock);
			continue;
		}
		if (!w.granted && !w.queued) {
			/* First pass, or back from a deferred kill. */
			if (r->wq.head == NULL && __arw_compatible(r, mode)) {
				__arw_take(r, mode);
				w.granted = 1;
			} else {
				fw_enqueue(&r->wq, &w);
			}
		}
		if (w.granted) { rc = XTC_OK; break; }
		if (fw_expired(deadline, &now)) {
			fw_remove(&r->wq, &w);
			__arw_grant_locked(r);
			rc = XTC_E_AGAIN;
			break;
		}
		XTC_PROBE_LOCK_WAIT_START((uintptr_t)r, XTC_PROBE_LOCK_ARWLOCK,
		    mode, (uintptr_t)cur);
		fw_park(&r->lock, cur, &w.waker, deadline, now);
		XTC_PROBE_LOCK_WAIT_DONE((uintptr_t)r, XTC_PROBE_LOCK_ARWLOCK,
		    mode, (uintptr_t)cur);
	}
	/* Every exit cancels the park timer (see xtc_amutex_lock). */
	__xtc_task_cancel_park_timer(cur);
	(void)__xtc_mtx_unlock(&r->lock);
	fw_mask_leave(masked);
	return rc;
}

int
xtc_arwlock_rdlock(xtc_arwlock_t *r, int64_t timeout_ns)
{
	return __arwlock_lock(r, ARW_SHARED, timeout_ns);
}

int
xtc_arwlock_wrlock(xtc_arwlock_t *r, int64_t timeout_ns)
{
	return __arwlock_lock(r, ARW_EXCL, timeout_ns);
}

int
xtc_arwlock_unlock(xtc_arwlock_t *r)
{
	if (r == NULL) return XTC_E_INVAL;
	(void)__xtc_mtx_lock(&r->lock);
	__arw_release_locked(r);
	__arw_grant_locked(r);
	(void)__xtc_mtx_unlock(&r->lock);
	return XTC_OK;
}

/* ----- rwlock (writer-priority) -------------------------------- */

struct xtc_rwlock {
	pthread_mutex_t lock;
	pthread_cond_t  cv;
	int             readers;       /* current readers */
	int             writer;        /* 1 if a writer holds */
	int             waiting_writers; /* writers blocked */
};

int
xtc_rwlock_create(xtc_rwlock_t **out)
{
	xtc_rwlock_t *r;
	int rc;
	if (out == NULL) return XTC_E_INVAL;
	if ((rc = __os_calloc(1, sizeof *r, (void **)&r)) != XTC_OK) return rc;
	(void)pthread_mutex_init(&r->lock, NULL);
	(void)pthread_cond_init(&r->cv, NULL);
	*out = r;
	return XTC_OK;
}

void
xtc_rwlock_destroy(xtc_rwlock_t *r)
{
	if (r == NULL) return;
	(void)pthread_cond_destroy(&r->cv);
	(void)pthread_mutex_destroy(&r->lock);
	__os_free(r);
}

static int
__rwlock_wait(xtc_rwlock_t *r, int64_t timeout_ns,
              int (*ready)(xtc_rwlock_t *))
{
	if (timeout_ns == 0) return ready(r) ? XTC_OK : XTC_E_AGAIN;
	if (timeout_ns < 0) {
		while (!ready(r)) (void)pthread_cond_wait(&r->cv, &r->lock);
		return XTC_OK;
	}
	{
		struct timespec ts;
		deadline_from_timeout(timeout_ns, &ts);
		while (!ready(r)) {
			if (cv_wait_until(&r->cv, &r->lock, &ts))
				return XTC_E_AGAIN;
		}
		return XTC_OK;
	}
}

static int __can_grant_read(xtc_rwlock_t *r) {
	return !r->writer && r->waiting_writers == 0;
}
static int __can_grant_write(xtc_rwlock_t *r) {
	return !r->writer && r->readers == 0;
}

int
xtc_rwlock_rdlock(xtc_rwlock_t *r, int64_t timeout_ns)
{
	int rc;
	if (r == NULL) return XTC_E_INVAL;
	(void)__xtc_mtx_lock(&r->lock);
	rc = __rwlock_wait(r, timeout_ns, __can_grant_read);
	if (rc == XTC_OK) r->readers++;
	(void)__xtc_mtx_unlock(&r->lock);
	return rc;
}

int
xtc_rwlock_wrlock(xtc_rwlock_t *r, int64_t timeout_ns)
{
	int rc;
	if (r == NULL) return XTC_E_INVAL;
	(void)__xtc_mtx_lock(&r->lock);
	r->waiting_writers++;
	rc = __rwlock_wait(r, timeout_ns, __can_grant_write);
	r->waiting_writers--;
	if (rc == XTC_OK) r->writer = 1;
	else (void)pthread_cond_broadcast(&r->cv); /* let other readers in */
	(void)__xtc_mtx_unlock(&r->lock);
	return rc;
}

int
xtc_rwlock_unlock(xtc_rwlock_t *r)
{
	if (r == NULL) return XTC_E_INVAL;
	(void)__xtc_mtx_lock(&r->lock);
	if (r->writer) r->writer = 0;
	else if (r->readers > 0) r->readers--;
	(void)pthread_cond_broadcast(&r->cv);
	(void)__xtc_mtx_unlock(&r->lock);
	return XTC_OK;
}

/* ----- barrier --------------------------------------------------- */

struct xtc_barrier {
	pthread_mutex_t lock;
	pthread_cond_t  cv;
	unsigned        target;
	unsigned        arrived;
	unsigned        generation;
	struct fw_queue wq;           /* fiber parties */
};

int
xtc_barrier_create(unsigned n, xtc_barrier_t **out)
{
	xtc_barrier_t *b;
	int rc;
	if (out == NULL || n == 0) return XTC_E_INVAL;
	if ((rc = __os_calloc(1, sizeof *b, (void **)&b)) != XTC_OK) return rc;
	(void)pthread_mutex_init(&b->lock, NULL);
	(void)pthread_cond_init(&b->cv, NULL);
	b->target = n;
	*out = b;
	return XTC_OK;
}

void
xtc_barrier_destroy(xtc_barrier_t *b)
{
	if (b == NULL) return;
	(void)pthread_cond_destroy(&b->cv);
	(void)pthread_mutex_destroy(&b->lock);
	__os_free(b);
}

int
xtc_barrier_wait(xtc_barrier_t *b)
{
	unsigned gen;
	xtc_task_t *cur;
	struct fiber_waiter w;
	int masked;

	if (b == NULL) return XTC_E_INVAL;
	(void)__xtc_mtx_lock(&b->lock);
	gen = b->generation;
	b->arrived++;
	if (b->arrived == b->target) {
		/* Last party: release everyone (all parties together). */
		b->arrived = 0;
		b->generation++;
		(void)pthread_cond_broadcast(&b->cv);
		fw_wake_all(&b->wq);
		(void)__xtc_mtx_unlock(&b->lock);
		return XTC_OK;
	}

	cur = __xtc_current_task();
	if (cur != NULL) {
		/* Fiber party: park until the generation advances (the
		 * barrier has no timeout, so no park timer).  A killed
		 * party's arrival stands: the generation still completes when
		 * the remaining parties arrive. */
		memset(&w, 0, sizeof w);
		masked = fw_mask_enter();
		while (gen == b->generation) {
			if (fw_kill_pending(masked)) {
				fw_remove(&b->wq, &w);
				(void)__xtc_mtx_unlock(&b->lock);
				fw_kill_deliver(&masked);
				(void)__xtc_mtx_lock(&b->lock);
				continue;
			}
			fw_enqueue(&b->wq, &w);
			fw_park(&b->lock, cur, &w.waker, -1, 0);
		}
		fw_remove(&b->wq, &w);
		(void)__xtc_mtx_unlock(&b->lock);
		fw_mask_leave(masked);
		return XTC_OK;
	}

	while (gen == b->generation)
		(void)pthread_cond_wait(&b->cv, &b->lock);
	(void)__xtc_mtx_unlock(&b->lock);
	return XTC_OK;
}

/* ----- gate ------------------------------------------------------- */

struct xtc_gate {
	pthread_mutex_t lock;
	pthread_cond_t  cv;
	int             count;
	int             closed;
	struct fw_queue wq;           /* fiber drainers */
};

int
xtc_gate_create(xtc_gate_t **out)
{
	xtc_gate_t *g;
	int rc;
	if (out == NULL) return XTC_E_INVAL;
	if ((rc = __os_calloc(1, sizeof *g, (void **)&g)) != XTC_OK) return rc;
	(void)pthread_mutex_init(&g->lock, NULL);
	(void)pthread_cond_init(&g->cv, NULL);
	*out = g;
	return XTC_OK;
}

void
xtc_gate_destroy(xtc_gate_t *g)
{
	if (g == NULL) return;
	(void)pthread_cond_destroy(&g->cv);
	(void)pthread_mutex_destroy(&g->lock);
	__os_free(g);
}

int
xtc_gate_enter(xtc_gate_t *g)
{
	int rc = XTC_OK;
	if (g == NULL) return XTC_E_INVAL;
	(void)__xtc_mtx_lock(&g->lock);
	if (g->closed) rc = XTC_E_INVAL;
	else g->count++;
	(void)__xtc_mtx_unlock(&g->lock);
	return rc;
}

int
xtc_gate_leave(xtc_gate_t *g)
{
	if (g == NULL) return XTC_E_INVAL;
	(void)__xtc_mtx_lock(&g->lock);
	if (g->count > 0) g->count--;
	/* Wake drainers whenever the count reaches 0, closed or not: drain
	 * waits for count == 0, and a drain on an open gate used to sleep
	 * through it.  Drainers re-check and re-park if a new entry beat
	 * them to it. */
	if (g->count == 0) {
		(void)pthread_cond_broadcast(&g->cv);
		fw_wake_all(&g->wq);
	}
	(void)__xtc_mtx_unlock(&g->lock);
	return XTC_OK;
}

int
xtc_gate_close(xtc_gate_t *g)
{
	if (g == NULL) return XTC_E_INVAL;
	(void)__xtc_mtx_lock(&g->lock);
	g->closed = 1;
	(void)pthread_cond_broadcast(&g->cv);
	if (g->count == 0)                   /* already drained */
		fw_wake_all(&g->wq);
	(void)__xtc_mtx_unlock(&g->lock);
	return XTC_OK;
}

int
xtc_gate_drain(xtc_gate_t *g, int64_t timeout_ns)
{
	int rc = XTC_OK;
	xtc_task_t *cur;
	struct fiber_waiter w;
	int64_t deadline, now = 0;
	int masked;

	if (g == NULL) return XTC_E_INVAL;
	(void)__xtc_mtx_lock(&g->lock);
	if (g->count == 0) { (void)__xtc_mtx_unlock(&g->lock); return XTC_OK; }
	if (timeout_ns == 0) {
		(void)__xtc_mtx_unlock(&g->lock);
		return XTC_E_AGAIN;
	}

	cur = __xtc_current_task();
	if (cur != NULL) {
		/* Fiber drainer: park until count hits 0. */
		memset(&w, 0, sizeof w);
		deadline = fw_deadline(timeout_ns);
		masked = fw_mask_enter();
		for (;;) {
			if (fw_kill_pending(masked)) {
				fw_remove(&g->wq, &w);
				__xtc_task_cancel_park_timer(cur);
				(void)__xtc_mtx_unlock(&g->lock);
				fw_kill_deliver(&masked);
				(void)__xtc_mtx_lock(&g->lock);
				continue;
			}
			if (g->count == 0) { rc = XTC_OK; break; }
			if (fw_expired(deadline, &now)) { rc = XTC_E_AGAIN; break; }
			fw_enqueue(&g->wq, &w);
			fw_park(&g->lock, cur, &w.waker, deadline, now);
		}
		fw_remove(&g->wq, &w);
		__xtc_task_cancel_park_timer(cur);
		(void)__xtc_mtx_unlock(&g->lock);
		fw_mask_leave(masked);
		return rc;
	}

	if (timeout_ns < 0) {
		while (g->count > 0)
			(void)pthread_cond_wait(&g->cv, &g->lock);
	} else {
		struct timespec ts;
		deadline_from_timeout(timeout_ns, &ts);
		while (g->count > 0) {
			if (cv_wait_until(&g->cv, &g->lock, &ts)) {
				rc = XTC_E_AGAIN; break;
			}
		}
	}
	(void)__xtc_mtx_unlock(&g->lock);
	return rc;
}

int
xtc_gate_count(const xtc_gate_t *g)
{
	int v;
	if (g == NULL) return 0;
	(void)__xtc_mtx_lock((pthread_mutex_t *)&g->lock);
	v = g->count;
	(void)__xtc_mtx_unlock((pthread_mutex_t *)&g->lock);
	return v;
}
