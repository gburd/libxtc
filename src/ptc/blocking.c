/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/ptc/blocking.c
 *	Blocking-work thread pool (see xtc_blocking.h).
 *
 *	A small fixed pool of worker threads drains a FIFO queue of
 *	work items.  xtc_blocking_run enqueues one, then parks the
 *	calling fiber on its task waker; the worker runs the function,
 *	stores the result, and fires the waker, which reschedules the
 *	fiber on its own loop.  The pool threads are the only ones that
 *	ever block in the user's call, so the loop threads stay free to
 *	run other work.
 *
 *	There is no per-call file descriptor.  Each call used to open a
 *	pipe (2 fds) for its wakeup: 1000 concurrent offloads held 2000
 *	fds, and when pipe() hit EMFILE the call silently ran fn ON THE
 *	LOOP THREAD (153 of 200 calls at ulimit -n 100), stalling every
 *	other fiber on the loop exactly when the process was already
 *	under fd pressure.  It also carried a close-vs-write race on the
 *	pipe that TSan reported and that had to be suppressed.
 *
 *	Completion handshake.  The work item lives on the caller's stack.
 *	The worker sets w->done and fires the waker while holding
 *	g_done_lock, and the caller only reads w->done under that lock,
 *	so the caller cannot see completion -- and return, freeing w --
 *	until the worker has released the lock and stopped touching w.
 *	An off-loop caller waits on g_done_cv under the same lock.
 */

#include "xtc_int.h"
#include "preempt_int.h"   /* __xtc_unsafe_* / __xtc_mtx_*: internal preemption brackets */
#include "xtc_blocking.h"
#include "xtc_proc.h"
#include "xtc_loop.h"   /* xtc_task_waker / xtc_waker_wake */
#include "xtc_async.h"  /* xtc_yield */
#include "proc_int.h"   /* __xtc_proc_ctx_save/restore */
#include "loop_int.h"   /* struct xtc_task: park_requested */
#include "coro_int.h"   /* __xtc_current_task */
#include "xtc_sim.h"   /* __xtc_sim_active: run inline under simulation */
#include "xtc_probes.h"

#include "os_thread.h"
#include "os_cpu.h"
#include "os_time.h"    /* __os_sleep_ns */

#include <pthread.h>
#include <stdatomic.h>
#include <errno.h>

struct blk_work {
	int            (*fn)(void *);
	void            *arg;
	int              result;       /* written before done, under g_done_lock */
	int              done;         /* g_done_lock */
	xtc_waker_t      waker;        /* g_done_lock; fiber caller only */
	int              fiber;        /* 1: fiber caller (wake waker), 0: thread */
	int              detached;     /* 1: fire-and-forget; worker frees it */
	struct blk_work *next;
};

#define BLK_MAX_THREADS 64

static pthread_mutex_t   g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t    g_cv = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t   g_done_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t    g_done_cv = PTHREAD_COND_INITIALIZER;
static struct blk_work  *g_head;          /* queue head (dequeue) */
static struct blk_work  *g_tail;          /* queue tail (enqueue) */
static __os_thread_t     g_threads[BLK_MAX_THREADS];
static int               g_nthreads;      /* user-configured size, 0 = auto */
static int               g_nstarted;      /* threads actually running */
static int               g_started;       /* pool up? */
static int               g_stopping;      /* shutdown in progress */
static int               g_max_threads;   /* growth cap (auto or configured) */
static int               g_idle;          /* workers parked in cond_wait */
static int               g_qlen;          /* items waiting in the queue */

/*
 * Auto pool size: scale with the host's CPU count so the offload path
 * is not an artificial bottleneck on a large machine, with a floor so a
 * small one still has a few workers.  Blocking-I/O threads are mostly
 * parked in the kernel rather than CPU-bound, so a count near the core
 * count is a reasonable steady-state floor; the pool also grows on
 * demand up to BLK_MAX_THREADS when work queues up (see blk_grow).
 */
