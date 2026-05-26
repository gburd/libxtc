/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the PostgreSQL License.
 *
 * src/inc/loop_int.h
 *	Internal definitions for the L2 event loop.  Not part of the
 *	public ABI.
 */

#ifndef XTC_LOOP_INT_H
#define XTC_LOOP_INT_H

#include <stdatomic.h>
#include <stdint.h>

#include "xtc_loop.h"
#include "xtc_io.h"
#include "xtc_res.h"
#include "deque.h"
#include "os_thread.h"

/*
 * Task state machine.  Transitions:
 *	SCHEDULED -> RUNNING            loop pops from queue
 *	RUNNING   -> SCHEDULED          fn returned RESCHED
 *	RUNNING   -> PARKED             fn returned PENDING
 *	RUNNING   -> DONE               fn returned DONE
 *	PARKED    -> SCHEDULED          waker fired (or timer / fd ready)
 *	PARKED    -> DONE               (not reachable; PENDING tasks
 *	                                 are reaped only after they
 *	                                 next return DONE)
 */
enum xtc_task_state {
	XTC_TS_SCHEDULED = 0,
	XTC_TS_RUNNING   = 1,
	XTC_TS_PARKED    = 2,
	XTC_TS_DONE      = 3
};

struct xtc_task {
	xtc_task_fn  fn;
	void        *user;
	xtc_loop_t  *loop;
	int          state;
	/* Run-queue intrusive next pointer. */
	struct xtc_task *q_next;

	/* Park bookkeeping.  At most one of these is active at a time
	 * while the task is in PARKED state. */
	xtc_timer_t *park_timer;
	int          park_fd;       /* -1 when not parked on fd */

	/* Linked into loop->all_tasks for cleanup at loop_fini. */
	struct xtc_task *all_next;
};

/*
 * Timer record.  Kept in a binary min-heap inside the loop.
 *
 * Cancel is lazy: we mark cancelled and skip on pop.  Cancel is O(1)
 * by cost; the heap may carry up to N stale entries until the next
 * extraction reaches them.  For M3 this is good enough; M5 may
 * upgrade to a hierarchical wheel.
 */
struct xtc_timer {
	int64_t      deadline_ns;
	xtc_timer_fn cb;
	void        *user;
	xtc_task_t  *waiter;        /* task to wake when fired (NULL if pure cb) */
	int          heap_idx;      /* current position in heap (-1 if not in) */
	int          cancelled;
	int          fired;
	xtc_loop_t  *loop;          /* back-pointer for cancel-by-handle */
	struct xtc_timer *all_next; /* per-loop linked list for cleanup */
};

/*
 * Inbox message kinds.  All inbox traffic is cross-thread; the loop
 * owner drains its inbox at the top of every step.
 */
enum xtc_inbox_kind {
	XTC_INB_WAKE = 0,        /* re-queue a parked task */
	XTC_INB_PUBLISH = 1,     /* publish a freshly-allocated task */
};

struct xtc_inbox_msg {
	enum xtc_inbox_kind  kind;
	xtc_task_t          *task;
	struct xtc_inbox_msg *next;
};

struct xtc_inbox {
	__os_mutex_t          lock;
	struct xtc_inbox_msg *head;
	struct xtc_inbox_msg *tail;
	int                   inited;
};

struct xtc_loop {
	xtc_io_t *io;

	/* Local run queue (Chase-Lev deque, owner pushes/pops). */
	xtc_deque_t deque;

	/* Slow-path overflow when the deque is full.  Owner-only. */
	struct xtc_task *q_head;
	struct xtc_task *q_tail;

	/* Timer min-heap. */
	xtc_timer_t **timers;
	int           n_timers;
	int           cap_timers;

	/* All tasks ever spawned, for cleanup.  Owner-only after init. */
	struct xtc_task *all_tasks;

	/* All timers ever created, for cleanup at fini. */
	xtc_timer_t *all_timers;

	/* M11.5b: per-loop slab cache for xtc_timer_t.  Created lazily
	 * by xtc_timer_set; freed in loop_fini.  Per-loop = single-
	 * threaded ownership = magazine fast path is lock-free. */
	struct xtc_slab *timer_slab;

	/* Live-task counter.  Atomic so cross-thread spawns/completions
	 * can update it without lock. */
	_Atomic int n_alive;

	int stop_requested;

	/* Cross-thread inbox: wakers and remote spawns deposit here;
	 * the owner drains in __xtc_loop_drain_inbox. */
	struct xtc_inbox inbox;

	/* For the multi-loop executor: 0-based index in xtc_exec; -1 if
	 * this loop is standalone (M3 single-thread mode). */
	int exec_id;

	/* Back-pointer to the executor (NULL if standalone). */
	struct xtc_exec *exec;

	/*
	 * Resource accountant.  Either owned by the loop (allocated and
	 * freed at init/fini) or borrowed from the executor.  Tracks
	 * tasks-alive, inbox messages, channels, etc.
	 */
	xtc_res_t *res;
	int        owns_res;
};

/* Internal helpers shared between loop.c, task.c, timer.c. */
int  __xtc_loop_enqueue(xtc_loop_t *loop, xtc_task_t *t);
int  __xtc_timer_heap_push(xtc_loop_t *loop, xtc_timer_t *t);
xtc_timer_t *__xtc_timer_heap_pop_due(xtc_loop_t *loop, int64_t now_ns);
int64_t      __xtc_timer_heap_next_deadline(xtc_loop_t *loop);
int  __xtc_loop_dispatch_event(xtc_loop_t *loop, xtc_io_event_t *ev);

/* Implemented in proc.c.  Called from loop_fini to release the
 * proc-table side struct hashed against this loop pointer.  Must be
 * idempotent. */
void __xtc_proc_loop_unregister(xtc_loop_t *loop);

/* Inbox API.  Producer-side functions are thread-safe. */
int  __xtc_inbox_init(struct xtc_inbox *ib);
void __xtc_inbox_fini(struct xtc_inbox *ib);
int  __xtc_inbox_push(struct xtc_inbox *ib, enum xtc_inbox_kind k, xtc_task_t *t);
int  __xtc_inbox_drain(xtc_loop_t *loop);  /* owner-only; drains into local queue */

/* Per-thread cursor: which loop the calling thread is running.
 * NULL on threads that aren't loop owners. */
extern __thread xtc_loop_t *__xtc_current_loop;

/* Forward declaration for back-pointer in xtc_loop. */
struct xtc_exec;

#endif /* XTC_LOOP_INT_H */
