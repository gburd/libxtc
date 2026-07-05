/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/sim/test_sim_app.c
 *	Deterministic Simulation Testing of the L4 application container
 *	(src/orc/app.c) -- OTP's `application`: a root supervisor + a
 *	process registry + lifecycle plumbing across multiple loops.
 *
 *	Under sim we bring the app up (xtc_app_create + xtc_app_start) and
 *	drive its executor with xtc_sim_exec_run instead of xtc_app_run, so
 *	the whole lifecycle is a pure function of the seed.  The scenario:
 *
 *	  - a SERVER child registers a well-known name in the app registry,
 *	    then serves request messages (reply = request + 1);
 *	  - CLIENT children (placed on other loops) look the server up by
 *	    name (xtc_reg_whereis), send a seeded value, await the reply,
 *	    and record it -- proving the registry is usable across the app
 *	    lifecycle and cross-loop messaging works within the app;
 *	  - once all clients are satisfied, a coordinator stops the app
 *	    (xtc_app_stop), which stops the root supervisor and the
 *	    executor, so the run quiesces.
 *
 *	Invariants (per seed):
 *	  (a) every client resolved the server by name and got the correct
 *	      reply (value + 1) -- no lost registration / lost message;
 *	  (b) the registry held exactly the server's name while running;
 *	  (c) clean quiescence (XTC_OK), no deadlock;
 *	  (d) REPLAY: identical fingerprint for a repeated seed.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_orc.h"
#include "xtc_app.h"
#include "xtc_reg.h"
#include "xtc_sim.h"

#define N_LOOPS   3
#define N_CLIENTS 4
#define SVC_NAME  "calc"

static xtc_reg_t *g_reg;             /* the app registry */
static xtc_app_t *g_app;
static atomic_int g_replies_ok;      /* clients that got the right reply */
static atomic_int g_clients_done;    /* clients that finished */
static atomic_int g_reg_count_seen;  /* registry count observed by server */

/* Request/reply envelope: sender pid + a value. */
struct req { xtc_pid_t from; int value; };

/* SERVER: register my name, then serve requests (reply value+1). */
static void
server(void *arg)
{
	(void)arg;
	(void)xtc_reg_register(g_reg, SVC_NAME, xtc_self());
	atomic_store(&g_reg_count_seen, xtc_reg_count(g_reg));
	for (;;) {
		void *m = NULL;
		size_t sz = 0;
		if (xtc_recv(&m, &sz, 50 * 1000 * 1000LL) == XTC_OK &&
		    m != NULL && sz == sizeof(struct req)) {
			struct req *r = m;
			int reply = r->value + 1;
			(void)xtc_send(r->from, &reply, sizeof reply);
			free(m);
		} else if (m != NULL) {
			free(m);
		}
	}
}

/* CLIENT: resolve the server by name (retry until registered), send a
 * seeded value, await the reply, verify value+1. */
static void
client(void *arg)
{
	int id = (int)(intptr_t)arg;
	xtc_pid_t svc = (xtc_pid_t){0};
	struct req rq;
	int tries;
	int val = (int)(id * 100 +
	    (int)__xtc_sim_rng_range(XTC_SIM_RNG_APP, 50));

	for (tries = 0; tries < 100; tries++) {
		if (xtc_reg_whereis(g_reg, SVC_NAME, &svc) == XTC_OK &&
		    !xtc_pid_is_none(svc))
			break;
		(void)xtc_proc_sleep(1000 * 1000LL);
	}
	if (xtc_pid_is_none(svc)) {
		atomic_fetch_add(&g_clients_done, 1);
		return;
	}

	rq.from = xtc_self();
	rq.value = val;
	(void)xtc_send(svc, &rq, sizeof rq);

	{
		void *m = NULL;
		size_t sz = 0;
		if (xtc_recv(&m, &sz, 200 * 1000 * 1000LL) == XTC_OK &&
		    m != NULL && sz == sizeof(int)) {
			int reply = *(int *)m;
			if (reply == val + 1)
				atomic_fetch_add(&g_replies_ok, 1);
			free(m);
		} else if (m != NULL) {
			free(m);
		}
	}
	atomic_fetch_add(&g_clients_done, 1);
}

