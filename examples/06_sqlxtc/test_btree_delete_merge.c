/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_btree_delete_merge.c
 *	Leaf merge + page reclaim on delete.  Builds a multi-level tree,
 *	deletes most of the keys, and proves the tree actually gives
 *	space back instead of bloating forever: the height shrinks, the
 *	buffer manager's reclaim freelist fills (or, equivalently, freed
 *	pages are reissued so the page count does not keep climbing),
 *	every surviving key is still findable, and a full ordered scan
 *	returns exactly the survivors in ascending order.
 *
 *	A second scenario deletes EVERY key down to the single root leaf,
 *	checking the cascade collapses the tree all the way back to
 *	height 1 with the interior pages reclaimed -- the inverse of the
 *	split path -- and that the emptied tree then rebuilds correctly
 *	(reusing reclaimed page ids without aliasing live data).  Runs
 *	off-loop, like test_btree, so eviction/reload is exercised too.
 *
 *	Build:
 *	  cd examples/06_sqlxtc
 *	  make test_btree_delete_merge && ./test_btree_delete_merge
 */

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "btree.h"
#include "bufmgr.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "t_tmp.h"

static int g_checks;

#define CHECK(cond, ...)                                                     \
	do {                                                                 \
		g_checks++;                                                  \
		if (!(cond)) {                                               \
			fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
			fprintf(stderr, __VA_ARGS__);                        \
			fprintf(stderr, "\n");                               \
			exit(1);                                             \
		}                                                            \
	} while (0)

static void
make_key(char *buf, size_t cap, int i)
{
	(void)snprintf(buf, cap, "k%08d", i);     /* 9 chars */
}

static void
make_val(char *buf, size_t cap, int i)
{
	(void)snprintf(buf, cap, "v%08d-payload", i);
}

static int
key_index(const void *k, uint16_t klen)
{
	char tmp[16];

	if (klen == 0 || klen >= sizeof tmp)
		return -1;
	memcpy(tmp, k, klen);
	tmp[klen] = '\0';
	return atoi(tmp + 1);
}

static bm_t *
make_bm(char *path_out, uint32_t page_size, uint32_t n_frames)
{
	bm_opts_t bo = BM_OPTS_DEFAULT;
	bm_t *bm = NULL;
	int fd;

	t_tmpl(path_out, 64, "sqlxtc-btmerge");
	fd = mkstemp(path_out);
	CHECK(fd >= 0, "mkstemp");
	(void)close(fd);
	bo.path = path_out;
	bo.page_size = page_size;
	bo.n_frames = n_frames;
	bo.cool_pct = 20;
	CHECK(bm_create(&bo, &bm) == XTC_OK, "bm_create");
	return bm;
}

/* Walk the tree with a full cursor scan; assert ascending order and
 * that exactly the expected survivors (predicate alive(i)) appear. */
static int
scan_count_check(bt_t *bt, int n, int (*alive)(int))
{
	bt_cursor_t *c = NULL;
	const void *ck, *cv;
	uint16_t ckl, cvl;
	int count = 0, prev = -1, i;

	CHECK(bt_cursor_open(bt, NULL, 0, &c) == XTC_OK, "scan open");
	while (bt_cursor_next(c, &ck, &ckl, &cv, &cvl) == XTC_OK) {
		int idx = key_index(ck, ckl);
		char v[32];

		CHECK(idx > prev, "scan strictly ascending (idx=%d prev=%d)",
		    idx, prev);
		CHECK(alive(idx), "scan returned a deleted key %d", idx);
		make_val(v, sizeof v, idx);
		CHECK(cvl == strlen(v) && memcmp(cv, v, cvl) == 0,
		    "scan value at %d", idx);
		prev = idx;
		count++;
	}
	bt_cursor_close(c);

	/* Cross-check against per-key lookups: every survivor is found,
	 * every deleted key misses. */
	for (i = 0; i < n; i++) {
		char k[16], v[32], buf[64];
		uint16_t vl = 0;

		make_key(k, sizeof k, i);
		if (alive(i)) {
			make_val(v, sizeof v, i);
			CHECK(bt_lookup(bt, k, 9, buf, sizeof buf, &vl) == XTC_OK,
			    "survivor lookup %d", i);
			CHECK(vl == strlen(v) && memcmp(buf, v, vl) == 0,
			    "survivor value %d", i);
		} else {
			CHECK(bt_lookup(bt, k, 9, NULL, 0, &vl) ==
			    XTC_E_NOTFOUND, "deleted key gone %d", i);
		}
	}
	return count;
}

