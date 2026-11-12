/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m10/test_sup.c -- verifies M10 supervisor.
 */

#include <pthread.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_orc.h"
#include "xtc_inspect.h"
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
	kids[0].size = sizeof kids[0];
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
	kids[0].size = sizeof kids[0];
	kids[1].size = sizeof kids[1];
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
	kids[0].size = sizeof kids[0];
	kids[1].size = sizeof kids[1];
	kids[2].size = sizeof kids[2];
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
	kids[0].size = sizeof kids[0];
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

/* SIMPLE_ONE_FOR_ONE: a 0-child pool grows on demand via
 * xtc_sup_add_child; TEMPORARY children are reclaimed (not restarted)
 * when they exit -- the session-pool policy. */
static int g_sofo_ran;
static void
sofo_session(void *arg)
{
	(void)arg;
	xtc_atomic_fetch_add_i32(&g_sofo_ran, 1,
	    xtc_mo_seq_cst); /* run once, then exit */
}
struct sofo_state { xtc_supervisor_t *sup; int added; };
static void
sofo_manager(void *arg)
{
	struct sofo_state *s = arg;
	xtc_child_spec_t spec;
	xtc_pid_t pid;
	void *m; size_t n;
	int i;
	for (i = 0; i < 3; i++) {
		memset(&spec, 0, sizeof spec);
		spec.size = sizeof spec;
		spec.name = "session";
		spec.fn = sofo_session;
		spec.policy = XTC_RESTART_TEMPORARY;
		if (xtc_sup_add_child(s->sup, &spec, &pid) == XTC_OK &&
		    !xtc_pid_is_none(pid))
			s->added++;
	}
	/* Yield so the spawned sessions run to completion. */
	(void)xtc_recv(&m, &n, 200LL * 1000 * 1000);
	(void)xtc_sup_stop(s->sup);
}

static MunitResult
test_simple_one_for_one(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_sup_opts_t opts = XTC_SUP_OPTS_DEFAULT;
	xtc_supervisor_t *sup = NULL;
	struct sofo_state st = { NULL, 0 };
	xtc_proc_opts_t popts = XTC_PROC_OPTS_INIT;
	xtc_pid_t mgr;
	(void)p; (void)d;

	xtc_atomic_store_i32(&g_sofo_ran, 0, xtc_mo_seq_cst);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	opts.strategy = XTC_SUP_SIMPLE_OFO;
	/* Start with zero children -- the pool is filled dynamically. */
	munit_assert_int(xtc_sup_start(loop, &opts, NULL, 0, &sup), ==, XTC_OK);
	st.sup = sup;
	popts.name = "mgr";
	munit_assert_int(xtc_proc_spawn(loop, sofo_manager, &st, &popts, &mgr),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_sup_join(sup, 1LL * 1000 * 1000 * 1000), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);

	munit_assert_int(st.added, ==, 3);            /* 3 spawned on demand */
	munit_assert_int(xtc_atomic_load_i32(&g_sofo_ran, xtc_mo_seq_cst), ==,
	    3); /* all ran */
	return MUNIT_OK;
}

/* Bounded pool: max_children caps live dynamic children.  A manager
 * spawns long-lived (permanent, blocking) children up to the cap, then
 * one more, which must be rejected with XTC_E_RESOURCE. */
