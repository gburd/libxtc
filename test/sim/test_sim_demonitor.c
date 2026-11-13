/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/sim/test_sim_demonitor.c
 *	Deterministic Simulation Testing of xtc_demonitor (2.2) against the
 *	exit of the monitored proc, across loops, with the simulator's
 *	seeded cross-loop message latency on, so a DOWN can be in flight
 *	(sent by the exiting target, not yet in the watcher's mailbox) at
 *	the moment the watcher turns the monitor off.
 *
 *	Each watcher, on its own loop, runs ROUNDS rounds: spawn a target
 *	on another loop that exits after a seeded delay, monitor it, wait a
 *	seeded time, then xtc_demonitor(ref, flags) with XTC_DEMONITOR_FLUSH
 *	on half the rounds.  The seeded delays put the demonitor before,
 *	around and after the target's exit.
 *
 *	Invariants (per seed):
 *	  (a) xtc_demonitor returned XTC_OK (the monitor was still on) =>
 *	      no DOWN for that ref is ever delivered: a DOWN in flight at the
 *	      call is dropped, not queued later.  2.2 records the cancel in
 *	      the watcher's entry under the watcher's mailbox lock, which the
 *	      delivery takes (planted as DEMON_INFLIGHT: the delivery ignores
 *	      the cancel);
 *	  (b) it returned XTC_E_NOTFOUND => the DOWN reached the mailbox,
 *	      and with FLUSH it is gone from it after the call, without
 *	      FLUSH it is still there;
 *	  (c) every round ends with exactly one of those, and the run
 *	      quiesces;
 *	  (d) REPLAY: the same seed gives the same per-round outcomes and sim
 *	      state hash.
 *
 *	argv: [base [count]] -- seeds base+1 .. base+count (default 0 24).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_sim.h"

#define N_LOOPS    3
#define N_WATCHERS 3
#define ROUNDS     16
#define MS         (1000LL * 1000)

struct watch {
	xtc_exec_t *exec;
	int         id;
	int         n_ok;        /* demonitor said: still on */
	int         n_fired;     /* demonitor said: already delivered */
	int         late_down;   /* a DOWN for an OK-cancelled ref */
	int         flush_left;  /* FLUSH said NOTFOUND, DOWN still queued */
	int         noflush_gone;/* no FLUSH, NOTFOUND, DOWN not queued */
	uint64_t    trace;       /* order-sensitive fold of the outcomes */
};

static struct watch g_w[N_WATCHERS];
static int g_done;

/* The target: exit after a seeded delay (0..3 ms of virtual time). */
static void
target(void *arg)
{
	int64_t ns = (int64_t)(intptr_t)arg;
	if (ns > 0)
		(void)xtc_proc_sleep(ns);
	(void)xtc_exit_self(9);
}

/* Is a DOWN for `ref` in the caller's mailbox?  Takes it if so. */
static int
take_down(uint64_t ref, int64_t timeout_ns)
{
	void *m = NULL;
	size_t sz = 0;
	xtc_down_info_t di = XTC_DOWN_INFO_INIT;
	int found = 0;
	while (xtc_recv(&m, &sz, timeout_ns) == XTC_OK) {
		if (xtc_down_decode_ex(m, sz, &di) == XTC_OK && di.ref == ref)
			found = 1;
		free(m);
		timeout_ns = 0;
	}
	return found;
}

static void
watcher(void *arg)
{
	struct watch *w = arg;
	xtc_loop_t *tl = xtc_exec_loop(w->exec,
	    (w->id + 1) % N_LOOPS);
	int r, rc, flush, queued;
	xtc_pid_t t;
	uint64_t ref;
	int64_t die, wait;

	for (r = 0; r < ROUNDS; r++) {
		die = (int64_t)__xtc_sim_rng_range(XTC_SIM_RNG_APP, 4) * MS;
		wait = (int64_t)__xtc_sim_rng_range(XTC_SIM_RNG_APP, 4) * MS;
		flush = (int)__xtc_sim_rng_range(XTC_SIM_RNG_APP, 2);
		if (xtc_proc_spawn(tl, target, (void *)(intptr_t)die, NULL,
		        &t) != XTC_OK ||
		    xtc_monitor(t, &ref) != XTC_OK)
			continue;
		if (wait > 0)
			(void)xtc_proc_sleep(wait);
		rc = xtc_demonitor(ref, flush ? XTC_DEMONITOR_FLUSH : 0);
		/* Long enough for any in-flight DOWN (latency <= 500 us) and
		 * the target's exit (<= 3 ms) to land. */
		queued = take_down(ref, 5 * MS);
		if (rc == XTC_OK) {
			w->n_ok++;
			if (queued)
				w->late_down++;
		} else if (rc == XTC_E_NOTFOUND) {
			w->n_fired++;
			if (flush && queued)
				w->flush_left++;
			if (!flush && !queued)
				w->noflush_gone++;
		}
		w->trace = w->trace * 1000003u + (uint64_t)(rc + 100) * 4u +
		    (uint64_t)flush * 2u + (uint64_t)queued;
	}
	xtc_atomic_fetch_add_i32(&g_done, 1, xtc_mo_seq_cst);
}

static int
run_one(uint64_t seed, uint64_t *out_trace, uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	int i, rc;
	uint64_t tr = 0;

	memset(g_w, 0, sizeof g_w);
	xtc_atomic_store_i32(&g_done, 0, xtc_mo_seq_cst);
	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK)
		return -1;
	xtc_sim_net_latency(10 * 1000LL, 500 * 1000LL);
	for (i = 0; i < N_WATCHERS; i++) {
		g_w[i].exec = e;
		g_w[i].id = i;
		(void)xtc_proc_spawn(xtc_exec_loop(e, i % N_LOOPS), watcher,
		    &g_w[i], NULL, NULL);
	}
	rc = xtc_sim_exec_run(e, seed, 5000000);
	xtc_sim_net_latency(0, 0);
	for (i = 0; i < N_WATCHERS; i++)
		tr = tr * 31u + g_w[i].trace;
	*out_trace = tr;
	*out_state = xtc_sim_state_hash(e);
	(void)xtc_exec_fini(e);
	return rc;
}

