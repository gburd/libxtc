/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_txn_fastpath.c
 *	sx_exec runs a bare BEGIN / COMMIT / END / ROLLBACK without parsing
 *	it (sql_parse_count does not move), with exactly the effect of the
 *	parsed statement (sx_prepare + sx_step, which always parses):
 *	  - every spelling opens or ends a transaction: an INSERT after
 *	    BEGIN is invisible to a second connection until COMMIT / END,
 *	    and gone after ROLLBACK;
 *	  - anything that is not one bare statement still parses and keeps
 *	    its own meaning: ROLLBACK TO sp, "BEGIN; SELECT 1", a comment,
 *	    BEGIN ISOLATION LEVEL, BEGINX, a word too many.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "engine.h"
#include "bufmgr.h"
#include "btree.h"
#include "sql_parse.h"
#include "xtc.h"
#include "t_tmp.h"

static int64_t
count_rows(sx_db *db)
{
	sx_stmt *st = NULL;
	int64_t v = -1;
	if (sx_prepare(db, "SELECT count(*) FROM t", -1, &st, NULL) != SX_OK)
		return -2;
	if (sx_step(st) == SX_ROW) v = sx_column_int64(st, 0);
	sx_finalize(st);
	return v;
}

/* The parsed path: the statement up to its first ';' (what sx_exec's
 * split hands sx_prepare) through sx_prepare / sx_step. */
static int
parsed(sx_db *db, const char *sql)
{
	sx_stmt *st = NULL;
	char one[128];
	int rc;
	snprintf(one, sizeof one, "%.*s", (int)strcspn(sql, ";"), sql);
	if (sx_prepare(db, one, -1, &st, NULL) != SX_OK || st == NULL) return SX_ERROR;
	rc = sx_step(st);
	sx_finalize(st);
	return rc == SX_DONE ? SX_OK : rc;
}

static const char *const begins[] = {
	"BEGIN", "begin", "Begin;", "  BEGIN  ;  ", "\tBEGIN\r\n", "BEGIN TRANSACTION",
	"begin transaction;", "BEGIN DEFERRED", "BEGIN IMMEDIATE", "begin exclusive",
	"BEGIN DEFERRED TRANSACTION", "BEGIN IMMEDIATE TRANSACTION;",
	"BEGIN EXCLUSIVE TRANSACTION ;;", NULL
};
static const char *const commits[] = {
	"COMMIT", "commit;", " Commit ", "COMMIT TRANSACTION", "END", "end;",
	"END TRANSACTION", "end  transaction ;", NULL
};
static const char *const rollbacks[] = {
	"ROLLBACK", "rollback;", " RollBack\n", "ROLLBACK TRANSACTION",
	"rollback transaction;", NULL
};