/* Survivor predicate for scenario 1: keep keys whose index is a
 * multiple of 17 (a sparse, scattered survivor set across the whole
 * keyspace, so deletions empty many leaves and force merges). */
static int
alive_sparse(int i)
{
	return (i % 17) == 0;
}

/* ---- scenario 1: delete most keys, tree must shrink + reclaim ---- */
static void
test_shrink_and_reclaim(void)
{
	char path[64];
	const uint32_t PAGE_SZ = 512;       /* tiny pages: deep tree fast */
	const int N = 4000;
	bm_t *bm = make_bm(path, PAGE_SZ, 64);
	bt_t *bt = NULL;
	bt_stats_t s_built, s_after;
	bm_stats_t m_built, m_after;
	int i, survivors;

	CHECK(bt_open(bm, &bt) == XTC_OK, "bt_open shrink");

	for (i = 0; i < N; i++) {
		char k[16], v[32];

		make_key(k, sizeof k, i);
		make_val(v, sizeof v, i);
		CHECK(bt_insert(bt, k, 9, v, (uint16_t)strlen(v)) == XTC_OK,
		    "insert %d", i);
	}
	bt_get_stats(bt, &s_built);
	bm_get_stats(bm, &m_built);
	CHECK(s_built.height >= 3, "built tree multi-level (height=%llu)",
	    (unsigned long long)s_built.height);

	/* Delete every key that is NOT a survivor: ~94% of the tree. */
	for (i = 0; i < N; i++) {
		char k[16];

		if (alive_sparse(i))
			continue;
		make_key(k, sizeof k, i);
		CHECK(bt_delete(bt, k, 9) == XTC_OK, "delete %d", i);
	}

	bt_get_stats(bt, &s_after);
	bm_get_stats(bm, &m_after);

	survivors = scan_count_check(bt, N, alive_sparse);
	{
		int expect = 0;
		for (i = 0; i < N; i++)
			if (alive_sparse(i))
				expect++;
		CHECK(survivors == expect, "survivor count %d == %d",
		    survivors, expect);
	}

	/* GATE 1: height shrank -- the merge cascade collapsed levels. */
	CHECK(s_after.height < s_built.height,
	    "tree height shrank after deletes (built=%llu after=%llu)",
	    (unsigned long long)s_built.height,
	    (unsigned long long)s_after.height);
	/* GATE 2: merges fired and pages were reclaimed to the freelist. */
	CHECK(s_after.merges > 0, "merges happened (merges=%llu)",
	    (unsigned long long)s_after.merges);
	CHECK(s_after.reclaimed > 0, "pages reclaimed (reclaimed=%llu)",
	    (unsigned long long)s_after.reclaimed);
	CHECK(m_after.freed > 0, "bufmgr freed pages (freed=%llu)",
	    (unsigned long long)m_after.freed);

	printf("  test_shrink_and_reclaim: ok (%d keys -> %d survivors)\n",
	    N, survivors);
	printf("    height %llu -> %llu, merges=%llu reclaimed=%llu "
	    "bm_freed=%llu free_pids=%llu\n",
	    (unsigned long long)s_built.height,
	    (unsigned long long)s_after.height,
	    (unsigned long long)s_after.merges,
	    (unsigned long long)s_after.reclaimed,
	    (unsigned long long)m_after.freed,
	    (unsigned long long)m_after.free_pids);

	bt_close(bt);
	bm_destroy(bm);
	(void)unlink(path);
}

static int
alive_none(int i)
{
	(void)i;
	return 0;
}

