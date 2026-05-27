/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m10/test_app.c -- verifies M10.5 xtc_app.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_app.h"
#include "xtc_proc.h"
#include "xtc_int.h"

static _Atomic int g_kid_ran;

static void
kid_proc(void *arg)
{
	void *m; size_t s;
	(void)arg;
	atomic_fetch_add_explicit(&g_kid_ran, 1, memory_order_relaxed);
	(void)xtc_recv(&m, &s, 50 * 1000 * 1000);
	if (m) __os_free(m);
}

/* A driver proc that stops the app once it sees the kid has run. */
static xtc_app_t *g_app;

static void
driver_proc(void *arg)
{
	void *m; size_t s;
	(void)arg;
	while (atomic_load_explicit(&g_kid_ran, memory_order_relaxed) < 1) {
		(void)xtc_recv(&m, &s, 5 * 1000 * 1000);
		if (m) __os_free(m);
	}
	(void)xtc_app_stop(g_app);
}

static MunitResult
test_app_basic(const MunitParameter p[], void *d)
{
	xtc_app_t *a;
	xtc_app_opts_t opts = XTC_APP_OPTS_DEFAULT;
	xtc_child_spec_t kids[1];
	xtc_pid_t dpid;
	(void)p; (void)d;

	atomic_store(&g_kid_ran, 0);
	opts.name = "test_app";
	opts.sup.max_restarts = 5;
	opts.sup.period_ns    = 1000LL * 1000 * 1000;

	memset(kids, 0, sizeof kids);
	kids[0].name   = "kid";
	kids[0].fn     = kid_proc;
	kids[0].policy = XTC_RESTART_TRANSIENT;

	munit_assert_int(xtc_app_create(&opts, &a), ==, XTC_OK);
	g_app = a;
	munit_assert_not_null(xtc_app_loop(a));
	munit_assert_not_null(xtc_app_registry(a));
	munit_assert_int(xtc_app_start(a, kids, 1), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(xtc_app_loop(a), driver_proc, NULL,
	    NULL, &dpid), ==, XTC_OK);
	munit_assert_int(xtc_app_run(a), ==, XTC_OK);

	munit_assert_int(atomic_load(&g_kid_ran), >=, 1);
	xtc_app_destroy(a);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/app_basic", test_app_basic, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m10.5/app", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
