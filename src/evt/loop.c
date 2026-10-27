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
#include "xtc_tail.h"     /* SCHED: per-loop poll liveness */
#include "tail_int.h"     /* __xtc_tail_emit / __xtc_tail_on */
#include "xtc_probes.h"
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
#include "xtc_inject.h"   /* loop.enqueue.fifo_link test hook */

#include <stdint.h>
#include <unistd.h>

/* Per-thread cursor -- see loop_int.h. */
XTC_THREAD_LOCAL xtc_loop_t *__xtc_current_loop = NULL;

/* Out-of-line read for fiber code after a stack switch: see loop_int.h. */
#if defined(_MSC_VER)
__declspec(noinline)
#else
__attribute__((noinline))
#endif
xtc_loop_t *
__xtc_current_loop_get(void)
{
	xtc_loop_t **volatile slot = &__xtc_current_loop;
	return *slot;
}

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
void  (*__xtc_fiber_kill_poll)(void) = NULL;

/* Loop-fini hook; installed by the process layer (proc.c) so the loop
 * can release its per-loop proc table at fini without depending on the
 * L3 proc layer directly.  NULL until a process is spawned. */
void  (*__xtc_loop_fini_hook)(xtc_loop_t *loop) = NULL;

/* --- inbox ---------------------------------------------------------- */

/*
 * The inbox is two lock-free LIFO stacks threaded through the tasks'
 * inbox_next (see struct xtc_inbox).  It used to be a mutex-guarded list
 * of calloc'd nodes: one allocation and one lock round trip per cross-
 * loop wake, and an ENOMEM silently dropped the wake, stranding a parked
 * receiver whose message was already in its mailbox.  A task can be on
 * at most one inbox at a time (loop_int.h, inbox_next), so the link field
 * is enough and a push cannot fail.
 */

int
__xtc_inbox_init(struct xtc_inbox *ib)
{
	atomic_store_explicit(&ib->pub_head, NULL, memory_order_relaxed);
	atomic_store_explicit(&ib->wake_head, NULL, memory_order_relaxed);
	atomic_store_explicit(&ib->sleeping, 0, memory_order_relaxed);
	ib->inited = 1;
	return XTC_OK;
}

void
__xtc_inbox_fini(struct xtc_inbox *ib)
{
	xtc_task_t *t, *n;
	if (!ib->inited) return;
	/* An undrained PUBLISH carries a task that was spawned cross-thread
	 * but never ran, so it was never linked into the loop's all_tasks
	 * list and thus was NOT freed by xtc_loop_fini's all_tasks walk.
	 * Reclaim it here exactly as that walk does: run its cleanup (the
	 * coroutine layer's callback releases the fiber stack + coro struct)
	 * then free the task struct.  A WAKE entry, by contrast, is a task
	 * that IS already tracked (parked, on all_tasks) and already freed,
	 * so the wake stack is dropped without touching its tasks. */
	for (t = atomic_load_explicit(&ib->pub_head, memory_order_acquire);
	    t != NULL; t = n) {
		n = t->inbox_next;
		if (t->cleanup != NULL)
			t->cleanup(t->cleanup_arg);
		__os_free(t);
	}
	atomic_store_explicit(&ib->pub_head, NULL, memory_order_relaxed);
	atomic_store_explicit(&ib->wake_head, NULL, memory_order_relaxed);
	ib->inited = 0;
}

int
__xtc_inbox_nonempty(const struct xtc_inbox *ib)
{
	return atomic_load_explicit(&((struct xtc_inbox *)ib)->pub_head,
	    memory_order_acquire) != NULL ||
	    atomic_load_explicit(&((struct xtc_inbox *)ib)->wake_head,
	    memory_order_acquire) != NULL;
}

/* XTC_NOALLOC_BEGIN: cross-loop inbox push + drain -- the per-wake path
 * (__xtc_inbox_push, __xtc_inbox_drain) links tasks intrusively and must
 * never touch the allocator. */

/* Push t onto one of the inbox stacks.  The CAS (seq_cst) publishes the
 * task's fields to the drain's exchange. */
static void
__inbox_stack_push(_Atomic(xtc_task_t *) *head, xtc_task_t *t)
{
	xtc_task_t *old = atomic_load_explicit(head, memory_order_relaxed);
	do {
		t->inbox_next = old;
	} while (!atomic_compare_exchange_weak(head, &old, t));
}

/*
 * Queue t on loop's inbox and nudge the loop.  Cannot fail.  The caller
 * must own t's single inbox slot: a PUBLISH of a freshly spawned task,
 * or the WAKE of a waker that won t's PARKED->SCHEDULED transition.
 * The caller must not touch t afterwards (the loop may run and free it).
 */