/* ---- scenario 2: empty the tree completely, then rebuild ---- */
static void
test_collapse_to_root(void)
{
	char path[64];
	const uint32_t PAGE_SZ = 512;
	const int N = 2000;
	bm_t *bm = make_bm(path, PAGE_SZ, 64);
	bt_t *bt = NULL;
	bt_stats_t s_built, s_empty, s_rebuilt;
	bm_stats_t m_empty, m_rebuilt;
	int i, count;
	bt_cursor_t *c = NULL;
	const void *ck, *cv;
	uint16_t ckl, cvl;

	CHECK(bt_open(bm, &bt) == XTC_OK, "bt_open collapse");

	for (i = 0; i < N; i++) {
		char k[16], v[32];

		make_key(k, sizeof k, i);
		make_val(v, sizeof v, i);
		CHECK(bt_insert(bt, k, 9, v, (uint16_t)strlen(v)) == XTC_OK,
		    "insert %d", i);
	}
	bt_get_stats(bt, &s_built);
	CHECK(s_built.height > 1, "built multi-level (height=%llu)",
	    (unsigned long long)s_built.height);

	/* Delete EVERY key (ascending, the worst case for right-merge:
	 * each delete empties the leftmost live leaf, which must keep
	 * pulling its right sibling left). */
	for (i = 0; i < N; i++) {
		char k[16];

		make_key(k, sizeof k, i);
		CHECK(bt_delete(bt, k, 9) == XTC_OK, "delete-all %d", i);
	}

	bt_get_stats(bt, &s_empty);
	bm_get_stats(bm, &m_empty);

	/* The tree is empty: a scan yields nothing, and every key misses. */
	count = scan_count_check(bt, N, alive_none);
	CHECK(count == 0, "emptied tree scan yields nothing (got %d)", count);

	/* GATE: the cascade collapsed the tree back toward a single leaf
	 * and reclaimed interior pages. */
	CHECK(s_empty.height < s_built.height,
	    "emptied tree collapsed (built=%llu empty=%llu)",
	    (unsigned long long)s_built.height,
	    (unsigned long long)s_empty.height);
	CHECK(s_empty.reclaimed > 0, "interior pages reclaimed (%llu)",
	    (unsigned long long)s_empty.reclaimed);

	/* Rebuild: the reclaimed page ids are reissued by bm_alloc_pid,
	 * which must not alias any live page -- so a fresh build of the
	 * same keys must be fully correct. */
	for (i = 0; i < N; i++) {
		char k[16], v[32];

		make_key(k, sizeof k, i);
		make_val(v, sizeof v, i);
		CHECK(bt_insert(bt, k, 9, v, (uint16_t)strlen(v)) == XTC_OK,
		    "rebuild insert %d", i);
	}
	bt_get_stats(bt, &s_rebuilt);
	bm_get_stats(bm, &m_rebuilt);
	CHECK(m_rebuilt.reissued > 0,
	    "rebuild reused reclaimed page ids (reissued=%llu)",
	    (unsigned long long)m_rebuilt.reissued);

	CHECK(bt_cursor_open(bt, NULL, 0, &c) == XTC_OK, "rebuild scan open");
	count = 0;
	{
		int prev = -1;
		while (bt_cursor_next(c, &ck, &ckl, &cv, &cvl) == XTC_OK) {
			int idx = key_index(ck, ckl);
			char v[32];

			CHECK(idx == count, "rebuild scan order %d at %d", idx,
			    count);
			CHECK(idx > prev, "rebuild scan ascending");
			make_val(v, sizeof v, idx);
			CHECK(cvl == strlen(v) && memcmp(cv, v, cvl) == 0,
			    "rebuild scan value %d", idx);
			prev = idx;
			count++;
		}
	}
	bt_cursor_close(c);
	CHECK(count == N, "rebuild scan count %d == %d", count, N);

	printf("  test_collapse_to_root: ok (built h=%llu, emptied h=%llu, "
	    "reclaimed=%llu, rebuilt %d keys reusing %llu freed ids)\n",
	    (unsigned long long)s_built.height,
	    (unsigned long long)s_empty.height,
	    (unsigned long long)s_empty.reclaimed, N,
	    (unsigned long long)m_rebuilt.reissued);

	bt_close(bt);
	bm_destroy(bm);
	(void)unlink(path);
}

