/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_btnode.c
 *	Standalone unit test for the slotted-page B-tree node module.
 *	Plain asserts + printf; exits nonzero on the first failure.
 *
 *	Build:
 *	  cd examples/06_sqlxtc && \
 *	    gcc -std=c11 -Wall -Wextra -O1 -fsanitize=address \
 *	      -o /tmp/test_btnode test_btnode.c btnode.c && /tmp/test_btnode
 */

#include "btnode.h"
#include "t_pagebytes.h"

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks;

#define CHECK(cond, ...)                                                  \
	do {                                                              \
		g_checks++;                                                \
		if (!(cond)) {                                            \
			fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
			fprintf(stderr, __VA_ARGS__);                     \
			fprintf(stderr, "\n");                            \
			exit(1);                                          \
		}                                                         \
	} while (0)

static int
cmpb(const void *a, uint16_t al, const void *b, uint16_t bl)
{
	uint16_t lim = al < bl ? al : bl;
	int c = lim ? memcmp(a, b, lim) : 0;

	if (c != 0)
		return c < 0 ? -1 : 1;
	if (al < bl)
		return -1;
	if (al > bl)
		return 1;
	return 0;
}

/*
 * ~200 keys sharing a long prefix: verify prefix detection, search,
 * get, full-key round-trip, count, and that slots stay sorted.
 */
static void
test_prefix_leaf(void)
{
	const uint32_t ps = 16384;
	const int N = 200;
	uint8_t *page = malloc(ps);
	char lo[24], hi[24];
	int i;
	uint8_t prev[32];
	uint16_t prev_len = 0;

	CHECK(page != NULL, "malloc");
	btnode_init(page, ps, 1);

	snprintf(lo, sizeof(lo), "user:%08d", 1);
	snprintf(hi, sizeof(hi), "user:%08d", N);
	btnode_set_fences(page, lo, 13, hi, 13);

	/* "user:00000001" vs "user:00000200" share "user:00000" (10). */
	CHECK(btnode_prefix_len(page) == 10, "prefix_len detected got %u",
	    btnode_prefix_len(page));
	CHECK(btnode_is_leaf(page) == 1, "is_leaf");

	/* Insert in a permuted order (7 is coprime with 200). */
	for (i = 0; i < N; i++) {
		int v = ((i * 7) % N) + 1; /* 1..N, each once */
		char key[24];
		uint64_t val = (uint64_t)v * 1000u + 7u;
		int r;

		snprintf(key, sizeof(key), "user:%08d", v);
		r = btnode_insert(page, key, 13, &val, sizeof(val));
		CHECK(r == 0, "insert v=%d", v);
	}
	CHECK(btnode_count(page) == N, "count == %d got %u", N,
	    btnode_count(page));

	/* Search / get / full_key round-trip for every key. */
	for (i = 1; i <= N; i++) {
		char key[24];
		int found = -1, slot;
		const void *ks, *vp;
		uint16_t kl, vl;
		uint8_t full[32];
		uint16_t fl = 0;
		uint64_t got;

		snprintf(key, sizeof(key), "user:%08d", i);
		slot = btnode_search(page, key, 13, &found);
		CHECK(found == 1, "search found v=%d", i);

		CHECK(btnode_get(page, slot, &ks, &kl, &vp, &vl) == 0, "get");
		CHECK(kl == 3, "suffix len == 3 got %u (v=%d)", kl, i);
		CHECK(vl == sizeof(uint64_t), "val len");
		memcpy(&got, vp, sizeof(got));
		CHECK(got == (uint64_t)i * 1000u + 7u, "value v=%d got %llu", i,
		    (unsigned long long)got);
		/* suffix must match the tail of the full key */
		CHECK(memcmp(ks, key + 10, 3) == 0, "suffix bytes v=%d", i);

		CHECK(btnode_full_key(page, slot, full, sizeof(full), &fl) == 0,
		    "full_key");
		CHECK(fl == 13 && memcmp(full, key, 13) == 0,
		    "full_key round-trip v=%d", i);
	}

	/* Slots must be in ascending full-key order. */
	for (i = 0; i < (int)btnode_count(page); i++) {
		uint8_t full[32];
		uint16_t fl = 0;

		CHECK(btnode_full_key(page, i, full, sizeof(full), &fl) == 0,
		    "full_key iter");
		if (i > 0)
			CHECK(cmpb(prev, prev_len, full, fl) < 0,
			    "slots sorted at %d", i);
		memcpy(prev, full, fl);
		prev_len = fl;
	}

	/* A missing key reports not found. */
	{
		int found = -1;
		char key[24];

		snprintf(key, sizeof(key), "user:%08d", N + 5);
		(void)btnode_search(page, key, 13, &found);
		CHECK(found == 0, "missing key not found");
	}

	free(page);
	printf("  test_prefix_leaf: ok (%d keys, prefix=10)\n", N);
}