/* COORDINATOR: wait until all clients are done, then stop the app. */
static void
coordinator(void *arg)
{
	(void)arg;
	int tries;
	for (tries = 0; tries < 500; tries++) {
		if (atomic_load(&g_clients_done) >= N_CLIENTS)
			break;
		(void)xtc_proc_sleep(1000 * 1000LL);
	}
	(void)xtc_proc_sleep(5 * 1000 * 1000LL);
	if (g_app != NULL)
		(void)xtc_app_stop(g_app);
}

static int
run_one(uint64_t seed, int *out_replies, uint64_t *out_state)
{
	xtc_app_opts_t opts = XTC_APP_OPTS_DEFAULT;
	xtc_child_spec_t children[1 + N_CLIENTS];
	xtc_exec_t *exec;
	int i, rc;

	atomic_store(&g_replies_ok, 0);
	atomic_store(&g_clients_done, 0);
	atomic_store(&g_reg_count_seen, 0);
	g_app = NULL;
	g_reg = NULL;

	opts.name = "calc-app";
	opts.n_loops = N_LOOPS;
	/* Children are long-running services; keep them alive (permanent),
	 * but if one exits we do not want infinite restarts to perturb the
	 * count -- give a generous budget. */
	opts.sup.strategy = XTC_SUP_ONE_FOR_ONE;
	opts.sup.max_restarts = 100;

	if (xtc_app_create(&opts, &g_app) != XTC_OK)
		return -1;
	g_reg = xtc_app_registry(g_app);

	/* child 0 = server on loop 0; clients spread across loops 1..N. */
	memset(children, 0, sizeof(children));
	children[0].name = "server";
	children[0].fn = server;
	children[0].policy = XTC_RESTART_PERMANENT;
	children[0].loop = 0;
	for (i = 0; i < N_CLIENTS; i++) {
		children[1 + i].name = "client";
		children[1 + i].fn = client;
		children[1 + i].arg = (void *)(intptr_t)i;
		/* Clients are TEMPORARY: they run once and exit, no restart. */
		children[1 + i].policy = XTC_RESTART_TEMPORARY;
		children[1 + i].loop = 1 + (i % (N_LOOPS - 1));
	}

	if (xtc_app_start(g_app, children, 1 + N_CLIENTS) != XTC_OK) {
		xtc_app_destroy(g_app);
		return -1;
	}

	/* Coordinator on loop 0 stops the app once the clients finish. */
	(void)xtc_proc_spawn(xtc_app_loop(g_app), coordinator, NULL,
	    NULL, NULL);

	exec = xtc_app_exec(g_app);
	rc = xtc_sim_exec_run(exec, seed, 5000000);

	if (out_replies != NULL)
		*out_replies = atomic_load(&g_replies_ok);
	if (out_state != NULL)
		*out_state = xtc_sim_state_hash(exec);

	xtc_app_destroy(g_app);
	g_app = NULL;
	return rc;
}

int
main(int argc, char **argv)
{
	uint64_t base = 0x617070; /* "app" */
	int n = 20, i, fails = 0;

	if (argc > 1) base = strtoull(argv[1], NULL, 0);
	if (argc > 2) n = atoi(argv[2]);

	printf("== app-lifecycle DST: %d seeds from base 0x%llx ==\n",
	    n, (unsigned long long)base);

	for (i = 0; i < n; i++) {
		uint64_t seed = base + (uint64_t)i * 0x9E3779B97F4A7C15ull;
		int replies = 0, replies2 = 0, rc, rc2, pass = 1;
		uint64_t st = 0, st2 = 0;

		rc = run_one(seed, &replies, &st);
		if (rc != XTC_OK) pass = 0;
		else if (replies != N_CLIENTS) pass = 0;  /* all got value+1 */
		else if (atomic_load(&g_reg_count_seen) != 1) pass = 0;

		if (pass) {
			rc2 = run_one(seed, &replies2, &st2);
			if (rc2 != rc || replies2 != replies || st2 != st)
				pass = 0;
		}

		if (!pass) {
			printf("  seed 0x%016llx: FAIL (replies=%d/%d rc=%d "
			    "reg_count=%d)\n",
			    (unsigned long long)seed, replies, N_CLIENTS, rc,
			    atomic_load(&g_reg_count_seen));
			fails++;
		}
	}

	if (fails == 0) {
		printf("OK: app-lifecycle DST -- %d seeds, registry + "
		    "cross-loop request/reply + clean stop, all replay\n", n);
		return 0;
	}
	printf("FAIL: %d/%d app-lifecycle seeds failed\n", fails, n);
	return 1;
}
