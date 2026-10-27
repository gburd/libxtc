/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_btree_sample.c
 *	bt_sample_keys, the get_distribution analogue (RethinkDB
 *	get_distribution.cc: read the top levels of the tree, return the
 *	separators as approximately equi-depth split points).
 *
 *	  1. a root-leaf tree samples its keys; [lo, hi) is honored.
 *	  2. 200k keys with a SKEWED distribution (90% of the keys in the top
 *	     1% of the key space): the sample is strictly ascending, the
 *	     page count is exactly the top-level node count (1 + root fanout
 *	     at depth 1) -- a sliver of the tree, never O(n) -- and the cut
 *	     points split the keys into chunks with max/min < 3, where a
 *	     uniform split of the key space leaves most chunks empty.
 *	  3. a range sample is exactly the full sample restricted to [lo,
 *	     hi), and reads fewer pages; thinning keeps order.
 *	  4. sampling while churners insert/delete (merges, splits) on a
 *	     multi-loop executor never crashes and stays ascending.
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
			g_fail = 1;                                          \
		}                                                            \
	} while (0)

#define NKEYS   200000
#define KCAP    16
#define MAXS    4096
#define NCHUNK  32

static bm_t *
mk(char *p, uint32_t nf)
{
	bm_opts_t bo = BM_OPTS_DEFAULT;
	bm_t *bm = NULL;
	int fd;

	t_tmpl(p, 64, "btsample");
	fd = mkstemp(p);
	if (fd < 0) { perror("mkstemp"); exit(2); }
	(void)close(fd);
	bo.path = p;
	bo.page_size = 4096;
	bo.n_frames = nf;
	bo.cool_pct = 20;
	if (bm_create(&bo, &bm) != XTC_OK) { fprintf(stderr, "bm_create\n"); exit(2); }
	return bm;
}

static void
be64(uint64_t v, uint8_t *k)
{
	int i;

	for (i = 7; i >= 0; i--) { k[i] = (uint8_t)v; v >>= 8; }
}

static uint64_t
get64(const uint8_t *k)
{
	uint64_t v = 0;
	int i;

	for (i = 0; i < 8; i++) v = (v << 8) | k[i];
	return v;
}

static int
kcmp(const uint8_t *a, uint16_t al, const uint8_t *b, uint16_t bl)
{
	uint16_t m = al < bl ? al : bl;
	int c = memcmp(a, b, m);

	return c != 0 ? c : (al > bl) - (al < bl);
}

static int
ascending(const uint8_t *keys, const uint16_t *lens, uint32_t n)
{
	uint32_t i;

	for (i = 1; i < n; i++)
		if (kcmp(keys + (i - 1) * KCAP, lens[i - 1], keys + i * KCAP,
		    lens[i]) >= 0)
			return 0;
	return 1;
}

static uint64_t
sample_pages(bt_t *bt)
{
	bt_stats_t st;

	bt_get_stats(bt, &st);
	return st.sample_pages;
}

static int
u64cmp(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;

	return (x > y) - (x < y);
}

/* Chunk sizes of sorted `v` (n keys) at cut points cuts[0..nc) (a key
 * k goes to chunk j where cuts[j-1] <= k < cuts[j]); returns max/min. */
static double
chunk_ratio(const uint64_t *v, int n, const uint64_t *cuts, int nc,
    long *minp, long *maxp)
{
	long cnt[MAXS + 1], mn, mx;
	int i, j = 0;

	memset(cnt, 0, sizeof(long) * (size_t)(nc + 1));
	for (i = 0; i < n; i++) {
		while (j < nc && v[i] >= cuts[j])
			j++;
		cnt[j]++;
	}
	mn = mx = cnt[0];
	for (i = 1; i <= nc; i++) {
		if (cnt[i] < mn) mn = cnt[i];
		if (cnt[i] > mx) mx = cnt[i];
	}
	*minp = mn;
	*maxp = mx;
	return mn > 0 ? (double)mx / (double)mn : 1e30;
}