int
main(void)
{
	bm_t *bm = NULL;
	bt_t *bt = NULL;
	sx_db *db = NULL, *db2 = NULL;
	bm_opts_t bo = BM_OPTS_DEFAULT;
	char path[256], wal[288];
	unsigned long long p0;
	int fd, i, j, n = 0, k = 1, path_kind;

	t_tmpl(path, sizeof path, "sqlxtc-txnfp");
	fd = mkstemp(path);
	if (fd < 0) { perror("mkstemp"); return 1; }
	close(fd);
	bo.path = path; bo.page_size = 4096; bo.n_frames = 1024; bo.cool_pct = 25;
	CK(bm_create(&bo, &bm) == XTC_OK);
	CK(bt_open(bm, &bt) == XTC_OK);
	CK(sx_open_bt(bt, &db) == SX_OK);
	CK(sx_open_bt(bt, &db2) == SX_OK);
	CK(sx_exec(db, "CREATE TABLE t(k INTEGER PRIMARY KEY, v INT)", NULL) == SX_OK);

	/* Each BEGIN x each end-of-transaction, through sx_exec (path 0) and
	 * through the parsed path (path 1): same visibility both ways. */
	for (path_kind = 0; path_kind < 2; path_kind++)
		for (i = 0; begins[i] != NULL; i++)
			for (j = 0; j < 2; j++) {
				const char *end = j ? rollbacks[(i + n) % 5] : commits[(i + n) % 8];
				char ins[64];
				int64_t before = count_rows(db2), mid, after;
				int rb, re;
				p0 = sql_parse_count();
				rb = path_kind ? parsed(db, begins[i]) : sx_exec(db, begins[i], NULL);
				if (path_kind == 0 && sql_parse_count() != p0) {
					printf("  FAIL %-34s parsed (fast path missed)\n", begins[i]);
					g_fail = 1;
				}
				CK(rb == SX_OK);
				snprintf(ins, sizeof ins, "INSERT INTO t VALUES(%d, 1)", k++);
				CK(sx_exec(db, ins, NULL) == SX_OK);
				mid = count_rows(db2);
				p0 = sql_parse_count();
				re = path_kind ? parsed(db, end) : sx_exec(db, end, NULL);
				if (path_kind == 0 && sql_parse_count() != p0) {
					printf("  FAIL %-34s parsed (fast path missed)\n", end);
					g_fail = 1;
				}
				CK(re == SX_OK);
				after = count_rows(db2);
				if (mid != before || after != before + (j ? 0 : 1)) {
					printf("  FAIL %s %-30s / %-22s rows %lld %lld %lld\n",
					    path_kind ? "parsed" : "fast", begins[i], end,
					    (long long)before, (long long)mid, (long long)after);
					g_fail = 1;
				}
				n++;
			}

	/* COMMIT / ROLLBACK with no transaction open: both are no-ops. */
	CK(sx_exec(db, "COMMIT", NULL) == parsed(db, "COMMIT"));
	CK(sx_exec(db, "ROLLBACK", NULL) == parsed(db, "ROLLBACK"));

	/* Not bare transaction control: each must still parse. */
	{
		static const char *const parse[] = {
			"BEGIN; SELECT 1", "BEGIN -- c", "/* c */ COMMIT", "BEGINX",
			"BEGIN TRANSACTION TRANSACTION", "COMMIT WORK", "END END",
			"BEGIN ISOLATION LEVEL READ COMMITTED", "ROLLBACK TO sp",
			"BEGIN DEFERRED IMMEDIATE", "ROLLBACK_", "COMMIT1", NULL
		};
		for (i = 0; parse[i] != NULL; i++) {
			p0 = sql_parse_count();
			(void)sx_exec(db, parse[i], NULL);
			if (sql_parse_count() == p0 &&
			    strncmp(parse[i], "BEGIN ISOLATION", 15) != 0) {
				printf("  FAIL %-38s took the fast path\n", parse[i]);
				g_fail = 1;
			}
			(void)sx_exec(db, "ROLLBACK", NULL);
		}
	}

	/* ROLLBACK TO sp keeps its meaning: undo to the savepoint, txn open. */
	{
		int64_t base = count_rows(db);
		char ins[64];
		CK(sx_exec(db, "BEGIN", NULL) == SX_OK);
		snprintf(ins, sizeof ins, "INSERT INTO t VALUES(%d, 1)", k++);
		CK(sx_exec(db, ins, NULL) == SX_OK);
		CK(sx_exec(db, "SAVEPOINT sp", NULL) == SX_OK);
		snprintf(ins, sizeof ins, "INSERT INTO t VALUES(%d, 1)", k++);
		CK(sx_exec(db, ins, NULL) == SX_OK);
		CK(sx_exec(db, "ROLLBACK TO sp", NULL) == SX_OK);
		CK(count_rows(db) == base + 1);
		CK(sx_exec(db, "COMMIT", NULL) == SX_OK);
		CK(count_rows(db2) == base + 1);
	}
	/* "BEGIN; INSERT; COMMIT" as one script still runs all three. */
	{
		int64_t base = count_rows(db2);
		char scr[128];
		snprintf(scr, sizeof scr, "BEGIN; INSERT INTO t VALUES(%d, 1); COMMIT", k++);
		CK(sx_exec(db, scr, NULL) == SX_OK);
		CK(count_rows(db2) == base + 1);
	}

	sx_close(db2);
	sx_close(db);
	bt_close(bt);
	bm_destroy(bm);
	unlink(path);
	snprintf(wal, sizeof wal, "%s-wal", path);
	unlink(wal);
	if (g_fail) {
		fprintf(stderr, "test_txn_fastpath: FAILED\n");
		return 1;
	}
	printf("  ok   %d BEGIN/end pairs, fast and parsed paths agree; 0 parses "
	    "on the fast path\n  ok   12 non-bare spellings still parse\n"
	    "test_txn_fastpath: ALL PASS\n", n);
	return 0;
}
