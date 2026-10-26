/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_btree_bounded_descent.c
 *	A damaged tree must make a descent FAIL, not spin.  A B-link
 *	descent restarts from the root when it meets a node a concurrent
 *	merge changed, and move_right follows right links while the key is
 *	beyond a node's upper fence; both loops were unbounded, so one bad
 *	page (a right-link cycle, a leaf whose lower fence no longer
 *	matches its parent's separator) froze every reader routed through
 *	it at 100% CPU with nothing reported.  This test damages a built
 *	tree three ways and checks that lookups, cursor opens, inserts and
 *	deletes through the damage return an error, and that keys elsewhere
 *	still work:
 *	  1. a leaf linked to itself (the shortest right-link cycle),
 *	  2. two leaves linked to each other, every hi fence below the key,
 *	  3. a leaf whose lower fence is raised above keys its parent routes
 *	     to it (the latched descent restarts forever).
 *	In case 3 the writes may succeed: under the SMO lock no merge runs,
 *	so the locked insert/delete paths take the routed leaf without the
 *	fence check and do not loop; they only must return.
 *	An alarm turns a regression (a hang) into a failure.  The bounds are
 *	lowered so the error path is reached in milliseconds; the
 *	implementation is included for the static descents.
 */
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define BT_RESTART_MAX    2000L
#define BT_MOVE_RIGHT_MAX 2000L
#include "bufmgr.c"
#include "btree.c"
#include "t_tmp.h"

#define PAGE_SZ 512
#define N_KEYS  400        /* many 512-byte leaves; height >= 2 */

static int g_fail;
#define TCK(c) do { if (!(c)) { \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
	g_fail = 1; } } while (0)

static void
on_alarm(int sig)
{
	static const char m[] = "FAIL: a descent did not return (hang)\n";
	ssize_t n;

	(void)sig;
	n = write(2, m, sizeof m - 1);
	_exit(n < 0 ? 2 : 1);
}

static uint16_t
mkkey(int i, char *buf)
{
	return (uint16_t)snprintf(buf, 16, "k%05d", i);
}

/* The leaf that holds key i (fixed, unlatched: the test is single-threaded
 * and the caller writes its header directly). */
static bm_frame_t *
leaf_of(bt_t *bt, int i)
{
	char k[16];
	uint16_t kl = mkkey(i, k);
	bm_frame_t *f = NULL;

	if (descend_shared(bt, k, kl, &f) != XTC_OK)
		return NULL;
	bm_unlatch(f);
	return f;
}

/* Index of the first and one-past-last key in the leaf (by probing). */
static void
leaf_span(bt_t *bt, int i, int *lo, int *hi)
{
	bm_frame_t *f = leaf_of(bt, i), *g;
	bm_pid_t pid = bm_frame_pid(f);
	int j;

	bm_unfix(bt->bm, f, 0);
	for (j = i; j > 0; j--) {
		g = leaf_of(bt, j - 1);
		bm_unfix(bt->bm, g, 0);
		if (bm_frame_pid(g) != pid)
			break;
	}
	*lo = j;
	for (j = i + 1; j < N_KEYS; j++) {
		g = leaf_of(bt, j);
		bm_unfix(bt->bm, g, 0);
		if (bm_frame_pid(g) != pid)
			break;
	}
	*hi = j;
}

/* Every operation on key i must return; reads (and writes, if
 * `writes`) must fail. */
static void
expect_fail(bt_t *bt, int i, const char *what, int writes)
{
	char k[16], v[16];
	uint16_t kl = mkkey(i, k), vl = 0;
	bt_cursor_t *c = NULL;
	int rl, rc, ri, rd;

	alarm(20);
	rl = bt_lookup(bt, k, kl, v, sizeof v, &vl);
	rc = bt_cursor_open(bt, k, kl, &c);
	if (rc == XTC_OK)
		bt_cursor_close(c);
	ri = bt_insert(bt, k, kl, "x", 1);
	rd = bt_delete(bt, k, kl);
	alarm(0);
	printf("  %-34s lookup=%d cursor=%d insert=%d delete=%d\n", what, rl,
	    rc, ri, rd);
	TCK(rl != XTC_OK && rc != XTC_OK);
	if (writes)
		TCK(ri != XTC_OK && rd != XTC_OK);
}

static void
expect_ok(bt_t *bt, int i)
{
	char k[16], v[16];
	uint16_t kl = mkkey(i, k), vl = 0;

	alarm(20);
	TCK(bt_lookup(bt, k, kl, v, sizeof v, &vl) == XTC_OK);
	alarm(0);
}

