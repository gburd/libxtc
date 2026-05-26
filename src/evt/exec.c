/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the PostgreSQL License.
 *
 * src/evt/exec.c
 *	The L2 multi-loop executor.  Owns N xtc_loop instances and N
 *	worker threads (one per loop).  See M5_CLAIMS.md.
 */

#include "xtc_int.h"
#include "loop_int.h"
#include "xtc_exec.h"
#include "xtc_async.h"

#include <stdatomic.h>

extern int __xtc_loop_step_once(xtc_loop_t *loop);

struct xtc_exec {
	int           n_loops;
	xtc_loop_t  **loops;
	int          *loop_node;       /* NUMA node per loop (M5.5) */
	__os_thread_t *workers;
	_Atomic int   stop_flag;
	int           started;
};

/*
 * Per-loop steal helper.  Walk the steal-order list which prefers
 * NUMA-near victims first (same node as the calling loop), then
 * cross-node loops.  Within each tier the start point is randomized
 * so we don't always pound on the same victim.
 */
void *
__xtc_exec_try_steal(xtc_loop_t *me)
{
	xtc_exec_t *exec = me->exec;
	int i, n, start;
	int my_node;
	if (exec == NULL) return NULL;
	n = exec->n_loops;
	if (n <= 1) return NULL;
	my_node = exec->loop_node ? exec->loop_node[me->exec_id] : 0;

	{
		static _Thread_local unsigned int rot = 0x9E3779B9u;
		rot += 0x9E3779B9u;
		start = (int)((rot >> 8) % (unsigned)n);
	}

	/* Pass 1: same NUMA node. */
	for (i = 0; i < n; i++) {
		int idx = (start + i) % n;
		xtc_loop_t *victim;
		void *t;
		if (idx == me->exec_id) continue;
		if (exec->loop_node && exec->loop_node[idx] != my_node) continue;
		victim = exec->loops[idx];
		if (xtc_deque_len(&victim->deque) == 0) continue;
		t = xtc_deque_steal(&victim->deque);
		if (t != NULL) return t;
	}
	/* Pass 2: any node. */
	for (i = 0; i < n; i++) {
		int idx = (start + i) % n;
		xtc_loop_t *victim;
		void *t;
		if (idx == me->exec_id) continue;
		victim = exec->loops[idx];
		if (xtc_deque_len(&victim->deque) == 0) continue;
		t = xtc_deque_steal(&victim->deque);
		if (t != NULL) return t;
	}
	return NULL;
}

/* Worker entry function.  arg is the xtc_loop the worker owns. */
static void *
__xtc_exec_worker(void *arg)
{
	xtc_loop_t *loop = arg;
	xtc_exec_t *exec = loop->exec;

	__xtc_current_loop = loop;
	/* Record NUMA placement so the steal pass-1 can prefer same-node. */
	if (exec->loop_node != NULL)
		exec->loop_node[loop->exec_id] = __os_numa_current_node();

	for (;;) {
		int rc;
		if (atomic_load_explicit(&exec->stop_flag,
		    memory_order_relaxed))
			break;
		rc = __xtc_loop_step_once(loop);
		if (rc < 0) break;
		if (rc == 0) {
			/*
			 * No local work.  Step blocks on io_poll inside
			 * step_once when there are timers/fds; otherwise
			 * we have nothing to do — the loop is done.
			 *
			 * But other loops might still be producing wakers
			 * for our parked tasks (cross-thread sends).  We
			 * sit on io_poll with a small timeout to remain
			 * responsive to inbox arrivals.
			 */
			xtc_io_event_t evs[8];
			int n_out;
			(void)xtc_io_poll(loop->io, evs, 8,
			    1 * 1000 * 1000LL /* 1ms */, &n_out);
			(void)__xtc_inbox_drain(loop);
			/* Loop again; if exec stopped or no real work
			 * appeared, we'll exit on the next iteration. */
			if (atomic_load_explicit(&exec->stop_flag,
			    memory_order_relaxed))
				break;
		}
	}

	__xtc_current_loop = NULL;
	return NULL;
}

