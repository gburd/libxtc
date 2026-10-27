/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_btree_shift.c
 *	Redistribution before a split (2.0, bt_shift): a full leaf moves
 *	entries into a sibling with room instead of splitting.
 *
 *	  1. fill: 300k random 20-byte keys with 40-byte values on 4 KiB
 *	     pages leave the leaves >= 78% full (a split-only tree: ~70%),
 *	     with shifts into both siblings, and every key reads back and
 *	     scans in order; every node passes btnode_check (BT_CHECK).
 *	  2. a parked cursor whose position moved into the LEFT sibling: a
 *	     cursor parks on leaf C after returning key k; inserts into C
 *	     shift its lower part, k included, into C's left sibling.  The
 *	     resume must re-descend (C's lower fence is now >= k) and
 *	     return every key after k exactly once, in order.  With the
 *	     lower-fence re-check skipped (planted bug SHIFTLO, compiled in
 *	     with -DXTC_DST_INJECT_BUG=22) the scan resumes at C's first
 *	     key and skips the keys that moved: the test fails.
 *	A cursor HOPPING onto C after the shift is refused by cursor_hop_ok
 *	(C's lower fence no longer equals the upper fence it left) and
 *	re-descends; test_sim_btree_scan drives both under the simulator.
 */
#define BT_CHECK 1
#define BT_SHIFT_PCT 80   /* redistribution on (off by default) */
#include "bufmgr.c"
#include "btree.c"
#include "t_tmp.h"

#include <unistd.h>

#define PS 4096

static int g_fail;
#define TCK(c, ...) do { if (!(c)) { \
	fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); \
	g_fail = 1; } } while (0)

static void
key20(uint8_t k[20], uint64_t x)
{
	int i;

	memset(k, 0, 20);
	k[3] = 7;                                /* one table id */
	for (i = 11; i >= 4; i--, x >>= 8)
		k[i] = (uint8_t)x;
	memset(k + 12, 0xEE, 8);
}

static int
mk(const char *tag, bm_t **bm, bt_t **bt, char *path, size_t cap)
{
	bm_opts_t bo = BM_OPTS_DEFAULT;
	int fd;

	t_tmpl(path, cap, tag);
	if ((fd = mkstemp(path)) < 0)
		return -1;
	close(fd);
	bo.path = path;
	bo.page_size = PS;
	bo.n_frames = 32768;
	if (bm_create(&bo, bm) != XTC_OK || bt_open(*bm, bt) != XTC_OK)
		return -1;
	return 0;
}

static void
done(bm_t *bm, bt_t *bt, const char *path)
{
	bt_close(bt);
	bm_destroy(bm);
	unlink(path);
}

/* Leaf fill: bytes in use (slots + cells + fences) over the pages. */
static double
leaf_fill(bt_t *bt, uint64_t *leaves)
{
	bt_cursor_t *c = NULL;
	const void *k, *v;
	uint16_t kl, vl;
	uint64_t used = 0, n = 0;
	bm_pid_t last = BM_PID_NONE;

	if (bt_cursor_open(bt, NULL, 0, &c) != XTC_OK)
		return 0;
	while (bt_cursor_next(c, &k, &kl, &v, &vl) == XTC_OK) {
		bm_pid_t p = bm_frame_pid(c->leaf);
		if (p != last) {
			const void *pg = bm_page(c->leaf);
			used += (uint64_t)btnode_count(pg) * 10u +
			    btnode_used_bytes(pg);
			n++;
			last = p;
		}
	}
	bt_cursor_close(c);
	*leaves = n;
	return n ? 100.0 * (double)used / ((double)n * (PS -
	    sizeof(struct btnode_hdr))) : 0;
}

