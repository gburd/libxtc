/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_btree_cursor_hop.c
 *	A forward cursor that leaves a leaf releases it before it fixes the
 *	leaf's right sibling.  In that window a merge can absorb the sibling
 *	into the leaf just left and free its page id, and the id can be
 *	reissued as a node of another key range.  The cursor then read the
 *	reissued page from slot 0: a full scan returned keys out of order
 *	(test_btree_smo_stress saw it under a cold, evicting pool).  The
 *	test-only BT_HOP_TEST_HOOK runs that interleaving deterministically
 *	in the window: empty the leaf just left (the merge pulls the sibling
 *	into it and frees the sibling), then insert keys of another range
 *	until a split reissues the freed id.
 *	The scan must still return every surviving key exactly once, in
 *	order.  Likewise a descent to the leftmost leaf (no routing key) that
 *	is handed a reissued pid naming a mid-tree leaf must not open there.
 *	The implementation is included so the hooks are reachable.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_fire;            /* armed: the next hop runs the merge */
static int g_fired;
static unsigned long long g_target, g_reissued_as;
struct bt_cursor;
static void hop_hook(struct bt_cursor *c, unsigned long long pid);
#define BT_HOP_TEST_HOOK(c, pid) hop_hook((c), (unsigned long long)(pid))
/* Restore the real root at the first latched-descent restart. */
static unsigned long long g_restore_root;
static void restart_hook(void);
#define BT_RESTART_TEST_HOOK() restart_hook()
#define BT_STALE_TEST_HOOK() 0
#define BT_STALE_TEST_HOOK_PEEK() (g_restore_root != 0)
#include "bufmgr.c"
#include "btree.c"
#include "t_tmp.h"

#define PAGE_SZ 512
#define N_A     60      /* keys "a000".. : several leaves */
#define N_Z     400     /* keys "z0000".. inserted in the window */

static bm_t *g_bm;
static bt_t *g_bt;
static int g_fail;
static int g_restarts;

static void
restart_hook(void)
{
	if (g_restore_root != 0 && g_restarts++ == 1) {
		atomic_store(&g_bt->root_pid, (bm_pid_t)g_restore_root);
		g_restore_root = 0;
	}
}
#define TCK(c) do { if (!(c)) { \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
	g_fail = 1; } } while (0)

static uint16_t
akey(char *k, int i)
{
	return (uint16_t)snprintf(k, 16, "a%03d", i);
}

/* Which a-keys are still present (the hook deletes some). */
static int g_alive[N_A];

static void
hop_hook(struct bt_cursor *c, unsigned long long pid)
{
	char k[16], v[16], last[16];
	bm_frame_t *f;
	int i, upto = -1;
	uint16_t l;

	if (!g_fire)
		return;
	g_fire = 0;
	g_fired = 1;
	g_target = pid;
	/* Empty the leaf the cursor just left (every a-key up to the last
	 * one returned): the deletes underflow it, and the merge pulls its
	 * right sibling -- `pid`, where the cursor is going -- into it and
	 * frees `pid`. */
	l = c->keylen < 15 ? c->keylen : 15;
	memcpy(last, c->keybuf, l);
	last[l] = '\0';
	(void)sscanf(last, "a%d", &upto);
	for (i = 0; i <= upto && i < N_A; i++)
		if (bt_delete(g_bt, k, akey(k, i)) == XTC_OK)
			g_alive[i] = 0;
	/* Drain the quarantine; z-key splits then reissue the freed id. */
	for (i = 0; i < N_Z && g_reissued_as == 0; i++) {
		l = (uint16_t)snprintf(k, sizeof k, "z%04d", i);
		memset(v, 'z', sizeof v);
		(void)bt_insert(g_bt, k, l, v, sizeof v);
		bm_reclaim_quarantine(g_bm);
		if (!pid_is_quarantined(g_bm, (bm_pid_t)pid) &&
		    bm_fix_pid(g_bm, (bm_pid_t)pid, &f) == XTC_OK) {
			if (!btnode_is_dead(bm_page(f)))
				g_reissued_as = pid;
			bm_unfix(g_bm, f, 0);
		}
	}
}