/* PUBLIC: int xtc_exec_init __P((xtc_exec_t **, int)); */
int
xtc_exec_init(xtc_exec_t **out, int n_loops)
{
	xtc_exec_t *e;
	int rc, i;

	if (out == NULL) return XTC_E_INVAL;

	if (n_loops <= 0) {
		n_loops = __os_ncpus();
		if (n_loops <= 0) n_loops = 4;
	}

	if ((rc = __os_calloc(1, sizeof *e, (void **)&e)) != XTC_OK)
		return rc;
	if ((rc = __os_calloc((size_t)n_loops, sizeof *e->loops,
	    (void **)&e->loops)) != XTC_OK) {
		__os_free(e); return rc;
	}
	if ((rc = __os_calloc((size_t)n_loops, sizeof *e->workers,
	    (void **)&e->workers)) != XTC_OK) {
		__os_free(e->loops); __os_free(e); return rc;
	}
	if ((rc = __os_calloc((size_t)n_loops, sizeof *e->loop_node,
	    (void **)&e->loop_node)) != XTC_OK) {
		__os_free(e->workers); __os_free(e->loops); __os_free(e); return rc;
	}
	atomic_store_explicit(&e->stop_flag, 0, memory_order_relaxed);
	e->n_loops = n_loops;
	e->started = 0;

	for (i = 0; i < n_loops; i++) {
		if ((rc = xtc_loop_init(&e->loops[i])) != XTC_OK) {
			while (--i >= 0) (void)xtc_loop_fini(e->loops[i]);
			__os_free(e->workers);
			__os_free(e->loops);
			__os_free(e);
			return rc;
		}
		e->loops[i]->exec_id = i;
		e->loops[i]->exec = e;
	}

	*out = e;
	return XTC_OK;
}

/* PUBLIC: int xtc_exec_fini __P((xtc_exec_t *)); */
int
xtc_exec_fini(xtc_exec_t *e)
{
	int i;
	if (e == NULL) return XTC_E_INVAL;
	for (i = 0; i < e->n_loops; i++)
		if (e->loops[i] != NULL)
			(void)xtc_loop_fini(e->loops[i]);
	__os_free(e->loops);
	__os_free(e->workers);
	__os_free(e->loop_node);
	__os_free(e);
	return XTC_OK;
}

/*
 * Aggregate "is the executor still doing useful work?"  Sums:
 *   - n_alive across all loops (tasks not yet DONE)
 *   - deque lengths (queued runnable tasks)
 *   - inbox head pointers (pending cross-thread messages)
 *   - per-loop pending timers (raw count; cancelled entries count)
 * Returns 1 if any of these is non-zero; 0 if globally idle.
 *
 * IMPORTANT: this runs on the supervisor thread (main), concurrently
 * with workers that own each loop.  Every read here must be
 * side-effect-free.  We deliberately do NOT call
 * __xtc_timer_heap_next_deadline() because that function mutates the
 * heap by popping cancelled entries, racing with the owning worker.
 * A bare read of `n_timers` is racy on int but harmless for the
 * "is non-zero?" question; the supervisor double-checks after a
 * short sleep before stopping, which absorbs any stale reads.
 */
static int
__exec_has_work(xtc_exec_t *e)
{
	int i;
	for (i = 0; i < e->n_loops; i++) {
		xtc_loop_t *l = e->loops[i];
		if (atomic_load_explicit(&l->n_alive,
		    memory_order_relaxed) > 0) return 1;
		if (xtc_deque_len(&l->deque) > 0) return 1;
		if (l->n_timers > 0) return 1;
		if (l->inbox.head != NULL) return 1;
	}
	return 0;
}

/* PUBLIC: int xtc_exec_run __P((xtc_exec_t *)); */
int
xtc_exec_run(xtc_exec_t *e)
{
	int i, rc;
	if (e == NULL) return XTC_E_INVAL;
	atomic_store_explicit(&e->stop_flag, 0, memory_order_relaxed);

	/* Spawn worker threads. */
	for (i = 0; i < e->n_loops; i++) {
		if ((rc = __os_thread_create(&e->workers[i],
		    __xtc_exec_worker, e->loops[i])) != XTC_OK) {
			atomic_store_explicit(&e->stop_flag, 1,
			    memory_order_relaxed);
			while (--i >= 0) {
				(void)xtc_io_wakeup(e->loops[i]->io);
				(void)__os_thread_join(&e->workers[i], NULL);
			}
			return rc;
		}
	}
	e->started = 1;

	/*
	 * Supervise from the calling thread.  Periodically check whether
	 * every loop is idle; if so, signal stop and join.
	 * Caller may also call xtc_exec_stop from any thread.
	 */
	for (;;) {
		if (atomic_load_explicit(&e->stop_flag,
		    memory_order_relaxed))
			break;
		if (!__exec_has_work(e)) {
			/*
			 * Confirm with a small re-check window: a worker
			 * might be mid-step.  Sleep ~1 ms, then re-check.
			 * Two consecutive idle observations -> stop.
			 */
			(void)__os_sleep_ns(1 * 1000 * 1000LL);
			if (!__exec_has_work(e)) {
				atomic_store_explicit(&e->stop_flag, 1,
				    memory_order_relaxed);
				for (i = 0; i < e->n_loops; i++)
					(void)xtc_io_wakeup(e->loops[i]->io);
				break;
			}
		}
		(void)__os_sleep_ns(2 * 1000 * 1000LL);    /* 2 ms */
	}

	for (i = 0; i < e->n_loops; i++)
		(void)__os_thread_join(&e->workers[i], NULL);
	e->started = 0;
	return XTC_OK;
}