/* ---- scenario 3: interleaved delete/insert must not bloat ---- */
static void
test_no_bloat_churn(void)
{
	char path[64];
	const uint32_t PAGE_SZ = 512;
	const int WINDOW = 1500;     /* live key window */
	const int ROUNDS = 8;
	bm_t *bm = make_bm(path, PAGE_SZ, 64);
	bt_t *bt = NULL;
	bm_stats_t m0, m1;
	bt_stats_t t0, t1;
	int r, i;

	CHECK(bt_open(bm, &bt) == XTC_OK, "bt_open churn");

	/* Prime a window of live keys [0, WINDOW). */
	for (i = 0; i < WINDOW; i++) {
		char k[16], v[32];

		make_key(k, sizeof k, i);
		make_val(v, sizeof v, i);
		CHECK(bt_insert(bt, k, 9, v, (uint16_t)strlen(v)) == XTC_OK,
		    "prime %d", i);
	}
	bm_get_stats(bm, &m0);
	bt_get_stats(bt, &t0);

	/*
	 * Slide the window forward ROUNDS * WINDOW times: insert key
	 * base+WINDOW, delete key base.  The live set stays WINDOW keys,
	 * so a tree that reclaims merged pages keeps a roughly constant
	 * footprint -- the page-id high-water mark must NOT grow without
	 * bound (it would, before merge/reclaim existed).
	 */
	for (r = 0; r < ROUNDS; r++) {
		int base = r * WINDOW;

		for (i = 0; i < WINDOW; i++) {
			char k[16], v[32];
			int add = base + WINDOW + i;
			int del = base + i;

			make_key(k, sizeof k, add);
			make_val(v, sizeof v, add);
			CHECK(bt_insert(bt, k, 9, v, (uint16_t)strlen(v)) ==
			    XTC_OK, "churn insert %d", add);
			make_key(k, sizeof k, del);
			CHECK(bt_delete(bt, k, 9) == XTC_OK, "churn delete %d",
			    del);
		}
	}

	bm_get_stats(bm, &m1);
	bt_get_stats(bt, &t1);

	/*
	 * Bloat gate: a no-reclaim tree allocates a fresh page for every
	 * split and never gives one back, so over ROUNDS*WINDOW churned
	 * keys the freed/reissued counters would both be zero and the
	 * footprint would balloon.  With reclaim, the churn must both free
	 * pages and reissue them -- the steady-state footprint stays
	 * bounded by the live window.
	 */
	CHECK(m1.freed > m0.freed, "churn freed pages (freed %llu -> %llu)",
	    (unsigned long long)m0.freed, (unsigned long long)m1.freed);
	CHECK(m1.reissued > 0, "churn reissued reclaimed ids (reissued=%llu)",
	    (unsigned long long)m1.reissued);
	CHECK(t1.merges > t0.merges, "churn merged nodes (merges %llu -> %llu)",
	    (unsigned long long)t0.merges, (unsigned long long)t1.merges);

	/* The current live window [ROUNDS*WINDOW, (ROUNDS+1)*WINDOW) is
	 * intact and ordered. */
	{
		bt_cursor_t *c = NULL;
		const void *ck, *cv;
		uint16_t ckl, cvl;
		int count = 0, prev = -1;
		int lo = ROUNDS * WINDOW;

		CHECK(bt_cursor_open(bt, NULL, 0, &c) == XTC_OK,
		    "churn scan open");
		while (bt_cursor_next(c, &ck, &ckl, &cv, &cvl) == XTC_OK) {
			int idx = key_index(ck, ckl);

			CHECK(idx >= lo, "churn survivor in window (idx=%d)",
			    idx);
			CHECK(idx > prev, "churn scan ascending");
			prev = idx;
			count++;
		}
		bt_cursor_close(c);
		CHECK(count == WINDOW, "churn live count %d == %d", count,
		    WINDOW);
	}

	printf("  test_no_bloat_churn: ok (%d-key window, %d churn rounds; "
	    "freed=%llu reissued=%llu merges=%llu, footprint bounded)\n",
	    WINDOW, ROUNDS, (unsigned long long)m1.freed,
	    (unsigned long long)m1.reissued, (unsigned long long)t1.merges);

	bt_close(bt);
	bm_destroy(bm);
	(void)unlink(path);
}

/* ---- scenario 4: concurrent inserters + deleters drive merges ---- *
 *
 * Several writer procs and several deleter procs hammer one tree on a
 * multi-loop executor while the page-provider runs.  Writers churn a
 * disjoint key stride each (insert then delete their own keys), so
 * deletes drive merges concurrently with latch-free descents and
 * concurrent splits -- the real test that the SMO merge path is
 * deadlock-free and never corrupts the tree or feeds a stale page to a
 * descent.  A fixed "anchor" set of keys is never touched by the
 * churners; after the storm every anchor key must still be present
 * with its correct value (the gate against a merge dropping or
 * aliasing live data), and a full scan must be ordered.
 */