static int
blk_auto_size(void)
{
	int n = __os_ncpus();
	if (n < 4) n = 4;                 /* floor */
	if (n > BLK_MAX_THREADS) n = BLK_MAX_THREADS;
	return n;
}

/* Publish w's result and wake its caller.  Last touch of w: once
 * g_done_lock is released the caller may return and free it. */
static void
blk_complete(struct blk_work *w, int r)
{
	(void)__xtc_mtx_lock(&g_done_lock);
	w->result = r;
	w->done = 1;
	if (w->fiber) {
		/* A cross-thread wake posts to the loop's inbox, which
		 * allocates; a dropped wake would park the caller forever,
		 * so wait out an allocation failure instead. */
		while (xtc_waker_wake(&w->waker) == XTC_E_NOMEM)
			(void)__os_sleep_ns(1000000);
	} else {
		(void)pthread_cond_broadcast(&g_done_cv);
	}
	(void)__xtc_mtx_unlock(&g_done_lock);
}

static void *
blk_worker(void *unused)
{
	(void)unused;
	for (;;) {
		struct blk_work *w;
		int r;

		(void)__xtc_mtx_lock(&g_lock);
		g_idle++;
		while (g_head == NULL && !g_stopping)
			(void)pthread_cond_wait(&g_cv, &g_lock);
		g_idle--;
		if (g_stopping && g_head == NULL) {
			(void)__xtc_mtx_unlock(&g_lock);
			return NULL;
		}
		w = g_head;
		g_head = w->next;
		if (g_head == NULL)
			g_tail = NULL;
		g_qlen--;
		(void)__xtc_mtx_unlock(&g_lock);

		/* Run the user's blocking call on this pool thread. */
		XTC_PROBE_BLOCKING_START((uintptr_t)w);
		r = w->fn(w->arg);
		XTC_PROBE_BLOCKING_DONE((uintptr_t)w, r);
		if (w->detached) {
			/* Fire-and-forget: the submitter never waits and does
			 * not own w, so we free the heap work item here and
			 * discard the result. */
			__os_free(w);
			continue;
		}
		blk_complete(w, r);
	}
}

/* Start the pool on first use.  Returns 0 on success.  Called with
 * g_lock held. */
static int
blk_start_locked(void)
{
	int i, n;

	if (g_started)
		return 0;
	/* Initial size: the user's explicit setting, or the CPU-scaled auto
	 * default.  Either way the pool may grow on demand up to the cap. */
	n = (g_nthreads > 0) ? g_nthreads : blk_auto_size();
	if (n < 1) n = 1;
	if (n > BLK_MAX_THREADS) n = BLK_MAX_THREADS;
	g_max_threads = (g_nthreads > 0) ? n : BLK_MAX_THREADS;
	g_stopping = 0;
	g_nstarted = 0;
	g_idle = 0;
	for (i = 0; i < n; i++) {
		if (__os_thread_create(&g_threads[i], blk_worker, NULL)
		    != XTC_OK)
			break;
		g_nstarted++;
	}
	if (g_nstarted == 0)
		return -1;
	g_started = 1;
	return 0;
}

/*
 * Grow the pool by one worker when work is queued and no idle thread is
 * waiting to take it, up to the cap.  Called with g_lock held, after an
 * item has been enqueued.  A spawn failure is harmless -- the item just
 * waits for an existing worker.  An explicitly configured pool
 * (g_nthreads > 0, g_max_threads == that size) does not grow past its
 * configured size.
 */
static void
blk_grow_locked(void)
{
	if (g_stopping || g_nstarted >= g_max_threads)
		return;
	/* Only add a thread if every started worker is (or is about to be)
	 * busy: more queued items than idle waiters. */
	if (g_qlen <= g_idle)
		return;
	if (__os_thread_create(&g_threads[g_nstarted], blk_worker, NULL)
	    == XTC_OK)
		g_nstarted++;
}

