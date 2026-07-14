/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_saga.h
 *	L3 saga: a sequence of (forward-action, compensating-action)
 *	pairs run in order from an ordinary fiber.  If a forward action
 *	fails, every already-completed step's compensation runs in
 *	REVERSE order, then xtc_saga_run returns the ORIGINAL failing
 *	step's error code.  All steps succeeding returns XTC_OK.
 *
 *	This is the saga pattern (distributed-transaction compensation,
 *	as used for multi-service workflows and, per PLAN.md 19.5, the
 *	intended replication-coordination and extension-flow use case):
 *	there is no two-phase-commit coordinator, no lock held across
 *	steps -- just a forward action and a hand-written undo for each
 *	step, run by ordinary synchronous-looking C called from a fiber.
 *
 *	NOTE on PLAN.md's original sketch: it proposed building this on
 *	a hypothetical "xtc_future" type with an "await()" call.  Neither
 *	exists in this codebase (grep confirms no xtc_future, no await()
 *	besides the real xtc_await in xtc_async.h, which is a coroutine
 *	join primitive, not a generic future).  A saga step is a plain
 *	function call, not a spawned task: xtc_saga_run calls each step
 *	function directly on the CALLING fiber, in order, exactly like
 *	calling a chain of ordinary functions.  A step is free to spawn
 *	an xtc_proc / use xtc_async / call xtc_await internally and block
 *	until that sub-work finishes -- the saga does not need its own
 *	future type to support that, because xtc_await already exists for
 *	it.  Sagas thus fit directly on top of the real primitives (fibers,
 *	xtc_proc, xtc_async/xtc_await) with no new async abstraction.
 *
 *	A compensation that itself fails is a documented UNRECOVERABLE
 *	SAGA condition: the runtime does not swallow it.  xtc_saga_run
 *	logs it loudly (stderr, matching the existing convention for a
 *	rare unrecoverable event -- see the redzone violation in
 *	src/ptc/slab.c and the determinism violation in src/evt/sim.c) and
 *	keeps running the REMAINING compensations (so a coding accident in
 *	one undo does not also skip cleanup of steps further back), then
 *	returns XTC_E_INTERNAL regardless of the forward failure's
 *	original code -- an unrecoverable saga is always distinguishable
 *	from an ordinary forward failure.  xtc_saga_last_error() still
 *	reports the original forward failure for diagnostics, and
 *	xtc_saga_compensate_failed() reports whether a compensation failed
 *	on the last run.
 */

#ifndef XTC_SAGA_H
#define XTC_SAGA_H

#include "xtc_export.h"

#include <stddef.h>

#include "xtc.h"

typedef struct xtc_saga xtc_saga_t;

/*
 * A step's forward action / compensating action.  ctx is whatever
 * the caller passed to xtc_saga_step (typically per-step state:
 * connection handles, request payloads, ids to undo).  Both return
 * the usual XTC_OK / negative XTC_E_* convention.  A compensation is
 * called ONLY for a step whose forward action already returned
 * XTC_OK; it must undo exactly that step's effect.
 */
typedef int (*xtc_saga_fn)(void *ctx);

/*
 * PUBLIC: int  xtc_saga_create __P((xtc_saga_t **));
 * PUBLIC: void xtc_saga_destroy __P((xtc_saga_t *));
 * PUBLIC: int  xtc_saga_step __P((xtc_saga_t *, xtc_saga_fn, xtc_saga_fn, void *));
 * PUBLIC: int  xtc_saga_run __P((xtc_saga_t *));
 * PUBLIC: int  xtc_saga_n_steps __P((const xtc_saga_t *));
 * PUBLIC: int  xtc_saga_n_completed __P((const xtc_saga_t *));
 * PUBLIC: int  xtc_saga_failed_step __P((const xtc_saga_t *));
 * PUBLIC: int  xtc_saga_last_error __P((const xtc_saga_t *));
 * PUBLIC: int  xtc_saga_compensate_failed __P((const xtc_saga_t *));
 */

/* Create an empty saga.  Returns XTC_OK with *out set, or XTC_E_NOMEM /
 * XTC_E_INVAL (out == NULL). */
XTC_API int  xtc_saga_create(xtc_saga_t **out);

/* Destroy a saga.  Does not touch step ctx pointers (caller-owned).
 * Safe on NULL. */
XTC_API void xtc_saga_destroy(xtc_saga_t *s);

/*
 * Append a step.  Steps run in the order added.  compensate may be
 * NULL for a step with no undo (e.g. a pure read) -- xtc_saga_run then
 * skips compensating it, exactly as if it were a no-op.  action is
 * required.  Returns XTC_OK, XTC_E_INVAL (s or action NULL), or
 * XTC_E_NOMEM.  Steps may not be added once xtc_saga_run has started
 * (returns XTC_E_INVAL).
 */
XTC_API int  xtc_saga_step(xtc_saga_t *s, xtc_saga_fn action, xtc_saga_fn compensate,
                           void *ctx);

/*
 * Run the saga on the CALLING fiber: call each step's action in order.
 * On the first action that returns != XTC_OK, run the compensation of
 * every step that already completed successfully, in REVERSE order,
 * skipping any step whose compensate is NULL, then return that
 * action's error code (xtc_saga_last_error() also reports it).
 *
 * If a compensation itself returns != XTC_OK, that is an unrecoverable
 * saga: xtc_saga_run logs it loudly to stderr (it does NOT abort the
 * process or silently swallow the error), continues running the
 * REMAINING compensations in reverse order regardless (so one broken
 * undo does not also skip cleanup further back), and the overall
 * return value becomes XTC_E_INTERNAL -- distinct from any ordinary
 * step error, so a caller can tell "a step failed and every
 * compensation ran cleanly" (the original step's code) apart from "a
 * step failed AND compensation itself is broken" (XTC_E_INTERNAL).
 * xtc_saga_compensate_failed() reports the latter case after the run.
 *
 * All steps succeeding returns XTC_OK.  A saga with zero steps
 * trivially succeeds (XTC_OK).  Returns XTC_E_INVAL if s is NULL or
 * xtc_saga_run was already called on this saga (a saga runs at most
 * once; create a new one to run again).
 */
XTC_API int  xtc_saga_run(xtc_saga_t *s);

/* Number of steps added via xtc_saga_step. */
XTC_API int  xtc_saga_n_steps(const xtc_saga_t *s);

/* Number of steps whose action completed successfully on the last
 * run (== n_steps if the saga fully succeeded).  0 before running. */
XTC_API int  xtc_saga_n_completed(const xtc_saga_t *s);

/* Index (0-based) of the step whose action failed on the last run, or
 * -1 if no step has failed (including: not yet run, or fully
 * succeeded). */
XTC_API int  xtc_saga_failed_step(const xtc_saga_t *s);

/* The failing step's error code from the last run, or XTC_OK if the
 * saga has not been run or fully succeeded. */
XTC_API int  xtc_saga_last_error(const xtc_saga_t *s);

/* 1 if any compensation itself failed during the last run (the
 * unrecoverable-saga condition), 0 otherwise. */
XTC_API int  xtc_saga_compensate_failed(const xtc_saga_t *s);

#endif /* XTC_SAGA_H */
