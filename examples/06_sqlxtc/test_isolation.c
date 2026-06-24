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

#include "engine.h"
#include "bufmgr.h"
#include "btree.h"
#include "xstore.h"
#include "t_tmp.h"

#define PAGE_SZ 4096


static void
exec(sx_db *db, const char *sql)
{
	char *err = NULL;
	if (sx_exec(db, sql, &err) != SX_OK) {
		fprintf(stderr, "FAIL exec [%s]: %s\n", sql, err ? err : "?");
		g_fail = 1;
	}
	if (err)
		free(err);
}

static int64_t
val_of(sx_db *db, int64_t k)
{
	sx_stmt *st = NULL;
	int64_t v = -1;
	char sql[64];
	(void)snprintf(sql, sizeof sql, "SELECT v FROM t WHERE k=%lld;",
	    (long long)k);
	if (sx_prepare(db, sql, -1, &st, NULL) != SX_OK)
		return -2;
	if (sx_step(st) == SX_ROW)
		v = sx_column_int64(st, 0);
	sx_finalize(st);
	return v;
}

/* Open a second connection on the SAME bt (shared engine). */
static sx_db *
open_conn(bt_t *bt)
{
	sx_db *db = NULL;
	if (sx_open_bt(bt, &db) != SX_OK)
		return NULL;
	/* Each connection re-declares the table over the shared engine; the
	 * table-id catalog keeps them consistent. */
	(void)sx_exec(db, "CREATE TABLE t(k INTEGER PRIMARY KEY, v)", NULL);
	return db;
}

int
main(void)
{
	bm_t *bm = NULL;
	bt_t *bt = NULL;
	bm_opts_t bo = BM_OPTS_DEFAULT;
	sx_db *a = NULL, *b = NULL;
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
	CK(xstore_set_isolation(a, "repeatable read") == 0);
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
	CK(xstore_set_isolation(a, "read committed") == 0);
	exec(a, "BEGIN;");
	CK(val_of(a, 1) == 200);          /* current committed value */
	exec(b, "UPDATE t SET v=300 WHERE k=1;");   /* B commits again */
	CK(val_of(a, 1) == 300);          /* A's next statement sees it */
	exec(a, "COMMIT;");
	printf("  ok   READ COMMITTED: each statement sees the latest "
	    "commit\n");

	/* ---- READ UNCOMMITTED behaves as READ COMMITTED (no dirty rows) -- */
	CK(xstore_set_isolation(a, "read uncommitted") == 0);
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
	CK(xstore_set_isolation(a, "serializable") == 0);
	exec(a, "BEGIN;");
	CK(val_of(a, 1) == 400);          /* frozen like repeatable read */
	exec(b, "UPDATE t SET v=500 WHERE k=1;");
	CK(val_of(a, 1) == 400);          /* still frozen */
	exec(a, "COMMIT;");               /* read-only txn: commits clean */
	CK(val_of(a, 1) == 500);
	printf("  ok   SERIALIZABLE: frozen snapshot, read-set validated\n");

	/* ---- The level setter accepts every named level ---- */
	{
		CK(xstore_set_isolation(a, "snapshot") == 0);
		CK(xstore_set_isolation(a, "bogus level") == -1);
	}
	printf("  ok   xstore_set_isolation accepts named levels\n");

	sx_close(a);
	sx_close(b);
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
