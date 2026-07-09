/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/evt/loop.c
 *	Event-loop lifecycle and the main run loop.
 *	M5: deque-based run queue with mutex slow path, MPSC inbox for
 *	    cross-thread wakers and spawns, atomic n_alive.
 */

#include "xtc_int.h"
#include "loop_int.h"
#include "xtc_slab.h"
#include "coro_int.h"
#include "xtc_async.h"
#include "xtc_preempt.h"
#include "os_time.h"
#include "xtc_sim.h"

#include <stdint.h>
#include <unistd.h>

/* Per-thread cursor -- see loop_int.h. */
XTC_THREAD_LOCAL xtc_loop_t *__xtc_current_loop = NULL;

/* Fiber-context preservation hooks; installed by the process layer
 * (proc.c) on first spawn.  NULL until then -- see loop_int.h. */
void *(*__xtc_fiber_ctx_save)(void) = NULL;
void  (*__xtc_fiber_ctx_restore)(void *) = NULL;
void  (*__xtc_fiber_kill_check)(void) = NULL;

/* --- inbox ---------------------------------------------------------- */

int
__xtc_inbox_init(struct xtc_inbox *ib)
{
	int rc;
	if ((rc = __os_mutex_init(&ib->lock)) != XTC_OK) return rc;
	ib->head = ib->tail = NULL;
	ib->inited = 1;
	return XTC_OK;
}

void
__xtc_inbox_fini(struct xtc_inbox *ib)
{
	struct xtc_inbox_msg *m, *n;
	if (!ib->inited) return;
	for (m = ib->head; m != NULL; m = n) {
		n = m->next;
		/* An undrained XTC_INB_PUBLISH carries a task that was spawned
		 * cross-thread but never ran, so it was never linked into the
		 * loop's all_tasks list and thus was NOT freed by xtc_loop_fini's
		 * all_tasks walk.  Reclaim it here exactly as that walk does:
		 * run its cleanup (the coroutine layer's callback releases the
		 * fiber stack + coro struct) then free the task struct.  An
		 * XTC_INB_WAKE, by contrast, references a task that IS already
		 * tracked (parked, on all_tasks) and already freed, so we must
		 * NOT touch its task here. */
		if (m->kind == XTC_INB_PUBLISH && m->task != NULL) {
			if (m->task->cleanup != NULL)
				m->task->cleanup(m->task->cleanup_arg);
			__os_free(m->task);
		}
		__os_free(m);
	}
	(void)__os_mutex_destroy(&ib->lock);
	ib->head = ib->tail = NULL;
	ib->inited = 0;
}

int
__xtc_inbox_push(struct xtc_inbox *ib, enum xtc_inbox_kind k, xtc_task_t *t)
{
	struct xtc_inbox_msg *m;
	int rc;
	if ((rc = __os_calloc(1, sizeof *m, (void **)&m)) != XTC_OK)
		return rc;
	m->kind = k;
	m->task = t;
	m->next = NULL;
	/* Critical section: the MPSC inbox is pushed by ANY thread/loop
	 * and drained by this loop's owner.  A DST fault point marks the
	 * cross-loop enqueue boundary. */
	XTC_SIM_FAULT_POINT("sched.inbox.pre_push");
	(void)__os_mutex_lock(&ib->lock);
	if (ib->tail == NULL) ib->head = ib->tail = m;
	else { ib->tail->next = m; ib->tail = m; }
	(void)__os_mutex_unlock(&ib->lock);
	return XTC_OK;
}