static int g_pool_alive;
static void
pool_worker(void *arg)
{
	void *m; size_t n;
	(void)arg;
	xtc_atomic_fetch_add_i32(&g_pool_alive, 1, xtc_mo_seq_cst);
	(void)xtc_recv(&m, &n, 10LL * 1000 * 1000 * 1000);  /* block, stay alive */
	xtc_atomic_fetch_sub_i32(&g_pool_alive, 1, xtc_mo_seq_cst);
}
struct pool_state { xtc_supervisor_t *sup; int over_cap_rc; int live_at_cap; };
static void
pool_manager(void *arg)
{
	struct pool_state *s = arg;
	xtc_child_spec_t spec;
	xtc_pid_t pid;
	int i, rc;
	memset(&spec, 0, sizeof spec);
	spec.size = sizeof spec;
	spec.name = "pw";
	spec.fn = pool_worker;
	spec.policy = XTC_RESTART_PERMANENT;
	for (i = 0; i < 2; i++)
		(void)xtc_sup_add_child(s->sup, &spec, &pid);   /* fill the cap (2) */
	xtc_proc_sleep(20LL * 1000 * 1000);                 /* let them go alive */
	s->live_at_cap = xtc_sup_n_alive(s->sup);
	rc = xtc_sup_add_child(s->sup, &spec, &pid);        /* over the cap */
	s->over_cap_rc = rc;
	(void)xtc_sup_stop(s->sup);
}
static MunitResult
test_pool_max_children(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_sup_opts_t opts = XTC_SUP_OPTS_DEFAULT;
	xtc_supervisor_t *sup = NULL;
	struct pool_state st = { NULL, XTC_OK, 0 };
	xtc_proc_opts_t popts = XTC_PROC_OPTS_INIT;
	(void)p; (void)d;
	xtc_atomic_store_i32(&g_pool_alive, 0, xtc_mo_seq_cst);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	opts.strategy = XTC_SUP_SIMPLE_OFO;
	opts.max_children = 2;
	munit_assert_int(xtc_sup_start(loop, &opts, NULL, 0, &sup), ==, XTC_OK);
	st.sup = sup;
	munit_assert_int(xtc_proc_spawn(loop, pool_manager, &st, &popts, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(st.live_at_cap, ==, 2);
	munit_assert_int(st.over_cap_rc, ==, XTC_E_RESOURCE);
	munit_assert_int(xtc_sup_join(sup, 1LL * 1000 * 1000 * 1000), ==, XTC_OK);
	(void)xtc_loop_fini(loop);
	return MUNIT_OK;
}


/*
 * The child spawn must be ATOMIC with establishing the monitor.
 *
 * xtc_proc_spawn followed by xtc_monitor leaves a window in which the child is
 * runnable but unmonitored.  A child that finishes inside it is reaped before
 * the monitor lands, so the DOWN arrives as XTC_DOWN_NOPROC and the REAL reason
 * is gone -- a fault becomes indistinguishable from "raced an already-dead
 * target", and because NOPROC is nonzero it also made the TRANSIENT policy
 * restart children that had exited cleanly.
 *
 * A supervisor that cannot tell a crash from a benign race cannot implement
 * "a crashed child means state may be corrupt, so fail-stop", which is the
 * whole reason to have one.
 *
 * This asserts the observable consequence rather than the mechanism: a child
 * that exits IMMEDIATELY and CLEANLY must produce reason 0, never NOPROC.
 * Proven non-vacuous out of band -- reverting __spawn_child to
 * spawn-then-monitor and widening the window 5 ms turns this from reason=0 into
 * reason=-100000 in 40 of 40 runs on a SINGLE loop, so no OS-thread race is
 * needed to hit it.
 */
static int g_atomic_mon_starts;

static void
spawnmon_child(void *arg)
{
	(void)arg;
	(void)xtc_atomic_fetch_add_i32(&g_atomic_mon_starts, 1, xtc_mo_seq_cst);
	(void)xtc_exit_self(0);      /* clean, and as fast as possible */
}

static xtc_supervisor_t *g_atomic_mon_sup;

/* Let the child run and die, then ask the supervisor to stop.  Stopping from
 * a proc (not the test function) is the existing pattern in this file; the
 * FREE happens via xtc_sup_join below, which must be called from OUTSIDE the
 * loop thread. */
static void
spawnmon_watch(void *arg)
{
	(void)arg;
	(void)xtc_proc_sleep(300000000LL);
	(void)xtc_sup_stop(g_atomic_mon_sup);
}

static MunitResult
test_atomic_spawn_monitor(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_supervisor_t *sup = NULL;
	xtc_sup_opts_t so;
	xtc_child_spec_t cs;
	(void)p; (void)d;

	xtc_atomic_store_i32(&g_atomic_mon_starts, 0, xtc_mo_seq_cst);
	memset(&so, 0, sizeof so);
	so.size = sizeof so;
	so.strategy = XTC_SUP_ONE_FOR_ONE;
	/*
	 * TRANSIENT plus a restart budget is the shape that shows the bug: if
	 * the clean exit is misreported as NOPROC (nonzero), __should_restart
	 * treats it as abnormal and restarts a child that finished normally.
	 */
	so.max_restarts = 5;
	so.period_ns = 2000000000LL;
	memset(&cs, 0, sizeof cs);
	cs.size = sizeof cs;
	cs.name = "atomic_mon";
	cs.fn = spawnmon_child;
	cs.policy = XTC_RESTART_TRANSIENT;

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_sup_start(loop, &so, &cs, 1, &sup), ==, XTC_OK);
	g_atomic_mon_sup = sup;
	munit_assert_int(xtc_proc_spawn(loop, spawnmon_watch, NULL, NULL, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	/*
	 * Exactly ONE start.  A second means the clean exit was classified as
	 * abnormal and the child was restarted -- the OTP transient violation
	 * this guards.
	 */
	munit_assert_int(xtc_atomic_load_i32(&g_atomic_mon_starts,
	                     xtc_mo_seq_cst),
	    ==, 1);
	munit_assert_int(xtc_sup_join(sup, 1LL * 1000 * 1000 * 1000), ==,
	    XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/*
 * PLAN 19.27.4: a group restart must NOT run a replacement child while its
 * killed predecessor is still cleaning up.
 *
 * Each "holder" child takes a shared resource on entry and releases it
 * from an AT-EXIT hook that deliberately takes 30ms -- the window in which
 * the old code (fire-and-forget kill, mark dead, respawn immediately)
 * started the replacement beside the still-exiting predecessor.  held_max
 * is the highest number of holders ever alive at once; with two holders
 * the steady state is 2, and anything above it is an OLD+NEW overlap.
 *
 * Fails on the unfixed library (held_max == 4) and passes with the fix.
 */
static int g_ov_held, g_ov_max, g_ov_starts, g_ov_crashes;

static void
ov_release(void *a)
{
	(void)a;
	/* Slow cleanup: the predecessor is still inside its exit hook, still
	 * holding the resource, for 30ms after it was told to exit. */
	(void)xtc_proc_sleep(30LL * 1000 * 1000);
	xtc_atomic_fetch_sub_i32(&g_ov_held, 1, xtc_mo_seq_cst);
}

static void
ov_holder(void *a)
{
	int cur, m;
	(void)a;
	xtc_atomic_fetch_add_i32(&g_ov_starts, 1, xtc_mo_seq_cst);
	cur = xtc_atomic_fetch_add_i32(&g_ov_held, 1, xtc_mo_seq_cst) + 1;
	do {
		m = xtc_atomic_load_i32(&g_ov_max, xtc_mo_seq_cst);
	} while (cur > m &&
	    !xtc_atomic_cas_weak_i32(&g_ov_max, &m, cur, xtc_mo_seq_cst,
	        xtc_mo_seq_cst));
	(void)xtc_proc_at_exit(ov_release, NULL);
	for (;;)
		(void)xtc_proc_sleep(1LL * 1000 * 1000);
}

static void
ov_crasher(void *a)
{
	(void)a;
	(void)xtc_proc_sleep(20LL * 1000 * 1000);
	if (xtc_atomic_fetch_add_i32(&g_ov_crashes, 1, xtc_mo_seq_cst) == 0)
		(void)xtc_exit_self(5);            /* crash exactly once */
	for (;;)
		(void)xtc_proc_sleep(1LL * 1000 * 1000);
}

static void
ov_stopper(void *a)
{
	(void)xtc_loop_stop((xtc_loop_t *)a);
}

static MunitResult
run_overlap_case(xtc_restart_strategy_t strategy)
{
	xtc_loop_t *loop = NULL;
	xtc_supervisor_t *sup = NULL;
	xtc_sup_opts_t o = XTC_SUP_OPTS_DEFAULT;
	xtc_child_spec_t cs[3];
	int i;

	xtc_atomic_store_i32(&g_ov_held, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_ov_max, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_ov_starts, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_ov_crashes, 0, xtc_mo_seq_cst);

	o.strategy = strategy;
	o.max_restarts = 5;
	memset(cs, 0, sizeof cs);
	cs[0].size = sizeof cs[0];
	cs[0].fn = ov_crasher; cs[0].name = "crasher";
	for (i = 1; i < 3; i++) { cs[i].size = sizeof cs[i]; cs[i].fn = ov_holder; cs[i].name = "holder"; }

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_sup_start(loop, &o, cs, 3, &sup), ==, XTC_OK);
	munit_assert_int(xtc_timer_set(loop, 400LL * 1000 * 1000, ov_stopper,
	    loop, NULL), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	/* The restart must actually have HAPPENED -- otherwise "no overlap"
	 * would be vacuous.  Two holders started, then both were replaced. */
	munit_assert_int(xtc_atomic_load_i32(&g_ov_crashes, xtc_mo_seq_cst), >=,
	    1);
	munit_assert_int(xtc_atomic_load_i32(&g_ov_starts, xtc_mo_seq_cst), >=,
	    4);
	/* The property: never more holders alive than the steady state. */
	munit_assert_int(xtc_atomic_load_i32(&g_ov_max, xtc_mo_seq_cst), <=, 2);

	(void)xtc_sup_stop(sup);
	(void)xtc_timer_set(loop, 300LL * 1000 * 1000, ov_stopper, loop, NULL);
	(void)xtc_loop_run(loop);
	/* The sup exited during that drain; join frees it (poll, it is done). */
	munit_assert_int(xtc_sup_join(sup, 0), ==, XTC_OK);
	(void)xtc_loop_fini(loop);
	return MUNIT_OK;
}

static MunitResult
test_one_for_all_no_overlap(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	return run_overlap_case(XTC_SUP_ONE_FOR_ALL);
}

static MunitResult
test_rest_for_one_no_overlap(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	return run_overlap_case(XTC_SUP_REST_FOR_ONE);
}

/* ---- xtc_sup_join on a supervisor that is still running ----
 *
 * A join that times out must NOT free the supervisor.  The joiner runs on
 * a foreign thread while the loop runs the supervisor; it joins with a
 * 50ms timeout (the sup will not stop by itself), and must get
 * XTC_E_AGAIN with the supervisor still alive.  It then stops the sup and
 * a second join succeeds.  Before the fix the first join returned XTC_OK
 * and freed sup under the running supervisor proc (ASan: heap-use-after-
 * free in __sup_entry); without ASan the rc assertion bites.
 */
static xtc_supervisor_t *g_join_sup;
static int g_join_rc1 = 12345;
static int g_join_alive_after = -1;
static int g_join_done;

/* Watchdog: on the unfixed library the first join returns XTC_OK (having
 * freed the sup), so nothing ever stops the PERMANENT worker and the loop
 * would run forever.  Stop the loop after 3s so that case FAILS on the rc
 * assertion instead of hanging; on the fixed path it exits as soon as the
 * joiner is done, adding no delay. */
static void
join_watchdog(void *a)
{
	int i;
	for (i = 0;
	     i < 300 && !xtc_atomic_load_i32(&g_join_done, xtc_mo_seq_cst); i++)
		(void)xtc_proc_sleep(10LL * 1000 * 1000);
	if (!xtc_atomic_load_i32(&g_join_done, xtc_mo_seq_cst))
		(void)xtc_loop_stop((xtc_loop_t *)a);
}

static void
join_worker(void *a)
{
	(void)a;
	for (;;)
		(void)xtc_proc_sleep(5LL * 1000 * 1000);
}

static void *
join_thread(void *a)
{
	struct timespec ts = { 0, 100 * 1000 * 1000 };
	(void)a;
	nanosleep(&ts, NULL);                        /* sup is running */
	g_join_rc1 = xtc_sup_join(g_join_sup, 50LL * 1000 * 1000);
	if (g_join_rc1 != XTC_OK) {
		g_join_alive_after = xtc_sup_alive(g_join_sup);
		(void)xtc_sup_stop(g_join_sup);          /* then the loop drains */
	}
	xtc_atomic_store_i32(&g_join_done, 1, xtc_mo_seq_cst);
	return NULL;
}

static MunitResult
test_join_timeout_keeps_sup(const MunitParameter p[], void *d)
{
	xtc_sup_opts_t opts = XTC_SUP_OPTS_DEFAULT;
	xtc_child_spec_t kids[1];
	xtc_loop_t *loop = NULL;
	pthread_t t;
	(void)p; (void)d;

	memset(kids, 0, sizeof kids);
	kids[0].size = sizeof kids[0];
	kids[0].name = "w";
	kids[0].fn = join_worker;
	kids[0].policy = XTC_RESTART_PERMANENT;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_sup_start(loop, &opts, kids, 1, &g_join_sup),
	    ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, join_watchdog, loop, NULL, NULL),
	    ==, XTC_OK);
	munit_assert_int(pthread_create(&t, NULL, join_thread, NULL), ==, 0);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	(void)pthread_join(t, NULL);

	munit_assert_int(g_join_rc1, ==, XTC_E_AGAIN);   /* not freed */
	munit_assert_int(g_join_alive_after, ==, 1);     /* still running */
	munit_assert_int(xtc_sup_join(g_join_sup, 1LL * 1000 * 1000 * 1000),
	    ==, XTC_OK);                                 /* now it is */
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

#include <pthread.h>

/* Through 2.1 PIDs were executor-relative: every standalone loop encoded
 * loop_id 0, so a proc on an UNRELATED standalone loop could carry a pid
 * equal to ours.  Since 2.2 the loop ids differ, but the bystander still
 * holds our (local_id, gen) -- the part a 2.1 pid could not tell apart.
 * seq_by is that bystander: it counts messages it receives and records
 * whether it exited normally (a kill aimed at the wrong proc stops it). */
static int g_seq_stop, g_seq_by_msgs, g_seq_by_normal;
static void
seq_by(void *arg)
{
	void *m;
	size_t s;
	(void)arg;
	while (!xtc_atomic_load_i32(&g_seq_stop, xtc_mo_seq_cst)) {
		m = NULL;
		if (xtc_recv(&m, &s, 1000000) == XTC_OK) {
			xtc_atomic_fetch_add_i32(&g_seq_by_msgs, 1,
			    xtc_mo_seq_cst);
			xtc_free(m);
		}
	}
	xtc_atomic_fetch_add_i32(&g_seq_by_normal, 1, xtc_mo_seq_cst);
}
static void *
seq_loop_thread(void *arg)
{
	(void)xtc_loop_run(arg);
	return NULL;
}
/* Spawn n bystanders on a fresh loop b (registered FIRST, so an off-loop
 * pid-only lookup also prefers it) and run it on its own thread. */
static void
seq_by_start(xtc_loop_t **b, xtc_pid_t *by, int n, pthread_t *th)
{
	int i;
	xtc_atomic_store_i32(&g_seq_stop, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_seq_by_msgs, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_seq_by_normal, 0, xtc_mo_seq_cst);
	munit_assert_int(xtc_loop_init(b), ==, XTC_OK);
	for (i = 0; i < n; i++)
		munit_assert_int(xtc_proc_spawn(*b, seq_by, NULL, NULL, &by[i]),
		    ==, XTC_OK);
	munit_assert_int(pthread_create(th, NULL, seq_loop_thread, *b), ==, 0);
}
/* A bystander in p's slot (local_id, gen) on another loop. */
static int
seq_has_equal(const xtc_pid_t *by, int n, xtc_pid_t p)
{
	int i;
	for (i = 0; i < n; i++)
		if (by[i].local_id == p.local_id && by[i].gen == p.gen &&
		    by[i].loop_id != p.loop_id)
			return 1;
	return 0;
}

static int g_seq_a_spawns, g_seq_b_spawns;
static xtc_pid_t g_seq_a_pid0;
static void
seq_a(void *arg)
{
	void *m;
	size_t s;
	(void)arg;
	if (xtc_atomic_fetch_add_i32(&g_seq_a_spawns, 1, xtc_mo_seq_cst) == 0) {
		g_seq_a_pid0 = xtc_self();
		(void)xtc_proc_sleep(20LL * 1000 * 1000);
		xtc_exit_self(7);
	}
	for (;;) {
		m = NULL;
		(void)xtc_recv(&m, &s, 200LL * 1000 * 1000);
		if (m) xtc_free(m);
	}
}
static void
seq_b(void *arg)
{
	void *m;
	size_t s;
	(void)arg;
	xtc_atomic_fetch_add_i32(&g_seq_b_spawns, 1, xtc_mo_seq_cst);
	for (;;) {
		m = NULL;
		(void)xtc_recv(&m, &s, 200LL * 1000 * 1000);
		if (m) xtc_free(m);
	}
}
static void
seq_watch(void *arg)
{
	xtc_supervisor_t *sup = arg;
	int64_t t0 = xtc_clock_mono();
	while (xtc_atomic_load_i32(&g_seq_b_spawns, xtc_mo_seq_cst) < 2 &&
	    xtc_sup_alive(sup) &&
	    xtc_clock_mono() - t0 < 3LL * 1000 * 1000 * 1000)
		(void)xtc_proc_sleep(5LL * 1000 * 1000);
	(void)xtc_sup_stop(sup);
}
/* one_for_all waits for the old generation to LEAVE the proc table before
 * respawning.  A pid-only presence check saw the bystander on loop B with
 * the crashed child's pid as "still cleaning up", waited out the 5 s
 * cleanup bound and STOPPED the supervisor instead of restarting. */
static MunitResult
test_one_for_all_equal_pid_other_loop(const MunitParameter p[], void *d)
{
	xtc_loop_t *la = NULL, *lb = NULL;
	xtc_supervisor_t *sup;
	xtc_sup_opts_t opts = XTC_SUP_OPTS_DEFAULT;
	xtc_child_spec_t kids[2];
	xtc_pid_t by[8], wpid;
	pthread_t th;
	(void)p; (void)d;

	xtc_atomic_store_i32(&g_seq_a_spawns, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_seq_b_spawns, 0, xtc_mo_seq_cst);
	seq_by_start(&lb, by, 8, &th);
	opts.strategy = XTC_SUP_ONE_FOR_ALL;
	opts.max_restarts = 5;
	opts.period_ns = 1000LL * 1000 * 1000;
	memset(kids, 0, sizeof kids);
	kids[0].size = sizeof kids[0];
	kids[1].size = sizeof kids[1];
	kids[0].name = "a"; kids[0].fn = seq_a; kids[0].policy = XTC_RESTART_TRANSIENT;
	kids[1].name = "b"; kids[1].fn = seq_b; kids[1].policy = XTC_RESTART_TRANSIENT;
	munit_assert_int(xtc_loop_init(&la), ==, XTC_OK);
	munit_assert_int(xtc_sup_start(la, &opts, kids, 2, &sup), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(la, seq_watch, sup, NULL, &wpid),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(la), ==, XTC_OK);
	xtc_atomic_store_i32(&g_seq_stop, 1, xtc_mo_seq_cst);
	munit_assert_int(pthread_join(th, NULL), ==, 0);

	munit_assert_true(seq_has_equal(by, 8, g_seq_a_pid0));   /* premise */
	munit_assert_int(xtc_atomic_load_i32(&g_seq_b_spawns, xtc_mo_seq_cst),
	    >=, 2); /* restarted */
	munit_assert_int(xtc_atomic_load_i32(&g_seq_by_normal, xtc_mo_seq_cst),
	    ==, 8); /* none killed */
	munit_assert_int(xtc_sup_join(sup, 1LL * 1000 * 1000 * 1000), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(la), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(lb), ==, XTC_OK);
	return MUNIT_OK;
}

/* xtc_sup_add_child called from a proc on ANOTHER standalone loop.  The
 * request (to the supervisor) and the reply (to the caller) are pid sends;
 * resolved by pid alone, each lands on whichever loop the resolver picks
 * for loop_id 0.  The caller must get its child, and no bystander with an
 * equal pid may receive either message. */
struct xadd { xtc_supervisor_t *sup; int rc, added; };
static void
xadd_caller(void *arg)
{
	struct xadd *x = arg;
	xtc_child_spec_t spec;
	xtc_pid_t pid = XTC_PID_NONE;
	memset(&spec, 0, sizeof spec);
	spec.size = sizeof spec;
	spec.name = "xsession";
	spec.fn = sofo_session;
	spec.policy = XTC_RESTART_TEMPORARY;
	x->rc = xtc_sup_add_child(x->sup, &spec, &pid);
	x->added = x->rc == XTC_OK && !xtc_pid_is_none(pid);
	(void)xtc_sup_stop(x->sup);
}
static void *
xadd_sup_thread(void *arg)
{
	(void)xtc_loop_run(arg);
	return NULL;
}
static MunitResult
test_add_child_other_loop(const MunitParameter p[], void *d)
{
	xtc_loop_t *ls = NULL, *lc = NULL, *lb = NULL;
	xtc_sup_opts_t opts = XTC_SUP_OPTS_DEFAULT;
	xtc_supervisor_t *sup = NULL;
	struct xadd x = { NULL, -1, 0 };
	xtc_pid_t by[8], cpid;
	pthread_t thb, ths;
	(void)p; (void)d;

	xtc_atomic_store_i32(&g_sofo_ran, 0, xtc_mo_seq_cst);
	seq_by_start(&lb, by, 8, &thb);      /* bystanders, registered first */
	munit_assert_int(xtc_loop_init(&ls), ==, XTC_OK);
	opts.strategy = XTC_SUP_SIMPLE_OFO;
	munit_assert_int(xtc_sup_start(ls, &opts, NULL, 0, &sup), ==, XTC_OK);
	munit_assert_int(xtc_loop_init(&lc), ==, XTC_OK);
	x.sup = sup;
	munit_assert_int(xtc_proc_spawn(lc, xadd_caller, &x, NULL, &cpid),
	    ==, XTC_OK);
	munit_assert_int(pthread_create(&ths, NULL, xadd_sup_thread, ls), ==, 0);
	munit_assert_int(xtc_loop_run(lc), ==, XTC_OK);
	munit_assert_int(pthread_join(ths, NULL), ==, 0);
	xtc_atomic_store_i32(&g_seq_stop, 1, xtc_mo_seq_cst);
	munit_assert_int(pthread_join(thb, NULL), ==, 0);

	munit_assert_true(seq_has_equal(by, 8, cpid));            /* premise */
	munit_assert_int(x.rc, ==, XTC_OK);
	munit_assert_int(x.added, ==, 1);
	munit_assert_int(xtc_atomic_load_i32(&g_sofo_ran, xtc_mo_seq_cst), ==,
	    1);
	munit_assert_int(xtc_atomic_load_i32(&g_seq_by_msgs, xtc_mo_seq_cst),
	    ==, 0); /* no stray */
	munit_assert_int(xtc_sup_join(sup, 1LL * 1000 * 1000 * 1000), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(lc), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(ls), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(lb), ==, XTC_OK);
	return MUNIT_OK;
}

/* E-m1: SIMPLE_OFO reclaims dynamic children that exit for good.
 * 2000 TEMPORARY children that return at once, plus 200 TRANSIENT ones
 * that exit normally.  Before 1.55.1 every one kept a slot: n_children
 * ended at 2200 with 0 alive (and add/DOWN cost grew with it). */
static void sofo_quick(void *a) { (void)a; }
struct reclaim_state {
	xtc_supervisor_t *sup;
	int fails;
	int n_children_end;
	int n_alive_end;
};
static void
reclaim_driver(void *arg)
{
	struct reclaim_state *s = arg;
	xtc_child_spec_t cs;
	int i, spins;
	memset(&cs, 0, sizeof cs);
	cs.size = sizeof cs;
	cs.name = "tmp";
	cs.fn = sofo_quick;
	for (i = 0; i < 2200; i++) {
		cs.policy = (i < 2000) ? XTC_RESTART_TEMPORARY
		                       : XTC_RESTART_TRANSIENT;
		if (xtc_sup_add_child(s->sup, &cs, NULL) != XTC_OK) s->fails++;
		if (i % 64 == 0) (void)xtc_proc_sleep(100 * 1000);
	}
	/* Let the last DOWNs drain. */
	for (spins = 0; spins < 200 && xtc_sup_n_children(s->sup) > 0; spins++)
		(void)xtc_proc_sleep(1000 * 1000);
	s->n_children_end = xtc_sup_n_children(s->sup);
	s->n_alive_end = xtc_sup_n_alive(s->sup);
	(void)xtc_sup_stop(s->sup);
}

static MunitResult
test_simple_ofo_reclaims_dead(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_sup_opts_t opts = XTC_SUP_OPTS_DEFAULT;
	xtc_supervisor_t *sup = NULL;
	struct reclaim_state st;
	(void)p; (void)d;

	memset(&st, 0, sizeof st);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	opts.strategy = XTC_SUP_SIMPLE_OFO;
	opts.max_restarts = 1000000;
	munit_assert_int(xtc_sup_start(loop, &opts, NULL, 0, &sup), ==, XTC_OK);
	st.sup = sup;
	munit_assert_int(xtc_proc_spawn(loop, reclaim_driver, &st, NULL, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_sup_join(sup, 2LL * 1000 * 1000 * 1000), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);

	munit_assert_int(st.fails, ==, 0);
	munit_assert_int(st.n_alive_end, ==, 0);
	munit_assert_int(st.n_children_end, ==, 0);
	return MUNIT_OK;
}

/*
 * A2 (otp probe F): xtc_sup_n_alive from a proc on the supervisor's loop
 * while a one_for_all restart waits for a masked child's cleanup.  2.1.1
 * held sup->lock (a pthread mutex) across that wait, which sleeps; the
 * same-loop caller blocked the loop thread in pthread_mutex_lock and the
 * supervisor that would release it never ran again (probe: killed by
 * timeout(1) after 12 s).  The run is bounded by an alarm so the
 * regression fails rather than hanging make check.
 */
#define NA_MS (1000LL * 1000)
static int g_na_n0, g_na_rc = -999;
static int64_t g_na_ns = -1;
static xtc_supervisor_t *g_na_sup;

static void
na_k0(void *a)
{
	(void)a;
	if (xtc_atomic_fetch_add_i32(&g_na_n0, 1, xtc_mo_seq_cst) == 0) {
		(void)xtc_proc_sleep(20 * NA_MS);
		(void)xtc_exit_self(4); /* crash once: one_for_all restart */
	}
	(void)xtc_proc_sleep(500 * NA_MS);
}

static void
na_k1(void *a)
{
	(void)a;
	/* Masked: the restart's kill is deferred, so the supervisor sits in
	 * its cleanup wait for the whole 600 ms. */
	(void)xtc_mask_enter();
	(void)xtc_proc_sleep(600 * NA_MS);
	(void)xtc_mask_leave();
}

static void
na_observer(void *a)
{
	int64_t t0;
	(void)a;
	(void)xtc_proc_sleep(100 * NA_MS); /* inside the restart wait */
	t0 = xtc_clock_mono();
	xtc_atomic_store_i32(&g_na_rc, xtc_sup_n_alive(g_na_sup),
	    xtc_mo_seq_cst);
	xtc_atomic_store_i64(&g_na_ns, xtc_clock_mono() - t0, xtc_mo_seq_cst);
	(void)xtc_sup_stop(g_na_sup);
}

#if !defined(_WIN32)
static void
na_alarm(int sig)
{
	(void)sig;
	_exit(9); /* the loop thread is wedged in pthread_mutex_lock */
}
#endif

static MunitResult
test_n_alive_during_group_restart(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_child_spec_t ch[2] = { XTC_CHILD_SPEC_INIT, XTC_CHILD_SPEC_INIT };
	xtc_sup_opts_t so = XTC_SUP_OPTS_DEFAULT;
	xtc_pid_t pid;
	(void)p;
	(void)d;
	xtc_atomic_store_i32(&g_na_n0, 0, xtc_mo_seq_cst);
	ch[0].fn = na_k0;
	ch[0].policy = XTC_RESTART_PERMANENT;
	ch[1].fn = na_k1;
	ch[1].policy = XTC_RESTART_TEMPORARY;
	so.strategy = XTC_SUP_ONE_FOR_ALL;
#if !defined(_WIN32)
	/* A wedged loop thread hangs here: fail in 10 s, not at the CI
	 * job timeout.  (No SIGALRM on Windows: the job's timeout bounds it.) */
	(void)signal(SIGALRM, na_alarm);
	(void)alarm(10);
#endif
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_sup_start(loop, &so, ch, 2, &g_na_sup), ==,
	    XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, na_observer, NULL, NULL, &pid),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
#if !defined(_WIN32)
	(void)alarm(0);
#endif
	/* It returned, at once: no park, no lock. */
	munit_assert_int(xtc_atomic_load_i32(&g_na_rc, xtc_mo_seq_cst), >=, 0);
	munit_assert_int64(xtc_atomic_load_i64(&g_na_ns, xtc_mo_seq_cst), <,
	    100 * NA_MS);
	munit_assert_int(xtc_sup_join(g_na_sup, 0), ==, XTC_OK);
	(void)xtc_loop_fini(loop);
	return MUNIT_OK;
}

/*
 * A1 (otp probe D): kill the supervisor proc itself.  2.1.1 ran the
 * shutdown inline after the receive loop, so the kill longjmp'd past
 * it: the PERMANENT child kept running unsupervised, xtc_sup_alive
 * stayed 1, and xtc_sup_join returned XTC_E_AGAIN forever.  Now the
 * shutdown is an at-exit hook: the child is killed and waited for, and
 * join frees the handle.
 */
static int g_kd_started, g_kd_running, g_kd_stop;
static xtc_supervisor_t *g_kd_sup;
static int g_kd_alive_after = -1, g_kd_child_after = -1;

static void
kd_dec(void *a)
{
	(void)a;
	xtc_atomic_fetch_sub_i32(&g_kd_running, 1, xtc_mo_seq_cst);
}

/* The child counts itself running until its exit hook runs. */
static void
kd_worker(void *a)
{
	(void)a;
	(void)xtc_proc_at_exit(kd_dec, NULL);
	xtc_atomic_fetch_add_i32(&g_kd_started, 1, xtc_mo_seq_cst);
	xtc_atomic_fetch_add_i32(&g_kd_running, 1, xtc_mo_seq_cst);
	while (!xtc_atomic_load_i32(&g_kd_stop, xtc_mo_seq_cst))
		(void)xtc_proc_sleep(10 * NA_MS);
}

struct kd_info {
	xtc_pid_t self;
	xtc_pid_t sup;
};

static int
kd_find_sup(const xtc_proc_info_t *i, void *u)
{
	struct kd_info *k = u;
	/* The supervisor is the first proc of this loop: lowest local_id. */
	if (!xtc_pid_eq(i->pid, k->self) &&
	    (xtc_pid_is_none(k->sup) || i->pid.local_id < k->sup.local_id))
		k->sup = i->pid;
	return 0;
}

static void
kd_killer(void *a)
{
	struct kd_info k;
	(void)a;
	(void)xtc_proc_sleep(50 * NA_MS);
	k.self = xtc_self();
	k.sup = XTC_PID_NONE;
	(void)xtc_inspect_procs(kd_find_sup, &k);
	munit_assert_false(xtc_pid_is_none(k.sup));
	munit_assert_int(xtc_exit_pid(k.sup, 9), ==, XTC_OK);
	(void)xtc_proc_sleep(300 * NA_MS);
	xtc_atomic_store_i32(&g_kd_alive_after, xtc_sup_alive(g_kd_sup),
	    xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_kd_child_after,
	    xtc_atomic_load_i32(&g_kd_running, xtc_mo_seq_cst), xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_kd_stop, 1,
	    xtc_mo_seq_cst); /* release a 2.1.1 orphan */
}

static MunitResult
test_killed_sup_stops_children(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_child_spec_t ch = XTC_CHILD_SPEC_INIT;
	xtc_sup_opts_t so = XTC_SUP_OPTS_DEFAULT;
	xtc_pid_t pid;
	(void)p;
	(void)d;
	xtc_atomic_store_i32(&g_kd_started, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_kd_running, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_kd_stop, 0, xtc_mo_seq_cst);
	ch.fn = kd_worker;
	ch.policy = XTC_RESTART_PERMANENT;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_sup_start(loop, &so, &ch, 1, &g_kd_sup), ==,
	    XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, kd_killer, NULL, NULL, &pid), ==,
	    XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_atomic_load_i32(&g_kd_started, xtc_mo_seq_cst), ==,
	    1);
	munit_assert_int(xtc_atomic_load_i32(&g_kd_alive_after, xtc_mo_seq_cst),
	    ==, 0);
	munit_assert_int(xtc_atomic_load_i32(&g_kd_child_after, xtc_mo_seq_cst),
	    ==, 0);
	munit_assert_int(xtc_sup_join(g_kd_sup, 0), ==, XTC_OK);
	(void)xtc_loop_fini(loop);
	return MUNIT_OK;
}

/*
 * A3 (otp probe E): a proc with a monitor DOWN already queued calls
 * xtc_sup_add_child.  2.1.1 waited for the reply with a plain xtc_recv
 * and freed everything that was not it: add_child returned XTC_OK and
 * the DOWN was gone.
 */
static int g_ac_rc = -999, g_ac_down = -1;
static xtc_supervisor_t *g_ac_sup;

static void
ac_quick(void *a)
{
	(void)a;
	(void)xtc_exit_self(3);
}

static void
ac_worker(void *a)
{
	(void)a;
	(void)xtc_proc_sleep(20 * NA_MS);
}

static void
ac_caller(void *a)
{
	xtc_child_spec_t spec = XTC_CHILD_SPEC_INIT;
	xtc_down_info_t di = XTC_DOWN_INFO_INIT;
	xtc_pid_t x, w;
	uint64_t ref;
	void *m = NULL;
	size_t sz;
	(void)xtc_proc_spawn_monitor((xtc_loop_t *)a, ac_quick, NULL, NULL, &x,
	    &ref);
	(void)xtc_proc_sleep(20 * NA_MS); /* the DOWN is now queued */
	spec.fn = ac_worker;
	spec.policy = XTC_RESTART_TEMPORARY;
	xtc_atomic_store_i32(&g_ac_rc, xtc_sup_add_child(g_ac_sup, &spec, &w),
	    xtc_mo_seq_cst);
	if (xtc_recv(&m, &sz, 50 * NA_MS) == XTC_OK &&
	    xtc_down_decode_ex(m, sz, &di) == XTC_OK && di.ref == ref &&
	    di.exit_code == 3)
		xtc_atomic_store_i32(&g_ac_down, 1, xtc_mo_seq_cst);
	else
		xtc_atomic_store_i32(&g_ac_down, 0, xtc_mo_seq_cst);
	if (m != NULL)
		xtc_free(m);
	(void)xtc_sup_stop(g_ac_sup);
}

static MunitResult
test_add_child_keeps_mail(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_sup_opts_t so = XTC_SUP_OPTS_DEFAULT;
	xtc_pid_t pid;
	(void)p;
	(void)d;
	so.strategy = XTC_SUP_SIMPLE_OFO;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_sup_start(loop, &so, NULL, 0, &g_ac_sup), ==,
	    XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, ac_caller, loop, NULL, &pid), ==,
	    XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_atomic_load_i32(&g_ac_rc, xtc_mo_seq_cst), ==,
	    XTC_OK);
	munit_assert_int(xtc_atomic_load_i32(&g_ac_down, xtc_mo_seq_cst), ==,
	    1);
	munit_assert_int(xtc_sup_join(g_ac_sup, 0), ==, XTC_OK);
	(void)xtc_loop_fini(loop);
	return MUNIT_OK;
}