int
xtc_blocking_pool_size(int nthreads)
{
	int rc = XTC_OK;
	(void)__xtc_mtx_lock(&g_lock);
	if (g_started)
		rc = XTC_E_INVAL;          /* too late */
	else if (nthreads >= 1 && nthreads <= BLK_MAX_THREADS)
		g_nthreads = nthreads;
	else
		rc = XTC_E_INVAL;
	(void)__xtc_mtx_unlock(&g_lock);
	return rc;
}

/*
 * Queue bound.  The queue was unbounded: a burst of offloads faster than
 * the pool drains them grew it without limit.  A queued item is a caller
 * that waits (a parked fiber, a blocked thread) or a fire-and-forget
 * closure, so the bound is generous -- it exists to turn runaway backlog
 * into an error the caller can see, not to throttle normal use.
 */
#define BLK_QUEUE_MAX 65536

/*
 * Append w to the queue, starting the pool on first use.  XTC_E_AGAIN if
 * the queue is at BLK_QUEUE_MAX, XTC_E_RESOURCE if no pool thread could
 * be started.
 */
static int
blk_enqueue(struct blk_work *w)
{
	(void)__xtc_mtx_lock(&g_lock);
	if (blk_start_locked() != 0) {
		(void)__xtc_mtx_unlock(&g_lock);
		return XTC_E_RESOURCE;
	}
	if (g_qlen >= BLK_QUEUE_MAX) {
		(void)__xtc_mtx_unlock(&g_lock);
		return XTC_E_AGAIN;
	}
	w->next = NULL;
	if (g_tail != NULL)
		g_tail->next = w;
	else
		g_head = w;
	g_tail = w;
	g_qlen++;
	XTC_PROBE_BLOCKING_ENQUEUE((uintptr_t)w, g_qlen);
	blk_grow_locked();
	(void)pthread_cond_signal(&g_cv);
	(void)__xtc_mtx_unlock(&g_lock);
	return XTC_OK;
}

static int
blk_run_sync(int (*fn)(void *), void *arg, int *out_result)
{
	int r = fn(arg);
	if (out_result != NULL)
		*out_result = r;
	return XTC_OK;
}

