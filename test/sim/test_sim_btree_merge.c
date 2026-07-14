/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * test/sim/test_sim_btree_merge.c
 *	DST coverage of the sqlxtc B-tree's concurrent MERGE structure
 *	modification (examples/06_sqlxtc/btree.c bt_merge/merge_level),
 *	the race .agent/M_SQLXTC_BTREE_MERGE.md diagnoses: latch-free
 *	descents (bt_insert_fast, bt_delete, descend_shared, cursor)
 *	racing a concurrent right-merge that unlinks and frees a page.
 *
 *	Several churner fibers each own a disjoint key stride: insert the
 *	stride, then delete it all back out (the delete-heavy pattern
 *	that drives merges), interleaved on a small pool / small pages so
 *	splits and merges fire often.  A fixed anchor set is never
 *	touched.  A concurrent reader cohort repeatedly looks up anchors
 *	while the storm runs.  Under xtc_sim_exec_run every fiber park
 *	(page I/O, yield) is part of the replayable schedule, and the
 *	pessimal scheduler + buggify (btree.split.eager) push the
 *	adversarial interleavings the bounded MT test cannot guarantee to
 *	hit.
 *
 *	Asserts (the exact gates test_btree_delete_merge.c's
 *	test_concurrent_merge uses, replayed under DST):
 *	  (a) xtc_sim_exec_run reaches quiescence (no hang/livelock).
 *	  (b) every anchor survives with its correct value (merge must
 *	      never drop or alias live data).
 *	  (c) every churn key is ABSENT after the storm (the churn-gone
 *	      gate -- a surviving churn key is a lost delete, the exact
 *	      symptom the merge race produced before the fix).
 *	  (d) a full forward scan is strictly ascending and is exactly
 *	      the anchor set (no duplicate/missing/out-of-order key --
 *	      catches a torn internal-node fence).
 *	  (e) no reader ever observed a wrong value for an anchor.
 *	  (f) REPLAY: the same seed reproduces the identical result hash
 *	      and engine state hash.
 *	  (g) a different seed (usually) reorders the schedule and still
 *	      holds every invariant.
 *	  (h) the same suite run again with the adversarial scheduler
 *	      (pessimal picks + buggify eager-split) still holds every
 *	      invariant -- the pessimal-path sweep the design doc's
 *	      "schedule-fuzzed DST harness" calls for.
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
#include "btree.h"
#include "bufmgr.h"

#define PAGE_SZ     512      /* tiny pages: many leaves, frequent merges */
#define N_FRAMES    32       /* small resident pool: forces eviction too */
#define N_LOOPS     3
#define N_CHURNERS  4
#define CHURN_PER   40
#define N_ROUNDS    2
#define N_ANCHORS   60
#define N_READERS   2
#define READER_ITERS 300

static bm_t       *g_bm;
static bt_t       *g_bt;
static _Atomic int g_left;         /* churners still running */
static _Atomic long g_read_mismatch;

static void
anchor_kv(int i, char *k, char *v)
{
	(void)snprintf(k, 16, "a%08d", i);
	(void)snprintf(v, 32, "A%08d-payload", i);
}

static void
churn_kv(int idx, char *k, char *v)
{
	(void)snprintf(k, 16, "c%08d", idx);
	(void)snprintf(v, 32, "C%08d-payload", idx);
}

static void
churner_proc(void *arg)
{
	long w = (long)arg;
	int r, i;
	char k[16], v[32];

	for (r = 0; r < N_ROUNDS; r++) {
		for (i = 0; i < CHURN_PER; i++) {
			int idx = i * N_CHURNERS + (int)w;
			churn_kv(idx, k, v);
			(void)bt_insert(g_bt, k, (uint16_t)strlen(k), v,
			    (uint16_t)strlen(v));
			if ((i & 3) == 0)
				xtc_yield();
		}
		for (i = 0; i < CHURN_PER; i++) {
			int idx = i * N_CHURNERS + (int)w;
			churn_kv(idx, k, v);
			(void)bt_delete(g_bt, k, (uint16_t)strlen(k));
			if ((i & 3) == 0)
				xtc_yield();
		}
	}
	if (atomic_fetch_sub(&g_left, 1) == 1)
		bm_provider_stop(g_bm);
}

/* Reader cohort: probes anchors while the storm runs.  A hit must
 * carry the correct value -- the descent-level dead/fence
 * revalidation must never hand a concurrent reader a stale/aliased
 * page during a merge. */
