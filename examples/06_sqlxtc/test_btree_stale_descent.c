/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_btree_stale_descent.c
 *	A descent that keeps finding stale nodes must still hand back a
 *	leaf it holds (fixed + latched).  descend_shared and cursor_descend
 *	gave up after BT_DELETE_RETRIES stale hits and returned the last
 *	frame, which the stale branch had already unlatched and unfixed, so
 *	the caller released it a second time (pin 0 -> -1).  The test-only
 *	BT_STALE_TEST_HOOK forces the root-level stale check to fire; the
 *	implementation is included so the static descents and the frame pin
 *	counts are reachable.
 */
static int g_force_stale;
#define BT_STALE_TEST_HOOK() (g_force_stale > 0 ? (g_force_stale--, 1) : 0)
#include "bufmgr.c"
#include "btree.c"
#include "t_tmp.h"

#define N_KEYS 2000        /* several leaves: the root is internal */

static int g_fail;
#define TCK(c) do { if (!(c)) { \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
	g_fail = 1; } } while (0)

static void
mkkey(int i, char *buf, uint16_t *len)
{
	*len = (uint16_t)snprintf(buf, 16, "k-%06d", i);
}

/* No frame may be pinned (or underflowed) once every descent released. */
static int
pins_balanced(bm_t *bm)
{
	uint32_t i;
	for (i = 0; i < bm->n_frames; i++) {
		int p = atomic_load(&bm->frames[i].pin);
		int st = atomic_load(&bm->frames[i].state);
		if (st != BM_FREE && p != 0) {
			fprintf(stderr, "  frame %u pid %llu pin %d\n", i,
			    (unsigned long long)bm->frames[i].pid, p);
			return 0;
		}
	}
	return 1;
}

/* The frame a descent returned is ours: pinned once, latched, a leaf
 * that owns `key`. */
static void
check_held(bt_t *bt, bm_frame_t *f, const char *key, uint16_t kl)
{
	void *pg = bm_page(f);
	int found = 0;
	TCK(atomic_load(&f->pin) == 1);
	TCK(btnode_is_leaf(pg));
	TCK(!btnode_is_dead(pg));
	(void)btnode_search(pg, key, kl, &found);
	TCK(found);
	bm_unlatch(f);
	bm_unfix(bt->bm, f, 0);
}

int
main(void)
{
	char path[256];
	bm_opts_t bo = BM_OPTS_DEFAULT;
	bm_t *bm = NULL;
	bt_t *bt = NULL;
	bm_frame_t *f = NULL;
	char k[16], v[16];
	uint16_t kl, vl;
	int fd, i, n;

	t_tmpl(path, sizeof path, "sqlxtc-stale");
	fd = mkstemp(path);
	if (fd < 0) return 1;
	close(fd);
	bo.path = path; bo.page_size = 4096; bo.n_frames = 256; bo.cool_pct = 25;
	TCK(bm_create(&bo, &bm) == XTC_OK);
	TCK(bt_open(bm, &bt) == XTC_OK);
	for (i = 0; i < N_KEYS; i++) {
		mkkey(i, k, &kl);
		vl = (uint16_t)snprintf(v, sizeof v, "v%d", i);
		TCK(bt_insert(bt, k, kl, v, vl) == XTC_OK);
	}
	TCK(atomic_load(&bt->st_height) > 1);
	TCK(pins_balanced(bm));
	mkkey(N_KEYS / 2, k, &kl);

	/* 0 = control (no stale hit); BT_DELETE_RETRIES = every bounded
	 * attempt stale (the old give-up path); more = past the old bound. */
	for (n = 0; n < 4; n++) {
		static const int counts[4] = { 0, 1, BT_DELETE_RETRIES,
		    BT_DELETE_RETRIES + 3 };
		int nn = counts[n];

		g_force_stale = nn;
		f = NULL;
		TCK(descend_shared(bt, k, kl, &f) == XTC_OK && f != NULL);
		TCK(g_force_stale == 0);
		if (f != NULL) check_held(bt, f, k, kl);
		TCK(pins_balanced(bm));
		if (g_fail) break;   /* a corrupted latch would hang the next one */

		g_force_stale = nn;
		f = NULL;
		TCK(cursor_descend(bt, k, kl, &f) == XTC_OK && f != NULL);
		TCK(g_force_stale == 0);
		if (f != NULL) check_held(bt, f, k, kl);
		TCK(pins_balanced(bm));
		printf("  %s %d forced stale descents: frame held, pins balanced\n",
		    g_fail ? "FAIL" : "ok  ", nn);
		if (g_fail) break;
	}

	bt_close(bt);
	bm_destroy(bm);
	unlink(path);
	if (g_fail) return 1;
	printf("test_btree_stale_descent: OK\n");
	return 0;
}
