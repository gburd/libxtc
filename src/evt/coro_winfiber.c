/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/evt/coro_winfiber.c
 *	Windows fiber substrate.  Mirror of src/evt/coro_uctx.c using
 *	Win32 fibers (CreateFiberEx/SwitchToFiber/ConvertThreadToFiber)
 *	in place of ucontext.
 *
 *	The fiber model on Windows is fundamentally similar to ucontext:
 *	a fiber is a stack + saved register state owned by an OS thread.
 *	The thread can switch fibers cooperatively.  CreateFiberEx
 *	allocates the stack with proper guard pages so we don't need
 *	mmap+mprotect.
 */

#include "xtc_int.h"

#if defined(_WIN32)

#include "xtc_async.h"
#include "loop_int.h"
#include "coro_int.h"

#include <windows.h>
#include <string.h>

/* Per-thread cursor -- the coroutine currently executing on this thread. */
XTC_THREAD_LOCAL struct xtc_coro *__xtc_current_coro = NULL;

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
	if (bytes < 16 * 1024) return XTC_E_INVAL;
	__xtc_stack_size = bytes;
	return XTC_OK;
}

/* Fiber entry: invoked the first time we SwitchToFiber into the
 * coroutine.  Runs the user function, marks done, and switches back
 * to the loop fiber.  Must NEVER return -- Win32 will terminate the
 * thread. */
static VOID CALLBACK
__coro_entry(LPVOID arg)
{
	struct xtc_coro *c = arg;
	c->result = (intptr_t)c->fn(c->arg);
	c->done = 1;
	(void)SwitchToFiber(c->loop_fiber);
	for (;;) (void)Sleep(0);          /* unreachable */
}

static void
__coro_destroy(struct xtc_coro *c)
{
	if (c == NULL) return;
	if (c->fiber != NULL) DeleteFiber(c->fiber);
	__os_free(c);
}

/* Task cleanup trampoline: xtc_loop_fini calls this for each coro
 * task so the fiber + coro struct are released with the loop. */
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

	if (loop == NULL || fn == NULL) return XTC_E_INVAL;
	if ((rc = __os_calloc(1, sizeof *c, (void **)&c)) != XTC_OK) return rc;
	c->stack_sz = __xtc_stack_size;
	c->fn = fn;
	c->arg = arg;
	c->fiber = CreateFiberEx(c->stack_sz, c->stack_sz, 0,
	    __coro_entry, c);
	if (c->fiber == NULL) {
		__os_free(c);
		return XTC_E_INTERNAL;
	}
	if ((rc = xtc_task_spawn(loop, __xtc_coro_step, c, &t)) != XTC_OK) {
		__coro_destroy(c);
		return rc;
	}
	c->self = t;
	/* Release the fiber + coro struct when the loop tears the task
	 * down at fini. */
	t->cleanup = __coro_task_cleanup;
	t->cleanup_arg = c;
	if (out_task) *out_task = t;
	return XTC_OK;
}

/* Step function: called by the loop runtime to give the coroutine
 * CPU time.  Mirrors the ucontext path's behaviour. */
int
__xtc_coro_step(xtc_task_t *self, void *user)
{
	struct xtc_coro *c = user;
	struct xtc_coro *saved;
	(void)self;

	/* Convert this thread to a fiber on first entry; subsequent
	 * calls return the existing fiber pointer. */
	{
		LPVOID lf = GetCurrentFiber();
		/* Win32 returns a sentinel (0x1e00) when the thread is
		 * not yet a fiber.  Convert it. */
		if (lf == NULL || (uintptr_t)lf == 0x1e00ul)
			lf = ConvertThreadToFiber(NULL);
		c->loop_fiber = lf;
	}

	saved = __xtc_current_coro;
	__xtc_current_coro = c;
	(void)SwitchToFiber(c->fiber);
	__xtc_current_coro = saved;

	if (c->done) {
		if (c->waiter != NULL) {
			xtc_waker_t w = { c->waiter->loop, c->waiter };
			(void)xtc_waker_wake(&w);
			c->waiter = NULL;
		}
		__coro_destroy(c);
		return XTC_TASK_DONE;
	}
	if (c->_parked_on != NULL) {
		c->_parked_on = NULL;
		return XTC_TASK_PENDING;
	}
	/* Parked on a timer or fd via xtc_task_park_on_*?  Stay parked.
	 * See coro_uctx.c for the rationale. */
	if (c->self != NULL &&
	    (c->self->park_timer != NULL || c->self->park_fd >= 0)) {
		return XTC_TASK_PENDING;
	}
	return XTC_TASK_RESCHED;
}

/*
 * PUBLIC: int xtc_await __P((xtc_task_t *, intptr_t *));
 */
int
xtc_await(xtc_task_t *t, intptr_t *result)
{
	struct xtc_coro *me, *target;

	if (t == NULL) return XTC_E_INVAL;
	target = (struct xtc_coro *)t->user;

	if (target->done) {
		if (result) *result = target->result;
		return XTC_OK;
	}

	me = __xtc_current_coro;
	if (me == NULL) {
		while (!target->done) {
			int rc = xtc_loop_run(t->loop);
			if (rc != XTC_OK) return rc;
		}
		if (result) *result = target->result;
		return XTC_OK;
	}

	target->waiter = me->self;
	me->_parked_on = target;
	(void)SwitchToFiber(me->loop_fiber);
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
	(void)SwitchToFiber(c->loop_fiber);
}

#endif /* _WIN32 */

typedef int __xtc_coro_winfiber_unused;