/*
 * on_child_down (2.2).  The embedder's hook sees every child DOWN, fully
 * classified, before the supervisor decides the restart, keyed by the
 * spec (a per-child identity the pid is not).  The PostgreSQL embedder's
 * use: TEMPORARY children whose crash must escalate to a fail-stop.
 */
#define HK_MAX 64
struct hk_rec {
	const char     *name;
	xtc_down_kind_t kind;
	int             signal;
	int             exit_code;
	int             restarts_at_call;   /* n_restarts when the hook ran */
	int             addchild_rc;        /* xtc_sup_add_child from the hook */
};
static struct hk_rec g_hk[HK_MAX];
static int g_hk_n;
static int g_hk_try_add;

static void
hk_fn(xtc_supervisor_t *sup, const xtc_child_spec_t *spec,
      const xtc_down_info_t *info, void *user)
{
	struct hk_rec *r;
	(void)user;
	if (g_hk_n >= HK_MAX)
		return;
	r = &g_hk[g_hk_n++];
	r->name = spec->name;
	r->kind = info->kind;
	r->signal = info->signal;
	r->exit_code = info->exit_code;
	r->restarts_at_call = xtc_sup_n_restarts(sup);
	r->addchild_rc = 1;
	if (g_hk_try_add) {
		xtc_child_spec_t c = XTC_CHILD_SPEC_INIT;
		c.name = "never";
		c.fn = ac_quick;
		c.policy = XTC_RESTART_TEMPORARY;
		r->addchild_rc = xtc_sup_add_child(sup, &c, NULL);
	}
}

