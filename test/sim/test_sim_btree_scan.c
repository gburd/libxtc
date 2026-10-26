/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * test/sim/test_sim_btree_scan.c
 *	DST of the sqlxtc B-tree's SCAN OPENS racing merges and page-id
 *	reuse (examples/06_sqlxtc/btree.c).  A forward scan opens a cursor
 *	(cursor_descend), steps leaf to leaf over right links, parks and
 *	resumes; concurrent deletes merge leaves, free their page ids, and
 *	later splits reissue those ids as nodes of other key ranges.  Every
 *	link a reader holds without a latch can then name a different page.
 *	In 1.59 two such holes were found by an MT stress test (a cursor hop
 *	onto a reissued page, a keyless open on a mid-tree leaf), and once a
 *	run hung with every reader inside cursor_descend.  This test drives
 *	that mix under the simulator, so a failing interleaving is a seed.
 *
 *	Churners each own a key stride: insert it, delete it all back out
 *	(merges, frees), with interleaved inserts of a second stride after
 *	the deletes (splits that reissue the freed ids).  Scanners open
 *	cursors at random keys (and with no key), step a bounded number of
 *	rows, park and resume on every few rows.  A fixed anchor set is
 *	never touched.
 *
 *	Asserts, per seed:
 *	  (a) xtc_sim_exec_run quiesces: no hang, no livelock (the descent
 *	      bounds turn a livelock into a failed operation: (e));
 *	  (b) every scan is strictly ascending, and every anchor inside a
 *	      completed scan's range is returned (no skipped live row);
 *	  (c) after the storm every anchor reads back, no churn key
 *	      survives, and a full scan is exactly the anchors, ascending;
 *	  (d) the same seed replays to the same result and state hash;
 *	  (e) no descent gave up (bt st_stuck == 0) and no cursor call
 *	      returned an error.
 *	Usage: test_sim_btree_scan [base [n_seeds]] (default base 0, 64
 *	seeds; seed s is 0x9E3779B97F4A7C15 * (base + s + 1); every 8th seed
 *	runs adversarial: pessimal picks + buggify).
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

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_async.h"
#include "xtc_sim.h"
/* The storage engine is included, so the test builds against the sim
 * library alone (scripts/dst-corpus.sh compiles corpus tests that way). */
#include "../../examples/06_sqlxtc/bufmgr.c"
#include "../../examples/06_sqlxtc/btnode.c"
#include "../../examples/06_sqlxtc/btree.c"

#define PAGE_SZ     512      /* tiny pages: many leaves, frequent merges */
#define N_FRAMES    20       /* small pool: evictions park readers in the hop */
#define N_LOOPS     3
#define N_CHURNERS  4
#define CHURN_PER   48
#define N_ANCHORS   80
#define N_SCANNERS  3
#define SCANS_PER   40
#define SCAN_ROWS   40

static bm_t        *g_bm;
static bt_t        *g_bt;
static _Atomic int  g_left;
static _Atomic long g_bad_order, g_skipped, g_errors, g_scans, g_hops;

static void
anchor_k(int i, char *k)
{
	(void)snprintf(k, 16, "m%06d", i * 10);
}

/* Churn keys interleave with the anchors ("m%06d" with a last digit
 * 1..8), so merges and splits happen in the leaves the scans walk. */
static void
churn_k(int w, int i, int gen, char *k)
{
	(void)snprintf(k, 16, "m%06d", (i % N_ANCHORS) * 10 + 1 +
	    (w * 2 + gen) % 8);
}

static void
churner(void *arg)
{
	int w = (int)(intptr_t)arg, i, gen;
	char k[16];

	for (gen = 0; gen < 2; gen++) {
		for (i = 0; i < CHURN_PER; i++) {
			churn_k(w, i, gen, k);
			(void)bt_insert(g_bt, k, (uint16_t)strlen(k), "cccccccccccccccc",
			    16);
			if ((i & 3) == 0)
				xtc_yield();
		}
		for (i = 0; i < CHURN_PER; i++) {
			churn_k(w, i, gen, k);
			(void)bt_delete(g_bt, k, (uint16_t)strlen(k));
			if ((i & 3) == 0)
				xtc_yield();
		}
	}
	(void)atomic_fetch_sub(&g_left, 1);
}

