/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * src/evt/coro_uctx.c
 *	Stackful coroutine substrate via POSIX ucontext.h.
 *
 *	A coroutine is wrapped in an xtc_task_t.  The task's fn is the
 *	tiny step function below; on each invocation the loop swaps
 *	into the fiber's saved context.  The fiber returns to the loop
 *	either by completing (fn returned), yielding via xtc_yield(),
 *	or awaiting another coroutine.
 *
 *	M4 ships ucontext as the default substrate.  M4.5 will add
 *	per-architecture make_fcontext/jump_fcontext asm for ~30 ns
 *	switches; the surface and contract here are unchanged.
 */

#include "xtc_int.h"

/* Windows uses a separate fiber implementation in src/evt/coro_winfiber.c.
 * This file is the POSIX (ucontext-based) substrate. */
#if defined(_WIN32)
typedef int __xtc_coro_uctx_unused;
#else

#include "xtc_async.h"
#include "loop_int.h"
#include "coro_int.h"

#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

/* Some platforms (FreeBSD, OpenBSD) only define MAP_ANON; Linux has
 * both spellings.  xtc_int.h already enables the BSD extension
 * namespace, so MAP_ANON should be visible by here. */
#if !defined(MAP_ANONYMOUS) && defined(MAP_ANON)
# define MAP_ANONYMOUS MAP_ANON
#endif

/* Per-thread cursor — the coroutine currently executing on this thread. */
__thread struct xtc_coro *__xtc_current_coro = NULL;

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
	/* Round up to page multiple. */
	if (bytes % (size_t)pg != 0)
		bytes += (size_t)pg - (bytes % (size_t)pg);
	__xtc_stack_size = bytes;
	return XTC_OK;
}

/*
 * The trampoline that runs as the fiber's entry point.  We pull the
 * coro pointer back from the per-thread cursor (set just before
 * makecontext_swap).
 */
static void
__coro_entry(void)
{
	struct xtc_coro *c = __xtc_current_coro;
	c->result = c->fn(c->arg);
	c->done = 1;
	/* Return to the loop's context. */
	(void)swapcontext(&c->ctx, &c->loop_ctx);
}

/*
 * The task fn that the L2 scheduler calls.  Swaps into the fiber.
 * On return, decides what to tell the loop based on the fiber's
 * state.
 */
int
__xtc_coro_step(xtc_task_t *self, void *user)
{
	struct xtc_coro *c = user;
	struct xtc_coro *saved;

	(void)self;

	/* Set the per-thread cursor so xtc_yield() can find us. */
	saved = __xtc_current_coro;
	__xtc_current_coro = c;

	(void)swapcontext(&c->loop_ctx, &c->ctx);

	__xtc_current_coro = saved;

	if (c->done) {
		if (c->waiter != NULL) {
			xtc_waker_t w = { c->waiter->loop, c->waiter };
			(void)xtc_waker_wake(&w);
			c->waiter = NULL;
		}
		return XTC_TASK_DONE;
	}
	/* Awaiting another coroutine?  Stay parked; the awaitee will
	 * fire our waker when it completes. */
	if (c->_parked_on != NULL) {
		c->_parked_on = NULL;
		return XTC_TASK_PENDING;
	}
	/* Plain yield; re-queue at the back of the run queue. */
	return XTC_TASK_RESCHED;
}

/*
 * Free the fiber's stack on task completion.  Hooked from the
 * loop's task-cleanup pass at fini time.  For now we register a
 * destructor on the task user pointer; M4 just frees in xtc_loop_fini
 * via the all_tasks walk.
 */
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
	/* The first page is the guard.  Make it inaccessible. */
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

	if (getcontext(&c->ctx) != 0) {
		__coro_destroy(c);
		return XTC_E_INTERNAL;
	}
	c->ctx.uc_stack.ss_sp = base + guard;
	c->ctx.uc_stack.ss_size = c->stack_sz;
	c->ctx.uc_link = NULL;       /* end-of-coroutine returns via swap */

	/* The trampoline reads __xtc_current_coro on entry; that cursor is
	 * set by __xtc_coro_step when the loop first runs us, so we don't
	 * need to touch it here.  Critically, we must NOT clobber the
	 * caller's cursor when xtc_async is invoked from inside another
	 * coroutine. */
	makecontext(&c->ctx, __coro_entry, 0);

	if ((rc = xtc_task_spawn(loop, __xtc_coro_step, c, &t)) != XTC_OK) {
		__coro_destroy(c);
		return rc;
	}
	c->self = t;

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

	/* Already done?  Take the fast path. */
	if (target->done) {
		if (result) *result = target->result;
		return XTC_OK;
	}

	me = __xtc_current_coro;

	if (me == NULL) {
		/* Awaiting from outside any coroutine.  Drive the loop
		 * until the target completes.  Useful for tests + main(). */
		while (!target->done) {
			int rc = xtc_loop_run(t->loop);
			if (rc != XTC_OK) return rc;
		}
		if (result) *result = target->result;
		return XTC_OK;
	}

	/* Inside a coroutine.  Register me as the target's waiter, yield,
	 * and resume after the target wakes me.  We park by setting our
	 * task's state to PARKED before yielding; the loop's task verdict
	 * for the next step is already RESCHED by virtue of __xtc_coro_step
	 * returning, so we have to coordinate manually:
	 *
	 *   - target->waiter = me->self
	 *   - swap back to loop with yield-shaped semantics
	 *   - the LAST thing __xtc_coro_step does is RESCHED, which would
	 *     re-run us prematurely.  Solution: leave a flag on the coro
	 *     that the step function checks; when set, return PENDING and
	 *     reset the flag.
	 */
	target->waiter = me->self;
	me->done = 0;            /* unchanged; for clarity */
	me->self->q_next = NULL; /* belt-and-braces; see q-pop semantics */

	/*
	 * Set the per-coro "i'm parked, don't reschedule" hint by
	 * stashing target into a side channel.  We use a simple flag
	 * on the coro: while parked == 1, __xtc_coro_step returns
	 * PENDING instead of RESCHED.
	 */
	me->_parked_on = target;
	(void)swapcontext(&me->ctx, &me->loop_ctx);
	/* When we return here, target->done must be true. */
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
	(void)swapcontext(&c->ctx, &c->loop_ctx);
}

#endif /* !_WIN32 */