static const struct hk_rec *
hk_find(const char *name, int nth)
{
	int i;
	for (i = 0; i < g_hk_n; i++)
		if (strcmp(g_hk[i].name, name) == 0 && nth-- == 0)
			return &g_hk[i];
	return NULL;
}

static void
hk_clean(void *a)
{
	(void)a;
}

static void
hk_exit7(void *a)
{
	(void)a;
	(void)xtc_exit_self(7);
}

static void
hk_crash(void *a)
{
	volatile int *np = NULL;
	(void)a;
	*np = 1;   /* contained under the default recovery frame */
}

static void
hk_park(void *a)
{
	(void)a;
	for (;;)
		(void)xtc_proc_sleep(1000 * NA_MS);
}

static void
hk_stopper(void *a)
{
	(void)xtc_proc_sleep(50 * NA_MS);
	(void)xtc_sup_stop((xtc_supervisor_t *)a);
}

/* Four TEMPORARY children -- clean, exit 7, a crash, and one parked until
 * the supervisor stops -- report CLEAN, EXIT 7, SIGNAL 11 and the stop's
 * kill, each once, by name; nothing is restarted. */
static MunitResult
test_down_hook_kinds(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_sup_opts_t so = XTC_SUP_OPTS_DEFAULT;
	xtc_child_spec_t ch[4] = { XTC_CHILD_SPEC_INIT, XTC_CHILD_SPEC_INIT,
	    XTC_CHILD_SPEC_INIT, XTC_CHILD_SPEC_INIT };
	xtc_supervisor_t *sup = NULL;
	const struct hk_rec *r;
	xtc_pid_t pid;
	(void)p; (void)d;

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
	return MUNIT_SKIP;   /* the sanitizer owns SIGSEGV (as test_proc's
	                      * fault-containment tests) */
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
	return MUNIT_SKIP;
#  endif
#endif

	munit_assert_int(xtc_fault_guard_install(), ==, XTC_OK);
	g_hk_n = 0;
	g_hk_try_add = 1;
	ch[0].name = "clean";  ch[0].fn = hk_clean;
	ch[1].name = "exit7";  ch[1].fn = hk_exit7;
	ch[2].name = "crash";  ch[2].fn = hk_crash;
	ch[3].name = "parked"; ch[3].fn = hk_park;
	for (int i = 0; i < 4; i++)
		ch[i].policy = XTC_RESTART_TEMPORARY;
	so.on_child_down = hk_fn;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_sup_start(loop, &so, ch, 4, &sup), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, hk_stopper, sup, NULL, &pid), ==,
	    XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_sup_join(sup, 0), ==, XTC_OK);
	(void)xtc_loop_fini(loop);

	munit_assert_int(g_hk_n, ==, 4);
	munit_assert_not_null(r = hk_find("clean", 0));
	munit_assert_int(r->kind, ==, XTC_DOWN_KIND_CLEAN);
	munit_assert_not_null(r = hk_find("exit7", 0));
	munit_assert_int(r->kind, ==, XTC_DOWN_KIND_EXIT);
	munit_assert_int(r->exit_code, ==, 7);
	munit_assert_not_null(r = hk_find("crash", 0));
	munit_assert_int(r->kind, ==, XTC_DOWN_KIND_SIGNAL);
	munit_assert_int(r->signal, ==, SIGSEGV);
	/* The stop's kill: reported before the stop completed. */
	munit_assert_not_null(r = hk_find("parked", 0));
	munit_assert_int(r->kind, !=, XTC_DOWN_KIND_CLEAN);
	munit_assert_int(r->kind, !=, XTC_DOWN_KIND_SIGNAL);
	/* Calling into its own supervisor from the hook fails at once
	 * instead of waiting 5 s on a reply only the hook's fiber sends. */
	munit_assert_int(g_hk[0].addchild_rc, ==, XTC_E_STATE);
	g_hk_try_add = 0;
	return MUNIT_OK;
}