/*
 * Fill a node until insert fails, split it, and verify nothing is lost,
 * the separator is correct, prefixes are recomputed, and the sibling
 * link is transferred.
 */
static void
test_fill_and_split(void)
{
	const uint32_t ps = 4096;
	uint8_t *L = malloc(ps), *R = malloc(ps);
	char lo[] = "k0000000000"; /* 11 bytes */
	char hi[] = "k9999999999";
	uint16_t old_prefix;
	int total = 0, n;
	uint8_t sep[64];
	uint16_t sep_len = 0;
	uint16_t lcnt, rcnt, lpref, rpref;
	int i;

	CHECK(L != NULL && R != NULL, "malloc");
	btnode_init(L, ps, 1);
	btnode_set_fences(L, lo, 11, hi, 11);
	old_prefix = btnode_prefix_len(L);
	CHECK(old_prefix == 1, "fence prefix == 1 got %u", old_prefix);

	for (n = 0;; n++) {
		char key[24];
		uint32_t v = (uint32_t)n;
		int r;

		snprintf(key, sizeof(key), "k%010d", n);
		r = btnode_insert(L, key, 11, &v, sizeof(v));
		if (r < 0)
			break;
	}
	total = n;
	CHECK(total > 8, "filled enough keys got %d", total);
	CHECK(btnode_count(L) == total, "filled count");

	/* Wire a downstream sibling so we can check it transfers. */
	btnode_set_right_sibling(L, 0xABCD1234u);

	btnode_init(R, ps, 1);
	CHECK(btnode_split(L, R, sep, &sep_len) == 0, "split");

	lcnt = btnode_count(L);
	rcnt = btnode_count(R);
	CHECK(lcnt + rcnt == total, "no slot lost: %u+%u != %d", lcnt, rcnt,
	    total);
	CHECK(lcnt > 0 && rcnt > 0, "both halves nonempty");

	/* Right node inherits the old downstream sibling. */
	CHECK(btnode_right_sibling(R) == 0xABCD1234u, "sibling transferred");
	/* Left's link is cleared for the caller to rewire. */
	CHECK(btnode_right_sibling(L) == 0, "left sibling cleared");

	/* Prefixes recomputed: left narrowed, both at least the old one. */
	CHECK(btnode_prefix_len(L) >= old_prefix, "left prefix >= old");
	CHECK(btnode_prefix_len(R) >= old_prefix, "right prefix >= old");
	CHECK(btnode_prefix_len(L) > old_prefix,
	    "left prefix grew (%u > %u)", btnode_prefix_len(L), old_prefix);

	/* Every original key lives in exactly one half. */
	for (i = 0; i < total; i++) {
		char key[24];
		int fl = -1, fr = -1;

		snprintf(key, sizeof(key), "k%010d", i);
		(void)btnode_search(L, key, 11, &fl);
		(void)btnode_search(R, key, 11, &fr);
		CHECK((fl ? 1 : 0) + (fr ? 1 : 0) == 1,
		    "key %d in exactly one half (L=%d R=%d)", i, fl, fr);
	}

	/* Separator splits the key space: left <= sep < right. */
	for (i = 0; i < (int)lcnt; i++) {
		uint8_t full[32];
		uint16_t fl = 0;

		CHECK(btnode_full_key(L, i, full, sizeof(full), &fl) == 0,
		    "L full_key");
		CHECK(cmpb(full, fl, sep, sep_len) <= 0,
		    "left key <= sep at %d", i);
	}
	for (i = 0; i < (int)rcnt; i++) {
		uint8_t full[32];
		uint16_t fl = 0;

		CHECK(btnode_full_key(R, i, full, sizeof(full), &fl) == 0,
		    "R full_key");
		CHECK(cmpb(full, fl, sep, sep_len) > 0,
		    "right key > sep at %d", i);
	}

	lpref = btnode_prefix_len(L);
	rpref = btnode_prefix_len(R);
	free(L);
	free(R);
	printf("  test_fill_and_split: ok (%d keys -> %u + %u, prefixes %u/%u)\n",
	    total, lcnt, rcnt, lpref, rpref);
}

