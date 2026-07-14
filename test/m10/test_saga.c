/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m10/test_saga.c
 *	Verifies xtc_saga: successful sagas, forward failure at step
 *	1/middle/last with exact reverse-order compensation, NULL
 *	(no-op) compensations, and the unrecoverable-saga case where a
 *	compensation itself fails.
 */

#include <stdint.h>
#include <string.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_saga.h"

#define MAX_CALLS 16

/* Records the order and identity of every action/compensate call, so
 * a test can assert the EXACT sequence (no extra, no missing, no
 * duplicate calls). */
struct call_log {
	int   seq[MAX_CALLS];   /* step index, 'A'ction or 'C'ompensate high bit */
	int   n;
};

#define LOG_ACTION(idx)     (0x1000 + (idx))
#define LOG_COMPENSATE(idx) (0x2000 + (idx))

static void
log_push(struct call_log *log, int tag)
{
	munit_assert_int(log->n, <, MAX_CALLS);
	log->seq[log->n++] = tag;
}

/* A step whose ctx carries: the log, this step's index, and whether
 * its action / compensate should fail. */
struct step_ctx {
	struct call_log *log;
	int               idx;
	int               action_rc;      /* XTC_OK unless told to fail */
	int               compensate_rc;  /* XTC_OK unless told to fail */
};

static int
step_action(void *arg)
{
	struct step_ctx *c = arg;
	log_push(c->log, LOG_ACTION(c->idx));
	return c->action_rc;
}

static int
step_compensate(void *arg)
{
	struct step_ctx *c = arg;
	log_push(c->log, LOG_COMPENSATE(c->idx));
	return c->compensate_rc;
}

/* ---------- full success ---------- */

static MunitResult
test_saga_all_succeed(const MunitParameter p[], void *d)
{
	xtc_saga_t *s = NULL;
	struct call_log log = {0};
	struct step_ctx ctx[3];
	int i;
	(void)p; (void)d;

	for (i = 0; i < 3; i++) {
		ctx[i].log = &log;
		ctx[i].idx = i;
		ctx[i].action_rc = XTC_OK;
		ctx[i].compensate_rc = XTC_OK;
	}

	munit_assert_int(xtc_saga_create(&s), ==, XTC_OK);
	for (i = 0; i < 3; i++)
		munit_assert_int(xtc_saga_step(s, step_action, step_compensate,
		    &ctx[i]), ==, XTC_OK);

	munit_assert_int(xtc_saga_run(s), ==, XTC_OK);
	munit_assert_int(xtc_saga_n_steps(s), ==, 3);
	munit_assert_int(xtc_saga_n_completed(s), ==, 3);
	munit_assert_int(xtc_saga_failed_step(s), ==, -1);
	munit_assert_int(xtc_saga_last_error(s), ==, XTC_OK);
	munit_assert_int(xtc_saga_compensate_failed(s), ==, 0);

	/* No compensation ever ran; actions ran in order 0,1,2. */
	munit_assert_int(log.n, ==, 3);
	munit_assert_int(log.seq[0], ==, LOG_ACTION(0));
	munit_assert_int(log.seq[1], ==, LOG_ACTION(1));
	munit_assert_int(log.seq[2], ==, LOG_ACTION(2));

	xtc_saga_destroy(s);
	return MUNIT_OK;
}

/* ---------- failure at first / middle / last step ---------- */

/* Runs a 4-step saga (indices 0..3) with the action at fail_idx
 * returning XTC_E_INVAL, and asserts the exact call sequence: actions
 * 0..fail_idx (inclusive) run in order, then compensations for
 * 0..fail_idx-1 run in REVERSE order, with no extra/missing/duplicate
 * calls. */
