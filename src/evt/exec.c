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
#include "xtc_preempt.h"
#include "xtc_sim.h"

#include <stdatomic.h>

extern int __xtc_loop_step_once(xtc_loop_t *loop);

/* Earliest pending sim-I/O event time for a loop's backend.  Provided by
 * io_sim.c only in a sim build; a non-sim build has no deferred sim I/O,
 * so the sim scheduler (compiled but never run off a sim build) gets a
 * stub that reports "none".  This keeps exec.c linkable in every build
 * while letting the sim scheduler account for in-flight deferred AIO. */
#if defined(XTC_IO_BACKEND_SIM)
extern int64_t __xtc_io_sim_next_due(xtc_io_t *io);
#else
static inline int64_t __xtc_io_sim_next_due(xtc_io_t *io)
{ (void)io; return -1; }
#endif

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
	int64_t       preempt_interval_ns; /* 0 = off (default); >0 arms a
	                                    * per-worker preemption timer at
	                                    * this CPU-time interval (Phase 1
	                                    * cooperative-assisted preemption:
	                                    * a tick makes xtc_yield_if_due
	                                    * callers yield). */
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
		/* Buggify: under DST, occasionally skip a NUMA-near victim that
		 * has stealable work.  A legal pessimal choice -- the work stays
		 * on the victim and this thief falls through to pass 2 or retries
		 * next scheduler turn -- that exercises the less-ideal steal
		 * placement deterministically. */
		if (XTC_SIM_BUGGIFY("sched.steal.skip_near") && xtc_sim_fault(200))
			continue;
		/* Critical section: the steal CAS races the victim's owner
		 * popping its own deque bottom.  A DST fault point here lets a
		 * seeded run perturb/observe the steal-vs-pop interleaving. */
		XTC_SIM_FAULT_POINT("sched.steal.pre_cas");
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

	/* Preemption Phase 1: if enabled, arm a per-worker CPU-time timer.
	 * A tick makes xtc_yield_if_due callers on this worker yield (see
	 * xtc_yield_check).  Off by default (preempt_interval_ns == 0). */
	if (exec->preempt_interval_ns > 0)
		(void)xtc_preempt_arm(exec->preempt_interval_ns);

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

	if (exec->preempt_interval_ns > 0)
		(void)xtc_preempt_disarm();
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

/* PUBLIC: int xtc_exec_set_preempt __P((xtc_exec_t *, int64_t)); */
/*
 * Enable cooperative-assisted preemption (M_PREEMPTION Phase 1): each
 * worker arms a per-worker CPU-time timer at `interval_ns`; a tick makes
 * this worker's xtc_yield_if_due() callers yield, so a long compute
 * fiber that touches a yield-check periodically is time-sliced without
 * a manual budget.  interval_ns == 0 disables (the default).  Must be
 * set BEFORE xtc_exec_run (workers read it at start).  Returns
 * XTC_E_NOSYS if the platform lacks per-thread CPU-time timers (the
 * setting is stored but arming will no-op), XTC_E_INVAL on a NULL exec.
 * NOTE: this does NOT preempt a fiber that never reaches a yield-check;
 * that is Phase 2 (signal-context involuntary yield).
 */
