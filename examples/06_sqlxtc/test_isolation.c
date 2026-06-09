/*-
 *
 *
 * examples/06_sqlxtc/test_isolation.c
 *	Isolation levels on the xstore virtual table.  All reads are
 *	against committed versions (writes buffer until commit), so the
 *	levels differ in WHEN the read snapshot is taken:
 *
 *	  REPEATABLE READ / SNAPSHOT / SERIALIZABLE  one snapshot per
 *	      transaction -- a read repeats the same value even after a
 *	      concurrent commit.
 *	  READ COMMITTED / READ UNCOMMITTED  a fresh snapshot per
 *	      statement -- a read sees a concurrent commit that landed
 *	      since the previous statement.
 *
 *	Two connections share one B-tree.  Connection A opens a
 *	transaction and reads a row; connection B commits a new value;
 *	A reads again.  Under repeatable read A sees the old value; under
 *	read committed A sees the new one.  Proves the level actually
 *	changes visibility.
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

static void
exec(xsql *db, const char *sql)
{
	char *err = NULL;
	if (xsql_exec(db, sql, 0, 0, &err) != SQLITE_OK) {
		fprintf(stderr, "FAIL exec [%s]: %s\n", sql, err ? err : "?");
		g_fail = 1;
	}
	if (err)
		xsql_free(err);
}

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

/* Open a second connection on the SAME bt (shared engine). */
static xsql *
open_conn(bt_t *bt)
{
	xsql *db = NULL;
	if (xsql_open(":memory:", &db) != SQLITE_OK)
		return NULL;
	if (xstore_register(db, bt) != SQLITE_OK) {
		xsql_close(db);
		return NULL;
	}
	/* Each connection re-declares the virtual table over the shared
	 * engine; the table-id catalog keeps them consistent. */
	(void)xsql_exec(db, "CREATE VIRTUAL TABLE t USING xstore;", 0, 0, 0);
	return db;
}

int
main(void)
{
	bm_t *bm = NULL;
	bt_t *bt = NULL;
	bm_opts_t bo = BM_OPTS_DEFAULT;
	xsql *a = NULL, *b = NULL;
	char path[64] = "/tmp/sqlxtc-isolation-XXXXXX";
	int fd;

	g_fail = 0;
	fd = mkstemp(path);
	if (fd < 0) { perror("mkstemp"); return 1; }
	(void)close(fd);
	bo.path = path; bo.page_size = PAGE_SZ; bo.n_frames = 64;
	bo.cool_pct = 25;
	CK(bm_create(&bo, &bm) == XTC_OK);
	CK(bt_open(bm, &bt) == XTC_OK);

	a = open_conn(bt);
	b = open_conn(bt);
	CK(a != NULL && b != NULL);

	/* Seed: row 1 = 100, committed. */
	exec(b, "INSERT INTO t(k,v) VALUES(1,100);");
	CK(val_of(a, 1) == 100);

	/* ---- REPEATABLE READ: A's snapshot is frozen at txn start ---- */
	exec(a, "SELECT xstore_isolation('repeatable read');");
	exec(a, "BEGIN;");
	CK(val_of(a, 1) == 100);          /* first read fixes the snapshot */
	exec(b, "UPDATE t SET v=200 WHERE k=1;");   /* B commits a new value */
	CK(val_of(b, 1) == 200);          /* B sees its own commit */
	CK(val_of(a, 1) == 100);          /* A still sees the frozen value */
	exec(a, "COMMIT;");
	CK(val_of(a, 1) == 200);          /* after commit, A sees latest */
	printf("  ok   REPEATABLE READ: snapshot frozen across a concurrent "
	    "commit\n");

	/* ---- READ COMMITTED: A re-samples per statement ---- */
	exec(a, "SELECT xstore_isolation('read committed');");
	exec(a, "BEGIN;");
	CK(val_of(a, 1) == 200);          /* current committed value */
	exec(b, "UPDATE t SET v=300 WHERE k=1;");   /* B commits again */
	CK(val_of(a, 1) == 300);          /* A's next statement sees it */
	exec(a, "COMMIT;");
	printf("  ok   READ COMMITTED: each statement sees the latest "
	    "commit\n");

	/* ---- READ UNCOMMITTED behaves as READ COMMITTED (no dirty rows) -- */
	exec(a, "SELECT xstore_isolation('read uncommitted');");
	exec(a, "BEGIN;");
	CK(val_of(a, 1) == 300);
	exec(b, "UPDATE t SET v=400 WHERE k=1;");
	CK(val_of(a, 1) == 400);          /* sees committed, never B's buffer */
	exec(a, "COMMIT;");
	printf("  ok   READ UNCOMMITTED: committed reads only (no dirty "
	    "data)\n");

	/* An autocommit read between explicit transactions lets the engine
	 * observe the transaction boundary.  SQLite only drives the vtab's
	 * xBegin/xCommit for transactions that WRITE the vtab, so a
	 * read-only transaction is invisible to those hooks; xs_enter
	 * detects its end on the next autocommit access.  Real clients read
	 * between transactions as a matter of course. */
	CK(val_of(a, 1) == 400);

	/* ---- SERIALIZABLE: snapshot frozen + read-set validation ---- */
	exec(a, "SELECT xstore_isolation('serializable');");
	exec(a, "BEGIN;");
	CK(val_of(a, 1) == 400);          /* frozen like repeatable read */
	exec(b, "UPDATE t SET v=500 WHERE k=1;");
	CK(val_of(a, 1) == 400);          /* still frozen */
	exec(a, "COMMIT;");               /* read-only txn: commits clean */
	CK(val_of(a, 1) == 500);
	printf("  ok   SERIALIZABLE: frozen snapshot, read-set validated\n");

	/* ---- The level setter reports the resulting integer level ---- */
	{
		xsql_stmt *st = NULL;
		int lvl = -1;
		CK(xsql_prepare_v2(a, "SELECT xstore_isolation('snapshot');",
		    -1, &st, 0) == SQLITE_OK);
		if (xsql_step(st) == SQLITE_ROW)
			lvl = xsql_column_int(st, 0);
		xsql_finalize(st);
		CK(lvl == 3);   /* XS_ISO_SNAPSHOT */
	}
	printf("  ok   xstore_isolation reports the resolved level\n");

	xsql_close(a);
	xsql_close(b);
	bt_close(bt);
	bm_destroy(bm);
	unlink(path);

	if (g_fail) {
		printf("FAIL: isolation-level tests\n");
		return 1;
	}
	printf("All sqlxtc isolation-level tests passed.\n");
	return 0;
}
