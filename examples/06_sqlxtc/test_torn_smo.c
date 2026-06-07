/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_torn_smo.c
 *	Adversarial torn-STRUCTURE crash recovery.
 *
 *	The double-write buffer makes each page write atomic, but a crash
 *	mid structure-modification (a split that flushed some of its pages
 *	to the data file but not others) can still leave the on-disk tree
 *	structurally torn: a parent referencing a child that was never
 *	flushed, or a stale parent that does not yet point at a flushed
 *	child.  This test builds a multi-level B-tree with many splits
 *	under a TINY buffer pool, so inner AND leaf pages are constantly
 *	demand-evicted to the data file mid-workload -- the on-disk image
 *	is a partial, evolving, structurally inconsistent tree at every
 *	instant.  It then simulates a crash (the pool is dropped WITHOUT a
 *	checkpoint, losing every still-dirty page) and recovers: reopen
 *	the partial base, replay the WAL, checkpoint.
 *
 *	Claim under test (the engine's recovery model, M_SQLXTC_WAL.md):
 *	because versions are immutable and append-only and logical redo is
 *	idempotent, replaying the whole post-checkpoint log onto the torn
 *	base reconstructs a correct, complete tree.  Every committed row
 *	must reappear, and a full ordered scan must return EXACTLY the
 *	inserted keys in order (catching a missing, duplicated, lost, or
 *	misordered row -- the symptoms of structural corruption).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bufmgr.h"
#include "btree.h"
#include "xstore.h"
#include "wal.h"
#include "sqlite3.h"
#include "xtc.h"
#include "t_tmp.h"

#define N_ROWS   1000
#define PAGE_SZ  512          /* tiny -> shallow fan-out -> deep tree, many splits */
#define POOL     16           /* tiny -> inner + leaf pages constantly evicted */

static int g_fail;
#define CK(c) do { if (!(c)) { \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); \
	g_fail = 1; } } while (0)

static int
eval_int(xsql *db, const char *sql)
{
	xsql_stmt *st = NULL;
	int v = -1;
	if (xsql_prepare_v2(db, sql, -1, &st, 0) != SQLITE_OK)
		return -1;
	if (xsql_step(st) == SQLITE_ROW)
		v = xsql_column_int(st, 0);
	xsql_finalize(st);
	return v;
}

/* Read v for key k; return 1 + fill out on hit, 0 on miss. */
static int
sel_v(xsql *db, int k, char *out, size_t cap)
{
	xsql_stmt *st = NULL;
	int got = 0;
	if (xsql_prepare_v2(db, "SELECT v FROM t WHERE k=?", -1, &st, 0) != SQLITE_OK)
		return -1;
	xsql_bind_int64(st, 1, k);
	if (xsql_step(st) == SQLITE_ROW) {
		const unsigned char *t = xsql_column_text(st, 0);
		size_t n = (size_t)xsql_column_bytes(st, 0);
		if (n >= cap) n = cap - 1;
		if (t) memcpy(out, t, n);
		out[n] = '\0';
		got = 1;
	}
	xsql_finalize(st);
	return got;
}

