/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/otp/test_otp_supervisor.c — port of selected supervisor_SUITE
 * cases from `lib/stdlib/test/supervisor_SUITE.erl`.
 *
 * Maps to xtc_orc.h:
 *   one_for_one        -> XTC_SUP_ONE_FOR_ONE
 *   one_for_all        -> XTC_SUP_ONE_FOR_ALL
 *   rest_for_one       -> XTC_SUP_REST_FOR_ONE
 *   simple_one_for_one -> XTC_SUP_SIMPLE_OFO
 *   permanent          -> XTC_RESTART_PERMANENT
 *   transient          -> XTC_RESTART_TRANSIENT
 *   temporary          -> XTC_RESTART_TEMPORARY
 *   intensity / period -> max_restarts / period_ns
 *
 * Tests omitted (BEAM-only):
 *   - hot code change
 *   - simple_global_supervisor (no global namespace)
 *   - tree (deeply nested supervisors — straightforward to add)
 *   - hibernate
 *   - format_log_*
 *   - get_callback_module (no callback modules in C)
 *   - which_children (we expose n_children only)
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_orc.h"
#include "xtc_int.h"

/* ----- Helpers ------------------------------------------------- */

static _Atomic int g_a_spawns;
static _Atomic int g_b_spawns;
static _Atomic int g_c_spawns;

static void
proc_a(void *arg)
{
	void *m; size_t sz;
	int crashes = (int)(intptr_t)arg;
	int n = atomic_fetch_add(&g_a_spawns, 1) + 1;
	if (n <= crashes) {
		(void)xtc_recv(&m, &sz, 20 * 1000 * 1000);
		if (m) __os_free(m);
		xtc_exit_self(7);
	}
	for (;;) {
		(void)xtc_recv(&m, &sz, 200 * 1000 * 1000);
		if (m) __os_free(m);
	}
}

static void
proc_b_endless(void *arg)
{
	void *m; size_t sz;
	(void)arg;
	atomic_fetch_add(&g_b_spawns, 1);
	for (;;) {
		(void)xtc_recv(&m, &sz, 200 * 1000 * 1000);
		if (m) __os_free(m);
	}
}

static void
proc_c_endless(void *arg)
{
	void *m; size_t sz;
	(void)arg;
	atomic_fetch_add(&g_c_spawns, 1);
	for (;;) {
		(void)xtc_recv(&m, &sz, 200 * 1000 * 1000);
		if (m) __os_free(m);
	}
}

static xtc_supervisor_t *g_sup_for_watcher;
static int g_target_a_spawns;

static void
watcher(void *arg)
{
	void *m; size_t sz;
	(void)arg;
	while (atomic_load(&g_a_spawns) < g_target_a_spawns) {
		(void)xtc_recv(&m, &sz, 5 * 1000 * 1000);
		if (m) __os_free(m);
	}
	(void)xtc_recv(&m, &sz, 50 * 1000 * 1000);   /* settle */
	if (m) __os_free(m);
	(void)xtc_sup_stop(g_sup_for_watcher);
}

/* ----- supervisor_SUITE: one_for_one ---------------------------- */

/* When child A crashes under one_for_one, only A is restarted. */
static MunitResult
test_one_for_one_basic(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_supervisor_t *sup;
	xtc_sup_opts_t opts = XTC_SUP_OPTS_DEFAULT;
	xtc_child_spec_t kids[2];
	xtc_pid_t wpid;
	(void)p; (void)d;
	atomic_store(&g_a_spawns, 0);
	atomic_store(&g_b_spawns, 0);
	g_target_a_spawns = 3;
	opts.strategy = XTC_SUP_ONE_FOR_ONE;
	opts.max_restarts = 5;
	opts.period_ns = 1000LL * 1000 * 1000;

	memset(kids, 0, sizeof kids);
	kids[0].name = "a"; kids[0].fn = proc_a;
	kids[0].arg = (void *)(intptr_t)2;   /* crash twice */
	kids[0].policy = XTC_RESTART_TRANSIENT;
	kids[1].name = "b"; kids[1].fn = proc_b_endless;
	kids[1].policy = XTC_RESTART_TRANSIENT;

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_sup_start(loop, &opts, kids, 2, &sup), ==, XTC_OK);
	g_sup_for_watcher = sup;
	munit_assert_int(xtc_proc_spawn(loop, watcher, NULL, NULL, &wpid),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	/* A was restarted twice (3 spawns total).
	 * B was started once and never restarted (one_for_one isolates). */
	munit_assert_int(atomic_load(&g_a_spawns), ==, 3);
	munit_assert_int(atomic_load(&g_b_spawns), ==, 1);
	munit_assert_int(xtc_sup_join(sup, 1000LL * 1000 * 1000), ==, XTC_OK);
	(void)xtc_loop_fini(loop);
	return MUNIT_OK;
}

