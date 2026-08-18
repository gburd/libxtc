/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/m14/test_stall.c
 *	L3 -- OVER-BUDGET STALL WATCHDOG.  INSPIRED BY Glommio's stall
 *	detector (executor/stall.rs): when a single task run exceeds a
 *	budget the runtime reports WHICH code monopolized the core.
 *	libxtc does it with a cheap in-loop wall-clock check at the
 *	run-end boundary (xtc_loop_set_stall_budget) -- no watcher
 *	thread, no signal -- so it is a single branch on a disabled flag
 *	when off.
 *
 *	Claims:
 *	  S1: a deliberately-hogging fiber (a long CPU burn with no yield)
 *	      trips the watchdog; the report callback fires and names the
 *	      offending loop + task with a sane ran/budget.
 *	  S2: a well-behaved run (short chunks, cooperative yields) under
 *	      the SAME budget never trips.
 *	  S3: OFF BY DEFAULT -- budget 0 (the default) never fires, even
 *	      for a hogging fiber; and the stall count stays 0.
 */

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_async.h"

static atomic_int  g_stall_reports;
static atomic_long g_last_ran_ns;
static atomic_long g_last_budget_ns;
static xtc_loop_t *g_last_loop;
static xtc_task_t *g_last_task;
static volatile uint64_t g_sink;

static void
stall_cb(xtc_loop_t *loop, xtc_task_t *task, int64_t ran_ns,
    int64_t budget_ns, void *user)
{
	(void)user;
	g_last_loop = loop;
	g_last_task = task;
	atomic_store_explicit(&g_last_ran_ns, ran_ns, memory_order_relaxed);
	atomic_store_explicit(&g_last_budget_ns, budget_ns,
	    memory_order_relaxed);
	atomic_fetch_add_explicit(&g_stall_reports, 1, memory_order_relaxed);
}

/* A hogging fiber: one long CPU burn with NO yield inside the run, so a
 * single run/quantum blows well past a small budget. */
static void
hog(void *arg)
{
	uint64_t acc = 0, k;
	(void)arg;
	for (k = 0; k < 200ULL * 1000 * 1000; k++)
		acc += (k ^ (k << 1)) + (acc >> 3);
	g_sink += acc;
}

/* A well-behaved fiber: many tiny chunks, each a separate short run
 * (it yields between them), so no single run exceeds the budget. */
static void
polite(void *arg)
{
	int i;
	(void)arg;
	for (i = 0; i < 200; i++) {
		uint64_t acc = 0, k;
		for (k = 0; k < 1000; k++)
			acc += (k ^ (k << 1)) + (acc >> 3);
		g_sink += acc;
		xtc_yield();
	}
}

static void
reset(void)
{
	atomic_store(&g_stall_reports, 0);
	atomic_store(&g_last_ran_ns, 0);
	atomic_store(&g_last_budget_ns, 0);
	g_last_loop = NULL;
	g_last_task = NULL;
}

/* S1: a hog trips the watchdog and the report names it. */
static MunitResult
test_hog_trips(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	(void)p; (void)d;
	reset();

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	/* 5 ms budget: a 200M-iter burn in one run blows past it. */
	xtc_loop_set_stall_budget(loop, 5 * 1000 * 1000LL);
	xtc_loop_set_stall_cb(loop, stall_cb, NULL);

	(void)xtc_proc_spawn(loop, hog, NULL, NULL, NULL);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	munit_assert_int(atomic_load(&g_stall_reports), >, 0);   /* tripped */
	munit_assert_ptr(g_last_loop, ==, loop);                 /* names loop */
	munit_assert_ptr_not_null(g_last_task);                  /* names task */
	/* ran >= budget (the definition of an overrun). */
	munit_assert_long(atomic_load(&g_last_ran_ns), >=,
	    atomic_load(&g_last_budget_ns));
	munit_assert_long(atomic_load(&g_last_budget_ns), ==, 5 * 1000 * 1000LL);
	munit_assert_uint64(xtc_loop_stall_count(loop), >, 0);

	(void)xtc_loop_fini(loop);
	return MUNIT_OK;
}

/* S2: a well-behaved run never trips under the same budget. */
static MunitResult
test_polite_ok(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	(void)p; (void)d;
	reset();

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	xtc_loop_set_stall_budget(loop, 5 * 1000 * 1000LL);
	xtc_loop_set_stall_cb(loop, stall_cb, NULL);

	(void)xtc_proc_spawn(loop, polite, NULL, NULL, NULL);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	munit_assert_int(atomic_load(&g_stall_reports), ==, 0);  /* never */
	munit_assert_uint64(xtc_loop_stall_count(loop), ==, 0);

	(void)xtc_loop_fini(loop);
	return MUNIT_OK;
}

/* S3: OFF BY DEFAULT -- budget 0 never fires, even for a hog. */
static MunitResult
test_off_by_default(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	(void)p; (void)d;
	reset();

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	/* No xtc_loop_set_stall_budget call: the default is off (0). */
	xtc_loop_set_stall_cb(loop, stall_cb, NULL);

	(void)xtc_proc_spawn(loop, hog, NULL, NULL, NULL);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	munit_assert_int(atomic_load(&g_stall_reports), ==, 0);  /* off */
	munit_assert_uint64(xtc_loop_stall_count(loop), ==, 0);

	(void)xtc_loop_fini(loop);
	return MUNIT_OK;
}

/* S3b: the exec-wide convenience arms every loop's budget. */
static MunitResult
test_exec_wide_budget(const MunitParameter p[], void *d)
{
	xtc_exec_t *e = NULL;
	int i;
	(void)p; (void)d;
	reset();

	munit_assert_int(xtc_exec_init(&e, 2), ==, XTC_OK);
	xtc_exec_set_stall_budget(e, 5 * 1000 * 1000LL);
	for (i = 0; i < 2; i++)
		xtc_loop_set_stall_cb(xtc_exec_loop(e, i), stall_cb, NULL);

	/* Spawn a hog proc on loop 0; the exec-wide budget armed it. */
	(void)xtc_proc_spawn(xtc_exec_loop(e, 0), hog, NULL, NULL, NULL);

	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);
	munit_assert_int(atomic_load(&g_stall_reports), >, 0);   /* tripped */

	(void)xtc_exec_fini(e);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/hog_trips", test_hog_trips, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/polite_ok", test_polite_ok, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/off_by_default", test_off_by_default, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/exec_wide_budget", test_exec_wide_budget, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite suite = {
	"/m14/stall", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

int
main(int argc, char *argv[])
{
	return munit_suite_main(&suite, NULL, argc, argv);
}
