/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_wal_recover.c
 *	WAL durability + crash recovery for the xstore SQL engine.
 *
 *	Phase 1 commits transactions through the group-commit WAL on a
 *	loop -- explicit multi-row transactions (the buffered commit path)
 *	and autocommit single-row inserts (the immediate path), both
 *	logged durably before they touch the B-tree.  Then it simulates a
 *	crash: the buffer pool is destroyed WITHOUT flushing, and the pool
 *	is large enough that nothing was ever evicted, so the B-tree data
 *	file never received a single page -- total loss of the materialized
 *	data, with only the WAL durable.
 *
 *	Phase 2 recovers: a FRESH, empty B-tree is opened and
 *	xstore_recover replays the WAL into it.  Every committed row must
 *	reappear, reconstructed from the log alone.  This is the NO-FORCE
 *	property of a WAL -- commits are durable via the log without
 *	forcing data pages -- and redo-only recovery.  No daemon.
 */

#include <stdatomic.h>
#include <stdint.h>
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
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "t_tmp.h"

#define N_TXN     40          /* explicit transactions, 2 rows each */
#define N_AUTO    20          /* autocommit single-row inserts */
#define N_T2      5           /* rows in a second table (catalog recovery) */
#define PAGE_SZ   4096

static wal_t      *g_wal;
static bt_t       *g_bt1;
static xsql       *g_db1;
static _Atomic int g_phase1;          /* 0 unset, 1 ok, -1 fail */

