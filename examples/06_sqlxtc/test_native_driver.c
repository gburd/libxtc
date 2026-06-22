/*
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 *
 * test_native_driver -- the native sx_stmt driver (subsystems A+B):
 * with sx_native_driver(1), a SELECT runs via vexec, DML via the native
 * write path, and BEGIN/COMMIT/ROLLBACK via the native txn API, all
 * through the SAME sx_prepare/sx_step/sx_column_* the application uses
 * -- with NO SQLite VDBE program.  The result is compared to the same
 * queries run with the driver OFF (the VDBE), proving byte-equivalence.
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "engine.h"

static int g_fail;
#define CK(c, m) do { if (!(c)) { fprintf(stderr, "FAIL: %s\n", (m)); g_fail = 1; } } while (0)

/* Run `sql` via the sx_* API, serialize the rows to `out`. */
static void
run(sx_db *h, const char *sql, char *out, size_t cap)
{
	sx_stmt *st = NULL;
	const char *tail = NULL;
	size_t n = 0;
	out[0] = 0;
	if (sx_prepare(h, sql, -1, &st, &tail) != SX_OK || st == NULL) {
		snprintf(out, cap, "<prepare-failed>");
		if (st) sx_finalize(st);
		return;
	}
	while (sx_step(st) == SX_ROW) {
		int nc = sx_column_count(st), i;
		for (i = 0; i < nc; i++) {
			int t = sx_column_type(st, i);
			if (t == SX_INTEGER)
				n += (size_t)snprintf(out + n, cap - n, "%lld ",
				    (long long)sx_column_int64(st, i));
			else if (t == SX_FLOAT)
				n += (size_t)snprintf(out + n, cap - n, "%g ", sx_column_double(st, i));
			else if (t == SX_TEXT)
				n += (size_t)snprintf(out + n, cap - n, "%s ", sx_column_text(st, i));
			else
				n += (size_t)snprintf(out + n, cap - n, "NULL ");
		}
		n += (size_t)snprintf(out + n, cap - n, "| ");
	}
	sx_finalize(st);
}

/* Run a side-effecting statement (DML / txn) via sx_* (no rows). */
static void
exec1(sx_db *h, const char *sql)
{
	sx_stmt *st = NULL;
	const char *tail = NULL;
	if (sx_prepare(h, sql, -1, &st, &tail) == SX_OK && st != NULL) {
		while (sx_step(st) == SX_ROW) { }
		sx_finalize(st);
	}
}

