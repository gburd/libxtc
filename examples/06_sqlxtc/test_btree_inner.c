/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_btree_inner.c
 *	The internal-node format (2.0, XBT3): truncated separators, internal
 *	nodes with real fences and prefix compression, 4-byte child ids.
 *
 *	Up to 1.60 a separator was the left half's whole largest key (a
 *	20-byte version key), internal nodes kept a -infinity lower fence
 *	and so prefix 0, and a child id took 8 bytes: an internal entry was
 *	38 bytes, a 4 KiB internal node held ~75 children, and every slot
 *	head of a table's region was the same table id, so every binary
 *	search step fell through to a memcmp.  A two-table store of 400k
 *	version keys inserted in random order is built and walked:
 *	  1. every key reads back and a full scan returns them in order;
 *	  2. every node passes btnode_check (BT_CHECK is on for every
 *	     mutation), and every child's fences are exactly the range its
 *	     parent routes to it -- lower fence included, for internal
 *	     nodes too (1.60's internal lower fences were -infinity);
 *	  3. the separators above the leaves average < 16 bytes, <= 6 stored
 *	     after the node prefix (1.60: 20 and 20);
 *	  4. internal nodes below the root hold >= 120 children on average
 *	     and carry a prefix (1.60: ~75, prefix 0);
 *	  5. the tree is 3 levels (1.60: 4).
 */
#define BT_CHECK 1
#include "bufmgr.c"
#include "btree.c"
#include "t_tmp.h"

#include <unistd.h>

#define PS     4096
#define NROWS  200000          /* per table */
#define VLEN   40

static int g_fail;
#define TCK(c, ...) do { if (!(c)) { \
	fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
	fprintf(stderr, __VA_ARGS__); fprintf(stderr, "\n"); \
	g_fail = 1; } } while (0)

struct lv {
	uint64_t nodes, children, sep_full, sep_sfx, seps, prefix;
};
static struct lv g_lv[BT_MAX_HEIGHT];
static int g_height, g_bad_fence;

/* A version key: (table id, rowid, inverted commit stamp), big-endian,
 * the shape xstore.c's enc_vkey writes. */
static void
vkey(uint8_t k[20], uint32_t tid, uint64_t rid, uint64_t ts)
{
	int i;

	rid ^= 0x8000000000000000ull;
	ts = ~ts;
	for (i = 3; i >= 0; i--, tid >>= 8) k[i] = (uint8_t)tid;
	for (i = 11; i >= 4; i--, rid >>= 8) k[i] = (uint8_t)rid;
	for (i = 19; i >= 12; i--, ts >>= 8) k[i] = (uint8_t)ts;
}

static int
fence_is(const void *pg, int hi, const uint8_t *want, uint16_t wl)
{
	uint8_t f[BT_MAX_KEY];
	uint16_t fl = 0;

	if ((hi ? btnode_hi_fence(pg, f, sizeof f, &fl) :
	    btnode_lo_fence(pg, f, sizeof f, &fl)) != 0)
		return 0;
	return fl == wl && (wl == 0 || memcmp(f, want, wl) == 0);
}

/* Walk the subtree at pid, which its parent routes (lo, hi] to. */
static void
walk(bt_t *bt, bm_pid_t pid, const uint8_t *lo, uint16_t lol,
    const uint8_t *hi, uint16_t hil, int level)
{
	bm_frame_t *f;
	void *pg;
	int i, n;
	char why[200];

	if (level >= BT_MAX_HEIGHT || bm_fix_pid(bt->bm, pid, &f) != XTC_OK) {
		g_bad_fence++;
		return;
	}
	pg = bm_page(f);
	if (level + 1 > g_height)
		g_height = level + 1;
	if (btnode_check(pg, why, sizeof why) != 0) {
		fprintf(stderr, "  pid %llu: %s\n", (unsigned long long)pid, why);
		g_bad_fence++;
	}
	if (!fence_is(pg, 0, lo, lol) || !fence_is(pg, 1, hi, hil))
		g_bad_fence++;
	n = (int)btnode_count(pg);
	if (!btnode_is_leaf(pg)) {
		g_lv[level].nodes++;
		g_lv[level].children += (uint64_t)n;
		g_lv[level].prefix += btnode_prefix_len(pg);
		for (i = 0; i < n; i++) {
			uint8_t sl[BT_MAX_KEY], sh[BT_MAX_KEY];
			uint16_t sll = lol, shl = hil, sfx = 0;
			const uint8_t *clo = lo, *chi = hi;

			if (i > 0) {
				(void)btnode_full_key(pg, i, sl, sizeof sl, &sll);
				(void)btnode_get(pg, i, NULL, &sfx, NULL, NULL);
				clo = sl;
				g_lv[level].sep_full += sll;
				g_lv[level].sep_sfx += sfx;
				g_lv[level].seps++;
			}
			if (i + 1 < n) {
				(void)btnode_full_key(pg, i + 1, sh, sizeof sh, &shl);
				chi = sh;
			}
			walk(bt, child_pid_at(pg, i), clo, sll, chi, shl, level + 1);
		}
	}
	bm_unfix(bt->bm, f, 0);
}