static int
build(const char *tag, bm_t **bmp, bt_t **btp, char *path, size_t cap)
{
	bm_opts_t bo = BM_OPTS_DEFAULT;
	char k[16];
	int fd, i;

	t_tmpl(path, cap, tag);
	if ((fd = mkstemp(path)) < 0)
		return -1;
	close(fd);
	bo.path = path;
	bo.page_size = PAGE_SZ;
	bo.n_frames = 512;         /* resident: no eviction */
	if (bm_create(&bo, bmp) != XTC_OK || bt_open(*bmp, btp) != XTC_OK)
		return -1;
	for (i = 0; i < N_KEYS; i++)
		if (bt_insert(*btp, k, mkkey(i, k), "v", 1) != XTC_OK)
			return -1;
	return atomic_load(&(*btp)->st_height) >= 2 ? 0 : -1;
}

static void
teardown(bm_t *bm, bt_t *bt, const char *path)
{
	bt_close(bt);
	bm_destroy(bm);
	unlink(path);
}

int
main(void)
{
	char path[256];
	bm_t *bm;
	bt_t *bt;
	bm_frame_t *a, *b;
	int lo, hi, mid = N_KEYS / 2;

	signal(SIGALRM, on_alarm);

	/* 1. a leaf whose right link is itself, its hi fence below the key. */
	TCK(build("sqlxtc-bnd1", &bm, &bt, path, sizeof path) == 0);
	if (g_fail) return 1;
	leaf_span(bt, mid, &lo, &hi);
	a = leaf_of(bt, mid);
	btnode_set_right_sibling(bm_page(a), (uint32_t)bm_frame_pid(a));
	/* Cut the hi fence to its first byte ("k"): every key is now
	 * beyond the leaf and move_right follows the self link. */
	((struct btnode_hdr *)bm_page(a))->hi_fence_len = 1;
	bm_unfix(bm, a, 0);
	expect_fail(bt, hi - 1, "1. right link to itself:", 1);
	expect_ok(bt, 0);
	expect_ok(bt, N_KEYS - 1);
	teardown(bm, bt, path);

	/* 2. a two-leaf cycle: A -> B -> A, both hi fences below the key. */
	TCK(build("sqlxtc-bnd2", &bm, &bt, path, sizeof path) == 0);
	if (g_fail) return 1;
	leaf_span(bt, mid, &lo, &hi);
	a = leaf_of(bt, mid);
	b = leaf_of(bt, hi);              /* the next leaf */
	TCK(bm_frame_pid(a) != bm_frame_pid(b));
	/* B links back to A and its hi fence is cut to "k": a key routed
	 * to B is beyond it, then beyond A's (true) fence: A <-> B. */
	btnode_set_right_sibling(bm_page(b), (uint32_t)bm_frame_pid(a));
	((struct btnode_hdr *)bm_page(b))->hi_fence_len = 1;
	bm_unfix(bm, b, 0);
	bm_unfix(bm, a, 0);
	expect_fail(bt, hi, "2. two-leaf right-link cycle:", 1);
	expect_ok(bt, 0);
	teardown(bm, bt, path);

	/* 3. a leaf whose lo fence is raised past keys routed to it: every
	 * latched descent for them restarts (the RANGE3 hang's shape). */
	TCK(build("sqlxtc-bnd3", &bm, &bt, path, sizeof path) == 0);
	if (g_fail) return 1;
	leaf_span(bt, mid, &lo, &hi);
	a = leaf_of(bt, mid);
	{
		/* lo fence := the hi fence: every key the parent routes here is
		 * at or below it.  The node prefix (read from the lo fence's
		 * first prefix_len bytes) is common to both, so slots decode. */
		struct btnode_hdr *h = (struct btnode_hdr *)bm_page(a);
		h->lo_fence_off = h->hi_fence_off;
		h->lo_fence_len = h->hi_fence_len;
	}
	bm_unfix(bm, a, 0);
	expect_fail(bt, lo, "3. lo fence above routed keys:", 0);
	expect_ok(bt, 0);
	expect_ok(bt, N_KEYS - 1);
	TCK(atomic_load(&bt->st_stuck) > 0);
	teardown(bm, bt, path);

	if (g_fail) {
		printf("test_btree_bounded_descent: FAIL\n");
		return 1;
	}
	printf("test_btree_bounded_descent: OK\n");
	return 0;
}
