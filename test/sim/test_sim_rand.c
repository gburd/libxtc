/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/sim/test_sim_rand.c
 *	DST: xtc_rand_u64 replays from the run's seed.  Off the simulator an
 *	un-seeded thread seeds its stream from the clock and the address of
 *	its state cell, which no seed can reproduce, so the determinism guard
 *	used to trap every un-seeded draw on a sim-reachable path.  Now such
 *	a draw takes the run's own stream.  This test draws from fibers on
 *	two loops inside seeded xtc_sim_exec_run runs and asserts for each
 *	seed that:
 *	  - the run returns XTC_OK (it refuses XTC_OK when the guard trips);
 *	  - a second run of the same seed draws the identical sequence;
 *	  - a different seed draws a different one;
 *	  - a thread that called xtc_rand_seed keeps its own sequence (the
 *	    one it asked for), under the simulator as off it.
 *	And off the simulator, two threads that never seeded draw different
 *	streams (the clock + address auto-seed is unchanged).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_sim.h"

#define N_LOOPS  2
#define N_FIBERS 4
#define N_DRAW   8
#define N_SEEDS  8

static uint64_t g_draw[N_FIBERS * N_DRAW];
static int      g_ndraw;
static int      g_done;
static xtc_exec_t *g_exec;

/* Each fiber draws N_DRAW values, yielding between draws so the
 * scheduler interleaves the fibers by the seed. */
static void
drawer(void *arg)
{
	int i;
	(void)arg;
	for (i = 0; i < N_DRAW; i++) {
		g_draw[g_ndraw++] = xtc_rand_u64();
		(void)xtc_proc_sleep(1000 * 1000LL);
	}
	if (++g_done == N_FIBERS)
		(void)xtc_exec_stop(g_exec);
}

static int
run_one(uint64_t seed, uint64_t *out)
{
	int rc, i;
	g_ndraw = 0;
	g_done = 0;
	if (xtc_exec_init(&g_exec, N_LOOPS) != XTC_OK)
		return -1;
	for (i = 0; i < N_FIBERS; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(g_exec, i % N_LOOPS), drawer,
		    NULL, NULL, NULL);
	rc = xtc_sim_exec_run(g_exec, seed, 1000000);
	(void)xtc_exec_fini(g_exec);
	memcpy(out, g_draw, sizeof g_draw);
	return (rc == XTC_OK && g_ndraw == N_FIBERS * N_DRAW) ? 0 : -1;
}

static void *
offsim_draw(void *arg)
{
	*(uint64_t *)arg = xtc_rand_u64();
	return NULL;
}

int
main(void)
{
	uint64_t a[N_FIBERS * N_DRAW], b[N_FIBERS * N_DRAW];
	uint64_t c[N_FIBERS * N_DRAW], x0, x1, y0, y1;
	pthread_t t0, t1;
	int i, fails = 0;

	for (i = 0; i < N_SEEDS; i++) {
		uint64_t seed = 0x72616e64ull + (uint64_t)i * 0x9E3779B97F4A7C15ull;
		if (run_one(seed, a) != 0 || run_one(seed, b) != 0 ||
		    run_one(seed + 1, c) != 0) {
			printf("  seed 0x%016llx: FAIL (run error or guard trip)\n",
			    (unsigned long long)seed);
			fails++;
			continue;
		}
		if (memcmp(a, b, sizeof a) != 0) {
			printf("  seed 0x%016llx: FAIL (replay draws differ)\n",
			    (unsigned long long)seed);
			fails++;
		}
		if (memcmp(a, c, sizeof a) == 0) {
			printf("  seed 0x%016llx: FAIL (seed+1 drew the same)\n",
			    (unsigned long long)seed);
			fails++;
		}
	}

	/* An explicitly seeded thread keeps its own sequence under sim. */
	xtc_rand_seed(42);
	x0 = xtc_rand_u64();
	x1 = xtc_rand_u64();
	xtc_sim_activate(7);
	xtc_rand_seed(42);
	y0 = xtc_rand_u64();
	y1 = xtc_rand_u64();
	xtc_sim_deactivate();
	if (x0 != y0 || x1 != y1) {
		printf("  FAIL: xtc_rand_seed(42) drew a different sequence "
		    "under the simulator\n");
		fails++;
	}

	/* Off the simulator, un-seeded threads still get distinct streams. */
	if (pthread_create(&t0, NULL, offsim_draw, &x0) != 0 ||
	    pthread_create(&t1, NULL, offsim_draw, &x1) != 0)
		return 1;
	(void)pthread_join(t0, NULL);
	(void)pthread_join(t1, NULL);
	if (x0 == x1) {
		printf("  FAIL: two un-seeded threads drew the same value off "
		    "the simulator\n");
		fails++;
	}

	if (fails) {
		printf("FAIL: %d check(s)\n", fails);
		return 1;
	}
	printf("OK: xtc_rand_u64 under DST -- %d seeds x %d fibers on %d loops, "
	    "no determinism-guard trip, every run replays its draws, seed+1 "
	    "differs; an explicit xtc_rand_seed keeps its sequence\n",
	    N_SEEDS, N_FIBERS, N_LOOPS);
	return 0;
}
