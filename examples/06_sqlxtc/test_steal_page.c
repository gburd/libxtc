/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_steal_page.c
 *	GENUINE page-level STEAL, proven with buffer-manager evidence, and
 *	in-place crash recovery that undoes the STOLEN loser with CLRs.
 *
 *	The existing test_steal.c proves the spill mechanism is correct on
 *	commit / rollback / crash, but it runs on a pool large enough that
 *	NO page is ever evicted -- the uncommitted staged data stays
 *	resident, so it does not prove STEAL actually reached disk.  This
 *	test closes that gap by driving a transaction whose dirty set FAR
 *	exceeds a tiny buffer pool, then asserting on bm_get_stats():
 *
 *	  (a) STEAL happened for real -- evict_flushes > 0 -- the buffer
 *	      manager found no clean victim and WROTE dirty pages carrying
 *	      the uncommitted transaction's staged version data to the base
 *	      file, under the write-ahead-log hook (WAL-before-data holds).
 *
 *	  (b) Invisible to other readers -- the uncommitted staged rows
 *	      live under a reserved table-id no user scan reads, so a
 *	      concurrent connection sees only the committed set while the
 *	      big transaction is mid-flight with pages already on disk.
 *
 *	  (c) Removable by undo on crash -- crash mid-large-uncommitted
 *	      transaction, recover the torn/stolen base IN PLACE via
 *	      xstore_recover_inplace: the stolen loser is reversed via CLRs
 *	      (xstore_undo_clrs increments) and the base is left holding
 *	      exactly the committed set -- no stolen uncommitted row leaks.
 *
 *	  (d) Durability -- a committed large transaction whose pages were
 *	      stolen to disk survives the crash + in-place recovery whole.
 *
 *	Every recovery here runs on a torn/stolen base, so the process caps
 *	its own address space at 256 MB (setrlimit(RLIMIT_AS)) BEFORE any
 *	recovery, so a regression that reintroduced the unbounded-alloc
 *	torn-base balloon fails cleanly (malloc NULL) instead of OOM-killing
 *	the machine.  Relaxed only under AddressSanitizer (huge shadow map).
 *
 *	Low-level bt/bm harness (like test_recover_undo / test_inplace_redo)
 *	so the buffer-manager stats are directly observable; no loop; the
 *	WAL is driven synchronously (off-loop -> wal_commit_sync).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/resource.h>

#include "bufmgr.h"
#include "btree.h"
#include "xstore.h"
#include "wal.h"
#include "xlog.h"
#include "engine.h"
#include "xtc.h"
#include "t_tmp.h"

/*
 * Tiny pool, big payloads, many rows: a large transaction's staged
 * version pages cannot all stay resident, so the foreground eviction
 * path must flush dirty (uncommitted) pages to the base -- real STEAL.
 * PAGE_SZ 4096 with VLEN 200 keeps a version record inside one node.
 */
#define PAGE_SZ  4096
#define POOL     16                      /* 16 * 4096 = 64 KB resident cap */
#define NROW     6000                    /* NROW*VLEN ~ 1.2 MB >> pool */
#define VLEN     200
#define NCOMMIT  32                      /* small committed set that MUST survive */
#define AS_CAP   (256u * 1024u * 1024u)  /* 256 MB address-space cap */

static char g_val[VLEN + 1];

/* Count live rows of table t through a fresh native connection over bt.
 * (CREATE TABLE re-declares the schema for the connection; the rows are
 * the persisted versions in bt.) */
static int
count_rows(bt_t *bt)
{
	sx_db *db = NULL;
	sx_stmt *st = NULL;
	int n = -1;
	if (sx_open_bt(bt, &db) != SX_OK)
		return -1;
	(void)sx_exec(db, "CREATE TABLE t(k INTEGER PRIMARY KEY, v)", NULL);
	if (sx_prepare(db, "SELECT count(*) FROM t", -1, &st, NULL) == SX_OK) {
		if (sx_step(st) == SX_ROW)
			n = (int)sx_column_int64(st, 0);
		sx_finalize(st);
	}
	sx_close(db);
	return n;
}

static void
cap_address_space(void)
{
#if defined(__SANITIZE_ADDRESS__)
#  define XTC_STEAL_UNDER_ASAN 1
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer)
#    define XTC_STEAL_UNDER_ASAN 1
#  endif
#endif
#ifndef XTC_STEAL_UNDER_ASAN
	struct rlimit rl;
	if (getrlimit(RLIMIT_AS, &rl) == 0) {
		if (rl.rlim_cur == RLIM_INFINITY || rl.rlim_cur > (rlim_t)AS_CAP) {
			rl.rlim_cur = (rlim_t)AS_CAP;
			if (rl.rlim_max != RLIM_INFINITY && rl.rlim_max < (rlim_t)AS_CAP)
				rl.rlim_cur = rl.rlim_max;
			(void)setrlimit(RLIMIT_AS, &rl);
		}
	}