int
__xtc_inbox_drain(xtc_loop_t *loop)
{
	struct xtc_inbox_msg *list, *m, *n, *defer = NULL;
	int64_t drained = 0;

	(void)__os_mutex_lock(&loop->inbox.lock);
	list = loop->inbox.head;
	loop->inbox.head = loop->inbox.tail = NULL;
	(void)__os_mutex_unlock(&loop->inbox.lock);

	/* Buggify: under DST, process one FEWER message this turn -- hold
	 * back the tail message and re-queue it for the next drain.  A WAKE
	 * / PUBLISH held back simply schedules its task one step later (the
	 * loop stays runnable because inbox.head != NULL, so the scheduler
	 * steps it again and drains it): a legal one-turn delay that cannot
	 * lose a message.  Only when there is more than one message (so the
	 * loop still makes progress this turn) and never for a WAKE alone,
	 * and only under the per-call BUGGIFY-stream coin.  A no-op in
	 * production. */
	if (list != NULL && list->next != NULL &&
	    XTC_SIM_BUGGIFY("sched.inbox.drain_one_fewer") &&
	    xtc_sim_buggify_fault(250)) {
		struct xtc_inbox_msg *pp = list;
		while (pp->next->next != NULL)
			pp = pp->next;
		defer = pp->next;      /* the last message */
		pp->next = NULL;       /* detach it from the processed list */
	}

	for (m = list; m != NULL; m = n) {
		n = m->next;
		drained++;
		switch (m->kind) {
		case XTC_INB_WAKE:
			if (m->task->state == XTC_TS_PARKED) {
				m->task->state = XTC_TS_SCHEDULED;
				(void)__xtc_loop_enqueue(loop, m->task);
			} else {
				/* The wake raced the park: the task is still
				 * RUNNING (between arming its waker and yielding
				 * to PARKED).  Latch it so the RUNNING->PARKED
				 * transition re-schedules instead of parking --
				 * otherwise a cross-thread wake fired in that
				 * window would be lost (the prepare/park race). */
				atomic_store_explicit(&m->task->wake_pending, 1,
				    memory_order_release);
			}
			break;
		case XTC_INB_PUBLISH:
			m->task->all_next = loop->all_tasks;
			m->task->all_prev = NULL;
			if (loop->all_tasks != NULL)
				loop->all_tasks->all_prev = m->task;
			loop->all_tasks = m->task;
			(void)__xtc_loop_enqueue(loop, m->task);
			break;
		}
		__os_free(m);
	}
	/* Re-queue the deferred message at the FRONT of the inbox (its
	 * relative order with any newly-arrived messages does not matter --
	 * inbox delivery is unordered across threads).  Preserve replay:
	 * only the sim thread touches the inbox during a step. */
	if (defer != NULL) {
		(void)__os_mutex_lock(&loop->inbox.lock);
		defer->next = loop->inbox.head;
		loop->inbox.head = defer;
		if (loop->inbox.tail == NULL)
			loop->inbox.tail = defer;
		(void)__os_mutex_unlock(&loop->inbox.lock);
	}
	if (drained > 0 && loop->res != NULL)
		xtc_res_release(loop->res, XTC_RES_INBOX_MSGS, drained);
	return XTC_OK;
}

/* --- lifecycle ------------------------------------------------------ */

/* PUBLIC: int xtc_loop_init __P((xtc_loop_t **)); */
int
xtc_loop_init(xtc_loop_t **out)
{
	xtc_loop_t *loop;
	int rc;

	if (out == NULL) return XTC_E_INVAL;
	if ((rc = __os_calloc(1, sizeof(*loop), (void **)&loop)) != XTC_OK)
		return rc;

	if ((rc = xtc_io_init(&loop->io)) != XTC_OK)        goto fail0;
	xtc_deque_init(&loop->deque);
	if ((rc = __xtc_inbox_init(&loop->inbox)) != XTC_OK) goto fail1;

	/* Default per-loop resource accountant. */
	if ((rc = __os_calloc(1, sizeof(*loop->res),
	    (void **)&loop->res)) != XTC_OK) goto fail2;
	(void)xtc_res_init(loop->res, NULL);
	loop->owns_res = 1;

	loop->q_head = loop->q_tail = NULL;
	loop->all_tasks = NULL;
	loop->all_timers = NULL;
	loop->timer_slab = NULL;
	loop->task_free = NULL;
	loop->task_free_n = 0;
	loop->timers = NULL;
	loop->n_timers = loop->cap_timers = 0;
	atomic_store_explicit(&loop->n_alive, 0, memory_order_relaxed);
	loop->stop_requested = 0;
	loop->exec_id = -1;
	loop->exec = NULL;

	*out = loop;
	return XTC_OK;

fail2:	__xtc_inbox_fini(&loop->inbox);
fail1:	(void)xtc_io_fini(loop->io);
fail0:	__os_free(loop);
	return rc;
}

