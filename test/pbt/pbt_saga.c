/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/pbt/pbt_saga.c
 *	Property-based tests for xtc_saga (PLAN.md 19.5).
 *
 *	Property: for ANY random step count N and ANY random failure
 *	point 0..N (N meaning "no failure -- the saga fully succeeds"),
 *	the compensation call sequence is EXACTLY the reverse of the
 *	completed steps, and no step's action or compensate is ever
 *	called more than once.
 */

#include <stdint.h>
#include <string.h>

#include "pbt_common.h"
#include "xtc_saga.h"
#include "xtc.h"

#define MAX_STEPS 32

#if defined(__GNUC__) || defined(__clang__)
# define MAYBE_UNUSED __attribute__((unused))
#else
# define MAYBE_UNUSED
#endif

/* Per-call counts (not just a sequence) so "called more than once" is
 * caught directly, independent of the order check. */
struct step_ctx {
	int idx;
	int fail_here;
	int action_calls;
	int compensate_calls;
	int *seq;      /* shared log across all steps in this saga */
	int *seq_n;
};

MAYBE_UNUSED static int
pbt_saga_action(void *arg)
{
	struct step_ctx *c = arg;
	c->action_calls++;
	c->seq[(*c->seq_n)++] = 0x1000 + c->idx;
	return c->fail_here ? XTC_E_ABORTED : XTC_OK;
}

MAYBE_UNUSED static int
pbt_saga_compensate(void *arg)
{
	struct step_ctx *c = arg;
	c->compensate_calls++;
	c->seq[(*c->seq_n)++] = 0x2000 + c->idx;
	return XTC_OK;
}

#if defined(XTC_HAVE_HEGEL)

/*
 * Draws N in [1, MAX_STEPS] and fail_at in [0, N] (N == "no failure").
 * Builds an N-step saga, runs it, and checks:
 *   - every action called exactly once, every triggered compensate
 *     called exactly once, nothing called twice;
 *   - if fail_at == N: full success, no compensate ran;
 *   - else: n_completed == fail_at, and the log's compensation
 *     sub-sequence is EXACTLY the reverse of [0, fail_at).
 */
static void
prop_saga_reverse_compensation(hegel_test_case *tc, void *u)
{
	int n, fail_at, i, pos;
	struct step_ctx ctx[MAX_STEPS];
	int seq[2 * MAX_STEPS];
	int seq_n = 0;
	xtc_saga_t *s = NULL;
	int rc;
	(void)u;

	n = (int)hegel_draw_int(tc, hegel_integers(1, MAX_STEPS));
	fail_at = (int)hegel_draw_int(tc, hegel_integers(0, n));  /* n == no fail */

	hegel_assume(xtc_saga_create(&s) == XTC_OK);
	for (i = 0; i < n; i++) {
		ctx[i].idx = i;
		ctx[i].fail_here = (i == fail_at);
		ctx[i].action_calls = 0;
		ctx[i].compensate_calls = 0;
		ctx[i].seq = seq;
		ctx[i].seq_n = &seq_n;
		hegel_assume(xtc_saga_step(s, pbt_saga_action,
		    pbt_saga_compensate, &ctx[i]) == XTC_OK);
	}

	rc = xtc_saga_run(s);

	/* No action/compensate ever called more than once. */
	for (i = 0; i < n; i++) {
		hegel_assume(ctx[i].action_calls <= 1);
		hegel_assume(ctx[i].compensate_calls <= 1);
	}

	if (fail_at == n) {
		/* Full success: every action ran once, nothing compensated. */
		hegel_assume(rc == XTC_OK);
		hegel_assume(xtc_saga_n_completed(s) == n);
		for (i = 0; i < n; i++) {
			hegel_assume(ctx[i].action_calls == 1);
			hegel_assume(ctx[i].compensate_calls == 0);
		}
		hegel_assume(seq_n == n);
		for (i = 0; i < n; i++)
			hegel_assume(seq[i] == 0x1000 + i);
	} else {
		/* Failure at fail_at: steps [0, fail_at) completed and
		 * compensate; step fail_at's action ran (and failed) but is
		 * NEVER compensated (only completed steps are undone). */
		hegel_assume(rc != XTC_OK);
		hegel_assume(xtc_saga_n_completed(s) == fail_at);
		hegel_assume(xtc_saga_failed_step(s) == fail_at);
		for (i = 0; i <= fail_at; i++)
			hegel_assume(ctx[i].action_calls == 1);
		for (i = fail_at + 1; i < n; i++)
			hegel_assume(ctx[i].action_calls == 0);
		for (i = 0; i < fail_at; i++)
			hegel_assume(ctx[i].compensate_calls == 1);
		hegel_assume(ctx[fail_at].compensate_calls == 0);

		/* seq is: action(0..fail_at) then EXACTLY the reverse
		 * compensate(fail_at-1 .. 0). */
		hegel_assume(seq_n == (fail_at + 1) + fail_at);
		pos = 0;
		for (i = 0; i <= fail_at; i++)
			hegel_assume(seq[pos++] == 0x1000 + i);
		for (i = fail_at - 1; i >= 0; i--)
			hegel_assume(seq[pos++] == 0x2000 + i);
	}

	xtc_saga_destroy(s);
}

static const pbt_entry_t tests[] = {
	{ "reverse_compensation", prop_saga_reverse_compensation, 200 },
	{ NULL, NULL, 0 }
};

#else
static const pbt_entry_t tests[] = {
	{ "reverse_compensation", NULL, 0 },
	{ NULL, NULL, 0 }
};
#endif

PBT_MAIN("saga", tests)
