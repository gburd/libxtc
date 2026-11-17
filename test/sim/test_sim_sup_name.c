/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/sim/test_sim_sup_name.c
 *	Deterministic Simulation Testing of supervisor-owned child names
 *	(2.2: xtc_child_spec_t.reg_name) and of xtc_reg_whereis_wait.
 *
 *	A multi-loop app's one_for_all root supervisor runs a named,
 *	PERMANENT "engine" that serves requests and crashes after a seeded
 *	number of them, and an "aux" sibling whose exit hook takes 20 ms, so
 *	every restart has a 20 ms window in which the engine is dead and not
 *	yet respawned.  Client procs on the other loops (not supervised)
 *	find the engine by name only: xtc_reg_whereis_wait(reg, "engine",
 *	last_dead, ...), a monitor, a send and a reply wait; on the DOWN they
 *	wait for the next incarnation with the dead pid as not_pid.  Seeded
 *	cross-loop latency is on.
 *
 *	Invariants (per seed):
 *	  (a) every client request is eventually answered;
 *	  (b) a name wait lasts at most one restart (well under 500 ms of
 *	      virtual time): longer is a lost wakeup, the wait being ended by
 *	      its 2 s timer instead of the registration (planted as
 *	      REG_WAIT_NOWAKE: "a name wait was not woken by the
 *	      registration");
 *	  (c) 5 ms after a client sees the engine's DOWN, inside the 20 ms
 *	      window, the registry no longer names the dead incarnation, and
 *	      after the app stops it names nothing (planted as
 *	      SUP_NAME_STALE: "the registry named a dead incarnation");
 *	  (d) the run quiesces and REPLAYS (same per-seed trace and sim
 *	      state hash).
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
#include "xtc_proc.h"
#include "xtc_reg.h"
#include "sim_int.h"

#define N_LOOPS   3
#define N_CLIENTS 4
#define N_REQS    12
#define MS        (1000LL * 1000)

static xtc_app_t *g_app;
static xtc_reg_t *g_reg;
static int g_answered, g_clients_done, g_lost_wake, g_stale, g_crashes;
static uint64_t g_trace;

struct req {
	xtc_pid_t from;
	int       v;
};

/* The slow sibling: its exit takes 20 ms (an exit hook that parks;
 * hooks run masked, so the kill does not cut it short). */
static void
aux_gone(void *arg)
{
	(void)arg;
	(void)xtc_proc_sleep(20 * MS);
}

static void
aux(void *arg)
{
	void *m;
	size_t sz;
	(void)arg;
	(void)xtc_proc_at_exit(aux_gone, NULL);
	for (;;)
		if (xtc_recv(&m, &sz, -1) == XTC_OK)
			free(m);
}

static void
engine(void *arg)
{
	void *m;
	size_t sz;
	int served = 0, life;
	(void)arg;
	life = 1 + (int)__xtc_sim_rng_range(XTC_SIM_RNG_APP, 6);
	for (;;) {
		if (xtc_recv(&m, &sz, -1) != XTC_OK)
			return;
		if (sz == sizeof(struct req)) {
			struct req *r = m;
			int reply = r->v + 1;
			(void)xtc_send(r->from, &reply, sizeof reply);
			if (++served >= life) {
				free(m);
				xtc_atomic_fetch_add_i32(&g_crashes, 1,
				    xtc_mo_seq_cst);
				(void)xtc_exit_self(3);
			}
		}
		free(m);
	}
}

/* A reply carrying v+1, or the DOWN of monitor `ref`. */
struct want {
	int      v;
	uint64_t ref;
};

static int
match_reply_or_down(const void *data, size_t size, void *u)
{
	const struct want *w = u;
	xtc_down_info_t di = XTC_DOWN_INFO_INIT;
	if (size == sizeof(int))
		return *(const int *)data == w->v + 1;
	return xtc_down_decode_ex(data, size, &di) == XTC_OK &&
	    di.ref == w->ref;
}

/* xtc_reg_visit callback: if the entry names *u (the dead pid), clear
 * *u to XTC_PID_NONE as the "stale" mark.  An exact lookup, unlike
 * xtc_reg_get, which may report a simulated transient miss. */
static int
stale_fn(xtc_pid_t pid, void *value, void *u)
{
	xtc_pid_t *dead = u;
	(void)value;
	if (xtc_pid_eq(pid, *dead))
		*dead = XTC_PID_NONE;
	return 0;
}

static void
client(void *arg)
{
	int id = (int)(intptr_t)arg, i, rc;
	xtc_pid_t dead = XTC_PID_NONE, eng, probe;
	struct req rq;
	struct want w;
	void *m;
	size_t sz;
	xtc_down_info_t di = XTC_DOWN_INFO_INIT;

	for (i = 0; i < N_REQS; ) {
		int64_t t0 = xtc_clock_mono();
		rc = xtc_reg_whereis_wait(g_reg, "engine", dead, 2000 * MS,
		    &eng);
		/* One restart is the 20 ms window plus message latency; a
		 * wait ended by its timer is not woken by the registration. */
		if (xtc_clock_mono() - t0 > 500 * MS)
			xtc_atomic_fetch_add_i32(&g_lost_wake, 1,
			    xtc_mo_seq_cst);
		if (rc != XTC_OK)
			continue;
		if (xtc_monitor(eng, &w.ref) != XTC_OK)
			continue;
		rq.from = xtc_self();
		rq.v = id * 1000 + i;
		w.v = rq.v;
		if (xtc_send(eng, &rq, sizeof rq) != XTC_OK) {
			(void)xtc_demonitor(w.ref, XTC_DEMONITOR_FLUSH);
			dead = eng;
			continue;
		}
		rc = xtc_recv_match(match_reply_or_down, &w, &m, &sz, -1);
		if (rc == XTC_OK && sz == sizeof(int)) {
			free(m);
			(void)xtc_demonitor(w.ref, XTC_DEMONITOR_FLUSH);
			xtc_atomic_fetch_add_i32(&g_answered, 1,
			    xtc_mo_seq_cst);
			g_trace = g_trace * 1000003u + (uint64_t)rq.v;
			i++;
			continue;
		}
		if (rc == XTC_OK) {
			(void)xtc_down_decode_ex(m, sz, &di);
			free(m);
			/* The supervisor monitors the engine too and drops
			 * its name on the DOWN; 5 ms later (latency is at
			 * most 300 us) the name must not name the dead pid. */
			(void)xtc_proc_sleep(5 * MS);
			probe = di.pid;
			if (xtc_reg_visit(g_reg, "engine", stale_fn, &probe,
			        NULL) == XTC_OK && xtc_pid_is_none(probe))
				xtc_atomic_fetch_add_i32(&g_stale, 1,
				    xtc_mo_seq_cst);
			dead = di.pid;
		}
	}
	xtc_atomic_fetch_add_i32(&g_clients_done, 1, xtc_mo_seq_cst);
}

static void
coordinator(void *arg)
{
	int t;
	(void)arg;
	for (t = 0; t < 20000; t++) {
		if (xtc_atomic_load_i32(&g_clients_done, xtc_mo_seq_cst) >=
		    N_CLIENTS)
			break;
		(void)xtc_proc_sleep(1 * MS);
	}
	(void)xtc_app_stop(g_app);
}

static int
run_one(uint64_t seed, uint64_t *out_trace, uint64_t *out_state)
{
	xtc_app_opts_t ao = XTC_APP_OPTS_INIT;
	xtc_sup_opts_t so = XTC_SUP_OPTS_INIT;
	xtc_child_spec_t kids[2];
	xtc_exec_t *e;
	xtc_pid_t probe;
	int i, rc;

	g_answered = g_clients_done = g_lost_wake = g_stale = g_crashes = 0;
	g_trace = 0;
	ao.name = "names";
	ao.n_loops = N_LOOPS;
	ao.no_tuning_check = 1;
	so.strategy = XTC_SUP_ONE_FOR_ALL;
	so.max_restarts = 1000;
	so.period_ns = 1000LL * 1000 * MS;
	ao.sup = &so;
	if (xtc_app_create(&ao, &g_app) != XTC_OK)
		return -1;
	g_reg = xtc_app_registry(g_app);
	memset(kids, 0, sizeof kids);
	kids[0].size = sizeof kids[0];
	kids[0].name = "engine";
	kids[0].reg_name = "engine";
	kids[0].fn = engine;
	kids[0].policy = XTC_RESTART_PERMANENT;
	kids[0].loop = 0;
	kids[1].size = sizeof kids[1];
	kids[1].name = "aux";
	kids[1].fn = aux;
	kids[1].policy = XTC_RESTART_PERMANENT;
	kids[1].loop = 0;
	if (xtc_app_start(g_app, kids, 2) != XTC_OK) {
		xtc_app_destroy(g_app);
		return -1;
	}
	(void)xtc_proc_spawn(xtc_app_loop(g_app), coordinator, NULL, NULL,
	    NULL);
	e = xtc_app_exec(g_app);
	/* Clients are not supervised: a one_for_all restart respawns every
	 * child. */
	for (i = 0; i < N_CLIENTS; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e, 1 + i % (N_LOOPS - 1)),
		    client, (void *)(intptr_t)i, NULL, NULL);
	xtc_sim_net_latency(10 * 1000LL, 300 * 1000LL);
	rc = xtc_sim_exec_run(e, seed, 20000000);
	xtc_sim_net_latency(0, 0);
	/* The stop took the last incarnation down: its name is gone. */
	probe = XTC_PID_NONE;
	if (xtc_reg_visit(g_reg, "engine", stale_fn, &probe, NULL) == XTC_OK)
		g_stale++;
	*out_trace = g_trace * 31u + (uint64_t)g_crashes;
	*out_state = xtc_sim_state_hash(e);
	xtc_app_destroy(g_app);
	g_app = NULL;
	return rc;
}