/* PUBLIC: int xtc_loop_fini __P((xtc_loop_t *)); */
int
xtc_loop_fini(xtc_loop_t *loop)
{
	xtc_task_t *t, *next_t;
	xtc_timer_t *tm, *next_tm;

	if (loop == NULL) return XTC_E_INVAL;

	__xtc_proc_loop_unregister(loop);

	for (t = loop->all_tasks; t != NULL; t = next_t) {
		next_t = t->all_next;
		if (t->cleanup != NULL) t->cleanup(t->cleanup_arg);
		/* Every task struct was malloc'd via __os_calloc (the free-list
		 * only recycles between spawns; structs still on all_tasks at
		 * fini were never recycled), so free them all here.  recyclable
		 * only marks free-list ELIGIBILITY on completion, not the
		 * allocation source. */
		__os_free(t);
	}
	for (tm = loop->all_timers; tm != NULL; tm = next_tm) {
		next_tm = tm->all_next;
		if (loop->timer_slab != NULL)
			xtc_slab_free((struct xtc_slab *)loop->timer_slab, tm);
		else
			__os_free(tm);
	}
	if (loop->timer_slab != NULL)
		xtc_slab_destroy((struct xtc_slab *)loop->timer_slab);
	/* Drain the task free-list (recycled task structs). */
	{
		xtc_task_t *ft, *fn;
		for (ft = loop->task_free; ft != NULL; ft = fn) {
			fn = ft->q_next;
			__os_free(ft);
		}
		loop->task_free = NULL;
	}
	__os_free(loop->timers);
	__xtc_inbox_fini(&loop->inbox);
	(void)xtc_io_fini(loop->io);
	if (loop->owns_res) __os_free(loop->res);
	__os_free(loop);
	return XTC_OK;
}

/* PUBLIC: struct xtc_res *xtc_loop_res __P((xtc_loop_t *)); */
xtc_res_t *
xtc_loop_res(xtc_loop_t *loop)
{
	return loop == NULL ? NULL : loop->res;
}

/* PUBLIC: int xtc_loop_stop __P((xtc_loop_t *)); */
int
xtc_loop_stop(xtc_loop_t *loop)
{
	if (loop == NULL) return XTC_E_INVAL;
	loop->stop_requested = 1;
	(void)xtc_io_wakeup(loop->io);
	return XTC_OK;
}

/* --- run-queue ops -------------------------------------------------- */

/*
 * Owner-side enqueue.  When the loop is part of an executor (i.e.,
 * exposed for work stealing), push into the Chase-Lev deque so peers
 * can steal.  When the loop is standalone (M3 single-thread mode),
 * use the slow-path FIFO so spawn order is preserved -- there is no
 * one to steal anyway.
 *
 * If the deque overflows, fall through to the FIFO.
 */
int
__xtc_loop_enqueue(xtc_loop_t *loop, xtc_task_t *t)
{
	if (t->q_next != NULL || loop->q_tail == t)
		return XTC_OK;        /* already in slow-path FIFO */

	if (loop->exec != NULL) {
		if (!t->pinned &&
		    xtc_deque_push(&loop->deque, t) == XTC_OK) {
			/* Critical section: a task pushed to the stealable deque
			 * is immediately visible to thieves on other loops. */
			XTC_SIM_FAULT_POINT("sched.enqueue.post_deque_push");
			t->q_next = NULL;
			return XTC_OK;
		}
		/* pinned, or deque full -- fall through to the owner-only
		 * FIFO, which is never work-stolen. */
	}

	t->q_next = NULL;
	if (loop->q_tail == NULL) loop->q_head = loop->q_tail = t;
	else { loop->q_tail->q_next = t; loop->q_tail = t; }
	return XTC_OK;
}

/*
 * Owner-side pop.  Prefer slow-path FIFO if non-empty (older items),
 * then deque (LIFO bottom).
 */