static void
run_failure_at(int fail_idx)
{
	xtc_saga_t *s = NULL;
	struct call_log log = {0};
	struct step_ctx ctx[4];
	int i, rc, expect_n, pos;

	for (i = 0; i < 4; i++) {
		ctx[i].log = &log;
		ctx[i].idx = i;
		ctx[i].action_rc = (i == fail_idx) ? XTC_E_INVAL : XTC_OK;
		ctx[i].compensate_rc = XTC_OK;
	}

	munit_assert_int(xtc_saga_create(&s), ==, XTC_OK);
	for (i = 0; i < 4; i++)
		munit_assert_int(xtc_saga_step(s, step_action, step_compensate,
		    &ctx[i]), ==, XTC_OK);

	rc = xtc_saga_run(s);
	munit_assert_int(rc, ==, XTC_E_INVAL);
	munit_assert_int(xtc_saga_n_completed(s), ==, fail_idx);
	munit_assert_int(xtc_saga_failed_step(s), ==, fail_idx);
	munit_assert_int(xtc_saga_last_error(s), ==, XTC_E_INVAL);
	munit_assert_int(xtc_saga_compensate_failed(s), ==, 0);

	/* Expected sequence: A(0)..A(fail_idx), then C(fail_idx-1)..C(0). */
	expect_n = (fail_idx + 1) + fail_idx;
	munit_assert_int(log.n, ==, expect_n);
	pos = 0;
	for (i = 0; i <= fail_idx; i++)
		munit_assert_int(log.seq[pos++], ==, LOG_ACTION(i));
	for (i = fail_idx - 1; i >= 0; i--)
		munit_assert_int(log.seq[pos++], ==, LOG_COMPENSATE(i));

	xtc_saga_destroy(s);
}

static MunitResult
test_saga_fail_first(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	run_failure_at(0);
	return MUNIT_OK;
}

static MunitResult
test_saga_fail_middle(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	run_failure_at(2);
	return MUNIT_OK;
}

static MunitResult
test_saga_fail_last(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	run_failure_at(3);
	return MUNIT_OK;
}

/* ---------- NULL compensate is skipped, not called ---------- */

static MunitResult
test_saga_null_compensate_skipped(const MunitParameter p[], void *d)
{
	xtc_saga_t *s = NULL;
	struct call_log log = {0};
	struct step_ctx ctx[3];
	int i;
	(void)p; (void)d;

	for (i = 0; i < 3; i++) {
		ctx[i].log = &log;
		ctx[i].idx = i;
		ctx[i].action_rc = (i == 2) ? XTC_E_AGAIN : XTC_OK;
		ctx[i].compensate_rc = XTC_OK;
	}

	munit_assert_int(xtc_saga_create(&s), ==, XTC_OK);
	/* Step 0 has NO compensation (e.g. a pure read). */
	munit_assert_int(xtc_saga_step(s, step_action, NULL, &ctx[0]), ==, XTC_OK);
	munit_assert_int(xtc_saga_step(s, step_action, step_compensate, &ctx[1]),
	    ==, XTC_OK);
	munit_assert_int(xtc_saga_step(s, step_action, step_compensate, &ctx[2]),
	    ==, XTC_OK);

	munit_assert_int(xtc_saga_run(s), ==, XTC_E_AGAIN);
	munit_assert_int(xtc_saga_n_completed(s), ==, 2);

	/* A(0) A(1) A(2:fails) C(1) -- step 0's compensate is never called. */
	munit_assert_int(log.n, ==, 4);
	munit_assert_int(log.seq[0], ==, LOG_ACTION(0));
	munit_assert_int(log.seq[1], ==, LOG_ACTION(1));
	munit_assert_int(log.seq[2], ==, LOG_ACTION(2));
	munit_assert_int(log.seq[3], ==, LOG_COMPENSATE(1));

	xtc_saga_destroy(s);
	return MUNIT_OK;
}

/* ---------- a compensation itself fails: unrecoverable saga ---------- */

