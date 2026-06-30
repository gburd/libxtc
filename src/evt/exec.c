/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/evt/exec.c
 *	The L2 multi-loop executor.  Owns N xtc_loop instances and N
 *	worker threads (one per loop).  See M5_CLAIMS.md.
 */

#include "xtc_int.h"
#include "loop_int.h"
#include "xtc_exec.h"
#include "xtc_async.h"
#include "xtc_sim.h"

#include <stdatomic.h>

extern int __xtc_loop_step_once(xtc_loop_t *loop);

struct xtc_exec {
	int           n_loops;
	xtc_loop_t  **loops;
	int          *loop_node;       /* NUMA node per loop (M5.5) */
	__os_thread_t *workers;
	_Atomic int   stop_flag;
	int           started;
	int           service_mode;    /* 1: run until xtc_exec_stop, never
	                                * idle-auto-stop (a supervised app is
	                                * a long-running service, not a finite
	                                * work pool that drains and exits). */
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
		/* Steal-victim start point.  Under a deterministic sim the
		 * choice is drawn from the seeded STEAL stream so the steal
		 * pattern replays; in production it is the cheap rotating
		 * thread-local counter. */
		if (__xtc_sim_active()) {
			start = (int)__xtc_sim_rng_range(XTC_SIM_RNG_STEAL,
			    (uint64_t)n);
		} else {
			static _Thread_local unsigned int rot = 0x9E3779B9u;
			rot += 0x9E3779B9u;
			start = (int)((rot >> 8) % (unsigned)n);
		}
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
	/* Bias this reactor thread onto the performance (P) cores on
	 * asymmetric hardware (Apple Silicon); no-op elsewhere. */
	__os_thread_apply_default_qos();
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
			 * we have nothing to do -- the loop is done.
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

/* PUBLIC: void xtc_exec_set_service_mode __P((xtc_exec_t *, int)); */
void
xtc_exec_set_service_mode(xtc_exec_t *e, int on)
{
	if (e != NULL)
		e->service_mode = on ? 1 : 0;
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
	 *
	 * In service mode (a supervised xtc_app) the idle check is skipped
	 * entirely: the application is a long-running service that runs
	 * until xtc_exec_stop is called explicitly (by xtc_app_stop or the
	 * supervisor on a restart-intensity giveup).  Without this a
	 * transient all-idle window during startup -- after the first
	 * children exit but before the rest have been scheduled across
	 * loops -- would stop the whole application prematurely.
	 */
	for (;;) {
		if (atomic_load_explicit(&e->stop_flag,
		    memory_order_relaxed))
			break;
		if (!e->service_mode && !__exec_has_work(e)) {
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

/* ---- deterministic simulation scheduler (DST, phase 3) ---------- *
 *
 * Runs the executor's N loops as N cooperatively-scheduled entities on
 * the CALLING thread (no worker threads), under a seed-determined
 * interleaving, against the virtual clock.  The identical loop /
 * work-stealing / cross-loop / shared-latch CODE runs (via
 * __xtc_loop_step_once); only WHICH runnable loop advances next, and
 * when virtual time advances, are decided by the seed -- so the whole
 * run replays byte-for-byte from (seed, config).  See docs/M_DST.md.
 *
 * Requires a sim build (XTC_IO_BACKEND_SIM): the sim I/O backend's
 * xtc_io_poll never blocks, so __xtc_loop_step_once always returns
 * promptly and this scheduler -- not a kernel poller -- owns blocking
 * and advancing the clock.  The parking primitives (amutex/arwlock/
 * recv) already yield the fiber to the per-thread coro scheduler, so a
 * parked task simply is not re-run until its waker fires on a later
 * step of its loop.
 */

/* A loop is runnable now if it has a live task, queued/stealable work,
 * a timer due at the current virtual time, or a pending cross-loop
 * inbox message. */
static int
__sim_loop_runnable(xtc_loop_t *l, int64_t now_ns)
{
	int64_t dl;
	if (atomic_load_explicit(&l->n_alive, memory_order_relaxed) > 0)
		return 1;
	if (xtc_deque_len(&l->deque) > 0)
		return 1;
	if (l->inbox.head != NULL)
		return 1;
	dl = __xtc_timer_heap_next_deadline(l);
	if (dl >= 0 && dl <= now_ns)
		return 1;
	return 0;
}

/* The earliest timer deadline across all loops, or -1 if none pending.
 * Used to advance the virtual clock when no loop is runnable now. */
static int64_t
__sim_earliest_deadline(xtc_exec_t *e)
{
	int64_t best = -1;
	int i;
	for (i = 0; i < e->n_loops; i++) {
		int64_t dl = __xtc_timer_heap_next_deadline(e->loops[i]);
		if (dl >= 0 && (best < 0 || dl < best))
			best = dl;
	}
	return best;
}

/* ---- DST invariant checker + state hash (phase 4) ---- */

/*
 * PUBLIC: int xtc_sim_check __P((xtc_exec_t *));
 *
 * Cheap structural invariants over every loop, meant to run after each
 * sim step (the scheduler calls it when checking is enabled).  Returns
 * XTC_OK if all hold, XTC_E_INTERNAL on the first violation.  These
 * catch corruption the moment a step produces it, at the exact seeded
 * interleaving that triggered it.
 */
int
xtc_sim_check(xtc_exec_t *e)
{
	int i;
	if (e == NULL) return XTC_E_INVAL;
	for (i = 0; i < e->n_loops; i++) {
		xtc_loop_t *l = e->loops[i];
		int64_t len = (int64_t)xtc_deque_len(&l->deque);
		int nalive = atomic_load_explicit(&l->n_alive,
		    memory_order_relaxed);
		/* Deque length within [0, CAP]: a negative or oversized span
		 * means top/bottom corruption or a torn steal. */
		if (len < 0 || len > XTC_DEQUE_CAP)
			return XTC_E_INTERNAL;
		/* Alive count is never negative (underflow on a double-reap). */
		if (nalive < 0)
			return XTC_E_INTERNAL;
		/* Timer count is never negative. */
		if (l->n_timers < 0)
			return XTC_E_INTERNAL;
	}
	return XTC_OK;
}

/*
 * PUBLIC: uint64_t xtc_sim_state_hash __P((xtc_exec_t *));
 *
 * A 64-bit digest of the observable per-loop state (tasks run, steals,
 * alive, timer count) folded in loop order.  Two runs with the same
 * (seed, config) must produce the same hash -- a stronger replay
 * assertion than a single application counter.  Folds only LOGICAL
 * state (counters), never pointers or timing.
 */
uint64_t
xtc_sim_state_hash(xtc_exec_t *e)
{
	uint64_t h = 0xCBF29CE484222325ull;   /* FNV-1a basis */
	int i;
	if (e == NULL) return 0;
	for (i = 0; i < e->n_loops; i++) {
		xtc_loop_t *l = e->loops[i];
		uint64_t v[4];
		int k;
		v[0] = atomic_load_explicit(&l->n_tasks_run, memory_order_relaxed);
		v[1] = atomic_load_explicit(&l->n_steals, memory_order_relaxed);
		v[2] = (uint64_t)(unsigned)atomic_load_explicit(&l->n_alive,
		    memory_order_relaxed);
		v[3] = (uint64_t)(unsigned)l->n_timers;
		for (k = 0; k < 4; k++) {
			h ^= v[k];
			h *= 0x100000001B3ull;       /* FNV-1a prime */
		}
	}
	return h;
}

/*
 * PUBLIC: int xtc_sim_exec_run __P((xtc_exec_t *, uint64_t, long));
 *
 * Activate sim (seed) + the virtual clock, then drive the loops
 * deterministically until quiescence (no runnable loop and no pending
 * timer), the step budget is hit, or xtc_exec_stop is called.  max_steps
 * <= 0 means unbounded.  Returns XTC_OK on quiescence, XTC_E_AGAIN if
 * the step budget was exhausted with work remaining (a possible hang /
 * livelock -- inspect), or a negative code on a loop-step error.
 */
int
xtc_sim_exec_run(xtc_exec_t *e, uint64_t seed, long max_steps)
{
	long steps = 0;
	if (e == NULL) return XTC_E_INVAL;

	xtc_sim_activate(seed);
	xtc_sim_clock_enable(0);
	atomic_store_explicit(&e->stop_flag, 0, memory_order_relaxed);
	e->started = 1;

	for (;;) {
		int64_t now, dl;
		int i, n_runnable = 0, pick, chosen = -1, rc;

		if (atomic_load_explicit(&e->stop_flag, memory_order_relaxed))
			break;
		if (max_steps > 0 && steps >= max_steps) {
			e->started = 0;
			xtc_sim_clock_disable();
			xtc_sim_deactivate();
			return XTC_E_AGAIN;   /* budget exhausted; work may remain */
		}

		(void)__xtc_sim_vclock(&now);

		/* Count loops runnable at the current virtual time. */
		for (i = 0; i < e->n_loops; i++)
			if (__sim_loop_runnable(e->loops[i], now))
				n_runnable++;

		if (n_runnable == 0) {
			/* Nobody can progress now.  Advance the virtual clock
			 * to the earliest pending timer; if there is none, the
			 * system is quiescent -- done. */
			dl = __sim_earliest_deadline(e);
			if (dl < 0)
				break;                 /* quiescent */
			if (dl > now)
				xtc_sim_clock_set(dl);
			continue;
		}

		/* Seeded pick among the runnable loops (the SCHED stream). */
		pick = (int)__xtc_sim_rng_range(XTC_SIM_RNG_SCHED,
		    (uint64_t)n_runnable);
		for (i = 0; i < e->n_loops; i++) {
			if (__sim_loop_runnable(e->loops[i], now)) {
				if (pick == 0) { chosen = i; break; }
				pick--;
			}
		}
		if (chosen < 0)
			continue;   /* raced to empty (shouldn't, single thread) */

		rc = __xtc_loop_step_once(e->loops[chosen]);
		steps++;
		if (rc < 0) {
			e->started = 0;
			xtc_sim_clock_disable();
			xtc_sim_deactivate();
			return rc;
		}
		/* Structural invariants must hold after every step; a
		 * violation pinpoints the exact seeded interleaving that
		 * produced the corruption. */
		if (xtc_sim_check(e) != XTC_OK) {
			e->started = 0;
			xtc_sim_clock_disable();
			xtc_sim_deactivate();
			return XTC_E_INTERNAL;
		}
	}

	e->started = 0;
	xtc_sim_clock_disable();
	xtc_sim_deactivate();
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

/* PUBLIC: int xtc_shard_id __P((void)); */
int
xtc_shard_id(void)
{
	/* Seastar-style this_shard_id(): the 0-based index of the loop
	 * the caller runs on, so shared-nothing consumers can index
	 * per-core state with no synchronization.  A standalone loop is
	 * shard 0 of 1; -1 only when not on any loop. */
	xtc_loop_t *l = __xtc_current_loop;
	if (l == NULL) return -1;
	return l->exec_id < 0 ? 0 : l->exec_id;
}

/* PUBLIC: int xtc_shard_count __P((void)); */
int
xtc_shard_count(void)
{
	xtc_loop_t *l = __xtc_current_loop;
	if (l == NULL) return 0;
	if (l->exec != NULL) return l->exec->n_loops;
	return 1;                        /* standalone loop == 1 shard */
}

/* PUBLIC: xtc_loop_t *xtc_exec_loop __P((xtc_exec_t *, int)); */
xtc_loop_t *
xtc_exec_loop(xtc_exec_t *e, int idx)
{
	if (e == NULL || idx < 0 || idx >= e->n_loops) return NULL;
	return e->loops[idx];
}

/* PUBLIC: int xtc_exec_loop_stats __P((xtc_exec_t *, int, xtc_loop_stats_t *)); */
int
xtc_exec_loop_stats(xtc_exec_t *e, int idx, xtc_loop_stats_t *out)
{
	xtc_loop_t *l;
	if (e == NULL || out == NULL || idx < 0 || idx >= e->n_loops)
		return XTC_E_INVAL;
	l = e->loops[idx];
	out->tasks_run = atomic_load_explicit(&l->n_tasks_run,
	    memory_order_relaxed);
	out->steals = atomic_load_explicit(&l->n_steals,
	    memory_order_relaxed);
	return XTC_OK;
}

/* --- spawn/async helpers ------------------------------------------- */

static int
__pick_loop(xtc_exec_t *e)
{
	/* Round-robin counter for default placement.  Under sim the
	 * placement is drawn from the seeded PLACE stream so spawn
	 * locations replay; otherwise the rr counter. */
	if (__xtc_sim_active())
		return (int)__xtc_sim_rng_range(XTC_SIM_RNG_PLACE,
		    (uint64_t)e->n_loops);
	{
		static _Atomic int rr;
		int n = atomic_fetch_add_explicit(&rr, 1, memory_order_relaxed);
		return n % e->n_loops;
	}
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
	return __xtc_task_spawn_ex(e->loops[idx], fn, user, 1, out);
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