/* Binary keys with embedded NUL bytes and varying lengths. */
static void
test_binary_keys(void)
{
	const uint32_t ps = 4096;
	uint8_t *page = malloc(ps);
	uint8_t lof[2] = { 0x20, 0x00 };
	uint8_t hif[2] = { 0x20, 0xff };
	const int NB = 120;
	struct {
		uint8_t k[8];
		uint16_t kl;
		uint32_t v;
	} items[120];
	int i;

	CHECK(page != NULL, "malloc");
	btnode_init(page, ps, 1);
	btnode_set_fences(page, lof, 2, hif, 2);
	CHECK(btnode_prefix_len(page) == 1, "binary prefix == 1 got %u",
	    btnode_prefix_len(page));

	for (i = 0; i < NB; i++) {
		uint8_t *k = items[i].k;
		int r;

		k[0] = 0x20; /* shared prefix byte */
		k[1] = 0x00; /* embedded NUL in every key */
		if (i & 1) {
			k[2] = (uint8_t)i; /* odd: 3-byte key */
			items[i].kl = 3;
		} else {
			k[2] = 0x00; /* even: another NUL, 4-byte key */
			k[3] = (uint8_t)i;
			items[i].kl = 4;
		}
		items[i].v = (uint32_t)(i * 131u + 7u);
		r = btnode_insert(page, k, items[i].kl, &items[i].v,
		    sizeof(items[i].v));
		CHECK(r == 0, "binary insert i=%d", i);
	}
	CHECK(btnode_count(page) == NB, "binary count == %d got %u", NB,
	    btnode_count(page));

	for (i = 0; i < NB; i++) {
		int found = -1, slot;
		uint8_t full[16];
		uint16_t fl = 0;
		const void *ks, *vp;
		uint16_t kl, vl;
		uint32_t gv;

		slot = btnode_search(page, items[i].k, items[i].kl, &found);
		CHECK(found == 1, "binary found i=%d", i);

		CHECK(btnode_full_key(page, slot, full, sizeof(full), &fl) == 0,
		    "binary full_key i=%d", i);
		CHECK(fl == items[i].kl &&
		        memcmp(full, items[i].k, fl) == 0,
		    "binary full_key round-trip i=%d", i);

		CHECK(btnode_get(page, slot, &ks, &kl, &vp, &vl) == 0,
		    "binary get i=%d", i);
		CHECK(kl == items[i].kl - 1, "binary suffix len i=%d", i);
		CHECK(memcmp(ks, items[i].k + 1, kl) == 0,
		    "binary suffix bytes i=%d", i);
		memcpy(&gv, vp, sizeof(gv));
		CHECK(vl == sizeof(uint32_t) && gv == items[i].v,
		    "binary value i=%d", i);
	}

	/* Removing a key makes it unfindable; the rest survive. */
	{
		int found = -1, slot;

		slot = btnode_search(page, items[10].k, items[10].kl, &found);
		CHECK(found == 1 && btnode_remove(page, slot) == 0,
		    "remove existing");
		CHECK(btnode_count(page) == NB - 1, "count after remove");
		found = -1;
		(void)btnode_search(page, items[10].k, items[10].kl, &found);
		CHECK(found == 0, "removed key gone");
		found = -1;
		(void)btnode_search(page, items[11].k, items[11].kl, &found);
		CHECK(found == 1, "neighbor survives remove");
	}

	free(page);
	printf("  test_binary_keys: ok (%d keys with NULs)\n", NB);
}

