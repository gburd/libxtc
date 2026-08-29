/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/ptc/blocking.c
 *	Blocking-work thread pool (see xtc_blocking.h).
 *
 *	A small fixed pool of worker threads drains a FIFO queue of
 *	work items.  xtc_blocking_run enqueues one, then parks the
 *	calling process on a pipe via xtc_proc_wait_fd; the worker
 *	runs the function, stores the result, and writes one byte to
 *	the pipe, which wakes the process on its own loop.  The pool
 *	threads are the only ones that ever block in the user's call,
 *	so the loop threads stay free to run other work.
 */

#include "xtc_int.h"
#include "preempt_int.h"   /* __xtc_unsafe_* / __xtc_mtx_*: internal preemption brackets */
#include "xtc_blocking.h"
#include "xtc_proc.h"
#include "xtc_io.h"
#include "xtc_sim.h"   /* __xtc_sim_active: run inline under simulation */

#include "os_thread.h"
#include "os_cpu.h"

#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <errno.h>

#if defined(_WIN32)
/* Windows has no POSIX pipe(); _pipe() in <io.h> is the equivalent.
 * Use a binary pipe with a small buffer (this is a 1-byte wakeup
 * channel).  Works under both MinGW and MSVC without the compat shim
 * being on the include path. */
#include <io.h>
#include <fcntl.h>
static __inline int xtc__blk_pipe(int fds[2])
{ return _pipe(fds, 4096, _O_BINARY); }
#undef pipe
#define pipe(fds) xtc__blk_pipe(fds)
#endif

struct blk_work {
	int            (*fn)(void *);
	void            *arg;
	_Atomic int      result;
	_Atomic int      done;         /* set AFTER the wakeup write; the
	                                * caller must spin-park until this is
	                                * 1 -- a spurious wake (cross-thread
	                                * nudge / eager rebalance / stray
	                                * xtc_proc_wake) can resume the parked
	                                * caller before the worker has written
	                                * the pipe, and reading/closing then
	                                * would race the worker's write and
	                                * free the on-stack work item out from
	                                * under it (TSan-caught close-vs-write
	                                * on the completion pipe under
	                                * concurrent commit). */
	int              wr_fd;        /* write end of the wakeup pipe */
	int              detached;     /* 1: fire-and-forget; worker frees it */
	struct blk_work *next;
};

#define BLK_MAX_THREADS 64

static pthread_mutex_t   g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t    g_cv = PTHREAD_COND_INITIALIZER;
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