/* A caller built against 2.1 passes the old, shorter xtc_sup_opts_t: the
 * library reads only its declared size, so the hook fields beyond it are
 * NULL and nothing is called.  Garbage past the old size must be ignored. */
static MunitResult
test_down_hook_old_opts(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	union { xtc_sup_opts_t o; unsigned char b[sizeof(xtc_sup_opts_t)]; } u;
	xtc_child_spec_t ch = XTC_CHILD_SPEC_INIT;
	xtc_supervisor_t *sup = NULL;
	xtc_pid_t pid;
	(void)p; (void)d;

	memset(u.b, 0xA5, sizeof u.b);      /* the 2.2 fields hold junk */
	u.o.size = offsetof(xtc_sup_opts_t, on_child_down);   /* 2.1 size */
	u.o.strategy = XTC_SUP_ONE_FOR_ONE;
	u.o.max_restarts = 3;
	u.o.period_ns = 5LL * 1000 * 1000 * 1000;
	u.o.exec = NULL;
	u.o.max_children = 0;
	g_hk_n = 0;
	ch.name = "clean";
	ch.fn = hk_clean;
	ch.policy = XTC_RESTART_TEMPORARY;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_sup_start(loop, &u.o, &ch, 1, &sup), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, hk_stopper, sup, NULL, &pid), ==,
	    XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_sup_join(sup, 0), ==, XTC_OK);
	(void)xtc_loop_fini(loop);
	munit_assert_int(g_hk_n, ==, 0);
	return MUNIT_OK;
}