static xtc_task_t *
__queue_pop(xtc_loop_t *loop)
{
	xtc_task_t *t;
	if (loop->q_head != NULL) {
		t = loop->q_head;
		loop->q_head = t->q_next;
		if (loop->q_head == NULL) loop->q_tail = NULL;
		t->q_next = NULL;
		return t;
	}
	return (xtc_task_t *)xtc_deque_pop(&loop->deque);
}

/* --- main step ------------------------------------------------------ */

static int
__xtc_loop_step(xtc_loop_t *loop)
{
	xtc_task_t *t;
	xtc_io_event_t evs[16];
	int n_out, i, rc;
	int64_t now_ns, next_deadline_ns, timeout_ns;

	/* Bind the current loop for the duration of this step.  xtc_loop_run
	 * sets this once for its dedicated thread, but the DST sim scheduler
	 * multiplexes N loops on ONE thread by calling __xtc_loop_step_once
	 * directly, so each step must (re)bind it -- otherwise code running
	 * inside a fiber here (e.g. xtc_aio_*, which consults
	 * __xtc_current_loop to find its reactor) would see a stale or NULL
	 * loop and wrongly take the off-loop blocking path.  No restore is
	 * needed: every step rebinds, and xtc_loop_run already saves/
	 * restores around its own run loop. */
	__xtc_current_loop = loop;

	/* Drain any cross-thread wakers / publishes. */
	(void)__xtc_inbox_drain(loop);

	/* 1. Run queue. */
	if ((t = __queue_pop(loop)) != NULL) {
		int verdict;
		/* Buggify: under DST, DEFER this ready task one extra turn --
		 * re-enqueue it and run a DIFFERENT ready task instead (the
		 * local-run-queue twin of sched.steal.skip_near).  A task that
		 * yields its turn is exactly XTC_TASK_RESCHED, which the loop
		 * already tolerates, so this is a legal pessimal reordering.
		 * Bounded against a spin: only when ANOTHER ready task exists
		 * (so the loop still makes progress this step) and we re-pop a
		 * DIFFERENT task; if the re-pop returns the same task (it was
		 * the only one), run it.  Per-call BUGGIFY-stream coin, so it
		 * never perturbs unrelated tests.  A no-op in production. */
		if ((loop->q_head != NULL ||
		     xtc_deque_len(&loop->deque) > 0) &&
		    XTC_SIM_BUGGIFY("sched.runq.defer_ready") &&
		    xtc_sim_buggify_fault(250)) {
			xtc_task_t *other;
			t->state = XTC_TS_SCHEDULED;
			(void)__xtc_loop_enqueue(loop, t);
			other = __queue_pop(loop);
			t = (other != NULL) ? other : __queue_pop(loop);
			if (t == NULL)
				return XTC_OK;   /* nothing ready after all */
		}
		atomic_fetch_add_explicit(&loop->n_tasks_run, 1,
		    memory_order_relaxed);
		if (loop->yield_budget_ns > 0) {
			int64_t s = 0;
			(void)__os_clock_mono(&s);
			t->run_start_ns = s;   /* start of this run quantum */
		}
		t->state = XTC_TS_RUNNING;
		verdict = t->fn(t, t->user);
		switch (verdict) {
		case XTC_TASK_DONE:
			t->state = XTC_TS_DONE;
			/* Decrement the HOME loop's alive count (where spawn
			 * incremented it), not the loop currently running the
			 * task.  Under work stealing the two differ; keying on
			 * t->loop keeps each loop's n_alive correct so the
			 * executor's idle detection terminates.  Same for the
			 * resource release, which was acquired against the home
			 * loop's accountant. */
			atomic_fetch_sub_explicit(&t->loop->n_alive, 1,
			    memory_order_relaxed);
			if (t->loop->res != NULL)
				xtc_res_release(t->loop->res, XTC_RES_TASKS, 1);
			/*
			 * Recycle a completed PLAIN task (no cleanup hook) back
			 * to its home loop's task_slab instead of leaking it into
			 * all_tasks until loop_fini -- this is the spawn-heavy
			 * hot path (no malloc/free per unit, no accumulation).
			 * Only when completing on the HOME loop (t->loop ==
			 * loop): the all_tasks unlink is not thread-safe, so a
			 * task that was stolen and completes on the thief is left
			 * for fini (correct, just not recycled -- a bounded,
			 * rare case).  Coro-backed tasks (cleanup != NULL) keep
			 * the cleanup-at-fini path so fiber-stack teardown is
			 * unchanged.
			 *
			 * The `t->fn != __xtc_coro_step` guard closes a
			 * cross-thread-spawn teardown leak: xtc_async sets
			 * t->cleanup only AFTER __xtc_task_spawn_ex makes the
			 * task visible (it pushes an XTC_INB_PUBLISH to the
			 * target loop's inbox for a foreign spawn).  A short-
			 * lived coro can therefore be drained, run, and reach
			 * DONE on the target loop's thread while the spawning
			 * thread has not yet stored t->cleanup -- so cleanup
			 * reads NULL here and we would wrongly recycle the task
			 * as plain, freeing the task struct without ever
			 * releasing its fiber stack + coro (the coro leaks) and
			 * leaving the spawner's pending t->cleanup store to land
			 * on freed memory (a latent UAF).  t->fn is set before
			 * the task is published and never changes, so keying on
			 * it is race-free: any coro-backed task (fn ==
			 * __xtc_coro_step) is left on all_tasks for the fini
			 * walk, which runs its (by-then-stored) cleanup exactly
			 * once.  Plain pinned tasks (xtc_exec_spawn_on) still
			 * recycle. */
			if (t->cleanup == NULL && t->loop == loop &&
			    t->fn != __xtc_coro_step) {
				extern void __xtc_task_free(xtc_task_t *);
				__xtc_task_free(t);
				t = NULL;
			}
			break;
		case XTC_TASK_RESCHED:
			t->state = XTC_TS_SCHEDULED;
			(void)__xtc_loop_enqueue(loop, t);
			break;
		case XTC_TASK_PENDING:
			/* A cross-thread wake that raced this park (arrived while
			 * the task was still RUNNING) latched wake_pending in the
			 * inbox drain.  Consume it and re-schedule instead of
			 * parking, so the wake is not lost -- the task will run
			 * again and re-evaluate its condition (fd readiness /
			 * mailbox).  Without this, a foreign wake fired in the
			 * prepare/park window would leave the task PARKED with no
			 * further wakeup pending -> a hang. */
			if (atomic_exchange_explicit(&t->wake_pending, 0,
			    memory_order_acquire)) {
				t->state = XTC_TS_SCHEDULED;
				(void)__xtc_loop_enqueue(loop, t);
			} else {
				t->state = XTC_TS_PARKED;
			}
			break;
		default:
			return XTC_E_INTERNAL;
		}

		/*
		 * I/O fairness.  Step 1 returns right after running one task,
		 * and the caller only polls I/O once the run queue drains.  A
		 * busy run queue (fibers that keep rescheduling -- e.g. a
		 * buffer manager spinning while an evictor is blocked on a
		 * flush completion) would therefore never let the loop poll,
		 * starving the very I/O completion that would break the spin.
		 * Interleave a non-blocking poll every IO_FAIRNESS_QUANTUM
		 * runs so parked completions are dispatched under load.
		 */
#define IO_FAIRNESS_QUANTUM 64u
		if (++loop->runs_since_poll >= IO_FAIRNESS_QUANTUM &&
		    (loop->q_head != NULL ||
		     xtc_deque_len(&loop->deque) > 0)) {
			xtc_io_event_t fevs[16];
			int fn_out = 0, fi;
			loop->runs_since_poll = 0;
			if (xtc_io_poll(loop->io, fevs,
			    (int)(sizeof fevs / sizeof fevs[0]), 0,
			    &fn_out) == XTC_OK)
				for (fi = 0; fi < fn_out; fi++)
					(void)__xtc_loop_dispatch_event(loop,
					    &fevs[fi]);
		}
		return XTC_OK;
	}

	/* 2. Drain due timers. */
	if ((rc = __os_clock_mono(&now_ns)) != XTC_OK) return rc;
	for (;;) {
		xtc_timer_t *due = __xtc_timer_heap_pop_due(loop, now_ns);
		if (due == NULL) break;
		if (!due->cancelled) {
			/* Buggify: under DST, fire this timer slightly LATE once.
			 * A timer firing late is always tolerated (deadlines are a
			 * lower bound, not a promise of exact instant), so instead
			 * of firing now, re-arm it a bounded step later and let the
			 * scheduler advance the clock to it -- exercising the
			 * timeout-races-work interleaving deterministically.  Bumped
			 * at most ONCE per timer (sim_late guards re-bumping) so a
			 * late fire can never spin; only when buggify is on and the
			 * per-call BUGGIFY-stream coin lands (so it never perturbs
			 * unrelated tests' FAULT/schedule streams).  A no-op in
			 * production (XTC_SIM_BUGGIFY == 0). */
			if (!due->sim_late &&
			    XTC_SIM_BUGGIFY("timer.fire.late") &&
			    xtc_sim_buggify_fault(250)) {
				due->sim_late = 1;
				due->deadline_ns = now_ns + 1000;   /* +1us */
				due->heap_idx = -1;
				if (__xtc_timer_heap_push(loop, due) == XTC_OK)
					continue;
				/* push failed (OOM): fall through, fire now. */
			}
			due->fired = 1;
			if (due->cb != NULL) due->cb(due->user);
			if (due->waiter != NULL) {
				xtc_waker_t w = { loop, due->waiter };
				atomic_fetch_or_explicit(
				    &due->waiter->wake_revents,
				    XTC_WAIT_TIMEOUT, memory_order_relaxed);
				(void)xtc_waker_wake(&w);
				due->waiter->park_timer = NULL;
			}
		}
	}
	if (loop->q_head != NULL || xtc_deque_len(&loop->deque) > 0)
		return XTC_OK;

	next_deadline_ns = __xtc_timer_heap_next_deadline(loop);
	if (next_deadline_ns < 0 &&
	    atomic_load_explicit(&loop->n_alive, memory_order_relaxed) == 0)
		return XTC_OK;

	if (next_deadline_ns >= 0) {
		timeout_ns = next_deadline_ns - now_ns;
		if (timeout_ns < 0) timeout_ns = 0;
	} else {
		/* M5: if part of an executor and have no work locally, try
		 * stealing before blocking. */
		if (loop->exec != NULL) {
			extern void *__xtc_exec_try_steal(xtc_loop_t *me);
			xtc_task_t *stolen = __xtc_exec_try_steal(loop);
			if (stolen != NULL) {
				atomic_fetch_add_explicit(&loop->n_steals, 1,
				    memory_order_relaxed);
				stolen->q_next = NULL;
				(void)__xtc_loop_enqueue(loop, stolen);
				return XTC_OK;
			}
		}
		timeout_ns = -1;
	}

	rc = xtc_io_poll(loop->io, evs,
	    (int)(sizeof evs / sizeof evs[0]), timeout_ns, &n_out);
	if (rc != XTC_OK) return rc;

	for (i = 0; i < n_out; i++)
		(void)__xtc_loop_dispatch_event(loop, &evs[i]);

	return XTC_OK;
}

