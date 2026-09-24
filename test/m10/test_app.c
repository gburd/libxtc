/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m10/test_app.c -- verifies M10.5 xtc_app.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>

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

/* ---- graceful drain: xtc_app_shutdown (PLAN.md 19.27.10) ---- */
#include <signal.h>
#include <unistd.h>
#include "xtc_inspect.h"

#define MS (1000LL * 1000)

static _Atomic int g_dr_hook_done;   /* at-exit hooks that COMPLETED */
static _Atomic int g_dr_started;     /* children that reached their body */

/* Wait for the drain request; return (the cooperative exit). */
static void
dr_polite(void *arg)
{
	void *m; size_t s;
	(void)arg;
	atomic_fetch_add(&g_dr_started, 1);
	for (;;) {
		if (xtc_recv(&m, &s, -1) != XTC_OK) continue;
		if (xtc_app_is_shutdown_msg(m, s)) { __os_free(m); return; }
		__os_free(m);
	}
}

/* The at-exit hook PARKS for 30 ms before it completes.  If shutdown
 * returned on `!alive` it would return with this hook still pending; the
 * cleanup-complete test (proc-table absence) must see it done. */
static void
dr_slow_hook(void *arg)
{
	(void)arg;
	(void)xtc_proc_sleep(30 * MS);
	atomic_fetch_add(&g_dr_hook_done, 1);
}

/* Ignores the drain request (throws the message away). */
static void
dr_deaf(void *arg)
{
	void *m; size_t s;
	(void)arg;
	(void)xtc_proc_at_exit(dr_slow_hook, NULL);
	atomic_fetch_add(&g_dr_started, 1);
	for (;;)
		if (xtc_recv(&m, &s, -1) == XTC_OK) __os_free(m);
}

/* Masked forever: neither the request nor the force-cancel can land. */
static void
dr_masked(void *arg)
{
	(void)arg;
	(void)xtc_mask_enter();
	atomic_fetch_add(&g_dr_started, 1);
	for (;;)
		(void)xtc_proc_sleep(5 * MS);
}

static void
dr_spec(xtc_child_spec_t *k, xtc_proc_fn fn, xtc_restart_policy_t pol)
{
	memset(k, 0, sizeof *k);
	k->name = "dr";
	k->fn = fn;
	k->policy = pol;
}

struct dr_run {
	xtc_app_t              *app;
	int64_t                 drain_ns, force_ns;
	int                     n_kids;
	int                     rc;
	xtc_app_drain_report_t  rep;
};

/* Shutdown from a plain thread once every child is in its body. */
static void *
dr_thread(void *arg)
{
	struct dr_run *r = arg;
	while (atomic_load(&g_dr_started) < r->n_kids)
		(void)xtc_sleep_ns(1 * MS);
	r->rc = xtc_app_shutdown(r->app, r->drain_ns, r->force_ns, &r->rep);
	return NULL;
}

static void
dr_go(struct dr_run *r, xtc_child_spec_t *kids, int n, int n_loops)
{
	xtc_app_opts_t opts = XTC_APP_OPTS_DEFAULT;
	xtc_app_drain_report_t init = XTC_APP_DRAIN_REPORT_INIT;
	pthread_t th;

	atomic_store(&g_dr_hook_done, 0);
	atomic_store(&g_dr_started, 0);
	opts.no_tuning_check = 1;
	opts.n_loops = n_loops;
	r->n_kids = n;
	r->rep = init;
	munit_assert_int(xtc_app_create(&opts, &r->app), ==, XTC_OK);
	munit_assert_int(xtc_app_start(r->app, kids, n), ==, XTC_OK);
	munit_assert_int(pthread_create(&th, NULL, dr_thread, r), ==, 0);
	munit_assert_int(xtc_app_run(r->app), ==, XTC_OK);
	munit_assert_int(pthread_join(th, NULL), ==, 0);
}

/* A child that drains within drain_ns is reported drained -- PERMANENT
 * (which must not be restarted into the drain) and TRANSIENT alike. */
static MunitResult
test_app_shutdown_drains(const MunitParameter p[], void *d)
{
	xtc_child_spec_t kids[2];
	struct dr_run r;
	(void)p; (void)d;
	memset(&r, 0, sizeof r);
	dr_spec(&kids[0], dr_polite, XTC_RESTART_PERMANENT);
	dr_spec(&kids[1], dr_polite, XTC_RESTART_TRANSIENT);
	r.drain_ns = 5000 * MS;
	r.force_ns = 1000 * MS;
	dr_go(&r, kids, 2, 1);
	munit_assert_int(r.rc, ==, XTC_OK);
	munit_assert_size(r.rep.size, ==, sizeof r.rep);
	munit_assert_int(r.rep.n_children, ==, 2);
	munit_assert_int(r.rep.n_drained, ==, 2);
	munit_assert_int(r.rep.n_forced, ==, 0);
	munit_assert_int(r.rep.n_survivors, ==, 0);
	munit_assert_uint64(r.rep.forced_mask | r.rep.survivor_mask, ==, 0);
	/* It returned when they drained, not at the deadline. */
	munit_assert_int64(r.rep.elapsed_ns, <, 2000 * MS);
	xtc_app_destroy(r.app);
	return MUNIT_OK;
}