/* PERMANENT one_for_all: a crash reports SIGNAL before the restart it
 * causes (the restart count is still 0 in the hook), and the sibling the
 * restart kills is reported too, once, before the replacements run -- the
 * DOWNs 2.1 dropped. */
static int g_hk_gen;

static void
hk_crash_once(void *a)
{
	volatile int *np = NULL;
	(void)a;
	if (xtc_atomic_fetch_add_i32(&g_hk_gen, 1, xtc_mo_relaxed) == 0) {
		(void)xtc_proc_sleep(10 * NA_MS);
		*np = 1;
	}
	for (;;)
		(void)xtc_proc_sleep(1000 * NA_MS);
}

static MunitResult
test_down_hook_group_restart(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_sup_opts_t so = XTC_SUP_OPTS_DEFAULT;
	xtc_child_spec_t ch[2] = { XTC_CHILD_SPEC_INIT, XTC_CHILD_SPEC_INIT };
	xtc_supervisor_t *sup = NULL;
	const struct hk_rec *r;
	xtc_pid_t pid;
	(void)p; (void)d;

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
	return MUNIT_SKIP;   /* the sanitizer owns SIGSEGV (as test_proc's
	                      * fault-containment tests) */
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
	return MUNIT_SKIP;
#  endif
#endif

	munit_assert_int(xtc_fault_guard_install(), ==, XTC_OK);
	g_hk_n = 0;
	g_hk_try_add = 0;
	xtc_atomic_store_i32(&g_hk_gen, 0, xtc_mo_relaxed);
	ch[0].name = "crasher"; ch[0].fn = hk_crash_once;
	ch[1].name = "sibling"; ch[1].fn = hk_park;
	ch[0].policy = ch[1].policy = XTC_RESTART_PERMANENT;
	so.strategy = XTC_SUP_ONE_FOR_ALL;
	so.on_child_down = hk_fn;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_sup_start(loop, &so, ch, 2, &sup), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, hk_stopper, sup, NULL, &pid), ==,
	    XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_sup_join(sup, 0), ==, XTC_OK);
	(void)xtc_loop_fini(loop);

	munit_assert_int(xtc_atomic_load_i32(&g_hk_gen, xtc_mo_relaxed), ==, 2);
	munit_assert_not_null(r = hk_find("crasher", 0));
	munit_assert_int(r->kind, ==, XTC_DOWN_KIND_SIGNAL);
	munit_assert_int(r->restarts_at_call, ==, 0);   /* before the restart */
	munit_assert_not_null(r = hk_find("sibling", 0));
	munit_assert_int(r->kind, !=, XTC_DOWN_KIND_SIGNAL);
	munit_assert_int(r->restarts_at_call, ==, 1);   /* in the restart */
	/* Then the stop kills both replacements: 4 reports in all. */
	munit_assert_int(g_hk_n, ==, 4);
	munit_assert_not_null(hk_find("crasher", 1));
	munit_assert_not_null(hk_find("sibling", 1));
	return MUNIT_OK;
}

