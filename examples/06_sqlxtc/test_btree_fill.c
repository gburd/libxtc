/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_btree_fill.c
 *	Sequential (append) inserts must fill pages, not leave them half
 *	empty.  Every split cut the node at the byte midpoint, so a key
 *	stream that always lands at the end of its node (ascending rowids,
 *	time-ordered ids, a table's newest rows) left each left page ~50%
 *	full forever: the ledger store was 5.3x SQLite's size and TPC-C's
 *	7.1 GB against 0.9 GB.  Berkeley DB's __bam_psplit moves one item to
 *	the new page when the insert is at the end; SQLite's balance_quick
 *	and InnoDB's insert-point split do the same.
 *
 *	Measures pages allocated for N keys, three patterns:
 *	  ascending   (append):      fill >= 85% (was ~50%)
 *	  K interleaved runs: K tables growing at once, each seeded with a few
 *	              thousand rows so its newest key is at the end of its
 *	              own leaf (the shape of several tables in one tree):
 *	              fill >= 80%
 *	  random      no regression: fill >= 60%, every key found
 *	Fill = (sum of entry bytes) / (pages x page size).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bufmgr.h"
#include "btree.h"
#include "btnode.h"
#include "t_tmp.h"

static int g_fail;

static void
enc(uint8_t k[20], uint32_t run, uint64_t seq)
{
	int i;
	for (i = 3; i >= 0; i--) { k[i] = (uint8_t)run; run >>= 8; }
	for (i = 11; i >= 4; i--) { k[i] = (uint8_t)seq; seq >>= 8; }
	memset(k + 12, 0xEE, 8);
}

static double
run(const char *name, int pattern, int n, double min_fill)
{
	bm_opts_t bo = BM_OPTS_DEFAULT;
	bm_t *bm = NULL;
	bt_t *bt = NULL;
	bt_stats_t st;
	char path[256];
	uint8_t k[20], v[60];
	uint64_t *perm = NULL, pages, used = 0;
	double fill;
	int fd, i, bad = 0;

	t_tmpl(path, sizeof path, "sqlxtc-fill");
	if ((fd = mkstemp(path)) < 0) return 0;
	close(fd);
	bo.path = path; bo.page_size = 4096; bo.n_frames = 8192;
	if (bm_create(&bo, &bm) != XTC_OK || bt_open(bm, &bt) != XTC_OK) return 0;
	memset(v, 'v', sizeof v);
	if (pattern == 2) {
		uint64_t x = 88172645463325252ull;
		perm = malloc(sizeof *perm * (size_t)n);
		for (i = 0; i < n; i++) perm[i] = (uint64_t)i;
		for (i = n - 1; i > 0; i--) {
			uint64_t t;
			x ^= x << 13; x ^= x >> 7; x ^= x << 17;
			t = perm[i]; perm[i] = perm[x % (uint64_t)(i + 1)]; perm[x % (uint64_t)(i + 1)] = t;
		}
	}
	if (pattern == 1) {
		int r, j;
		for (r = 0; r < 8; r++)                /* seed each run */
			for (j = 0; j < 2000; j++) {
				enc(k, (uint32_t)r, (uint64_t)j);
				if (bt_insert(bt, k, sizeof k, v, sizeof v) != XTC_OK) bad++;
				used += btnode_entry_cost(sizeof k, sizeof v);
			}
	}
	for (i = 0; i < n; i++) {
		if (pattern == 0) enc(k, 7, (uint64_t)i);
		else if (pattern == 1) enc(k, (uint32_t)(i % 8), 2000 + (uint64_t)(i / 8));
		else enc(k, 7, perm[i]);
		if (bt_insert(bt, k, sizeof k, v, sizeof v) != XTC_OK) { bad++; break; }
		used += btnode_entry_cost(sizeof k, sizeof v);
	}
	for (i = 0; i < n && !bad; i += 97) {
		uint8_t out[64];
		uint16_t ol = 0;
		if (pattern == 0) enc(k, 7, (uint64_t)i);
		else if (pattern == 1) enc(k, (uint32_t)(i % 8), 2000 + (uint64_t)(i / 8));
		else enc(k, 7, (uint64_t)i);
		if (bt_lookup(bt, k, sizeof k, out, sizeof out, &ol) != XTC_OK || ol != sizeof v) bad++;
	}
	bt_get_stats(bt, &st);
	pages = st.splits + 1;                 /* every split allocates one page */
	fill = (double)used / ((double)pages * 4096.0);
	printf("  %-22s n=%d pages=%llu height=%llu fill=%.0f%%%s\n", name, n,
	    (unsigned long long)pages, (unsigned long long)st.height, fill * 100.0,
	    bad ? " LOOKUP FAIL" : "");
	if (bad || fill < min_fill) {
		fprintf(stderr, "FAIL %s: fill %.0f%% < %.0f%% or %d lookups failed\n",
		    name, fill * 100.0, min_fill * 100.0, bad);
		g_fail = 1;
	}
	free(perm);
	bt_close(bt);
	bm_destroy(bm);
	unlink(path);
	return fill;
}

int
main(void)
{
	(void)run("ascending", 0, 200000, 0.85);
	(void)run("8 interleaved runs", 1, 200000, 0.80);
	(void)run("random", 2, 200000, 0.60);
	if (g_fail) { fprintf(stderr, "FAIL: test_btree_fill\n"); return 1; }
	printf("  ok   append inserts fill pages; random inserts unchanged\n");
	return 0;
}