/* A child that ignores the request is force-cancelled and reported, and
 * shutdown returns only after its (parking) at-exit hook COMPLETED. */
static MunitResult
test_app_shutdown_forces(const MunitParameter p[], void *d)
{
	xtc_child_spec_t kids[2];
	struct dr_run r;
	(void)p; (void)d;
	memset(&r, 0, sizeof r);
	dr_spec(&kids[0], dr_polite, XTC_RESTART_PERMANENT);
	dr_spec(&kids[1], dr_deaf, XTC_RESTART_PERMANENT);
	r.drain_ns = 100 * MS;
	r.force_ns = 2000 * MS;
	dr_go(&r, kids, 2, 1);
	munit_assert_int(r.rc, ==, XTC_OK);
	munit_assert_int(r.rep.n_drained, ==, 1);
	munit_assert_int(r.rep.n_forced, ==, 1);
	munit_assert_int(r.rep.n_survivors, ==, 0);
	munit_assert_uint64(r.rep.forced_mask, ==, 1u << 1);
	munit_assert_int(atomic_load(&g_dr_hook_done), ==, 1);
	munit_assert_int64(r.rep.elapsed_ns, >=, 100 * MS);
	xtc_app_destroy(r.app);
	return MUNIT_OK;
}

/* A MASKED child is reported not-drained and shutdown still returns
 * within drain_ns + force_ns + slack (and so does xtc_app_run). */
static MunitResult
test_app_shutdown_masked_bounded(const MunitParameter p[], void *d)
{
	xtc_child_spec_t kids[2];
	struct dr_run r;
	int64_t t0, t1;
	(void)p; (void)d;
	memset(&r, 0, sizeof r);
	dr_spec(&kids[0], dr_polite, XTC_RESTART_PERMANENT);
	dr_spec(&kids[1], dr_masked, XTC_RESTART_PERMANENT);
	r.drain_ns = 200 * MS;
	r.force_ns = 300 * MS;
	t0 = xtc_clock_mono();
	dr_go(&r, kids, 2, 1);
	t1 = xtc_clock_mono();
	munit_assert_int(r.rc, ==, XTC_E_AGAIN);
	munit_assert_int(r.rep.n_drained, ==, 1);
	munit_assert_int(r.rep.n_forced, ==, 0);
	munit_assert_int(r.rep.n_survivors, ==, 1);
	munit_assert_uint64(r.rep.survivor_mask, ==, 1u << 1);
	munit_assert_int64(r.rep.elapsed_ns, >=, 500 * MS);
	munit_assert_int64(r.rep.elapsed_ns, <, 500 * MS + 250 * MS);
	/* The whole run (incl. waiting for the children to start). */
	munit_assert_int64(t1 - t0, <, 3000 * MS);
	xtc_app_destroy(r.app);
	return MUNIT_OK;
}

/* Multi-loop: the supervisor stops the executor the instant it exits
 * (sup.c), which can freeze a force-cancelled child mid-cleanup.  The
 * report must stay HONEST about that and the call bounded: a child is
 * only "forced" if its cleanup really completed, else a survivor. */
static MunitResult
test_app_shutdown_multiloop(const MunitParameter p[], void *d)
{
	xtc_child_spec_t kids[3];
	struct dr_run r;
	(void)p; (void)d;
	memset(&r, 0, sizeof r);
	dr_spec(&kids[0], dr_polite, XTC_RESTART_PERMANENT);
	dr_spec(&kids[1], dr_deaf, XTC_RESTART_PERMANENT);
	dr_spec(&kids[2], dr_polite, XTC_RESTART_TRANSIENT);
	kids[1].loop = 1;
	kids[2].loop = 2;
	r.drain_ns = 100 * MS;
	r.force_ns = 300 * MS;
	dr_go(&r, kids, 3, 3);
	munit_assert_int(r.rep.n_drained + r.rep.n_forced + r.rep.n_survivors,
	    ==, 3);
	munit_assert_int(r.rc, ==, r.rep.n_survivors ? XTC_E_AGAIN : XTC_OK);
	/* The TRANSIENT child returned and exited while the executor ran. */
	munit_assert_int(r.rep.n_drained, >=, 1);
	munit_assert_uint64(r.rep.survivor_mask & (1u << 2), ==, 0);
	/* Deaf child: forced means its (parking) hook finished. */
	if (r.rep.forced_mask & (1u << 1))
		munit_assert_int(atomic_load(&g_dr_hook_done), ==, 1);
	else
		munit_assert_uint64(r.rep.survivor_mask & (1u << 1), !=, 0);
	munit_assert_int64(r.rep.elapsed_ns, <, 400 * MS + 250 * MS);
	xtc_app_destroy(r.app);
	return MUNIT_OK;
}

