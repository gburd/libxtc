/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/m10/test_sup.c — verifies M10 supervisor.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_orc.h"
#include "xtc_int.h"

/* Shared counter that flapping children bump on each spawn. */
static int g_spawn_count;
static int g_target_spawns;

static void
flapper_proc(void *arg)
{
	void *msg; size_t sz;
	(void)arg;
	__os_atomic_fetch_add_i32(&g_spawn_count, 1);
	if (__os_atomic_load_i32(&g_spawn_count) < g_target_spawns) {
		/* Die abnormally so the supervisor restarts us. */
		xtc_exit_self(7);
	}
	/* Otherwise stay alive briefly using a yield-based wait so we
	 * don't block the loop thread. */
	(void)xtc_recv(&msg, &sz, 50 * 1000 * 1000);
	if (msg) __os_free(msg);
}

/* The supervisor restarts a flapping child until it reaches a target
 * spawn count, then the child stays alive.  The test uses a separate
 * watchdog proc that polls until the target spawn count is reached,
 * then asks the supervisor to stop so xtc_loop_run can return. */
static xtc_supervisor_t *g_sup_for_watch;

static void
watch_proc(void *arg)
{
	void *msg; size_t sz;
	(void)arg;
	while (__os_atomic_load_i32(&g_spawn_count) < g_target_spawns) {
		/* Yield-based polling so we don't block the loop thread. */
		(void)xtc_recv(&msg, &sz, 5 * 1000 * 1000);   /* 5ms */
		if (msg) __os_free(msg);
	}
	/* Brief settle to let the supervisor record the last restart. */
	(void)xtc_recv(&msg, &sz, 50 * 1000 * 1000);
	if (msg) __os_free(msg);
	(void)xtc_sup_stop(g_sup_for_watch);
}