int
__xtc_inbox_push(xtc_loop_t *loop, enum xtc_inbox_kind k, xtc_task_t *t)
{
	/* Critical section: the MPSC inbox is pushed by ANY thread/loop
	 * and drained by this loop's owner.  A DST fault point marks the
	 * cross-loop enqueue boundary. */
	XTC_SIM_FAULT_POINT("sched.inbox.pre_push");
	__inbox_stack_push(k == XTC_INB_PUBLISH ? &loop->inbox.pub_head :
	    &loop->inbox.wake_head, t);
	/*
	 * Nudge the loop only if its owner is blocked (or about to block) in
	 * its poller.  A running owner drains the inbox at the top of its
	 * next step and re-checks it before it blocks, so writing the wakeup
	 * fd to a running loop was a wasted write(2) per cross-loop wake --
	 * one per message under load, ~10% of all cycles in a cross-loop
	 * ping-pong at 2-16 loops (helio's WakeupIfNeeded skips it the same
	 * way).  Dekker with __xtc_inbox_sleep_begin: the push above and the
	 * owner's sleeping store are seq_cst, then each side reads the
	 * other's, so either the owner sees this task or we see sleeping.
	 * The exchange lets one producer per sleep write; the owner drains
	 * everything pushed meanwhile when that write wakes it.  The task is
	 * queued whatever the nudge returns; a failed nudge only delays it
	 * to the loop's next turn (an executor loop re-polls every
	 * millisecond), so it must not be reported as a failed wake/spawn.
	 */
	if (atomic_load(&loop->inbox.sleeping) != 0 &&
	    atomic_exchange(&loop->inbox.sleeping, 0) != 0 &&
	    xtc_io_wakeup(loop->io) != XTC_OK)
		atomic_store(&loop->inbox.sleeping, 1);   /* next push retries */
	return XTC_OK;
}

/*
 * Owner side of the nudge elision (see __xtc_inbox_push).  Publish
 * "about to block", then look at the inbox once more: a producer that
 * pushed before the store is seen here, one that pushes after it sees
 * sleeping == 1 and writes the wakeup fd.  Returns 1 (and withdraws
 * the flag) if work is already queued, so the caller must not block.
 */
int
__xtc_inbox_sleep_begin(xtc_loop_t *loop)
{
	atomic_store(&loop->inbox.sleeping, 1);
	if (atomic_load(&loop->inbox.pub_head) != NULL ||
	    atomic_load(&loop->inbox.wake_head) != NULL) {
		atomic_store_explicit(&loop->inbox.sleeping, 0,
		    memory_order_relaxed);
		return 1;
	}
	return 0;
}

/* The blocking poll returned: producers need not nudge until the next
 * __xtc_inbox_sleep_begin.  A producer that already took the flag wrote
 * the fd; that byte at most makes a later poll return early. */
void
__xtc_inbox_sleep_end(xtc_loop_t *loop)
{
	atomic_store_explicit(&loop->inbox.sleeping, 0, memory_order_relaxed);
}

/* Reverse a LIFO-pushed chain into arrival order. */
static xtc_task_t *
__inbox_reverse(xtc_task_t *t)
{
	xtc_task_t *r = NULL, *n;
	for (; t != NULL; t = n) {
		n = t->inbox_next;
		t->inbox_next = r;
		r = t;
	}
	return r;
}

/* Detach and return the last task of a non-empty chain. */
static xtc_task_t *
__inbox_detach_last(xtc_task_t **list)
{
	xtc_task_t **pp = list, *last;
	while ((*pp)->inbox_next != NULL)
		pp = &(*pp)->inbox_next;
	last = *pp;
	*pp = NULL;
	return last;
}

/*
 * Owner-only.  Returns the number of tasks drained (>= 0).
 */
