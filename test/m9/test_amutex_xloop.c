/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m9/test_amutex_xloop.c
 *	Recursive xtc_amutex + cross-loop contention.  Regression guard
 *	for the cross-loop hand-off crash: many fibers across loops
 *	contend on one recursive amutex, holding it across a park so
 *	unlock must hand off to a parked waiter on another loop.  Also
 *	checks recursive re-entry and mutual exclusion.
 */

#include <stdint.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_sync.h"
#include "xtc_int.h"

/* ---- recursive re-entry on a single context (off loop) ---- */
static MunitResult
test_recursive_basic(const MunitParameter p[], void *d)
{
	xtc_amutex_t *m;
	(void)p; (void)d;
	munit_assert_int(xtc_amutex_create_ex(&m, XTC_AMUTEX_RECURSIVE),
	    ==, XTC_OK);
	munit_assert_int(xtc_amutex_lock(m, -1), ==, XTC_OK);
	munit_assert_int(xtc_amutex_lock(m, -1), ==, XTC_OK);  /* re-enter */
	munit_assert_int(xtc_amutex_lock(m, -1), ==, XTC_OK);  /* again */
	munit_assert_int(xtc_amutex_unlock(m), ==, XTC_OK);
	munit_assert_int(xtc_amutex_unlock(m), ==, XTC_OK);
	/* Still held by us: a try_lock from a *different* identity would
	 * fail, but from the same context recursion still succeeds. */
	munit_assert_int(xtc_amutex_try_lock(m), ==, XTC_OK);  /* re-enter */
	munit_assert_int(xtc_amutex_unlock(m), ==, XTC_OK);
	munit_assert_int(xtc_amutex_unlock(m), ==, XTC_OK);    /* fully free */
	munit_assert_int(xtc_amutex_try_lock(m), ==, XTC_OK);  /* now free */
	munit_assert_int(xtc_amutex_unlock(m), ==, XTC_OK);
	xtc_amutex_destroy(m);
	return MUNIT_OK;
}

/* ---- cross-loop recursive contention stress ---- */
static xtc_amutex_t *g_m;
static int           g_iters;
static int64_t       g_critical;   /* guarded counter; detects races */
static int           g_done;

static void
xloop_worker(void *arg)
{
	int i;
	(void)arg;
	for (i = 0; i < g_iters; i++) {
		if (xtc_amutex_lock(g_m, -1) != XTC_OK)
			continue;
		(void)xtc_amutex_lock(g_m, -1);     /* recursive re-entry */
		/* Critical section: non-atomic RMW; correct iff mutual
		 * exclusion holds.  Park while holding to force waiters to
		 * queue and unlock to hand off across loops. */
		g_critical++;
		xtc_proc_sleep(1000);
		g_critical++;
		(void)xtc_amutex_unlock(g_m);
		(void)xtc_amutex_unlock(g_m);
	}
	(void)__os_atomic_fetch_add_i32(&g_done, 1);
}

static MunitResult
test_xloop_recursive(const MunitParameter p[], void *d)
{
	xtc_exec_t *e;
	int n_loops = 4;
	int workers = 48;
	int i;
	(void)p; (void)d;

	g_iters = 300;
	g_critical = 0;
	__os_atomic_store_i32(&g_done, 0);
	munit_assert_int(xtc_amutex_create_ex(&g_m, XTC_AMUTEX_RECURSIVE),
	    ==, XTC_OK);
	munit_assert_int(xtc_exec_init(&e, n_loops), ==, XTC_OK);
	for (i = 0; i < workers; i++) {
		xtc_loop_t *l = xtc_exec_loop(e, (unsigned)(i % n_loops));
		munit_assert_int(
		    xtc_proc_spawn(l, xloop_worker, NULL, NULL, NULL),
		    ==, XTC_OK);
	}
	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);
	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);
	xtc_amutex_destroy(g_m);

	/* Every worker finished (no crash / no lost-wakeup deadlock). */
	munit_assert_int(__os_atomic_load_i32(&g_done), ==, workers);
	/* Mutual exclusion held: 2 increments per iteration, no torn RMW. */
	munit_assert_int64(g_critical, ==,
	    (int64_t)workers * g_iters * 2);
	return MUNIT_OK;
}

/* ---- static mutex pool returns stable, recursive objects ---- */
static MunitResult
test_static_pool(const MunitParameter p[], void *d)
{
	xtc_amutex_t *a, *b;
	(void)p; (void)d;
	a = xtc_amutex_static(3);
	b = xtc_amutex_static(3);
	munit_assert_not_null(a);
	munit_assert_ptr_equal(a, b);            /* stable per slot */
	munit_assert_ptr_not_equal(a, xtc_amutex_static(4));
	munit_assert_null(xtc_amutex_static(XTC_AMUTEX_STATIC_MAX));
	/* Recursive by contract. */
	munit_assert_int(xtc_amutex_lock(a, -1), ==, XTC_OK);
	munit_assert_int(xtc_amutex_lock(a, -1), ==, XTC_OK);
	munit_assert_int(xtc_amutex_unlock(a), ==, XTC_OK);
	munit_assert_int(xtc_amutex_unlock(a), ==, XTC_OK);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/recursive_basic",  test_recursive_basic, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/xloop_recursive",  test_xloop_recursive, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/static_pool",      test_static_pool,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m9/amutex_xloop", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