static xtc_app_t              *g_inval_app;
static xtc_app_drain_report_t  g_inval_rep;
static int                     g_inval_rc, g_inval_rc2;

static void
inval_driver(void *arg)
{
	(void)arg;
	g_inval_rc = xtc_app_shutdown(g_inval_app, 0, 100 * MS, &g_inval_rep);
	g_inval_rc2 = xtc_app_shutdown(g_inval_app, 0, 0, NULL);
}

/* Argument validation incl. the size-versioned report. */
static MunitResult
test_app_shutdown_inval(const MunitParameter p[], void *d)
{
	xtc_app_t *a;
	xtc_app_opts_t opts = XTC_APP_OPTS_DEFAULT;
	xtc_app_drain_report_t rep = XTC_APP_DRAIN_REPORT_INIT;
	xtc_pid_t dpid;
	(void)p; (void)d;
	munit_assert_int(xtc_app_shutdown(NULL, 0, 0, NULL), ==, XTC_E_INVAL);
	opts.no_tuning_check = 1;
	munit_assert_int(xtc_app_create(&opts, &a), ==, XTC_OK);
	munit_assert_int(xtc_app_shutdown(a, 0, 0, &rep), ==, XTC_E_INVAL);
	munit_assert_int(xtc_app_start(a, NULL, 0), ==, XTC_OK);
	munit_assert_int(xtc_app_shutdown(a, -1, 0, NULL), ==, XTC_E_INVAL);
	rep.size = sizeof rep.size;
	munit_assert_int(xtc_app_shutdown(a, 0, 0, &rep), ==, XTC_E_INVAL);
	munit_assert_int(xtc_app_is_shutdown_msg(XTC_APP_SHUTDOWN_MSG,
	    sizeof XTC_APP_SHUTDOWN_MSG), ==, 1);
	munit_assert_int(xtc_app_is_shutdown_msg("x", 2), ==, 0);
	/* Not running yet: nothing could drain. */
	rep.size = sizeof rep;
	munit_assert_int(xtc_app_shutdown(a, 0, 0, &rep), ==, XTC_E_INVAL);
	/* Zero children, shutdown from a (non-child) proc: run returns. */
	g_inval_app = a;
	g_inval_rep = rep;
	munit_assert_int(xtc_proc_spawn(xtc_app_loop(a), inval_driver, NULL,
	    NULL, &dpid), ==, XTC_OK);
	munit_assert_int(xtc_app_run(a), ==, XTC_OK);
	munit_assert_int(g_inval_rc, ==, XTC_OK);
	munit_assert_int(g_inval_rep.n_children, ==, 0);
	munit_assert_int(g_inval_rc2, ==, XTC_E_INVAL);   /* one per app */
	xtc_app_destroy(a);
	return MUNIT_OK;
}

/* The opt-in helper drains on a self-sent SIGTERM, and restores the
 * previous disposition on destroy. */
static xtc_app_drain_report_t g_sig_rep;

static void
sig_sender(void *arg)
{
	(void)arg;
	while (atomic_load(&g_dr_started) < 2)
		(void)xtc_proc_sleep(1 * MS);
	(void)kill(getpid(), SIGTERM);
}

