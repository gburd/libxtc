/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_wal_compact.c
 *	In-WAL checkpoint: bounded log for an always-on server.
 *
 *	The write-ahead log is the source of truth and recovery rebuilds
 *	the tree by replaying it.  Left alone the log would grow with the
 *	whole write history, so an always-on server would never bound its
 *	recovery time or disk use.  The in-WAL checkpoint
 *	(xstore_checkpoint_wal) fixes that: it atomically rewrites the log
 *	as a CHECKPOINT record plus a dump of the LIVE row set, discarding
 *	every superseded version and tombstone.  The checkpoint lives in
 *	the log itself -- no side files.
 *
 *	This test writes N rows and then UPDATEs every one of them K times
 *	(heavy churn: the log holds N*(K+1) records), checkpoints, and
 *	asserts the log shrank to roughly the live row count -- proof the
 *	log is bounded by live data, not by history.  It then writes more,
 *	simulates a crash (drop the tree, keep the log), and recovers:
 *	every row -- the compacted base plus the post-checkpoint tail --
 *	must come back with its latest value.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bufmgr.h"
#include "btree.h"
#include "xstore.h"
#include "wal.h"
#include "engine.h"
#include "xtc.h"
#include "t_tmp.h"

#define N_ROWS   200
#define N_UPD    10           /* updates per row -> heavy version churn */
#define N_TAIL   50           /* rows written AFTER the checkpoint */
#define PAGE_SZ  4096


static off_t
fsize(const char *p)
{
	struct stat st;
	return stat(p, &st) == 0 ? st.st_size : -1;
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
		const unsigned char *t = sx_column_text(st, 0);
		size_t n = (size_t)sx_column_bytes(st, 0);
		if (n >= cap) n = cap - 1;
		if (t) memcpy(out, t, n);
		out[n] = '\0';
		got = 1;
	}
	sx_finalize(st);
	return got;
}

static int
count_rows(sx_db *db)
{
	sx_stmt *st = NULL;
	int n = -1;
	if (sx_prepare(db, "SELECT count(*) FROM t", -1, &st, NULL) != SX_OK)
		return -1;
	if (sx_step(st) == SX_ROW) n = (int)sx_column_int64(st, 0);
	sx_finalize(st);
	return n;
}