int
xtc_exec_set_preempt(xtc_exec_t *e, int64_t interval_ns)
{
	if (e == NULL)
		return XTC_E_INVAL;
	e->preempt_interval_ns = interval_ns < 0 ? 0 : interval_ns;
	if (interval_ns > 0 && !xtc_preempt_supported())
		return XTC_E_NOSYS;
	return XTC_OK;
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

/*
 * A loop is RUNNABLE NOW if it has a task it can actually advance: a
 * ready task on the owner FIFO (q_head) or the stealable deque, a timer
 * already due at the current virtual time, or a pending cross-loop
 * inbox message.  Note: n_alive > 0 does NOT imply runnable -- an alive
 * proc may be PARKED (awaiting a timer, fd, or cross-loop waker), and
 * treating a parked-only loop as runnable would spin the scheduler
 * forever (it would keep being picked, make no progress, and the
 * virtual clock would never advance to fire its timer).  When no loop
 * is runnable the scheduler advances the clock to the earliest pending
 * deadline, which makes that timer due and the owning loop runnable on
 * the next iteration.
 *
 * `peer_stealable` is the total length of OTHER loops' stealable deques
 * (computed once per scheduler iteration): an otherwise-idle loop is
 * also runnable when a peer has stealable work, so the deterministic
 * scheduler can pick it and exercise the work-stealing path (its
 * step_once then steals).  This is bounded -- a step that steals shrinks
 * the global stealable count, and a step that fails to steal (a peer
 * raced it away) finds peer_stealable == 0 next iteration -- so it
 * cannot spin.
 */
static int
__sim_loop_runnable(xtc_loop_t *l, int64_t now_ns, int64_t peer_stealable)
{
	int64_t dl, iod;
	if (l->q_head != NULL)
		return 1;
	if (xtc_deque_len(&l->deque) > 0)
		return 1;
	if (l->inbox.head != NULL)
		return 1;
	dl = __xtc_timer_heap_next_deadline(l);
	if (dl >= 0 && dl <= now_ns)
		return 1;
	/* A sim-I/O event (readiness or a deferred AIO completion) due at
	 * the current virtual time makes the loop runnable -- step_once
	 * polls the sim backend and dispatches it, waking the parked op. */
	iod = (l->io != NULL) ? __xtc_io_sim_next_due(l->io) : -1;
	if (iod >= 0 && iod <= now_ns)
		return 1;
	/* Idle locally with NO pending timer or I/O, but a peer has
	 * stealable work and this loop is part of an executor -> runnable
	 * (its step_once will reach the steal branch and take the work).
	 * The "no pending timer/io" guard ensures the step actually reaches
	 * steal (a loop with a future wakeup takes the clock-wait branch
	 * instead, so marking it runnable-to-steal would spin). */
	if (peer_stealable > 0 && l->exec != NULL && dl < 0 && iod < 0)
		return 1;
	return 0;
}

/* Total stealable (deque) work across all loops EXCEPT `except_id`. */
static int64_t
__sim_peer_stealable(xtc_exec_t *e, int except_id)
{
	int64_t total = 0;
	int i;
	for (i = 0; i < e->n_loops; i++) {
		if (i == except_id)
			continue;
		total += (int64_t)xtc_deque_len(&e->loops[i]->deque);
	}
	return total;
}

/* The earliest pending event (timer OR deferred sim-I/O) across all
 * loops, or -1 if none.  Used to advance the virtual clock when no loop
 * is runnable now -- so an in-flight deferred AIO completion is not
 * mistaken for a deadlock. */
static int64_t
__sim_earliest_deadline(xtc_exec_t *e)
{
	int64_t best = -1;
	int i;
	for (i = 0; i < e->n_loops; i++) {
		xtc_loop_t *l = e->loops[i];
		int64_t dl = __xtc_timer_heap_next_deadline(l);
		int64_t iod = (l->io != NULL) ? __xtc_io_sim_next_due(l->io) : -1;
		if (dl >= 0 && (best < 0 || dl < best))
			best = dl;
		if (iod >= 0 && (best < 0 || iod < best))
			best = iod;
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
		/* Timer count is never negative, and never exceeds capacity
		 * (an oversized n_timers means a heap push overran its
		 * backing array). */
		if (l->n_timers < 0 || l->n_timers > l->cap_timers)
			return XTC_E_INTERNAL;
		/* The min-heap invariant at the root: the top timer's deadline
		 * is <= its children's, so the earliest deadline sits at index
		 * 0 (what the scheduler reads to advance the virtual clock).  A
		 * violated root means a broken sift, which would make the
		 * scheduler advance the clock to the wrong time. */
		if (l->n_timers >= 2 && l->timers != NULL) {
			int64_t top = l->timers[0]->deadline_ns;
			if (l->timers[1]->deadline_ns < top)
				return XTC_E_INTERNAL;
			if (l->n_timers >= 3 &&
			    l->timers[2]->deadline_ns < top)
				return XTC_E_INTERNAL;
		}
		/* Slow-path FIFO run-queue coherence: head NULL iff tail NULL
		 * (a half-cleared queue drops or duplicates a ready task). */
		if ((l->q_head == NULL) != (l->q_tail == NULL))
			return XTC_E_INTERNAL;
		/* Recycled-task free-list count is never negative. */
		if (l->task_free_n < 0)
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
 * <= 0 means unbounded.  Returns XTC_OK on clean quiescence, XTC_E_AGAIN
 * if the step budget was exhausted with work remaining (a possible
 * livelock -- inspect), XTC_E_DEADLK if the system stopped with procs
 * still alive but all parked (a deadlock: no waker can arrive), or a
 * negative code on a loop-step error / invariant violation.
 */
int
xtc_sim_exec_run(xtc_exec_t *e, uint64_t seed, long max_steps)
{
	long steps = 0;
	xtc_loop_t *saved;
	/* Per-run last-run-step per loop, for the pessimal (starve) pick.
	 * A fixed cap keeps it a stack local (no alloc); loops beyond the
	 * cap simply are not starve-tracked and fall back to uniform. */
#define XTC_SIM_MAX_TRACKED_LOOPS 256
	long last_run[XTC_SIM_MAX_TRACKED_LOOPS];
	int pinned = -1;   /* pessimal: the loop currently monopolizing */
	{
		int li;
		for (li = 0; li < XTC_SIM_MAX_TRACKED_LOOPS; li++)
			last_run[li] = 0;
	}
	if (e == NULL) return XTC_E_INVAL;

	/* Save the caller's current-loop binding.  __xtc_loop_step (called
	 * below via __xtc_loop_step_once) binds __xtc_current_loop to the
	 * loop it steps and does NOT restore it, so on return this thread's
	 * binding would dangle at the last-stepped loop.  Left dangling, a
	 * SECOND sim run in the same process would see a non-NULL binding on
	 * entry: if malloc happened to reuse a freed loop's address for the
	 * new run's loop, a spawn-from-caller (__xtc_current_loop == loop)
	 * would take the direct-enqueue path instead of the cross-loop inbox
	 * publish path the first run took -- a different initial deque
	 * distribution and thus a different (but still valid) steal schedule.
	 * That is the work-stealing completion-ORDER replay gap: not the
	 * steal path itself but leaked per-thread state across runs.  Restore
	 * on every exit so each run starts from the identical binding, like
	 * xtc_loop_run already does.  Sim-only: this function only runs under
	 * sim, so production current-loop handling is unchanged. */
	saved = __xtc_current_loop;

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
			__xtc_current_loop = saved;
			return XTC_E_AGAIN;   /* budget exhausted; work may remain */
		}

		(void)__xtc_sim_vclock(&now);

		/* Count loops runnable at the current virtual time.  A loop is
		 * also runnable-to-steal when a peer has stealable deque work. */
		for (i = 0; i < e->n_loops; i++)
			if (__sim_loop_runnable(e->loops[i], now,
			    __sim_peer_stealable(e, i)))
				n_runnable++;

		if (n_runnable == 0) {
			/* Nobody can progress now.  Advance the virtual clock
			 * to the earliest pending timer; if there is none, the
			 * system has stopped.  Distinguish clean quiescence
			 * (no proc alive anywhere) from a DEADLOCK (procs still
			 * alive but all parked with no timer and no inbox -- no
			 * waker can ever arrive on the single sim thread). */
			dl = __sim_earliest_deadline(e);
			if (dl < 0) {
				int alive = 0;
				for (i = 0; i < e->n_loops; i++)
					alive += atomic_load_explicit(
					    &e->loops[i]->n_alive,
					    memory_order_relaxed);
					e->started = 0;
				xtc_sim_clock_disable();
				__xtc_current_loop = saved;
				if (alive > 0) {
					xtc_sim_deactivate();
					return XTC_E_DEADLK;
				}
				/* Clean quiescence: run the optional semantic
				 * consistency check while the sim is still active
				 * (so it may inspect sim-visible state), THEN
				 * deactivate. */
				{
					int crc = __xtc_sim_run_consistency_check();
					/* Determinism proof: if any nondeterministic
					 * primitive was hit on the executed path,
					 * this run cannot be trusted to replay --
					 * fail it.  (In strict mode the guard has
					 * already aborted; this covers count-only
					 * mode and makes the guarantee explicit.) */
					if (crc == XTC_OK &&
					    xtc_sim_nondeterminism_count() > 0)
						crc = XTC_E_INTERNAL;
					xtc_sim_deactivate();
					return crc;
				}
			}
			if (dl > now)
				xtc_sim_clock_set(dl);
			continue;
		}

		/* Seeded pick among the runnable loops.  Default: uniform over
		 * the SCHED stream.  Adversarial (xtc_sim_sched_pessimal): on a
		 * seeded coin take the PESSIMAL pick -- keep running the pinned
		 * loop as long as it stays runnable, monopolizing the executor
		 * and STARVING every peer (the classic worst interleaving: a
		 * fiber that holds a resource a peer is blocked on never yields
		 * the scheduler).  Only when the pin cannot run do we pin a new
		 * runnable loop -- the least-recently-run one, so over a long run
		 * every loop eventually gets starved in turn rather than one
		 * fixed loop always winning.  last_run[]/pinned are per-run
		 * locals keyed by loop index (sim-only; no struct/ABI change). */
		pick = (int)__xtc_sim_rng_range(XTC_SIM_RNG_SCHED,
		    (uint64_t)n_runnable);
		{
			int pess = __xtc_sim_sched_pessimal_pct();
			if (pess > 0 &&
			    (int)__xtc_sim_rng_range(XTC_SIM_RNG_SCHED, 1000) <
			        pess) {
				/* Prefer the pinned loop if it is still runnable
				 * (monopolize/starve). */
				if (pinned >= 0 && pinned < e->n_loops &&
				    __sim_loop_runnable(e->loops[pinned], now,
				    __sim_peer_stealable(e, pinned))) {
					chosen = pinned;
				} else {
					/* Pin a new victim: least-recently-run
					 * runnable loop, so starvation rotates. */
					long oldest = -1;
					chosen = -1;
					for (i = 0; i < e->n_loops; i++) {
						if (!__sim_loop_runnable(
						    e->loops[i], now,
						    __sim_peer_stealable(e, i)))
							continue;
						if (oldest < 0 ||
						    last_run[i] < oldest) {
							oldest = last_run[i];
							chosen = i;
						}
					}
					pinned = chosen;
				}
			}
		}
		if (chosen < 0) {
			for (i = 0; i < e->n_loops; i++) {
				if (__sim_loop_runnable(e->loops[i], now,
				    __sim_peer_stealable(e, i))) {
					if (pick == 0) { chosen = i; break; }
					pick--;
				}
			}
		}
		if (chosen < 0)
			continue;   /* raced to empty (shouldn't, single thread) */
		if (chosen < XTC_SIM_MAX_TRACKED_LOOPS)
			last_run[chosen] = steps;

		rc = __xtc_loop_step_once(e->loops[chosen]);
		steps++;
		if (rc < 0) {
			e->started = 0;
			xtc_sim_clock_disable();
			xtc_sim_deactivate();
			__xtc_current_loop = saved;
			return rc;
		}
		/* Structural invariants must hold after every step; a
		 * violation pinpoints the exact seeded interleaving that
		 * produced the corruption. */
		if (xtc_sim_check(e) != XTC_OK) {
			e->started = 0;
			xtc_sim_clock_disable();
			xtc_sim_deactivate();
			__xtc_current_loop = saved;
			return XTC_E_INTERNAL;
		}
	}

	e->started = 0;
	xtc_sim_clock_disable();
	xtc_sim_deactivate();
	__xtc_current_loop = saved;
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