static MunitResult
test_app_drain_on_sigterm(const MunitParameter p[], void *d)
{
	xtc_app_t *a;
	xtc_app_opts_t opts = XTC_APP_OPTS_DEFAULT;
	xtc_app_drain_report_t init = XTC_APP_DRAIN_REPORT_INIT;
	xtc_child_spec_t kids[2];
	struct sigaction before, after;
	xtc_pid_t spid;
	(void)p; (void)d;

	atomic_store(&g_dr_hook_done, 0);
	atomic_store(&g_dr_started, 0);
	g_sig_rep = init;
	opts.no_tuning_check = 1;
	dr_spec(&kids[0], dr_polite, XTC_RESTART_PERMANENT);
	dr_spec(&kids[1], dr_deaf, XTC_RESTART_PERMANENT);
	munit_assert_int(sigaction(SIGTERM, NULL, &before), ==, 0);
	munit_assert_int(xtc_app_create(&opts, &a), ==, XTC_OK);
	munit_assert_int(xtc_app_start(a, kids, 2), ==, XTC_OK);
	munit_assert_int(xtc_app_drain_on_signal(a, 100 * MS, 2000 * MS,
	    &g_sig_rep), ==, XTC_OK);
	munit_assert_int(xtc_app_drain_on_signal(a, 0, 0, NULL), ==,
	    XTC_E_INVAL);                            /* one armed app */
	munit_assert_int(xtc_proc_spawn(xtc_app_loop(a), sig_sender, NULL,
	    NULL, &spid), ==, XTC_OK);
	munit_assert_int(xtc_app_run(a), ==, XTC_OK);
	munit_assert_int(g_sig_rep.n_children, ==, 2);
	munit_assert_int(g_sig_rep.n_drained, ==, 1);
	munit_assert_int(g_sig_rep.n_forced, ==, 1);
	munit_assert_int(g_sig_rep.n_survivors, ==, 0);
	munit_assert_int(atomic_load(&g_dr_hook_done), ==, 1);
	xtc_app_destroy(a);
	munit_assert_int(sigaction(SIGTERM, NULL, &after), ==, 0);
	munit_assert_ptr(after.sa_handler, ==, before.sa_handler);
	return MUNIT_OK;
}

/* ---- sup exit waits for its children's cleanup (multi-loop) ----
 *
 * A multi-loop app: 3 children on loops 1..3, each with an at-exit hook
 * that takes 50 ms and then records that it finished.  xtc_app_stop ->
 * the supervisor kills them and exits.  Every hook must have FINISHED by
 * the time xtc_app_run returns.  Before the fix the supervisor called
 * xtc_exec_stop right after sending the kills, so the executor stopped
 * while the hooks were still sleeping: hooks_done < 3 when run returned. */
static _Atomic int g_ex_started, g_ex_hooks_done;
static xtc_app_t *g_ex_app;

static void
ex_slow_hook(void *a)
{
	(void)a;
	(void)xtc_proc_sleep(50LL * 1000 * 1000);
	atomic_fetch_add(&g_ex_hooks_done, 1);
}

static void
ex_child(void *a)
{
	void *m = NULL;
	size_t n;
	(void)a;
	(void)xtc_proc_at_exit(ex_slow_hook, NULL);
	atomic_fetch_add(&g_ex_started, 1);
	for (;;) {
		(void)xtc_recv(&m, &n, 100LL * 1000 * 1000);
		if (m != NULL) { xtc_free(m); m = NULL; }
	}
}

static void
ex_driver(void *a)
{
	(void)a;
	while (atomic_load(&g_ex_started) < 3)
		(void)xtc_proc_sleep(5LL * 1000 * 1000);
	(void)xtc_app_stop(g_ex_app);
}

static MunitResult
test_app_stop_waits_cleanup_multiloop(const MunitParameter p[], void *d)
{
	xtc_app_t *a;
	xtc_app_opts_t opts = XTC_APP_OPTS_DEFAULT;
	xtc_child_spec_t kids[3];
	int i;
	(void)p; (void)d;

	atomic_store(&g_ex_started, 0);
	atomic_store(&g_ex_hooks_done, 0);
	opts.name = "test_ex";
	opts.n_loops = 4;
	memset(kids, 0, sizeof kids);
	for (i = 0; i < 3; i++) {
		kids[i].name   = "ex_child";
		kids[i].fn     = ex_child;
		kids[i].loop   = i + 1;
		kids[i].policy = XTC_RESTART_TEMPORARY;
	}
	munit_assert_int(xtc_app_create(&opts, &a), ==, XTC_OK);
	g_ex_app = a;
	munit_assert_int(xtc_app_start(a, kids, 3), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(xtc_app_loop(a), ex_driver, NULL, NULL,
	    NULL), ==, XTC_OK);
	munit_assert_int(xtc_app_run(a), ==, XTC_OK);
	munit_assert_int(atomic_load(&g_ex_hooks_done), ==, 3);
	xtc_app_destroy(a);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/app_basic", test_app_basic, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/app_multiloop", test_app_multiloop, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/shutdown_drains", test_app_shutdown_drains, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/shutdown_forces", test_app_shutdown_forces, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/shutdown_masked_bounded", test_app_shutdown_masked_bounded, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/shutdown_multiloop", test_app_shutdown_multiloop, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/shutdown_inval", test_app_shutdown_inval, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/drain_on_sigterm", test_app_drain_on_sigterm, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/stop_waits_cleanup_multiloop", test_app_stop_waits_cleanup_multiloop, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m10.5/app", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