int
main(void)
{
	bm_opts_t bo = BM_OPTS_DEFAULT;
	bm_t *bm = NULL;
	bt_t *bt = NULL;
	char path[256];
	uint8_t k[20], v[VLEN], out[VLEN];
	uint32_t *perm;
	uint16_t vl, kl, pl = 0;
	bt_cursor_t *c = NULL;
	const void *ck, *cv;
	uint8_t prev[20];
	uint64_t n = 2ull * NROWS, i, found = 0, scanned = 0, misorder = 0;
	uint64_t inner = 0, ich = 0, ipre = 0, lsep = 0, lsfx = 0, nsep = 0;
	int fd, lvl;

	t_tmpl(path, sizeof path, "sqlxtc-inner");
	if ((fd = mkstemp(path)) < 0)
		return 1;
	close(fd);
	bo.path = path;
	bo.page_size = PS;
	bo.n_frames = 32768;        /* the whole tree stays resident */
	if (bm_create(&bo, &bm) != XTC_OK || bt_open(bm, &bt) != XTC_OK)
		return 1;
	if ((perm = malloc(sizeof *perm * n)) == NULL)
		return 1;
	for (i = 0; i < n; i++)
		perm[i] = (uint32_t)i;
	srand(7);
	for (i = n - 1; i > 0; i--) {
		uint64_t j = ((uint64_t)rand() * ((uint64_t)RAND_MAX + 1) +
		    (uint64_t)rand()) % (i + 1);
		uint32_t t = perm[i];
		perm[i] = perm[j];
		perm[j] = t;
	}
	memset(v, 'v', sizeof v);
	for (i = 0; i < n; i++) {
		uint32_t r = perm[i];

		vkey(k, 5 + r % 2, r / 2, 1000 + r);
		TCK(bt_insert(bt, k, 20, v, VLEN) == XTC_OK, "insert %llu",
		    (unsigned long long)i);
	}
	/* 1. every key reads back; a full scan is complete and ascending. */
	for (i = 0; i < n; i++) {
		vkey(k, 5 + (uint32_t)i % 2, i / 2, 1000 + i);
		if (bt_lookup(bt, k, 20, out, sizeof out, &vl) == XTC_OK &&
		    vl == VLEN)
			found++;
	}
	TCK(found == n, "1. %llu of %llu keys read back",
	    (unsigned long long)found, (unsigned long long)n);
	TCK(bt_cursor_open(bt, NULL, 0, &c) == XTC_OK, "cursor");
	while (c != NULL && bt_cursor_next(c, &ck, &kl, &cv, &vl) == XTC_OK) {
		if (kl != 20 || (pl != 0 && memcmp(prev, ck, 20) >= 0))
			misorder++;
		memcpy(prev, ck, 20);
		pl = kl;
		scanned++;
	}
	bt_cursor_close(c);
	TCK(scanned == n && misorder == 0, "1. scan %llu of %llu keys, %llu "
	    "out of order", (unsigned long long)scanned, (unsigned long long)n,
	    (unsigned long long)misorder);

	/* 2-5. the shape. */
	walk(bt, atomic_load(&bt->root_pid), NULL, 0, NULL, 0, 0);
	TCK(g_bad_fence == 0, "2. %d nodes malformed or with fences that are "
	    "not their parent's routing range", g_bad_fence);
	for (lvl = 0; lvl + 1 < g_height; lvl++) {
		struct lv *l = &g_lv[lvl];

		printf("  level %d: %llu internal nodes, %.1f children each, "
		    "prefix %.1f B, separators %.1f B (%.1f B stored)\n", lvl,
		    (unsigned long long)l->nodes, l->nodes ? (double)l->children /
		    (double)l->nodes : 0, l->nodes ? (double)l->prefix /
		    (double)l->nodes : 0, l->seps ? (double)l->sep_full /
		    (double)l->seps : 0, l->seps ? (double)l->sep_sfx /
		    (double)l->seps : 0);
		if (lvl > 0) {
			inner += l->nodes;
			ich += l->children;
			ipre += l->prefix;
		}
	}
	if (g_height >= 2) {
		lsep = g_lv[g_height - 2].sep_full;
		lsfx = g_lv[g_height - 2].sep_sfx;
		nsep = g_lv[g_height - 2].seps;
	}
	printf("  %llu keys, height %d\n", (unsigned long long)n, g_height);
	TCK(nsep > 0 && lsep < 16 * nsep && lsfx <= 6 * nsep, "3. separators "
	    "above the leaves average %.1f B, %.1f B stored (want < 16 and "
	    "<= 6; a whole key is 20)", nsep ? (double)lsep / (double)nsep : 0,
	    nsep ? (double)lsfx / (double)nsep : 0);
	TCK(inner > 0 && ich >= 120 * inner && ipre > 0, "4. internal nodes "
	    "below the root: %.1f children each (want >= 120), prefix %.1f B "
	    "(want > 0)", inner ? (double)ich / (double)inner : 0,
	    inner ? (double)ipre / (double)inner : 0);
	TCK(g_height == 3, "5. height %d (want 3)", g_height);
	free(perm);
	bt_close(bt);
	bm_destroy(bm);
	unlink(path);
	printf("test_btree_inner: %s\n", g_fail ? "FAILED" : "ok");
	return g_fail;
}