int
main(void)
{
	wal_t *wal = NULL;
	bm_opts_t bo = BM_OPTS_DEFAULT, b2 = BM_OPTS_DEFAULT;
	bm_t *bm1 = NULL, *bm2 = NULL;
	bt_t *bt1 = NULL, *bt2 = NULL;
	xsql *db1 = NULL, *db2 = NULL;
	wal_opts_t wo = { 0 };
	char btp[256]; t_tmpl(btp, sizeof btp, "sqlxtc-tsmo");
	char logp[256]; t_tmpl(logp, sizeof logp, "sqlxtc-tsmo-log");
	char dwp[288];
	char want[32], got[32];
	xsql_stmt *st = NULL;
	int fd, i, miss = 0, scanned = 0, prev = -1, ordered = 1;

	fd = mkstemp(btp);  if (fd < 0) return 1; close(fd);
	fd = mkstemp(logp); if (fd < 0) return 1; close(fd);

	/* ---- phase 1: build a deep tree under a tiny pool, log durably ---- */
	wo.path = logp; wo.window_ns = 0; wo.max_batch = 1;
	if (wal_open(&wo, &wal) != XTC_OK) return 1;
	bo.path = btp; bo.page_size = PAGE_SZ; bo.n_frames = POOL;
	bo.cool_pct = 25; bo.double_write = 1;
	if (bm_create(&bo, &bm1) != XTC_OK) return 1;
	if (bt_open(bm1, &bt1) != XTC_OK) return 1;
	xstore_set_wal((struct wal *)wal);
	if (xsql_open(":memory:", &db1) != SQLITE_OK) return 1;
	if (xstore_register(db1, bt1) != SQLITE_OK) return 1;
	if (xsql_exec(db1, "CREATE VIRTUAL TABLE t USING xstore(k, v);",
	    0, 0, 0) != SQLITE_OK) return 1;

	/* Each INSERT is an autocommit transaction: durable via wal_commit_sync
	 * (off a loop), then bt_insert -- which splits as the tree grows and
	 * demand-evicts victims to the data file because the pool is tiny. */
	for (i = 0; i < N_ROWS; i++) {
		char sql[64];
		snprintf(sql, sizeof sql, "INSERT INTO t(k,v) VALUES(%d,'val-%d');",
		    i, i);
		if (xsql_exec(db1, sql, 0, 0, 0) != SQLITE_OK) {
			fprintf(stderr, "FAIL: insert %d\n", i);
			return 1;
		}
	}

	/* ---- crash: drop the pool WITHOUT a checkpoint.  Every dirty page
	 * is lost; only pages demand-evicted during the workload (a partial,
	 * structurally torn tree) survive on the data file.  The WAL holds
	 * every commit. ---- */
	xsql_close(db1);
	xstore_set_wal(NULL);
	bt_close(bt1);
	bm_destroy(bm1);
	wal_close(wal);

	/* ---- restart: reopen the PARTIAL base, replay the WAL onto it,
	 * then checkpoint the recovered state durable. ---- */
	/* Unclean shutdown (the WAL is non-empty): the on-disk tree may be
	 * structurally torn by partial mid-SMO eviction, so DISCARD it
	 * (reopen=0 truncates the page file) and rebuild onto a fresh tree
	 * by replaying the full WAL -- the engine's unclean-reopen policy. */
	b2.path = btp; b2.page_size = PAGE_SZ; b2.n_frames = POOL;
	b2.cool_pct = 25; b2.reopen = 0; b2.double_write = 1;
	if (bm_create(&b2, &bm2) != XTC_OK) { fprintf(stderr, "FAIL: reopen bm\n"); return 1; }
	if (bt_open(bm2, &bt2) != XTC_OK) { fprintf(stderr, "FAIL: bt_open\n"); return 1; }
	if (xstore_recover(bt2, logp) != XTC_OK) { fprintf(stderr, "FAIL: recover\n"); return 1; }
	if (bm_checkpoint(bm2) != XTC_OK) { fprintf(stderr, "FAIL: checkpoint\n"); return 1; }

	if (xsql_open(":memory:", &db2) != SQLITE_OK) return 1;
	if (xstore_register(db2, bt2) != SQLITE_OK) return 1;
	if (xsql_exec(db2, "CREATE VIRTUAL TABLE t USING xstore(k, v);",
	    0, 0, 0) != SQLITE_OK) return 1;

	/* (a) the count is exactly right. */
	CK(eval_int(db2, "SELECT count(*) FROM t") == N_ROWS);

	/* (b) every key reads back its own value. */
	for (i = 0; i < N_ROWS; i++) {
		snprintf(want, sizeof want, "val-%d", i);
		if (sel_v(db2, i, got, sizeof got) == 1 && strcmp(got, want) == 0)
			continue;
		miss++;
	}
	CK(miss == 0);

	/* (c) a full ordered scan returns EXACTLY the inserted keys, in order
	 * and without gaps or duplicates -- the structural integrity check. */
	if (xsql_prepare_v2(db2, "SELECT k FROM t ORDER BY k", -1, &st, 0)
	    == SQLITE_OK) {
		while (xsql_step(st) == SQLITE_ROW) {
			int k = xsql_column_int(st, 0);
			if (k <= prev) ordered = 0;     /* gap, dup, or misorder */
			prev = k;
			scanned++;
		}
		xsql_finalize(st);
	}
	CK(ordered == 1);
	CK(scanned == N_ROWS);
	CK(prev == N_ROWS - 1);

	xsql_close(db2); bt_close(bt2); bm_destroy(bm2);
	unlink(btp); unlink(logp);
	snprintf(dwp, sizeof dwp, "%s.dwb", btp); unlink(dwp);

	if (g_fail) {
		fprintf(stderr, "FAIL: torn-structure recovery (miss=%d scanned=%d "
		    "ordered=%d)\n", miss, scanned, ordered);
		return 1;
	}
	printf("  ok   torn-structure recovery: %d rows in a multi-level tree "
	    "(page=%d, pool=%d) survived a crash with constant mid-SMO eviction; "
	    "logical redo onto the partial base rebuilt a correct ordered tree\n",
	    N_ROWS, PAGE_SZ, POOL);
	printf("All sqlxtc torn-structure recovery tests passed.\n");
	return 0;
}