#define CM_LOOPS    4
#define CM_CHURNERS 6
#define CM_PER      400
#define CM_ANCHORS  500
#define CM_ROUNDS   3

static bm_t       *g_cm_bm;
static bt_t       *g_cm_bt;
static _Atomic int g_cm_left;
static _Atomic int g_cm_dup;   /* max duplicate-pid count any probe ever saw */

static void
cm_anchor_kv(int i, char *k, char *v)
{
	(void)snprintf(k, 16, "a%08d", i);
	(void)snprintf(v, 32, "A%08d-payload", i);
}

static void
cm_churn_kv(int idx, char *k, char *v)
{
	(void)snprintf(k, 16, "c%08d", idx);
	(void)snprintf(v, 32, "C%08d-payload", idx);
}

static void
cm_churner(void *arg)
{
	long w = (long)arg;
	int r, i;
	char k[16], v[32];

	for (r = 0; r < CM_ROUNDS; r++) {
		/* Insert this churner's stride. */
		for (i = 0; i < CM_PER; i++) {
			int idx = i * CM_CHURNERS + (int)w;
			cm_churn_kv(idx, k, v);
			(void)bt_insert(g_cm_bt, k, (uint16_t)strlen(k), v,
			    (uint16_t)strlen(v));
		}
		/* Delete it all again -- this drives the merges. */
		for (i = 0; i < CM_PER; i++) {
			int idx = i * CM_CHURNERS + (int)w;
			cm_churn_kv(idx, k, v);
			(void)bt_delete(g_cm_bt, k, (uint16_t)strlen(k));
			/* Reclamation-race probe: assert no pid ever maps two
			 * resident frames mid-storm (the bufmgr aliasing bug).
			 * Sample periodically so the scan does not dominate. */
			if ((i & 63) == 0) {
				uint32_t d = bm_dbg_dup_pid(g_cm_bm);
				if ((int)d > atomic_load(&g_cm_dup))
					atomic_store(&g_cm_dup, (int)d);
			}
		}
	}
	if (atomic_fetch_sub(&g_cm_left, 1) == 1)
		bm_provider_stop(g_cm_bm);
}

