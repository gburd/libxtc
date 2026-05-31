/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_btree.c
 *	Standalone stress + correctness test for the B+-tree (btree.c)
 *	layered on the buffer manager (bufmgr.c) and the slotted node
 *	(btnode.c).  Plain asserts + printf; exits nonzero on the first
 *	failure.  Runs entirely off-loop -- the buffer manager performs
 *	synchronous page I/O when not on a loop -- so demand eviction
 *	exercises the cool / flush / evict / reload cycle as the tree
 *	outgrows the resident pool.
 *
 *	Build:
 *	  cd examples/06_sqlxtc
 *	  nix-shell -p openssl pkg-config liburing --command '\
 *	    gcc -std=c11 -Wall -Wextra -O1 -g -fno-omit-frame-pointer \
 *	      -D_GNU_SOURCE -I../../src/inc -I. -fsanitize=address \
 *	      -o /tmp/test_btree test_btree.c btree.c bufmgr.c btnode.c \
 *	      ../../build_unix/libxtc.a -pthread -ldl -lm -luring && \
 *	    ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 /tmp/test_btree'
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "btree.h"
#include "bufmgr.h"
#include "xtc.h"

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

/* ---- helpers ---------------------------------------------------------- */

static void
make_key(char *buf, size_t cap, int i)
{
	(void)snprintf(buf, cap, "k%08d", i);     /* 9 chars */
}

static void
make_val(char *buf, size_t cap, int i)
{
	(void)snprintf(buf, cap, "v%08d-payload", i); /* 17 chars */
}

/* Parse the integer index out of a (not necessarily NUL-terminated)
 * "k%08d" key. */
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

/* Lexicographic compare of two byte strings as unsigned bytes. */
static int
key_cmp_test(const void *a, uint16_t al, const void *b, uint16_t bl)
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

static void
shuffle(int *a, int n, unsigned seed)
{
	int i;

	srand(seed);
	for (i = n - 1; i > 0; i--) {
		int j = rand() % (i + 1);
		int t = a[i];

		a[i] = a[j];
		a[j] = t;
	}
}

