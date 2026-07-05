/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_fuzzy_checkpoint.c
 *	Increment 4: fuzzy (ARIES-style) checkpoint with recLSN-horizon
 *	log truncation.
 *
 *	The full-compaction checkpoint (xstore_checkpoint_wal) is
 *	O(live-data): it dumps every live row into the log.  The fuzzy
 *	checkpoint (xstore_fuzzy_checkpoint) is O(dirty): it flushes the
 *	dirty page set to the base (durability), makes the whole log up to
 *	the durable LSN redundant with the flushed base, writes a
 *	CHECKPOINT record carrying that redo horizon as its start-LSN, and
 *	TRUNCATES the log below the horizon (dropping records whose changes
 *	are already durable on the base).  Recovery of a fuzzy checkpoint
 *	trusts the base in place (xstore_recover_inplace) and replays only
 *	the retained tail, so restart cost is O(dirty), not O(database).
 *
 *	This test proves the two required properties:
 *
 *	  1. TRUNCATION.  After a fuzzy checkpoint the log is a small
 *	     fraction of the pre-checkpoint log: everything below the
 *	     horizon (already on the flushed base) was dropped.
 *
 *	  2. EQUIVALENCE.  Recovering from the fuzzy checkpoint (in place,
 *	     from the horizon over the trusted base plus a post-checkpoint
 *	     tail) restores EXACTLY the same committed rows as a full-scan
 *	     recovery of the untruncated log onto a fresh tree.  Same count,
 *	     same per-key values, ordered gap/dup-free scan.
 *
 *	Low-level bt/bm harness (like test_steal_leaf / test_inplace_redo);
 *	no loop; the WAL is driven synchronously (wal_commit_sync).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "bufmgr.h"
#include "btree.h"
#include "xstore.h"
#include "wal.h"
#include "xlog.h"
#include "engine.h"
#include "xtc.h"
#include "t_tmp.h"

#define N_BASE   4000         /* rows written before the fuzzy checkpoint */
#define N_TAIL   200          /* rows written AFTER it (the retained tail) */
#define N_ROWS   (N_BASE + N_TAIL)
#define PAGE_SZ  512          /* tiny pages -> many leaves, deep tree */
#define POOL     64           /* small pool -> most leaves evicted before flush */

static off_t
fsize(const char *p)
{
	struct stat st;
	return stat(p, &st) == 0 ? st.st_size : -1;
}

static int
eval_int(sx_db *db, const char *sql)
{
	sx_stmt *st = NULL;
	int v = -1;
	if (sx_prepare(db, sql, -1, &st, NULL) != SX_OK)
		return -1;
	if (sx_step(st) == SX_ROW)
		v = (int)sx_column_int64(st, 0);
	sx_finalize(st);
	return v;
}

static int
sel_v(sx_db *db, int k, char *out, size_t cap)
{
	sx_stmt *st = NULL;
	int got = 0;
	if (sx_prepare(db, "SELECT v FROM t WHERE k=?", -1, &st, NULL) != SX_OK)
		return -1;
	sx_bind_int64(st, 1, k);
	if (sx_step(st) == SX_ROW) {
		const char *t = sx_column_text(st, 0);
		size_t n = (size_t)sx_column_bytes(st, 0);
		if (n >= cap) n = cap - 1;
		if (t) memcpy(out, t, n);
		out[n] = '\0';
		got = 1;
	}
	sx_finalize(st);
	return got;
}

/* Insert [base, base+n) rows through db as autocommit inserts. */
static int
fill(sx_db *db, int base, int n)
{
	int i;
	for (i = 0; i < n; i++) {
		char sql[80];
		snprintf(sql, sizeof sql, "INSERT INTO t(k,v) VALUES(%d,'val-%d');",
		    base + i, base + i);
		if (sx_exec(db, sql, NULL) != SX_OK) {
			fprintf(stderr, "insert %d failed\n", base + i);
			return -1;
		}
	}
	return 0;
}