/* ----- supervisor_SUITE: one_for_all ---------------------------- */

/* When child A crashes under one_for_all, ALL siblings are killed
 * and restarted in original order. */
static MunitResult
test_one_for_all(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_supervisor_t *sup;
	xtc_sup_opts_t opts = XTC_SUP_OPTS_DEFAULT;
	xtc_child_spec_t kids[2];
	xtc_pid_t wpid;
	(void)p; (void)d;
	atomic_store(&g_a_spawns, 0);
	atomic_store(&g_b_spawns, 0);
	g_target_a_spawns = 2;
	opts.strategy = XTC_SUP_ONE_FOR_ALL;
	opts.max_restarts = 5;
	opts.period_ns = 1000LL * 1000 * 1000;

	memset(kids, 0, sizeof kids);
	kids[0].name = "a"; kids[0].fn = proc_a;
	kids[0].arg = (void *)(intptr_t)1;   /* crash once */
	kids[0].policy = XTC_RESTART_TRANSIENT;
	kids[1].name = "b"; kids[1].fn = proc_b_endless;
	kids[1].policy = XTC_RESTART_TRANSIENT;

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_sup_start(loop, &opts, kids, 2, &sup), ==, XTC_OK);
	g_sup_for_watcher = sup;
	munit_assert_int(xtc_proc_spawn(loop, watcher, NULL, NULL, &wpid),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	/* A: 2 spawns (initial + 1 restart).
	 * B: at least 2 spawns (one_for_all killed and restarted it
	 *    when A died). */
	munit_assert_int(atomic_load(&g_a_spawns), ==, 2);
	munit_assert_int(atomic_load(&g_b_spawns), >=, 2);
	munit_assert_int(xtc_sup_join(sup, 1000LL * 1000 * 1000), ==, XTC_OK);
	(void)xtc_loop_fini(loop);
	return MUNIT_OK;
}

static void
rest_watcher_proc(void *arg)
{
	void *m; size_t sz;
	(void)arg;
	(void)xtc_recv(&m, &sz, 100 * 1000 * 1000);
	if (m) __os_free(m);
	(void)xtc_sup_stop(g_sup_for_watcher);
}

/* ----- supervisor_SUITE: rest_for_one --------------------------- */

/* B (middle child) crashes; under rest_for_one, A is unaffected,
 * B and C restart.  Without xtc_sup_get_child_pid we can't crash B
 * externally; this test instead verifies the strategy enum is
 * accepted and the supervisor starts.  The full cascade is tested
 * by test/m10/test_sup.c::test_rest_for_one. */
static MunitResult
test_rest_for_one(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_supervisor_t *sup;
	xtc_sup_opts_t opts = XTC_SUP_OPTS_DEFAULT;
	xtc_child_spec_t kids[3];
	xtc_pid_t wpid;
	(void)p; (void)d;
	atomic_store(&g_a_spawns, 0);
	atomic_store(&g_c_spawns, 0);

	opts.strategy = XTC_SUP_REST_FOR_ONE;
	opts.max_restarts = 5;
	opts.period_ns = 1000LL * 1000 * 1000;

	memset(kids, 0, sizeof kids);
	kids[0].name = "a"; kids[0].fn = proc_a;
	kids[0].arg = (void *)(intptr_t)0;
	kids[0].policy = XTC_RESTART_TRANSIENT;
	kids[1].name = "b"; kids[1].fn = proc_a;
	kids[1].arg = (void *)(intptr_t)0;
	kids[1].policy = XTC_RESTART_TRANSIENT;
	kids[2].name = "c"; kids[2].fn = proc_c_endless;
	kids[2].policy = XTC_RESTART_TRANSIENT;

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_sup_start(loop, &opts, kids, 3, &sup), ==, XTC_OK);
	g_sup_for_watcher = sup;
	munit_assert_int(xtc_proc_spawn(loop, rest_watcher_proc, NULL, NULL,
	    &wpid), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	/* All children spawned exactly once (no crashes triggered). */
	munit_assert_int(atomic_load(&g_a_spawns), ==, 2);   /* A + B both call proc_a */
	munit_assert_int(atomic_load(&g_c_spawns), ==, 1);
	munit_assert_int(xtc_sup_join(sup, 1000LL * 1000 * 1000), ==, XTC_OK);
	(void)xtc_loop_fini(loop);
	return MUNIT_OK;
}