/* One scan from anchor index `a` (or from the first key if a < 0): at
 * most SCAN_ROWS rows, ascending, parking every 3 rows.  Every anchor
 * between the start and the last key seen must have been returned. */
static void
one_scan(int a)
{
	bt_cursor_t *c = NULL;
	const void *k, *v;
	uint16_t kl, vl;
	char start[16], prev[16];
	uint16_t sl = 0, pl = 0;
	int rows = 0, next_anchor, rc;

	if (a >= 0) {
		anchor_k(a, start);
		sl = (uint16_t)strlen(start);
	}
	next_anchor = a >= 0 ? a : 0;
	if (bt_cursor_open(g_bt, a >= 0 ? start : NULL, sl, &c) != XTC_OK) {
		atomic_fetch_add(&g_errors, 1);
		return;
	}
	while (rows < SCAN_ROWS) {
		rc = bt_cursor_next(c, &k, &kl, &v, &vl);
		if (rc == XTC_E_NOTFOUND)
			break;
		if (rc != XTC_OK || kl == 0 || kl >= sizeof prev) {
			atomic_fetch_add(&g_errors, 1);
			break;
		}
		if (pl != 0) {
			uint16_t ml = kl < pl ? kl : pl;
			int cmpv = memcmp(prev, k, ml);
			if (cmpv > 0 || (cmpv == 0 && pl >= kl))
				atomic_fetch_add(&g_bad_order, 1);
		}
		memcpy(prev, k, kl);
		pl = kl;
		/* An anchor key ends in 0: it must be the next one expected. */
		if (kl == 7 && ((const char *)k)[6] == '0') {
			int idx = atoi((const char *)k + 1) / 10;
			if (idx != next_anchor)
				atomic_fetch_add(&g_skipped, 1);
			next_anchor = idx + 1;
		}
		if (++rows % 3 == 0) {
			if (bt_cursor_park(c) != XTC_OK ||
			    bt_cursor_resume(c) != XTC_OK) {
				atomic_fetch_add(&g_errors, 1);
				break;
			}
			xtc_yield();
		}
	}
	bt_cursor_close(c);
	atomic_fetch_add(&g_scans, 1);
}

static void
scanner(void *arg)
{
	unsigned s = (unsigned)(uintptr_t)arg;
	int i;

	for (i = 0; i < SCANS_PER && atomic_load(&g_left) > 0; i++) {
		s = s * 1103515245u + 12345u;
		one_scan((s >> 8) % 8 == 0 ? -1 : (int)((s >> 8) % N_ANCHORS));
		xtc_yield();
	}
}

struct run_out {
	int      missing, churn_surv, scan_count, scan_bad;
	long     bad_order, skipped, errors, scans;
	uint64_t stuck, reissued, merges;
	uint64_t result, state;
	int      bug_active;
};

static uint64_t
mix(uint64_t h, uint64_t x)
{
	h ^= x;
	return h * 0x100000001B3ull;
}