/*
 * Exercise the remove + reinsert path hard enough to trigger internal
 * compaction (holes reclaimed when the contiguous gap is too small).
 */
static void
test_remove_compact(void)
{
	const uint32_t ps = 2048;
	uint8_t *page = malloc(ps);
	int n, total, kept, half, i, added;

	CHECK(page != NULL, "malloc");
	btnode_init(page, ps, 0); /* inner node, no fences -> prefix 0 */
	CHECK(btnode_is_leaf(page) == 0, "inner node");

	for (n = 0;; n++) {
		char key[24];
		char val[16];

		snprintf(key, sizeof(key), "rec:%06d", n);
		memset(val, (int)(n & 0xff), sizeof(val));
		if (btnode_insert(page, key, 10, val, sizeof(val)) < 0)
			break;
	}
	total = n;
	CHECK(total > 10, "filled rec keys got %d", total);
	CHECK(btnode_prefix_len(page) == 0, "no-fence prefix == 0");

	/* Drop the lower half (repeatedly remove slot 0 = smallest). */
	half = total / 2;
	for (i = 0; i < half; i++)
		CHECK(btnode_remove(page, 0) == 0, "remove slot 0");
	kept = total - half;
	CHECK(btnode_count(page) == kept, "count after bulk remove");

	/* Removed (small) keys gone, kept (large) keys still found. */
	for (i = 0; i < total; i++) {
		char key[24];
		int found = -1;

		snprintf(key, sizeof(key), "rec:%06d", i);
		(void)btnode_search(page, key, 10, &found);
		if (i < half)
			CHECK(found == 0, "removed rec %d gone", i);
		else
			CHECK(found == 1, "kept rec %d present", i);
	}

	/*
	 * Insert larger keys.  The freed cells are holes below
	 * data_offset, so once the contiguous gap is exhausted the next
	 * insert must compact rather than fail.
	 */
	added = 0;
	{
		uint16_t gap_before = btnode_free_space(page);

		for (n = 0;; n++) {
			char key[24];
			char val[16];

			snprintf(key, sizeof(key), "zzz:%06d", n);
			memset(val, 0x5a, sizeof(val));
			if (btnode_insert(page, key, 10, val, sizeof(val)) < 0)
				break;
			added++;
		}
		/*
		 * Each entry costs slot+suffix+value bytes.  If we added
		 * more total bytes than the contiguous gap held before we
		 * started, compaction must have reclaimed the holes.
		 */
		CHECK((uint32_t)added * 36u > (uint32_t)gap_before,
		    "compaction reclaimed holes: %d*36 > gap %u", added,
		    gap_before);
	}

	/* All kept "rec:" keys plus all new "zzz:" keys are searchable. */
	for (i = half; i < total; i++) {
		char key[24];
		int found = -1;

		snprintf(key, sizeof(key), "rec:%06d", i);
		(void)btnode_search(page, key, 10, &found);
		CHECK(found == 1, "rec %d survived compaction", i);
	}
	for (i = 0; i < added; i++) {
		char key[24];
		int found = -1;

		snprintf(key, sizeof(key), "zzz:%06d", i);
		(void)btnode_search(page, key, 10, &found);
		CHECK(found == 1, "zzz %d present", i);
	}
	CHECK(btnode_count(page) == kept + added, "final count");

	free(page);
	printf("  test_remove_compact: ok (fill %d, kept %d, re-added %d)\n",
	    total, kept, added);
}

/*
 * btnode_check (the in-debug validator): a well-formed leaf and internal
 * node pass -- including after split, merge, remove and compaction --
 * and each planted defect class is reported.  A validator that passes a
 * corrupt page is decoration, so every check here must FIRE.
 */
static void
corrupt_expect(const uint8_t *good, uint32_t ps, uint8_t *scratch,
    size_t off, const void *bytes, size_t n, const char *what)
{
	char why[200];

	memcpy(scratch, good, ps);
	memcpy(scratch + off, bytes, n);
	why[0] = 0;
	CHECK(btnode_check(scratch, why, sizeof why) != 0,
	    "btnode_check missed: %s", what);
	printf("    caught %-34s -> %s\n", what, why);
}