/* Full verification of a recovered base holding [0, n) rows: count,
 * per-key value, ordered gap/dup-free full scan.  Returns 0 on success. */
static int
verify_full(bt_t *bt, int n)
{
	sx_db *db = NULL;
	sx_stmt *st = NULL;
	int i, miss = 0, scanned = 0, prev = -1, ordered = 1, ok = 1;
	char want[32], got[32];

	if (sx_open_bt(bt, &db) != SX_OK)
		return -1;
	(void)sx_exec(db, "CREATE TABLE t(k INTEGER PRIMARY KEY, v)", NULL);
	if (eval_int(db, "SELECT count(*) FROM t") != n) ok = 0;
	for (i = 0; i < n; i++) {
		snprintf(want, sizeof want, "val-%d", i);
		if (sel_v(db, i, got, sizeof got) == 1 && strcmp(got, want) == 0)
			continue;
		miss++;
	}
	if (miss) ok = 0;
	if (sx_prepare(db, "SELECT k FROM t ORDER BY k", -1, &st, NULL) == SX_OK) {
		while (sx_step(st) == SX_ROW) {
			int k = (int)sx_column_int64(st, 0);
			if (k <= prev) ordered = 0;
			prev = k;
			scanned++;
		}
		sx_finalize(st);
	}
	if (!ordered || scanned != n || prev != n - 1) ok = 0;
	if (!ok)
		fprintf(stderr, "FAIL: verify n=%d miss=%d scanned=%d ordered=%d prev=%d\n",
		    n, miss, scanned, ordered, prev);
	sx_close(db);
	return ok ? 0 : -1;
}

/*
 * Build the workload into a fresh base at btp with its log at logp.
 * Writes N_BASE rows.  When fuzzy != 0, runs a fuzzy checkpoint (flush
 * base durable + truncate log + mark clean), captures pre/post log sizes
 * and the horizon, THEN writes N_TAIL more rows into the (fresh) log.
 * When fuzzy == 0, writes all N_ROWS rows into one growing log (the
 * full-scan baseline).  Leaves the engine cleanly torn down, log intact.
 */
static int
build(const char *btp, const char *logp, int fuzzy,
    uint64_t *horizon_out, off_t *pre_out, off_t *post_out)
{
	bm_opts_t bo = BM_OPTS_DEFAULT;
	wal_opts_t wo;
	bm_t *bm = NULL;
	bt_t *bt = NULL;
	wal_t *wal = NULL;
	sx_db *db = NULL;

	memset(&wo, 0, sizeof wo);
	wo.path = logp; wo.window_ns = 0; wo.max_batch = 1;   /* synchronous */
	if (wal_open(&wo, &wal) != XTC_OK)
		return -1;
	bo.path = btp; bo.page_size = PAGE_SZ; bo.n_frames = POOL;
	bo.lsn_off = 0;             /* ARIES page LSN at the node front */
	bo.double_write = 1;
	if (bm_create(&bo, &bm) != XTC_OK) { wal_close(wal); return -1; }
	if (bt_open(bm, &bt) != XTC_OK) { bm_destroy(bm); wal_close(wal); return -1; }
	xstore_set_wal((struct wal *)wal);
	xstore_register_smo(1);     /* physiological logging -> trusted base */

	if (sx_open_bt(bt, &db) != SX_OK) goto fail;
	if (sx_exec(db, "CREATE TABLE t(k INTEGER PRIMARY KEY, v)", NULL) != SX_OK)
		goto fail;
	if (fill(db, 0, N_BASE) != 0) goto fail;

	if (fuzzy) {
		uint64_t horizon = 0;
		sx_close(db); db = NULL;         /* quiesce commits for the checkpoint */
		if (pre_out) *pre_out = fsize(logp);
		/* Fuzzy checkpoint: flush dirty pages durable, truncate the log
		 * below the redo horizon, mark base clean. */
		if (xstore_fuzzy_checkpoint(bt, bm, (struct wal *)wal, logp,
		    &horizon) != XTC_OK)
			goto fail;
		bt_set_meta(bt, 1, xstore_clock());   /* base trusted from here */
		bt_write_super(bt);
		(void)bm_sync(bm);
		if (post_out) *post_out = fsize(logp);
		if (horizon_out) *horizon_out = horizon;
		/* Post-checkpoint tail: reopen the SQL layer, write more rows.
		 * These land in the fresh (truncated + rebound) log; recovery
		 * replays them in place over the trusted base. */
		if (sx_open_bt(bt, &db) != SX_OK) goto fail;
		(void)sx_exec(db, "CREATE TABLE t(k INTEGER PRIMARY KEY, v)", NULL);
		if (fill(db, N_BASE, N_TAIL) != 0) goto fail;
	} else {
		if (fill(db, N_BASE, N_TAIL) != 0) goto fail;
	}
	sx_close(db); db = NULL;

	xstore_register_smo(0);
	xstore_set_wal(NULL);
	bt_close(bt);
	bm_destroy(bm);
	wal_close(wal);
	return 0;
fail:
	if (db) sx_close(db);
	xstore_register_smo(0);
	xstore_set_wal(NULL);
	if (bt) bt_close(bt);
	if (bm) bm_destroy(bm);
	wal_close(wal);
	return -1;
}

