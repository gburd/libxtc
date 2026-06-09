/*-
 *
 *
 * examples/06_sqlxtc/test_savepoint.c
 *	Nested transactions (SQL SAVEPOINT / RELEASE / ROLLBACK TO) on the
 *	xstore virtual table.  SQLite parses the savepoint statements and
 *	drives the vtab's xSavepoint / xRelease / xRollbackTo hooks; the
 *	storage engine records a write-buffer mark per open savepoint and
 *	rolls back by truncating to it -- the Berkeley DB child-abort
 *	model on the buffered write set.  Verifies that a rolled-back
 *	nested transaction leaves the parent's state intact, a released
 *	one merges, and nesting works to several levels.
 *
 *	No daemon; standalone (plain asserts + printf).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "sqlite3.h"
#include "bufmgr.h"
#include "btree.h"
#include "xstore.h"

#define PAGE_SZ 4096

static int g_fail;
#define CK(c) do { if (!(c)) { \
	fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); \
	g_fail = 1; } } while (0)

/* Value of key k, or -1 if absent. */
static int64_t
val_of(xsql *db, int64_t k)
{
	xsql_stmt *st = NULL;
	int64_t v = -1;
	char sql[64];
	(void)snprintf(sql, sizeof sql, "SELECT v FROM t WHERE k=%lld;",
	    (long long)k);
	if (xsql_prepare_v2(db, sql, -1, &st, 0) != SQLITE_OK)
		return -2;
	if (xsql_step(st) == SQLITE_ROW)
		v = xsql_column_int64(st, 0);
	xsql_finalize(st);
	return v;
}

static int
row_count(xsql *db)
{
	xsql_stmt *st = NULL;
	int n = -1;
	if (xsql_prepare_v2(db, "SELECT count(*) FROM t;", -1, &st, 0)
	    != SQLITE_OK)
		return -1;
	if (xsql_step(st) == SQLITE_ROW)
		n = xsql_column_int(st, 0);
	xsql_finalize(st);
	return n;
}

static void
exec(xsql *db, const char *sql)
{
	char *err = NULL;
	int rc = xsql_exec(db, sql, 0, 0, &err);
	if (rc != SQLITE_OK) {
		fprintf(stderr, "FAIL exec [%s]: %s\n", sql,
		    err ? err : "?");
		g_fail = 1;
	}
	if (err)
		xsql_free(err);
}

int
main(void)
{
	bm_t *bm = NULL;
	bt_t *bt = NULL;
	bm_opts_t bo = BM_OPTS_DEFAULT;
	xsql *db = NULL;
	char path[64] = "/tmp/sqlxtc-savepoint-XXXXXX";
	int fd;

	g_fail = 0;
	fd = mkstemp(path);
	if (fd < 0) { perror("mkstemp"); return 1; }
	(void)close(fd);
	bo.path = path; bo.page_size = PAGE_SZ; bo.n_frames = 64;
	bo.cool_pct = 25;
	CK(bm_create(&bo, &bm) == XTC_OK);
	CK(bt_open(bm, &bt) == XTC_OK);
	CK(xsql_open(":memory:", &db) == SQLITE_OK);
	CK(xstore_register(db, bt) == SQLITE_OK);
	CK(xsql_exec(db, "CREATE VIRTUAL TABLE t USING xstore;", 0, 0, 0)
	    == SQLITE_OK);

	/* ---- ROLLBACK TO undoes a nested transaction ---- */
	exec(db, "BEGIN;");
	exec(db, "INSERT INTO t(k,v) VALUES(1,10);");
	exec(db, "SAVEPOINT s1;");
	exec(db, "INSERT INTO t(k,v) VALUES(2,20);");
	exec(db, "UPDATE t SET v=11 WHERE k=1;");
	/* Inside s1: both effects visible (read-your-writes). */
	CK(val_of(db, 1) == 11);
	CK(val_of(db, 2) == 20);
	exec(db, "ROLLBACK TO s1;");
	/* s1's effects gone; the pre-savepoint row 1 restored to 10. */
	CK(val_of(db, 1) == 10);
	CK(val_of(db, 2) == -1);
	exec(db, "RELEASE s1;");
	exec(db, "COMMIT;");
	CK(val_of(db, 1) == 10);
	CK(val_of(db, 2) == -1);
	CK(row_count(db) == 1);
	printf("  ok   ROLLBACK TO undoes the nested txn; parent intact\n");

	/* ---- RELEASE merges a nested transaction into the parent ---- */
	exec(db, "BEGIN;");
	exec(db, "SAVEPOINT s2;");
	exec(db, "INSERT INTO t(k,v) VALUES(3,30);");
	exec(db, "RELEASE s2;");          /* merge: row 3 stays */
	exec(db, "COMMIT;");
	CK(val_of(db, 3) == 30);
	printf("  ok   RELEASE merges the nested txn into the parent\n");

	/* ---- Nested several levels; roll back the middle ---- */
	exec(db, "BEGIN;");
	exec(db, "INSERT INTO t(k,v) VALUES(4,40);");
	exec(db, "SAVEPOINT a;");
	exec(db, "INSERT INTO t(k,v) VALUES(5,50);");
	exec(db, "SAVEPOINT b;");
	exec(db, "INSERT INTO t(k,v) VALUES(6,60);");
	exec(db, "SAVEPOINT c;");
	exec(db, "INSERT INTO t(k,v) VALUES(7,70);");
	CK(val_of(db, 7) == 70);
	/* Roll back to b: drops 6 and 7, keeps 4 and 5. */
	exec(db, "ROLLBACK TO b;");
	CK(val_of(db, 4) == 40);
	CK(val_of(db, 5) == 50);
	CK(val_of(db, 6) == -1);
	CK(val_of(db, 7) == -1);
	/* Can still write under the re-opened b and commit. */
	exec(db, "INSERT INTO t(k,v) VALUES(8,80);");
	exec(db, "COMMIT;");
	CK(val_of(db, 4) == 40);
	CK(val_of(db, 5) == 50);
	CK(val_of(db, 8) == 80);
	CK(val_of(db, 6) == -1 && val_of(db, 7) == -1);
	printf("  ok   multi-level nesting: ROLLBACK TO a middle savepoint\n");

	/* ---- Statement-level implicit savepoint: a failing statement
	 * does not corrupt the surrounding transaction. ---- */
	exec(db, "BEGIN;");
	exec(db, "INSERT INTO t(k,v) VALUES(9,90);");
	/* A constraint-free engine: emulate a rolled-back nested unit. */
	exec(db, "SAVEPOINT t1;");
	exec(db, "UPDATE t SET v=999 WHERE k=9;");
	exec(db, "ROLLBACK TO t1;");
	exec(db, "RELEASE t1;");
	exec(db, "COMMIT;");
	CK(val_of(db, 9) == 90);
	printf("  ok   nested unit rolled back, surrounding txn commits\n");

	xsql_close(db);
	bt_close(bt);
	bm_destroy(bm);
	unlink(path);

	if (g_fail) {
		printf("FAIL: savepoint/nested-transaction tests\n");
		return 1;
	}
	printf("All sqlxtc savepoint / nested-transaction tests passed.\n");
	return 0;
}