static void
test_check(void)
{
	const uint32_t ps = 4096;
	uint8_t *L = malloc(ps), *R = malloc(ps), *X = malloc(ps);
	char why[200];
	uint8_t sep[64], b8[8];
	uint16_t sep_len = 0, u16;
	uint32_t u32;
	size_t s0 = sizeof(struct btnode_hdr), sl = sizeof(struct btnode_slot);
	struct btnode_hdr hh;
	struct btnode_slot s1, s2;
	int i;
	uint8_t ref[12 + 8];
	uint8_t empty = 0;

	CHECK(L != NULL && R != NULL && X != NULL, "malloc");
	btnode_init(L, ps, 1);
	btnode_set_fences(L, "k0000", 5, "k9999", 5);
	CHECK(btnode_check(L, why, sizeof why) == 0, "empty leaf: %s", why);
	for (i = 1; i < 120; i++) {
		char key[16], val[24];
		snprintf(key, sizeof key, "k%04d", i * 7);
		memset(val, 'a' + i % 26, sizeof val);
		if (btnode_insert(L, key, 5, val, (uint16_t)(i % 20)) < 0)
			break;
	}
	CHECK(btnode_check(L, why, sizeof why) == 0, "filled leaf: %s", why);
	for (i = 0; i < 30; i++)
		CHECK(btnode_remove(L, i) == 0, "remove");
	CHECK(btnode_check(L, why, sizeof why) == 0, "holes: %s", why);
	btnode_init(R, ps, 1);
	CHECK(btnode_split(L, R, sep, &sep_len) == 0, "split");
	CHECK(btnode_check(L, why, sizeof why) == 0, "split left: %s", why);
	CHECK(btnode_check(R, why, sizeof why) == 0, "split right: %s", why);
	CHECK(btnode_merge(L, R) == 0, "merge");
	CHECK(btnode_check(L, why, sizeof why) == 0, "merged: %s", why);

	/* Planted defects on the good leaf L. */
	memcpy(&hh, L, sizeof hh);
	memcpy(&s1, L + s0, sl);
	memcpy(&s2, L + s0 + sl, sl);
	u16 = (uint16_t)(hh.space_used + 1);
	corrupt_expect(L, ps, X, offsetof(struct btnode_hdr, space_used),
	    &u16, 2, "space_used off by one");
	u16 = (uint16_t)(s0 + (size_t)hh.count * sl - 2);
	corrupt_expect(L, ps, X, offsetof(struct btnode_hdr, data_offset),
	    &u16, 2, "data_offset inside slot array");
	/* slot 0 := slot 1 (same cell, head consistent): overlap check */
	corrupt_expect(L, ps, X, s0, &s2, sl, "two slots share one cell");
	u16 = (uint16_t)(ps - 1);
	corrupt_expect(L, ps, X, s0 + offsetof(struct btnode_slot, offset),
	    &u16, 2, "cell runs past the page");
	memcpy(X, L, ps);
	/* swap slot 0 and 1: keys out of order */
	memcpy(X + s0, &s2, sl);
	memcpy(X + s0 + sl, &s1, sl);
	CHECK(btnode_check(X, why, sizeof why) != 0, "missed: order");
	printf("    caught %-34s -> %s\n", "keys not ascending", why);
	u16 = (uint16_t)(hh.prefix_len + 1);
	corrupt_expect(L, ps, X, offsetof(struct btnode_hdr, prefix_len),
	    &u16, 2, "prefix_len != fence prefix");
	u32 = s1.head ^ 1u;
	corrupt_expect(L, ps, X, s0 + offsetof(struct btnode_slot, head),
	    &u32, 4, "stale slot head");
	u16 = (uint16_t)(s1.val_len | BTNODE_VFLAG);
	corrupt_expect(L, ps, X, s0 + offsetof(struct btnode_slot, val_len),
	    &u16, 2, "VFLAG on a non-ref value");
	/* A key outside (lo, hi]: rewrite the lo fence above the keys. */
	/* lo fence k0000 -> k8000 (common prefix with k9999 still 1) */
	corrupt_expect(L, ps, X, (size_t)hh.lo_fence_off + 1, "8", 1,
	    "leaf key <= lo fence");

	/* A flagged, well-formed out-of-line ref passes; a bad one fails. */
	btnode_init(L, ps, 1);
	btnode_set_fences(L, NULL, 0, NULL, 0);
	memset(ref, 0, sizeof ref);
	ref[0] = 0x5F; ref[1] = 1;              /* BT_OVF_TAG, one page */
	ref[4] = 100;                           /* total_len 100 */
	ref[12] = 7;                            /* pid 7 */
	CHECK(btnode_insert_f(L, "big", 3, ref, sizeof ref, BTNODE_VFLAG) == 0,
	    "insert ref");
	CHECK(btnode_check(L, why, sizeof why) == 0, "good ref: %s", why);
	memcpy(&hh, L, sizeof hh);
	memcpy(&s1, L + s0, sl);
	corrupt_expect(L, ps, X, (size_t)s1.offset + 3, "\x5E", 1,
	    "ovf ref with a bad tag");
	corrupt_expect(L, ps, X, (size_t)s1.offset + 3 + 12, "\0", 1,
	    "ovf ref with page id 0");

	/* Internal node in the btree's shape (XBT3): real fences and their
	 * prefix, slot 0 the -infinity child with an empty stored suffix,
	 * 4-byte child ids. */
	btnode_init(L, ps, 0);
	btnode_set_fences(L, "ka", 2, "kz", 2);
	CHECK(btnode_prefix_len(L) == 1, "internal prefix %u",
	    btnode_prefix_len(L));
	u32 = 11;
	memcpy(b8, &u32, 4);
	CHECK(btnode_insert(L, &empty, 0, b8, 4) == 0, "slot 0");
	u32 = 12;
	memcpy(b8, &u32, 4);
	CHECK(btnode_insert(L, "km", 2, b8, 4) == 0, "sep");
	CHECK(btnode_insert(L, "m", 1, b8, 4) == -2, "a key outside the "
	    "prefix is refused");
	CHECK(btnode_check(L, why, sizeof why) == 0, "internal: %s", why);
	memcpy(&s1, L + s0, sl);
	memcpy(&s2, L + s0 + sl, sl);
	u16 = 8;
	corrupt_expect(L, ps, X, s0 + sl + offsetof(struct btnode_slot,
	    val_len), &u16, 2, "internal value not 4 bytes");
	u32 = 0;
	corrupt_expect(L, ps, X, (size_t)s2.offset + s2.key_len, &u32, 4,
	    "internal child pid 0");
	corrupt_expect(L, ps, X, (size_t)s2.offset, "a", 1,
	    "internal separator <= lo fence");
	/* An internal node whose slot 0 is a real key. */
	btnode_init(X, ps, 0);
	btnode_set_fences(X, NULL, 0, NULL, 0);
	u32 = 11;
	memcpy(b8, &u32, 4);
	CHECK(btnode_insert(X, "a", 1, b8, 4) == 0, "slot 0 keyed");
	CHECK(btnode_check(X, why, sizeof why) != 0, "missed: slot 0 key");
	printf("    caught %-34s -> %s\n", "internal slot 0 not empty", why);

	free(L);
	free(R);
	free(X);
	printf("  test_check: ok (valid nodes pass, every planted defect "
	    "caught)\n");
}


