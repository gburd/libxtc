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
#include "xtc_exec.h"
#include "xtc_preempt.h"
#include "os_time.h"
#include "os_backtrace.h"
#include "xtc_log.h"
#include "xtc_sim.h"

#include <stdint.h>
#include <unistd.h>

/* Per-thread cursor -- see loop_int.h. */
XTC_THREAD_LOCAL xtc_loop_t *__xtc_current_loop = NULL;

#if defined(XTC_DIAGNOSTIC)
#include <stdio.h>
#include <stdlib.h>
/*
 * DIAGNOSTIC: a non-owner thread mutated one of this loop's owner-only
 * structures.  This is the cross-loop-race category (v1.40.1..v1.40.4);
 * abort loudly and precisely rather than let it corrupt a list/heap and
 * strand a fiber under load.  Compiled out in a normal build.
 */
void
__xtc_loop_owner_violation(const struct xtc_loop *loop, const char *site)
{
	fprintf(stderr,
	    "XTC DIAGNOSTIC: cross-loop mutation of owner-only \"%s\" on "
	    "loop %p (exec_id=%d): touched by a thread that is not its "
	    "owner -- a work-stolen fiber resumed on the wrong thread and "
	    "mutated a single-owner structure.  This is the cross-loop "
	    "race category; fix the site to route the mutation to the "
	    "owning loop.\n",
	    site, (const void *)loop, loop->exec_id);
	abort();
}
#endif

/* Fiber-context preservation hooks; installed by the process layer
 * (proc.c) on first spawn.  NULL until then -- see loop_int.h. */
void *(*__xtc_fiber_ctx_save)(void) = NULL;
void  (*__xtc_fiber_ctx_restore)(void *) = NULL;
void  (*__xtc_fiber_kill_check)(void) = NULL;

/* Loop-fini hook; installed by the process layer (proc.c) so the loop
 * can release its per-loop proc table at fini without depending on the
 * L3 proc layer directly.  NULL until a process is spawned. */