#endif
}

/* Insert [base, base+n) big rows through db; wrap in BEGIN..fin when
 * fin != NULL (COMMIT / ROLLBACK); leave the txn OPEN when fin == NULL
 * (the caller crashes). */
static int
big_txn(sx_db *db, int base, int n, const char *fin)
{
	int i;
	(void)sx_exec(db, "BEGIN;", NULL);
	for (i = 0; i < n; i++) {
		char sql[VLEN + 64];
		snprintf(sql, sizeof sql, "INSERT INTO t(k,v) VALUES(%d,'%s');",
		    base + i, g_val);
		if (sx_exec(db, sql, NULL) != SX_OK) {
			fprintf(stderr, "insert %d failed\n", base + i);
			return -1;
		}
	}
	if (fin != NULL) {
		char b[16];
		snprintf(b, sizeof b, "%s;", fin);
		if (sx_exec(db, b, NULL) != SX_OK)
			return -1;
	}
	return 0;
}

/* Open a fresh base+log at (btp, logp) with the tiny pool and the WAL
 * hook installed the way the live engine installs it, so eviction is
 * WAL-gated.  Out params return the created handles. */
static int
open_fresh(const char *btp, const char *logp, bm_t **bmo, bt_t **bto,
    wal_t **walo)
{
	bm_opts_t bo = BM_OPTS_DEFAULT;
	wal_opts_t wo;
	bm_t *bm = NULL;
	bt_t *bt = NULL;
	wal_t *wal = NULL;

	memset(&wo, 0, sizeof wo);
	wo.path = logp; wo.window_ns = 0; wo.max_batch = 1;   /* synchronous */
	if (wal_open(&wo, &wal) != XTC_OK)
		return -1;
	bo.path = btp; bo.page_size = PAGE_SZ; bo.n_frames = POOL;
	bo.lsn_off = 0;             /* ARIES page LSN at the node front */
	bo.double_write = 1;        /* torn-home protection for the base */
	if (bm_create(&bo, &bm) != XTC_OK) { wal_close(wal); return -1; }
	if (bt_open(bm, &bt) != XTC_OK) { bm_destroy(bm); wal_close(wal); return -1; }
	xstore_set_wal((struct wal *)wal);
	*bmo = bm; *bto = bt; *walo = wal;
	return 0;
}

/* Reopen the base in place (reopen=1) and recover from the log with
 * xstore_recover_inplace, returning the CLRs written and pages redone. */
static int
reopen_recover_inplace(const char *btp, const char *logp, bm_t **bmo,
    bt_t **bto, uint64_t *pages)
{
	bm_opts_t bo = BM_OPTS_DEFAULT;
	wal_opts_t wo;
	bm_t *bm = NULL;
	bt_t *bt = NULL;
	wal_t *w = NULL;

	bo.path = btp; bo.page_size = PAGE_SZ; bo.n_frames = POOL;
	bo.lsn_off = 0; bo.double_write = 1; bo.reopen = 1;
	if (bm_create(&bo, &bm) != XTC_OK)
		return -1;
	if (bt_reopen(bm, &bt) != XTC_OK) { bm_destroy(bm); return -1; }

	memset(&wo, 0, sizeof wo);
	wo.path = logp; wo.append = 1;
	if (wal_open(&wo, &w) != XTC_OK) { bt_close(bt); bm_destroy(bm); return -1; }
	xstore_set_wal((struct wal *)w);        /* CLRs append to the same log */
	if (xstore_recover_inplace(bt, bm, logp, pages) != XTC_OK) {
		xstore_set_wal(NULL); wal_close(w);
		bt_close(bt); bm_destroy(bm);
		return -1;
	}
	xstore_set_wal(NULL);
	wal_close(w);
	(void)bm_checkpoint(bm);
	*bmo = bm; *bto = bt;
	return 0;
}