int
main(void)
{
	wal_t *wal = NULL;
	bm_opts_t bo = BM_OPTS_DEFAULT, b2 = BM_OPTS_DEFAULT;
	bm_t *bm1 = NULL, *bm2 = NULL;
	bt_t *bt1 = NULL, *bt2 = NULL;
	sx_db *db1 = NULL, *db2 = NULL;
	wal_opts_t wo = { 0 };
	char btp[256]; t_tmpl(btp, sizeof btp, "sqlxtc-cmp");
	char logp[256]; t_tmpl(logp, sizeof logp, "sqlxtc-cmp-log");
	char dwp[288], b[32], want[32];
	off_t w_churn, w_compact;
	int fd, i, u, miss = 0;

	fd = mkstemp(btp);  if (fd < 0) return 1; close(fd);
	fd = mkstemp(logp); if (fd < 0) return 1; close(fd);

	/* ---- build + churn ---- */
	wo.path = logp; wo.window_ns = 0; wo.max_batch = 1; wo.append = 0;
	if (wal_open(&wo, &wal) != XTC_OK) return 1;
	bo.path = btp; bo.page_size = PAGE_SZ; bo.n_frames = 256;
	bo.cool_pct = 25; bo.double_write = 1;
	if (bm_create(&bo, &bm1) != XTC_OK) return 1;
	if (bt_open(bm1, &bt1) != XTC_OK) return 1;
	xstore_set_wal((struct wal *)wal);
	if (sx_open_bt(bt1, &db1) != SX_OK) return 1;
	if (sx_exec(db1, "CREATE TABLE t(k INTEGER PRIMARY KEY, v)", NULL) != SX_OK) return 1;

	for (i = 0; i < N_ROWS; i++) {
		char sql[64];
		snprintf(sql, sizeof sql, "INSERT INTO t(k,v) VALUES(%d,'v-%d-0');", i, i);
		CK(sx_exec(db1, sql, NULL) == SX_OK);
	}
	for (u = 1; u <= N_UPD; u++) {
		for (i = 0; i < N_ROWS; i++) {
			char sql[64];
			snprintf(sql, sizeof sql,
			    "UPDATE t SET v='v-%d-%d' WHERE k=%d;", i, u, i);
			CK(sx_exec(db1, sql, NULL) == SX_OK);
		}
	}
	w_churn = fsize(logp);                       /* log holds N*(N_UPD+1) records */

	/* ---- in-WAL checkpoint: compact the log to the live set ---- */
	CK(xstore_checkpoint_wal(bt1, (struct wal *)wal, logp) == XTC_OK);
	w_compact = fsize(logp);

	/* The compacted log must be a fraction of the churned log: it holds
	 * ~N live rows + one checkpoint record, not the N*(N_UPD+1) history. */
	CK(w_compact > 0 && w_compact < w_churn / 2);

	/* Data is intact through the checkpoint (latest value per row). */
	for (i = 0; i < N_ROWS; i++) {
		snprintf(want, sizeof want, "v-%d-%d", i, N_UPD);
		if (sel_v(db1, i, b, sizeof b) == 1 && strcmp(b, want) == 0) continue;
		miss++;
	}
	CK(miss == 0);

	/* ---- write a post-checkpoint tail, then crash (keep the log) ---- */
	for (i = N_ROWS; i < N_ROWS + N_TAIL; i++) {
		char sql[64];
		snprintf(sql, sizeof sql, "INSERT INTO t(k,v) VALUES(%d,'tail-%d');", i, i);
		CK(sx_exec(db1, sql, NULL) == SX_OK);
	}
	sx_close(db1);
	xstore_set_wal(NULL);
	bt_close(bt1);
	bm_destroy(bm1);                             /* lose the tree (crash) */
	wal_close(wal);

	/* ---- recover from the compacted log + tail onto a fresh tree ---- */
	b2.path = btp; b2.page_size = PAGE_SZ; b2.n_frames = 256;
	b2.cool_pct = 25; b2.reopen = 0; b2.double_write = 1;
	if (bm_create(&b2, &bm2) != XTC_OK) return 1;
	if (bt_open(bm2, &bt2) != XTC_OK) return 1;
	if (xstore_recover(bt2, logp) != XTC_OK) return 1;
	if (sx_open_bt(bt2, &db2) != SX_OK) return 1;
	if (sx_exec(db2, "CREATE TABLE t(k INTEGER PRIMARY KEY, v)", NULL) != SX_OK) return 1;

	CK(count_rows(db2) == N_ROWS + N_TAIL);
	miss = 0;
	for (i = 0; i < N_ROWS; i++) {               /* compacted base: latest value */
		snprintf(want, sizeof want, "v-%d-%d", i, N_UPD);
		if (sel_v(db2, i, b, sizeof b) == 1 && strcmp(b, want) == 0) continue;
		miss++;
	}
	for (i = N_ROWS; i < N_ROWS + N_TAIL; i++) {  /* post-checkpoint tail */
		snprintf(want, sizeof want, "tail-%d", i);
		if (sel_v(db2, i, b, sizeof b) == 1 && strcmp(b, want) == 0) continue;
		miss++;
	}
	CK(miss == 0);

	sx_close(db2); bt_close(bt2); bm_destroy(bm2);
	unlink(btp); unlink(logp);
	snprintf(dwp, sizeof dwp, "%s.dwb", btp); unlink(dwp);

	if (g_fail) return 1;
	printf("  ok   in-WAL checkpoint: %d rows x %d updates churned the log to "
	    "%lld bytes; compaction bounded it to %lld (%.1fx smaller); recovery "
	    "from the compacted log + %d-row tail restored every row\n",
	    N_ROWS, N_UPD, (long long)w_churn, (long long)w_compact,
	    w_churn / (double)(w_compact ? w_compact : 1), N_TAIL);
	printf("All sqlxtc WAL-compaction tests passed.\n");
	return 0;
}