/*
 * The split guarantee behind btree.c's entry bound: with keys of at most
 * K = cap/8 - 9 bytes and entries (slot + key + value) of at most
 * M = (cap - 2K) / 3 bytes, a byte-balanced split of ANY full leaf finds a
 * cut that leaves room for ANY pending entry, and the pending entry then
 * fits on its side.  Random leaves: fences with a shared prefix of 0..60
 * bytes (or infinite), keys of random length that share it, values up to
 * the bound; filled until the pending entry no longer fits in place.
 * With m_over the bound is exceeded on purpose, to show the check bites.
 */
static uint64_t g_rs = 0x9E3779B97F4A7C15ull;
static uint32_t
rnd32(void)
{
	g_rs ^= g_rs << 13; g_rs ^= g_rs >> 7; g_rs ^= g_rs << 17;
	return (uint32_t)g_rs;
}

static int
split_trial(uint32_t ps, uint32_t K, uint32_t M, int quiet)
{
	uint8_t *L = malloc(ps), *R = malloc(ps), *keys = malloc((size_t)ps * 2);
	uint8_t lo[1100], hi[1100], k[1100], sep[1100], v[22000], pk[1100];  /* K <= 1024 */
	uint32_t pl = rnd32() % 61, cap = ps - (uint32_t)sizeof(struct btnode_hdr);
	uint16_t lol = 0, hil = 0, sl = 0, pkl, pvl;
	int inf_lo = rnd32() % 4 == 0, inf_hi = rnd32() % 4 == 0, n = 0, rc = 0, i;

	if (pl + 18 > K) pl = K - 18;
	memset(v, 'v', sizeof v);
	/* fences: [pl shared bytes][0x10 ...] and [pl shared bytes][0xF0 ...] */
	for (i = 0; i < (int)pl; i++) lo[i] = hi[i] = (uint8_t)(0x41 + rnd32() % 20);
	lo[pl] = 0x10; hi[pl] = 0xF0;
	lol = (uint16_t)(pl + 1 + rnd32() % 8); hil = (uint16_t)(pl + 1 + rnd32() % 8);
	for (i = (int)pl + 1; i < lol; i++) lo[i] = 0x00;
	for (i = (int)pl + 1; i < hil; i++) hi[i] = 0xFF;
	btnode_init(L, ps, 1);
	btnode_set_fences(L, inf_lo ? NULL : lo, inf_lo ? 0 : lol, inf_hi ? NULL : hi,
	    inf_hi ? 0 : hil);
	(void)cap;
	for (;;) {
		uint32_t kl = pl + 2 + rnd32() % (K - pl - 1), vl, emax;
		emax = M - 10 - kl;
		vl = (rnd32() % 3 == 0) ? rnd32() % (emax + 1) : rnd32() % (emax / 8 + 1);
		memcpy(k, lo, pl);
		k[pl] = (uint8_t)(0x20 + rnd32() % 0xC0);
		for (i = (int)pl + 1; i < (int)kl; i++) k[i] = (uint8_t)rnd32();
		if (btnode_insert(L, k, (uint16_t)kl, v, (uint16_t)vl) != 0) {
			/* this entry did not fit: it is the pending one */
			memcpy(pk, k, kl); pkl = (uint16_t)kl; pvl = (uint16_t)vl;
			break;
		}
		n++;
	}
	/* a pending entry of the maximum size half the time */
	if (rnd32() % 2) pvl = (uint16_t)(M - 10 - pkl);
	if (n >= 2) {
		btnode_init(R, ps, 1);
		if (btnode_split_ex(L, R, sep, sizeof sep, &sl, pk, pkl, pvl) != 0) {
			rc = 1;
		} else {
			int c = memcmp(pk, sep, pkl < sl ? pkl : sl);
			void *dst = (c < 0 || (c == 0 && pkl <= sl)) ? L : R;
			char why[200];
			if (btnode_insert(dst, pk, pkl, v, pvl) != 0) rc = 2;
			else if (btnode_check(L, why, sizeof why) != 0 ||
			    btnode_check(R, why, sizeof why) != 0) rc = 3;
		}
		if (rc && !quiet)
			fprintf(stderr, "    split_trial ps=%u K=%u M=%u pl=%u n=%d pend %u+%u "
			    "-> %s\n", ps, K, M, pl, n, pkl, pvl,
			    rc == 1 ? "no cut" : rc == 2 ? "pending did not fit" : "bad node");
	}
	free(L); free(R); free(keys);
	return rc;
}