int
main(void)
{
	char btp[256], logp[256], dwp[288];
	bm_t *bm = NULL;
	bt_t *bt = NULL;
	wal_t *wal = NULL;
	sx_db *db = NULL;
	bm_stats_t st;
	uint64_t clrs0, pages = 0;
	int fd;

	cap_address_space();      /* MANDATORY: torn-base recovery below */

	memset(g_val, 'x', VLEN); g_val[VLEN] = '\0';
	t_tmpl(btp, sizeof btp, "steal-page-bt");
	t_tmpl(logp, sizeof logp, "steal-page-log");
	if ((fd = mkstemp(btp)) >= 0) close(fd);
	if ((fd = mkstemp(logp)) >= 0) close(fd);

	/* =============================================================
	 * (a)+(b)+(c): crash mid-large-UNCOMMITTED transaction whose pages
	 * are stolen to disk; recover in place; undo the stolen loser.
	 * ============================================================= */
	CK(open_fresh(btp, logp, &bm, &bt, &wal) == 0);
	CK(sx_open_bt(bt, &db) == SX_OK);
	CK(sx_exec(db, "CREATE TABLE t(k INTEGER PRIMARY KEY, v)", NULL) == SX_OK);

	/* A committed baseline that MUST survive. */
	CK(big_txn(db, 0, NCOMMIT, "COMMIT") == 0);

	/* The large uncommitted transaction: distinct key range, never
	 * commits -> a loser whose staged pages the pool evicts to disk. */
	clrs0 = xstore_undo_clrs();
	CK(big_txn(db, 100000, NROW, NULL) == 0);

	/* (a) STEAL happened for real: dirty (uncommitted) pages were
	 *     force-flushed to the base because no clean victim was free. */
	bm_get_stats(bm, &st);
	CK(st.evict_flushes > 0);
	printf("  ok   real STEAL: %llu dirty pages force-flushed to disk under a "
	    "%d-frame pool (%llu evictions) -- uncommitted data genuinely stolen\n",
	    (unsigned long long)st.evict_flushes, POOL,
	    (unsigned long long)st.evicted);

	/* (b) invisible to a concurrent reader: a second connection over the
	 *     SAME bt (its pages already partly on disk) sees only the
	 *     committed baseline, never the mid-flight uncommitted rows. */
	CK(count_rows(bt) == NCOMMIT);
	printf("  ok   invisible: a concurrent scan sees %d committed rows, none "
	    "of the %d mid-flight uncommitted (stolen) rows\n", NCOMMIT, NROW);

	/* CRASH: drop everything with no commit, no clean checkpoint. */
	sx_close(db); db = NULL;
	xstore_set_wal(NULL);
	bt_close(bt); bt = NULL;
	bm_destroy(bm); bm = NULL;
	wal_close(wal); wal = NULL;

	/* (c) recover the torn/stolen base IN PLACE: undo the stolen loser. */
	CK(reopen_recover_inplace(btp, logp, &bm, &bt, &pages) == 0);
	CK(xstore_undo_clrs() > clrs0);          /* stolen loser reversed via CLRs */
	CK(count_rows(bt) == NCOMMIT);           /* only the committed set remains */
	printf("  ok   in-place undo of stolen loser: %llu CLRs reversed the "
	    "stolen uncommitted rows; base holds exactly %d committed rows "
	    "(%llu page images applied)\n",
	    (unsigned long long)(xstore_undo_clrs() - clrs0), NCOMMIT,
	    (unsigned long long)pages);

	bt_close(bt); bt = NULL;
	bm_destroy(bm); bm = NULL;
	unlink(btp); unlink(logp);
	snprintf(dwp, sizeof dwp, "%s.dwb", btp); unlink(dwp);

	/* =============================================================
	 * (d): durability -- a COMMITTED large transaction whose pages were
	 * stolen to disk survives crash + in-place recovery WHOLE.
	 * ============================================================= */
	t_tmpl(btp, sizeof btp, "steal-page-bt2");
	t_tmpl(logp, sizeof logp, "steal-page-log2");
	if ((fd = mkstemp(btp)) >= 0) close(fd);
	if ((fd = mkstemp(logp)) >= 0) close(fd);

	CK(open_fresh(btp, logp, &bm, &bt, &wal) == 0);
	CK(sx_open_bt(bt, &db) == SX_OK);
	CK(sx_exec(db, "CREATE TABLE t(k INTEGER PRIMARY KEY, v)", NULL) == SX_OK);
	CK(big_txn(db, 0, NROW, "COMMIT") == 0);   /* one large committed txn */
	bm_get_stats(bm, &st);
	CK(st.evict_flushes > 0);                  /* its pages were stolen too */

	/* CRASH after commit is durable in the log, before a clean checkpoint. */
	sx_close(db); db = NULL;
	xstore_set_wal(NULL);
	bt_close(bt); bt = NULL;
	bm_destroy(bm); bm = NULL;
	wal_close(wal); wal = NULL;

	CK(reopen_recover_inplace(btp, logp, &bm, &bt, &pages) == 0);
	CK(count_rows(bt) == NROW);                /* every committed row survives */
	printf("  ok   durability: committed large txn (%llu pages stolen) "
	    "survives crash + in-place recovery whole: all %d rows present\n",
	    (unsigned long long)st.evict_flushes, NROW);

	bt_close(bt);
	bm_destroy(bm);
	unlink(btp); unlink(logp);
	snprintf(dwp, sizeof dwp, "%s.dwb", btp); unlink(dwp);

	if (g_fail) { fprintf(stderr, "test_steal_page: FAILED\n"); return 1; }
	printf("test_steal_page: genuine page-level STEAL (evict_flushes>0); "
	    "stolen loser undone in place via CLRs; committed txns durable\n");
	return 0;
}
