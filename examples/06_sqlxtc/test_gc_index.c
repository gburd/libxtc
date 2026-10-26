/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_gc_index.c
 *	The version GC (xstore_gc) keeps secondary-index entries that are
 *	exactly as long as a version key.
 *
 *	xs_gc walked every 20-byte key from the start of the tree as a
 *	version key ([table:4][rowid:8][inverted commit ts:8]) and stopped
 *	only at the first key of another length.  An index entry key is
 *	[0xFE][index id:4][elements][rowid:8]; a single 4-character TEXT
 *	value (or one integer of 2^39..2^47) makes it 20 bytes.  Read as a
 *	version, its "commit ts" is 2^63 - 1 - rowid, below the GC horizon
 *	for a rowid near INT64_MAX, so two rows with the same value looked
 *	like two versions of one row and the GC deleted the "older" entry:
 *	the index lost the row (a query through it returned too few rows).
 *	Two rows near INT64_MAX share each value; after xstore_gc() both
 *	are found through the index, and xstore_index_check reports no
 *	entry missing.  Off a loop (synchronous storage).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "engine.h"
#include "xstore.h"
#include "t_tmp.h"

static int64_t
q1(sx_db *db, const char *sql)
{
	sx_stmt *st = NULL;
	int64_t v = -1;
	if (sx_prepare(db, sql, -1, &st, NULL) != SX_OK)
		return -1;
	if (sx_step(st) == SX_ROW)
		v = sx_column_int64(st, 0);
	sx_finalize(st);
	return v;
}

int
main(void)
{
	char path[256], side[300];
	static const char *sfx[] = { "", ".xstore", ".xstore-wal", ".xstore.lock" };
	sx_db *db = NULL;
	uint64_t seeks, miss = 0, stale = 0;
	int64_t s, n;
	int fd, i;

	printf("test_gc_index\n");
	t_tmpl(path, sizeof path, "sqlxtc-gcidx");
	if ((fd = mkstemp(path)) >= 0)
		close(fd);
	CK(sx_init() == SX_OK);
	CK(sx_storage_open(path, 1024) == SX_OK);
	CK(sx_open(":memory:", &db) == SX_OK);
	if (db == NULL)
		return 1;
	CK(sx_exec(db, "CREATE TABLE t(k INTEGER PRIMARY KEY, s TEXT, n INTEGER)",
	    NULL) == SX_OK);
	CK(sx_exec(db, "CREATE INDEX t_s ON t(s)", NULL) == SX_OK);
	CK(sx_exec(db, "CREATE INDEX t_n ON t(n)", NULL) == SX_OK);
	CK(sx_exec(db, "INSERT INTO t VALUES"
	    "(9223372036854775801, 'abcd', 1099511627776),"
	    "(9223372036854775802, 'abcd', 1099511627776),"
	    "(9223372036854775803, 'wxyz', 1099511627777),"
	    "(9223372036854775804, 'wxyz', 1099511627777), (5, 'abcd', 7)",
	    NULL) == SX_OK);
	CK(sx_exec(db, "UPDATE t SET n = n WHERE k = 5", NULL) == SX_OK);
	sx_close(db);

	/* As a new connection, after the commits are visible. */
	CK(sx_open(":memory:", &db) == SX_OK);
	if (db == NULL)
		return 1;
	(void)sx_exec(db, "CREATE TABLE t(k INTEGER PRIMARY KEY, s TEXT, n INTEGER)",
	    NULL);
	printf("  xstore_gc reclaimed %d versions\n",
	    xstore_gc_run((struct xsql *)db));
	seeks = xstore_index_seek_count();
	s = q1(db, "SELECT count(*) FROM t WHERE s = 'abcd'");
	n = q1(db, "SELECT count(*) FROM t WHERE n = 1099511627776");
	printf("  through the indexes after GC: s='abcd' %lld rows (want 3), "
	    "n=2^40 %lld rows (want 2)\n", (long long)s, (long long)n);
	CK(s == 3);
	CK(n == 2);
	CK(q1(db, "SELECT count(*) FROM t WHERE s = 'wxyz'") == 2);
	CK(q1(db, "SELECT count(*) FROM t WHERE n = 1099511627777") == 2);
	CK(xstore_index_seek_count() > seeks);     /* read via the indexes */
	CK(xstore_index_check(sx_storage_bt(), &miss, &stale) == 2);
	printf("  xstore_index_check: %llu missing\n", (unsigned long long)miss);
	CK(miss == 0);
	sx_close(db);
	CK(sx_storage_close() == SX_OK);
	(void)sx_shutdown();
	for (i = 0; i < 4; i++) {
		snprintf(side, sizeof side, "%s%s", path, sfx[i]);
		unlink(side);
	}
	if (g_fail) {
		fprintf(stderr, "FAIL: test_gc_index\n");
		return 1;
	}
	printf("  ok   xstore_gc keeps 20-byte index entries\n");
	return 0;
}