void  (*__xtc_loop_fini_hook)(xtc_loop_t *loop) = NULL;

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
			{
			/*
			 * CAS the PARKED->SCHEDULED transition.  This handler
			 * runs on THIS loop's thread, but the target task may
			 * be a migratable fiber currently being dispatched or
			 * run on ANOTHER loop (a stale WAKE that targeted the
			 * task's waker loop after the task was work-stolen).
			 * A plain read-check-write of task->state would race
			 * that peer loop's dispatch store (loop.c
			 * t->state = XTC_TS_RUNNING) and could double-enqueue
			 * the task.  The CAS makes the transition atomic:
			 * exactly one PARKED->SCHEDULED winner enqueues; a task
			 * that is already SCHEDULED/RUNNING/DONE (not PARKED)
			 * loses the CAS and instead latches wake_pending, so
			 * the RUNNING->PARKED verdict re-schedules rather than
			 * losing the wake (the prepare/park race, unchanged).
			 * A PARKED task is in no run queue, so the winning
			 * enqueue cannot race a concurrent dispatch of the same
			 * task. */
			int expect = XTC_TS_PARKED;
			if (atomic_compare_exchange_strong_explicit(
			    &m->task->state, &expect, XTC_TS_SCHEDULED,
			    memory_order_acq_rel, memory_order_acquire)) {
				(void)__xtc_loop_enqueue(loop, m->task);
			} else {
				/* Not PARKED (raced the park, or running on a
				 * peer): latch so the eventual RUNNING->PARKED
				 * verdict re-schedules instead of parking. */
				atomic_store_explicit(&m->task->wake_pending, 1,
				    memory_order_release);
			}
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

	if (__xtc_loop_fini_hook != NULL)
		__xtc_loop_fini_hook(loop);

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

/* PUBLIC: int xtc_loop_wake __P((xtc_loop_t *)); */
int
xtc_loop_wake(xtc_loop_t *loop)
{
	if (loop == NULL) return XTC_E_INVAL;
	/* Nudge the loop's poller out of its I/O wait so it re-polls and
	 * re-checks runnability.  Lost-wake-free against the pre-sleep
	 * window: xtc_io_wakeup writes the loop's wakeup fd, which the
	 * backends keep armed across the drain (io_uring re-arm-before-drain;
	 * epoll/kqueue level-triggered), so a wake issued at any time
	 * surfaces on the next poll.  See the header for the cross-thread
	 * producer-must-nudge contract. */
	return xtc_io_wakeup(loop->io);
}

/* --- run-queue ops -------------------------------------------------- */

/* XTC_NOALLOC_BEGIN: scheduler run-queue + main step (PLAN.md 19.23) --
 * the per-tick dispatch path (__xtc_loop_enqueue, __queue_pop,
 * __xtc_loop_step, __xtc_loop_step_once) must never touch the
 * allocator; task and timer structs are pre-allocated (task free-
 * list in evt/task.c, timer slab in evt/timer.c) precisely so this
 * hot path stays allocation-free per PLAN.md 19.23. */

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
	XTC_ASSERT_LOOP_OWNER(loop, "run-queue (enqueue)");
	if (t->q_next != NULL || loop->q_tail == t)
		return XTC_OK;        /* already in slow-path FIFO */

	/* L1 proportional-share: a class-tagged task on a loop that has
	 * that class goes onto the class's own ready FIFO (never the
	 * stealable deque -- classes are an owner-side overlay).  A stolen
	 * task whose tag index does not exist on this loop falls through to
	 * the default path below, so cross-loop steals never lose a task.
	 * INSPIRED BY Glommio's per-task-queue ready set. */
	if (t->sched_class >= 0 && t->sched_class < loop->n_classes &&
	    loop->classes[t->sched_class].in_use) {
		struct xtc_run_class *rc = &loop->classes[t->sched_class];
		/* Guard against a double-enqueue into a class FIFO (mirrors
		 * the q_tail == t guard above for the default FIFO). */
		if (rc->q_tail == t)
			return XTC_OK;
		t->q_next = NULL;
		if (rc->q_tail == NULL)
			rc->q_head = rc->q_tail = t;
		else { rc->q_tail->q_next = t; rc->q_tail = t; }
		return XTC_OK;
	}

	if (loop->exec != NULL) {
		if (!t->pinned) {
			/* Initialize the task's fields BEFORE publishing it to the
			 * stealable deque: once xtc_deque_push() succeeds the task
			 * is immediately visible to thieves on other loops, and a
			 * thief that steals it writes stolen->q_next -- so writing
			 * t->q_next after the push races the thief on the same
			 * field (both store NULL so harmless in value, but a real
			 * publish-before-init ordering bug ThreadSanitizer rightly
			 * flags).  Set it first, then publish. */
			t->q_next = NULL;
			if (xtc_deque_push(&loop->deque, t) == XTC_OK) {
				XTC_SIM_FAULT_POINT("sched.enqueue.post_deque_push");
				/* Eager rebalance: nudge one idle peer so it steals
				 * this promptly instead of waiting for its poll edge
				 * (no-op unless eager rebalance is on and a peer is
				 * idle). */
				{
					extern void __xtc_exec_nudge_idle_peer(xtc_loop_t *);
					__xtc_exec_nudge_idle_peer(loop);
				}
				return XTC_OK;
			}
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
	XTC_ASSERT_LOOP_OWNER(loop, "run-queue (pop)");

	/* L1 proportional-share: when at least one class exists, run a
	 * min-vruntime pick over the in-use classes AND the implicit
	 * default lane (the plain FIFO/deque, for untagged tasks, weighted
	 * with XTC_DEFAULT_CLASS_SHARES).  Including the default lane in the
	 * race means always-ready class work can never starve background
	 * untagged work -- it just out-weighs it in proportion to shares.
	 * A tiny linear scan over the few classes (no heap, no alloc -- the
	 * dispatch-path contract).  INSPIRED BY Glommio's min-vruntime queue
	 * pick over its task queues incl. the default queue. */
	if (loop->n_classes > 0) {
		int i, best = -1;    /* -1 => default lane is best so far */
		uint64_t best_vr;
		int default_ready = (loop->q_head != NULL ||
		    xtc_deque_len(&loop->deque) > 0);

		best_vr = default_ready ? loop->default_vruntime : 0;
		if (!default_ready)
			best = -2;   /* -2 => nothing chosen yet */
		for (i = 0; i < loop->n_classes; i++) {
			struct xtc_run_class *rc = &loop->classes[i];
			if (!rc->in_use || rc->q_head == NULL)
				continue;
			if (best == -2 || rc->vruntime < best_vr) {
				best = i;
				best_vr = rc->vruntime;
			}
		}
		if (best >= 0) {
			struct xtc_run_class *rc = &loop->classes[best];
			t = rc->q_head;
			rc->q_head = t->q_next;
			if (rc->q_head == NULL)
				rc->q_tail = NULL;
			t->q_next = NULL;
			rc->runs++;
			return t;
		}
		/* best == -1: default lane wins the race -> fall through to
		 * the plain FIFO/deque pop below.  best == -2: nothing ready
		 * at all -> that pop returns NULL. */
	}

	if (loop->q_head != NULL) {
		t = loop->q_head;
		loop->q_head = t->q_next;
		if (loop->q_head == NULL) loop->q_tail = NULL;
		t->q_next = NULL;
		return t;
	}
	return (xtc_task_t *)xtc_deque_pop(&loop->deque);
}

/*
 * L1: create a run class on `loop`.  Owner-only (called from the loop's
 * own thread, or before the loop runs).  Precomputes the reciprocal
 * (1<<22)/shares used by the vruntime account formula (Glommio's
 * shares.rs) and recomputes the loop's effective latency bound.
 */
int
__xtc_loop_class_create(xtc_loop_t *loop, int shares, int64_t latency_ns,
    int *out_idx)
{
	struct xtc_run_class *rc;
	int idx, i;

	if (loop == NULL || shares < 1 || shares > 1000 || latency_ns < 0)
		return XTC_E_INVAL;
	if (loop->n_classes >= XTC_LOOP_MAX_CLASSES)
		return XTC_E_AGAIN;

	idx = loop->n_classes;
	rc = &loop->classes[idx];
	rc->shares = shares;
	rc->latency_ns = latency_ns;
	/* Glommio's reciprocal_shares: (1 << 22) / shares.  vruntime then
	 * accrues delta_ns * reciprocal >> 12, so a class with more shares
	 * accrues virtual time more SLOWLY and is picked more often. */
	rc->reciprocal = ((uint64_t)1 << 22) / (uint64_t)shares;
	/* Start the new class at the current minimum vruntime so a
	 * late-created class is not unfairly favoured (vruntime 0) nor
	 * starved.  Matches CFS's place_entity min-vruntime seeding. */
	{
		uint64_t minvr = 0;
		int have = 0;
		for (i = 0; i < loop->n_classes; i++) {
			if (!loop->classes[i].in_use)
				continue;
			if (!have || loop->classes[i].vruntime < minvr) {
				minvr = loop->classes[i].vruntime;
				have = 1;
			}
		}
		rc->vruntime = minvr;
	}
	rc->q_head = rc->q_tail = NULL;
	rc->in_use = 1;
	loop->n_classes = idx + 1;

	/* Recompute the effective latency bound: the smallest non-zero
	 * latency over all classes (like Glommio's reevaluate_preempt_timer
	 * picking the tightest active deadline). */
	loop->class_latency_ns = 0;
	for (i = 0; i < loop->n_classes; i++) {
		int64_t l = loop->classes[i].latency_ns;
		if (l > 0 && (loop->class_latency_ns == 0 ||
		    l < loop->class_latency_ns))
			loop->class_latency_ns = l;
	}
	/* A latency class shrinks the loop's effective yield/preempt
	 * interval so a long-running peer cannot hold the core past the
	 * bound: arm (or tighten) the cooperative yield watchdog to the
	 * tightest latency bound.  Coordinates with loop->yield_budget_ns
	 * (the existing xtc_yield_check watchdog); a cooperative fiber that
	 * calls xtc_yield_if_due then yields within the bound, and the
	 * min-vruntime pick runs the latency class next.  INSPIRED BY
	 * Glommio's reevaluate_preempt_timer.  Only ever shrinks the
	 * interval (never loosens a stricter budget the consumer set). */
	if (loop->class_latency_ns > 0 &&
	    (loop->yield_budget_ns == 0 ||
	     loop->class_latency_ns < loop->yield_budget_ns))
		loop->yield_budget_ns = loop->class_latency_ns;

	if (out_idx != NULL)
		*out_idx = idx;
	return XTC_OK;
}

/*
 * Fire every timer whose deadline has passed.  Allocation-free (the
 * dispatch-path contract); safe to call from both the run-queue-empty
 * step-2 path AND the busy-run-queue IO-fairness path.  Draining due
 * timers under a busy run queue is the timer analogue of the I/O
 * fairness poll: a run queue that never empties (fibers that keep
 * rescheduling -- e.g. a busy reader busy-yielding) would otherwise
 * never let step 2 run, starving a xtc_proc_sleep / recv-timeout /
 * any deadline indefinitely.  Returns XTC_OK, or a clock error.
 */
static int
__xtc_drain_due_timers(xtc_loop_t *loop)
{
	int64_t now_ns;
	int rc;

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
	return XTC_OK;
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
			atomic_store_explicit(&t->state, XTC_TS_SCHEDULED,
			    memory_order_release);
			(void)__xtc_loop_enqueue(loop, t);
			other = __queue_pop(loop);
			t = (other != NULL) ? other : __queue_pop(loop);
			if (t == NULL)
				return XTC_OK;   /* nothing ready after all */
		}
		atomic_fetch_add_explicit(&loop->n_tasks_run, 1,
		    memory_order_relaxed);
		/* Record this run quantum's start when any time-accounting
		 * feature is active: the cooperative yield watchdog, the L1
		 * proportional-share scheduler (needs elapsed to accrue
		 * vruntime), or the L3 stall watchdog (needs elapsed to detect
		 * an overrun).  When ALL are off (the default) this is skipped,
		 * so the hot path takes no clock read -- unchanged. */
		if (loop->yield_budget_ns > 0 || loop->n_classes > 0 ||
		    loop->stall_budget_ns > 0) {
			int64_t s = 0;
			(void)__os_clock_mono(&s);
			t->run_start_ns = s;   /* start of this run quantum */
		}
		atomic_store_explicit(&t->state, XTC_TS_RUNNING,
		    memory_order_release);
		verdict = t->fn(t, t->user);
		/*
		 * L1 + L3 run-end time accounting.  Read the clock ONCE at the
		 * verdict boundary and use it for both the proportional-share
		 * vruntime accrual and the over-budget stall check.  Guarded on
		 * (n_classes || stall_budget): zero cost when both are off.
		 * INSPIRED BY Glommio (account_vruntime + stall detector).
		 */
		if (loop->n_classes > 0 || loop->stall_budget_ns > 0) {
			int64_t end_ns = 0, elapsed;
			(void)__os_clock_mono(&end_ns);
			elapsed = end_ns - t->run_start_ns;
			if (elapsed < 0)
				elapsed = 0;
			/* L1: accrue weighted virtual runtime to the task's class.
			 * Glommio's exact formula (executor/mod.rs account_vruntime
			 * + shares.rs): vruntime += (cost_ns * reciprocal) >> 12,
			 * where reciprocal = (1<<22)/shares.  A higher-shares class
			 * accrues vruntime more slowly, so __queue_pop's min-vruntime
			 * pick chooses it more often -> a proportional CPU share.
			 *
			 * cost_ns is the measured run time, floored to a minimum
			 * quantum.  The floor guarantees vruntime always advances --
			 * essential under the deterministic simulator, where the
			 * virtual clock does not advance WITHIN a compute run (start
			 * and end read the same virtual instant, elapsed 0) so an
			 * unfloored accrual would leave every class at vruntime 0 and
			 * the pick would degenerate.  With equal-cost runs the floor
			 * makes the run ratio between classes equal their shares
			 * ratio (reduction-style accounting), which is exactly what
			 * the DST gate proves.  In production, a real run's elapsed
			 * dominates the floor, so the accounting stays Glommio-
			 * faithful (weighted by actual CPU time). */
			int64_t cost = elapsed < XTC_VRUNTIME_MIN_QUANTUM_NS
			    ? XTC_VRUNTIME_MIN_QUANTUM_NS : elapsed;
			if (loop->n_classes > 0 && t->sched_class >= 0 &&
			    t->sched_class < loop->n_classes &&
			    loop->classes[t->sched_class].in_use) {
				struct xtc_run_class *rc =
				    &loop->classes[t->sched_class];
				rc->vruntime += ((uint64_t)cost *
				    rc->reciprocal) >> 12;
			} else if (loop->n_classes > 0) {
				/* Untagged (default-lane) task: accrue against the
				 * implicit default lane so it races the classes
				 * fairly in __queue_pop and cannot be starved. */
				loop->default_vruntime += ((uint64_t)cost *
				    XTC_DEFAULT_CLASS_RECIP) >> 12;
			}
			/* L3: over-budget stall watchdog.  When a single run
			 * exceeded the budget, report it (callback or log) with a
			 * backtrace of where the loop was.  See
			 * __xtc_loop_stall_report. */
			if (loop->stall_budget_ns > 0 &&
			    elapsed >= loop->stall_budget_ns) {
				extern void __xtc_loop_stall_report(xtc_loop_t *,
				    xtc_task_t *, int64_t, int64_t);
				__xtc_loop_stall_report(loop, t, elapsed,
				    loop->stall_budget_ns);
			}
		}
		switch (verdict) {
		case XTC_TASK_DONE:
			atomic_store_explicit(&t->state, XTC_TS_DONE,
			    memory_order_release);
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
			atomic_store_explicit(&t->state, XTC_TS_SCHEDULED,
			    memory_order_release);
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
				atomic_store_explicit(&t->state, XTC_TS_SCHEDULED,
				    memory_order_release);
				(void)__xtc_loop_enqueue(loop, t);
			} else {
				atomic_store_explicit(&t->state, XTC_TS_PARKED,
				    memory_order_release);
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
			/* Same fairness for TIMERS: a busy run queue never
			 * empties, so step 2 (below) never runs -- fire due
			 * timers here too, or a xtc_proc_sleep / recv-timeout /
			 * any deadline is starved indefinitely under load.
			 * (Symmetric with the I/O poll above; found via a
			 * busy-yielding-reader bench that hung a timer on a
			 * saturated single loop.) */
			(void)__xtc_drain_due_timers(loop);
		}
		return XTC_OK;
	}

	/* 2. Drain due timers. */
	if ((rc = __xtc_drain_due_timers(loop)) != XTC_OK) return rc;
	if (loop->q_head != NULL || xtc_deque_len(&loop->deque) > 0)
		return XTC_OK;

	/*
	 * Run queue empty (but we may still own parked fibers or timers, so
	 * n_alive > 0).  Under eager rebalance, try to steal a sibling's
	 * runnable migratable proc BEFORE blocking in the poller on our own
	 * fds -- this is what lets a loop whose backends are all parked on
	 * sockets run a peer's runnable query instead of idling.  Without
	 * eager rebalance a loop steals only when FULLY idle (the branch
	 * below), so a loop that owns parked fibers never rebalances.
	 */
	if (loop->exec != NULL) {
		extern int __xtc_exec_eager(xtc_loop_t *);
		if (__xtc_exec_eager(loop)) {
			extern void *__xtc_exec_try_steal(xtc_loop_t *me);
			xtc_task_t *stolen = __xtc_exec_try_steal(loop);
			if (stolen != NULL) {
				atomic_fetch_add_explicit(&loop->n_steals, 1,
				    memory_order_relaxed);
				stolen->q_next = NULL;
				(void)__xtc_loop_enqueue(loop, stolen);
				return XTC_OK;   /* run it next step */
			}
		}
	}

	/* Run queue empty: block in xtc_io_poll until the next timer
	 * deadline (or an I/O event).  Re-read the clock -- the timer drain
	 * above may have fired callbacks that took measurable time -- so the
	 * poll timeout below is computed against now, not a stale sample. */
	if ((rc = __os_clock_mono(&now_ns)) != XTC_OK) return rc;
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
	/* This thread owns loop->io for the duration; record it so any
	 * cross-thread xtc_proc_wait_fd cleanup defers (see
	 * __xtc_io_set_owner). */
	{
		extern void __xtc_io_set_owner(xtc_io_t *);
		__xtc_io_set_owner(loop->io);
	}
#if defined(XTC_DIAGNOSTIC)
	/* Standalone (single-thread) run: this thread owns the loop for the
	 * duration.  Record it so the owner-only-structure guards fire on a
	 * stray cross-thread mutation. */
	loop->owner_tid = pthread_self();
	loop->owner_set = 1;
#endif

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
	/* Bind this thread's current-loop to `loop` up front, BEFORE the
	 * pre-step steal below enqueues onto it -- __xtc_loop_step (called
	 * at the end) sets it too, but the steal-enqueue happens first, and
	 * the DIAGNOSTIC owner guard (and any code consulting the binding)
	 * must see the correct stepping loop, not a stale prior one. */
	__xtc_current_loop = loop;
	int has_tasks  =
	    atomic_load_explicit(&loop->n_alive, memory_order_relaxed) > 0;
	int has_timers = __xtc_timer_heap_next_deadline(loop) >= 0;
	if (!has_tasks && !has_timers) {
		/*
		 * No locally-homed alive task and no timer.  But this loop may
		 * still own a PARKED fiber whose completion is pending on its
		 * OWN io ring: a migratable fiber that submitted an
		 * xtc_aio_* here and yielded is charged to its HOME loop's
		 * n_alive (see loop.c DONE bookkeeping), so has_tasks reads 0
		 * here even though this loop must reap that completion.  Reap
		 * our own io FIRST -- a non-blocking poll that dispatches any
		 * ready completion, making the woken owner locally runnable --
		 * BEFORE trying to steal.  Without this, under eager rebalance
		 * the loop busy-spins steal->run->park->steal (each stolen
		 * fiber parks immediately, keeping has_tasks==0) and NEVER
		 * polls its own ring, so a fiber parked here on an
		 * xtc_aio_fdatasync -- possibly holding a lock every peer needs
		 * (PG's WALWriteLock) -- is stranded forever (the native-path
		 * concurrent-commit collapse, 2026-08-30).
		 */
		if (loop->exec != NULL) {
			extern void *__xtc_exec_try_steal(xtc_loop_t *me);
			xtc_task_t *stolen;
			xtc_io_event_t oevs[16];
			int on_out = 0, oi;
			if (xtc_io_poll(loop->io, oevs,
			    (int)(sizeof oevs / sizeof oevs[0]), 0,
			    &on_out) == XTC_OK && on_out > 0) {
				for (oi = 0; oi < on_out; oi++)
					(void)__xtc_loop_dispatch_event(loop,
					    &oevs[oi]);
				/* A dispatched completion enqueued the woken
				 * owner locally; run it. */
				rc = __xtc_loop_step(loop);
				return rc < 0 ? rc : 1;
			}
			stolen = __xtc_exec_try_steal(loop);
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

/* XTC_NOALLOC_END */

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
	/*
	 * L2 io_uring ring-pointer preempt (INSPIRED BY Glommio's
	 * need_preempt: reactor.rs + sys/uring.rs preempt_pointers).  On
	 * the io_uring backend the executor arms a rearmed TIMEOUT SQE on a
	 * dedicated ring instead of the SIGVTALRM timer; "due" is then two
	 * relaxed/acquire loads of that ring's head/tail -- no signal, so
	 * this slices a long compute fiber with no signal delivered.  A
	 * no-op (returns 0) on every non-uring backend, where the signal
	 * path above is the trigger. */
	if (__xtc_io_uring_preempt_due(t->loop->io)) {
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

/* ---- L3 over-budget stall watchdog -------------------------------- *
 *
 * INSPIRED BY Glommio's stall detector (executor/stall.rs): when a
 * single task run exceeds a budget, report WHICH code monopolized the
 * core.  Glommio uses a background thread + timerfd + SIGUSR1; libxtc
 * does the cheaper thing the plan calls for -- a wall-clock check at
 * the run-end boundary in __xtc_loop_step (it already has run_start_ns
 * and can read the clock at verdict time), so there is NO extra thread
 * and NO signal, and it is a single branch on stall_budget_ns == 0 when
 * off (proven zero-overhead by test/m14).
 */

/* PUBLIC: void xtc_loop_set_stall_budget __P((xtc_loop_t *, int64_t)); */
void
xtc_loop_set_stall_budget(xtc_loop_t *loop, int64_t budget_ns)
{
	if (loop != NULL)
		loop->stall_budget_ns = budget_ns < 0 ? 0 : budget_ns;
}

/* PUBLIC: void xtc_loop_set_stall_cb __P((xtc_loop_t *, xtc_stall_cb, void *)); */
void
xtc_loop_set_stall_cb(xtc_loop_t *loop, xtc_stall_cb cb, void *user)
{
	if (loop != NULL) {
		loop->stall_cb = cb;
		loop->stall_cb_user = user;
	}
}

/* PUBLIC: uint64_t xtc_loop_stall_count __P((const xtc_loop_t *)); */
uint64_t
xtc_loop_stall_count(const xtc_loop_t *loop)
{
	if (loop == NULL)
		return 0;
	return atomic_load_explicit(&loop->n_stalls, memory_order_relaxed);
}

/*
 * Called from __xtc_loop_step's run-end boundary when a run exceeded
 * the loop's stall budget.  Bumps the telemetry counter and reports:
 * either the consumer callback, or (when none is set) a WARN log line
 * plus a backtrace of the loop emitted to stderr -- "loop L, task went
 * over-budget (ran X ms, budget Y ms), here:".  Not on the hot path
 * unless a stall actually fired, so the symbolizing cost is paid only
 * on an overrun.
 */
void
__xtc_loop_stall_report(xtc_loop_t *loop, xtc_task_t *task, int64_t ran_ns,
    int64_t budget_ns)
{
	atomic_fetch_add_explicit(&loop->n_stalls, 1, memory_order_relaxed);

	if (loop->stall_cb != NULL) {
		loop->stall_cb(loop, task, ran_ns, budget_ns,
		    loop->stall_cb_user);
		return;
	}

	/* Default sink: log the overrun, then dump a backtrace of where
	 * the loop is (the offending run has returned by now, but the
	 * loop's own frames still point at the dispatch path, and the
	 * A3 causal trace -- if enabled -- explains how the parked/ran
	 * proc reached here).  Reuse os_backtrace.h. */
	xtc_log_write(xtc_log_default(), XTC_LOG_WARN,
	    "loop %d: task %p went over-budget (ran %lld ms, budget %lld ms)",
	    loop->exec_id, (void *)task,
	    (long long)(ran_ns / (1000 * 1000LL)),
	    (long long)(budget_ns / (1000 * 1000LL)));

	if (__os_backtrace_supported()) {
		void *frames[32];
		int n = __os_backtrace(frames, 32);
		if (n > 0)
			__os_backtrace_emit(2 /* stderr */, frames, n);
	}
}
