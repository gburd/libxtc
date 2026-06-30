#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <stdint.h>
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_sim.h"

/*
 * DST phase 6 -- scale / soak.  Sweeps many seeds; for each seed runs a
 * substantial mixed multi-loop workload under the deterministic
 * scheduler twice and asserts:
 *
 *   - every seed reaches quiescence (no hang / livelock);
 *   - the per-step structural invariants hold (xtc_sim_exec_run aborts
 *     with XTC_E_INTERNAL on any violation -- we check the return);
 *   - the run REPLAYS: two runs of the same seed produce the identical
 *     state hash AND application result;
 *   - across the sweep the scheduler genuinely EXPLORES: the seeds
 *     produce many distinct state hashes (not all the same schedule).
 *
 * The workload mixes the concurrency surfaces DST is meant to stress:
 * many cross-loop ping/pong pairs (mailbox park/wake across loops) and
 * timer-driven sleepers (the virtual clock).  Both patterns provably
 * terminate, so any non-termination is a scheduler/runtime defect, not
 * a workload bug.
 *
 * Seed count is configurable via argv[1] (default 200) so CI runs a
 * quick sweep and a soak run can crank it up.
 */

#define N_LOOPS    4
#define N_PAIRS    24       /* cross-loop ping/pong pairs */
#define N_SLEEPERS 8        /* timer-driven procs */
#define N_HOPS     4        /* round-trips per pair */

static atomic_int  g_replies;
static atomic_int  g_sleeps;
static atomic_long g_app_hash;

/* ---- ping/pong: a ping does N_HOPS round-trips with a pong on another
 * loop, parking on each reply; both terminate after N_HOPS. ---- */
static void
pong(void *arg)
{
	(void)arg;
	int hops = N_HOPS;
	while (hops-- > 0) {
		void *m = NULL;
		size_t n = 0;
		xtc_pid_t from;
		if (xtc_recv(&m, &n, -1) != XTC_OK || m == NULL)
			return;
		memcpy(&from, m, sizeof from);
		free(m);
		int r = 1;
		(void)xtc_send(from, &r, sizeof r);
	}
}

struct ping_arg { xtc_pid_t peer; long id; };
static struct ping_arg g_args[N_PAIRS];

static void
ping(void *arg)
{
	struct ping_arg *pa = arg;
	xtc_pid_t self = xtc_self();
	int hops = N_HOPS;
	while (hops-- > 0) {
		void *m = NULL;
		size_t n = 0;
		long h;
		(void)xtc_send(pa->peer, &self, sizeof self);
		if (xtc_recv(&m, &n, -1) != XTC_OK || m == NULL)
			return;
		free(m);
		atomic_fetch_add_explicit(&g_replies, 1, memory_order_relaxed);
		h = atomic_load_explicit(&g_app_hash, memory_order_relaxed);
		h = h * 1000003L + (pa->id + 1);
		atomic_store_explicit(&g_app_hash, h, memory_order_relaxed);
	}
}

/* ---- timer sleeper: sleeps a few times on the virtual clock. ---- */
static void
sleeper(void *arg)
{
	long id = (long)(intptr_t)arg;
	int i;
	for (i = 0; i < 3; i++)
		(void)xtc_proc_sleep((int64_t)(id % 5 + 1) * 1000000LL); /* 1-5 ms */
	atomic_fetch_add_explicit(&g_sleeps, 1, memory_order_relaxed);
}

/* Build + run the workload once with `seed`. */
static int
run_once(uint64_t seed, uint64_t *out_state, long *out_app, int *out_done)
{
	xtc_exec_t *e = NULL;
	int i, rc;
	atomic_store(&g_replies, 0);
	atomic_store(&g_sleeps, 0);
	atomic_store(&g_app_hash, 0);

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK)
		return -1;

	for (i = 0; i < N_PAIRS; i++) {
		xtc_loop_t *lp = xtc_exec_loop(e, (unsigned)(i % N_LOOPS));
		xtc_loop_t *li = xtc_exec_loop(e, (unsigned)((i + 1) % N_LOOPS));
		xtc_pid_t pong_pid;
		(void)xtc_proc_spawn(lp, pong, NULL, NULL, &pong_pid);
		g_args[i].peer = pong_pid;
		g_args[i].id = i;
		(void)xtc_proc_spawn(li, ping, &g_args[i], NULL, NULL);
	}
	for (i = 0; i < N_SLEEPERS; i++) {
		xtc_loop_t *l = xtc_exec_loop(e, (unsigned)(i % N_LOOPS));
		(void)xtc_proc_spawn(l, sleeper, (void *)(intptr_t)i, NULL, NULL);
	}

	rc = xtc_sim_exec_run(e, seed, 5000000);
	*out_state = xtc_sim_state_hash(e);
	*out_app = atomic_load(&g_app_hash);
	*out_done = atomic_load(&g_replies) + atomic_load(&g_sleeps);
	(void)xtc_exec_fini(e);
	return rc;
}

int
main(int argc, char **argv)
{
	long n_seeds = (argc > 1) ? strtol(argv[1], NULL, 10) : 200;
	long s;
	int expect_done = N_PAIRS * N_HOPS + N_SLEEPERS;
	uint64_t seen[256];
	int n_seen = 0;
	int failures = 0;

	if (n_seeds < 1)
		n_seeds = 1;

	for (s = 0; s < n_seeds; s++) {
		uint64_t seed = 0x9E3779B97F4A7C15ull * (uint64_t)(s + 1);
		uint64_t st1 = 0, st2 = 0;
		long app1 = 0, app2 = 0;
		int done1 = 0, done2 = 0, rc1, rc2, i;

		rc1 = run_once(seed, &st1, &app1, &done1);
		rc2 = run_once(seed, &st2, &app2, &done2);

		if (rc1 != XTC_OK || rc2 != XTC_OK) {
			printf("FAIL seed=%llu: rc1=%d rc2=%d (invariant "
			    "violation or budget exhaustion)\n",
			    (unsigned long long)seed, rc1, rc2);
			failures++;
			continue;
		}
		if (done1 != expect_done || done2 != expect_done) {
			printf("FAIL seed=%llu: done1=%d done2=%d (want %d) -- "
			    "did not reach quiescence\n",
			    (unsigned long long)seed, done1, done2, expect_done);
			failures++;
			continue;
		}
		if (st1 != st2 || app1 != app2) {
			printf("FAIL seed=%llu: replay mismatch "
			    "(state %016llx/%016llx app %ld/%ld)\n",
			    (unsigned long long)seed,
			    (unsigned long long)st1, (unsigned long long)st2,
			    app1, app2);
			failures++;
			continue;
		}
		for (i = 0; i < n_seen; i++)
			if (seen[i] == st1)
				break;
		if (i == n_seen && n_seen < (int)(sizeof seen / sizeof seen[0]))
			seen[n_seen++] = st1;
	}

	printf("swept %ld seeds: %d failures, %d distinct schedules "
	    "(state hashes)\n", n_seeds, failures, n_seen);

	if (failures > 0) {
		printf("FAIL: %d seed(s) failed\n", failures);
		return 1;
	}
	if (n_seeds >= 20 && n_seen < 2) {
		printf("FAIL: the sweep explored only one schedule -- the "
		    "scheduler is not seed-sensitive\n");
		return 1;
	}
	printf("OK: %ld-seed soak -- every seed reached quiescence, replayed "
	    "identically, invariants held; %d distinct schedules explored\n",
	    n_seeds, n_seen);
	return 0;
}