/* Recover a fresh (truncated) tree from the FULL log by logical replay
 * (xstore_recover).  Leaves the recovered bt/bm in *bto/*bmo. */
static int
recover_full(const char *btp, const char *logp, bm_t **bmo, bt_t **bto)
{
	bm_opts_t bo = BM_OPTS_DEFAULT;
	bm_t *bm = NULL;
	bt_t *bt = NULL;

	bo.path = btp; bo.page_size = PAGE_SZ; bo.n_frames = POOL;
	bo.lsn_off = 0; bo.reopen = 0;    /* fresh page file: rebuild logically */
	bo.double_write = 1;
	if (bm_create(&bo, &bm) != XTC_OK)
		return -1;
	if (bt_open(bm, &bt) != XTC_OK) { bm_destroy(bm); return -1; }
	if (xstore_recover(bt, logp) != XTC_OK) {
		bt_close(bt); bm_destroy(bm);
		return -1;
	}
	*bmo = bm; *bto = bt;
	return 0;
}

/* Recover the fuzzy-checkpointed base IN PLACE from the truncated log:
 * trust the flushed base (reopen=1), replay only the retained tail. */
static int
recover_inplace(const char *btp, const char *logp, bm_t **bmo, bt_t **bto,
    uint64_t *pages)
{
	bm_opts_t bo = BM_OPTS_DEFAULT;
	wal_opts_t wo;
	bm_t *bm = NULL;
	bt_t *bt = NULL;
	wal_t *w = NULL;

	bo.path = btp; bo.page_size = PAGE_SZ; bo.n_frames = POOL;
	bo.lsn_off = 0; bo.reopen = 1;
	bo.double_write = 1;
	if (bm_create(&bo, &bm) != XTC_OK)
		return -1;
	if (bt_reopen(bm, &bt) != XTC_OK) { bm_destroy(bm); return -1; }

	memset(&wo, 0, sizeof wo);
	wo.path = logp; wo.append = 1;
	if (wal_open(&wo, &w) != XTC_OK) { bt_close(bt); bm_destroy(bm); return -1; }
	xstore_set_wal((struct wal *)w);
	xstore_register_smo(1);
	if (xstore_recover_inplace(bt, bm, logp, pages) != XTC_OK) {
		xstore_register_smo(0);
		xstore_set_wal(NULL); wal_close(w);
		bt_close(bt); bm_destroy(bm);
		return -1;
	}
	xstore_register_smo(0);
	xstore_set_wal(NULL);
	wal_close(w);
	*bmo = bm; *bto = bt;
	return 0;
}

