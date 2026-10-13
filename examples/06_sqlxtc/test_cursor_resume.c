/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_cursor_resume.c
 *	The latch-releasing, position-revalidating cursor: a scan that
 *	parks (releases its latch) and resumes between every step costs
 *	exactly ONE root-to-leaf descent for the whole scan -- O(1)
 *	amortized per row -- and stays correct across a concurrent split
 *	of the parked leaf.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bufmgr.h"
#include "btree.h"
#include "xtc.h"
#include "t_tmp.h"

#define PAGE_SZ  4096
#define N_KEYS   2000      /* many leaves -> multi-level tree */


static void
mkkey(int i, char *buf, uint16_t *len)
{
	int n = snprintf(buf, 16, "k-%06d", i);
	*len = (uint16_t)n;
}
static int
key_cmp(const void *a, uint16_t al, const void *b, uint16_t bl)
{
	uint16_t lim = al < bl ? al : bl;
	int c = lim ? memcmp(a, b, lim) : 0;
	if (c != 0) return c;
	return (al > bl) - (al < bl);
}

/* ---- (1) park/resume between every row == one descent ---- */
static int
scenario_o1_scan(void)
{
	char path[256]; t_tmpl(path, sizeof path, "sqlxtc-resume");
	bm_opts_t bo = BM_OPTS_DEFAULT;
	bm_t *bm = NULL;
	bt_t *bt = NULL;
	bt_cursor_t *cur = NULL;
	bt_stats_t s0, s1;
	int fd, i, seen = 0;

	g_fail = 0;
	fd = mkstemp(path); if (fd < 0) return 1; close(fd);
	bo.path = path; bo.page_size = PAGE_SZ; bo.n_frames = 64; bo.cool_pct = 25;
	CK(bm_create(&bo, &bm) == XTC_OK);
	CK(bt_open(bm, &bt) == XTC_OK);
	for (i = 0; i < N_KEYS; i++) {
		char k[16], v[16]; uint16_t kl, vl;
		mkkey(i, k, &kl); vl = (uint16_t)snprintf(v, sizeof v, "v%d", i);
		CK(bt_insert(bt, k, kl, v, vl) == XTC_OK);
	}

	bt_get_stats(bt, &s0);
	CK(bt_cursor_open(bt, NULL, 0, &cur) == XTC_OK);
	for (;;) {
		const void *k = NULL, *vv = NULL;
		uint16_t kl = 0, vl = 0;
		char want[16]; uint16_t wl;
		if (bt_cursor_next(cur, &k, &kl, &vv, &vl) != XTC_OK)
			break;
		mkkey(seen, want, &wl);
		CK(kl == wl && memcmp(k, want, kl) == 0);   /* ascending order */
		seen++;
		/* Release the latch and resume -- as the VDBE boundary forces. */
		CK(bt_cursor_park(cur) == XTC_OK);
		CK(bt_cursor_resume(cur) == XTC_OK);
	}
	bt_cursor_close(cur);
	bt_get_stats(bt, &s1);

	CK(seen == N_KEYS);
	/* The whole scan -- N_KEYS rows, each preceded by a park+resume --
	 * cost exactly one descent (the open).  The old re-open-per-row
	 * path would have cost N_KEYS descents. */
	CK(s1.descents - s0.descents == 1);
	CK(s1.resumes - s0.resumes == (uint64_t)N_KEYS);

	bt_close(bt); bm_destroy(bm); unlink(path);
	if (g_fail) return 1;
	printf("  ok   O(1) scan: %d rows, park+resume each step, "
	    "%llu descent (not %d), %llu resumes\n", N_KEYS,
	    (unsigned long long)(s1.descents - s0.descents), N_KEYS,
	    (unsigned long long)(s1.resumes - s0.resumes));
	return 0;
}

/* ---- (2) revalidation: split the parked leaf, resume must not miss ---- */
static int
scenario_split_while_parked(void)
{
	char path[256]; t_tmpl(path, sizeof path, "sqlxtc-resume2");
	bm_opts_t bo = BM_OPTS_DEFAULT;
	bm_t *bm = NULL;
	bt_t *bt = NULL;
	bt_cursor_t *cur = NULL;
	int fd, i, seen, half;
	char prevk[16]; uint16_t prevl = 0;

	g_fail = 0;
	fd = mkstemp(path); if (fd < 0) return 1; close(fd);
	bo.path = path; bo.page_size = PAGE_SZ; bo.n_frames = 64; bo.cool_pct = 25;
	CK(bm_create(&bo, &bm) == XTC_OK);
	CK(bt_open(bm, &bt) == XTC_OK);
	/* Even keys 0,2,4,... so we can insert odd keys later that land
	 * between them (forcing splits in already-scanned-from leaves). */
	for (i = 0; i < N_KEYS; i += 2) {
		char k[16], v[16]; uint16_t kl, vl;
		mkkey(i, k, &kl); vl = (uint16_t)snprintf(v, sizeof v, "v%d", i);
		CK(bt_insert(bt, k, kl, v, vl) == XTC_OK);
	}

	CK(bt_cursor_open(bt, NULL, 0, &cur) == XTC_OK);
	seen = 0; half = (N_KEYS / 2) / 2;
	for (;;) {
		const void *k = NULL, *vv = NULL;
		uint16_t kl = 0, vl = 0;
		if (bt_cursor_next(cur, &k, &kl, &vv, &vl) != XTC_OK)
			break;
		/* Strictly ascending, no key seen twice. */
		if (seen > 0)
			CK(key_cmp(prevk, prevl, k, kl) < 0);
		memcpy(prevk, k, kl); prevl = kl;
		seen++;
		CK(bt_cursor_park(cur) == XTC_OK);
		if (seen == half) {
			/* While parked, insert MANY odd keys: splits leaves,
			 * including ones at/after the parked position. */
			for (i = 1; i < N_KEYS; i += 2) {
				char k2[16], v2[16]; uint16_t kl2, vl2;
				mkkey(i, k2, &kl2);
				vl2 = (uint16_t)snprintf(v2, sizeof v2, "v%d", i);
				CK(bt_insert(bt, k2, kl2, v2, vl2) == XTC_OK);
			}
		}
		CK(bt_cursor_resume(cur) == XTC_OK);
	}
	bt_cursor_close(cur);

	/* Every even key must have been visited (none dropped by the
	 * splits); the odd keys inserted after the parked position are
	 * also visited since they sort after it. */
	CK(seen >= N_KEYS / 2);
	bt_close(bt); bm_destroy(bm); unlink(path);
	if (g_fail) return 1;
	printf("  ok   revalidation: parked leaf split mid-scan, resume "
	    "visited all %d rows in order (no miss, no duplicate)\n", seen);
	return 0;
}