int
xtc_blocking_run(int (*fn)(void *), void *arg, int *out_result)
{
	struct blk_work w;
	xtc_task_t *cur;
	void *proc_ctx;
	int rc, done;

	if (fn == NULL)
		return XTC_E_INVAL;

	/* Under deterministic simulation there is no real thread pool to
	 * offload to (a pool worker runs on a real OS thread outside the
	 * sim's control, destroying determinism), so run the work
	 * synchronously on the calling fiber -- the same result the
	 * off-a-loop synchronous fallback already produces, and a pure
	 * function of the seed.  The production pool path below is
	 * unaffected. */
	if (__xtc_sim_active())
		return blk_run_sync(fn, arg, out_result);

	/* Not on a fiber: nothing to park, run on this thread (documented). */
	cur = __xtc_current_task();
	if (xtc_pid_is_none(xtc_self()) || cur == NULL)
		return blk_run_sync(fn, arg, out_result);

	w.fn = fn;
	w.arg = arg;
	w.result = 0;
	w.done = 0;
	w.fiber = 1;
	w.detached = 0;        /* synchronous: the caller owns w and waits */
	(void)xtc_task_waker(cur, &w.waker);

	/*
	 * CANCELLATION: the whole window from enqueue to completion is
	 * MASKED.  `w` is STACK-OWNED by this frame and the pool worker reads
	 * and writes it until it drops g_done_lock in blk_complete.  An
	 * unmasked kill delivered during the park would unwind this frame
	 * while the worker still owned it.  MEASURED before (ASan, epoll,
	 * kill 50ms into a 400ms offload): the dead fiber's stack was reused,
	 * the worker read a garbage w->detached and called __os_free on a
	 * STACK address.
	 *
	 * A kill arriving during the wait is LATCHED and honored by
	 * xtc_mask_leave below, which does not return when one was latched
	 * -- so the fiber dies at the FIRST point where the worker no longer
	 * owns this frame.  That bound is the minimum achievable: the work
	 * runs on a pool thread that cannot be interrupted.  Masking does NOT
	 * hide a wedged offload from a supervisor: the latched kill sets
	 * mask_deferred while mask_depth is raised, which is the pair
	 * xtc_exit_pid_deadline reports as XTC_KILL_DEFERRED.
	 */
	(void)xtc_mask_enter();
	if ((rc = blk_enqueue(&w)) != XTC_OK) {
		(void)xtc_mask_leave();
		return rc;
	}
	/*
	 * Park until the worker publishes completion.  w.done, read under
	 * g_done_lock, is the completion token: a wake can also be spurious
	 * (a stray xtc_proc_wake, an eager rebalance), so re-park until it is
	 * set.  Requesting the park while holding the lock is lost-wake-free:
	 * a worker that completes after we drop it fires a waker that either
	 * finds us PARKED or latches wake_pending, which the PENDING verdict
	 * consumes instead of parking.
	 */
	for (;;) {
		(void)__xtc_mtx_lock(&g_done_lock);
		done = w.done;
		if (!done)
			cur->park_requested = 1;
		(void)__xtc_mtx_unlock(&g_done_lock);
		if (done)
			break;
		proc_ctx = __xtc_proc_ctx_save();
		XTC_PROBE_FIBER_PARK((uintptr_t)cur, XTC_PROBE_PARK_BLOCKING,
		    (uintptr_t)&w);
		xtc_yield();
		XTC_PROBE_FIBER_RESUME((uintptr_t)cur, XTC_PROBE_PARK_BLOCKING,
		    (uintptr_t)&w);
		__xtc_proc_ctx_restore(proc_ctx);
		/* The fiber may have been work-stolen while parked; re-aim the
		 * waker at the loop it now runs on (under the lock the worker
		 * reads it under). */
		(void)__xtc_mtx_lock(&g_done_lock);
		(void)xtc_task_waker(cur, &w.waker);
		(void)__xtc_mtx_unlock(&g_done_lock);
	}
	if (out_result != NULL)
		*out_result = w.result;
	/* The worker is done with `w`, so the frame is ours again: drop the
	 * mask and honor a kill latched during the wait.  This may NOT
	 * return (it unwinds via xtc_exit_self), which is why it comes after
	 * the result store. */
	(void)xtc_mask_leave();
	return XTC_OK;
}

/*
 * PUBLIC: int xtc_blocking_run_off_loop __P((int (*)(void *), void *, int *));
 *
 * Like xtc_blocking_run, but the CALLER is a plain OS thread that is not
 * a libxtc fiber/loop process (xtc_self() == none): offload `fn(arg)` to
 * the pool and block THIS thread until it completes, rather than parking
 * a fiber (which a bare thread cannot do).  The pool worker runs the
 * blocking call, so `fn` no longer runs inline on the caller -- useful
 * for a consumer that multiplexes many logical tasks onto one OS thread
 * and wants the blocking syscall (an fsync, a getaddrinfo) to run OFF
 * that thread on a pool worker.
 *
 * Contract / caveats the caller must understand:
 *   - It STILL blocks the calling thread until the work completes (it is
 *     synchronous from the caller's view; it just does not run `fn`
 *     inline).  It does NOT let the caller do other work during the
 *     call -- only a fiber on a loop gets that (use xtc_blocking_run
 *     from a fiber for cooperative yielding).
 *   - It does NOT shorten any lock the caller holds across the call: an
 *     offloaded fsync under an exclusive lock holds that lock for the
 *     same wall time.  This primitive is a building block for a
 *     group-commit/batching design (submit once, wait once, wake many),
 *     not a fix for lock-hold-across-blocking-syscall coupling.
 *   - Callable ONLY off a loop.  If called from a fiber it returns
 *     XTC_E_INVAL (use xtc_blocking_run there, which yields).
 *
 * Returns XTC_OK (result via out_result) on success; XTC_E_INVAL (fn
 * NULL, or called on a loop); XTC_E_AGAIN (the offload queue is full)
 * or XTC_E_RESOURCE (no pool thread could be started) without running
 * fn.
 */