int
main(void)
{
	char storepath[64] = "/tmp/sqlxtc_ndstoreXXXXXX";
	int sfd = mkstemp(storepath);
	sx_db *h = NULL;
	char on[512], off[512];
	const char *qs[] = {
		"SELECT a, b FROM t WHERE a > 15 ORDER BY a",
		"SELECT count(*), sum(a) FROM t",
		"SELECT b, count(*) FROM t GROUP BY b ORDER BY b",
		"SELECT a FROM t WHERE a NOT NULL ORDER BY a",
	};
	unsigned i;

	if (sfd >= 0) close(sfd);
	if (sx_init() != SX_OK) { fprintf(stderr, "FAIL: sx_init\n"); return 1; }
	if (sx_storage_open(storepath, 64) != SX_OK) {
		fprintf(stderr, "FAIL: storage_open\n"); return 1; }
	if (sx_open(":memory:", &h) != SX_OK) { fprintf(stderr, "FAIL: open\n"); return 1; }

	/* Seed via the VDBE (driver off): create + populate. */
	exec1(h, "CREATE VIRTUAL TABLE t USING xstore(k, a INT, b TEXT)");
	exec1(h, "INSERT INTO t VALUES(1,10,'x'),(2,20,'y'),(3,30,'x'),(4,NULL,'z'),(5,40,'y')");

	for (i = 0; i < sizeof qs / sizeof qs[0]; i++) {
		sx_native_driver(0);
		run(h, qs[i], off, sizeof off);
		sx_native_driver(1);
		run(h, qs[i], on, sizeof on);
		sx_native_driver(0);
		if (strcmp(on, off) != 0)
			fprintf(stderr, "FAIL: [%s]\n  vdbe : %s\n  native: %s\n", qs[i], off, on);
		CK(strcmp(on, off) == 0, qs[i]);
	}

	/* DML + transactions through the native driver, then read back. */
	sx_native_driver(1);
	exec1(h, "BEGIN");
	exec1(h, "INSERT INTO t(a, b) VALUES(60, 'n')");   /* auto-PK, in-txn native */
	exec1(h, "COMMIT");
	sx_native_driver(0);
	run(h, "SELECT count(*) FROM t WHERE b = 'n'", off, sizeof off);
	CK(strcmp(off, "1 | ") == 0, "native BEGIN/insert/COMMIT persisted one row");

	sx_native_driver(1);
	exec1(h, "BEGIN");
	exec1(h, "INSERT INTO t(a, b) VALUES(70, 'r')");
	exec1(h, "ROLLBACK");
	sx_native_driver(0);
	run(h, "SELECT count(*) FROM t WHERE b = 'r'", off, sizeof off);
	CK(strcmp(off, "0 | ") == 0, "native BEGIN/insert/ROLLBACK discarded the row");

	/* Native DDL: CREATE TABLE + DROP TABLE through the native driver
	 * (no SQLite schema), then write + read the natively-created table
	 * entirely natively. */
	sx_native_driver(1);
	exec1(h, "CREATE TABLE nt(id INTEGER PRIMARY KEY, v INT, s TEXT)");
	exec1(h, "INSERT INTO nt(v, s) VALUES(11, 'aa')");
	exec1(h, "INSERT INTO nt(v, s) VALUES(22, 'bb')");
	run(h, "SELECT v, s FROM nt WHERE v > 5 ORDER BY v", on, sizeof on);
	CK(strcmp(on, "11 aa | 22 bb | ") == 0, "native CREATE + INSERT + SELECT on a vtab-free table");
	exec1(h, "DROP TABLE nt");
	{
		/* After DROP the catalog entry is gone: the native SELECT prepares
		 * (a SELECT classifies structurally) but vexec finds no catalog
		 * entry, so the step errors -- no rows, not SX_ROW. */
		sx_stmt *st = NULL; const char *tl = NULL;
		int pr = sx_prepare(h, "SELECT v FROM nt", -1, &st, &tl);
		int got_row = 0;
		if (pr == SX_OK && st != NULL)
			got_row = (sx_step(st) == SX_ROW);
		CK(!got_row, "DROP TABLE: no rows from the dropped table");
		if (st) sx_finalize(st);
	}
	sx_native_driver(1);
	{
		/* A value-setting PRAGMA classifies native (no-op, no rows). */
		sx_stmt *st = NULL; const char *tl = NULL;
		int pr = sx_prepare(h, "PRAGMA synchronous=NORMAL", -1, &st, &tl);
		int rows = 0, done = 0;
		if (pr == SX_OK && st != NULL) {
			int rc;
			while ((rc = sx_step(st)) == SX_ROW) rows++;
			done = (rc == SX_DONE);
			sx_finalize(st);
		}
		CK(pr == SX_OK && rows == 0 && done, "native value-setting PRAGMA is a no-op");
	}

	/* Plain CREATE TABLE -- NO vtab -- is the post-excision table
	 * creation idiom: the native driver creates the xstore catalog
	 * entry, then INSERT + SELECT run natively over it.  This is the
	 * drop-in that replaces CREATE VIRTUAL TABLE ... USING xstore. */
	sx_native_driver(1);
	exec1(h, "CREATE TABLE m(k INTEGER PRIMARY KEY, a INT, b TEXT)");
	exec1(h, "INSERT INTO m(a, b) VALUES(5, 'p')");
	exec1(h, "INSERT INTO m(a, b) VALUES(9, 'q')");
	run(h, "SELECT a, b FROM m WHERE a > 4 ORDER BY a", on, sizeof on);
	CK(strcmp(on, "5 p | 9 q | ") == 0,
	   "plain CREATE TABLE: native create + insert + select (no vtab)");
	sx_native_driver(0);

	sx_close(h);
	sx_storage_close();
	unlink(storepath);

	if (g_fail) { fprintf(stderr, "test_native_driver: FAILURES\n"); return 1; }
	printf("All sqlxtc native-driver (sx_stmt) tests passed.\n");
	return 0;
}