/* ----- supervisor_SUITE: abnormal_termination + restart -------- */

/* Permanent child: even normal exit triggers restart. */
static _Atomic int g_perm_spawns;

static void
perm_proc(void *arg)
{
	(void)arg;
	atomic_fetch_add(&g_perm_spawns, 1);
	/* Return normally. */
}

static void
perm_watcher(void *arg)
{
	xtc_supervisor_t *sup = arg;
	void *m; size_t sz;
	while (atomic_load(&g_perm_spawns) < 3) {
		(void)xtc_recv(&m, &sz, 5 * 1000 * 1000);
		if (m) __os_free(m);
	}
	(void)xtc_sup_stop(sup);
}

static MunitResult
test_permanent_restarts_on_normal_exit(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_supervisor_t *sup;
	xtc_sup_opts_t opts = XTC_SUP_OPTS_DEFAULT;
	xtc_child_spec_t kids[1];
	xtc_pid_t wpid;
	(void)p; (void)d;
	atomic_store(&g_perm_spawns, 0);
	opts.max_restarts = 10;
	opts.period_ns = 1000LL * 1000 * 1000;

	memset(kids, 0, sizeof kids);
	kids[0].name = "perm"; kids[0].fn = perm_proc;
	kids[0].policy = XTC_RESTART_PERMANENT;

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_sup_start(loop, &opts, kids, 1, &sup), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, perm_watcher, sup, NULL, &wpid),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(atomic_load(&g_perm_spawns), >=, 3);
	munit_assert_int(xtc_sup_join(sup, 1000LL * 1000 * 1000), ==, XTC_OK);
	(void)xtc_loop_fini(loop);
	return MUNIT_OK;
}

/* ----- supervisor_SUITE: temporary_bystander -------------------- */

/* TEMPORARY child never restarts even on abnormal exit. */
static _Atomic int g_temp_spawns;

static void
temp_proc(void *arg)
{
	(void)arg;
	atomic_fetch_add(&g_temp_spawns, 1);
	xtc_exit_self(99);
}

static void
temp_watcher(void *arg)
{
	xtc_supervisor_t *sup = arg;
	void *m; size_t sz;
	(void)xtc_recv(&m, &sz, 200 * 1000 * 1000);   /* settle */
	if (m) __os_free(m);
	(void)xtc_sup_stop(sup);
}

static MunitResult
test_temporary_no_restart(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_supervisor_t *sup;
	xtc_sup_opts_t opts = XTC_SUP_OPTS_DEFAULT;
	xtc_child_spec_t kids[1];
	xtc_pid_t wpid;
	(void)p; (void)d;
	atomic_store(&g_temp_spawns, 0);
	memset(kids, 0, sizeof kids);
	kids[0].name = "temp"; kids[0].fn = temp_proc;
	kids[0].policy = XTC_RESTART_TEMPORARY;

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_sup_start(loop, &opts, kids, 1, &sup), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, temp_watcher, sup, NULL, &wpid),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(atomic_load(&g_temp_spawns), ==, 1);
	munit_assert_int(xtc_sup_join(sup, 1000LL * 1000 * 1000), ==, XTC_OK);
	(void)xtc_loop_fini(loop);
	return MUNIT_OK;
}

/* ----- supervisor_SUITE: multiple_restarts hits intensity ------- */

static _Atomic int g_flap_count;

static void
fast_flap(void *arg) { (void)arg; atomic_fetch_add(&g_flap_count, 1); xtc_exit_self(1); }

