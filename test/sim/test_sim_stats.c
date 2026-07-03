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
#include "xtc_stats.h"
#include "xtc_async.h"     /* xtc_yield */
#include "xtc_sim.h"

/*
 * DST coverage of the stats counters/gauges/histograms
 * (src/ptc/stats.c): per-CPU sharded, atomic; a counter add is an
 * atomic add to the caller's shard, a read sums all shards.  NON-
 * blocking -- no cond_wait, no shim.  Under the single-thread sim
 * every fiber runs on one CPU so all shards collapse to one, which
 * makes this primarily a DETERMINISM + accounting check: the read of a
 * shared counter must equal the exact sum of all fibers' increments
 * regardless of the seeded interleaving, and the whole run must replay.
 *
 * Three shared metrics driven by W fibers across N loops over ITERS
 * rounds each:
 *   counter -- each fiber does inc + add across yields; final read must
 *            equal the exact total (W * ITERS increments + the adds).
 *   gauge   -- each fiber add/sub a balanced delta; final read must be
 *            the exact net (0 here -- adds and subs cancel).
 *   hist    -- each fiber records ITERS samples; hist_count must equal
 *            the exact number of samples, and a quantile must fall
 *            inside the recorded range.
 *
 * INVARIANTS: (a) quiescence (rc == XTC_OK); (b) counter read == exact
 * sum of increments (no lost/double count), gauge net exact, hist count
 * exact + quantile in range; (c) byte-identical REPLAY (the metric
 * values + sim state hash -- deterministic given the schedule); (d) a
 * different seed reorders but yields the SAME totals (the totals are
 * schedule-independent; only the sim state hash may differ).  Footprint
 * tiny + per-run free.
 */

#define N_LOOPS 4
#define WORKERS 8
#define ITERS   5
#define ADD_STEP 3

static xtc_counter_t *g_ctr;
static xtc_gauge_t   *g_gauge;
static xtc_hist_t    *g_hist;
static atomic_int     g_done;

static void
stats_worker(void *arg)
{
	int id = (int)(intptr_t)arg;
	int it;

	for (it = 0; it < ITERS; it++) {
		xtc_counter_inc(g_ctr);                 /* +1 */
		xtc_counter_add(g_ctr, ADD_STEP);       /* +ADD_STEP */
		xtc_yield();
		xtc_gauge_add(g_gauge, id + 1);         /* balanced below */
		xtc_hist_record(g_hist, (int64_t)((id + 1) * 1000 + it));
		xtc_yield();
		xtc_gauge_add(g_gauge, -(id + 1));      /* net 0 */
	}
	atomic_fetch_add_explicit(&g_done, 1, memory_order_relaxed);
}

static int
run_stats(uint64_t seed, int *out_done, uint64_t *out_ctr, int64_t *out_gauge,
    uint64_t *out_hcount, int *out_q_ok, uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	int i, rc;

	atomic_store(&g_done, 0);
	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK) return -1;
	if (xtc_counter_create("dst.ctr", &g_ctr) != XTC_OK ||
	    xtc_gauge_create("dst.gauge", &g_gauge) != XTC_OK ||
	    xtc_hist_create("dst.hist", &g_hist) != XTC_OK) {
		(void)xtc_exec_fini(e); return -1;
	}
	for (i = 0; i < WORKERS; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e, (unsigned)(i % N_LOOPS)),
		    stats_worker, (void *)(intptr_t)i, NULL, NULL);

	rc = xtc_sim_exec_run(e, seed, 5000000);

	*out_done = atomic_load(&g_done);
	*out_ctr = xtc_counter_read(g_ctr);
	*out_gauge = xtc_gauge_read(g_gauge);
	*out_hcount = xtc_hist_count(g_hist);
	{
		int64_t q = xtc_hist_quantile(g_hist, 0.5);
		/* Samples range from 1000 (id0,it0) to 8004 (id7,it4). */
		*out_q_ok = (q >= 1000 && q <= 8004) ? 1 : 0;
	}
	if (out_state) *out_state = xtc_sim_state_hash(e);
	xtc_hist_destroy(g_hist);   g_hist = NULL;
	xtc_gauge_destroy(g_gauge); g_gauge = NULL;
	xtc_counter_destroy(g_ctr); g_ctr = NULL;
	(void)xtc_exec_fini(e);
	return rc;
}

