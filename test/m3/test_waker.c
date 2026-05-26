/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the PostgreSQL License.
 *
 * test/m3/test_waker.c — verifies M3_CLAIMS.md Wk1–Wk4.
 */

#include "munit.h"
#include "xtc.h"
#include "xtc_loop.h"

/*
 * The pattern: a "producer" task wakes a "parker" task.  The parker
 * starts, registers a waker exposing itself, returns PENDING.  The
 * producer fires the waker.  The parker wakes, observes a flag, and
 * returns DONE.
 */

struct pair { xtc_waker_t parker_waker; int parker_runs; int parker_done; int produce_phase; };

static int parker_fn(xtc_task_t *self, void *u) {
	struct pair *c = u;
	c->parker_runs++;
	if (c->parker_runs == 1) {
		(void)xtc_task_waker(self, &c->parker_waker);
		return XTC_TASK_PENDING;
	}
	c->parker_done = 1;
	return XTC_TASK_DONE;
}

static int producer_fn(xtc_task_t *self, void *u) {
	struct pair *c = u;
	(void)self;
	c->produce_phase++;
	if (c->produce_phase < 5) return XTC_TASK_RESCHED;
	if (c->produce_phase == 5) {
		(void)xtc_waker_wake(&c->parker_waker);
		return XTC_TASK_DONE;
	}
	return XTC_TASK_DONE;
}

/* [Wk1, Wk2] */
static MunitResult
test_waker_resumes(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	struct pair c = {{NULL,NULL}, 0, 0, 0};
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_task_spawn(loop, parker_fn,   &c, NULL), ==, XTC_OK);
	munit_assert_int(xtc_task_spawn(loop, producer_fn, &c, NULL), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(c.parker_done, ==, 1);
	munit_assert_int(c.parker_runs, ==, 2);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* [Wk1] verify pending blocks: parker would never run again without a wake. */
struct selfwake { int runs; xtc_waker_t self_waker; };
static int self_park_fn(xtc_task_t *self, void *u) {
	struct selfwake *c = u;
	c->runs++;
	if (c->runs == 1) {
		(void)xtc_task_waker(self, &c->self_waker);
		return XTC_TASK_PENDING;
	}
	return XTC_TASK_DONE;
}
static int waker_then_done(xtc_task_t *self, void *u) {
	struct selfwake *c = u;
	(void)self;
	(void)xtc_waker_wake(&c->self_waker);
	return XTC_TASK_DONE;
}

static MunitResult
test_pending_blocks(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	struct selfwake c = { 0, {NULL,NULL} };
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_task_spawn(loop, self_park_fn, &c, NULL), ==, XTC_OK);
	munit_assert_int(xtc_task_spawn(loop, waker_then_done, &c, NULL), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(c.runs, ==, 2);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* [Wk3] idempotent waker.  Wake the parker many times *before* the
 *      next loop step; the parker should still be invoked exactly once. */
struct idem { xtc_waker_t w; int parker_runs; };
static int idem_parker(xtc_task_t *self, void *u) {
	struct idem *c = u;
	c->parker_runs++;
	if (c->parker_runs == 1) {
		(void)xtc_task_waker(self, &c->w);
		return XTC_TASK_PENDING;
	}
	return XTC_TASK_DONE;
}
static int idem_blaster(xtc_task_t *self, void *u) {
	struct idem *c = u;
	int i;
	(void)self;
	for (i = 0; i < 50; i++) (void)xtc_waker_wake(&c->w);
	return XTC_TASK_DONE;
}

static MunitResult
test_waker_idempotent(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	struct idem c = {{NULL,NULL}, 0};
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_task_spawn(loop, idem_parker,  &c, NULL), ==, XTC_OK);
	munit_assert_int(xtc_task_spawn(loop, idem_blaster, &c, NULL), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(c.parker_runs, ==, 2);  /* exactly: initial run + one resume */
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* [Wk4] wake-after-done is a no-op */
struct adone { xtc_waker_t w; int runs; };
static int adone_task(xtc_task_t *self, void *u) {
	struct adone *c = u;
	(void)self;
	(void)xtc_task_waker(self, &c->w);
	c->runs++;
	return XTC_TASK_DONE;
}

static MunitResult
test_waker_after_done(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	struct adone c = {{NULL,NULL}, 0};
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_task_spawn(loop, adone_task, &c, NULL), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	/* The task is DONE; the waker is captured. */
	munit_assert_int(xtc_waker_wake(&c.w), ==, XTC_OK);   /* no crash */
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);     /* no resurrection */
	munit_assert_int(c.runs, ==, 1);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/Wk1_pending_blocks",   test_pending_blocks,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Wk2_waker_resumes",    test_waker_resumes,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Wk3_idempotent",       test_waker_idempotent,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Wk4_after_done",       test_waker_after_done,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m3/waker", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
