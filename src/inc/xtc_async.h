/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_async.h
 *	The L2 coroutine surface.  Stackful fibers via ucontext (M4),
 *	protothreads (always available), and the macros that let user
 *	code write sync-looking async logic.
 *
 *	See M4_CLAIMS.md.
 */

#ifndef XTC_ASYNC_H
#define XTC_ASYNC_H

#include <stdint.h>

#include "xtc_loop.h"

/*
 * Coroutine entry function.  The argument is the user's `arg`; the
 * return value is recovered by the awaiter via xtc_await().
 *
 * The signature uses intptr_t so any pointer or fixed-width integer
 * fits without per-call allocation.  Wider returns can be done by
 * setting an out-parameter inside the user struct passed via arg.
 */
typedef intptr_t (*xtc_coro_fn)(void *arg);

/*
 * PUBLIC: int      xtc_async __P((xtc_loop_t *, xtc_coro_fn, void *, xtc_task_t **));
 * PUBLIC: int      xtc_await __P((xtc_task_t *, intptr_t *));
 * PUBLIC: void     xtc_yield __P((void));
 * PUBLIC: void     xtc_yield_set_budget __P((xtc_loop_t *, int64_t));
 * PUBLIC: int      xtc_yield_check __P((void));
 * PUBLIC: int      xtc_yield_if_due __P((void));
 * PUBLIC: uint64_t xtc_yield_due_count __P((const xtc_loop_t *));
 * PUBLIC: size_t   xtc_stack_size __P((void));
 * PUBLIC: int      xtc_set_stack_size __P((size_t));
 */

/*
 * xtc_async --
 *	Spawn fn(arg) as a stackful coroutine.  Returns immediately
 *	with the task handle in *out_task.  The fiber starts in the
 *	scheduled state and runs on the next loop step.
 */
int      xtc_async(xtc_loop_t *loop, xtc_coro_fn fn, void *arg,
                   xtc_task_t **out_task);

/*
 * xtc_await --
 *	Wait for `t` to complete and recover its return value into
 *	*result.  Must be called from inside a coroutine spawned via
 *	xtc_async (or from the main thread before xtc_loop_run).
 *
 *	Returns XTC_OK on success.
 */
int      xtc_await(xtc_task_t *t, intptr_t *result);

/*
 * xtc_yield --
 *	From inside a coroutine, return control to the loop.  The next
 *	loop step resumes at the line after the call.
 */
void     xtc_yield(void);

/*
 * Cooperative yield watchdog.  xtc has no forcible preemption, so a
 * long compute loop must cooperate.  Set a per-loop time budget with
 * xtc_yield_set_budget (ns; 0 disables, the default), then in the
 * compute loop call xtc_yield_if_due() -- it yields when the current
 * run quantum has exceeded the budget.  xtc_yield_check() is the
 * queryable form (1 == over budget) for embedders that want to react
 * differently (e.g. fire an abort token -> xtc_svr_call_abortable).
 * xtc_yield_due_count() reports how many times a task went over
 * budget on the loop (telemetry).  All are no-ops / 0 off a loop.
 */
void     xtc_yield_set_budget(xtc_loop_t *loop, int64_t budget_ns);
int      xtc_yield_check(void);
int      xtc_yield_if_due(void);
uint64_t xtc_yield_due_count(const xtc_loop_t *loop);

/*
 * Default fiber stack size in bytes.  Configurable per process via
 * xtc_set_stack_size().  M4 default: 64 KiB.
 */
size_t   xtc_stack_size(void);
int      xtc_set_stack_size(size_t bytes);

/*
 * XTC_COOP_REGION { ... } --
 *	A block that is guaranteed to run to completion without the
 *	scheduler interleaving another task between its statements.
 *	M4 implementation: the macro is a documentation marker only,
 *	because the M4 scheduler is single-threaded and never
 *	preempts a running coroutine outside an explicit yield point.
 *	M5 (multi-loop) hardens this by setting a per-task "do not
 *	steal" flag for the duration of the block.
 */
#define XTC_COOP_REGION  /* see xtc_async(3) */

/*
 * Protothread macros (constrained-platform fallback).  Bodies of
 * these functions cannot use stack-resident locals; lift them into a
 * state struct or static.  Documented in xtc_async(3).
 */
typedef struct xtc_pt {
	unsigned short lc;     /* local-continuation cookie */
} xtc_pt_t;

#define XTC_PT_THREAD(...)         char __VA_ARGS__
#define XTC_PT_INIT(pt)            ((pt)->lc = 0)
#define XTC_PT_BEGIN(pt)           switch ((pt)->lc) { case 0:
#define XTC_PT_END(pt)             } (pt)->lc = 0; return 2 /* DONE */
#define XTC_PT_YIELD(pt)           do {                                    \
	(pt)->lc = __LINE__; return 0 /* PENDING */;                       \
	case __LINE__:; } while (0)
#define XTC_PT_WAIT_UNTIL(pt, c)   do {                                    \
	(pt)->lc = __LINE__; case __LINE__:                                \
	if (!(c)) return 0; } while (0)

#define XTC_PT_DONE     2
#define XTC_PT_YIELDED  0

#endif /* XTC_ASYNC_H */