int
__xtc_inbox_drain(xtc_loop_t *loop)
{
	xtc_task_t *pub, *wake, *t, *n, *defer = NULL;
	int defer_pub = 0;
	int64_t n_pub = 0;
	int drained = 0;

	if (atomic_load(&loop->inbox.pub_head) == NULL &&
	    atomic_load(&loop->inbox.wake_head) == NULL)
		return 0;
	pub = __inbox_reverse(atomic_exchange(&loop->inbox.pub_head, NULL));
	wake = __inbox_reverse(atomic_exchange(&loop->inbox.wake_head, NULL));

	/* Buggify: under DST, process one FEWER task this turn -- hold back
	 * the last one and re-queue it for the next drain.  A WAKE / PUBLISH
	 * held back simply schedules its task one step later (the loop stays
	 * runnable because its inbox is non-empty, so the scheduler steps it
	 * again and drains it): a legal one-turn delay that cannot lose a
	 * task.  Only when there is more than one entry (so the loop still
	 * makes progress this turn), and only under the per-call
	 * BUGGIFY-stream coin.  A no-op in production. */
	if (((pub != NULL && (pub->inbox_next != NULL || wake != NULL)) ||
	    (wake != NULL && wake->inbox_next != NULL)) &&
	    XTC_SIM_BUGGIFY("sched.inbox.drain_one_fewer") &&
	    xtc_sim_buggify_fault(250)) {
		if (wake != NULL && wake->inbox_next != NULL)
			defer = __inbox_detach_last(&wake);
		else if (pub != NULL && pub->inbox_next != NULL) {
			defer = __inbox_detach_last(&pub);
			defer_pub = 1;
		} else {
			defer = wake;      /* exactly one PUBLISH + one WAKE */
			wake = NULL;
		}
	}

	for (t = pub; t != NULL; t = n) {
		n = t->inbox_next;   /* read before the task becomes visible */
		drained++;
		n_pub++;
		t->all_next = loop->all_tasks;
		t->all_prev = NULL;
		if (loop->all_tasks != NULL)
			loop->all_tasks->all_prev = t;
		loop->all_tasks = t;
		(void)__xtc_loop_enqueue(loop, t);
	}
	/* WAKE: the waker already moved the task PARKED->SCHEDULED (its CAS
	 * is what entitled it to queue the task here), and a PARKED task is
	 * in no run queue, so the enqueue cannot race a dispatch of the same
	 * task on another loop. */
	for (t = wake; t != NULL; t = n) {
		n = t->inbox_next;
		drained++;
		(void)__xtc_loop_enqueue(loop, t);
	}
	/* Re-queue the deferred task (its relative order with newly arrived
	 * entries does not matter -- inbox delivery is unordered across
	 * threads).  Only the sim thread touches the inbox during a step. */
	if (defer != NULL)
		__inbox_stack_push(defer_pub ? &loop->inbox.pub_head :
		    &loop->inbox.wake_head, defer);
	/* Only a cross-thread PUBLISH charges XTC_RES_INBOX_MSGS
	 * (__xtc_task_spawn_ex), so release exactly those.  Releasing one
	 * per drained entry, WAKEs included, released units nobody charged
	 * and drove the count negative under cross-loop traffic. */
	if (n_pub > 0 && loop->res != NULL)
		xtc_res_release(loop->res, XTC_RES_INBOX_MSGS, n_pub);
	return drained;
}

/* XTC_NOALLOC_END */

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
	atomic_store_explicit(&loop->stop_requested, 0, memory_order_relaxed);
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

	if (loop == NULL) return XTC_E_INVAL;

	if (__xtc_loop_fini_hook != NULL)
		__xtc_loop_fini_hook(loop);
	/* lifetime: tasks handed back by other loops are still on all_tasks;
	 * move them to the free list first so the walk below skips them. */
	__xtc_task_reclaim_drain(loop);

	for (t = loop->all_tasks; t != NULL; t = next_t) {
		next_t = t->all_next;
		/*
		 * Release any park timer still referenced by this task's slot.
		 * A park timer armed but never fired/cancelled (a fiber torn
		 * down while parked in a timed recv/sleep at exec shutdown) is
		 * reachable ONLY through this slot once it has left the heap, so
		 * the heap-array walk below would miss it.  Exchange + release
		 * so it is freed exactly once regardless of whether the heap
		 * still holds its other reference (freed there) or not (freed
		 * here).  Single-threaded at fini, so the exchange is just for
		 * symmetry with the running-path release.
		 */
		{
			xtc_timer_t *pt = atomic_exchange_explicit(
			    &t->park_timer, NULL, memory_order_relaxed);
			if (pt != NULL)
				__xtc_timer_park_release(pt);
		}
		if (t->cleanup != NULL) t->cleanup(t->cleanup_arg);
		/* Every task struct was malloc'd via __os_calloc (the free-list
		 * only recycles between spawns; structs still on all_tasks at
		 * fini were never recycled), so free them all here.  recyclable
		 * only marks free-list ELIGIBILITY on completion, not the
		 * allocation source. */
		__os_free(t);
	}
	/*
	 * Refcounted park timers (xtc_proc_sleep / recv / wait_fd timeouts)
	 * are NOT on all_timers -- a fired or cancelled one is freed during
	 * the run by __xtc_timer_park_release, so only those still armed at
	 * fini remain, and an armed park timer is always on the heap (its
	 * two owners are the heap and a parked task's slot).  The loop is
	 * stopped and single-threaded here, so free each heap entry with
	 * refs > 0 outright; the dangling slot pointers in the already-freed
	 * task structs are never dereferenced again.
	 */
	{
		int i;
		for (i = 0; i < loop->n_timers; i++) {
			xtc_timer_t *ht = loop->timers[i].t;
			if (ht != NULL &&
			    atomic_load_explicit(&ht->refs,
			    memory_order_relaxed) > 0)
				__xtc_timer_park_release(ht);  /* drop the heap ref; frees at 0 */
		}
	}
	if (loop->timer_slab != NULL)
		xtc_slab_destroy((struct xtc_slab *)loop->timer_slab);
	/* Drain the task free-list (recycled task structs). */
	__xtc_task_spare_fini(loop);
	{
		xtc_task_t *ft, *fn;
		for (ft = loop->task_free; ft != NULL; ft = fn) {
			fn = ft->q_next;
			__os_free(ft);
		}
		loop->task_free = NULL;
		for (ft = loop->task_free_mig; ft != NULL; ft = fn) {
			fn = ft->q_next;
			__os_free(ft);
		}
		loop->task_free_mig = NULL;
	}
	__os_free(loop->timers);
	__xtc_inbox_fini(&loop->inbox);
	/* After every cleanup above: those may have refilled the spare. */
	__xtc_coro_stack_spare_fini(loop);
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
	/* Store BEFORE the wakeup: the wakeup is what makes the loop re-check,
	 * so the flag must already be visible when it does. */
	atomic_store_explicit(&loop->stop_requested, 1, memory_order_relaxed);
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
	else {
		loop->q_tail->q_next = t;
		/* Test hook: a preemption signal raised here lands between
		 * the link and the tail store (test_preempt_p2 /runq_*). */
		XTC_INJECTION_POINT("loop.enqueue.fifo_link");
		loop->q_tail = t;
	}
	return XTC_OK;
}