/* PUBLIC: int xtc_exec_stop __P((xtc_exec_t *)); */
int
xtc_exec_stop(xtc_exec_t *e)
{
	int i;
	if (e == NULL) return XTC_E_INVAL;
	atomic_store_explicit(&e->stop_flag, 1, memory_order_relaxed);
	/* Wake every loop so they observe the flag. */
	for (i = 0; i < e->n_loops; i++)
		(void)xtc_io_wakeup(e->loops[i]->io);
	return XTC_OK;
}

/* PUBLIC: int xtc_exec_n_loops __P((xtc_exec_t *)); */
int
xtc_exec_n_loops(xtc_exec_t *e)
{
	return e == NULL ? 0 : e->n_loops;
}

/* PUBLIC: int xtc_exec_loop_id __P((void)); */
int
xtc_exec_loop_id(void)
{
	xtc_loop_t *l = __xtc_current_loop;
	return l == NULL ? -1 : l->exec_id;
}

/* PUBLIC: xtc_loop_t *xtc_exec_loop __P((xtc_exec_t *, int)); */
xtc_loop_t *
xtc_exec_loop(xtc_exec_t *e, int idx)
{
	if (e == NULL || idx < 0 || idx >= e->n_loops) return NULL;
	return e->loops[idx];
}

/* --- spawn/async helpers ------------------------------------------- */

static int
__pick_loop(xtc_exec_t *e)
{
	/* Round-robin counter for default placement.  */
	static _Atomic int rr;
	int n = atomic_fetch_add_explicit(&rr, 1, memory_order_relaxed);
	return n % e->n_loops;
}

/* PUBLIC: int xtc_exec_spawn __P((xtc_exec_t *, xtc_task_fn, void *, xtc_task_t **)); */
int
xtc_exec_spawn(xtc_exec_t *e, xtc_task_fn fn, void *user, xtc_task_t **out)
{
	if (e == NULL) return XTC_E_INVAL;
	return xtc_task_spawn(e->loops[__pick_loop(e)], fn, user, out);
}

/* PUBLIC: int xtc_exec_spawn_on __P((xtc_exec_t *, int, xtc_task_fn, void *, xtc_task_t **)); */
int
xtc_exec_spawn_on(xtc_exec_t *e, int idx, xtc_task_fn fn, void *user,
                  xtc_task_t **out)
{
	if (e == NULL) return XTC_E_INVAL;
	if (idx < 0 || idx >= e->n_loops) return XTC_E_INVAL;
	return xtc_task_spawn(e->loops[idx], fn, user, out);
}

/* PUBLIC: int xtc_exec_async __P((xtc_exec_t *, xtc_coro_fn, void *, xtc_task_t **)); */
int
xtc_exec_async(xtc_exec_t *e, xtc_coro_fn fn, void *arg, xtc_task_t **out)
{
	if (e == NULL) return XTC_E_INVAL;
	return xtc_async(e->loops[__pick_loop(e)], fn, arg, out);
}

/* PUBLIC: int xtc_exec_async_on __P((xtc_exec_t *, int, xtc_coro_fn, void *, xtc_task_t **)); */
int
xtc_exec_async_on(xtc_exec_t *e, int idx, xtc_coro_fn fn, void *arg,
                  xtc_task_t **out)
{
	if (e == NULL) return XTC_E_INVAL;
	if (idx < 0 || idx >= e->n_loops) return XTC_E_INVAL;
	return xtc_async(e->loops[idx], fn, arg, out);
}
