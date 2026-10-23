/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_btree_smo_stress.c
 *	Structural invariants of the B-tree under concurrent split / merge /
 *	page-id reuse.  Off-loop threads (as the in-process engine's client
 *	threads are) churn one tree on tiny pages: an MVCC-like version
 *	churn (insert a new version key, delete the old one) over a small
 *	hot key range, an append/trim queue, and scans and point reads.
 *	Every few hundred milliseconds a checker takes the tree's SMO lock
 *	(no split or merge can run) and walks it top-down with shared
 *	latches, checking that every live parent routes to a live, unfreed
 *	page whose fences equal the parent's routing bounds, that each
 *	level's right-link chain matches the parent order, and that no page
 *	id is linked twice.  A descent that restarts without bound (the
 *	symptom of a broken link: every reader of the key range spins
 *	forever) is caught by a restart counter.  The file includes the
 *	implementation so the checker can see frames and pids.
 *
 *	Two phases run back to back: a resident tree (every page created in
 *	memory), then the same tree written out, closed cleanly and reopened
 *	into a SMALL pool, so the churn loads pages from disk, evicts them and
 *	reissues freed page ids while splits and merges run, with a
 *	checkpoint-like full scan (park/resume every 64 rows) alongside and
 *	the page provider (prefetch drain, cooling) and trickler (writeback)
 *	running on a loop thread.
 *
 *	SMO_STRESS_SECS (default 2, per phase) and SMO_STRESS_THREADS
 *	(default 8) scale it for a soak.
 */
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>

#include "xtc_loop.h"

static _Atomic long g_restarts_max;   /* longest run of stale restarts */
static _Thread_local long t_restarts;
#define BT_STALE_TEST_HOOK() (0)
#define BT_STALE_TEST_HOOK_PEEK() (0)
#define BT_RESTART_TEST_HOOK() smo_restart_hook()
static void smo_restart_hook(void);
#include "bufmgr.c"
#include "btree.c"
#include "t_tmp.h"

#define PAGE_SZ     512
#define N_FRAMES    4096        /* phase 1: the whole tree stays resident */
#define N_FRAMES_COLD 160       /* phase 2: a pool far smaller than the tree */
#define HOT_ROWS    400
#define COLD_ROWS   6000        /* phase 2: rows built before the reopen */
#define RESTART_CAP 2000000L

static bm_t *g_bm;
static bt_t *g_bt;
static _Atomic int g_stop;
static _Atomic long g_ops, g_bad;
static _Atomic uint64_t g_ver[HOT_ROWS];   /* newest version stamp per row */
static _Atomic int g_rowlock[HOT_ROWS];
static _Atomic uint64_t g_qhead, g_qtail;  /* queue: keys [head, tail) live */
static pthread_mutex_t g_qmu = PTHREAD_MUTEX_INITIALIZER;

static void
smo_restart_hook(void)
{
	if (++t_restarts > RESTART_CAP) {
		fprintf(stderr, "FAIL: a descent restarted %ld times in a row "
		    "(a broken link)\n", t_restarts);
		abort();
	}
}

/* Versions: [0x10][row:4 BE][~ver:8 BE] (newest first, as xstore). */
static uint16_t
vkey(uint8_t *k, uint32_t row, uint64_t ver)
{
	int i;
	k[0] = 0x10;
	for (i = 0; i < 4; i++) k[1 + i] = (uint8_t)(row >> (24 - 8 * i));
	ver = ~ver;
	for (i = 0; i < 8; i++) k[5 + i] = (uint8_t)(ver >> (56 - 8 * i));
	return 13;
}

/* Queue: [0x20][seq:8 BE]. */
static uint16_t
qkey(uint8_t *k, uint64_t seq)
{
	int i;
	k[0] = 0x20;
	for (i = 0; i < 8; i++) k[1 + i] = (uint8_t)(seq >> (56 - 8 * i));
	return 9;
}

static uint64_t
rnd(uint64_t *s)
{
	*s ^= *s << 13; *s ^= *s >> 7; *s ^= *s << 17;
	return *s;
}

/* One row update: lock the row, insert the next version, delete the old
 * one (the version chain stays one long, as with GC at the horizon). */