static void
reader_proc(void *arg)
{
	unsigned seed = (unsigned)(uintptr_t)arg;
	int i;

	for (i = 0; i < READER_ITERS; i++) {
		char k[16], v[32], buf[64];
		uint16_t vl = 0;
		int idx = (int)((unsigned)rand_r(&seed) % N_ANCHORS);

		anchor_kv(idx, k, v);
		if (bt_lookup(g_bt, k, (uint16_t)strlen(k), buf, sizeof buf,
		    &vl) == XTC_OK) {
			if (vl != strlen(v) || memcmp(buf, v, vl) != 0)
				atomic_fetch_add(&g_read_mismatch, 1);
		}
		if ((i & 7) == 0)
			xtc_yield();
	}
}

/* Fold the post-storm observables into one replay hash. */
static uint64_t
result_hash(int missing, int churn_surv, int scan_count, long mismatch)
{
	uint64_t h = 0xCBF29CE484222325ull;
#define MIX(x) do { h ^= (uint64_t)(x); h *= 0x100000001B3ull; } while (0)
	MIX(missing);
	MIX(churn_surv);
	MIX(scan_count);
	MIX(mismatch);
#undef MIX
	return h;
}

struct run_out {
	int      missing;      /* anchors lost/wrong */
	int      churn_surv;   /* churn keys still present (lost delete) */
	int      scan_count;   /* total keys a forward scan yields */
	int      scan_bad;     /* 1 if scan was not strictly ascending */
	long     mismatch;     /* reader saw a wrong anchor value */
	int      bug_active;   /* buggify activations this run (adversarial) */
	uint64_t result;
	uint64_t state;
};

