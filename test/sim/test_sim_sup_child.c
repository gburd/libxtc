/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/sim/test_sim_sup_child.c
 *	Deterministic Simulation Testing of servers and supervisors as
 *	supervised children (2.2): xtc_svr_child_main,
 *	xtc_sup_child_main, and xtc_sup_opts_t.wait_children, inside an
 *	xtc_app on three loops under the simulator's cross-loop latency.
 *
 *	The app's root supervisor (wait_children, one_for_one) has, in
 *	order:
 *	  0. "conf": a server child (loop 1) whose init takes a seeded time
 *	     and records when it finished;
 *	  1. "pool": a nested supervisor (loop 2, budget 2 restarts) over
 *	     one worker that crashes at a seeded point in some runs, and
 *	     otherwise serves until a drain;
 *	  2. "user": a plain child (loop 0) that records when it first ran,
 *	     calls conf by name and waits for a drain.
 *	A plain proc then drains the app (the XTC_APP_SHUTDOWN_MSG every
 *	xtc_app_shutdown sends first) once the user has its reply.
 *
 *	Invariants (per seed):
 *	  (a) the user ran only after conf's init finished, and conf's init
 *	      after nothing else of the app (planted as SUP_WAIT_SKIP: "a
 *	      child ran before the one before it was up");
 *	  (b) when the worker crashes on every start, the nested supervisor
 *	      spends its budget and its exit reaches the root as a failure
 *	      (EXIT XTC_E_RESOURCE), which restarts the subtree (planted as
 *	      SUP_NESTED_CLEAN: "a nested supervisor's give-up did not reach
 *	      its parent");
 *	  (c) the drain reaches conf's handle_info and the nested worker
 *	      (planted as SUP_NESTED_NODRAIN: "a drain did not reach a
 *	      nested supervisor's child");
 *	  (d) the run quiesces and REPLAYS.
 *
 *	argv: [base [count]] -- seeds base+1 .. base+count (default 0 16).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xtc.h"
#include "xtc_app.h"
#include "xtc_exec.h"
#include "xtc_orc.h"
#include "xtc_proc.h"
#include "xtc_reg.h"
#include "sim_int.h"
#include "xtc_svr.h"

#define N_LOOPS 3
#define MS (1000LL * 1000)

static xtc_app_t *g_app;
static int g_seq; /* order of first events */
static int g_conf_up_at, g_user_at, g_order_bad;
static int g_conf_drained, g_wk_drained, g_user_reply;
static int g_crashy; /* this run: the worker crashes on start */
static int g_wk_crashes, g_pool_fail_downs, g_pool_downs;
static uint64_t g_trace;

/* ---- conf: a server child ---------------------------------------- */

static int
conf_init(void *st)
{
	(void)st;
	(void)xtc_proc_sleep(
	    (int64_t)(1 + __xtc_sim_rng_range(XTC_SIM_RNG_APP, 5)) * MS);
	g_conf_up_at = ++g_seq;
	return XTC_OK;
}

static int
conf_call(void *st, const void *req, size_t n, xtc_svr_call_t *call)
{
	int v = 42;
	(void)st;
	(void)req;
	(void)n;
	(void)xtc_svr_reply(call, &v, sizeof v);
	return XTC_SVR_CONTINUE;
}

static int
conf_info(void *st, const void *msg, size_t n)
{
	(void)st;
	if (!xtc_app_is_shutdown_msg(msg, n))
		return XTC_SVR_CONTINUE;
	g_conf_drained = 1;
	return XTC_SVR_STOP;
}

static xtc_svr_callbacks_t g_conf_cb = { .size = sizeof(xtc_svr_callbacks_t),
	.init = conf_init,
	.handle_call = conf_call,
	.handle_info = conf_info };
static xtc_svr_child_t g_conf = { .size = sizeof(xtc_svr_child_t),
	.cb = &g_conf_cb };

/* ---- pool: a nested supervisor over one worker -------------------- */

static void
worker(void *arg)
{
	void *m;
	size_t sz;
	(void)arg;
	if (g_crashy) {
		(void)xtc_proc_sleep(
		    (int64_t)__xtc_sim_rng_range(XTC_SIM_RNG_APP, 3) * MS);
		g_wk_crashes++;
		(void)xtc_exit_self(9);
	}
	while (xtc_recv(&m, &sz, -1) == XTC_OK) {
		int drain = xtc_app_is_shutdown_msg(m, sz);
		xtc_free(m);
		if (drain) {
			g_wk_drained = 1;
			return;
		}
	}
}

static xtc_sup_opts_t g_pool_opts = { .size = sizeof(xtc_sup_opts_t),
	.strategy = XTC_SUP_ONE_FOR_ONE,
	.max_restarts = 2,
	.period_ns = 60LL * 1000 * MS };
static xtc_child_spec_t g_pool_kids[1] = { { .size = sizeof(xtc_child_spec_t),
    .name = "worker",
    .fn = worker,
    .policy = XTC_RESTART_PERMANENT } };
static xtc_sup_child_t g_pool = { .size = sizeof(xtc_sup_child_t),
	.opts = &g_pool_opts,
	.children = g_pool_kids,
	.n_children = 1 };

/* ---- user: a plain child after both -------------------------------- */

static void
user(void *arg)
{
	void *rep = NULL, *m;
	size_t n = 0, sz;
	(void)arg;
	if (g_user_at == 0) {
		g_user_at = ++g_seq;
		if (g_conf_up_at == 0 || g_conf_up_at > g_user_at)
			g_order_bad = 1;
	}
	if (xtc_svr_call_name(xtc_app_registry(g_app), "conf", "q", 1, &rep, &n,
	        1000 * MS) == XTC_OK &&
	    n == sizeof(int))
		g_user_reply = *(int *)rep;
	xtc_free(rep);
	while (xtc_recv(&m, &sz, -1) == XTC_OK) {
		int drain = xtc_app_is_shutdown_msg(m, sz);
		xtc_free(m);
		if (drain)
			return;
	}
}

/* The root's hook: the nested supervisor's deaths. */
static void
on_down(xtc_supervisor_t *sup, const xtc_child_spec_t *spec,
    const xtc_down_info_t *info, void *u)
{
	(void)sup;
	(void)u;
	g_trace = g_trace * 1000003u +
	    (uint64_t)(info->kind * 100 + info->exit_code + 50);
	if (spec->fn != xtc_sup_child_main)
		return;
	g_pool_downs++;
	if (info->kind == XTC_DOWN_KIND_EXIT &&
	    info->exit_code == XTC_E_RESOURCE)
		g_pool_fail_downs++;
}

/* Drain like xtc_app_shutdown's first step, once the user has its reply
 * (and, in a crashy run, the subtree has given up twice), then stop. */
static void
drainer(void *arg)
{
	xtc_reg_t *reg = xtc_app_registry(g_app);
	xtc_pid_t pid;
	const char *names[3] = { "conf", "pool", "user" };
	int i;
	(void)arg;
	for (i = 0;
	     i < 2000 && (g_user_reply == 0 || (g_crashy && g_pool_downs < 2));
	     i++)
		(void)xtc_proc_sleep(1 * MS);
	if (g_crashy)
		g_crashy = 0; /* the next subtree incarnation serves */
	for (i = 0; i < 2000 && xtc_reg_whereis(reg, "pool", &pid) != XTC_OK;
	     i++)
		(void)xtc_proc_sleep(1 * MS);
	(void)xtc_proc_sleep(5 * MS);
	for (i = 0; i < 3; i++)
		if (xtc_reg_whereis(reg, names[i], &pid) == XTC_OK)
			(void)xtc_send(pid, XTC_APP_SHUTDOWN_MSG,
			    sizeof XTC_APP_SHUTDOWN_MSG);
	for (i = 0; i < 200 && !(g_conf_drained && g_wk_drained); i++)
		(void)xtc_proc_sleep(1 * MS);
	(void)xtc_app_stop(g_app);
}

static int
run_one(uint64_t seed, int crashy, uint64_t *out_state)
{
	xtc_app_opts_t opts = XTC_APP_OPTS_DEFAULT;
	xtc_sup_opts_t so = XTC_SUP_OPTS_DEFAULT;
	xtc_child_spec_t kids[3] = { XTC_CHILD_SPEC_INIT, XTC_CHILD_SPEC_INIT,
		XTC_CHILD_SPEC_INIT };
	xtc_exec_t *exec;
	int rc;

	g_seq = g_conf_up_at = g_user_at = g_order_bad = 0;
	g_conf_drained = g_wk_drained = g_user_reply = 0;
	g_wk_crashes = g_pool_fail_downs = g_pool_downs = 0;
	g_crashy = crashy;
	g_trace = 0;

	opts.n_loops = N_LOOPS;
	opts.no_tuning_check = 1;
	so.max_restarts = 4;
	so.period_ns = 60LL * 1000 * MS;
	so.wait_children = 1;
	so.on_child_down = on_down;
	opts.sup = &so;
	if (xtc_app_create(&opts, &g_app) != XTC_OK)
		return -1;
	/* The nested supervisor names its worker nowhere; the root names
	 * its three children in the app's registry. */
	kids[0].name = kids[0].reg_name = "conf";
	kids[0].fn = xtc_svr_child_main;
	kids[0].arg = &g_conf;
	kids[0].policy = XTC_RESTART_TRANSIENT;
	kids[0].loop = 1;
	kids[1].name = kids[1].reg_name = "pool";
	kids[1].fn = xtc_sup_child_main;
	kids[1].arg = &g_pool;
	kids[1].policy = XTC_RESTART_TRANSIENT;
	kids[1].loop = 2;
	kids[2].name = kids[2].reg_name = "user";
	kids[2].fn = user;
	kids[2].policy = XTC_RESTART_TRANSIENT;
	kids[2].loop = 0;
	if (xtc_app_start(g_app, kids, 3) != XTC_OK) {
		xtc_app_destroy(g_app);
		return -1;
	}
	exec = xtc_app_exec(g_app);
	(void)xtc_proc_spawn(xtc_exec_loop(exec, 0), drainer, NULL, NULL, NULL);
	xtc_sim_net_latency(10 * 1000LL, 300 * 1000LL);
	rc = xtc_sim_exec_run(exec, seed, 20000000);
	xtc_sim_net_latency(0, 0);
	*out_state = xtc_sim_state_hash(exec);
	xtc_app_destroy(g_app);
	g_app = NULL;
	return rc;
}

int
main(int argc, char **argv)
{
	uint64_t base = 0, seed, st, st2, tr;
	int n = 16, k, fails = 0, rc, rc2, crashy;

	if (argc > 1)
		base = strtoull(argv[1], NULL, 0);
	if (argc > 2)
		n = atoi(argv[2]);
	printf("== supervised-child DST: %d seeds from base %llu ==\n", n,
	    (unsigned long long)base);
	for (k = 1; k <= n; k++) {
		int pass = 1;
		seed = 0x9E3779B97F4A7C15ull * (base + (uint64_t)k);
		/* Every other seed: the worker crashes on every start until
		 * the subtree has given up twice. */
		crashy = (int)((base + (uint64_t)k) & 1);
		rc = run_one(seed, crashy, &st);
		tr = g_trace;
		if (g_order_bad) {
			printf("  seed %llu: a child ran before the one before "
			       "it was up (conf up at %d, user at %d)\n",
			    (unsigned long long)(base + (uint64_t)k),
			    g_conf_up_at, g_user_at);
			pass = 0;
		}
		if (crashy && g_pool_fail_downs < 2) {
			printf("  seed %llu: a nested supervisor's give-up did "
			       "not reach its parent (%d of %d pool DOWNs were "
			       "failures, %d worker crashes)\n",
			    (unsigned long long)(base + (uint64_t)k),
			    g_pool_fail_downs, g_pool_downs, g_wk_crashes);
			pass = 0;
		}
		if (!g_conf_drained || !g_wk_drained) {
			printf("  seed %llu: a drain did not reach a nested "
			       "supervisor's child (conf %d, worker %d)\n",
			    (unsigned long long)(base + (uint64_t)k),
			    g_conf_drained, g_wk_drained);
			pass = 0;
		}
		if (rc != XTC_OK || g_user_reply != 42) {
			printf("  seed %llu: rc=%d reply=%d\n",
			    (unsigned long long)(base + (uint64_t)k), rc,
			    g_user_reply);
			pass = 0;
		}
		if (pass) {
			rc2 = run_one(seed, crashy, &st2);
			if (rc2 != rc || st2 != st || g_trace != tr) {
				printf("  seed %llu: replay diverged\n",
				    (unsigned long long)(base + (uint64_t)k));
				pass = 0;
			}
		}
		if (!pass)
			fails++;
	}
	if (fails != 0) {
		printf("FAIL: %d/%d supervised-child seeds failed\n", fails, n);
		return 1;
	}
	printf("OK: supervised-child DST -- %d seeds, a server child and a "
	       "nested supervisor in an app on %d loops: ordered start, the "
	       "subtree's give-up restarts it, the drain reaches both, all "
	       "replay\n",
	    n, N_LOOPS);
	return 0;
}