static void
test_split_bound(void)
{
	static const uint32_t pss[] = { 1024, 4096, 16384 };
	int p, t, bad_ok = 0, bad_over = 0;

	for (p = 0; p < 3; p++) {
		uint32_t ps = pss[p], cap = ps - (uint32_t)sizeof(struct btnode_hdr);
		uint32_t K = cap / 8 - 9, M;
		if (K > 1024) K = 1024;
		M = (cap - 2 * K) / 3;
		for (t = 0; t < 4000; t++)
			bad_ok += split_trial(ps, K, M, 0) != 0;
		/* Over the bound (an entry up to cap/2), failures must appear,
		 * or this test proves nothing. */
		for (t = 0; t < 4000; t++)
			bad_over += split_trial(ps, K, cap / 2, 1) != 0;
	}
	CHECK(bad_ok == 0, "split failed within the bound in %d trials", bad_ok);
	CHECK(bad_over > 0, "no failure above the bound: the trial is not "
	    "exercising the split");
	printf("  test_split_bound: ok (12000 random full leaves split for a "
	    "pending entry up to M = (cap - 2K)/3; %d failures above it)\n", bad_over);
}

/*
 * No page image carries memory it was not meant to.  Every node rebuild
 * (compaction, split, merge) builds into a stack buffer, and btnode_init
 * zeroed only the header, so the gap between the slot array and the cell
 * heap was whatever the stack held -- written to the page file and the
 * log with the page.  Build the same split, merge and compaction twice,
 * with the stack below pre-filled with two different patterns: the pages
 * must be byte-identical, and every byte that belongs to nothing must be
 * zero, also after removes (a removed cell's bytes are cleared).
 */