static MunitResult
test_supervisor_restarts(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_supervisor_t *sup;
	xtc_sup_opts_t opts = XTC_SUP_OPTS_DEFAULT;
	xtc_child_spec_t kids[1];
	xtc_pid_t watcher_pid;
	(void)p; (void)d;

	__os_atomic_store_i32(&g_spawn_count, 0);
	g_target_spawns = 3;
	opts.max_restarts = 5;
	opts.period_ns    = 1000LL * 1000 * 1000;

	memset(&kids[0], 0, sizeof kids[0]);
	kids[0].name   = "flapper";
	kids[0].fn     = flapper_proc;
	kids[0].arg    = NULL;
	kids[0].policy = XTC_RESTART_TRANSIENT;   /* restart only on abnormal exit */

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_sup_start(loop, &opts, kids, 1, &sup), ==, XTC_OK);
	g_sup_for_watch = sup;
	munit_assert_int(xtc_proc_spawn(loop, watch_proc, NULL, NULL,
	    &watcher_pid), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	munit_assert_int(__os_atomic_load_i32(&g_spawn_count), ==, g_target_spawns);
	munit_assert_int(xtc_sup_n_restarts(sup), >=, g_target_spawns - 1);
	munit_assert_int(xtc_sup_join(sup, 1LL * 1000 * 1000 * 1000), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* one_for_all: when one child crashes, all siblings are killed and
 * restarted in order.  We track per-child spawn counts and verify
 * that crashing child A also restarts child B. */
static int g_ofa_a_spawns;
static int g_ofa_b_spawns;
static int g_ofa_target_a_crashes;

static void ofa_a(void *arg) {
	void *m; size_t s;
	(void)arg;
	__os_atomic_fetch_add_i32(&g_ofa_a_spawns, 1);
	if (__os_atomic_load_i32(&g_ofa_a_spawns) <= g_ofa_target_a_crashes) {
		(void)xtc_recv(&m, &s, 20 * 1000 * 1000);
		if (m) __os_free(m);
		xtc_exit_self(7);
	}
	(void)xtc_recv(&m, &s, 200 * 1000 * 1000);
	if (m) __os_free(m);
}
static void ofa_b(void *arg) {
	void *m; size_t s;
	(void)arg;
	__os_atomic_fetch_add_i32(&g_ofa_b_spawns, 1);
	for (;;) {
		(void)xtc_recv(&m, &s, 200 * 1000 * 1000);
		if (m) __os_free(m);
	}
}

static void ofa_watch(void *arg) {
	void *m; size_t s;
	xtc_supervisor_t *sup = arg;
	while (__os_atomic_load_i32(&g_ofa_b_spawns) < 2) {
		(void)xtc_recv(&m, &s, 5 * 1000 * 1000);
		if (m) __os_free(m);
	}
	(void)xtc_sup_stop(sup);
}

static MunitResult
test_one_for_all(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_supervisor_t *sup;
	xtc_sup_opts_t opts = XTC_SUP_OPTS_DEFAULT;
	xtc_child_spec_t kids[2];
	xtc_pid_t wpid;
	(void)p; (void)d;

	__os_atomic_store_i32(&g_ofa_a_spawns, 0);
	__os_atomic_store_i32(&g_ofa_b_spawns, 0);
	g_ofa_target_a_crashes = 1;
	opts.strategy = XTC_SUP_ONE_FOR_ALL;
	opts.max_restarts = 5;
	opts.period_ns    = 1000LL * 1000 * 1000;

	memset(kids, 0, sizeof kids);
	kids[0].name = "a"; kids[0].fn = ofa_a; kids[0].policy = XTC_RESTART_TRANSIENT;
	kids[1].name = "b"; kids[1].fn = ofa_b; kids[1].policy = XTC_RESTART_TRANSIENT;

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_sup_start(loop, &opts, kids, 2, &sup), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, ofa_watch, sup, NULL, &wpid),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	munit_assert_int(__os_atomic_load_i32(&g_ofa_b_spawns), >=, 2);
	munit_assert_int(__os_atomic_load_i32(&g_ofa_a_spawns), >=, 2);
	munit_assert_int(xtc_sup_join(sup, 1LL * 1000 * 1000 * 1000), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* rest_for_one: when child N crashes, children N..end restart;
 * children 0..N-1 are unaffected. */
static int g_rfo_a_spawns;
static int g_rfo_b_spawns;
static int g_rfo_c_spawns;
static int g_rfo_target_b_crashes;

static void rfo_long_running(int *counter) {
	void *m; size_t s;
	__os_atomic_fetch_add_i32(counter, 1);
	for (;;) {
		(void)xtc_recv(&m, &s, 200 * 1000 * 1000);
		if (m) __os_free(m);
	}
}
static void rfo_a(void *arg) { (void)arg; rfo_long_running(&g_rfo_a_spawns); }
static void rfo_c(void *arg) { (void)arg; rfo_long_running(&g_rfo_c_spawns); }
static void rfo_b(void *arg) {
	void *m; size_t s;
	(void)arg;
	__os_atomic_fetch_add_i32(&g_rfo_b_spawns, 1);
	if (__os_atomic_load_i32(&g_rfo_b_spawns) <= g_rfo_target_b_crashes) {
		(void)xtc_recv(&m, &s, 20 * 1000 * 1000);
		if (m) __os_free(m);
		xtc_exit_self(7);
	}
	for (;;) {
		(void)xtc_recv(&m, &s, 200 * 1000 * 1000);
		if (m) __os_free(m);
	}
}
static void rfo_watch(void *arg) {
	void *m; size_t s;
	xtc_supervisor_t *sup = arg;
	while (__os_atomic_load_i32(&g_rfo_c_spawns) < 2) {
		(void)xtc_recv(&m, &s, 5 * 1000 * 1000);
		if (m) __os_free(m);
	}
	(void)xtc_sup_stop(sup);
}

static MunitResult
test_rest_for_one(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_supervisor_t *sup;
	xtc_sup_opts_t opts = XTC_SUP_OPTS_DEFAULT;
	xtc_child_spec_t kids[3];
	xtc_pid_t wpid;
	(void)p; (void)d;

	__os_atomic_store_i32(&g_rfo_a_spawns, 0);
	__os_atomic_store_i32(&g_rfo_b_spawns, 0);
	__os_atomic_store_i32(&g_rfo_c_spawns, 0);
	g_rfo_target_b_crashes = 1;
	opts.strategy = XTC_SUP_REST_FOR_ONE;
	opts.max_restarts = 5;
	opts.period_ns    = 1000LL * 1000 * 1000;

	memset(kids, 0, sizeof kids);
	kids[0].name = "a"; kids[0].fn = rfo_a; kids[0].policy = XTC_RESTART_TRANSIENT;
	kids[1].name = "b"; kids[1].fn = rfo_b; kids[1].policy = XTC_RESTART_TRANSIENT;
	kids[2].name = "c"; kids[2].fn = rfo_c; kids[2].policy = XTC_RESTART_TRANSIENT;

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_sup_start(loop, &opts, kids, 3, &sup), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, rfo_watch, sup, NULL, &wpid),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	/* a should NOT have been restarted (it's before the crashing b). */
	munit_assert_int(__os_atomic_load_i32(&g_rfo_a_spawns), ==, 1);
	/* b restarted (it crashed). */
	munit_assert_int(__os_atomic_load_i32(&g_rfo_b_spawns), >=, 2);
	/* c restarted (it's after b). */
	munit_assert_int(__os_atomic_load_i32(&g_rfo_c_spawns), >=, 2);

	munit_assert_int(xtc_sup_join(sup, 1LL * 1000 * 1000 * 1000), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* Restart intensity exceeded: the supervisor itself exits. */
static int g_intensity_count;
static void
fast_flapper(void *arg)
{
	(void)arg;
	__os_atomic_fetch_add_i32(&g_intensity_count, 1);
	xtc_exit_self(1);   /* always die abnormally */
}

static MunitResult
test_intensity_exceeded(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_supervisor_t *sup;
	xtc_sup_opts_t opts = XTC_SUP_OPTS_DEFAULT;
	xtc_child_spec_t kids[1];
	(void)p; (void)d;

	__os_atomic_store_i32(&g_intensity_count, 0);

	opts.max_restarts = 3;
	opts.period_ns    = 5LL * 1000 * 1000 * 1000;

	memset(&kids[0], 0, sizeof kids[0]);
	kids[0].name   = "fast";
	kids[0].fn     = fast_flapper;
	kids[0].policy = XTC_RESTART_PERMANENT;

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_sup_start(loop, &opts, kids, 1, &sup), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	/* Supervisor died (alive=0); restarts at most max_restarts+1.
	 * The +1 is the initial spawn. */
	munit_assert_int(xtc_sup_alive(sup), ==, 0);
	munit_assert_int(__os_atomic_load_i32(&g_intensity_count), <=,
	    opts.max_restarts + 2);
	munit_assert_int(xtc_sup_join(sup, 1LL * 1000 * 1000 * 1000), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/supervisor_restarts",   test_supervisor_restarts, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/one_for_all",           test_one_for_all,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/rest_for_one",          test_rest_for_one,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/intensity_exceeded",    test_intensity_exceeded,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m10/sup", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
