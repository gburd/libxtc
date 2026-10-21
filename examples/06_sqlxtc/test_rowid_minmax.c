/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_rowid_minmax.c
 *	MAX(rowid) and MIN(rowid) must not scan the table.  MAX of an
 *	INTEGER PRIMARY KEY walked every row (403 ms at 2M rows); the ledger
 *	"most recent N" query "WHERE seq > (SELECT max(seq) ...) - N" paid it
 *	per execution.  Asserts rows visited for MAX/MIN stay tiny, and the
 *	answers are right under deletes (a tombstoned top rowid), a
 *	snapshot that predates newer rows, and a transaction's own inserts
 *	and deletes.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "engine.h"
#include "xstore.h"
#include "xtc.h"
#include "t_tmp.h"

static int g_fail;
#ifdef CK
#undef CK
#endif
#define CK(x) do { if (!(x)) { \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
	g_fail = 1; } } while (0)

static int64_t
one(sx_db *h, const char *sql)
{
	sx_stmt *st = NULL;
	int64_t v = -2;
	if (sx_prepare(h, sql, -1, &st, NULL) != SX_OK) return -3;
	if (sx_step(st) == SX_ROW)
		v = sx_column_type(st, 0) == SX_NULL ? -1 : sx_column_int64(st, 0);
	sx_finalize(st);
	return v;
}

int
main(void)
{
	char path[256], side[320];
	const char *sfx[] = { "", "-wal", ".dwb" };
	sx_db *h = NULL, *h2 = NULL;
	uint64_t v0;
	int fd, i;

	t_tmpl(path, sizeof path, "sqlxtc-rowmax");
	if ((fd = mkstemp(path)) < 0) return 1;
	close(fd);
	unlink(path);
	CK(sx_init() == SX_OK);
	CK(sx_storage_open(path, 1024) == SX_OK);
	CK(sx_open(":memory:", &h) == SX_OK);
	CK(sx_open(":memory:", &h2) == SX_OK);
	CK(sx_exec(h, "CREATE TABLE t(k INTEGER PRIMARY KEY, v INTEGER)", NULL) == SX_OK);
	CK(one(h, "SELECT max(k) FROM t") == -1);          /* empty: NULL */
	CK(sx_exec(h, "BEGIN", NULL) == SX_OK);
	for (i = 1; i <= 20000; i++) {
		char sql[96];
		snprintf(sql, sizeof sql, "INSERT INTO t VALUES(%d, %d)", i, i);
		CK(sx_exec(h, sql, NULL) == SX_OK);
	}
	CK(sx_exec(h, "COMMIT", NULL) == SX_OK);

	v0 = xstore_rows_visited();
	CK(one(h, "SELECT max(k) FROM t") == 20000);
	CK(one(h, "SELECT min(k) FROM t") == 1);
	CK(one(h, "SELECT count(*) FROM t WHERE k > (SELECT max(k) FROM t) - 100") == 100);
	printf("  rows visited for max + min + recent-100: %llu (table: 20000)\n",
	    (unsigned long long)(xstore_rows_visited() - v0));
	CK(xstore_rows_visited() - v0 < 400);

	/* tombstoned top rows */
	CK(sx_exec(h, "DELETE FROM t WHERE k > 19990", NULL) == SX_OK);
	CK(one(h, "SELECT max(k) FROM t") == 19990);
	CK(sx_exec(h, "DELETE FROM t WHERE k < 5", NULL) == SX_OK);
	CK(one(h, "SELECT min(k) FROM t") == 5);

	/* a REPEATABLE READ snapshot taken before newer rows */
	CK(sx_exec(h2, "BEGIN ISOLATION LEVEL REPEATABLE READ", NULL) == SX_OK);
	CK(one(h2, "SELECT max(k) FROM t") == 19990);
	CK(sx_exec(h, "INSERT INTO t VALUES(30000, 1)", NULL) == SX_OK);
	CK(one(h, "SELECT max(k) FROM t") == 30000);
	CK(one(h2, "SELECT max(k) FROM t") == 19990);   /* still the old snapshot */
	CK(sx_exec(h2, "COMMIT", NULL) == SX_OK);

	/* a transaction's own writes */
	CK(sx_exec(h, "BEGIN", NULL) == SX_OK);
	CK(sx_exec(h, "INSERT INTO t VALUES(40000, 1)", NULL) == SX_OK);
	CK(one(h, "SELECT max(k) FROM t") == 40000);
	CK(one(h2, "SELECT max(k) FROM t") == 30000);   /* not visible to others */
	CK(sx_exec(h, "DELETE FROM t WHERE k >= 30000", NULL) == SX_OK);
	CK(one(h, "SELECT max(k) FROM t") == 19990);
	CK(sx_exec(h, "ROLLBACK", NULL) == SX_OK);
	CK(one(h, "SELECT max(k) FROM t") == 30000);

	/* with a WHERE: still right (the general path) */
	CK(one(h, "SELECT max(k) FROM t WHERE v < 100") == 30000);   /* v=1 */
	CK(one(h, "SELECT max(k) FROM t WHERE v > 100") == 19990);

	sx_close(h2);
	sx_close(h);
	(void)sx_storage_close();
	(void)sx_shutdown();
	for (i = 0; i < 3; i++) { snprintf(side, sizeof side, "%s%s", path, sfx[i]); unlink(side); }
	if (g_fail) { fprintf(stderr, "FAIL: test_rowid_minmax\n"); return 1; }
	printf("  ok   MAX/MIN(rowid) without a table scan; correct under deletes, snapshots, own writes\n");
	return 0;
}
