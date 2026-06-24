/*
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 *
 * test_native_txn -- the native-driver transaction ownership (subsystems
 * A+B foundation): xstore_native_mode / _begin / _commit / _rollback /
 * savepoint, driven WITHOUT SQLite's autocommit flag.  Proves the txn
 * state machine (subsystem C) works under native ownership, which the
 * full native sx_stmt driver builds on.
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "vexec.h"
#include "engine.h"
#include "bufmgr.h"
#include "btree.h"
#include "xstore.h"

static int g_fail;
#define CK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", (m)); g_fail = 1; } } while (0)

static void
dump(sx_db *db, char *buf, size_t cap)
{
	sx_stmt *s = NULL;
	size_t n = 0;
	buf[0] = 0;
	if (sx_prepare(db, "SELECT k, a FROM t ORDER BY k", -1, &s, NULL) != SX_OK)
		return;
	while (sx_step(s) == SX_ROW)
		n += (size_t)snprintf(buf + n, cap - n, "(%lld,%lld)",
		    (long long)sx_column_int64(s, 0),
		    (long long)sx_column_int64(s, 1));
	sx_finalize(s);
}

static int
W(sx_db *db, const char *q)
{
	int64_t n = 0;
	return vx_run_write((sqlite3 *)db, q, &n, NULL);
}

int
main(void)
{
	char path[64] = "/tmp/sqlxtc_ntxXXXXXX";
	int fd = mkstemp(path);
	bm_t *bm = NULL;
	bt_t *bt = NULL;
	bm_opts_t bo = BM_OPTS_DEFAULT;
	sx_db *db = NULL;
	char a1[256], a2[256], a3[256];

	if (fd >= 0) close(fd);
	if (sx_init() != SX_OK) { fprintf(stderr, "FAIL: sx_init\n"); return 1; }
	bo.path = path; bo.page_size = 4096; bo.n_frames = 64;
	if (bm_create(&bo, &bm) != 0 || bt_open(bm, &bt) != 0 ||
	    sx_open_bt(bt, &db) != SX_OK) {
		fprintf(stderr, "FAIL: setup\n");
		return 1;
	}
	sx_exec(db, "CREATE TABLE t(k INTEGER PRIMARY KEY, a INT)", NULL);

	/* Take native ownership of autocommit -- from here the native API
	 * drives the txn, NOT SQLite's autocommit flag. */
	CK(xstore_native_mode((sqlite3 *)db, 1) == 0, "native_mode on");

	/* Autocommit: each native write commits on its own. */
	CK(W(db, "INSERT INTO t(a) VALUES(10)") == 1, "autocommit insert 1");
	CK(W(db, "INSERT INTO t(a) VALUES(20)") == 1, "autocommit insert 2");
	dump(db, a1, sizeof a1);
	CK(strstr(a1, "10") && strstr(a1, "20"), "autocommit writes persisted");

	/* Explicit native txn with an inner savepoint rolled back. */
	CK(xstore_native_begin((sqlite3 *)db) == 0, "native begin");
	CK(W(db, "INSERT INTO t(a) VALUES(30)") == 1, "in-txn insert before sp");
	CK(xstore_savepoint((sqlite3 *)db, 0) == 0, "savepoint 0");
	CK(W(db, "INSERT INTO t(a) VALUES(40)") == 1, "in-txn insert in sp");
	CK(xstore_rollback_to((sqlite3 *)db, 0) == 0, "rollback to sp 0");
	CK(W(db, "INSERT INTO t(a) VALUES(50)") == 1, "in-txn insert after rollback");
	CK(xstore_commit((sqlite3 *)db) == 0, "native commit");
	dump(db, a2, sizeof a2);
	/* 30 (before sp, kept) and 50 (after rollback, kept) survive; 40
	 * (inside the rolled-back savepoint) does not. */
	CK(strstr(a2, "30") != NULL, "pre-savepoint write committed");
	CK(strstr(a2, "50") != NULL, "post-rollback write committed");
	CK(strstr(a2, "40") == NULL, "rolled-back savepoint write gone");

	/* Whole-transaction rollback discards everything since BEGIN. */
	CK(xstore_native_begin((sqlite3 *)db) == 0, "native begin 2");
	CK(W(db, "INSERT INTO t(a) VALUES(99)") == 1, "in-txn insert to roll back");
	CK(xstore_rollback((sqlite3 *)db) == 0, "native rollback");
	dump(db, a3, sizeof a3);
	CK(strcmp(a3, a2) == 0, "rollback left the table unchanged");
	CK(strstr(a3, "99") == NULL, "rolled-back write gone");

	sx_close(db);
	bt_close(bt);
	bm_destroy(bm);
	unlink(path);

	if (g_fail) { fprintf(stderr, "test_native_txn: FAILURES\n"); return 1; }
	printf("All sqlxtc native-driver transaction tests passed.\n");
	return 0;
}