static void
do_update(uint64_t *s)
{
	uint32_t row = (uint32_t)(rnd(s) % HOT_ROWS);
	uint8_t k[16], v[48];
	uint64_t old;
	int e = 0;
	uint16_t vl = (uint16_t)(8 + rnd(s) % 40);

	while (!atomic_compare_exchange_weak(&g_rowlock[row], &e, 1))
		e = 0;
	old = atomic_load(&g_ver[row]);
	memset(v, (int)(row & 0xff), sizeof v);
	if (bt_insert(g_bt, k, vkey(k, row, old + 1), v, vl) != XTC_OK)
		atomic_fetch_add(&g_bad, 1);
	if (bt_delete(g_bt, k, vkey(k, row, old)) != XTC_OK) {
		fprintf(stderr, "lost version row %u ver %llu\n", row,
		    (unsigned long long)old);
		atomic_fetch_add(&g_bad, 1);
	}
	atomic_store(&g_ver[row], old + 1);
	atomic_store(&g_rowlock[row], 0);
}

/* Queue: append at the tail or trim at the head (empties leaves on the
 * left, splits on the right: the merge-left and append-split paths). */
static void
do_queue(uint64_t *s)
{
	uint8_t k[16], v[32];
	uint64_t seq;
	int push = (rnd(s) & 1) != 0;

	memset(v, 'q', sizeof v);
	(void)pthread_mutex_lock(&g_qmu);
	if (!push && atomic_load(&g_qhead) == atomic_load(&g_qtail))
		push = 1;
	if (push) {
		seq = atomic_fetch_add(&g_qtail, 1);
		if (bt_insert(g_bt, k, qkey(k, seq), v,
		    (uint16_t)(8 + rnd(s) % 24)) != XTC_OK)
			atomic_fetch_add(&g_bad, 1);
	} else {
		seq = atomic_fetch_add(&g_qhead, 1);
		if (bt_delete(g_bt, k, qkey(k, seq)) != XTC_OK) {
			fprintf(stderr, "lost queue key %llu\n",
			    (unsigned long long)seq);
			atomic_fetch_add(&g_bad, 1);
		}
	}
	(void)pthread_mutex_unlock(&g_qmu);
}

/* Read a row's newest version with a cursor (the scan path that hung)
 * and a point lookup. */
static void
do_read(uint64_t *s)
{
	uint32_t row = (uint32_t)(rnd(s) % HOT_ROWS);
	uint8_t k[16], buf[64];
	uint16_t kl = vkey(k, row, ~(uint64_t)0 >> 1), vl;
	bt_cursor_t *c = NULL;
	const void *ck, *cv;
	uint16_t ckl, cvl;

	if (bt_cursor_open(g_bt, k, kl, &c) == XTC_OK) {
		(void)bt_cursor_next(c, &ck, &ckl, &cv, &cvl);
		bt_cursor_park(c);
		(void)bt_cursor_resume(c);
		(void)bt_cursor_next(c, &ck, &ckl, &cv, &cvl);
		bt_cursor_close(c);
	}
	(void)bt_lookup(g_bt, k, kl, buf, sizeof buf, &vl);
}

/* Phase 2: delete a random cold row and insert it again (the cold
 * region's leaves underflow, merge, and are refilled by splits; their
 * pages come from disk). */
static _Atomic int g_cold;
static _Atomic int g_coldlock[COLD_ROWS];

static void
do_cold(uint64_t *s)
{
	uint32_t row = HOT_ROWS + (uint32_t)(rnd(s) % COLD_ROWS);
	uint8_t k[16], v[48], buf[64];
	uint16_t kl = vkey(k, row, 0), vl;
	int e = 0;

	while (!atomic_compare_exchange_weak(&g_coldlock[row - HOT_ROWS], &e, 1))
		e = 0;
	if (bt_lookup(g_bt, k, kl, buf, sizeof buf, &vl) != XTC_OK) {
		fprintf(stderr, "lost cold row %u\n", row);
		atomic_fetch_add(&g_bad, 1);
	} else if (bt_delete(g_bt, k, kl) != XTC_OK) {
		atomic_fetch_add(&g_bad, 1);
	} else {
		memset(v, 'c', sizeof v);
		if (bt_insert(g_bt, k, kl, v, (uint16_t)(8 + rnd(s) % 40)) != XTC_OK)
			atomic_fetch_add(&g_bad, 1);
	}
	atomic_store(&g_coldlock[row - HOT_ROWS], 0);
}

