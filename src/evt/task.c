/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/evt/task.c
 *	Task spawn / waker / park-on-{timer,fd}.
 *	See M3_CLAIMS.md, Ts1-Ts6, Wk1-Wk4, Io1-Io3.
 */

#include "xtc_int.h"
#include "loop_int.h"

#include <stdint.h>
#include <unistd.h>

/*
 * PUBLIC: int xtc_task_spawn __P((xtc_loop_t *, xtc_task_fn, void *, xtc_task_t **));
 */
int
xtc_task_spawn(xtc_loop_t *loop, xtc_task_fn fn, void *user,
               xtc_task_t **out_task)
{
	xtc_task_t *t;
	int rc;

	if (loop == NULL || fn == NULL)
		return XTC_E_INVAL;

	/* Charge the task slot before allocating.  If the cap is hit we
	 * return XTC_E_RESOURCE without ever touching malloc. */
	if (loop->res != NULL) {
		if ((rc = xtc_res_acquire(loop->res, XTC_RES_TASKS, 1)) != XTC_OK)
			return rc;
	}

	if ((rc = __os_calloc(1, sizeof(*t), (void **)&t)) != XTC_OK) {
		if (loop->res != NULL)
			xtc_res_release(loop->res, XTC_RES_TASKS, 1);
		return rc;
	}
	t->fn = fn;
	t->user = user;
	t->loop = loop;
	t->state = XTC_TS_SCHEDULED;
	t->q_next = NULL;
	t->park_timer = NULL;
	t->park_fd = -1;
	t->wake_revents = 0;

	if (out_task) *out_task = t;

	if (__xtc_current_loop == loop) {
		t->all_next = loop->all_tasks;
		loop->all_tasks = t;
		atomic_fetch_add_explicit(&loop->n_alive, 1,
		    memory_order_relaxed);
		(void)__xtc_loop_enqueue(loop, t);
		return XTC_OK;
	}

	/* Cross-thread publish.  Charge inbox slot too. */
	if (loop->res != NULL) {
		rc = xtc_res_acquire(loop->res, XTC_RES_INBOX_MSGS, 1);
		if (rc != XTC_OK) {
			xtc_res_release(loop->res, XTC_RES_TASKS, 1);
			__os_free(t);
			if (out_task) *out_task = NULL;
			return rc;
		}
	}

	atomic_fetch_add_explicit(&loop->n_alive, 1, memory_order_relaxed);
	if ((rc = __xtc_inbox_push(&loop->inbox, XTC_INB_PUBLISH, t)) != XTC_OK) {
		atomic_fetch_sub_explicit(&loop->n_alive, 1, memory_order_relaxed);
		if (loop->res != NULL) {
			xtc_res_release(loop->res, XTC_RES_INBOX_MSGS, 1);
			xtc_res_release(loop->res, XTC_RES_TASKS, 1);
		}
		__os_free(t);
		if (out_task) *out_task = NULL;
		return rc;
	}
	(void)xtc_io_wakeup(loop->io);
	return XTC_OK;
}

/*
 * PUBLIC: int xtc_task_waker __P((xtc_task_t *, xtc_waker_t *));
 */
int
xtc_task_waker(xtc_task_t *task, xtc_waker_t *out)
{
	if (task == NULL || out == NULL)
		return XTC_E_INVAL;
	out->loop = task->loop;
	out->task = task;
	return XTC_OK;
}

/*
 * PUBLIC: int xtc_waker_wake __P((const xtc_waker_t *));
 *
 *   - PARKED   -> SCHEDULED + enqueue   (the normal path)
 *   - SCHEDULED-> no-op                 (Wk3 idempotent)
 *   - RUNNING  -> no-op                 (loop will observe verdict)
 *   - DONE     -> no-op                 (Wk4 safe-after-completion)
 *
 * Thread-aware: if the caller is on the owning loop's thread, do the
 * existing fast path.  Otherwise post to the inbox and ping the loop's
 * I/O backend so it wakes from io_poll.
 */