/* 1. A root leaf: its keys are the sample, inside [lo, hi). */
static void
t_root_leaf(void)
{
	char p[64];
	bm_t *bm = mk(p, 64);
	bt_t *bt;
	uint8_t k[8], keys[MAXS * KCAP], lo[8], hi[8];
	uint16_t lens[MAXS];
	uint32_t n = 0, i;

	CHECK(bt_open(bm, &bt) == XTC_OK, "open");
	CHECK(bt_sample_keys(bt, NULL, 0, NULL, 0, 1, keys, KCAP, lens, MAXS,
	    &n) == XTC_OK && n == 0, "empty tree: no keys (n=%u)", n);
	for (i = 1; i <= 20; i++) {
		be64(i * 10, k);
		CHECK(bt_insert(bt, k, 8, "v", 1) == XTC_OK, "insert");
	}
	CHECK(bt_sample_keys(bt, NULL, 0, NULL, 0, 2, keys, KCAP, lens, MAXS,
	    &n) == XTC_OK && n == 20, "root leaf: all 20 keys (n=%u)", n);
	CHECK(ascending(keys, lens, n), "root leaf ascending");
	be64(50, lo);
	be64(100, hi);
	CHECK(bt_sample_keys(bt, lo, 8, hi, 8, 1, keys, KCAP, lens, MAXS,
	    &n) == XTC_OK && n == 5, "[50,100): 5 keys (n=%u)", n);
	CHECK(n > 0 && get64(keys) == 50 && get64(keys + (n - 1) * KCAP) == 90,
	    "[50,100) = 50..90");
	CHECK(bt_sample_keys(bt, NULL, 0, NULL, 0, 1, keys, 4, lens, MAXS,
	    &n) == XTC_OK && n == 0, "keys longer than key_cap are skipped");
	CHECK(bt_sample_keys(bt, NULL, 0, NULL, 0, -1, keys, KCAP, lens, MAXS,
	    &n) == XTC_E_INVAL, "negative depth rejected");
	bt_close(bt);
	bm_destroy(bm);
	(void)unlink(p);
	printf("  ok   root leaf: keys in [lo, hi) are the sample\n");
}

/* Key i of the skewed set: 90% packed in the top 1% of [0, 2^40). */
static uint64_t
skew_key(uint32_t i)
{
	const uint64_t S = 1ull << 40, top = S - S / 100;

	if (i % 10 != 0)                               /* 9 in 10 keys */
		return top + (uint64_t)i * ((S / 100) / NKEYS);
	return (uint64_t)i * (top / NKEYS);            /* 1 in 10 keys */
}