static void *
blk_worker(void *unused)
{
	(void)unused;
	for (;;) {
		struct blk_work *w;
		int fn_fd, r;

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
		r = w->fn(w->arg);
		if (w->detached) {
			/* Fire-and-forget: the submitter never waits and does
			 * not own w, so we free the heap work item here and
			 * discard the result.  No wakeup pipe to write. */
			__os_free(w);
			continue;
		}
		fn_fd = w->wr_fd;
		atomic_store_explicit(&w->result, r, memory_order_release);
		/* Publish completion with a release store to w->done BEFORE the
		 * wakeup write.  Ordering rationale:
		 *   - done-before-write is LOST-WAKE-FREE: the wake (the pipe
		 *     byte) is what resumes the parked caller, and by the time
		 *     any wake is observable, done is already visible -- so a
		 *     caller that wakes and checks done never re-parks with the
		 *     completion already delivered (the bug the other ordering
		 *     has: wake seen, done not yet stored, re-park forever --
		 *     which hung the Windows offloaded fdatasync).
		 *   - the CALLER makes teardown safe by waiting for done, then
		 *     doing a BLOCKING read of the byte before it closes: the
		 *     byte only exists after this write, so the close strictly
		 *     follows the write -- no close-vs-write race.
		 * The write is our LAST touch of w (and of the pipe); after it
		 * the caller owns w and may free it. */
		atomic_store_explicit(&w->done, 1, memory_order_release);
		{
			char b = 'x';
			ssize_t nw;
			do {
				/* One byte into a fresh pipe never blocks. */
				nw = write(fn_fd, &b, 1);  /* XTC_BLOCKING_OK */
			} while (nw < 0 && errno == EINTR);
		}
		atomic_store_explicit(&w->done, 1, memory_order_release);
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

int
xtc_blocking_run(int (*fn)(void *), void *arg, int *out_result)
{
	struct blk_work w;
	int pfd[2];
	uint32_t revents = 0;
	char drain[8];
	ssize_t n;

	if (fn == NULL)
		return XTC_E_INVAL;

	/* Under deterministic simulation there is no real thread pool to
	 * offload to (a pool worker runs on a real OS thread outside the
	 * sim's control, destroying determinism), so run the work
	 * synchronously on the calling fiber -- the same result the
	 * off-a-loop synchronous fallback already produces, and a pure
	 * function of the seed.  ADDITIVE: gated on __xtc_sim_active(); the
	 * production pool path below is byte-identical. */
	if (__xtc_sim_active())
		goto run_sync;

	/* Synchronous fallback: not on a loop process (cannot park), or
	 * the wakeup pipe / pool could not be set up. */
	if (xtc_pid_is_none(xtc_self()))
		goto run_sync;
	if (pipe(pfd) != 0)
		goto run_sync;

	(void)__xtc_mtx_lock(&g_lock);
	if (blk_start_locked() != 0) {
		(void)__xtc_mtx_unlock(&g_lock);
		(void)close(pfd[0]);
		(void)close(pfd[1]);
		goto run_sync;
	}
	w.fn = fn;
	w.arg = arg;
	atomic_store_explicit(&w.result, 0, memory_order_relaxed);
	atomic_store_explicit(&w.done, 0, memory_order_relaxed);
	w.wr_fd = pfd[1];
	w.detached = 0;        /* synchronous: the caller owns w and waits */
	w.next = NULL;
	if (g_tail != NULL)
		g_tail->next = &w;
	else
		g_head = &w;
	g_tail = &w;
	g_qlen++;
	blk_grow_locked();
	(void)pthread_cond_signal(&g_cv);
	(void)__xtc_mtx_unlock(&g_lock);

	/* Park until the worker publishes completion (w.done), then do a
	 * BLOCKING read of the wakeup byte, then close.  w.done -- not the
	 * wake -- is the completion token: xtc_proc_wait_fd may return
	 * spuriously (a cross-thread nudge, eager-rebalance poke, or stray
	 * xtc_proc_wake are all documented to cause spurious wakes the
	 * waiter must re-evaluate), so we re-park until done is set.  The
	 * worker stores done (release) BEFORE its wakeup write, so this is
	 * lost-wake-free: whenever a wake is observable, done is already
	 * visible, so we never re-park with the completion already delivered.
	 * The blocking read below then waits for the byte -- which the worker
	 * writes AFTER the done store -- so our close strictly follows the
	 * worker's write and cannot race it, and w is not freed until the
	 * worker is finished with it. */
	while (!atomic_load_explicit(&w.done, memory_order_acquire)) {
		revents = 0;
		(void)xtc_proc_wait_fd(pfd[0], XTC_IO_READABLE, -1, &revents);
	}
	do {
		/* done is set; the worker's byte write follows it and this
		 * blocking read waits for that byte, synchronizing our close
		 * after the worker's last pipe touch. */
		n = read(pfd[0], drain, sizeof drain);  /* XTC_BLOCKING_OK */
	} while (n < 0 && errno == EINTR);

	if (out_result != NULL)
		*out_result = atomic_load_explicit(&w.result,
		    memory_order_acquire);
	(void)close(pfd[0]);
	(void)close(pfd[1]);
	return XTC_OK;

run_sync:
	{
		int r = fn(arg);
		if (out_result != NULL)
			*out_result = r;
	}
	return XTC_OK;
}

/*
 * PUBLIC: int xtc_blocking_run_off_loop __P((int (*)(void *), void *, int *));
 *
 * Like xtc_blocking_run, but the CALLER is a plain OS thread that is not
 * a libxtc fiber/loop process (xtc_self() == none): offload `fn(arg)` to
 * the pool and block THIS thread on the completion pipe with a real
 * read(2), rather than parking a fiber (which a bare thread cannot do).
 * The pool worker runs the blocking call, so `fn` no longer runs inline
 * on the caller -- useful for a consumer that multiplexes many logical
 * tasks onto one OS thread and wants the blocking syscall (an fsync, a
 * getaddrinfo) to run OFF that thread on a pool worker.
 *
 * Contract / caveats the caller must understand:
 *   - It STILL blocks the calling thread until the work completes (it is
 *     synchronous from the caller's view; it just does not run `fn`
 *     inline).  It does NOT let the caller do other work during the
 *     call -- only a fiber on a loop gets that (use xtc_blocking_run
 *     from a fiber for cooperative yielding).  If you multiplex N tasks
 *     on one thread and must keep serving the other N-1 during a
 *     blocking call, the answer is to make those tasks fibers on an
 *     xtc loop, not this primitive.
 *   - It does NOT shorten any lock the caller holds across the call: an
 *     offloaded fsync under an exclusive lock holds that lock for the
 *     same wall time.  This primitive is a building block for a
 *     group-commit/batching design (submit once, wait once, wake many),
 *     not a fix for lock-hold-across-blocking-syscall coupling.
 *   - Callable ONLY off a loop.  If called from a fiber it returns
 *     XTC_E_INVAL (use xtc_blocking_run there, which yields).
 *
 * Returns XTC_OK (result via out_result) on success; XTC_E_INVAL (fn
 * NULL, or called on a loop); on a pool/pipe setup failure it degrades
 * to running `fn` inline on the caller (same as the off-a-loop fallback)
 * and returns XTC_OK.  Never runs under deterministic simulation on a
 * loop (there is no bare-thread caller there).
 */
int
xtc_blocking_run_off_loop(int (*fn)(void *), void *arg, int *out_result)
{
	struct blk_work w;
	int pfd[2];
	char drain[8];
	ssize_t n;

	if (fn == NULL)
		return XTC_E_INVAL;
	/* This entry point is for NON-fiber callers; a fiber must use
	 * xtc_blocking_run so it yields the loop rather than blocking the
	 * carrier thread. */
	if (!xtc_pid_is_none(xtc_self()))
		return XTC_E_INVAL;
	if (pipe(pfd) != 0)
		goto run_sync;

	(void)__xtc_mtx_lock(&g_lock);
	if (blk_start_locked() != 0) {
		(void)__xtc_mtx_unlock(&g_lock);
		(void)close(pfd[0]);
		(void)close(pfd[1]);
		goto run_sync;
	}
	w.fn = fn;
	w.arg = arg;
	atomic_store_explicit(&w.result, 0, memory_order_relaxed);
	atomic_store_explicit(&w.done, 0, memory_order_relaxed);
	w.wr_fd = pfd[1];
	w.detached = 0;        /* the caller owns w and waits */
	w.next = NULL;
	if (g_tail != NULL)
		g_tail->next = &w;
	else
		g_head = &w;
	g_tail = &w;
	g_qlen++;
	blk_grow_locked();
	(void)pthread_cond_signal(&g_cv);
	(void)__xtc_mtx_unlock(&g_lock);

	/* Block THIS OS thread on the completion pipe (no fiber to park).
	 * The worker writes one byte after storing the result. */
	do {
		n = read(pfd[0], drain, sizeof drain);  /* XTC_BLOCKING_OK */
	} while (n < 0 && errno == EINTR);

	if (out_result != NULL)
		*out_result = atomic_load_explicit(&w.result,
		    memory_order_acquire);
	(void)close(pfd[0]);
	(void)close(pfd[1]);
	return XTC_OK;

run_sync:
	{
		int r = fn(arg);
		if (out_result != NULL)
			*out_result = r;
	}
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
 * completion signal.  Returns XTC_E_INVAL (fn NULL), XTC_E_NOMEM, or
 * XTC_E_INTERNAL (pool could not start).
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
	w->wr_fd = -1;
	w->detached = 1;
	w->next = NULL;

	(void)__xtc_mtx_lock(&g_lock);
	if (blk_start_locked() != 0) {
		(void)__xtc_mtx_unlock(&g_lock);
		__os_free(w);
		return XTC_E_INTERNAL;
	}
	if (g_tail != NULL)
		g_tail->next = w;
	else
		g_head = w;
	g_tail = w;
	g_qlen++;
	blk_grow_locked();
	(void)pthread_cond_signal(&g_cv);
	(void)__xtc_mtx_unlock(&g_lock);
	return XTC_OK;
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