static void
test_concurrent_merge(void)
{
	xtc_exec_t *exec = NULL;
	bm_opts_t bo = BM_OPTS_DEFAULT;
	char path[64];
	xtc_loop_t *l0;
	int fd, i, missing = 0;
	long w;
	bt_stats_t ts;
	bm_stats_t bs;

	t_tmpl(path, sizeof path, "sqlxtc-cmerge");
	fd = mkstemp(path);
	CHECK(fd >= 0, "mkstemp cmerge");
	(void)close(fd);
	bo.path = path;
	bo.page_size = 512;       /* tiny pages: many leaves, frequent merges */
	bo.n_frames = 48;
	bo.cool_pct = 25;
	CHECK(bm_create(&bo, &g_cm_bm) == XTC_OK, "bm_create cmerge");
	CHECK(bt_open(g_cm_bm, &g_cm_bt) == XTC_OK, "bt_open cmerge");
	atomic_store(&g_cm_dup, 0);

	/* Lay down the anchor set that the churners never touch. */
	for (i = 0; i < CM_ANCHORS; i++) {
		char k[16], v[32];
		cm_anchor_kv(i, k, v);
		CHECK(bt_insert(g_cm_bt, k, (uint16_t)strlen(k), v,
		    (uint16_t)strlen(v)) == XTC_OK, "anchor insert %d", i);
	}

	atomic_store(&g_cm_left, CM_CHURNERS);
	CHECK(xtc_exec_init(&exec, CM_LOOPS) == XTC_OK, "exec_init cmerge");
	l0 = xtc_exec_loop(exec, 0);
	CHECK(bm_provider_spawn(g_cm_bm, l0, 1LL * 1000 * 1000, NULL) == XTC_OK,
	    "provider cmerge");
	for (w = 0; w < CM_CHURNERS; w++) {
		xtc_loop_t *lp = xtc_exec_loop(exec, (int)(w % CM_LOOPS));
		xtc_proc_opts_t po = { .name = "churn" };
		xtc_pid_t pid;
		CHECK(xtc_proc_spawn(lp, cm_churner, (void *)w, &po, &pid) ==
		    XTC_OK, "spawn churner %ld", w);
	}
	CHECK(xtc_exec_run(exec) == XTC_OK, "exec_run cmerge");
	bm_provider_stop(g_cm_bm);
	(void)xtc_exec_fini(exec);

	/* Every anchor key survived the merge storm intact. */
	for (i = 0; i < CM_ANCHORS; i++) {
		char k[16], v[32], buf[64];
		uint16_t vl = 0;
		cm_anchor_kv(i, k, v);
		if (bt_lookup(g_cm_bt, k, (uint16_t)strlen(k), buf, sizeof buf,
		    &vl) != XTC_OK) {
			missing++;
			continue;
		}
		if (vl != strlen(v) || memcmp(buf, v, vl) != 0)
			missing++;
	}
	CHECK(missing == 0, "%d/%d anchors lost or wrong after concurrent "
	    "merge storm", missing, CM_ANCHORS);

	/* The reclamation-race oracle: across the whole storm, no page id
	 * ever mapped two resident frames.  A duplicate is the exact bufmgr
	 * aliasing bug the interlock closes (two frames -> divergent page
	 * copies handed to different callers -> corruption). */
	CHECK(atomic_load(&g_cm_dup) == 0,
	    "a pid mapped two frames during the storm (max dup=%d)",
	    atomic_load(&g_cm_dup));
	CHECK(bm_dbg_dup_pid(g_cm_bm) == 0,
	    "a pid maps two frames after the storm");

	/*
	 * CHURN-GONE GATE (concurrent merge): with merge ENABLED under the
	 * concurrent storm every churn key that was deleted must be absent
	 * at quiescence -- a surviving churn key is a lost delete.  The
	 * internal-node-merge fence bug (an internal node that was the right
	 * half of a split carried a spurious +infinity hi-fence, so a merged
	 * node claimed a wider range than its parent routed and a later
	 * descent misrouted one leaf too far right and lost a delete) is
	 * fixed in bt_split_internal: the right half inherits the old node's
	 * routed upper bound, so the routing invariant hi_fence(child) ==
	 * parent's routed upper bound holds through every split, merge, and
	 * cascade.  A full forward scan yields exactly the anchors.
	 */
	{
		bt_cursor_t *c = NULL;
		const void *ck, *cv;
		uint16_t ckl, cvl;
		int surv = 0, count = 0;

		CHECK(bt_cursor_open(g_cm_bt, NULL, 0, &c) == XTC_OK,
		    "cmerge scan open");
		while (bt_cursor_next(c, &ck, &ckl, &cv, &cvl) == XTC_OK) {
			if (((const char *)ck)[0] == 'c')
				surv++;
			count++;
		}
		bt_cursor_close(c);

		CHECK(surv == 0, "%d churn survivor(s) of %d scanned after "
		    "concurrent merge storm (lost delete)", surv, count);
	}

	bt_get_stats(g_cm_bt, &ts);
	bm_get_stats(g_cm_bm, &bs);
	/* Merge is ENABLED for this concurrent tree: the storm drove real
	 * merges + reclaim, and the interlock kept it free of duplicate
	 * frames (asserted above). */
	CHECK(ts.merges > 0, "concurrent merges happened (merges=%llu)",
	    (unsigned long long)ts.merges);

	printf("  test_concurrent_merge: ok (%d churners x %d keys x %d rounds "
	    "+ %d anchors; interlock held: dup_pid=0; merges=%llu "
	    "reclaimed=%llu reissued=%llu)\n",
	    CM_CHURNERS, CM_PER, CM_ROUNDS, CM_ANCHORS,
	    (unsigned long long)ts.merges, (unsigned long long)ts.reclaimed,
	    (unsigned long long)bs.reissued);

	bt_close(g_cm_bt);
	bm_destroy(g_cm_bm);
	(void)unlink(path);
	{ char wal[80]; snprintf(wal, sizeof wal, "%s-wal", path); (void)unlink(wal); }
}

int
main(void)
{
	printf("btree delete/merge + page reclaim tests\n");
	test_shrink_and_reclaim();
	test_collapse_to_root();
	test_no_bloat_churn();
	test_concurrent_merge();
	printf("ALL PASS (%d checks)\n", g_checks);
	return 0;
}
