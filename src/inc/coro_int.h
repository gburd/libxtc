/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/coro_int.h
 *	Internal types for the L2 coroutine substrate.
 */

#ifndef XTC_CORO_INT_H
#define XTC_CORO_INT_H

#if defined(_WIN32)
# include <windows.h>
#else
# include <ucontext.h>
#endif

#include "xtc_async.h"
#include "loop_int.h"

/*
 * The fiber context attached to an xtc_task_t when it was spawned
 * via xtc_async.  Lives in the per-task arena (currently malloc).
 */
struct xtc_coro {
#if defined(_WIN32)
	LPVOID       fiber;         /* the coroutine's own Win32 fiber */
	LPVOID       loop_fiber;    /* return-to-loop fiber pointer */
#else
	ucontext_t   ctx;          /* the coroutine's own machine state */
	ucontext_t   loop_ctx;     /* return-to-loop context (set on resume) */
#endif
	void        *stack;
	size_t       stack_sz;
	xtc_coro_fn  fn;
	void        *arg;
	intptr_t     result;
	int          done;          /* 1 once fn has returned */

	xtc_task_t  *self;          /* back-pointer to our task */
	xtc_task_t  *waiter;        /* task awaiting this one (or NULL) */

	/*
	 * When non-NULL, this coroutine has just registered itself as
	 * the `waiter` of another and is yielding into the loop with
	 * the intent of staying parked rather than rescheduling.  The
	 * step function reads and clears this flag to decide between
	 * RESCHED and PENDING.
	 */
	struct xtc_coro *_parked_on;
};

/* Shared by loop.c — the currently-running coroutine on this loop.  */
extern __thread struct xtc_coro *__xtc_current_coro;

/* Forward declarations for the dispatch glue.  */
int  __xtc_coro_step(xtc_task_t *self, void *user);

#endif /* XTC_CORO_INT_H */