int
main(void)
{
	char path[256], k[16], v[16];
	bm_opts_t bo = BM_OPTS_DEFAULT;
	bt_cursor_t *c = NULL;
	const void *ck, *cv;
	uint16_t ckl, cvl;
	char prev[16] = "";
	int seen[N_A];
	int fd, i, n = 0, order_bad = 0, missing = 0, dup = 0;

	t_tmpl(path, sizeof path, "sqlxtc-hop");
	if ((fd = mkstemp(path)) < 0) return 1;
	close(fd);
	bo.path = path; bo.page_size = PAGE_SZ; bo.n_frames = 256;
	TCK(bm_create(&bo, &g_bm) == XTC_OK);
	TCK(bt_open(g_bm, &g_bt) == XTC_OK);
	if (g_fail) return 1;
	for (i = 0; i < N_A; i++) {
		memset(v, 'a', sizeof v);
		TCK(bt_insert(g_bt, k, akey(k, i), v, sizeof v) == XTC_OK);
		g_alive[i] = 1;
	}
	TCK(atomic_load(&g_bt->st_height) > 1);

	/* Scan; arm the hook on the FIRST leaf hop. */
	memset(seen, 0, sizeof seen);
	g_fire = 1;
	TCK(bt_cursor_open(g_bt, NULL, 0, &c) == XTC_OK);
	while (c != NULL && bt_cursor_next(c, &ck, &ckl, &cv, &cvl) == XTC_OK) {
		char cur[16];
		uint16_t l = ckl < 15 ? ckl : 15;
		memcpy(cur, ck, l);
		cur[l] = '\0';
		if (n > 0 && strcmp(prev, cur) >= 0) {
			if (order_bad++ == 0)
				fprintf(stderr, "  out of order: %s after %s\n", cur,
				    prev);
		}
		if (cur[0] == 'a') {
			int idx = -1;
			(void)sscanf(cur, "a%d", &idx);
			if (idx >= 0 && idx < N_A && seen[idx]++ > 0)
				dup++;
		}
		memcpy(prev, cur, l + 1u);
		n++;
	}
	bt_cursor_close(c);
	for (i = 0; i < N_A; i++)
		if (g_alive[i] && seen[i] == 0)
			missing++;
	TCK(g_fired);
	TCK(g_reissued_as != 0);    /* the interleaving really happened */
	TCK(order_bad == 0);
	TCK(missing == 0);
	TCK(dup == 0);
	printf("  hop into pid %llu after it was merged away and reissued: %d "
	    "keys scanned, %d out of order, %d live keys missed, %d repeated\n",
	    g_target, n, order_bad, missing, dup);
	/* A keyless descent (to the leftmost leaf) handed a pid that now
	 * names a leaf in the middle of the tree (a stale root pid after a
	 * collapse, reissued) must not open there: point the root at a
	 * non-leftmost leaf for one descent; the scan has to restart and
	 * still start at the first key.  (The real root is restored after
	 * the descent's first root fix, by the restart hook.) */
	{
		bm_frame_t *f = NULL;
		bm_pid_t realroot = atomic_load(&g_bt->root_pid), mid;
		TCK(cursor_descend(g_bt, (const void *)"z0100", 5, &f) == XTC_OK);
		mid = bm_frame_pid(f);
		bm_unlatch(f);
		bm_unfix(g_bm, f, 0);
		g_restore_root = realroot;
		atomic_store(&g_bt->root_pid, mid);
		f = NULL;
		TCK(cursor_descend(g_bt, NULL, 0, &f) == XTC_OK && f != NULL);
		if (f != NULL) {
			uint16_t l = 1;
			TCK(btnode_lo_fence(bm_page(f), NULL, 0, &l) == 0 && l == 0);
			printf("  a keyless descent through a mid-tree leaf pid %llu "
			    "restarted and opened the leftmost leaf: %s\n",
			    (unsigned long long)mid, l == 0 ? "yes" : "NO");
			bm_unlatch(f);
			bm_unfix(g_bm, f, 0);
		}
		atomic_store(&g_bt->root_pid, realroot);
	}
	bt_close(g_bt);
	bm_destroy(g_bm);
	unlink(path);
	if (g_fail) return 1;
	printf("test_btree_cursor_hop: OK\n");
	return 0;
}
