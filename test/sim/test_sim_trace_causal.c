/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * test/sim/test_sim_trace_causal.c
 *	DST gate for A3 (async causal trace).  The per-fiber suspend/resume
 *	ring is a debugging aid, but it HAS a deterministic-observable
 *	property: a fiber that parks a seeded number of times under the
 *	deterministic scheduler records EXACTLY that sequence of park/resume
 *	boundaries, and the same seed reproduces the identical recorded
 *	chain byte-for-byte.  This test proves it, replayably.
 *
 *	One run, for a given seed:
 *	  - N worker fibers across several loops each park (xtc_proc_sleep, a
 *	    timer park -- deterministic under the sim virtual clock) a
 *	    SEEDED number of times, then, WHILE STILL ALIVE, read their own
 *	    causal ring and fold it into a per-worker (count, hash) pair.
 *	  - INVARIANT: each worker's recorded PARK count equals the number
 *	    of times it actually parked (capped at the ring window), and its
 *	    chain strictly alternates PARK,RESUME.
 *	  - REPLAY: the same seed reproduces the identical per-worker counts,
 *	    the identical chain hash, and the identical scheduler state hash.
 *
 *	Fork-isolated per run (FoundationDB discipline): a fresh address
 *	space so pid/gen counters and RNG bookkeeping start pristine and the
 *	state hash is a pure function of the seed.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_async.h"
#include "xtc_sim.h"
#include "xtc_trace.h"

#define N_LOOPS    3
#define N_WORKERS  12
#define RING_CAP   16          /* must match XTC_PROC_CAUSAL_RING */

/* Per-worker observations (disjoint slots -> no cross-fiber races). */
static _Atomic int      g_parks[N_WORKERS];     /* PARK records seen */
static _Atomic int      g_alt_ok[N_WORKERS];    /* 1 if strictly alternating */
static _Atomic uint64_t g_hash[N_WORKERS];      /* FNV of the kind sequence */
static int              g_nsleep[N_WORKERS];     /* seeded sleep count */

static xtc_exec_t *g_exec;
struct warg { int id; };
static struct warg g_warg[N_WORKERS];

/* Fold one worker's own ring: count parks, verify strict alternation
 * (the FIRST record is always a PARK, then PARK/RESUME alternate), and
 * hash the kind sequence so a replay must reproduce it exactly. */
struct fold { int idx; int parks; int alt_ok; uint64_t h; };

static int
fold_cb(const xtc_causal_rec_t *rec, void *user)
{
	struct fold *f = user;
	int expect_park = (f->idx % 2) == 0;
	if (rec->kind == XTC_CAUSAL_PARK_TIMER)
		f->parks++;
	/* Alternation: even index = PARK, odd = RESUME. */
	if (expect_park) {
		if (rec->kind != XTC_CAUSAL_PARK_TIMER)
			f->alt_ok = 0;
	} else {
		if (rec->kind != XTC_CAUSAL_RESUME)
			f->alt_ok = 0;
	}
	f->h = (f->h ^ (uint64_t)(unsigned)rec->kind) * 0x100000001B3ull;
	f->idx++;
	return 0;
}

static void
worker(void *arg)
{
	struct warg *wa = arg;
	int w = wa->id;
	int i;
	struct fold f;

	for (i = 0; i < g_nsleep[w]; i++)
		(void)xtc_proc_sleep(1000000LL);   /* 1ms timer park */

	/* Read our OWN ring while still alive (the Cats Effect splice
	 * point is a live fiber). */
	f.idx = 0; f.parks = 0; f.alt_ok = 1; f.h = 1469598103934665603ull;
	(void)xtc_trace_causal_dump(xtc_self(), fold_cb, &f);

	atomic_store(&g_parks[w], f.parks);
	atomic_store(&g_alt_ok[w], f.alt_ok);
	atomic_store(&g_hash[w], f.h);
	xtc_exit_self(0);
}

static int
run_one_inproc(uint64_t seed, int *out_started, int *out_ok, uint64_t *out_state)
{
	int i, rc, started = 0, ok = 1;

	(void)xtc_fault_guard_install();
	(void)xtc_trace_causal_enable(1);   /* A3 ON for this run */

	for (i = 0; i < N_WORKERS; i++) {
		atomic_store(&g_parks[i], -1);
		atomic_store(&g_alt_ok[i], 0);
		atomic_store(&g_hash[i], 0);
		g_nsleep[i] = 0;
	}

	if (xtc_exec_init(&g_exec, N_LOOPS) != XTC_OK)
		return -1;
	xtc_exec_set_service_mode(g_exec, 1);

	/* Seeded per-worker sleep count in [1, 20] (drawn from the APP
	 * stream so it does not perturb scheduler streams).  Values > the
	 * ring window exercise eviction; the recorded park count is then
	 * capped at RING_CAP/2. */
	for (i = 0; i < N_WORKERS; i++)
		g_nsleep[i] = 1 + (int)__xtc_sim_rng_range(XTC_SIM_RNG_APP, 20);

	for (i = 0; i < N_WORKERS; i++) {
		xtc_loop_t *l = xtc_exec_loop(g_exec, i % N_LOOPS);
		g_warg[i].id = i;
		if (xtc_proc_spawn(l, worker, &g_warg[i], NULL, NULL) != XTC_OK) {
			(void)xtc_exec_fini(g_exec);
			g_exec = NULL;
			return -2;
		}
	}

	rc = xtc_sim_exec_run(g_exec, seed, 20000000);

	for (i = 0; i < N_WORKERS; i++) {
		int parks = atomic_load(&g_parks[i]);
		int alt = atomic_load(&g_alt_ok[i]);
		/* Expected recorded park count: min(nsleep, RING_CAP/2) --
		 * once the ring fills, the oldest are evicted and the visible
		 * window holds the last RING_CAP events = RING_CAP/2 parks.
		 * (A worker that parked N < 8 times shows all N; one that
		 * parked >= 8 times shows exactly 8 in the window.) */
		int cap = RING_CAP / 2;
		int expect = g_nsleep[i] < cap ? g_nsleep[i] : cap;
		if (parks < 0)
			continue;   /* worker did not run (should not happen) */
		started++;
		if (parks != expect || !alt)
			ok = 0;
	}

	if (out_started) *out_started = started;
	if (out_ok)      *out_ok = ok;
	if (out_state)   *out_state = xtc_sim_state_hash(g_exec);

	(void)xtc_exec_fini(g_exec);
	g_exec = NULL;
	return rc;
}