static int
run_once(uint64_t seed, int adversarial, struct run_out *out)
{
	xtc_exec_t *exec = NULL;
	xtc_proc_opts_t opts = XTC_PROC_OPTS_INIT;
	bm_opts_t bo = BM_OPTS_DEFAULT;
	bt_stats_t bs;
	bm_stats_t ms;
	char path[] = "/tmp/sim_btscan_XXXXXX", k[16], prev[16];
	bt_cursor_t *c = NULL;
	const void *ck, *cv;
	uint16_t ckl, cvl, pl = 0;
	int fd, i, rc;
	intptr_t w;

	memset(out, 0, sizeof *out);
	atomic_store(&g_left, N_CHURNERS);
	atomic_store(&g_bad_order, 0);
	atomic_store(&g_skipped, 0);
	atomic_store(&g_errors, 0);
	atomic_store(&g_scans, 0);
	if ((fd = mkstemp(path)) < 0)
		return -1;
	close(fd);
	bo.path = path;
	bo.page_size = PAGE_SZ;
	bo.n_frames = N_FRAMES;
	bo.cool_pct = 25;
	if (bm_create(&bo, &g_bm) != XTC_OK || bt_open(g_bm, &g_bt) != XTC_OK) {
		unlink(path);
		return -1;
	}
	for (i = 0; i < N_ANCHORS; i++) {
		anchor_k(i, k);
		if (bt_insert(g_bt, k, (uint16_t)strlen(k), "aaaa", 4) != XTC_OK)
			return -1;
	}
	if (xtc_exec_init(&exec, N_LOOPS) != XTC_OK)
		return -1;
	xtc_sim_io_faults_enable(20 * 1000LL, 200 * 1000LL, 0);
	if (adversarial) {
		xtc_sim_sched_pessimal(600);
		xtc_sim_buggify_enable(300);
	}
	for (w = 0; w < N_CHURNERS; w++) {
		opts.name = "churn";
		(void)xtc_proc_spawn(xtc_exec_loop(exec, (int)(w % N_LOOPS)),
		    churner, (void *)w, &opts, NULL);
	}
	for (w = 0; w < N_SCANNERS; w++) {
		opts.name = "scan";
		(void)xtc_proc_spawn(xtc_exec_loop(exec, (int)((w + 1) % N_LOOPS)),
		    scanner, (void *)(uintptr_t)(0x9e3779b9u * (unsigned)(w + 1)
		    ^ (unsigned)seed), &opts, NULL);
	}
	rc = xtc_sim_exec_run(exec, seed, 40000000);
	out->state = xtc_sim_state_hash(exec);
	out->bug_active = xtc_sim_buggify_active_count();
	xtc_sim_io_faults_disable();
	xtc_sim_sched_pessimal(0);
	xtc_sim_buggify_disable();
	(void)xtc_exec_fini(exec);
	if (rc != XTC_OK) {
		bt_close(g_bt);
		bm_destroy(g_bm);
		unlink(path);
		return rc;
	}

	/* (c) after the storm, single-threaded. */
	for (i = 0; i < N_ANCHORS; i++) {
		char buf[8];
		uint16_t vl = 0;
		anchor_k(i, k);
		if (bt_lookup(g_bt, k, (uint16_t)strlen(k), buf, sizeof buf,
		    &vl) != XTC_OK || vl != 4)
			out->missing++;
	}
	if (bt_cursor_open(g_bt, NULL, 0, &c) == XTC_OK) {
		while (bt_cursor_next(c, &ck, &ckl, &cv, &cvl) == XTC_OK) {
			out->scan_count++;
			if (ckl != 7 || ((const char *)ck)[6] != '0')
				out->churn_surv++;
			if (pl != 0 && memcmp(prev, ck, ckl < pl ? ckl : pl) >= 0)
				out->scan_bad = 1;
			if (ckl < sizeof prev) {
				memcpy(prev, ck, ckl);
				pl = ckl;
			}
		}
		bt_cursor_close(c);
	} else
		out->scan_bad = 1;
	bt_get_stats(g_bt, &bs);
	bm_get_stats(g_bm, &ms);
	out->stuck = bs.stuck;
	out->merges = bs.merges;
	out->reissued = ms.reissued;
	out->bad_order = atomic_load(&g_bad_order);
	out->skipped = atomic_load(&g_skipped);
	out->errors = atomic_load(&g_errors);
	out->scans = atomic_load(&g_scans);
	out->result = 0xCBF29CE484222325ull;
	out->result = mix(out->result, (uint64_t)out->missing);
	out->result = mix(out->result, (uint64_t)out->churn_surv);
	out->result = mix(out->result, (uint64_t)out->scan_count);
	out->result = mix(out->result, (uint64_t)out->bad_order);
	out->result = mix(out->result, (uint64_t)out->skipped);
	out->result = mix(out->result, (uint64_t)out->errors);
	out->result = mix(out->result, (uint64_t)out->scans);
	out->result = mix(out->result, out->merges);
	out->result = mix(out->result, out->reissued);
	bt_close(g_bt);
	bm_destroy(g_bm);
	unlink(path);
	return XTC_OK;
}

