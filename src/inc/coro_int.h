/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/coro_int.h
 *	Internal types for the L2 coroutine substrate.
 */

#ifndef XTC_CORO_INT_H
#define XTC_CORO_INT_H

#if defined(_WIN32)
# include <windows.h>
#elif defined(XTC_HAVE_UCONTEXT) && !defined(XTC_CORO_FORCE_FCTX) && \
    !(defined(__APPLE__) && defined(__aarch64__) && \
      !defined(XTC_CORO_FORCE_UCONTEXT) && !defined(XTC_AMALGAMATION))
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
#elif defined(XTC_HAVE_UCONTEXT) && !defined(XTC_CORO_FORCE_FCTX) && \
    !(defined(__APPLE__) && defined(__aarch64__) && \
      !defined(XTC_CORO_FORCE_UCONTEXT) && !defined(XTC_AMALGAMATION))
	ucontext_t   ctx;          /* the coroutine's own machine state */
	ucontext_t   loop_ctx;     /* return-to-loop context (set on resume) */
#else
	/* fcontext substrate (coro_fctx.c): a single saved stack pointer.
	 * On make_fcontext it is the fresh entry point; each yield/await
	 * overwrites it with the coroutine's current resume point.  The
	 * scheduler's return point is a per-thread cursor in coro_fctx.c,
	 * not stored here, because coroutines always return to the
	 * scheduler, never directly to one another. */
	void        *fctx;
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

	/*
	 * Sanitizer fiber-switch save token (ASan/TSan/LSan).  Holds this
	 * coro's "fake stack" across a park so __sanitizer_start/finish_
	 * switch_fiber can track the user-space stack switch and stop
	 * mis-attributing stack memory.  Unused (always NULL) in a
	 * non-sanitized build.  See XTC_FIBER_SWITCH_ANNOTATE in
	 * coro_fctx.c / coro_uctx.c.
	 */
	void        *san_fake_stack;

	/*
	 * TSan fiber-IDENTITY token (clang ThreadSanitizer only).  TSan
	 * does NOT implement __sanitizer_*_switch_fiber; it needs each
	 * coroutine represented as a TSan "fiber object" (__tsan_create_
	 * fiber at create, __tsan_switch_to_fiber at every switch,
	 * __tsan_destroy_fiber at teardown) so it can carry per-fiber
	 * happens-before across cooperative switches instead of seeing
	 * them as one confused thread.  Distinct from san_fake_stack (the
	 * two sanitizer models are mutually exclusive per build).  Always
	 * NULL unless built with clang -fsanitize=thread.  See
	 * XTC_TSAN_FIBERS in coro_fctx.c / coro_uctx.c.
	 */
	void        *tsan_fiber;
};

/* Shared by loop.c -- the currently-running coroutine on this loop.  */
extern XTC_THREAD_LOCAL struct xtc_coro *__xtc_current_coro;

/* Forward declarations for the dispatch glue.  */
int  __xtc_coro_step(xtc_task_t *self, void *user);

/*
 * Internal spawn with an explicit pin flag.  The public xtc_async wraps
 * this with pinned=1 (the long-standing default: a coro's task is not
 * work-stealable).  pinned=0 places the task on the stealable deque so
 * it can migrate across loops -- used by the proc layer when
 * xtc_proc_opts_t.migratable is set.  Every coro backend
 * (coro_fctx/uctx/winfiber) defines it.
 */
int  __xtc_async_ex(xtc_loop_t *loop, xtc_coro_fn fn, void *arg, int pinned,
                    xtc_task_t **out_task);

/* The task wrapping the currently-running coroutine on this thread,
 * or NULL when not running inside a coroutine.  Lets lower-level
 * primitives (e.g. xtc_amutex) find the current task to park it. */
xtc_task_t *__xtc_current_task(void);

#endif /* XTC_CORO_INT_H */