static void
t_skewed(void)
{
	char p[64];
	bm_t *bm = mk(p, 4096);
	bt_t *bt;
	static uint64_t v[NKEYS];
	static uint8_t keys[MAXS * KCAP], rkeys[MAXS * KCAP];
	static uint16_t lens[MAXS], rlens[MAXS];
	uint64_t cuts[NCHUNK], ucuts[NCHUNK], pg0, pages, rpages;
	uint32_t n = 0, rn = 0, i, j, root_children;
	uint8_t k[8], lo[8], hi[8];
	bt_stats_t st;
	long mn, mx, umn, umx;
	double ratio, uratio;
	char val[24];

	CHECK(bt_open(bm, &bt) == XTC_OK, "open");
	for (i = 0; i < NKEYS; i++)
		v[i] = skew_key(i);
	/* Insert in a scrambled order (realistic fill, not append-only). */
	for (i = 0; i < NKEYS; i++) {
		uint32_t x = (uint32_t)(((uint64_t)i * 2654435761u) % NKEYS);

		be64(v[x], k);
		memset(val, 'a' + (int)(x % 26), sizeof val);
		if (bt_insert(bt, k, 8, val, (uint16_t)(8 + x % 16)) != XTC_OK) {
			CHECK(0, "insert %u", i);
			break;
		}
	}
	qsort(v, NKEYS, sizeof v[0], u64cmp);
	bt_get_stats(bt, &st);
	CHECK(st.height >= 3, "height %llu >= 3", (unsigned long long)st.height);

	/* depth 0: just the root's separators, one page. */
	pg0 = sample_pages(bt);
	CHECK(bt_sample_keys(bt, NULL, 0, NULL, 0, 0, keys, KCAP, lens, MAXS,
	    &n) == XTC_OK, "depth 0");
	CHECK(sample_pages(bt) - pg0 == 1, "depth 0 reads only the root");
	root_children = n + 1;
	CHECK(ascending(keys, lens, n), "depth 0 ascending");

	/* depth 1: root + each child, exactly 1 + root fanout pages. */
	pg0 = sample_pages(bt);
	CHECK(bt_sample_keys(bt, NULL, 0, NULL, 0, 1, keys, KCAP, lens, MAXS,
	    &n) == XTC_OK, "depth 1");
	pages = sample_pages(bt) - pg0;
	CHECK(pages == 1 + root_children, "depth 1 pages %llu == 1 + %u",
	    (unsigned long long)pages, root_children);
	CHECK(n > root_children && n < MAXS, "depth 1 keys %u > root's %u", n,
	    root_children);
	CHECK(ascending(keys, lens, n), "depth 1 strictly ascending");
	/* Separators are truncated (2.0): 1..8 bytes, read zero-padded
	 * (a padded separator still lies between the keys it separates). */
	for (i = 0; i < n; i++) {
		CHECK(lens[i] >= 1 && lens[i] <= 8, "separator %u is 1..8 B "
		    "(%u B)", i, lens[i]);
		memset(keys + i * KCAP + lens[i], 0, KCAP - lens[i]);
	}
	printf("  ok   200k skewed keys, height %llu: depth 0 = 1 page (%u "
	    "children), depth 1 = %llu pages -> %u split points\n",
	    (unsigned long long)st.height, root_children,
	    (unsigned long long)pages, n);

	/* Balance: NCHUNK-1 cuts thinned from the sample vs. a uniform
	 * split of the key space [v[0], v[last]]. */
	j = 0;
	for (i = 0; i < NCHUNK - 1; i++) {
		uint32_t at = (uint32_t)(((uint64_t)(i + 1) * n) / NCHUNK);

		cuts[i] = get64(keys + at * KCAP);
		ucuts[i] = v[0] + (v[NKEYS - 1] - v[0]) / NCHUNK * (i + 1);
		j++;
	}
	ratio = chunk_ratio(v, NKEYS, cuts, (int)j, &mn, &mx);
	uratio = chunk_ratio(v, NKEYS, ucuts, (int)j, &umn, &umx);
	CHECK(ratio < 3.0, "sampled chunks max/min %.2f < 3", ratio);
	CHECK(umn == 0 || uratio > 3.0, "control: uniform split is unbalanced");
	printf("  ok   %d chunks: sampled cuts min %ld max %ld (max/min %.2f); "
	    "uniform key-space cuts min %ld max %ld\n", NCHUNK, mn, mx, ratio,
	    umn, umx);

	/* Range [lo, hi) == full sample restricted to it, fewer pages. */
	be64(v[NKEYS / 3], lo);
	be64(v[NKEYS / 2], hi);
	pg0 = sample_pages(bt);
	CHECK(bt_sample_keys(bt, lo, 8, hi, 8, 1, rkeys, KCAP, rlens, MAXS,
	    &rn) == XTC_OK, "range sample");
	rpages = sample_pages(bt) - pg0;
	j = 0;
	for (i = 0; i < n; i++)
		if (kcmp(keys + i * KCAP, 8, lo, 8) >= 0 &&
		    kcmp(keys + i * KCAP, 8, hi, 8) < 0) {
			CHECK(j < rn && rlens[j] == lens[i] && memcmp(rkeys +
			    j * KCAP, keys + i * KCAP, lens[i]) == 0,
			    "range sample key %u matches", j);
			j++;
		}
	CHECK(j == rn, "range sample has %u keys, full sample in range %u",
	    rn, j);
	CHECK(rpages < pages, "range read %llu < %llu pages",
	    (unsigned long long)rpages, (unsigned long long)pages);
	printf("  ok   range [1/3, 1/2): %u keys == full sample in range, %llu "
	    "pages\n", rn, (unsigned long long)rpages);

	/* Thinning: at most max_keys, still ascending, all from the sample. */
	CHECK(bt_sample_keys(bt, NULL, 0, NULL, 0, 1, rkeys, KCAP, rlens, 7,
	    &rn) == XTC_OK && rn == 7, "thinned to 7 (got %u)", rn);
	CHECK(ascending(rkeys, rlens, rn), "thinned ascending");

	/* depth >= height-1: capped at the leaves' parent level (one leaf
	 * probe), more points, still a sliver of the tree. */
	pg0 = sample_pages(bt);
	CHECK(bt_sample_keys(bt, NULL, 0, NULL, 0, 30, keys, KCAP, lens, MAXS,
	    &n) == XTC_OK, "deep sample");
	pages = sample_pages(bt) - pg0;
	CHECK(ascending(keys, lens, n), "deep sample ascending");
	CHECK(pages < (uint64_t)n / 10 + 64, "deep sample %llu pages for %u "
	    "keys", (unsigned long long)pages, n);
	printf("  ok   depth 30 (capped above the leaves): %u points from %llu "
	    "pages\n", n, (unsigned long long)pages);

	bt_close(bt);
	bm_destroy(bm);
	(void)unlink(p);
}