static int
check(uint64_t seed, const char *mode, const struct run_out *o)
{
	if (o->missing == 0 && o->churn_surv == 0 && !o->scan_bad &&
	    o->scan_count == N_ANCHORS && o->bad_order == 0 &&
	    o->skipped == 0 && o->errors == 0 && o->stuck == 0)
		return 0;
	printf("FAIL: seed 0x%016llx (%s): missing=%d churn_surv=%d scan=%d/%d "
	    "scan_bad=%d bad_order=%ld skipped=%ld errors=%ld stuck=%llu\n",
	    (unsigned long long)seed, mode, o->missing, o->churn_surv,
	    o->scan_count, N_ANCHORS, o->scan_bad, o->bad_order, o->skipped,
	    o->errors, (unsigned long long)o->stuck);
	return 1;
}

int
main(int argc, char **argv)
{
	long base = argc > 1 ? strtol(argv[1], NULL, 0) : 0;
	long n = argc > 2 ? strtol(argv[2], NULL, 10) : 64;
	long s, scans = 0;
	uint64_t merges = 0, reissued = 0;
	int fails = 0, adv = 0, bugs = 0, rc;

	for (s = 0; s < n; s++) {
		uint64_t seed = 0x9E3779B97F4A7C15ull * (uint64_t)(base + s + 1);
		int a = (s % 8) == 7;
		struct run_out o1, o2;
		const char *mode = a ? "adversarial" : "benign";

		rc = run_once(seed, a, &o1);
		if (rc != XTC_OK) {
			printf("FAIL: seed 0x%016llx (%s) did not quiesce (rc=%d): "
			    "a hang or livelock\n", (unsigned long long)seed, mode,
			    rc);
			fails++;
			continue;
		}
		fails += check(seed, mode, &o1);
		scans += o1.scans;
		merges += o1.merges;
		reissued += o1.reissued;
		adv += a;
		bugs += o1.bug_active;
		if (s % 16 == 0) {      /* (d) replay, on a sample of seeds */
			rc = run_once(seed, a, &o2);
			if (rc != XTC_OK || o1.result != o2.result ||
			    o1.state != o2.state) {
				printf("FAIL: seed 0x%016llx (%s) did not replay "
				    "(rc=%d result %016llx/%016llx state %016llx/"
				    "%016llx)\n", (unsigned long long)seed, mode, rc,
				    (unsigned long long)o1.result,
				    (unsigned long long)o2.result,
				    (unsigned long long)o1.state,
				    (unsigned long long)o2.state);
				fails++;
			}
		}
		if (fails > 0)
			printf("  pin: test_sim_btree_scan %ld 1\n", base + s);
		if (fails > 0)
			break;
	}
	if (fails == 0 && (merges == 0 || reissued == 0)) {
		printf("FAIL: the storm never merged (%llu) or reissued a page id "
		    "(%llu): the race under test was not reached\n",
		    (unsigned long long)merges, (unsigned long long)reissued);
		fails++;
	}
	if (fails == 0 && adv >= 4 && bugs == 0) {   /* 30% per site per seed */
		printf("FAIL: adversarial seeds activated no buggify site (%ld "
		    "scans, %llu merges, %llu reissued)\n", scans,
		    (unsigned long long)merges, (unsigned long long)reissued);
		fails++;
	}
	if (fails != 0)
		return 1;
	printf("OK: sqlxtc B-tree scan-vs-merge DST -- %ld seeds (%d "
	    "adversarial), %ld scans, %llu merges, %llu page ids reissued: every "
	    "scan ascending with no skipped anchor, no descent gave up, every "
	    "sampled seed replays\n", n, adv, scans, (unsigned long long)merges,
	    (unsigned long long)reissued);
	return 0;
}
