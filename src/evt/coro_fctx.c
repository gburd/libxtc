/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * src/evt/coro_fctx.c
 *	Stackful coroutine substrate via hand-written make_fcontext /
 *	jump_fcontext assembly (src/os/asm/fctx_*.S), the portable
 *	alternative to ucontext.
 *
 *	This translation unit is the active substrate when the platform
 *	lacks ucontext -- notably musl libc, which omits swapcontext /
 *	getcontext / makecontext on purpose -- or when the build forces
 *	it with -DXTC_CORO_FORCE_FCTX (used to exercise this path on a
 *	glibc host that also has ucontext).  Exactly one of coro_uctx.c
 *	and coro_fctx.c provides the substrate symbols; the other
 *	compiles to an empty unit.
 *
 *	Surface and contract are identical to coro_uctx.c: xtc_async,
 *	__xtc_coro_step, xtc_await, xtc_yield, xtc_stack_size /
 *	xtc_set_stack_size, plus the busy-loop-avoidance verdict logic
 *	(return PENDING when parked on an awaitee, a timer, or an fd).
 *
 *	The fcontext model (see the asm headers):
 *	  void *make_fcontext(stack_top, size, fn) -> a saved sp; the
 *	    first jump into it calls fn(transfer) on the new stack.
 *	  void *jump_fcontext(&from, to, transfer) -> saves the current
 *	    sp into *from, switches to `to`, and returns the transfer
 *	    value supplied by whoever next jumps back.
 *
 *	A coroutine's resume point lives in c->fctx.  The scheduler's
 *	return point is a per-thread cursor (g_sched_fctx): coroutines
 *	always jump back to the scheduler, never directly to each other,
 *	so one cursor per worker thread suffices.
 */

#include "xtc_int.h"

#if !defined(_WIN32) && \
    (!defined(XTC_HAVE_UCONTEXT) || defined(XTC_CORO_FORCE_FCTX))

#include "xtc_async.h"
#include "loop_int.h"
#include "coro_int.h"

#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
# define MAP_ANONYMOUS MAP_ANON
#endif

/* The fcontext primitives (src/os/asm/fctx_x86_64_sysv.S). */
extern void *__xtc_make_fcontext(void *stack_top, size_t size,
                                 void (*fn)(void *transfer));
extern void *__xtc_jump_fcontext(void **from, void *to, void *transfer);

/* Per-thread cursor: the coroutine currently executing on this thread. */
XTC_THREAD_LOCAL struct xtc_coro *__xtc_current_coro = NULL;

/* Per-thread scheduler resume point.  Set by __xtc_coro_step's jump
 * into a coroutine; the coroutine jumps back here to yield/await/finish. */
static XTC_THREAD_LOCAL void *g_sched_fctx = NULL;

/* Default stack size; configurable via xtc_set_stack_size(). */
static size_t __xtc_stack_size = 64 * 1024;

size_t
xtc_stack_size(void)
{
	return __xtc_stack_size;
}

int
xtc_set_stack_size(size_t bytes)
{
	long pg;
	if (bytes < 16 * 1024)
		return XTC_E_INVAL;
	pg = sysconf(_SC_PAGESIZE);
	if (pg <= 0) pg = 4096;
	if (bytes % (size_t)pg != 0)
		bytes += (size_t)pg - (bytes % (size_t)pg);
	__xtc_stack_size = bytes;
	return XTC_OK;
}

/*
 * Fiber entry trampoline.  Reached by the first jump_fcontext into a
 * freshly make_fcontext'd coroutine, with `transfer` carrying the
 * coro pointer the scheduler passed.  Runs the user fn to completion,
 * marks done, then jumps back to the scheduler and never returns.
 */
static void
__coro_entry(void *transfer)
{
	struct xtc_coro *c = (struct xtc_coro *)transfer;
	__xtc_current_coro = c;
	c->result = c->fn(c->arg);
	c->done = 1;
	/* Final jump back to the scheduler.  We will not be resumed, so
	 * the saved-sp slot is irrelevant; reuse c->fctx. */
	(void)__xtc_jump_fcontext(&c->fctx, g_sched_fctx, NULL);
}

/*
 * The task fn the L2 scheduler calls.  Jumps into the fiber; on return
 * the fiber has yielded, awaited, or finished, and c->fctx holds its
 * new resume point (saved by the fiber's own jump back).
 */
