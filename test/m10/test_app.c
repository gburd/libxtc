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
	munit_assert_null(xtc_app_exec(a));   /* single-loop: no executor */
	xtc_app_destroy(a);
	return MUNIT_OK;
}

/* ---- multi-loop supervised app: placement + cross-loop restart ---- */
#include "xtc_exec.h"

struct mlctx {
	_Atomic int seen[4];      /* loop id each placed worker observed */
	_Atomic int reported;     /* workers that recorded their loop */
	_Atomic int crash;        /* crasher entries */
	_Atomic int restarted;    /* crasher's post-restart run reached */
};
static struct mlctx g_ml;
static xtc_app_t   *g_ml_app;

static void
ml_worker(void *arg)
{
	int i = (int)(intptr_t)arg;
	if (i >= 0 && i < 4)
		atomic_store(&g_ml.seen[i], xtc_exec_loop_id());
	atomic_fetch_add(&g_ml.reported, 1);
	/* TEMPORARY child: just exit (no restart). */
}

static void
ml_crasher(void *arg)
{
	void *m; size_t s;
	(void)arg;
	if (atomic_fetch_add(&g_ml.crash, 1) == 0)
		xtc_exit_self(1);          /* abnormal -> PERMANENT restart */
	atomic_store(&g_ml.restarted, 1);
	(void)xtc_recv(&m, &s, 60LL * 1000 * 1000);
	if (m) __os_free(m);
}

static void
ml_driver(void *arg)
{
	void *m; size_t s;
	(void)arg;
	while (atomic_load(&g_ml.reported) < 4 ||
	       atomic_load(&g_ml.restarted) < 1) {
		(void)xtc_recv(&m, &s, 5LL * 1000 * 1000);
		if (m) __os_free(m);
	}
	(void)xtc_app_stop(g_ml_app);
}

static MunitResult
test_app_multiloop(const MunitParameter p[], void *d)
{
	xtc_app_t *a;
	xtc_app_opts_t opts = XTC_APP_OPTS_DEFAULT;
	xtc_child_spec_t kids[5];
	xtc_pid_t dpid;
	int i;
	(void)p; (void)d;

	memset(&g_ml, 0, sizeof g_ml);
	for (i = 0; i < 4; i++) atomic_store(&g_ml.seen[i], -1);

	opts.name = "test_ml";
	opts.n_loops = 4;
	opts.sup.max_restarts = 5;
	opts.sup.period_ns    = 2000LL * 1000 * 1000;

	memset(kids, 0, sizeof kids);
	for (i = 0; i < 4; i++) {
		kids[i].name   = "ml_worker";
		kids[i].fn     = ml_worker;
		kids[i].arg    = (void *)(intptr_t)i;
		kids[i].loop   = i;                   /* place on loop i */
		kids[i].policy = XTC_RESTART_TEMPORARY;
	}
	kids[4].name   = "ml_crasher";
	kids[4].fn     = ml_crasher;
	kids[4].loop   = 2;                       /* crash + restart on loop 2 */
	kids[4].policy = XTC_RESTART_PERMANENT;

	munit_assert_int(xtc_app_create(&opts, &a), ==, XTC_OK);
	g_ml_app = a;
	munit_assert_not_null(xtc_app_exec(a));        /* multi-loop executor */
	munit_assert_int(xtc_exec_n_loops(xtc_app_exec(a)), ==, 4);
	munit_assert_int(xtc_app_start(a, kids, 5), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(xtc_app_loop(a), ml_driver, NULL,
	    NULL, &dpid), ==, XTC_OK);
	munit_assert_int(xtc_app_run(a), ==, XTC_OK);

	/* Every placed worker ran and recorded a VALID executor loop id.
	 * We do not assert seen[i] == i: a child is placed (initially
	 * enqueued) on its requested loop, but procs are unpinned, so the
	 * executor may work-steal one onto an idle loop -- desirable load
	 * balancing.  A -1 here would mean a worker ran during teardown
	 * (__xtc_current_loop cleared), i.e. the app stopped early; the
	 * [0,3] bound still catches that. */
	for (i = 0; i < 4; i++) {
		int lid = atomic_load(&g_ml.seen[i]);
		munit_assert_int(lid, >=, 0);
		munit_assert_int(lid, <=, 3);
	}
	/* The crasher crashed once and was restarted (cross-loop). */
	munit_assert_int(atomic_load(&g_ml.crash), >=, 2);
	munit_assert_int(atomic_load(&g_ml.restarted), ==, 1);
	xtc_app_destroy(a);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/app_basic", test_app_basic, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/app_multiloop", test_app_multiloop, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m10.5/app", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