static void
t_fill(void)
{
	bm_t *bm;
	bt_t *bt;
	char path[256];
	uint8_t k[20], v[40], out[40], prev[20];
	const void *ck, *cv;
	uint16_t kl, vl;
	bt_cursor_t *c = NULL;
	bt_stats_t st;
	uint64_t i, n = 300000, found = 0, scanned = 0, bad = 0, leaves = 0;
	uint64_t x = 88172645463325252ull;
	double fill;

	TCK(mk("sqlxtc-shift1", &bm, &bt, path, sizeof path) == 0, "setup");
	memset(v, 'v', sizeof v);
	for (i = 0; i < n; i++) {
		x ^= x << 13; x ^= x >> 7; x ^= x << 17;
		key20(k, x % 100000000ull * 4 + 1);
		(void)bt_insert(bt, k, 20, v, sizeof v);
	}
	bt_get_stats(bt, &st);
	fill = leaf_fill(bt, &leaves);
	x = 88172645463325252ull;
	for (i = 0; i < n; i++) {
		x ^= x << 13; x ^= x >> 7; x ^= x << 17;
		key20(k, x % 100000000ull * 4 + 1);
		if (bt_lookup(bt, k, 20, out, sizeof out, &vl) == XTC_OK)
			found++;
	}
	TCK(found == n, "1. %llu of %llu keys read back",
	    (unsigned long long)found, (unsigned long long)n);
	TCK(bt_cursor_open(bt, NULL, 0, &c) == XTC_OK, "cursor");
	while (bt_cursor_next(c, &ck, &kl, &cv, &vl) == XTC_OK) {
		if (scanned > 0 && memcmp(prev, ck, 20) >= 0)
			bad++;
		memcpy(prev, ck, 20);
		scanned++;
	}
	bt_cursor_close(c);
	TCK(bad == 0, "1. scan out of order %llu times", (unsigned long long)bad);
	printf("  1. %llu random inserts: %llu leaves, fill %.1f%%, splits "
	    "%llu, shifts %llu (%llu left), height %llu\n",
	    (unsigned long long)n, (unsigned long long)leaves, fill,
	    (unsigned long long)st.splits, (unsigned long long)st.shifts,
	    (unsigned long long)st.shifts_left,
	    (unsigned long long)st.height);
	TCK(fill >= 78.0, "1. leaf fill %.1f%% (want >= 78; split-only ~70)",
	    fill);
	TCK(st.shifts > 0 && st.shifts_left > 0 && st.shifts_left < st.shifts,
	    "1. shifts %llu, left %llu (want both sides)",
	    (unsigned long long)st.shifts, (unsigned long long)st.shifts_left);
	done(bm, bt, path);
}

static uint64_t
kval(const void *k)
{
	uint64_t x = 0;
	int i;

	for (i = 4; i < 12; i++)
		x = x << 8 | ((const uint8_t *)k)[i];
	return x;
}

/* Case 2.  The leaves hold even keys 2..2N, filled by append splits
 * (each full).  The cursor parks after the second key of a leaf C in the
 * middle.  C's left sibling L is thinned by a few deletes (it stays far
 * above the merge threshold) so it is the only sibling with room; then
 * odd keys go into C until it is full: bt_shift moves C's lower part --
 * the parked position included -- into L. */