int
__xtc_coro_step(xtc_task_t *self, void *user)
{
	struct xtc_coro *c = user;
	struct xtc_coro *saved;

	(void)self;

	saved = __xtc_current_coro;
	__xtc_current_coro = c;

	/* Save our (scheduler) sp into g_sched_fctx and jump into the
	 * coroutine.  `c` is the transfer arg; on the coroutine's first
	 * run it lands in __coro_entry(transfer == c). */
	(void)__xtc_jump_fcontext(&g_sched_fctx, c->fctx, c);

	__xtc_current_coro = saved;

	if (c->done) {
		if (c->waiter != NULL) {
			xtc_waker_t w = { c->waiter->loop, c->waiter };
			(void)xtc_waker_wake(&w);
			c->waiter = NULL;
		}
		return XTC_TASK_DONE;
	}
	/* Awaiting another coroutine: stay parked; the awaitee fires our
	 * waker when it completes. */
	if (c->_parked_on != NULL) {
		c->_parked_on = NULL;
		return XTC_TASK_PENDING;
	}
	/* Parked on a timer or fd: stay parked until a waker re-enqueues
	 * us, or xtc_recv-with-timeout would busy-spin (see coro_uctx.c). */
	if (c->self != NULL &&
	    (c->self->park_timer != NULL || c->self->park_fd >= 0)) {
		return XTC_TASK_PENDING;
	}
	/* Plain yield: re-queue at the back of the run queue. */
	return XTC_TASK_RESCHED;
}

static void
__coro_destroy(struct xtc_coro *c)
{
	if (c == NULL) return;
	if (c->stack != NULL) {
		long pg = sysconf(_SC_PAGESIZE);
		size_t total = c->stack_sz + (pg > 0 ? (size_t)pg : 4096);
		(void)munmap(c->stack, total);
	}
	__os_free(c);
}

static void
__coro_task_cleanup(void *coro)
{
	__coro_destroy((struct xtc_coro *)coro);
}

/*
 * PUBLIC: int xtc_async __P((xtc_loop_t *, xtc_coro_fn, void *, xtc_task_t **));
 */
int
xtc_async(xtc_loop_t *loop, xtc_coro_fn fn, void *arg, xtc_task_t **out_task)
{
	struct xtc_coro *c;
	xtc_task_t *t;
	int rc;
	long pg;
	size_t guard, total;
	char *base;

	if (loop == NULL || fn == NULL)
		return XTC_E_INVAL;

	if ((rc = __os_calloc(1, sizeof *c, (void **)&c)) != XTC_OK)
		return rc;

	pg = sysconf(_SC_PAGESIZE);
	if (pg <= 0) pg = 4096;
	guard = (size_t)pg;
	c->stack_sz = __xtc_stack_size;
	total = c->stack_sz + guard;

	base = mmap(NULL, total, PROT_READ | PROT_WRITE,
	    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (base == MAP_FAILED) {
		__os_free(c);
		return XTC_E_NOMEM;
	}
	/* First page is the guard. */
	if (mprotect(base, guard, PROT_NONE) != 0) {
		(void)munmap(base, total);
		__os_free(c);
		return XTC_E_INTERNAL;
	}
	c->stack = base;
	c->fn = fn;
	c->arg = arg;
	c->done = 0;
	c->waiter = NULL;

	/* The usable stack is [base+guard, base+guard+stack_sz); stacks
	 * grow down, so make_fcontext takes the HIGH end as stack_top. */
	c->fctx = __xtc_make_fcontext(base + guard + c->stack_sz,
	    c->stack_sz, __coro_entry);
	if (c->fctx == NULL) {
		(void)munmap(base, total);
		__os_free(c);
		return XTC_E_INTERNAL;
	}

	if ((rc = __xtc_task_spawn_ex(loop, __xtc_coro_step, c, 1, &t)) != XTC_OK) {
		__coro_destroy(c);
		return rc;
	}
	c->self = t;
	t->cleanup = __coro_task_cleanup;
	t->cleanup_arg = c;

	if (out_task) *out_task = t;
	return XTC_OK;
}

/*
 * PUBLIC: int xtc_await __P((xtc_task_t *, intptr_t *));
 */
int
xtc_await(xtc_task_t *t, intptr_t *result)
{
	struct xtc_coro *me;
	struct xtc_coro *target;

	if (t == NULL) return XTC_E_INVAL;

	target = (struct xtc_coro *)t->user;

	if (target->done) {
		if (result) *result = target->result;
		return XTC_OK;
	}

	me = __xtc_current_coro;

	if (me == NULL) {
		/* Awaiting from outside any coroutine: drive the loop. */
		while (!target->done) {
			int rc = xtc_loop_run(t->loop);
			if (rc != XTC_OK) return rc;
		}
		if (result) *result = target->result;
		return XTC_OK;
	}

	/* Inside a coroutine: register as the target's waiter, set the
	 * "stay parked" hint, and jump back to the scheduler. */
	target->waiter = me->self;
	me->self->q_next = NULL;
	me->_parked_on = target;
	(void)__xtc_jump_fcontext(&me->fctx, g_sched_fctx, NULL);
	/* Resumed: target->done is now true. */
	if (result) *result = target->result;
	return XTC_OK;
}

/*
 * PUBLIC: void xtc_yield __P((void));
 */
void
xtc_yield(void)
{
	struct xtc_coro *c = __xtc_current_coro;
	if (c == NULL) return;
	(void)__xtc_jump_fcontext(&c->fctx, g_sched_fctx, NULL);
}

#endif /* substrate active */