struct run_result {
	int      rc;
	int      started;
	int      ok;
	uint64_t hashmix;   /* xor of all per-worker chain hashes */
	uint64_t state;
};

/* Mix per-worker chain hashes so the replay comparison covers the exact
 * recorded sequences, not just their counts. */
static uint64_t
mix_hashes(void)
{
	uint64_t m = 0;
	int i;
	for (i = 0; i < N_WORKERS; i++)
		m ^= atomic_load(&g_hash[i]) + (uint64_t)i * 0x9E3779B97F4A7C15ull;
	return m;
}

static int
run_one(uint64_t seed, int *out_started, int *out_ok, uint64_t *out_hashmix,
    uint64_t *out_state)
{
	int pfd[2];
	pid_t pid;
	struct run_result rr;

	if (pipe(pfd) != 0)
		return -3;
	pid = fork();
	if (pid < 0) {
		close(pfd[0]); close(pfd[1]);
		return -3;
	}
	if (pid == 0) {
		ssize_t wn;
		close(pfd[0]);
		memset(&rr, 0, sizeof rr);
		rr.rc = run_one_inproc(seed, &rr.started, &rr.ok, &rr.state);
		rr.hashmix = mix_hashes();
		wn = write(pfd[1], &rr, sizeof rr);
		close(pfd[1]);
		_exit(wn == (ssize_t)sizeof rr ? 0 : 2);
	}
	close(pfd[1]);
	{
		size_t got = 0;
		int status;
		while (got < sizeof rr) {
			ssize_t n = read(pfd[0], (char *)&rr + got,
			    sizeof rr - got);
			if (n <= 0)
				break;
			got += (size_t)n;
		}
		close(pfd[0]);
		(void)waitpid(pid, &status, 0);
		if (got != sizeof rr)
			return -4;
	}
	if (out_started) *out_started = rr.started;
	if (out_ok)      *out_ok = rr.ok;
	if (out_hashmix) *out_hashmix = rr.hashmix;
	if (out_state)   *out_state = rr.state;
	return rr.rc;
}

int
main(int argc, char **argv)
{
	uint64_t base = 0x4341555341ull;   /* "CAUSA" */
	int n = 40, i, fails = 0;

	if (argc > 1) base = strtoull(argv[1], NULL, 0);
	if (argc > 2) n = atoi(argv[2]);

	printf("== causal-trace DST: %d seeds from base 0x%llx "
	    "(%d workers, %d loops) ==\n", n, (unsigned long long)base,
	    N_WORKERS, N_LOOPS);

	for (i = 0; i < n; i++) {
		uint64_t seed = base + (uint64_t)i * 0x9E3779B97F4A7C15ull;
		int st = 0, ok = 0, st2 = 0, ok2 = 0;
		uint64_t hm = 0, hm2 = 0, sh = 0, sh2 = 0;
		int rc, rc2, pass = 1;

		rc = run_one(seed, &st, &ok, &hm, &sh);
		if (rc != XTC_OK) pass = 0;
		else if (st != N_WORKERS) pass = 0;   /* all workers ran */
		else if (ok != 1) pass = 0;           /* counts + alternation */

		if (pass) {
			rc2 = run_one(seed, &st2, &ok2, &hm2, &sh2);
			if (rc2 != rc || st2 != st || ok2 != ok ||
			    hm2 != hm || sh2 != sh)
				pass = 0;
		}

		if (!pass) {
			printf("  seed 0x%016llx: FAIL (started=%d ok=%d rc=%d; "
			    "replay hm=%016llx/%016llx sh=%016llx/%016llx)\n",
			    (unsigned long long)seed, st, ok, rc,
			    (unsigned long long)hm, (unsigned long long)hm2,
			    (unsigned long long)sh, (unsigned long long)sh2);
			fails++;
		}
	}

	if (fails == 0) {
		printf("OK: causal-trace DST -- %d seeds, every worker's "
		    "recorded park/resume chain matched its actual seeded park "
		    "count (ring-window capped), strictly alternated "
		    "PARK,RESUME, and reproduced byte-identically on replay "
		    "(same chain hash + same scheduler state hash)\n", n);
		return 0;
	}
	printf("FAIL: %d/%d causal-trace DST seeds failed\n", fails, n);
	return 1;
}