int
main(int argc, char **argv)
{
	uint64_t base = 0, seed, tr, tr2, st, st2;
	int n = 24, k, i, fails = 0, rc, rc2, ok = 0, fired = 0;

	if (argc > 1) base = strtoull(argv[1], NULL, 0);
	if (argc > 2) n = atoi(argv[2]);
	printf("== demonitor DST: %d seeds from base %llu ==\n", n,
	    (unsigned long long)base);
	for (k = 1; k <= n; k++) {
		int late = 0, left = 0, gone = 0, rounds = 0, pass = 1;
		seed = 0x9E3779B97F4A7C15ull * (base + (uint64_t)k);
		rc = run_one(seed, &tr, &st);
		for (i = 0; i < N_WATCHERS; i++) {
			late += g_w[i].late_down;
			left += g_w[i].flush_left;
			gone += g_w[i].noflush_gone;
			rounds += g_w[i].n_ok + g_w[i].n_fired;
			ok += g_w[i].n_ok;
			fired += g_w[i].n_fired;
		}
		if (late > 0) {
			printf("  seed %llu: a DOWN arrived after xtc_demonitor "
			       "returned XTC_OK (%d rounds)\n",
			    (unsigned long long)(base + (uint64_t)k), late);
			pass = 0;
		}
		if (left > 0 || gone > 0) {
			printf("  seed %llu: FLUSH left %d DOWNs queued; %d "
			       "fired DOWNs missing without FLUSH\n",
			    (unsigned long long)(base + (uint64_t)k), left,
			    gone);
			pass = 0;
		}
		if (rc != XTC_OK || rounds != N_WATCHERS * ROUNDS ||
		    xtc_atomic_load_i32(&g_done, xtc_mo_seq_cst) != N_WATCHERS) {
			printf("  seed %llu: rc=%d rounds=%d/%d\n",
			    (unsigned long long)(base + (uint64_t)k), rc,
			    rounds, N_WATCHERS * ROUNDS);
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
		printf("FAIL: %d/%d demonitor seeds failed\n", fails, n);
		return 1;
	}
	/* Both outcomes must have been exercised, or the race was not. */
	if (ok == 0 || fired == 0) {
		printf("FAIL: demonitor outcomes not both reached (ok=%d "
		       "fired=%d)\n", ok, fired);
		return 1;
	}
	printf("OK: demonitor DST -- %d seeds, %d cancels before the DOWN "
	       "(none delivered later), %d after it (FLUSH exact), cross-loop "
	       "latency on, all replay\n", n, ok, fired);
	return 0;
}
