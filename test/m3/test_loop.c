/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/m3/test_loop.c — verifies M3_CLAIMS.md Ev1–Ev5.
 */

#include "munit.h"
#include "xtc.h"
#include "xtc_loop.h"

/* [Ev1, Ev2] */
static MunitResult
test_init_fini(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_not_null(loop);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_init(NULL), ==, XTC_E_INVAL);
	munit_assert_int(xtc_loop_fini(NULL), ==, XTC_E_INVAL);
	return MUNIT_OK;
}

/* [Ev3] */
static MunitResult
test_run_empty(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* [Ev4] */
static int hits;
static int task_done_once(xtc_task_t *self, void *u) {
	(void)self; (void)u; hits++; return XTC_TASK_DONE;
}

static MunitResult
test_run_until_done(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	(void)p; (void)d;
	hits = 0;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_task_spawn(loop, task_done_once, NULL, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(hits, ==, 1);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* [Ev5] stop */
struct stop_ctx { xtc_loop_t *loop; int counter; };
static int task_stop(xtc_task_t *self, void *u) {
	struct stop_ctx *c = u;
	(void)self;
	c->counter++;
	if (c->counter == 3)
		(void)xtc_loop_stop(c->loop);
	return XTC_TASK_RESCHED;
}

static MunitResult
test_stop(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	struct stop_ctx c;
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	c.loop = loop; c.counter = 0;
	munit_assert_int(xtc_task_spawn(loop, task_stop, &c, NULL), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(c.counter, >=, 3);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/Ev1_Ev2_init_fini",     test_init_fini,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Ev3_run_empty",         test_run_empty,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Ev4_run_until_done",    test_run_until_done,NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Ev5_stop",              test_stop,          NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m3/loop", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