/* PUBLIC: int xtc_loop_run __P((xtc_loop_t *)); */
int
xtc_loop_run(xtc_loop_t *loop)
{
	int rc;
	xtc_loop_t *saved;
	if (loop == NULL) return XTC_E_INVAL;

	saved = __xtc_current_loop;
	__xtc_current_loop = loop;

	while (!loop->stop_requested) {
		int has_tasks  =
		    atomic_load_explicit(&loop->n_alive, memory_order_relaxed) > 0;
		int has_timers =
		    __xtc_timer_heap_next_deadline(loop) >= 0;
		if (!has_tasks && !has_timers) break;
		if ((rc = __xtc_loop_step(loop)) != XTC_OK) {
			__xtc_current_loop = saved;
			return rc;
		}
	}

	loop->stop_requested = 0;
	__xtc_current_loop = saved;
	return XTC_OK;
}

/*
 * Single-step variant used by the executor's worker loop.  Returns:
 *   1  - made progress
 *   0  - idle (no work; caller may steal or block)
 *  <0  - error
 */
int
__xtc_loop_step_once(xtc_loop_t *loop)
{
	int rc;
	int has_tasks  =
	    atomic_load_explicit(&loop->n_alive, memory_order_relaxed) > 0;
	int has_timers = __xtc_timer_heap_next_deadline(loop) >= 0;
	if (!has_tasks && !has_timers) {
		/*
		 * No local work.  If this loop is part of an executor, a
		 * peer may have stealable work -- attempt a steal before
		 * reporting idle.  Without this an idle worker (its own
		 * n_alive == 0) returns immediately and never steals, so
		 * all work piles on the loop the tasks were spawned on.
		 * On a successful steal we enqueue locally and fall through
		 * to step (to run it); otherwise we are genuinely idle and
		 * return 0 so the worker does its bounded poll + stop-flag
		 * check rather than blocking in step.
		 */
		if (loop->exec != NULL) {
			extern void *__xtc_exec_try_steal(xtc_loop_t *me);
			xtc_task_t *stolen = __xtc_exec_try_steal(loop);
			if (stolen != NULL) {
				atomic_fetch_add_explicit(&loop->n_steals, 1,
				    memory_order_relaxed);
				stolen->q_next = NULL;
				(void)__xtc_loop_enqueue(loop, stolen);
			} else {
				return 0;
			}
		} else {
			return 0;
		}
	}
	rc = __xtc_loop_step(loop);
	return rc < 0 ? rc : 1;
}