static int
run_once(uint64_t seed, int adversarial, struct run_out *out)
{
	xtc_exec_t *exec = NULL;
	xtc_proc_opts_t opts = { 0 };
	bm_opts_t bo = BM_OPTS_DEFAULT;
	xtc_pid_t pp;
	char path[] = "/tmp/sim_btmerge_XXXXXX";
	int fd, i, rc;
	long w;
	bt_cursor_t *c = NULL;
	const void *ck, *cv;
	uint16_t ckl, cvl;

	memset(out, 0, sizeof *out);
	atomic_store(&g_left, N_CHURNERS);
	atomic_store(&g_read_mismatch, 0);

	fd = mkstemp(path);
	if (fd < 0)
		return -1;
	close(fd);

	bo.path = path;
	bo.page_size = PAGE_SZ;
	bo.n_frames = N_FRAMES;
	bo.cool_pct = 25;
	if (bm_create(&bo, &g_bm) != XTC_OK) {
		unlink(path);
		return -1;
	}
	if (bt_open(g_bm, &g_bt) != XTC_OK) {
		bm_destroy(g_bm);
		unlink(path);
		return -1;
	}

	/* Lay down the anchor set the churners never touch. */
	for (i = 0; i < N_ANCHORS; i++) {
		char k[16], v[32];
		anchor_kv(i, k, v);
		if (bt_insert(g_bt, k, (uint16_t)strlen(k), v,
		    (uint16_t)strlen(v)) != XTC_OK) {
			bt_close(g_bt);
			bm_destroy(g_bm);
			unlink(path);
			return -1;
		}
	}

	if (xtc_exec_init(&exec, N_LOOPS) != XTC_OK) {
		bt_close(g_bt);
		bm_destroy(g_bm);
		unlink(path);
		return -1;
	}

	/* Seeded page-I/O latency (no injected errors): reorders eviction
	 * completions across runs so I/O ordering is part of the schedule. */
	xtc_sim_io_faults_enable(20 * 1000LL, 200 * 1000LL, 0);

	if (adversarial) {
		xtc_sim_sched_pessimal(600);   /* 60% pessimal (starve) picks */
		xtc_sim_buggify_enable(300);   /* 30% per buggify site,
		                                 * incl. btree.split.eager */
	}

	if (bm_provider_spawn(g_bm, xtc_exec_loop(exec, 0), 200 * 1000LL,
	    &pp) != XTC_OK) {
		xtc_sim_io_faults_disable();
		(void)xtc_exec_fini(exec);
		bt_close(g_bt);
		bm_destroy(g_bm);
		unlink(path);
		return -1;
	}

	for (w = 0; w < N_CHURNERS; w++) {
		xtc_loop_t *lp = xtc_exec_loop(exec, (int)(w % N_LOOPS));
		opts.name = "churn";
		if (xtc_proc_spawn(lp, churner_proc, (void *)(long)w, &opts,
		    NULL) != XTC_OK) {
			bm_provider_stop(g_bm);
			(void)xtc_sim_exec_run(exec, seed, 20000000);
			xtc_sim_io_faults_disable();
			(void)xtc_exec_fini(exec);
			bt_close(g_bt);
			bm_destroy(g_bm);
			unlink(path);
			return -1;
		}
	}
	for (i = 0; i < N_READERS; i++) {
		xtc_loop_t *lp = xtc_exec_loop(exec, (i + 1) % N_LOOPS);
		opts.name = "rd";
		(void)xtc_proc_spawn(lp, reader_proc, (void *)(uintptr_t)
		    (0x9e3779b9u ^ (unsigned)(i * 2654435761u + 1)), &opts,
		    NULL);
	}

	rc = xtc_sim_exec_run(exec, seed, 20000000);
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

	/* (b) every anchor present + correct (single-threaded now). */
	for (i = 0; i < N_ANCHORS; i++) {
		char k[16], v[32], buf[64];
		uint16_t vl = 0;
		anchor_kv(i, k, v);
		if (bt_lookup(g_bt, k, (uint16_t)strlen(k), buf, sizeof buf,
		    &vl) != XTC_OK) {
			out->missing++;
			continue;
		}
		if (vl != strlen(v) || memcmp(buf, v, vl) != 0)
			out->missing++;
	}

	/* (c)+(d) full ordered scan: exactly the anchors, strictly
	 * ascending, and count every surviving churn key. */
	if (bt_cursor_open(g_bt, NULL, 0, &c) == XTC_OK) {
		char prevbuf[16];
		uint16_t prevlen = 0;
		while (bt_cursor_next(c, &ck, &ckl, &cv, &cvl) == XTC_OK) {
			out->scan_count++;
			if (((const char *)ck)[0] == 'c')
				out->churn_surv++;
			if (prevlen != 0) {
				uint16_t ml = ckl < prevlen ? ckl : prevlen;
				int cmpv = memcmp(prevbuf, ck, ml);
				if (cmpv > 0 || (cmpv == 0 && prevlen >= ckl))
					out->scan_bad = 1;
			}
			if (ckl <= sizeof prevbuf) {
				memcpy(prevbuf, ck, ckl);
				prevlen = ckl;
			} else
				out->scan_bad = 1;
		}
		bt_cursor_close(c);
	} else
		out->scan_bad = 1;

	out->mismatch = atomic_load(&g_read_mismatch);
	out->result = result_hash(out->missing, out->churn_surv,
	    out->scan_count, out->mismatch);

	bt_close(g_bt);
	bm_destroy(g_bm);
	unlink(path);
	{
		char wal[80];
		(void)snprintf(wal, sizeof wal, "%s-wal", path);
		unlink(wal);
	}
	return XTC_OK;
}

static int
check_invariants(const char *label, const struct run_out *o)
{
	int fail = 0;

	if (o->missing != 0) {
		printf("FAIL: %s -- %d anchor(s) lost or wrong\n", label,
		    o->missing);
		fail = 1;
	}
	if (o->churn_surv != 0) {
		printf("FAIL: %s -- %d churn key(s) survived (lost delete "
		    "under concurrent merge)\n", label, o->churn_surv);
		fail = 1;
	}
	if (o->scan_bad) {
		printf("FAIL: %s -- forward scan not strictly ascending "
		    "(torn internal-node fence)\n", label);
		fail = 1;
	}
	if (o->scan_count != N_ANCHORS) {
		printf("FAIL: %s -- scan yielded %d keys, expected exactly "
		    "the %d anchors\n", label, o->scan_count, N_ANCHORS);
		fail = 1;
	}
	if (o->mismatch != 0) {
		printf("FAIL: %s -- %ld reader(s) saw a wrong anchor value\n",
		    label, o->mismatch);
		fail = 1;
	}
	return fail;
}