static MunitResult
test_intensity_exceeded(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_supervisor_t *sup;
	xtc_sup_opts_t opts = XTC_SUP_OPTS_DEFAULT;
	xtc_child_spec_t kids[1];
	(void)p; (void)d;
	atomic_store(&g_flap_count, 0);
	opts.max_restarts = 3;
	opts.period_ns = 5LL * 1000 * 1000 * 1000;

	memset(kids, 0, sizeof kids);
	kids[0].name = "flap"; kids[0].fn = fast_flap;
	kids[0].policy = XTC_RESTART_PERMANENT;

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_sup_start(loop, &opts, kids, 1, &sup), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	munit_assert_int(xtc_sup_alive(sup), ==, 0);
	munit_assert_int(atomic_load(&g_flap_count), <=, opts.max_restarts + 2);
	munit_assert_int(xtc_sup_join(sup, 1000LL * 1000 * 1000), ==, XTC_OK);
	(void)xtc_loop_fini(loop);
	return MUNIT_OK;
}

/* ----- supervisor_SUITE: count_children ------------------------- */

static void
count_watcher(void *arg)
{
	xtc_supervisor_t *sup = arg;
	void *m; size_t sz;
	(void)xtc_recv(&m, &sz, 100 * 1000 * 1000);
	if (m) __os_free(m);
	(void)xtc_sup_stop(sup);
}

static void noop_endless(void *arg) { (void)arg;
	void *m; size_t sz;
	for(;;) { (void)xtc_recv(&m, &sz, 200*1000*1000); if (m) __os_free(m); }
}

static MunitResult
test_count_children(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_supervisor_t *sup;
	xtc_sup_opts_t opts = XTC_SUP_OPTS_DEFAULT;
	xtc_child_spec_t kids[5];
	xtc_pid_t wpid;
	int i;
	(void)p; (void)d;
	memset(kids, 0, sizeof kids);
	for (i = 0; i < 5; i++) {
		kids[i].name = "k";
		kids[i].fn = noop_endless;
		kids[i].policy = XTC_RESTART_TEMPORARY;
	}
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_sup_start(loop, &opts, kids, 5, &sup), ==, XTC_OK);
	munit_assert_int(xtc_sup_n_children(sup), ==, 5);
	munit_assert_int(xtc_proc_spawn(loop, count_watcher, sup, NULL, &wpid),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_sup_join(sup, 1000LL * 1000 * 1000), ==, XTC_OK);
	(void)xtc_loop_fini(loop);
	return MUNIT_OK;
}

/* ----- supervisor_SUITE phase-2: scale + hanging restart ------- */

static _Atomic int g_endless_alive;

static void
endless_with_counter(void *arg)
{
	void *m; size_t sz;
	(void)arg;
	atomic_fetch_add(&g_endless_alive, 1);
	for (;;) {
		int rc = xtc_recv(&m, &sz, 200LL * 1000 * 1000);
		if (rc == XTC_OK && m) __os_free(m);
		/* Loop until killed by supervisor stop (which sets the
		 * kill-pending flag and the next yield/recv park exits). */
	}
}

static void
stop_after_50ms(void *arg)
{
	xtc_supervisor_t *sup = arg;
	void *m; size_t sz;
	(void)xtc_recv(&m, &sz, 50LL * 1000 * 1000);
	if (m) __os_free(m);
	(void)xtc_sup_stop(sup);
}

static MunitResult
test_scale_start_stop_many(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_supervisor_t *sup;
	xtc_sup_opts_t opts = XTC_SUP_OPTS_DEFAULT;
	xtc_child_spec_t kids[50];
	xtc_pid_t wpid;
	int i;
	(void)p; (void)d;
	atomic_store(&g_endless_alive, 0);
	memset(kids, 0, sizeof kids);
	for (i = 0; i < 50; i++) {
		kids[i].name = "k";
		kids[i].fn = endless_with_counter;
		kids[i].policy = XTC_RESTART_TEMPORARY;
	}
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_sup_start(loop, &opts, kids, 50, &sup), ==, XTC_OK);
	munit_assert_int(xtc_sup_n_children(sup), ==, 50);

	munit_assert_int(xtc_proc_spawn(loop, stop_after_50ms, sup, NULL, &wpid),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_sup_join(sup, 2000LL * 1000 * 1000), ==, XTC_OK);
	munit_assert_int(atomic_load(&g_endless_alive), ==, 50);
	(void)xtc_loop_fini(loop);
	return MUNIT_OK;
}

/* hanging_restart_loop: a permanent worker that exits immediately,
 * causing the supervisor to keep restarting until intensity is hit
 * and the supervisor itself dies.  Verify n_restarts is bounded. */

static _Atomic int g_quick_exit_count;

