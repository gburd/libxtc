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

int
main(void)
{
	printf("btnode unit tests\n");
	test_prefix_leaf();
	test_fill_and_split();
	test_binary_keys();
	test_remove_compact();
	printf("ALL PASS (%d checks)\n", g_checks);
	return 0;
}