static bm_t *
make_bm(char *path_out, uint32_t page_size, uint32_t n_frames)
{
	bm_opts_t bo = BM_OPTS_DEFAULT;
	bm_t *bm = NULL;
	int fd;

	strcpy(path_out, "/tmp/sqlxtc-btree-XXXXXX");
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

/* ---- focused routing / child-selection check ------------------------- */

/*
 * A small tree with a tiny page so splits (and a multi-level shape)
 * happen after very few inserts.  This directly exercises the
 * separator + child-selection logic: every key inserted must be found
 * via descent, the keys just below / at / above each split boundary
 * must route to the right leaf, absent keys must miss, and a full
 * cursor scan must return everything in ascending order.
 */
static void
test_routing(void)
{
	char path[64];
	bm_t *bm = make_bm(path, 512, 8);
	bt_t *bt = NULL;
	bt_stats_t st;
	const int N = 240;
	int i;
	int count;
	bt_cursor_t *c = NULL;
	const void *ck, *cv;
	uint16_t ckl, cvl;
	int prev;

	CHECK(bt_open(bm, &bt) == XTC_OK, "bt_open small");

	for (i = 0; i < N; i++) {
		char k[16], v[32];

		make_key(k, sizeof k, i);
		make_val(v, sizeof v, i);
		CHECK(bt_insert(bt, k, 9, v, (uint16_t)strlen(v)) == XTC_OK,
		    "insert routing %d", i);
	}

	bt_get_stats(bt, &st);
	CHECK(st.height > 1, "routing tree multi-level (height=%llu)",
	    (unsigned long long)st.height);
	CHECK(st.splits > 0, "routing tree split (splits=%llu)",
	    (unsigned long long)st.splits);

	/* Every key resolves to the correct value through the inner nodes. */
	for (i = 0; i < N; i++) {
		char k[16], v[32], buf[64];
		uint16_t vl = 0;

		make_key(k, sizeof k, i);
		make_val(v, sizeof v, i);
		CHECK(bt_lookup(bt, k, 9, buf, sizeof buf, &vl) == XTC_OK,
		    "routing lookup %d", i);
		CHECK(vl == strlen(v) && memcmp(buf, v, vl) == 0,
		    "routing value %d", i);
	}

	/* Keys that were never inserted miss. */
	for (i = 0; i < 50; i++) {
		char k[16];
		uint16_t vl = 0;

		make_key(k, sizeof k, N + 1000 + i);
		CHECK(bt_lookup(bt, k, 9, NULL, 0, &vl) == XTC_E_NOTFOUND,
		    "routing absent %d", i);
	}

	/* Full ascending scan returns exactly N keys in order. */
	CHECK(bt_cursor_open(bt, NULL, 0, &c) == XTC_OK, "routing cursor open");
	count = 0;
	prev = -1;
	while (bt_cursor_next(c, &ck, &ckl, &cv, &cvl) == XTC_OK) {
		int idx = key_index(ck, ckl);
		char v[32];

		CHECK(idx == count, "routing scan order: got idx %d at pos %d",
		    idx, count);
		CHECK(idx > prev, "routing scan strictly ascending");
		make_val(v, sizeof v, idx);
		CHECK(cvl == strlen(v) && memcmp(cv, v, cvl) == 0,
		    "routing scan value at %d", idx);
		prev = idx;
		count++;
	}
	CHECK(count == N, "routing scan count %d == %d", count, N);
	bt_cursor_close(c);

	/*
	 * Upsert: re-inserting an existing key replaces its value (and may
	 * change its length) without adding a slot.  Probe a few keys
	 * around split boundaries.
	 */
	{
		static const int probe[] = { 0, 1, 15, 16, 17, 119, 120, 239 };
		size_t pi;
		int rescan;

		for (pi = 0; pi < sizeof probe / sizeof probe[0]; pi++) {
			char k[16], nv[40], buf[64];
			uint16_t vl = 0;

			make_key(k, sizeof k, probe[pi]);
			(void)snprintf(nv, sizeof nv, "REPLACED-%08d-x",
			    probe[pi]);
			CHECK(bt_insert(bt, k, 9, nv,
			    (uint16_t)strlen(nv)) == XTC_OK, "upsert %d",
			    probe[pi]);
			CHECK(bt_lookup(bt, k, 9, buf, sizeof buf, &vl) ==
			    XTC_OK, "upsert lookup %d", probe[pi]);
			CHECK(vl == strlen(nv) && memcmp(buf, nv, vl) == 0,
			    "upsert replaced value %d", probe[pi]);
		}
		/* No keys added or lost. */
		CHECK(bt_cursor_open(bt, NULL, 0, &c) == XTC_OK,
		    "upsert rescan open");
		rescan = 0;
		while (bt_cursor_next(c, &ck, &ckl, &cv, &cvl) == XTC_OK)
			rescan++;
		CHECK(rescan == N, "upsert preserved count %d == %d", rescan,
		    N);
		bt_cursor_close(c);
	}

	bt_close(bt);
	bm_destroy(bm);
	(void)unlink(path);
	printf("  test_routing: ok (%d keys, height=%llu, splits=%llu)\n", N,
	    (unsigned long long)st.height, (unsigned long long)st.splits);
}

/* ---- big stress test: tree far larger than the resident pool --------- */

static void
test_big(void)
{
	char path[64];
	const uint32_t PAGE_SZ = 4096;
	const uint32_t N_FRAMES = 24;
	const int N = 5000;
	bm_t *bm = make_bm(path, PAGE_SZ, N_FRAMES);
	bt_t *bt = NULL;
	int *order;
	int i;
	bt_stats_t bst;
	bm_stats_t mst;
	bt_cursor_t *c = NULL;
	const void *ck, *cv;
	uint16_t ckl, cvl;
	int count, prev, mid;

	CHECK(bt_open(bm, &bt) == XTC_OK, "bt_open big");

	order = malloc(sizeof(int) * (size_t)N);
	CHECK(order != NULL, "malloc order");
	for (i = 0; i < N; i++)
		order[i] = i;

	/* (2) Insert N keys in shuffled order -- far exceeds the pool. */
	shuffle(order, N, 12345u);
	for (i = 0; i < N; i++) {
		char k[16], v[32];

		make_key(k, sizeof k, order[i]);
		make_val(v, sizeof v, order[i]);
		CHECK(bt_insert(bt, k, 9, v, (uint16_t)strlen(v)) == XTC_OK,
		    "big insert %d", order[i]);
	}

	bt_get_stats(bt, &bst);
	CHECK(bst.height > 1, "big tree multi-level (height=%llu)",
	    (unsigned long long)bst.height);
	CHECK(bst.inserts == (uint64_t)N, "big inserts == %d (got %llu)", N,
	    (unsigned long long)bst.inserts);

	/* (3) Look up all keys in a different shuffled order. */
	shuffle(order, N, 67890u);
	for (i = 0; i < N; i++) {
		char k[16], v[32], buf[64];
		uint16_t vl = 0;

		make_key(k, sizeof k, order[i]);
		make_val(v, sizeof v, order[i]);
		CHECK(bt_lookup(bt, k, 9, buf, sizeof buf, &vl) == XTC_OK,
		    "big lookup %d", order[i]);
		CHECK(vl == strlen(v) && memcmp(buf, v, vl) == 0,
		    "big value %d", order[i]);
	}

	/* (4) Look up 200 absent keys. */
	for (i = 0; i < 200; i++) {
		char k[16];
		uint16_t vl = 0;

		make_key(k, sizeof k, N + 100000 + i);
		CHECK(bt_lookup(bt, k, 9, NULL, 0, &vl) == XTC_E_NOTFOUND,
		    "big absent %d", i);
	}

	/* (5a) Full ascending scan: exactly N keys, in order, right values. */
	CHECK(bt_cursor_open(bt, NULL, 0, &c) == XTC_OK, "big cursor open");
	count = 0;
	prev = -1;
	while (bt_cursor_next(c, &ck, &ckl, &cv, &cvl) == XTC_OK) {
		int idx = key_index(ck, ckl);
		char v[32];

		CHECK(idx == count, "big scan order: idx %d at pos %d", idx,
		    count);
		CHECK(idx > prev, "big scan strictly ascending at %d", idx);
		make_val(v, sizeof v, idx);
		CHECK(cvl == strlen(v) && memcmp(cv, v, cvl) == 0,
		    "big scan value at %d", idx);
		prev = idx;
		count++;
	}
	CHECK(count == N, "big scan count %d == %d", count, N);
	bt_cursor_close(c);

	/* (5b) Cursor from a midpoint key yields the correct suffix. */
	mid = N / 2;
	{
		char k[16];

		make_key(k, sizeof k, mid);
		CHECK(bt_cursor_open(bt, k, 9, &c) == XTC_OK,
		    "big mid cursor open");
	}
	count = 0;
	prev = mid - 1;
	while (bt_cursor_next(c, &ck, &ckl, &cv, &cvl) == XTC_OK) {
		int idx = key_index(ck, ckl);
		char v[32];

		CHECK(idx == mid + count, "mid scan order: idx %d expected %d",
		    idx, mid + count);
		CHECK(idx > prev, "mid scan ascending");
		make_val(v, sizeof v, idx);
		CHECK(cvl == strlen(v) && memcmp(cv, v, cvl) == 0,
		    "mid scan value at %d", idx);
		prev = idx;
		count++;
	}
	CHECK(count == N - mid, "mid scan count %d == %d", count, N - mid);
	bt_cursor_close(c);

	/* (6) Stats: prove the tree exceeded memory and the cycle ran. */
	bt_get_stats(bt, &bst);
	bm_get_stats(bm, &mst);
	CHECK(mst.evicted > 0, "evictions happened (evicted=%llu)",
	    (unsigned long long)mst.evicted);
	CHECK(mst.loads > 0, "reloads happened (loads=%llu)",
	    (unsigned long long)mst.loads);
	CHECK(mst.resident <= N_FRAMES, "resident %llu <= pool %u",
	    (unsigned long long)mst.resident, N_FRAMES);

	printf("  test_big: ok (%d keys through a %u-frame, %u-byte pool)\n",
	    N, N_FRAMES, PAGE_SZ);
	printf("    bt_stats : inserts=%llu lookups=%llu splits=%llu "
	    "height=%llu\n",
	    (unsigned long long)bst.inserts, (unsigned long long)bst.lookups,
	    (unsigned long long)bst.splits, (unsigned long long)bst.height);
	printf("    bm_stats : hits=%llu rescues=%llu loads=%llu cooled=%llu "
	    "flushed=%llu evicted=%llu resident=%llu free=%llu\n",
	    (unsigned long long)mst.hits, (unsigned long long)mst.rescues,
	    (unsigned long long)mst.loads, (unsigned long long)mst.cooled,
	    (unsigned long long)mst.flushed, (unsigned long long)mst.evicted,
	    (unsigned long long)mst.resident,
	    (unsigned long long)mst.free_frames);

	free(order);
	bt_close(bt);
	bm_destroy(bm);
	(void)unlink(path);
}

/* ---- binary keys with embedded NUL bytes ----------------------------- */

static void
test_binary(void)
{
	char path[64];
	bm_t *bm = make_bm(path, 1024, 8);
	bt_t *bt = NULL;
	const int NB = 300;
	int i;
	bt_cursor_t *c = NULL;
	const void *ck, *cv;
	uint16_t ckl, cvl;
	int count;
	uint8_t prevk[8];
	uint16_t prevkl = 0;

	CHECK(bt_open(bm, &bt) == XTC_OK, "bt_open binary");

	/* Every key has an embedded NUL; lengths vary (4 or 5 bytes). */
	for (i = 0; i < NB; i++) {
		uint8_t k[5];
		uint32_t v = (uint32_t)(i * 2654435761u);
		uint16_t kl;

		k[0] = 0x10;
		k[1] = 0x00;                 /* embedded NUL */
		k[2] = (uint8_t)(i >> 8);
		k[3] = (uint8_t)(i & 0xff);
		if (i & 1) {
			k[4] = 0xAB;
			kl = 5;
		} else {
			kl = 4;
		}
		CHECK(bt_insert(bt, k, kl, &v, sizeof v) == XTC_OK,
		    "binary insert %d", i);
	}

	/* Look every key back up. */
	for (i = 0; i < NB; i++) {
		uint8_t k[5];
		uint32_t v = (uint32_t)(i * 2654435761u), got = 0;
		uint16_t kl, vl = 0;

		k[0] = 0x10;
		k[1] = 0x00;
		k[2] = (uint8_t)(i >> 8);
		k[3] = (uint8_t)(i & 0xff);
		if (i & 1) {
			k[4] = 0xAB;
			kl = 5;
		} else {
			kl = 4;
		}
		CHECK(bt_lookup(bt, k, kl, &got, sizeof got, &vl) == XTC_OK,
		    "binary lookup %d", i);
		CHECK(vl == sizeof v && got == v, "binary value %d", i);
	}

	/* Delete the even-indexed keys; odd ones survive. */
	for (i = 0; i < NB; i += 2) {
		uint8_t k[4];

		k[0] = 0x10;
		k[1] = 0x00;
		k[2] = (uint8_t)(i >> 8);
		k[3] = (uint8_t)(i & 0xff);
		CHECK(bt_delete(bt, k, 4) == XTC_OK, "binary delete %d", i);
	}
	for (i = 0; i < NB; i += 2) {
		uint8_t k[4];
		uint16_t vl = 0;

		k[0] = 0x10;
		k[1] = 0x00;
		k[2] = (uint8_t)(i >> 8);
		k[3] = (uint8_t)(i & 0xff);
		CHECK(bt_delete(bt, k, 4) == XTC_E_NOTFOUND,
		    "binary re-delete %d", i);
		CHECK(bt_lookup(bt, k, 4, NULL, 0, &vl) == XTC_E_NOTFOUND,
		    "binary deleted gone %d", i);
	}

	/* Surviving (odd) keys are still present and ordered under scan. */
	CHECK(bt_cursor_open(bt, NULL, 0, &c) == XTC_OK, "binary cursor open");
	count = 0;
	while (bt_cursor_next(c, &ck, &ckl, &cv, &cvl) == XTC_OK) {
		if (count > 0)
			CHECK(key_cmp_test(prevk, prevkl, ck, ckl) < 0,
			    "binary scan ascending at %d", count);
		CHECK(ckl == 5, "binary surviving key len at %d", count);
		memcpy(prevk, ck, ckl);
		prevkl = ckl;
		count++;
	}
	CHECK(count == NB / 2, "binary surviving count %d == %d", count,
	    NB / 2);
	bt_cursor_close(c);

	bt_close(bt);
	bm_destroy(bm);
	(void)unlink(path);
	printf("  test_binary: ok (%d NUL-bearing keys, %d deleted)\n", NB,
	    NB / 2);
}

int
main(void)
{
	printf("btree tests\n");
	test_routing();
	test_big();
	test_binary();
	printf("ALL PASS (%d checks)\n", g_checks);
	return 0;
}
