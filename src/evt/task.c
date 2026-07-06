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
#include <string.h>
#include <unistd.h>

/*
 * Per-loop task-struct free-list -- a plain single-threaded LIFO stack
 * of recycled task structs, threaded through the task's own q_next
 * field while it sits on the free list.  A completed PLAIN task on its
 * home loop is pushed here instead of being freed; the next spawn on
 * that loop pops it instead of calling malloc.  malloc is only hit to
 * grow the pool.  This is a deliberate rejection of xtc_slab for this
 * path: xtc_slab is a shm-capable, chunked, magazine + mutex allocator
 * (measured ~25x slower than malloc for the task pattern); a bare
 * per-loop free-list, touched only by the owning loop thread, is
 * lock-free and beats malloc on the recycle-heavy hot path.
 *
 * Ownership: only the HOME loop's thread pushes/pops its free list
 * (the DONE-handler recycle is gated on t->loop == running loop), so
 * no synchronization is needed.  A stolen task completing on a thief
 * is NOT recycled (it is left for loop_fini), keeping this single-
 * threaded.  The free list is drained (structs __os_free'd) at
 * loop_fini.
 */

/* Pop a recycled task struct from the loop's free list, or NULL. */
static xtc_task_t *
__task_freelist_pop(xtc_loop_t *loop)
{
	xtc_task_t *t = loop->task_free;
	if (t != NULL) {
		loop->task_free = t->q_next;
		loop->task_free_n--;
	}
	return t;
}

/* Allocate a zeroed task struct: reuse a recycled one from the loop's
 * free list if available, else malloc.  recyclable marks a free-list-
 * eligible struct (recycle on completion). */
static int
__task_alloc(xtc_loop_t *loop, xtc_task_t **out)
{
	xtc_task_t *t = NULL;
	int rc;
	if (__xtc_current_loop == loop)      /* home thread owns the list */
		t = __task_freelist_pop(loop);
	if (t != NULL) {
		memset(t, 0, sizeof *t);
		t->recyclable = 1;
		*out = t;
		return XTC_OK;
	}
	if ((rc = __os_calloc(1, sizeof(*t), (void **)&t)) != XTC_OK)
		return rc;
	t->recyclable = 1;   /* freelist-eligible on completion */
	*out = t;
	return XTC_OK;
}

/* A cap so a burst of simultaneously-live tasks that all complete does
 * not pin unbounded memory on the free list; beyond it, recycled
 * structs are returned to the OS.  Overridable at build time (set to 0
 * to disable recycling entirely, e.g. for an A/B measurement). */
#ifndef XTC_TASK_FREELIST_MAX
#define XTC_TASK_FREELIST_MAX 4096
#endif

/*
 * PUBLIC: void __xtc_task_free __P((xtc_task_t *));
 *
 * Reclaim a completed task: unlink it O(1) from its home loop's
 * all_tasks list and push it onto the loop's task free list for reuse
 * (or __os_free if the list is full or the struct was a bare alloc).
 * Called from the loop's DONE handler for plain tasks (cleanup ==
 * NULL) completing ON THEIR HOME LOOP -- so the free-list push and the
 * all_tasks unlink are single-threaded (the home loop's thread).
 */
void
__xtc_task_free(xtc_task_t *t)
{
	xtc_loop_t *loop;
	if (t == NULL)
		return;
	loop = t->loop;
	/* Unlink from all_tasks (doubly linked -> O(1)). */
	if (t->all_prev != NULL)
		t->all_prev->all_next = t->all_next;
	else if (loop != NULL && loop->all_tasks == t)
		loop->all_tasks = t->all_next;
	if (t->all_next != NULL)
		t->all_next->all_prev = t->all_prev;
	if (t->recyclable && loop != NULL &&
	    loop->task_free_n < XTC_TASK_FREELIST_MAX) {
		t->q_next = loop->task_free;   /* reuse q_next as free-list link */
		loop->task_free = t;
		loop->task_free_n++;
	} else {
		__os_free(t);
	}
}

/*
 * PUBLIC: int xtc_task_spawn __P((xtc_loop_t *, xtc_task_fn, void *, xtc_task_t **));
 */
int
__xtc_task_spawn_ex(xtc_loop_t *loop, xtc_task_fn fn, void *user,
                    int pinned, xtc_task_t **out_task)
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

	if ((rc = __task_alloc(loop, &t)) != XTC_OK) {
		if (loop->res != NULL)
			xtc_res_release(loop->res, XTC_RES_TASKS, 1);
		return rc;
	}
	t->fn = fn;
	t->user = user;
	t->loop = loop;
	t->pinned = pinned;
	t->state = XTC_TS_SCHEDULED;
	t->q_next = NULL;
	t->park_timer = NULL;
	t->park_fd = -1;
	atomic_store_explicit(&t->wake_revents, 0, memory_order_relaxed);

	if (out_task) *out_task = t;

	if (__xtc_current_loop == loop) {
		t->all_next = loop->all_tasks;
		t->all_prev = NULL;
		if (loop->all_tasks != NULL)
			loop->all_tasks->all_prev = t;
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
			__xtc_task_free(t);   /* not yet on all_tasks; slab-aware */
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
		__xtc_task_free(t);
		if (out_task) *out_task = NULL;
		return rc;
	}
	(void)xtc_io_wakeup(loop->io);
	return XTC_OK;
}

int
xtc_task_spawn(xtc_loop_t *loop, xtc_task_fn fn, void *user,
               xtc_task_t **out_task)
{
	/* Public entry: unpinned (stealable in executor mode). */
	return __xtc_task_spawn_ex(loop, fn, user, 0, out_task);
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

	/* Cross-thread.  Inbox + wakeup.
	 *
	 * Capture loop and task into locals BEFORE the inbox push.  The
	 * waker *w may be embedded in a stack-allocated waiter (e.g. the
	 * struct amutex_waiter / chan / proc waiters) owned by the parked
	 * task.  The instant the task is pushed to the target loop's
	 * inbox, that loop can resume it on another thread and the task
	 * may return -- freeing *w -- before we read it again.
	 * Dereferencing w->loop after the push is a use-after-free,
	 * observed as an EXC_BAD_ACCESS / pointer-authentication fault
	 * inside xtc_io_wakeup under a multi-loop executor.  The loop and
	 * its io are owned by the executor and outlive the task, so the
	 * captured locals stay valid. */
	{
		xtc_loop_t *loop = w->loop;
		xtc_task_t *task = w->task;
		int rc = __xtc_inbox_push(&loop->inbox, XTC_INB_WAKE, task);
		if (rc != XTC_OK) return rc;
		return xtc_io_wakeup(loop->io);
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
	atomic_fetch_or_explicit(&t->wake_revents, ev->flags,
	    memory_order_relaxed);
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
