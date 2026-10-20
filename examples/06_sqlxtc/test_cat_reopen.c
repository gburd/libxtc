/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_cat_reopen.c
 *	After a store is REOPENED (the normal case for every server start),
 *	a statement must not scan the catalog B-tree to resolve its table:
 *	the in-process catalog cache (g_cat) used to be filled only by a
 *	CREATE in this process, so on a reopened store every statement ran
 *	xs_cat_lookup (and xs_cat_value + a schema parse) -- ~19% of a TPC-C
 *	connection's CPU.  Asserts: after reopen, 1000 point SELECTs and
 *	1000 UPDATEs do 0 catalog-tree scans (xstore_cat_scan_count), and
 *	DROP TABLE / ALTER TABLE RENAME still resolve correctly afterwards.
 *	Off a loop.
 */

#include <stdint.h>
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

/* First column of the first row; -2 when the statement fails (at
 * prepare or at step, e.g. "no such table"). */
static int64_t
one(sx_db *h, const char *sql)
{
	sx_stmt *st = NULL;
	int64_t v = -2;
	if (sx_prepare(h, sql, -1, &st, NULL) != SX_OK) return -2;
	if (sx_step(st) == SX_ROW) v = sx_column_int64(st, 0);
	sx_finalize(st);
	return v;
}

int
main(void)
{
	char path[256], wal[300], dwb[300];
	sx_db *h = NULL;
	sx_stmt *q = NULL, *u = NULL;
	uint64_t s0;
	int i, fd;

	t_tmpl(path, sizeof path, "sqlxtc-catreopen");
	if ((fd = mkstemp(path)) < 0) return 1;
	close(fd);
	unlink(path);
	snprintf(wal, sizeof wal, "%s-wal", path);
	snprintf(dwb, sizeof dwb, "%s.dwb", path);

	CK(sx_init() == SX_OK);
	CK(sx_storage_open(path, 256) == SX_OK);
	CK(sx_open(":memory:", &h) == SX_OK);
	CK(sx_exec(h, "CREATE TABLE a(k INTEGER PRIMARY KEY, v INTEGER)", NULL) == SX_OK);
	CK(sx_exec(h, "CREATE TABLE b(x INTEGER, y INTEGER, z TEXT, PRIMARY KEY(x, y))", NULL) == SX_OK);
	CK(sx_exec(h, "CREATE TABLE c(k INTEGER PRIMARY KEY, w TEXT)", NULL) == SX_OK);
	for (i = 1; i <= 50; i++) {
		char sql[160];
		snprintf(sql, sizeof sql, "INSERT INTO a VALUES(%d, %d)", i, i * 10);
		CK(sx_exec(h, sql, NULL) == SX_OK);
		snprintf(sql, sizeof sql, "INSERT INTO b VALUES(%d, %d, 'r%d')", i % 5, i, i);
		CK(sx_exec(h, sql, NULL) == SX_OK);
	}
	sx_close(h);
	CK(sx_storage_close() == SX_OK);

	/* Reopen: a fresh process would see only the persisted catalog. */
	CK(sx_storage_open(path, 256) == SX_OK);
	CK(sx_open(":memory:", &h) == SX_OK);
	CK(one(h, "SELECT count(*) FROM a") == 50);      /* warm: may scan once */
	CK(one(h, "SELECT count(*) FROM b") == 50);
	CK(sx_prepare(h, "SELECT v FROM a WHERE k = ?", -1, &q, NULL) == SX_OK);
	CK(sx_prepare(h, "UPDATE b SET z = 'u' WHERE x = ? AND y = ?", -1, &u, NULL) == SX_OK);
	s0 = xstore_cat_scan_count();
	for (i = 0; i < 1000; i++) {
		sx_bind_int64(q, 1, 1 + i % 50);
		CK(sx_step(q) == SX_ROW && sx_column_int64(q, 0) == (1 + i % 50) * 10);
		sx_reset(q);
		sx_bind_int64(u, 1, (1 + i % 50) % 5);
		sx_bind_int64(u, 2, 1 + i % 50);
		CK(sx_step(u) == SX_DONE);
		sx_reset(u);
	}
	printf("  catalog-tree scans for 2000 statements on a reopened store: %llu\n",
	    (unsigned long long)(xstore_cat_scan_count() - s0));
	CK(xstore_cat_scan_count() - s0 == 0);
	sx_finalize(q); sx_finalize(u);

	/* DDL after the cache was primed from the persisted catalog. */
	CK(sx_exec(h, "ALTER TABLE c RENAME TO c2", NULL) == SX_OK);
	CK(sx_exec(h, "INSERT INTO c2 VALUES(1, 'x')", NULL) == SX_OK);
	CK(one(h, "SELECT count(*) FROM c2") == 1);
	CK(one(h, "SELECT count(*) FROM c") == -2);      /* gone */
	CK(sx_exec(h, "DROP TABLE a", NULL) == SX_OK);
	CK(one(h, "SELECT count(*) FROM a") == -2);
	CK(sx_exec(h, "CREATE TABLE a(k INTEGER PRIMARY KEY, v INTEGER)", NULL) == SX_OK);
	CK(one(h, "SELECT count(*) FROM a") == 0);       /* a fresh, empty table */
	CK(one(h, "SELECT count(*) FROM b") == 50);
	sx_close(h);
	CK(sx_storage_close() == SX_OK);

	/* And the renamed / recreated tables survive a second reopen. */
	CK(sx_storage_open(path, 256) == SX_OK);
	CK(sx_open(":memory:", &h) == SX_OK);
	CK(one(h, "SELECT count(*) FROM c2") == 1);
	CK(one(h, "SELECT count(*) FROM a") == 0);
	CK(one(h, "SELECT count(*) FROM b") == 50);
	sx_close(h);
	CK(sx_storage_close() == SX_OK);
	unlink(path); unlink(wal); unlink(dwb);
	if (g_fail) { fprintf(stderr, "FAIL: test_cat_reopen\n"); return 1; }
	printf("  ok   a reopened store resolves tables without scanning the catalog tree\n");
	return 0;
}