static void
t_resume_left(void)
{
	bm_t *bm;
	bt_t *bt;
	char path[256];
	uint8_t k[20], v[40], fk[BT_MAX_KEY];
	const void *ck, *cv;
	uint16_t kl, vl, fkl = 0;
	bt_cursor_t *c = NULL, *o = NULL;
	bt_stats_t st0, st1;
	bm_frame_t *f;
	uint64_t i, n = 2000, first, pos, got = 0, want = 0, prev = 0, bad = 0;

	TCK(mk("sqlxtc-shift2", &bm, &bt, path, sizeof path) == 0, "setup");
	memset(v, 'v', sizeof v);
	for (i = 1; i <= n; i++) {
		key20(k, 2 * i);
		TCK(bt_insert(bt, k, 20, v, sizeof v) == XTC_OK, "load");
	}
	/* C: the leaf holding key n; its first key. */
	key20(k, n);
	TCK(bt_cursor_open(bt, k, 20, &c) == XTC_OK, "open C");
	TCK(btnode_full_key(bm_page(c->leaf), 0, fk, sizeof fk, &fkl) == 0 &&
	    fkl == 20, "C's first key");
	bt_cursor_close(c);
	first = kval(fk);
	pos = first + 2;
	/* Park after C's second key. */
	key20(k, first);
	TCK(bt_cursor_open(bt, k, 20, &c) == XTC_OK, "open");
	TCK(bt_cursor_next(c, &ck, &kl, &cv, &vl) == XTC_OK &&
	    kval(ck) == first, "first");
	TCK(bt_cursor_next(c, &ck, &kl, &cv, &vl) == XTC_OK &&
	    kval(ck) == pos, "second");
	TCK(bt_cursor_park(c) == XTC_OK, "park");
	/* Room in L only: every third key below C, 20 of them. */
	for (i = 0; i < 20; i++) {
		key20(k, first - 2 - 6 * i);
		TCK(bt_delete(bt, k, 20) == XTC_OK, "thin L");
	}
	/* Odd keys into C, descending (an ascending run would take the
	 * split's sequential cut instead), until C is full and relieved. */
	bt_get_stats(bt, &st0);
	st1 = st0;
	for (i = 30; i + 1 > 0 && st1.shifts == st0.shifts &&
	    st1.splits == st0.splits; i--) {
		key20(k, pos + 1 + 2 * i);
		TCK(bt_insert(bt, k, 20, v, sizeof v) == XTC_OK, "insert into C");
		bt_get_stats(bt, &st1);
	}
	TCK(st1.shifts_left == st0.shifts_left + 1 && st1.splits == st0.splits,
	    "2. C was relieved by a left shift (%llu -> %llu), not a split "
	    "(%llu -> %llu)", (unsigned long long)st0.shifts_left,
	    (unsigned long long)st1.shifts_left,
	    (unsigned long long)st0.splits, (unsigned long long)st1.splits);
	/* The position left C: C's lower fence is at or above it. */
	TCK(bm_fix_pid(bm, c->parked_pid, &f) == XTC_OK, "fix C");
	key20(k, pos);
	TCK(btnode_below_lo_fence(bm_page(f), k, 20), "2. the parked position "
	    "is still in C: the shift did not move it left");
	bm_unfix(bm, f, 0);
	/* Every key after the position, from a fresh cursor. */
	key20(k, pos + 1);
	if (bt_cursor_open(bt, k, 20, &o) == XTC_OK) {
		while (bt_cursor_next(o, &ck, &kl, &cv, &vl) == XTC_OK)
			want++;
		bt_cursor_close(o);
	}
	/* The resumed cursor must return exactly those. */
	TCK(bt_cursor_resume(c) == XTC_OK, "resume");
	while (bt_cursor_next(c, &ck, &kl, &cv, &vl) == XTC_OK) {
		uint64_t kv = kval(ck);

		if (kv <= pos || (got > 0 && kv <= prev))
			bad++;
		prev = kv;
		got++;
	}
	bt_cursor_close(c);
	printf("  2. parked after key %llu; inserts shifted it into the "
	    "left sibling; resume returned %llu of the %llu keys after it, "
	    "%llu out of place\n", (unsigned long long)pos,
	    (unsigned long long)got, (unsigned long long)want,
	    (unsigned long long)bad);
	TCK(want > 0 && got == want && bad == 0, "2. resume after a left "
	    "shift returned %llu of %llu keys, %llu out of place",
	    (unsigned long long)got, (unsigned long long)want,
	    (unsigned long long)bad);
	done(bm, bt, path);
}

int
main(void)
{
	t_fill();
	t_resume_left();
	printf("test_btree_shift: %s\n", g_fail ? "FAILED" : "ok");
	return g_fail;
}
