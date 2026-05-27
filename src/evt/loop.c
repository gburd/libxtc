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

#include <stdint.h>
#include <unistd.h>

/* Per-thread cursor -- see loop_int.h. */
__thread xtc_loop_t *__xtc_current_loop = NULL;

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
	for (m = ib->head; m != NULL; m = n) { n = m->next; __os_free(m); }
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
	(void)__os_mutex_lock(&ib->lock);
	if (ib->tail == NULL) ib->head = ib->tail = m;
	else { ib->tail->next = m; ib->tail = m; }
	(void)__os_mutex_unlock(&ib->lock);
	return XTC_OK;
}

int
__xtc_inbox_drain(xtc_loop_t *loop)
{
	struct xtc_inbox_msg *list, *m, *n;
	int64_t drained = 0;

	(void)__os_mutex_lock(&loop->inbox.lock);
	list = loop->inbox.head;
	loop->inbox.head = loop->inbox.tail = NULL;
	(void)__os_mutex_unlock(&loop->inbox.lock);

	for (m = list; m != NULL; m = n) {
		n = m->next;
		drained++;
		switch (m->kind) {
		case XTC_INB_WAKE:
			if (m->task->state == XTC_TS_PARKED) {
				m->task->state = XTC_TS_SCHEDULED;
				(void)__xtc_loop_enqueue(loop, m->task);
			}
			break;
		case XTC_INB_PUBLISH:
			m->task->all_next = loop->all_tasks;
			loop->all_tasks = m->task;
			(void)__xtc_loop_enqueue(loop, m->task);
			break;
		}
		__os_free(m);
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
		next_t = t->all_next; __os_free(t);
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
		if (xtc_deque_push(&loop->deque, t) == XTC_OK) {
			t->q_next = NULL;
			return XTC_OK;
		}
		/* deque full -- fall through to slow path */
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

	/* Drain any cross-thread wakers / publishes. */
	(void)__xtc_inbox_drain(loop);

	/* 1. Run queue. */
	if ((t = __queue_pop(loop)) != NULL) {
		int verdict;
		t->state = XTC_TS_RUNNING;
		verdict = t->fn(t, t->user);
		switch (verdict) {
		case XTC_TASK_DONE:
			t->state = XTC_TS_DONE;
			atomic_fetch_sub_explicit(&loop->n_alive, 1,
			    memory_order_relaxed);
			if (loop->res != NULL)
				xtc_res_release(loop->res, XTC_RES_TASKS, 1);
			break;
		case XTC_TASK_RESCHED:
			t->state = XTC_TS_SCHEDULED;
			(void)__xtc_loop_enqueue(loop, t);
			break;
		case XTC_TASK_PENDING:
			t->state = XTC_TS_PARKED;
			break;
		default:
			return XTC_E_INTERNAL;
		}
		return XTC_OK;
	}

	/* 2. Drain due timers. */
	if ((rc = __os_clock_mono(&now_ns)) != XTC_OK) return rc;
	for (;;) {
		xtc_timer_t *due = __xtc_timer_heap_pop_due(loop, now_ns);
		if (due == NULL) break;
		if (!due->cancelled) {
			due->fired = 1;
			if (due->cb != NULL) due->cb(due->user);
			if (due->waiter != NULL) {
				xtc_waker_t w = { loop, due->waiter };
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
	if (!has_tasks && !has_timers) return 0;
	rc = __xtc_loop_step(loop);
	return rc < 0 ? rc : 1;
}