static int
sel_v(xsql *db, const char *tbl, int64_t k, char *out, size_t cap)
{
	xsql_stmt *st = NULL;
	char q[64];
	int got = 0;
	snprintf(q, sizeof q, "SELECT v FROM %s WHERE k=?", tbl);
	if (xsql_prepare_v2(db, q, -1, &st, 0) != SQLITE_OK)
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

/* Phase 1: commit durably, then stop the writer so the loop drains. */
static void
worker(void *arg)
{
	int ok = 1, k;
	char sql[128];
	(void)arg;

	for (k = 0; ok && k < N_TXN; k++) {
		if (xsql_exec(g_db1, "BEGIN;", 0, 0, 0) != SQLITE_OK) { ok = 0; break; }
		snprintf(sql, sizeof sql, "INSERT INTO t(k,v) VALUES(%d,'tx-%d');", 1000 + k, k);
		if (xsql_exec(g_db1, sql, 0, 0, 0) != SQLITE_OK) { ok = 0; break; }
		snprintf(sql, sizeof sql, "INSERT INTO t(k,v) VALUES(%d,'tx-%d');", 2000 + k, k);
		if (xsql_exec(g_db1, sql, 0, 0, 0) != SQLITE_OK) { ok = 0; break; }
		if (xsql_exec(g_db1, "COMMIT;", 0, 0, 0) != SQLITE_OK) { ok = 0; break; }
	}
	for (k = 0; ok && k < N_AUTO; k++) {
		snprintf(sql, sizeof sql, "INSERT INTO t(k,v) VALUES(%d,'au-%d');", 3000 + k, k);
		if (xsql_exec(g_db1, sql, 0, 0, 0) != SQLITE_OK) { ok = 0; break; }
	}
	/* A second table whose rowids OVERLAP t's (1000..) but live in a
	 * distinct table-id: recovery must restore the catalog mapping for
	 * BOTH names or the two tables' rows would alias. */
	for (k = 0; ok && k < N_T2; k++) {
		snprintf(sql, sizeof sql, "INSERT INTO t2(k,v) VALUES(%d,'t2-%d');", 1000 + k, k);
		if (xsql_exec(g_db1, sql, 0, 0, 0) != SQLITE_OK) { ok = 0; break; }
	}
	atomic_store(&g_phase1, ok ? 1 : -1);
	(void)wal_writer_stop(g_wal);
}

int
main(void)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t po = { .name = "rec-worker" };
	xtc_pid_t w;
	wal_opts_t wo = { 0 };
	bm_opts_t bo = BM_OPTS_DEFAULT, b2 = BM_OPTS_DEFAULT;
	bm_t *bm1 = NULL, *bm2 = NULL;
	bt_t *bt2 = NULL;
	xsql *db2 = NULL;
	char logp[256]; t_tmpl(logp, sizeof logp, "sqlxtc-rec-log");
	char btA[256]; t_tmpl(btA, sizeof btA, "sqlxtc-rec-A");
	char btB[256]; t_tmpl(btB, sizeof btB, "sqlxtc-rec-B");
	char b[32], want[16];
	int fd, k, found = 0, miss = 0, expected = N_TXN * 2 + N_AUTO + N_T2;

	fd = mkstemp(logp); if (fd < 0) return 1; close(fd);
	fd = mkstemp(btA);  if (fd < 0) return 1; close(fd);
	fd = mkstemp(btB);  if (fd < 0) return 1; close(fd);

	/* ---- phase 1: durable commits through the WAL on a loop ---- */
	wo.path = logp; wo.window_ns = 500000; wo.max_batch = 256;
	if (wal_open(&wo, &g_wal) != XTC_OK) return 1;
	/* Pool large enough that NOTHING is evicted -> no page ever reaches
	 * the data file; the WAL is the only durable copy. */
	bo.path = btA; bo.page_size = PAGE_SZ; bo.n_frames = 512;
	if (bm_create(&bo, &bm1) != XTC_OK) return 1;
	if (bt_open(bm1, &g_bt1) != XTC_OK) return 1;
	xstore_set_wal((struct wal *)g_wal);
	if (xsql_open(":memory:", &g_db1) != SQLITE_OK) return 1;
	if (xstore_register(g_db1, g_bt1) != SQLITE_OK) return 1;
	if (xsql_exec(g_db1, "CREATE VIRTUAL TABLE t USING xstore;", 0, 0, 0) != SQLITE_OK)
		return 1;
	if (xsql_exec(g_db1, "CREATE VIRTUAL TABLE t2 USING xstore;", 0, 0, 0) != SQLITE_OK)
		return 1;

	if (xtc_loop_init(&loop) != XTC_OK) return 1;
	if (wal_writer_spawn(g_wal, loop, NULL) != XTC_OK) return 1;
	if (xtc_proc_spawn(loop, worker, NULL, &po, &w) != XTC_OK) return 1;
	if (xtc_loop_run(loop) != XTC_OK) return 1;
	(void)xtc_loop_fini(loop);
	if (atomic_load(&g_phase1) != 1) {
		fprintf(stderr, "FAIL: phase 1 commits failed\n");
		return 1;
	}

	/* ---- crash: lose the pool WITHOUT flushing (data file A empty) ---- */
	xsql_close(g_db1);
	xstore_set_wal(NULL);
	bt_close(g_bt1);
	bm_destroy(bm1);                 /* every dirty page is lost */
	wal_close(g_wal);

	/* ---- restart + recover into a FRESH empty B-tree from the WAL ---- */
	b2.path = btB; b2.page_size = PAGE_SZ; b2.n_frames = 64;
	if (bm_create(&b2, &bm2) != XTC_OK) return 1;
	if (bt_open(bm2, &bt2) != XTC_OK) return 1;
	if (xstore_recover(bt2, logp) != XTC_OK) {
		fprintf(stderr, "FAIL: xstore_recover\n");
		return 1;
	}
	if (xsql_open(":memory:", &db2) != SQLITE_OK) return 1;
	if (xstore_register(db2, bt2) != SQLITE_OK) return 1;
	if (xsql_exec(db2, "CREATE VIRTUAL TABLE t USING xstore;", 0, 0, 0) != SQLITE_OK)
		return 1;
	if (xsql_exec(db2, "CREATE VIRTUAL TABLE t2 USING xstore;", 0, 0, 0) != SQLITE_OK)
		return 1;

	for (k = 0; k < N_TXN; k++) {
		snprintf(want, sizeof want, "tx-%d", k);
		if (sel_v(db2, "t", 1000 + k, b, sizeof b) == 1 && strcmp(b, want) == 0) found++; else miss++;
		if (sel_v(db2, "t", 2000 + k, b, sizeof b) == 1 && strcmp(b, want) == 0) found++; else miss++;
	}
	for (k = 0; k < N_AUTO; k++) {
		snprintf(want, sizeof want, "au-%d", k);
		if (sel_v(db2, "t", 3000 + k, b, sizeof b) == 1 && strcmp(b, want) == 0) found++; else miss++;
	}
	/* t2's rows must recover under their own table-id: rowids 1000..
	 * exist in BOTH t and t2 with different values, so any catalog
	 * mix-up shows up as a wrong value here. */
	for (k = 0; k < N_T2; k++) {
		snprintf(want, sizeof want, "t2-%d", k);
		if (sel_v(db2, "t2", 1000 + k, b, sizeof b) == 1 && strcmp(b, want) == 0) found++; else miss++;
	}

	xsql_close(db2); bt_close(bt2); bm_destroy(bm2);
	unlink(logp); unlink(btA); unlink(btB);

	if (miss != 0 || found != expected) {
		fprintf(stderr, "FAIL: recovered %d/%d rows (%d missing/wrong)\n",
		    found, expected, miss);
		return 1;
	}
	printf("  ok   WAL recovery: %d rows across two tables (%d explicit-txn"
	    " + %d autocommit in t, %d in t2 with overlapping rowids)"
	    " reconstructed from the log after the entire buffer pool was lost"
	    " unflushed; the catalog restored both table-ids by name\n",
	    found, N_TXN * 2, N_AUTO, N_T2);
	printf("All sqlxtc WAL recovery tests passed.\n");
	return 0;
}