/* The same group-restart reporting without a fault (the sibling killed
 * by a one_for_all restart, then the stop), so the sanitizer builds,
 * which skip the fault tests, still run the hook path. */
static void
hk_exit5_once(void *a)
{
	(void)a;
	if (xtc_atomic_fetch_add_i32(&g_hk_gen, 1, xtc_mo_relaxed) == 0) {
		(void)xtc_proc_sleep(10 * NA_MS);
		(void)xtc_exit_self(5);
	}
	for (;;)
		(void)xtc_proc_sleep(1000 * NA_MS);
}

static MunitResult
test_down_hook_group_nofault(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_sup_opts_t so = XTC_SUP_OPTS_DEFAULT;
	xtc_child_spec_t ch[2] = { XTC_CHILD_SPEC_INIT, XTC_CHILD_SPEC_INIT };
	xtc_supervisor_t *sup = NULL;
	const struct hk_rec *r;
	xtc_pid_t pid;
	(void)p; (void)d;

	g_hk_n = 0;
	g_hk_try_add = 0;
	xtc_atomic_store_i32(&g_hk_gen, 0, xtc_mo_relaxed);
	ch[0].name = "exiter";  ch[0].fn = hk_exit5_once;
	ch[1].name = "sibling"; ch[1].fn = hk_park;
	ch[0].policy = ch[1].policy = XTC_RESTART_PERMANENT;
	so.strategy = XTC_SUP_ONE_FOR_ALL;
	so.on_child_down = hk_fn;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_sup_start(loop, &so, ch, 2, &sup), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, hk_stopper, sup, NULL, &pid), ==,
	    XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_sup_join(sup, 0), ==, XTC_OK);
	(void)xtc_loop_fini(loop);

	munit_assert_not_null(r = hk_find("exiter", 0));
	munit_assert_int(r->kind, ==, XTC_DOWN_KIND_EXIT);
	munit_assert_int(r->exit_code, ==, 5);
	munit_assert_int(r->restarts_at_call, ==, 0);
	munit_assert_not_null(r = hk_find("sibling", 0));
	munit_assert_int(r->restarts_at_call, ==, 1);
	munit_assert_int(g_hk_n, ==, 4);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/add_child_other_loop", test_add_child_other_loop, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/supervisor_restarts",   test_supervisor_restarts, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/one_for_all",           test_one_for_all,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/rest_for_one",          test_rest_for_one,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/one_for_all_no_overlap",  test_one_for_all_no_overlap,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/join_timeout_keeps_sup",  test_join_timeout_keeps_sup,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/rest_for_one_no_overlap", test_rest_for_one_no_overlap, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/intensity_exceeded",    test_intensity_exceeded,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/simple_one_for_one",    test_simple_one_for_one,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/simple_ofo_reclaims_dead", test_simple_ofo_reclaims_dead, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/pool_max_children",     test_pool_max_children,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/atomic_spawn_monitor",  test_atomic_spawn_monitor, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/one_for_all_equal_pid_other_loop", test_one_for_all_equal_pid_other_loop, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/n_alive_during_group_restart", test_n_alive_during_group_restart,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/killed_sup_stops_children", test_killed_sup_stops_children, NULL,
	    NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/add_child_keeps_mail", test_add_child_keeps_mail, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/down_hook_kinds", test_down_hook_kinds, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/down_hook_group_restart", test_down_hook_group_restart, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/down_hook_old_opts", test_down_hook_old_opts, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/down_hook_group_nofault", test_down_hook_group_nofault, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m10/sup", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
