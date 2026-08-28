/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_loop.h
 *	The L2 single-thread event loop, run-queue, timer-heap, and
 *	task state machine.  See M3_CLAIMS.md.
 */

#ifndef XTC_LOOP_H
#define XTC_LOOP_H

#include "xtc_export.h"

#include <stdint.h>

#include "xtc_io.h"

typedef struct xtc_loop  xtc_loop_t;
typedef struct xtc_task  xtc_task_t;
typedef struct xtc_timer xtc_timer_t;

/*
 * A task's main function.  Called once per "step" by the loop.
 * Returns one of the XTC_TASK_* codes below to tell the loop what to
 * do next.
 *	XTC_TASK_DONE      The task is finished; the loop reaps it.
 *	XTC_TASK_RESCHED   Re-queue me at the back of the run queue.
 *	XTC_TASK_PENDING   I am waiting on a waker; do not run me again
 *	                   until xtc_waker_wake (or a timer / fd that
 *	                   I parked on) fires.
 */
typedef int (*xtc_task_fn)(xtc_task_t *self, void *user);

#define XTC_TASK_DONE     0
#define XTC_TASK_RESCHED  1
#define XTC_TASK_PENDING  2

/*
 * The waker is a value-type cookie (loop, task) used to re-schedule
 * a parked task.  In M3 wakers are valid only on the owning thread;
 * cross-thread wake arrives in M5.
 */
typedef struct xtc_waker {
	xtc_loop_t *loop;
	xtc_task_t *task;
} xtc_waker_t;

/* Timer callback. */
typedef void (*xtc_timer_fn)(void *user);

/*
 * PUBLIC: int  xtc_loop_init __P((xtc_loop_t **));
 * PUBLIC: int  xtc_loop_fini __P((xtc_loop_t *));
 * PUBLIC: int  xtc_loop_run __P((xtc_loop_t *));
 * PUBLIC: int  xtc_loop_stop __P((xtc_loop_t *));
 * PUBLIC: int  xtc_loop_wake __P((xtc_loop_t *));
 *
 * PUBLIC: int  xtc_task_spawn __P((xtc_loop_t *, xtc_task_fn, void *, xtc_task_t **));
 * PUBLIC: int  xtc_task_waker __P((xtc_task_t *, xtc_waker_t *));
 * PUBLIC: int  xtc_task_park_on_timer __P((xtc_task_t *, int64_t));
 * PUBLIC: int  xtc_task_park_on_fd __P((xtc_task_t *, int, uint32_t));
 *
 * PUBLIC: int  xtc_waker_wake __P((const xtc_waker_t *));
 *
 * PUBLIC: int  xtc_timer_set __P((xtc_loop_t *, int64_t, xtc_timer_fn, void *, xtc_timer_t **));
 * PUBLIC: int  xtc_timer_cancel __P((xtc_timer_t *));
 */
XTC_API int  xtc_loop_init(xtc_loop_t **out);
XTC_API int  xtc_loop_fini(xtc_loop_t *loop);
XTC_API int  xtc_loop_run(xtc_loop_t *loop);
XTC_API int  xtc_loop_stop(xtc_loop_t *loop);

/*
 * xtc_loop_wake --
 *	Nudge a loop's poller out of its I/O wait from ANY OS thread, so
 *	it re-polls its registered fds and re-checks runnability.  This is
 *	the loop-level companion to xtc_proc_wake(): use it when a foreign
 *	thread has made a condition true for a task parked on THIS loop
 *	(e.g. written a self-pipe / eventfd the loop's poll watches, or set
 *	an embedder latch a task re-checks on resume) and the consumer
 *	holds a loop handle rather than a pid -- as an xtc_exec carrier
 *	scheduler does when it marks a session runnable on a sibling loop.
 *
 *	CONTRACT (important): relying on raw fd readiness alone to wake a
 *	loop is only safe for a condition libxtc itself produces on that
 *	loop's thread.  When the readiness is produced by a DIFFERENT
 *	thread, that write races the target loop's park/re-arm window and
 *	can be missed -- the loop sleeps in xtc_io_poll while the fd is
 *	ready.  The producer MUST pair the readiness with an explicit
 *	nudge: xtc_loop_wake(target_loop) (or xtc_proc_wake(pid) for a
 *	parked proc).  The nudge is lost-wake-free against the pre-sleep
 *	window -- it writes the loop's wakeup fd, which the backends keep
 *	armed across the drain (io_uring re-arm-before-drain; epoll/kqueue
 *	level-triggered) -- so a wake issued at any time surfaces on the
 *	next poll.  It delivers no event; the woken loop just re-polls, so
 *	a spurious wake is always safe.
 *
 *	Returns XTC_OK (including when the loop is already awake/running),
 *	XTC_E_INVAL for a NULL loop.  Safe to call from any thread.
 */
XTC_API int  xtc_loop_wake(xtc_loop_t *loop);

/*
 * Borrow the loop's resource accountant.  Non-NULL after init.
 * The pointer is owned by the loop and must not be freed by the
 * caller; it is reset on xtc_loop_fini.
 *
 * PUBLIC: struct xtc_res *xtc_loop_res __P((xtc_loop_t *));
 */
struct xtc_res;
XTC_API struct xtc_res *xtc_loop_res(xtc_loop_t *loop);

XTC_API int  xtc_task_spawn(xtc_loop_t *loop, xtc_task_fn fn, void *user,
                            xtc_task_t **out_task);
XTC_API int  xtc_task_waker(xtc_task_t *task, xtc_waker_t *out);

/*
 * Park the calling task on a timer.  The task's fn returned PENDING
 * (or is about to); after delay_ns elapses, the loop will re-run fn.
 */
XTC_API int  xtc_task_park_on_timer(xtc_task_t *self, int64_t delay_ns);

/*
 * Park the calling task on fd readiness.  When the fd becomes ready
 * for any of the requested interest bits, the loop unregisters the
 * fd and re-runs the task fn.
 *
 * A task may have at most one outstanding park (timer or fd) at any
 * moment; a second park while the first is in flight returns
 * XTC_E_INVAL.
 */
XTC_API int  xtc_task_park_on_fd(xtc_task_t *self, int fd, uint32_t interest);

XTC_API int  xtc_waker_wake(const xtc_waker_t *w);

XTC_API int  xtc_timer_set(xtc_loop_t *loop, int64_t delay_ns,
                           xtc_timer_fn fn, void *user, xtc_timer_t **out_timer);
XTC_API int  xtc_timer_cancel(xtc_timer_t *timer);

#endif /* XTC_LOOP_H */