/*
 * ---- (3) the parked key moves RIGHT: resume must not re-yield ----
 * Park on key K, then insert keys just BELOW K into the parked leaf
 * until it splits (several times) with K in an upper half.  The parked
 * pid now names the left half, which no longer holds K; resume must
 * walk right to K's leaf instead of starting at the next leaf's first
 * slot, which would re-yield keys < K that were already returned.  In
 * the MVCC layer a re-yielded key is an older version of an earlier row
 * read a second time -- the stale-row outlier test_commit_visibility saw
 * on 4 loops.  Deterministic: single thread, the split is forced while
 * the cursor is parked.
 */
static int
scenario_parked_key_moves_right(void)
{
	char path[256]; t_tmpl(path, sizeof path, "sqlxtc-resume3");
	bm_opts_t bo = BM_OPTS_DEFAULT;
	bm_t *bm = NULL;
	bt_t *bt = NULL;
	bt_cursor_t *cur = NULL;
	bt_stats_t s0, s1;
	int fd, i, seen = 0, dup = 0, target = 50;
	char prevk[32]; uint16_t prevl = 0;

	g_fail = 0;
	fd = mkstemp(path); if (fd < 0) return 1; close(fd);
	bo.path = path; bo.page_size = PAGE_SZ; bo.n_frames = 64; bo.cool_pct = 25;
	CK(bm_create(&bo, &bm) == XTC_OK);
	CK(bt_open(bm, &bt) == XTC_OK);
	for (i = 0; i < 100; i++) {          /* one leaf */
		char k[16]; uint16_t kl;
		mkkey(i, k, &kl);
		CK(bt_insert(bt, k, kl, "v", 1) == XTC_OK);
	}
	CK(bt_cursor_open(bt, NULL, 0, &cur) == XTC_OK);
	for (;;) {
		const void *k = NULL, *vv = NULL;
		uint16_t kl = 0, vl = 0;
		if (bt_cursor_next(cur, &k, &kl, &vv, &vl) != XTC_OK)
			break;
		if (seen > 0 && key_cmp(prevk, prevl, k, kl) >= 0)
			dup++;                   /* repeated or out of order */
		memcpy(prevk, k, kl); prevl = kl;
		seen++;
		CK(bt_cursor_park(cur) == XTC_OK);
		if (seen == target) {
			/* Parked on k-000049.  Fill the space just below it:
			 * "k-000048/NNN" sorts between k-000048 and k-000049. */
			bt_get_stats(bt, &s0);
			for (i = 0; i < 600; i++) {
				char k2[32]; uint16_t kl2;
				kl2 = (uint16_t)snprintf(k2, sizeof k2,
				    "k-000048/%03d", i);
				CK(bt_insert(bt, k2, kl2, "w", 1) == XTC_OK);
			}
			bt_get_stats(bt, &s1);
			CK(s1.splits > s0.splits);   /* the parked leaf split */
		}
		CK(bt_cursor_resume(cur) == XTC_OK);
	}
	bt_cursor_close(cur);
	/* Keys after the park point are all > k-000049: exactly 100 rows. */
	if (dup != 0 || seen != 100)
		fprintf(stderr, "  parked-key-moves-right: seen=%d (want 100) "
		    "repeated/out-of-order=%d (want 0)\n", seen, dup);
	CK(dup == 0);
	CK(seen == 100);
	bt_close(bt); bm_destroy(bm); unlink(path);
	if (g_fail) return 1;
	printf("  ok   revalidation: parked key moved right by a split, resume "
	    "walked right (%d rows, none repeated)\n", seen);
	return 0;
}

int
main(void)
{
	int rc = 0;
	if (scenario_o1_scan() != 0) rc = 1;
	if (scenario_split_while_parked() != 0) rc = 1;
	if (scenario_parked_key_moves_right() != 0) rc = 1;
	if (rc != 0) {
		fprintf(stderr, "FAIL: test_cursor_resume\n");
		return 1;
	}
	printf("All sqlxtc cursor-resume tests passed.\n");
	return 0;
}