int
main(void)
{
	char btpF[256], logF[256], dwF[288];   /* full-scan baseline base */
	char btpZ[256], logZ[256], dwZ[288];   /* fuzzy-checkpointed base */
	bm_t *bm = NULL;
	bt_t *bt = NULL;
	off_t pre = -1, post = -1;
	uint64_t horizon = 0, pages = 0;
	int fd;

	/* ============================================================
	 * Baseline: build the full workload into one growing log, recover
	 * it logically onto a fresh tree, and assert the full committed set
	 * is present.  This is the "same rows a full-scan recovery yields"
	 * reference.
	 * ============================================================ */
	t_tmpl(btpF, sizeof btpF, "fuzzy-full-bt");
	t_tmpl(logF, sizeof logF, "fuzzy-full-log");
	if ((fd = mkstemp(btpF)) >= 0) close(fd);
	if ((fd = mkstemp(logF)) >= 0) close(fd);

	CK(build(btpF, logF, 0, NULL, NULL, NULL) == 0);   /* NO checkpoint */
	/* Discard the built base; recover from the FULL log onto a fresh tree. */
	unlink(btpF);
	snprintf(dwF, sizeof dwF, "%s.dwb", btpF); unlink(dwF);
	CK(recover_full(btpF, logF, &bm, &bt) == 0);
	CK(verify_full(bt, N_ROWS) == 0);
	bt_close(bt); bt = NULL;
	bm_destroy(bm); bm = NULL;

	/* ============================================================
	 * Fuzzy checkpoint: build the base rows, fuzzy-checkpoint (flush the
	 * base durable, truncate the log below the redo horizon, mark clean),
	 * write a post-checkpoint tail, then recover in place from the
	 * truncated log + trusted base and assert (1) the log shrank and
	 * (2) the SAME full row set recovers.
	 * ============================================================ */
	t_tmpl(btpZ, sizeof btpZ, "fuzzy-ckp-bt");
	t_tmpl(logZ, sizeof logZ, "fuzzy-ckp-log");
	if ((fd = mkstemp(btpZ)) >= 0) close(fd);
	if ((fd = mkstemp(logZ)) >= 0) close(fd);

	CK(build(btpZ, logZ, 1, &horizon, &pre, &post) == 0);

	/* (1) TRUNCATION: at the checkpoint the log shrank sharply -- every
	 * record below the horizon (now durable on the flushed base) was
	 * dropped, leaving essentially just the checkpoint record. */
	CK(pre > 0 && post > 0);
	CK(post < pre / 2);            /* a large, not marginal, truncation */
	CK(horizon > 0);

	/* (2) EQUIVALENCE: recover in place from the truncated log + trusted
	 * base + post-checkpoint tail, and assert exactly the full-scan set. */
	CK(recover_inplace(btpZ, logZ, &bm, &bt, &pages) == 0);
	CK(verify_full(bt, N_ROWS) == 0);
	bt_close(bt); bt = NULL;
	bm_destroy(bm); bm = NULL;

	unlink(btpF); unlink(logF);
	unlink(btpZ); unlink(logZ);
	snprintf(dwZ, sizeof dwZ, "%s.dwb", btpZ); unlink(dwZ);
	unlink(dwF);

	if (g_fail) { fprintf(stderr, "test_fuzzy_checkpoint: FAILED\n"); return 1; }
	printf("  ok   fuzzy checkpoint truncated the log below the redo "
	    "horizon (LSN %llu): %lld -> %lld bytes (%.1fx smaller)\n",
	    (unsigned long long)horizon, (long long)pre, (long long)post,
	    pre / (double)(post ? post : 1));
	printf("  ok   in-place recovery from the horizon (trusted base + %d-row "
	    "post-checkpoint tail) restored the same %d committed rows as a "
	    "full-scan recovery, ordered and gap/dup-free\n", N_TAIL, N_ROWS);
	printf("test_fuzzy_checkpoint: O(dirty) fuzzy checkpoint carries the redo "
	    "horizon; recovery starts redo there over the trusted base and "
	    "matches a full-scan recovery exactly\n");
	return 0;
}
