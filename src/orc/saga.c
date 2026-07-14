/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/orc/saga.c
 *	L4 saga: a sequence of (action, compensate) pairs run on the
 *	calling fiber.  See src/inc/xtc_saga.h for the contract.
 *
 *	Internal model: a growable array of steps, run forward in order;
 *	on the first action failure, walk the completed prefix backward
 *	calling each compensate.  A compensation failure is reported loudly
 *	(fprintf stderr, matching the existing "rare unrecoverable event"
 *	convention in src/ptc/slab.c redzone violations and src/evt/sim.c
 *	determinism violations -- NOT routed through the optional
 *	xtc_log.h default logger, which silently no-ops when no logger has
 *	been configured, which is exactly the "silently swallow it" this
 *	condition must never do) but does not abort the process: the
 *	remaining reverse-order compensations still run, so one broken undo
 *	does not also skip cleanup further back in the chain.
 */

#include "xtc_int.h"
#include "xtc_saga.h"

#include <stdio.h>

struct saga_step {
	xtc_saga_fn action;
	xtc_saga_fn compensate;
	void       *ctx;
};

struct xtc_saga {
	struct saga_step *steps;
	int               n_steps;
	int               cap;

	int               started;            /* run() called (at most once) */
	int               n_completed;        /* actions that returned XTC_OK */
	int               failed_step;        /* -1 if none failed */
	int               last_error;         /* the failing step's rc */
	int               compensate_failed;  /* 1 if any compensate() failed */
};

int
xtc_saga_create(xtc_saga_t **out)
{
	struct xtc_saga *s;
	int rc;

	if (out == NULL) return XTC_E_INVAL;
	*out = NULL;
	if ((rc = __os_calloc(1, sizeof *s, (void **)&s)) != XTC_OK)
		return rc;
	s->failed_step = -1;
	*out = s;
	return XTC_OK;
}

void
xtc_saga_destroy(xtc_saga_t *s)
{
	if (s == NULL) return;
	__os_free(s->steps);
	__os_free(s);
}

int
xtc_saga_step(xtc_saga_t *s, xtc_saga_fn action, xtc_saga_fn compensate,
    void *ctx)
{
	int rc;

	if (s == NULL || action == NULL) return XTC_E_INVAL;
	if (s->started) return XTC_E_INVAL;   /* cannot extend a running saga */

	if (s->n_steps == s->cap) {
		int newcap = s->cap == 0 ? 4 : s->cap * 2;
		void *np = NULL;
		if ((rc = __os_realloc(s->steps,
		    (size_t)newcap * sizeof *s->steps, &np)) != XTC_OK)
			return rc;
		s->steps = np;
		s->cap = newcap;
	}
	s->steps[s->n_steps].action     = action;
	s->steps[s->n_steps].compensate = compensate;
	s->steps[s->n_steps].ctx        = ctx;
	s->n_steps++;
	return XTC_OK;
}

int
xtc_saga_run(xtc_saga_t *s)
{
	int i, rc;

	if (s == NULL) return XTC_E_INVAL;
	if (s->started) return XTC_E_INVAL;   /* a saga runs at most once */
	s->started = 1;

	for (i = 0; i < s->n_steps; i++) {
		rc = s->steps[i].action(s->steps[i].ctx);
		if (rc == XTC_OK) {
			s->n_completed++;
			continue;
		}

		/* Forward failure at step i: undo steps [0, i-1] in reverse. */
		s->failed_step = i;
		s->last_error = rc;
		for (i--; i >= 0; i--) {
			int crc;
			if (s->steps[i].compensate == NULL)
				continue;
			crc = s->steps[i].compensate(s->steps[i].ctx);
			if (crc != XTC_OK) {
				/*
				 * Unrecoverable saga: a compensation itself
				 * failed.  Log loudly and keep going -- do
				 * NOT stop the reverse walk, or every step
				 * behind this one leaks its own undo.
				 */
				fprintf(stderr,
				    "xtc_saga: UNRECOVERABLE -- compensation "
				    "for step %d failed (rc=%d) while "
				    "unwinding after step %d failed (rc=%d); "
				    "saga left PARTIALLY compensated, "
				    "remaining steps' compensations still "
				    "run\n",
				    i, crc, s->failed_step, s->last_error);
				s->compensate_failed = 1;
			}
		}
		return s->compensate_failed ? XTC_E_INTERNAL : rc;
	}
	return XTC_OK;
}

int
xtc_saga_n_steps(const xtc_saga_t *s)
{
	return s == NULL ? 0 : s->n_steps;
}

int
xtc_saga_n_completed(const xtc_saga_t *s)
{
	return s == NULL ? 0 : s->n_completed;
}

int
xtc_saga_failed_step(const xtc_saga_t *s)
{
	return s == NULL ? -1 : s->failed_step;
}

int
xtc_saga_last_error(const xtc_saga_t *s)
{
	return s == NULL ? XTC_OK : s->last_error;
}

int
xtc_saga_compensate_failed(const xtc_saga_t *s)
{
	return s == NULL ? 0 : s->compensate_failed;
}