/* ---- cooperative yield watchdog ---------------------------------- *
 *
 * A non-yielding compute loop monopolises its loop thread; xtc has no
 * forcible preemption (the OS-process tier is the preemption lever).
 * These let a long compute cooperate: set a per-loop time budget, then
 * call xtc_yield_check() (or xtc_yield_if_due()) inside the loop and
 * yield when it reports the quantum over budget.  This is also the
 * queryable "over budget" signal an embedder wires to a cancellation
 * token (fire xtc_abort_source on over-budget; see xtc_svr_call_abortable).
 */

/* PUBLIC: void xtc_yield_set_budget __P((xtc_loop_t *, int64_t)); */
void
xtc_yield_set_budget(xtc_loop_t *loop, int64_t budget_ns)
{
	if (loop != NULL)
		loop->yield_budget_ns = budget_ns < 0 ? 0 : budget_ns;
}

/* PUBLIC: int xtc_yield_check __P((void)); */
int
xtc_yield_check(void)
{
	xtc_task_t *t = __xtc_current_task();
	int64_t now = 0, budget;

	if (t == NULL || t->loop == NULL)
		return 0;                       /* off a loop: never due */
	/*
	 * Preemption timer (M_PREEMPTION Phase 1, cooperative-assisted):
	 * if a per-worker preemption tick fired, this run quantum is over
	 * regardless of the manual budget, so any xtc_yield_if_due() caller
	 * responds to the timer.  A single relaxed atomic load + clear when
	 * armed; a no-op (the flag is never set) when the timer is off, so
	 * the cooperative fast path is unchanged unless preemption is
	 * enabled.  This does NOT preempt a loop that never reaches a
	 * yield-check -- that is Phase 2 (signal-context involuntary
	 * yield). */
	if (xtc_preempt_tick_pending()) {
		atomic_fetch_add_explicit(&t->loop->n_yield_due, 1,
		    memory_order_relaxed);
		return 1;
	}
	budget = t->loop->yield_budget_ns;
	if (budget <= 0 || t->run_start_ns == 0)
		return 0;                       /* watchdog disabled */
	(void)__os_clock_mono(&now);
	if (now - t->run_start_ns >= budget) {
		atomic_fetch_add_explicit(&t->loop->n_yield_due, 1,
		    memory_order_relaxed);
		return 1;
	}
	return 0;
}

/* PUBLIC: int xtc_yield_if_due __P((void)); */
int
xtc_yield_if_due(void)
{
	if (xtc_yield_check()) {
		xtc_yield();
		return 1;
	}
	return 0;
}

/* PUBLIC: uint64_t xtc_yield_due_count __P((const xtc_loop_t *)); */
uint64_t
xtc_yield_due_count(const xtc_loop_t *loop)
{
	if (loop == NULL)
		return 0;
	return atomic_load_explicit(&loop->n_yield_due, memory_order_relaxed);
}