static void
quick_exit_worker(void *arg)
{
	(void)arg;
	atomic_fetch_add(&g_quick_exit_count, 1);
	/* Return immediately (which the supervisor sees as a normal
	 * exit and — because policy is permanent — restarts). */
}

static MunitResult
test_hanging_restart_loop(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_supervisor_t *sup;
	xtc_sup_opts_t opts = XTC_SUP_OPTS_DEFAULT;
	xtc_child_spec_t kids[1];
	int n_restarts;
	(void)p; (void)d;
	atomic_store(&g_quick_exit_count, 0);
	opts.max_restarts = 5;
	opts.period_ns = 5LL * 1000 * 1000 * 1000;
	memset(kids, 0, sizeof kids);
	kids[0].name = "qe"; kids[0].fn = quick_exit_worker;
	kids[0].policy = XTC_RESTART_PERMANENT;

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_sup_start(loop, &opts, kids, 1, &sup), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	munit_assert_int(xtc_sup_alive(sup), ==, 0);
	n_restarts = xtc_sup_n_restarts(sup);
	/* Bounded: at most max_restarts + a small slack for race. */
	munit_assert_int(n_restarts, <=, opts.max_restarts + 2);
	munit_assert_int(atomic_load(&g_quick_exit_count), >=, 1);
	munit_assert_int(atomic_load(&g_quick_exit_count), <=,
	    opts.max_restarts + 3);
	munit_assert_int(xtc_sup_join(sup, 1000LL * 1000 * 1000), ==, XTC_OK);
	(void)xtc_loop_fini(loop);
	return MUNIT_OK;
}

/* count_restarting_children: while children are restarting, the
 * n_children API should still report the configured count. */

static _Atomic int g_oneshot_starts;

static void
slow_oneshot(void *arg)
{
	void *m; size_t sz;
	(void)arg;
	atomic_fetch_add(&g_oneshot_starts, 1);
	(void)xtc_recv(&m, &sz, 30LL * 1000 * 1000);
	if (m) __os_free(m);
	/* exit normally — supervisor will restart if permanent */
}

static void
stop_after_long(void *arg)
{
	xtc_supervisor_t *sup = arg;
	void *m; size_t sz;
	(void)xtc_recv(&m, &sz, 250LL * 1000 * 1000);
	if (m) __os_free(m);
	(void)xtc_sup_stop(sup);
}

static MunitResult
test_count_restarting_children(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_supervisor_t *sup;
	xtc_sup_opts_t opts = XTC_SUP_OPTS_DEFAULT;
	xtc_child_spec_t kids[3];
	xtc_pid_t wpid;
	int i;
	(void)p; (void)d;
	atomic_store(&g_oneshot_starts, 0);
	opts.max_restarts = 100;
	opts.period_ns = 5LL * 1000 * 1000 * 1000;
	memset(kids, 0, sizeof kids);
	for (i = 0; i < 3; i++) {
		kids[i].name = "kr";
		kids[i].fn = slow_oneshot;
		kids[i].policy = XTC_RESTART_PERMANENT;
	}
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_sup_start(loop, &opts, kids, 3, &sup), ==, XTC_OK);
	munit_assert_int(xtc_sup_n_children(sup), ==, 3);

	munit_assert_int(xtc_proc_spawn(loop, stop_after_long, sup, NULL, &wpid),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	/* Even after multiple restart cycles, the configured child
	 * count is stable. */
	munit_assert_int(xtc_sup_n_children(sup), ==, 3);
	/* Each child started at least once. */
	munit_assert_int(atomic_load(&g_oneshot_starts), >=, 3);
	munit_assert_int(xtc_sup_join(sup, 2000LL * 1000 * 1000), ==, XTC_OK);
	(void)xtc_loop_fini(loop);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/one_for_one_basic",            test_one_for_one_basic,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/one_for_all",                  test_one_for_all,               NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/rest_for_one_strategy_accepts", test_rest_for_one,             NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/permanent_restarts_on_normal", test_permanent_restarts_on_normal_exit, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/temporary_no_restart",         test_temporary_no_restart,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/intensity_exceeded",           test_intensity_exceeded,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/count_children",               test_count_children,            NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/scale_start_stop_many",        test_scale_start_stop_many,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/hanging_restart_loop",         test_hanging_restart_loop,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/count_restarting_children",    test_count_restarting_children, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/otp/supervisor", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