/* A checkpoint-like full scan: every key once, in order, parking and
 * resuming the cursor every 64 rows (xs_dump_tree's online dump). */
static void
do_scan(void)
{
	bt_cursor_t *c = NULL;
	const void *ck, *cv;
	uint16_t ckl, cvl;
	uint8_t prev[BT_MAX_KEY];
	uint16_t pl = 0;
	long n = 0;

	if (bt_cursor_open(g_bt, NULL, 0, &c) != XTC_OK)
		return;
	while (bt_cursor_next(c, &ck, &ckl, &cv, &cvl) == XTC_OK) {
		if (n > 0 && key_cmp(prev, pl, ck, ckl) >= 0) {
			int j;
			fprintf(stderr, "scan out of order at row %ld (parked %d): prev ", n,
			    (int)((n & 63) == 0));
			for (j = 0; j < pl; j++) fprintf(stderr, "%02x", prev[j]);
			fprintf(stderr, " now ");
			for (j = 0; j < ckl; j++) fprintf(stderr, "%02x", ((const uint8_t *)ck)[j]);
			fprintf(stderr, " leaf %llu slot %d\n",
			    (unsigned long long)bm_frame_pid(c->leaf), c->slot);
			atomic_fetch_add(&g_bad, 1);
			break;
		}
		memcpy(prev, ck, ckl);
		pl = ckl;
		if ((++n & 63) == 0) {
			t_restarts = 0;
			bt_cursor_park(c);
			(void)bt_cursor_resume(c);
		}
	}
	bt_cursor_close(c);
}

static void *
worker(void *arg)
{
	uint64_t s = 0x9E3779B97F4A7C15ull * ((uint64_t)(uintptr_t)arg + 1);

	while (!atomic_load(&g_stop)) {
		uint64_t r = rnd(&s) % 10;
		t_restarts = 0;
		if (atomic_load(&g_cold) && (uintptr_t)arg == 0)
			do_scan();
		else if (atomic_load(&g_cold) && r < 3)
			do_cold(&s);
		else if (r < 5)
			do_update(&s);
		else if (r < 8)
			do_queue(&s);
		else
			do_read(&s);
		if (t_restarts > atomic_load(&g_restarts_max))
			atomic_store(&g_restarts_max, t_restarts);
		atomic_fetch_add(&g_ops, 1);
	}
	return NULL;
}

/* ---- the structural checker (caller holds bt->smo exclusively) ---- */

static uint8_t *g_seen;     /* pid -> linked once */
static uint32_t g_seen_cap;

struct chk {
	int bad;
	char why[256];
};

static void
chk_fail(struct chk *c, const char *msg, bm_pid_t pid)
{
	if (c->bad++ == 0)
		snprintf(c->why, sizeof c->why, "%s (pid %llu)", msg,
		    (unsigned long long)pid);
}

static int
fence_eq(const void *pg, int hi, const uint8_t *want, uint16_t wl)
{
	uint8_t f[BT_MAX_KEY];
	uint16_t fl = 0;

	if ((hi ? btnode_hi_fence(pg, f, sizeof f, &fl) :
	    btnode_lo_fence(pg, f, sizeof f, &fl)) != 0)
		return 0;
	return fl == wl && (wl == 0 || memcmp(f, want, wl) == 0);
}

/* Check the subtree at `pid`, which its parent routes (lo, hi] to
 * (lo/hi NULL = infinite).  `level` counts down to 1 at the leaves.
 * `next` is the pid the parent says follows this node at its level
 * (0 = the rightmost node of the level). */
