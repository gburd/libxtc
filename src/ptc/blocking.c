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
#include "xtc_blocking.h"
#include "xtc_proc.h"
#include "xtc_io.h"

#include "os_thread.h"

#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>
#include <errno.h>

struct blk_work {
	int            (*fn)(void *);
	void            *arg;
	_Atomic int      result;
	int              wr_fd;        /* write end of the wakeup pipe */
	struct blk_work *next;
};

#define BLK_MAX_THREADS 64

static pthread_mutex_t   g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t    g_cv = PTHREAD_COND_INITIALIZER;
static struct blk_work  *g_head;          /* queue head (dequeue) */
static struct blk_work  *g_tail;          /* queue tail (enqueue) */
static __os_thread_t     g_threads[BLK_MAX_THREADS];
static int               g_nthreads = 4;  /* configured pool size */
static int               g_nstarted;      /* threads actually running */
static int               g_started;       /* pool up? */
static int               g_stopping;      /* shutdown in progress */

static void *
blk_worker(void *unused)
{
	(void)unused;
	for (;;) {
		struct blk_work *w;
		int fn_fd, r;

		(void)pthread_mutex_lock(&g_lock);
		while (g_head == NULL && !g_stopping)
			(void)pthread_cond_wait(&g_cv, &g_lock);
		if (g_stopping && g_head == NULL) {
			(void)pthread_mutex_unlock(&g_lock);
			return NULL;
		}
		w = g_head;
		g_head = w->next;
		if (g_head == NULL)
			g_tail = NULL;
		(void)pthread_mutex_unlock(&g_lock);

		/* Run the user's blocking call on this pool thread. */
		r = w->fn(w->arg);
		fn_fd = w->wr_fd;
		atomic_store_explicit(&w->result, r, memory_order_release);
		/* Wake the parked process.  After this byte is readable the
		 * caller owns w again, so do not touch w afterwards. */
		{
			char b = 'x';
			ssize_t n;
			do {
				/* One byte into a fresh pipe never blocks. */
				n = write(fn_fd, &b, 1);  /* XTC_BLOCKING_OK */
			} while (n < 0 && errno == EINTR);
		}
	}
}

/* Start the pool on first use.  Returns 0 on success. */
static int
blk_start_locked(void)
{
	int i, n;

	if (g_started)
		return 0;
	n = g_nthreads;
	if (n < 1) n = 1;
	if (n > BLK_MAX_THREADS) n = BLK_MAX_THREADS;
	g_stopping = 0;
	g_nstarted = 0;
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

int
xtc_blocking_pool_size(int nthreads)
{
	int rc = XTC_OK;
	(void)pthread_mutex_lock(&g_lock);
	if (g_started)
		rc = XTC_E_INVAL;          /* too late */
	else if (nthreads >= 1 && nthreads <= BLK_MAX_THREADS)
		g_nthreads = nthreads;
	else
		rc = XTC_E_INVAL;
	(void)pthread_mutex_unlock(&g_lock);
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

	/* Synchronous fallback: not on a loop process (cannot park), or
	 * the wakeup pipe / pool could not be set up. */
	if (xtc_pid_is_none(xtc_self()))
		goto run_sync;
	if (pipe(pfd) != 0)
		goto run_sync;

	(void)pthread_mutex_lock(&g_lock);
	if (blk_start_locked() != 0) {
		(void)pthread_mutex_unlock(&g_lock);
		(void)close(pfd[0]);
		(void)close(pfd[1]);
		goto run_sync;
	}
	w.fn = fn;
	w.arg = arg;
	atomic_store_explicit(&w.result, 0, memory_order_relaxed);
	w.wr_fd = pfd[1];
	w.next = NULL;
	if (g_tail != NULL)
		g_tail->next = &w;
	else
		g_head = &w;
	g_tail = &w;
	(void)pthread_cond_signal(&g_cv);
	(void)pthread_mutex_unlock(&g_lock);

	/* Park until the worker signals completion. */
	(void)xtc_proc_wait_fd(pfd[0], XTC_IO_READABLE, -1, &revents);
	do {
		/* Readable per wait_fd above, so this does not block. */
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

void
xtc_blocking_shutdown(void)
{
	int i, n;
	__os_thread_t threads[BLK_MAX_THREADS];

	(void)pthread_mutex_lock(&g_lock);
	if (!g_started) {
		(void)pthread_mutex_unlock(&g_lock);
		return;
	}
	g_stopping = 1;
	n = g_nstarted;
	for (i = 0; i < n; i++)
		threads[i] = g_threads[i];
	(void)pthread_cond_broadcast(&g_cv);
	(void)pthread_mutex_unlock(&g_lock);

	for (i = 0; i < n; i++)
		(void)__os_thread_join(&threads[i], NULL);

	(void)pthread_mutex_lock(&g_lock);
	g_started = 0;
	g_nstarted = 0;
	g_stopping = 0;
	(void)pthread_mutex_unlock(&g_lock);
}