int
xtc_waker_wake(const xtc_waker_t *w)
{
	if (w == NULL || w->loop == NULL || w->task == NULL)
		return XTC_E_INVAL;

	if (__xtc_current_loop == w->loop) {
		switch (w->task->state) {
		case XTC_TS_PARKED:
			w->task->state = XTC_TS_SCHEDULED;
			return __xtc_loop_enqueue(w->loop, w->task);
		case XTC_TS_SCHEDULED:
		case XTC_TS_RUNNING:
		case XTC_TS_DONE:
		default:
			return XTC_OK;
		}
	}

	/* Cross-thread.  Inbox + wakeup. */
	{
		int rc = __xtc_inbox_push(&w->loop->inbox, XTC_INB_WAKE, w->task);
		if (rc != XTC_OK) return rc;
		return xtc_io_wakeup(w->loop->io);
	}
}

/*
 * PUBLIC: int xtc_task_park_on_timer __P((xtc_task_t *, int64_t));
 */
int
xtc_task_park_on_timer(xtc_task_t *self, int64_t delay_ns)
{
	xtc_timer_t *t;
	int64_t now_ns;
	int rc;

	if (self == NULL || delay_ns < 0)
		return XTC_E_INVAL;
	if (self->park_timer != NULL || self->park_fd >= 0)
		return XTC_E_INVAL;     /* already parked */

	if ((rc = __os_calloc(1, sizeof(*t), (void **)&t)) != XTC_OK)
		return rc;
	if ((rc = __os_clock_mono(&now_ns)) != XTC_OK) {
		__os_free(t);
		return rc;
	}
	t->deadline_ns = now_ns + delay_ns;
	t->cb = NULL;
	t->user = NULL;
	t->waiter = self;
	t->heap_idx = -1;
	t->cancelled = 0;
	t->fired = 0;
	t->loop = self->loop;

	if ((rc = __xtc_timer_heap_push(self->loop, t)) != XTC_OK) {
		__os_free(t);
		return rc;
	}
	/* Splice into all_timers so loop_fini frees it. */
	t->all_next = self->loop->all_timers;
	self->loop->all_timers = t;
	self->park_timer = t;
	return XTC_OK;
}

/*
 * PUBLIC: int xtc_task_park_on_fd __P((xtc_task_t *, int, uint32_t));
 *
 * The tag we hand to xtc_io is the task pointer itself; the loop's
 * dispatcher recognises it and wakes the task.
 */
int
xtc_task_park_on_fd(xtc_task_t *self, int fd, uint32_t interest)
{
	int rc;
	if (self == NULL || fd < 0 || interest == 0)
		return XTC_E_INVAL;
	if (self->park_timer != NULL || self->park_fd >= 0)
		return XTC_E_INVAL;
	if ((rc = xtc_io_reg_fd(self->loop->io, fd, interest, self)) != XTC_OK)
		return rc;
	self->park_fd = fd;
	return XTC_OK;
}

/*
 * Called by loop.c when an io event arrives.  The tag is either NULL
 * (XTC_IO_WAKEUP -- ignored in M3) or a task pointer registered by
 * xtc_task_park_on_fd.
 */
int
__xtc_loop_dispatch_event(xtc_loop_t *loop, xtc_io_event_t *ev)
{
	xtc_task_t *t;
	xtc_waker_t w;

	if (ev->flags & XTC_IO_WAKEUP)
		return XTC_OK;     /* M3: nothing parked on the wakeup */
	if (ev->tag == NULL)
		return XTC_OK;

	t = (xtc_task_t *)ev->tag;
	/* Record the io flags so the parker knows what fired. */
	t->wake_revents |= ev->flags;
	/* Drop our fd registration before waking; the task may register
	 * a fresh one when it runs. */
	if (t->park_fd >= 0) {
		(void)xtc_io_del_fd(loop->io, t->park_fd);
		t->park_fd = -1;
	}
	w.loop = loop;
	w.task = t;
	return xtc_waker_wake(&w);
}