static void
chk_node(struct chk *c, bm_pid_t pid, const uint8_t *lo, uint16_t lol,
    const uint8_t *hi, uint16_t hil, bm_pid_t next, int level)
{
	bm_frame_t *f;
	void *pg;
	int i, n, leaf;

	if (c->bad)
		return;
	if (pid == BM_PID_NONE || pid >= g_seen_cap) {
		chk_fail(c, "child pid out of range", pid);
		return;
	}
	if (g_seen[pid]++) {
		chk_fail(c, "pid linked twice", pid);
		return;
	}
	if (pid_is_quarantined(g_bm, pid)) {
		chk_fail(c, "a live parent links a retired (freed) pid", pid);
		return;
	}
	if (bm_fix_pid(g_bm, pid, &f) != XTC_OK) {
		chk_fail(c, "child pid cannot be fixed", pid);
		return;
	}
	bm_latch_shared(f);
	pg = bm_page(f);
	leaf = btnode_is_leaf(pg);
	n = (int)btnode_count(pg);
	if (btnode_is_dead(pg))
		chk_fail(c, "a live parent links a dead node", pid);
	else if (leaf != (level == 1))
		chk_fail(c, "leaf at the wrong depth", pid);
	else if (lo != NULL && leaf && !fence_eq(pg, 0, lo, lol))
		chk_fail(c, "leaf lo fence != parent separator", pid);
	else if (!fence_eq(pg, 1, hi, hil))
		chk_fail(c, "hi fence != parent routing bound", pid);
	else if (btnode_right_sibling(pg) != (uint32_t)next)
		chk_fail(c, "right link != parent's next child", pid);
	else if (!leaf && n == 0)
		chk_fail(c, "internal node with no children", pid);
	if (c->bad || leaf) {
		bm_unlatch(f);
		bm_unfix(g_bm, f, 0);
		return;
	}
	for (i = 0; i < n && !c->bad; i++) {
		uint8_t sl[BT_MAX_KEY], sh[BT_MAX_KEY];
		uint16_t sll = 0, shl = 0;
		const uint8_t *clo = lo, *chi = hi;
		uint16_t clol = lol, chil = hil;
		bm_pid_t cn = next;

		if (i > 0) {
			(void)btnode_full_key(pg, i, sl, sizeof sl, &sll);
			clo = sl; clol = sll;
		}
		if (i + 1 < n) {
			(void)btnode_full_key(pg, i + 1, sh, sizeof sh, &shl);
			chi = sh; chil = shl;
			cn = child_pid_at(pg, i + 1);
		} else if (next != BM_PID_NONE) {
			/* The parent's next node's first child follows us. */
			bm_frame_t *nf;
			cn = BM_PID_NONE;
			if (bm_fix_pid(g_bm, next, &nf) == XTC_OK) {
				bm_latch_shared(nf);
				cn = child_pid_at(bm_page(nf), 0);
				bm_unlatch(nf);
				bm_unfix(g_bm, nf, 0);
			}
		}
		chk_node(c, child_pid_at(pg, i), clo, clol, chi, chil, cn,
		    level - 1);
	}
	bm_unlatch(f);
	bm_unfix(g_bm, f, 0);
}

static int
check_tree(char *why, size_t wl)
{
	struct chk c;
	int height;

	memset(&c, 0, sizeof c);
	if (xtc_arwlock_wrlock(g_bt->smo, -1) != XTC_OK)
		return 0;
	(void)pthread_mutex_lock(&g_bm->pid_mu);
	g_seen_cap = (uint32_t)g_bm->next_pid;
	(void)pthread_mutex_unlock(&g_bm->pid_mu);
	g_seen = calloc(g_seen_cap + 1u, 1);
	height = (int)atomic_load(&g_bt->st_height);
	if (g_seen != NULL)
		chk_node(&c, atomic_load(&g_bt->root_pid), NULL, 0, NULL, 0,
		    BM_PID_NONE, height);
	(void)xtc_arwlock_unlock(g_bt->smo);
	free(g_seen);
	g_seen = NULL;
	if (c.bad)
		snprintf(why, wl, "%s", c.why);
	return c.bad;
}

/* Run nth workers for secs seconds, checking the tree every 200 ms.
 * Returns nonzero (with the reason in why) on a violation. */
static int
run_phase(int nth, int secs, char *why, size_t wl, int *checks)
{
	pthread_t th[64];
	int i, bad = 0;
	int64_t t0;

	atomic_store(&g_stop, 0);
	for (i = 0; i < nth; i++)
		if (pthread_create(&th[i], NULL, worker, (void *)(uintptr_t)i) != 0)
			return snprintf(why, wl, "pthread_create"), 1;
	t0 = xtc_clock_mono();
	while (xtc_clock_mono() - t0 < (int64_t)secs * 1000000000LL && !bad) {
		(void)xtc_sleep_ns(200LL * 1000 * 1000);
		bad = check_tree(why, wl);
		(*checks)++;
	}
	atomic_store(&g_stop, 1);
	for (i = 0; i < nth; i++)
		(void)pthread_join(th[i], NULL);
	if (!bad)
		bad = check_tree(why, wl);
	return bad;
}