static void __attribute__((noinline))
fill_stack(uint8_t v)
{
	volatile uint8_t junk[96 * 1024];
	size_t i;

	for (i = 0; i < sizeof junk; i++)
		junk[i] = v;
}

static void __attribute__((noinline))
build_pages(uint8_t *L, uint8_t *R, uint32_t ps)
{
	uint8_t sep[64];
	uint16_t sl = 0;
	int i;

	btnode_init(L, ps, 1);
	btnode_set_fences(L, "k0000", 5, "k9999", 5);
	for (i = 1; i < 300; i++) {
		char key[16], val[24];
		snprintf(key, sizeof key, "k%04d", i * 7 % 9000 + 1);
		memset(val, 'a' + i % 26, sizeof val);
		if (btnode_insert(L, key, 5, val, (uint16_t)(i % 20)) < 0)
			break;
	}
	for (i = 0; i < 40; i++)
		(void)btnode_remove(L, (i * 3) % (int)btnode_count(L));
	btnode_init(R, ps, 1);
	(void)btnode_split(L, R, sep, &sl);
	(void)btnode_merge(L, R);
	(void)btnode_split(L, R, sep, &sl);
}

static void
test_page_bytes(void)
{
	const uint32_t ps = 4096;
	uint8_t *L1 = malloc(ps), *R1 = malloc(ps), *L2 = malloc(ps),
	    *R2 = malloc(ps);
	long d;

	CHECK(L1 != NULL && R1 != NULL && L2 != NULL && R2 != NULL, "malloc");
	memset(L1, 0x5A, ps); memset(R1, 0x5A, ps);
	memset(L2, 0xC3, ps); memset(R2, 0xC3, ps);
	fill_stack(0x11);
	build_pages(L1, R1, ps);
	fill_stack(0xEE);
	build_pages(L2, R2, ps);
	CHECK(memcmp(L1, L2, ps) == 0 && memcmp(R1, R2, ps) == 0,
	    "the same split/merge/compaction built from differently filled "
	    "memory gives different pages");
	d = t_dead_bytes(L1) + t_dead_bytes(R1);
	CHECK(d == 0, "%ld bytes outside the live header/slots/cells are not "
	    "zero", d);
	printf("  test_page_bytes: ok (rebuilt pages identical from different "
	    "stack contents; 0 unowned non-zero bytes)\n");
	free(L1); free(R1); free(L2); free(R2);
}

int
main(void)
{
	printf("btnode unit tests\n");
	test_page_bytes();
	test_prefix_leaf();
	test_fill_and_split();
	test_binary_keys();
	test_remove_compact();
	test_check();
	test_split_bound();
	printf("ALL PASS (%d checks)\n", g_checks);
	return 0;
}