int
main(int argc, char **argv)
{
	uint64_t base = 0, seed, tr, tr2, st, st2;
	int n = 16, k, fails = 0, rc, rc2, crashes = 0;

	if (argc > 1) base = strtoull(argv[1], NULL, 0);
	if (argc > 2) n = atoi(argv[2]);
	printf("== supervisor child-name DST: %d seeds from base %llu ==\n",
	    n, (unsigned long long)base);
	for (k = 1; k <= n; k++) {
		int pass = 1;
		seed = 0x9E3779B97F4A7C15ull * (base + (uint64_t)k);
		rc = run_one(seed, &tr, &st);
		crashes += g_crashes;
		if (g_lost_wake > 0) {
			printf("  seed %llu: a name wait was not woken by the "
			       "registration (%d)\n",
			    (unsigned long long)(base + (uint64_t)k),
			    g_lost_wake);
			pass = 0;
		}
		if (g_stale > 0) {
			printf("  seed %llu: the registry named a dead "
			       "incarnation (%d)\n",
			    (unsigned long long)(base + (uint64_t)k), g_stale);
			pass = 0;
		}
		if (rc != XTC_OK || g_answered != N_CLIENTS * N_REQS) {
			printf("  seed %llu: rc=%d answered=%d/%d\n",
			    (unsigned long long)(base + (uint64_t)k), rc,
			    g_answered, N_CLIENTS * N_REQS);
			pass = 0;
		}
		if (pass) {
			rc2 = run_one(seed, &tr2, &st2);
			if (rc2 != rc || tr2 != tr || st2 != st) {
				printf("  seed %llu: replay diverged\n",
				    (unsigned long long)(base + (uint64_t)k));
				pass = 0;
			}
		}
		if (!pass)
			fails++;
	}
	if (fails != 0) {
		printf("FAIL: %d/%d child-name seeds failed\n", fails, n);
		return 1;
	}
	if (crashes == 0) {
		printf("FAIL: the engine never crashed; nothing was tested\n");
		return 1;
	}
	printf("OK: supervisor child-name DST -- %d seeds, %d engine "
	       "restarts, every request answered by name, no lost name wake, "
	       "no stale name, all replay\n", n, crashes);
	return 0;
}