/*
 * Owner-side pop of the default lane: the owner-only FIFO (pinned tasks
 * and deque overflow) and the stealable deque.  Both are taken oldest
 * first, and when both hold work the loop alternates between them.  A
 * task that returns XTC_TASK_RESCHED is re-queued at the back of its lane,
 * so it runs again only after everything already queued there.
 *
 * The deque used to be popped LIFO from the bottom and the FIFO always
 * won: a migratable fiber that yielded was re-pushed and immediately
 * re-popped, so every other migratable fiber on the loop starved until it
 * finished (two yielders: counts [0, 1655348] in one second), and a
 * yielding pinned task starved every migratable one the same way.
 */
static xtc_task_t *
__default_pop(xtc_loop_t *loop)
{
	xtc_task_t *t;
	int fifo = loop->q_head != NULL;

	if (fifo && xtc_deque_len(&loop->deque) > 0)
		fifo = (loop->pop_flip ^= 1u) != 0;
	if (!fifo && (t = xtc_deque_take(&loop->deque)) != NULL)
		return t;
	if ((t = loop->q_head) == NULL)
		return NULL;
	loop->q_head = t->q_next;
	if (loop->q_head == NULL) loop->q_tail = NULL;
	t->q_next = NULL;
	return t;
}

/*
 * Owner-side pop: the class pick when classes exist, else (or when the
 * default lane wins it) __default_pop.
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

	return __default_pop(loop);
}

#ifdef XTC_ENABLE_SCHED_POLICY
/* Runtime alone removes task links. At most one policy callback per pop;
 * a stolen default lane falls back, never hiding class work behind idle. */
static xtc_task_t *
__policy_pop(xtc_loop_t *loop)
{
	xtc_task_t *t = NULL;
	struct xtc_run_class *rc;
	int lane, i;
	if (loop->sched.policy.select == NULL)
		return __queue_pop(loop);
	lane = __xtc_sched_select(loop);
	if (lane >= 0) {
		rc = &loop->classes[lane];
		t = rc->q_head;
		if (t != NULL) {
			rc->q_head = t->q_next;
			if (rc->q_head == NULL) rc->q_tail = NULL;
			t->q_next = NULL;
			rc->runs++;
		}
	} else if (lane == -1) {
		t = __default_pop(loop);
	}
	if (t != NULL)
		return t;
	if (loop->sched.stats.fallbacks != UINT64_MAX)
		loop->sched.stats.fallbacks++;
	t = __queue_pop(loop);
	/* A thief can also win the ordinary pick's default pop. With that
	 * lane now empty, the next ordinary pick must see owner-only classes. */
	if (t == NULL)
		for (i = 0; i < loop->n_classes; i++)
			if (loop->classes[i].q_head != NULL) {
				t = __queue_pop(loop);
				break;
			}
	return t;
}
#endif

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
			XTC_PROBE_TIMER_FIRE((uintptr_t)due, due->deadline_ns,
			    now_ns);
			if (due->cb != NULL) due->cb(due->user);
			if (due->waiter != NULL) {
				xtc_task_t *waiter = due->waiter;
				xtc_waker_t w = { loop, waiter };
				xtc_timer_t *slot;
				atomic_fetch_or_explicit(
				    &waiter->wake_revents,
				    XTC_WAIT_TIMEOUT, memory_order_relaxed);
				(void)xtc_waker_wake(&w);
				/*
				 * Clear the waiter's park_timer slot only if it
				 * still names THIS timer, so exactly one side
				 * (this drain or the task-side cancel) drops the
				 * slot's reference.  If the slot holds another
				 * timer, the fiber already took this one out and
				 * released it, then re-armed -- typically after
				 * being stolen, on another loop's thread.  An
				 * unconditional exchange took THAT timer's slot
				 * reference: it could free a timer still in a
				 * peer's heap and read a node nothing published
				 * to this thread (the TSan report at timer.c's
				 * refs load, m5 Blk4).  The heap's reference is
				 * dropped unconditionally below (we popped it),
				 * and holding it rules out ABA on `due`.
				 */
				slot = due;
				if (atomic_compare_exchange_strong_explicit(
				    &waiter->park_timer, &slot, NULL,
				    memory_order_relaxed, memory_order_relaxed))
					__xtc_timer_park_release(due);  /* XTC_NOALLOC_OK: bounded free of one park timer */
			}
			__xtc_timer_park_release(due);  /* XTC_NOALLOC_OK: drop the heap reference of the fired timer */
		} else {
			/*
			 * The timer was cancelled AFTER pop_due returned it as due
			 * (a migrated fiber's cancel_park_timer on another thread
			 * raced this drain, between the pop and this check).  We
			 * still own the heap reference of the popped node, so drop
			 * it here or it leaks -- the cancel released only the slot
			 * reference.  (ASan LeakSanitizer, m5 Blk4.)
			 */
			__xtc_timer_park_release(due);  /* XTC_NOALLOC_OK: drop the heap reference of a raced-cancel timer */
		}
	}
	return XTC_OK;
}