static MunitResult
test_saga_compensate_fails(const MunitParameter p[], void *d)
{
	xtc_saga_t *s = NULL;
	struct call_log log = {0};
	struct step_ctx ctx[3];
	int i, rc;
	(void)p; (void)d;

	for (i = 0; i < 3; i++) {
		ctx[i].log = &log;
		ctx[i].idx = i;
		ctx[i].action_rc = (i == 2) ? XTC_E_AGAIN : XTC_OK;
		/* Step 1's compensation itself fails. */
		ctx[i].compensate_rc = (i == 1) ? XTC_E_IO : XTC_OK;
	}

	munit_assert_int(xtc_saga_create(&s), ==, XTC_OK);
	for (i = 0; i < 3; i++)
		munit_assert_int(xtc_saga_step(s, step_action, step_compensate,
		    &ctx[i]), ==, XTC_OK);

	rc = xtc_saga_run(s);
	/* The unrecoverable-saga condition overrides the return code. */
	munit_assert_int(rc, ==, XTC_E_INTERNAL);
	munit_assert_int(xtc_saga_compensate_failed(s), ==, 1);
	/* The ORIGINAL forward failure is still recoverable via last_error. */
	munit_assert_int(xtc_saga_last_error(s), ==, XTC_E_AGAIN);
	munit_assert_int(xtc_saga_failed_step(s), ==, 2);
	munit_assert_int(xtc_saga_n_completed(s), ==, 2);

	/* Documented behavior: the broken compensation does NOT stop the
	 * reverse walk -- step 0's compensation still runs after step 1's
	 * compensation fails. A(0) A(1) A(2:fails) C(1:fails) C(0). */
	munit_assert_int(log.n, ==, 5);
	munit_assert_int(log.seq[0], ==, LOG_ACTION(0));
	munit_assert_int(log.seq[1], ==, LOG_ACTION(1));
	munit_assert_int(log.seq[2], ==, LOG_ACTION(2));
	munit_assert_int(log.seq[3], ==, LOG_COMPENSATE(1));
	munit_assert_int(log.seq[4], ==, LOG_COMPENSATE(0));

	xtc_saga_destroy(s);
	return MUNIT_OK;
}

/* ---------- misuse / edge cases ---------- */

static MunitResult
test_saga_misuse(const MunitParameter p[], void *d)
{
	xtc_saga_t *s = NULL;
	struct step_ctx ctx;
	struct call_log log = {0};
	(void)p; (void)d;

	munit_assert_int(xtc_saga_create(NULL), ==, XTC_E_INVAL);
	munit_assert_int(xtc_saga_create(&s), ==, XTC_OK);

	munit_assert_int(xtc_saga_step(NULL, step_action, NULL, NULL),
	    ==, XTC_E_INVAL);
	munit_assert_int(xtc_saga_step(s, NULL, NULL, NULL), ==, XTC_E_INVAL);

	/* A zero-step saga trivially succeeds. */
	munit_assert_int(xtc_saga_run(s), ==, XTC_OK);
	munit_assert_int(xtc_saga_n_steps(s), ==, 0);

	/* A saga runs at most once. */
	munit_assert_int(xtc_saga_run(s), ==, XTC_E_INVAL);

	/* Cannot append a step after running. */
	ctx.log = &log; ctx.idx = 0; ctx.action_rc = XTC_OK;
	ctx.compensate_rc = XTC_OK;
	munit_assert_int(xtc_saga_step(s, step_action, step_compensate, &ctx),
	    ==, XTC_E_INVAL);

	/* NULL-saga introspection returns defaults, not a crash. */
	munit_assert_int(xtc_saga_n_steps(NULL), ==, 0);
	munit_assert_int(xtc_saga_n_completed(NULL), ==, 0);
	munit_assert_int(xtc_saga_failed_step(NULL), ==, -1);
	munit_assert_int(xtc_saga_last_error(NULL), ==, XTC_OK);
	munit_assert_int(xtc_saga_compensate_failed(NULL), ==, 0);

	xtc_saga_destroy(s);
	xtc_saga_destroy(NULL);   /* no-op, must not crash */
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/all_succeed",       test_saga_all_succeed,             NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/fail_first",         test_saga_fail_first,              NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/fail_middle",        test_saga_fail_middle,             NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/fail_last",          test_saga_fail_last,               NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/null_compensate",    test_saga_null_compensate_skipped, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/compensate_fails",   test_saga_compensate_fails,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/misuse",             test_saga_misuse,                  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite suite = {
	"/m14/saga", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

int
main(int argc, char *argv[])
{
	return munit_suite_main(&suite, NULL, argc, argv);
}