int
main(void)
{
	struct run_out o1, o2, o3;
	int rc, fails = 0;

	/* --- benign scheduler: same seed twice (quiescence + replay) --- */
	rc = run_once(0x8B7EE, 0, &o1);
	if (rc != XTC_OK) {
		printf("FAIL: btree-merge run did not quiesce (rc=%d) -- a "
		    "hang or lost wakeup under concurrent merge?\n", rc);
		return 1;
	}
	fails += check_invariants("benign run1", &o1);

	rc = run_once(0x8B7EE, 0, &o2);
	if (rc != XTC_OK) {
		printf("FAIL: btree-merge replay run did not quiesce "
		    "(rc=%d)\n", rc);
		return 1;
	}
	fails += check_invariants("benign run2 (replay)", &o2);

	printf("run1: missing=%d churn_surv=%d scan=%d mismatch=%ld "
	    "result=%016llx state=%016llx\n", o1.missing, o1.churn_surv,
	    o1.scan_count, o1.mismatch, (unsigned long long)o1.result,
	    (unsigned long long)o1.state);
	printf("run2: missing=%d churn_surv=%d scan=%d mismatch=%ld "
	    "result=%016llx state=%016llx\n", o2.missing, o2.churn_surv,
	    o2.scan_count, o2.mismatch, (unsigned long long)o2.result,
	    (unsigned long long)o2.state);

	if (o1.result != o2.result || o1.state != o2.state) {
		printf("FAIL: btree-merge run did not replay byte-identically "
		    "(result %016llx/%016llx state %016llx/%016llx)\n",
		    (unsigned long long)o1.result, (unsigned long long)o2.result,
		    (unsigned long long)o1.state, (unsigned long long)o2.state);
		fails++;
	}

	/* --- a different seed: usually reorders, must still be safe --- */
	rc = run_once(0x1DEA7, 0, &o3);
	if (rc != XTC_OK) {
		printf("FAIL: alt-seed btree-merge run did not quiesce "
		    "(rc=%d)\n", rc);
		return 1;
	}
	fails += check_invariants("alt-seed benign", &o3);
	printf("altseed: missing=%d churn_surv=%d scan=%d mismatch=%ld "
	    "result=%016llx state=%016llx\n", o3.missing, o3.churn_surv,
	    o3.scan_count, o3.mismatch, (unsigned long long)o3.result,
	    (unsigned long long)o3.state);

	/* --- adversarial: pessimal scheduler + buggify eager-split, a
	 * seed sweep, each replayed once against itself --- */
	{
		uint64_t base = 0xA0DE5A17E5ull;
		int n = 12, i;
		int adv_bug_seen = 0;

		for (i = 0; i < n; i++) {
			uint64_t seed = base + (uint64_t)i *
			    0x9E3779B97F4A7C15ull;
			struct run_out a1, a2;
			char label[64];

			rc = run_once(seed, 1, &a1);
			adv_bug_seen += a1.bug_active;
			if (rc != XTC_OK) {
				printf("FAIL: adversarial seed 0x%016llx did "
				    "not quiesce (rc=%d)\n",
				    (unsigned long long)seed, rc);
				fails++;
				continue;
			}
			(void)snprintf(label, sizeof label,
			    "adversarial seed 0x%016llx",
			    (unsigned long long)seed);
			fails += check_invariants(label, &a1);

			rc = run_once(seed, 1, &a2);
			if (rc != XTC_OK || a1.result != a2.result ||
			    a1.state != a2.state) {
				printf("FAIL: adversarial seed 0x%016llx did "
				    "not replay (rc=%d, result %016llx/"
				    "%016llx, state %016llx/%016llx)\n",
				    (unsigned long long)seed, rc,
				    (unsigned long long)a1.result,
				    (unsigned long long)a2.result,
				    (unsigned long long)a1.state,
				    (unsigned long long)a2.state);
				fails++;
			}
		}

		if (fails == 0 && adv_bug_seen == 0) {
			printf("FAIL: adversarial mode planted ZERO buggify "
			    "sites -- the pessimal merge-vs-descent paths "
			    "were never reached\n");
			fails++;
		}

		if (fails == 0) {
			printf("OK: sqlxtc B-tree concurrent-merge DST -- "
			    "%d churners x %d loops x %d rounds + %d anchors, "
			    "benign seed sweep + %d adversarial seeds "
			    "(pessimal scheduler + buggify, %d activations) "
			    "-- no lost delete, no anchor loss/aliasing, no "
			    "reader saw a wrong value, forward scan always "
			    "well-ordered, every run replays byte-identically "
			    "from its seed\n",
			    N_CHURNERS, N_LOOPS, N_ROUNDS, N_ANCHORS, n,
			    adv_bug_seen);
			return 0;
		}
	}

	printf("FAIL: %d invariant/replay violation(s) in the btree "
	    "concurrent-merge DST suite\n", fails);
	return 1;
}