static void
report(const char *phase, int nth, int secs, int checks)
{
	bt_stats_t ts;
	bm_stats_t bs;

	bt_get_stats(g_bt, &ts);
	bm_get_stats(g_bm, &bs);
	printf("  %s: %d threads %d s: %ld ops, %d checks, height %llu splits "
	    "%llu merges %llu freed %llu reissued %llu loads %llu evicted "
	    "%llu, longest restart run %ld\n", phase, nth, secs,
	    atomic_load(&g_ops), checks, (unsigned long long)ts.height,
	    (unsigned long long)ts.splits, (unsigned long long)ts.merges,
	    (unsigned long long)bs.freed, (unsigned long long)bs.reissued,
	    (unsigned long long)bs.loads, (unsigned long long)bs.evicted,
	    atomic_load(&g_restarts_max));
}

static void *
loop_thread(void *arg)
{
	(void)xtc_loop_run(arg);
	return NULL;
}

int
main(void)
{
	char path[256], why[256];
	pthread_t lt;
	bm_opts_t bo = BM_OPTS_DEFAULT;
	xtc_loop_t *loop = NULL;
	const char *e;
	int secs = 2, nth = 8, i, fd, checks = 0, bad;
	uint64_t clean = 0, clock = 0;

	if ((e = getenv("SMO_STRESS_SECS")) != NULL) secs = atoi(e);
	if ((e = getenv("SMO_STRESS_THREADS")) != NULL) nth = atoi(e);
	if (nth < 2) nth = 2;
	if (nth > 64) nth = 64;
	t_tmpl(path, sizeof path, "sqlxtc-smo");
	if ((fd = mkstemp(path)) < 0) return 1;
	close(fd);
	bo.path = path; bo.page_size = PAGE_SZ; bo.n_frames = N_FRAMES;
	if (bm_create(&bo, &g_bm) != XTC_OK || bt_open(g_bm, &g_bt) != XTC_OK)
		return 1;
	for (i = 0; i < HOT_ROWS + COLD_ROWS; i++) {   /* version 0 of every row */
		uint8_t k[16], v[16];
		memset(v, 'v', sizeof v);
		(void)bt_insert(g_bt, k, vkey(k, (uint32_t)i, 0), v, sizeof v);
	}

	/* Phase 1: resident. */
	bad = run_phase(nth, secs, why, sizeof why, &checks);
	report("resident", nth, secs, checks);

	/* Phase 2: clean close, reopen into a small pool (cold, evicting),
	 * with the page provider running on a loop thread. */
	if (!bad) {
		bt_set_meta(g_bt, 1, 0);
		if (bm_checkpoint(g_bm) != XTC_OK || bt_write_super(g_bt) != XTC_OK ||
		    bm_sync(g_bm) != XTC_OK)
			return 1;
		bt_close(g_bt);
		bm_destroy(g_bm);
		bo.n_frames = N_FRAMES_COLD;
		bo.reopen = 1;
		if (bm_create(&bo, &g_bm) != XTC_OK || bt_reopen(g_bm, &g_bt) != XTC_OK)
			return 1;
		bt_get_meta(g_bt, &clean, &clock);
		if (clean != 1)
			return 1;
		if (xtc_loop_init(&loop) != XTC_OK ||
		    bm_provider_spawn(g_bm, loop, 1LL * 1000 * 1000, NULL) != XTC_OK ||
		    bm_trickler_spawn(g_bm, loop, 1LL * 1000 * 1000, NULL) != XTC_OK ||
		    pthread_create(&lt, NULL, loop_thread, loop) != 0)
			return 1;
		atomic_store(&g_cold, 1);
		atomic_store(&g_ops, 0);
		checks = 0;
		bad = run_phase(nth, secs, why, sizeof why, &checks);
		report("cold+evicting", nth, secs, checks);
		bm_provider_stop(g_bm);
		bm_trickler_stop(g_bm);
		(void)pthread_join(lt, NULL);
		(void)xtc_loop_fini(loop);
	}
	bt_close(g_bt);
	bm_destroy(g_bm);
	unlink(path);
	if (bad || atomic_load(&g_bad) != 0) {
		fprintf(stderr, "FAIL: %s; %ld bad ops\n", bad ? why : "tree ok",
		    atomic_load(&g_bad));
		return 1;
	}
	printf("test_btree_smo_stress: OK\n");
	return 0;
}