/* 4. Sampling during concurrent splits and merges. */
static bm_t *g_bm;
static bt_t *g_bt;
static atomic_int g_left;
static atomic_int g_bad, g_samples;

static void
churner(void *arg)
{
	long w = (long)arg;
	uint8_t k[8];
	int r, i;

	for (r = 0; r < 3; r++) {
		for (i = 0; i < 3000; i++) {
			be64((uint64_t)i * 8 + (uint64_t)w, k);
			(void)bt_insert(g_bt, k, 8, "churn-value-xx", 14);
		}
		for (i = 0; i < 3000; i++) {
			be64((uint64_t)i * 8 + (uint64_t)w, k);
			(void)bt_delete(g_bt, k, 8);
		}
	}
	if (atomic_fetch_sub(&g_left, 1) == 1)
		bm_provider_stop(g_bm);
}

static void
sampler(void *arg)
{
	static uint8_t keys[MAXS * KCAP];
	static uint16_t lens[MAXS];
	uint32_t n;

	(void)arg;
	while (atomic_load(&g_left) > 0) {
		int rc = bt_sample_keys(g_bt, NULL, 0, NULL, 0, 2, keys, KCAP,
		    lens, MAXS, &n);

		/* An error after the last churner stopped the page provider
		 * is not the sampler's fault; any other is. */
		if ((rc == XTC_OK && !ascending(keys, lens, n)) ||
		    (rc != XTC_OK && atomic_load(&g_left) > 0))
			atomic_fetch_add(&g_bad, 1);
		atomic_fetch_add(&g_samples, 1);
		xtc_yield();
	}
}

static void
t_concurrent(void)
{
	char p[64];
	xtc_exec_t *exec = NULL;
	xtc_proc_opts_t po = XTC_PROC_OPTS_INIT;
	long w;

	g_bm = mk(p, 48);
	CHECK(bt_open(g_bm, &g_bt) == XTC_OK, "open");
	atomic_store(&g_left, 4);
	CHECK(xtc_exec_init(&exec, 4) == XTC_OK, "exec");
	CHECK(bm_provider_spawn(g_bm, xtc_exec_loop(exec, 0), 1000000LL,
	    NULL) == XTC_OK, "provider");
	for (w = 0; w < 4; w++) {
		po.name = "churn";
		CHECK(xtc_proc_spawn(xtc_exec_loop(exec, (int)w), churner,
		    (void *)w, &po, NULL) == XTC_OK, "spawn");
	}
	po.name = "sample";
	CHECK(xtc_proc_spawn(xtc_exec_loop(exec, 3), sampler, NULL, &po,
	    NULL) == XTC_OK, "spawn sampler");
	CHECK(xtc_exec_run(exec) == XTC_OK, "run");
	(void)xtc_exec_fini(exec);
	CHECK(atomic_load(&g_bad) == 0, "%d bad samples",
	    atomic_load(&g_bad));
	CHECK(atomic_load(&g_samples) > 0, "sampler ran");
	bt_close(g_bt);
	bm_destroy(g_bm);
	(void)unlink(p);
	printf("  ok   %d samples during concurrent splits/merges, all "
	    "ascending\n", atomic_load(&g_samples));
}

int
main(void)
{
	t_root_leaf();
	t_skewed();
	t_concurrent();
	if (g_fail) {
		fprintf(stderr, "test_btree_sample: FAILURES\n");
		return 1;
	}
	printf("test_btree_sample: ALL PASS (%d checks)\n", g_checks);
	return 0;
}
