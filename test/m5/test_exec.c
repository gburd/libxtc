/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m5/test_exec.c -- verifies M5_CLAIMS.md Ex1-Ex4, Sp1-Sp3.
 */

#include <stdint.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_int.h"

/* [Ex1, Ex2] init/fini round-trip. */
static MunitResult
test_init_fini(const MunitParameter p[], void *d)
{
	xtc_exec_t *e;
	(void)p; (void)d;
	munit_assert_int(xtc_exec_init(&e, 4), ==, XTC_OK);
	munit_assert_int(xtc_exec_n_loops(e), ==, 4);
	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);

	/* Ex1.b: n_loops <= 0 defaults to __os_ncpus(). */
	munit_assert_int(xtc_exec_init(&e, 0), ==, XTC_OK);
	{
		int expect = __os_ncpus();
		if (expect <= 0) expect = 4;
		munit_assert_int(xtc_exec_n_loops(e), ==, expect);
	}
	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);

	munit_assert_int(xtc_exec_init(NULL, 4), ==, XTC_E_INVAL);
	munit_assert_int(xtc_exec_fini(NULL), ==, XTC_E_INVAL);
	return MUNIT_OK;
}

/* [Ex3] run-until-done with no tasks should return immediately. */
static MunitResult
test_run_until_done(const MunitParameter p[], void *d)
{
	xtc_exec_t *e;
	(void)p; (void)d;
	munit_assert_int(xtc_exec_init(&e, 2), ==, XTC_OK);
	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);
	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);
	return MUNIT_OK;
}

/* [Ex4] n_loops sanity. */
static MunitResult
test_n_loops(const MunitParameter p[], void *d)
{
	xtc_exec_t *e;
	(void)p; (void)d;
	munit_assert_int(xtc_exec_init(&e, 1), ==, XTC_OK);
	munit_assert_int(xtc_exec_n_loops(e), ==, 1);
	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);
	munit_assert_int(xtc_exec_init(&e, 7), ==, XTC_OK);
	munit_assert_int(xtc_exec_n_loops(e), ==, 7);
	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);
	return MUNIT_OK;
}

/* [Sp1] basic spawn+run. */
static int sp1_ran;
static int sp1_task(xtc_task_t *self, void *u) {
	(void)self; (void)u;
	__os_atomic_fetch_add_i32(&sp1_ran, 1);
	return XTC_TASK_DONE;
}

static MunitResult
test_spawn(const MunitParameter p[], void *d)
{
	xtc_exec_t *e;
	(void)p; (void)d;
	__os_atomic_store_i32(&sp1_ran, 0);
	munit_assert_int(xtc_exec_init(&e, 4), ==, XTC_OK);
	munit_assert_int(xtc_exec_spawn(e, sp1_task, NULL, NULL), ==, XTC_OK);
	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);
	munit_assert_int(__os_atomic_load_i32(&sp1_ran), ==, 1);
	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);
	return MUNIT_OK;
}

/* [Sp2] spawn_on places on the requested loop. */
static int sp2_observed_id;
static int sp2_task(xtc_task_t *self, void *u) {
	(void)self; (void)u;
	sp2_observed_id = xtc_exec_loop_id();
	return XTC_TASK_DONE;
}

static MunitResult
test_spawn_on(const MunitParameter p[], void *d)
{
	xtc_exec_t *e;
	(void)p; (void)d;
	munit_assert_int(xtc_exec_init(&e, 4), ==, XTC_OK);
	sp2_observed_id = -1;
	munit_assert_int(xtc_exec_spawn_on(e, 2, sp2_task, NULL, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);
	munit_assert_int(sp2_observed_id, ==, 2);
	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);
	return MUNIT_OK;
}

/* [Sp3] N spawned tasks: shared atomic counter sums correctly. */
#define SP3_N 64
static int64_t sp3_counter;
static int sp3_task(xtc_task_t *self, void *u) {
	int64_t inc = (intptr_t)u;
	(void)self;
	(void)__os_atomic_fetch_add_i64(&sp3_counter, inc);
	return XTC_TASK_DONE;
}

static MunitResult
test_n_spawned_sum(const MunitParameter p[], void *d)
{
	xtc_exec_t *e;
	int i;
	int64_t expected = 0;
	(void)p; (void)d;
	munit_assert_int(xtc_exec_init(&e, 4), ==, XTC_OK);
	__os_atomic_store_i64(&sp3_counter, 0);
	for (i = 0; i < SP3_N; i++) {
		expected += i + 1;
		munit_assert_int(xtc_exec_spawn(e, sp3_task,
		    (void *)(intptr_t)(i + 1), NULL), ==, XTC_OK);
	}
	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);
	munit_assert_int64(__os_atomic_load_i64(&sp3_counter), ==, expected);
	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/Ex1_Ex2_init_fini",       test_init_fini,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Ex3_run_until_done",      test_run_until_done,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Ex4_n_loops",             test_n_loops,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Sp1_spawn",               test_spawn,           NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Sp2_spawn_on",            test_spawn_on,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Sp3_n_spawned_sum",       test_n_spawned_sum,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m5/exec", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