int
main(void)
{
	int rc;
	int d1 = 0, q1 = 0, d2 = 0, q2 = 0, d3 = 0, q3 = 0;
	uint64_t c1 = 0, hc1 = 0, s1 = 0, c2 = 0, hc2 = 0, s2 = 0;
	uint64_t c3 = 0, hc3 = 0, s3 = 0;
	int64_t g1 = 0, g2 = 0, g3 = 0;
	uint64_t want_ctr = (uint64_t)WORKERS * ITERS * (1 + ADD_STEP);
	uint64_t want_hcount = (uint64_t)WORKERS * ITERS;

	rc = run_stats(0xC0FF1, &d1, &c1, &g1, &hc1, &q1, &s1);
	if (rc != XTC_OK) {
		printf("FAIL: stats rc=%d (hang?)\n", rc);
		return 1;
	}
	(void)run_stats(0xC0FF1, &d2, &c2, &g2, &hc2, &q2, &s2);
	rc = run_stats(0xD1AA2, &d3, &c3, &g3, &hc3, &q3, &s3);
	if (rc != XTC_OK) {
		printf("FAIL: stats diff-seed rc=%d\n", rc);
		return 1;
	}
	printf("stats run1: done=%d ctr=%llu (want %llu) gauge=%lld "
	    "hcount=%llu (want %llu) q-ok=%d state=%016llx\n", d1,
	    (unsigned long long)c1, (unsigned long long)want_ctr,
	    (long long)g1, (unsigned long long)hc1,
	    (unsigned long long)want_hcount, q1,
	    (unsigned long long)s1);
	if (d1 != WORKERS) {
		printf("FAIL: not all stats workers finished (done=%d "
		    "want %d)\n", d1, WORKERS); return 1;
	}
	if (c1 != want_ctr) {
		printf("FAIL: counter read %llu != exact sum %llu "
		    "(lost/double count)\n", (unsigned long long)c1,
		    (unsigned long long)want_ctr); return 1;
	}
	if (g1 != 0) {
		printf("FAIL: gauge net %lld != 0 (unbalanced add/sub)\n",
		    (long long)g1); return 1;
	}
	if (hc1 != want_hcount) {
		printf("FAIL: hist count %llu != %llu (lost sample)\n",
		    (unsigned long long)hc1,
		    (unsigned long long)want_hcount); return 1;
	}
	if (q1 != 1) {
		printf("FAIL: hist median out of recorded range\n");
		return 1;
	}
	if (d1 != d2 || c1 != c2 || g1 != g2 || hc1 != hc2 || s1 != s2) {
		printf("FAIL: stats did not replay (ctr %llu/%llu state "
		    "%016llx/%016llx)\n", (unsigned long long)c1,
		    (unsigned long long)c2, (unsigned long long)s1,
		    (unsigned long long)s2); return 1;
	}
	/* Totals are schedule-independent: a different seed must produce
	 * the SAME counter/gauge/hist totals (only the sim state hash may
	 * differ). */
	if (d3 != WORKERS || c3 != want_ctr || g3 != 0 ||
	    hc3 != want_hcount || q3 != 1) {
		printf("FAIL: stats diff-seed changed the totals "
		    "(ctr=%llu gauge=%lld hcount=%llu q-ok=%d)\n",
		    (unsigned long long)c3, (long long)g3,
		    (unsigned long long)hc3, q3); return 1;
	}

	printf("OK: stats counter read == exact increment sum, gauge net "
	    "exact, hist count exact + quantile in range, replayed; totals "
	    "schedule-independent across a different seed\n");
	return 0;
}