/* --- main step ------------------------------------------------------ */

static int __runq_nonempty(const xtc_loop_t *loop);



static int
__xtc_loop_step(xtc_loop_t *loop)
{
	xtc_task_t *t;
	/* Zeroed, then .fd forced to -1 before each poll: xtc_io_event_t.fd
	 * is consumed by __xtc_loop_dispatch_event, and an uninitialised 0
	 * reads as a VALID fd (fd 0 is stdin) -- which made dispatch try to
	 * unregister fd 0.  Backends set it, but the consumer must not depend
	 * on every fill path in every backend getting it right. */
	xtc_io_event_t evs[16];
	int n_out, i, rc;
#ifdef XTC_ENABLE_SCHED_POLICY
	int selected_lane = -1;
#endif
	int64_t now_ns, next_deadline_ns, timeout_ns;

#ifdef XTC_ENABLE_SCHED_POLICY
	__xtc_sched_freeze(loop);
#endif

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
	__xtc_task_reclaim_drain(loop);   /* lifetime: tasks handed back */

	/* 1. Run queue. */
#ifdef XTC_ENABLE_SCHED_POLICY
	if ((t = __policy_pop(loop)) != NULL) {
#else
	if ((t = __queue_pop(loop)) != NULL) {
#endif
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
		if (
#ifdef XTC_ENABLE_SCHED_POLICY
		    /* Replacing the selected task would bypass the policy
		     * runtime's mandatory lane fairness override. */
		    loop->sched.policy.select == NULL &&
#endif
		    (loop->q_head != NULL ||
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
#ifdef XTC_ENABLE_SCHED_POLICY
		if (loop->sched.policy.select != NULL) {
			selected_lane = t->sched_class;
			if (selected_lane < 0 || selected_lane >= loop->n_classes ||
			    !loop->classes[selected_lane].in_use)
				selected_lane = -1;
			__xtc_sched_service(loop, selected_lane);
		}
#endif
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
		XTC_PROBE_TASK_RUN_START((uintptr_t)t, loop->exec_id,
		    t->sched_class);
		verdict = t->fn(t, t->user);
		XTC_PROBE_TASK_RUN_DONE((uintptr_t)t, loop->exec_id, verdict);
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
			if (!t->pinned && t->loop->exec != NULL) {
				extern void __xtc_exec_mig_alive(xtc_loop_t *, int);
				__xtc_exec_mig_alive(t->loop, -1);
			}
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
			} else if (t->reclaim) {
				/* lifetime: a finished proc fiber (task.c). */
				__xtc_task_reclaim(loop, t);
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
				/* seq_cst store + the seq_cst exchange below pair
				 * with __task_wake_claim's latch-then-load-state
				 * (src/evt/task.c): of this re-check and a waker's
				 * re-check, at least one sees the other's write. */
				atomic_store(&t->state, XTC_TS_PARKED);
				/*
				 * NINTH SURFACE.  The exchange above and this
				 * store are NOT one atomic step, so a waker can
				 * land BETWEEN them and be lost:
				 *
				 *   this thread: exchange(wake_pending,0) -> 0
				 *                                (decides to park)
				 *   waker thread:  CAS(state, PARKED->SCHEDULED)
				 *                  sees state == RUNNING, FAILS,
				 *                  latches wake_pending = 1
				 *   this thread: store(state, PARKED)
				 *
				 * The task is now PARKED with wake_pending set,
				 * and the ONLY consumer of wake_pending is the
				 * exchange above -- which already ran.  Nothing
				 * re-delivers the wake, so the fiber is stranded
				 * forever.  Both wakers behave correctly (each
				 * latches on a lost CAS, as of the 7th-surface
				 * fix); the defect is that the park decision was
				 * not re-validated after the state became
				 * visible to them.
				 *
				 * Re-check now that state == PARKED is published.
				 * A waker arriving from here on WINS its CAS and
				 * enqueues normally, so this is the last window
				 * that needs closing.  Use the same
				 * PARKED->SCHEDULED CAS the wakers use: whoever
				 * wins enqueues exactly once, so a wake that
				 * arrives concurrently with this re-check cannot
				 * double-enqueue.
				 */
				if (atomic_exchange(&t->wake_pending, 0)) {
					int exp = XTC_TS_PARKED;
					if (atomic_compare_exchange_strong_explicit(
					    &t->state, &exp, XTC_TS_SCHEDULED,
					    memory_order_acq_rel,
					    memory_order_acquire))
						(void)__xtc_loop_enqueue(loop, t);
				}
			}
			break;
		default:
			return XTC_E_INTERNAL;
		}

#ifdef XTC_ENABLE_SCHED_POLICY
		if (loop->sched.policy.select != NULL) {
			/* A pending verdict consumed by a latched wake is a yield. */
			if (verdict == XTC_TASK_PENDING && t != NULL &&
			    atomic_load_explicit(&t->state, memory_order_acquire) != XTC_TS_PARKED)
				verdict = XTC_TASK_RESCHED;
			__xtc_sched_ran(loop, selected_lane, verdict);
		}
#endif

		/*
		 * I/O fairness.  Step 1 returns right after running one task,
		 * and the caller only polls I/O once the run queue drains.  A
		 * busy run queue (fibers that keep rescheduling -- e.g. a
		 * buffer manager spinning while an evictor is blocked on a
		 * flush completion) would therefore never let the loop poll,
		 * starving the very I/O completion that would break the spin.
		 * Interleave a non-blocking poll every IO_FAIRNESS_QUANTUM
		 * runs so parked completions are dispatched under load.
		 *
		 * Whether or not the run queue is empty right now: fibers
		 * that alternate on short timers (each parks with its peer's
		 * deadline already due) leave it empty after every run, and
		 * step 2 refills it before the blocking poll below is ever
		 * reached, so no poll ran at all -- a 1-loop sqlxtc test
		 * (6 readers on 300 us sleeps) reaped its writer's completed
		 * aio about once a second (test_aio /timer_churn).  The blocking
		 * poll resets the count, so a loop that reaches it pays
		 * nothing here.
		 */
#define IO_FAIRNESS_QUANTUM 64u
#ifndef IO_SUBMIT_QUANTUM
#define IO_SUBMIT_QUANTUM 8u   /* a power of two */
#endif
		if (++loop->runs_since_poll >= IO_FAIRNESS_QUANTUM) {
			xtc_io_event_t fevs[16];
			int fn_out = 0, fi;
			for (fi = 0; fi < (int)(sizeof fevs / sizeof fevs[0]);
			    fi++)
				fevs[fi].fd = -1;
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
#if defined(XTC_IO_BACKEND_URING)
		else if (verdict == XTC_TASK_RESCHED ||
		    (loop->runs_since_poll & (IO_SUBMIT_QUANTUM - 1u)) == 0)
			/* Producers leave SQEs queued for the next poll, which
			 * a busy run queue postpones: submit them after a yield
			 * and every IO_SUBMIT_QUANTUM runs (io_uring.c
			 * __submit_wait). */
			__xtc_io_flush(loop->io);
#endif
		return XTC_OK;
	}

	/* 2. Drain due timers. */
	if ((rc = __xtc_drain_due_timers(loop)) != XTC_OK) return rc;
	if (__runq_nonempty(loop))
		return XTC_OK;

	/*
	 * Run queue empty.  An executor loop tries to steal a peer's queued
	 * migratable work before it blocks, whether or not it owns parked
	 * fibers or pending timers, and while it waits it re-checks on a
	 * short tick (__xtc_exec_steal_recheck_ns) instead of sleeping until
	 * its own next deadline.  Stealing used to happen here only when the
	 * loop had no pending timer (or eager rebalance was on), so a single
	 * parked xtc_proc_sleep or timed recv -- i.e. every real loop --
	 * turned stealing off: 64 runnable migratable procs on loop 0 and a
	 * loop 1 holding one sleeping proc gave steals [0, 0] for the whole
	 * sleep.
	 */
	if (loop->exec != NULL) {
		extern void *__xtc_exec_try_steal(xtc_loop_t *me);
		xtc_task_t *stolen = __xtc_exec_try_steal(loop);
		if (stolen != NULL) {
			atomic_fetch_add_explicit(&loop->n_steals, 1,
			    memory_order_relaxed);
			stolen->q_next = NULL;
			(void)__xtc_loop_enqueue(loop, stolen);
			/* A steal skips this step's blocking poll, and a loop
			 * that keeps finding peer work (each stolen fiber parks
			 * at once) would otherwise never reap its own ring --
			 * stranding a fiber parked here on an fd or an aio
			 * completion.  Poll it once, without blocking, per
			 * steal. */
			for (i = 0; i < (int)(sizeof evs / sizeof evs[0]); i++)
				evs[i].fd = -1;
			n_out = 0;
			if (xtc_io_poll(loop->io, evs,
			    (int)(sizeof evs / sizeof evs[0]), 0,
			    &n_out) == XTC_OK)
				for (i = 0; i < n_out; i++)
					(void)__xtc_loop_dispatch_event(loop,
					    &evs[i]);
			return XTC_OK;   /* run it next step */
		}
	}

	/* Block in xtc_io_poll until the next timer deadline (or an I/O
	 * event).  Re-read the clock -- the timer drain above may have fired
	 * callbacks that took measurable time -- so the poll timeout below is
	 * computed against now, not a stale sample. */
	if ((rc = __os_clock_mono(&now_ns)) != XTC_OK) return rc;
	next_deadline_ns = __xtc_timer_heap_next_deadline(loop);
	if (next_deadline_ns < 0 &&
	    atomic_load_explicit(&loop->n_alive, memory_order_relaxed) == 0)
		return XTC_OK;

	if (next_deadline_ns >= 0) {
		timeout_ns = next_deadline_ns - now_ns;
		if (timeout_ns < 0) timeout_ns = 0;
	} else
		timeout_ns = -1;
	if (loop->exec != NULL) {
		extern int64_t __xtc_exec_steal_recheck_ns(xtc_loop_t *);
		int64_t rk = __xtc_exec_steal_recheck_ns(loop);
		if (rk > 0 && (timeout_ns < 0 || timeout_ns > rk))
			timeout_ns = rk;
	}
	if (timeout_ns != 0 && __xtc_inbox_sleep_begin(loop))
		return XTC_OK;   /* a wake/spawn arrived: drain it first */

	for (i = 0; i < (int)(sizeof evs / sizeof evs[0]); i++)
		evs[i].fd = -1;
	loop->runs_since_poll = 0;
	rc = xtc_io_poll(loop->io, evs,
	    (int)(sizeof evs / sizeof evs[0]), timeout_ns, &n_out);
	if (timeout_ns != 0)
		__xtc_inbox_sleep_end(loop);
	if (rc != XTC_OK) return rc;
	XTC_PROBE_LOOP_POLL(loop->exec_id, timeout_ns, n_out);

	/*
	 * xtc_tail SCHED: this loop completed an IDLE poll on its OWN ring
	 * (one that dispatched nothing).  The value is liveness: a stream of
	 * LOOP_POLL for a loop_id proves that loop is still turning, which is
	 * the only way to tell "the loop kept working and skipped one parked
	 * fiber" from "the loop stopped" when a PARK has no matching RUN.
	 *
	 * ONLY the idle poll is recorded, and that restriction is load-bearing.
	 * The first version emitted every poll, and at 32 loops that was
	 * 87-93% of a 16384-slot ring -- measured by a consumer across three
	 * captures.  The ring wrapped, evicting the PARK/RUN events the
	 * liveness answer is compared against, and 23 of 33 loops showed ZERO
	 * polls purely because their events had been evicted.  That turned two
	 * of their verdicts into artifacts: "count == 0" is unfalsifiable when
	 * the event being counted is what overflowed the buffer.  An
	 * observability event that crowds out the data it explains is worse
	 * than none.
	 *
	 * A poll that DISPATCHED work needs no separate liveness record -- the
	 * events it dispatched are themselves proof the loop ran.  The
	 * unobservable case is precisely the poll that found nothing, so that
	 * is the one worth a slot.  Same question answered, ~2 orders of
	 * magnitude fewer events.
	 *
	 * pid carries only loop_id -- a loop is not a proc.  Gated, so a
	 * disabled tail is one branch.
	 */
	if (n_out == 0 && __xtc_tail_on(XTC_TAIL_SCHED)) {
		xtc_pid_t lp;
		memset(&lp, 0, sizeof lp);
		lp.loop_id = (uint32_t)(loop->exec_id >= 0 ? (unsigned)loop->exec_id :
		    XTC_TAIL_LOOP_NONE);
		__xtc_tail_emit(XTC_TAIL_SCHED, XTC_TAIL_LOOP_POLL, lp, 0);
	}

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
#ifdef XTC_ENABLE_SCHED_POLICY
	__xtc_sched_freeze(loop);
#endif

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

	while (!atomic_load_explicit(&loop->stop_requested,
	    memory_order_relaxed)) {
		int has_tasks  =
		    atomic_load_explicit(&loop->n_alive, memory_order_relaxed) > 0;
		int has_timers =
		    __xtc_timer_heap_next_deadline(loop) >= 0;
		if (!has_tasks && !has_timers) break;
		if ((rc = __xtc_loop_step(loop)) != XTC_OK) {
			__xtc_io_owner_leave(loop->io);
			__xtc_current_loop = saved;
			return rc;
		}
	}

	atomic_store_explicit(&loop->stop_requested, 0, memory_order_relaxed);
	/* Hand the io back (see the matching call in the exec worker). */
	__xtc_io_owner_leave(loop->io);
	__xtc_current_loop = saved;
	return XTC_OK;
}

/*
 * Single-step variant used by the executor's worker loop.  Returns:
 *   1  - made progress
 *   0  - idle (no work; caller may steal or block)
 *  <0  - error
 */
/*
 * Does this loop hold work it can RUN RIGHT NOW (its own FIFO, its class
 * FIFOs, or its stealable deque)?
 *
 * This is NOT the same question as n_alive > 0, and conflating the two is a
 * real defect.  n_alive counts tasks HOMED here: spawn increments it and the
 * DONE verdict decrements against t->loop, the HOME loop.  It therefore
 * counts a PARKED fiber (correctly -- that is what keeps the executor from
 * quiescing while a completion is outstanding) and does NOT count a
 * foreign-homed task sitting in THIS loop's queues.  Two things put one
 * there: a task stolen from a peer and re-enqueued locally, and a task woken
 * by a completion reaped here (__xtc_loop_dispatch_event builds the waker
 * with w.loop = the REAPING loop, so the enqueue targets this loop even when
 * the task is homed elsewhere).
 *
 * MEASURED: 165,733 times in one 8-loop run, a loop took the "no local work"
 * branch while its own deque was non-empty.  The consequence is not a strand
 * -- the queued task does run on a later turn -- but reporting idle makes the
 * executor account the worker as idle and grow its poll timeout toward the
 * 32 ms backoff cap, so a runnable task waits on a sleep it should never
 * have entered.  A latency bug, which is why no functional test caught it.
 *
 * Used ONLY to decide "may I report idle?", never to skip the !has_tasks
 * branch itself.  That branch is load-bearing: it polls this loop's own ring
 * before trying to steal, which is what keeps a fiber parked here on an
 * xtc_aio_* completion from being stranded.  An earlier attempt folded the
 * queue check into has_tasks and thereby skipped the poll; it hung
 * m5/exec/Blk2_concurrent_commit.  Keep the two questions separate.
 *
 * Owner-thread call only.  q_head is owner-written; the deque indices are
 * atomic and a stale read is harmless here -- a false "empty" costs one
 * extra idle turn (the task is still queued and runs next turn), and a false
 * "non-empty" costs one extra step that finds nothing.  Neither loses work.
 */
static int
__runq_nonempty(const xtc_loop_t *loop)
{
	int i;

	if (loop->q_head != NULL)
		return 1;
	if (atomic_load_explicit(&loop->deque.bottom, memory_order_relaxed) >
	    atomic_load_explicit(&loop->deque.top, memory_order_relaxed))
		return 1;
	/* L1 proportional-share: a class-tagged task lives on its class's
	 * own ready FIFO, never the shared deque, so those must be checked
	 * too or a loop running only class-tagged work looks idle. */
	for (i = 0; i < loop->n_classes; i++) {
		if (loop->classes[i].in_use &&
		    loop->classes[i].q_head != NULL)
			return 1;
	}
	return 0;
}

static int __loop_step_once(xtc_loop_t *loop);

int
__xtc_loop_step_once(xtc_loop_t *loop)
{
	int rc = __loop_step_once(loop);
	atomic_store_explicit(&loop->n_timers_pub, loop->n_timers,
	    memory_order_relaxed);
	return rc;
}

static int
__loop_step_once(xtc_loop_t *loop)
{
	int rc, has_tasks, has_timers;

#ifdef XTC_ENABLE_SCHED_POLICY
	__xtc_sched_freeze(loop);
#endif
	/* Bind this thread's current-loop to `loop` up front, BEFORE the
	 * pre-step steal below enqueues onto it -- __xtc_loop_step (called
	 * at the end) sets it too, but the steal-enqueue happens first, and
	 * the DIAGNOSTIC owner guard (and any code consulting the binding)
	 * must see the correct stepping loop, not a stale prior one. */
	__xtc_current_loop = loop;
	has_tasks =
	    atomic_load_explicit(&loop->n_alive, memory_order_relaxed) > 0;
	has_timers = __xtc_timer_heap_next_deadline(loop) >= 0;
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
			} else if (__runq_nonempty(loop) ||
			    __xtc_inbox_nonempty(&loop->inbox)) {
				/* Nothing to steal, but we DO hold runnable work --
				 * fall through to run it rather than reporting idle
				 * (see __runq_nonempty). */
			} else {
				return 0;
			}
		} else if (!__runq_nonempty(loop) && !__xtc_inbox_nonempty(&loop->inbox)) {
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
	 * path above is the trigger.
	 *
	 * Consult the loop this fiber is RUNNING on, not its HOME loop
	 * (t->loop).  __xtc_io_uring_preempt_due is not a pure read: on a
	 * due tick it calls io_uring_cq_advance (moving that ring's CQ head)
	 * and re-arms the timeout SQE.  A migratable fiber that has been
	 * work-stolen runs on a peer thread while t->loop still names its
	 * home loop, so keying on t->loop mutated ANOTHER loop's preempt
	 * ring -- its single-producer SQ and its CQ head -- from the wrong
	 * thread.  That is the same cross-loop-mutation family as the
	 * v1.40.3 fd-registry and v1.40.7 io->fds races.  The executor arms
	 * one preempt ring per worker, so the running loop's ring is the one
	 * that describes this thread's quantum anyway.
	 */
	{
		xtc_loop_t *rl = __xtc_current_loop != NULL
		    ? __xtc_current_loop : t->loop;
		if (__xtc_io_uring_preempt_due(rl->io)) {
			atomic_fetch_add_explicit(&rl->n_yield_due, 1,
			    memory_order_relaxed);
			return 1;
		}
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