int
xtc_blocking_run_off_loop(int (*fn)(void *), void *arg, int *out_result)
{
	struct blk_work w;
	int rc;

	if (fn == NULL)
		return XTC_E_INVAL;
	/* This entry point is for NON-fiber callers; a fiber must use
	 * xtc_blocking_run so it yields the loop rather than blocking the
	 * carrier thread. */
	if (!xtc_pid_is_none(xtc_self()))
		return XTC_E_INVAL;

	w.fn = fn;
	w.arg = arg;
	w.result = 0;
	w.done = 0;
	w.fiber = 0;
	w.detached = 0;        /* the caller owns w and waits */
	if ((rc = blk_enqueue(&w)) != XTC_OK)
		return rc;
	(void)__xtc_mtx_lock(&g_done_lock);
	while (!w.done)
		(void)pthread_cond_wait(&g_done_cv, &g_done_lock); /* XTC_BLOCKING_OK: off-loop by contract */
	if (out_result != NULL)
		*out_result = w.result;
	(void)__xtc_mtx_unlock(&g_done_lock);
	return XTC_OK;
}

/*
 * PUBLIC: int xtc_blocking_submit __P((int (*)(void *), void *));
 *
 * Fire-and-forget: hand `fn(arg)` to the offload pool and return
 * immediately without waiting for, or collecting, its result.  Unlike
 * xtc_blocking_run this never parks, so it is callable from any context
 * (on or off a loop, or a bare thread) -- e.g. read-ahead/prefetch that
 * must not block the caller.  The caller owns `arg`'s lifetime and must
 * keep it valid until `fn` has run (or have `fn` free it); there is no
 * completion signal.  Returns XTC_E_INVAL (fn NULL), XTC_E_NOMEM,
 * XTC_E_AGAIN (the offload queue is full) or XTC_E_RESOURCE (no pool
 * thread could be started); fn has not run on any error.
 */
int
xtc_blocking_submit(int (*fn)(void *), void *arg)
{
	struct blk_work *w;
	int rc;

	if (fn == NULL)
		return XTC_E_INVAL;
	if ((rc = __os_calloc(1, sizeof *w, (void **)&w)) != XTC_OK)
		return XTC_E_NOMEM;
	w->fn = fn;
	w->arg = arg;
	w->detached = 1;
	if ((rc = blk_enqueue(w)) != XTC_OK)
		__os_free(w);
	return rc;
}

void
xtc_blocking_shutdown(void)
{
	int i, n;
	__os_thread_t threads[BLK_MAX_THREADS];

	(void)__xtc_mtx_lock(&g_lock);
	if (!g_started) {
		(void)__xtc_mtx_unlock(&g_lock);
		return;
	}
	g_stopping = 1;
	n = g_nstarted;
	for (i = 0; i < n; i++)
		threads[i] = g_threads[i];
	(void)pthread_cond_broadcast(&g_cv);
	(void)__xtc_mtx_unlock(&g_lock);

	for (i = 0; i < n; i++)
		(void)__os_thread_join(&threads[i], NULL);

	(void)__xtc_mtx_lock(&g_lock);
	g_started = 0;
	g_nstarted = 0;
	g_stopping = 0;
	g_idle = 0;
	g_qlen = 0;
	/* g_nthreads (user-configured size) is intentionally preserved so a
	 * later restart honors it; g_max_threads is recomputed on restart. */
	(void)__xtc_mtx_unlock(&g_lock);
}
