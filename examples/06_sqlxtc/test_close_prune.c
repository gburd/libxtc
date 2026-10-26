/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_close_prune.c
 *	A clean close leaves one version per live row (sx_storage_close
 *	runs the version GC before the final flush).
 *
 *	Autovacuum prunes a row's old versions inline only while prunes
 *	keep paying (it backs off to 1 write in 256 when they do not), so
 *	right after a load a row updated several times still holds those
 *	versions; the ledger load left ~5 per account row on disk.  With no
 *	connection open there is no snapshot to keep them for.
 *	(1) 2000 rows updated 6 times each in their own transactions, an
 *	    open/close cycle: the reopened tree holds exactly 2000 version
 *	    entries for the table (was ~2000 x 7 less what autovacuum
 *	    took), and every row reads back its last value.
 *	(2) 500 deleted rows leave no version entry (their tombstones go
 *	    too), and a count still sees 1500 rows.
 *	(3) An index whose entry keys are exactly 20 bytes long, the
 *	    length of a version key ([0xFE][index id:4][one 6-byte number
 *	    element:7][rowid:8]), keeps every entry: the GC stops at the
 *	    index namespace instead of reading its keys as versions.  Read
 *	    as one, an entry's "commit ts" is ~(rowid ^ 2^63), small for a
 *	    rowid near INT64_MAX, so two such rows with the same value
 *	    looked like an old and a new version of one row, and the GC
 *	    deleted the "old" entry: the index lost a row.
 *	Off a loop (synchronous storage).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "btree.h"
#include "engine.h"
#include "xstore.h"
#include "t_tmp.h"

#define NROW 2000
#define NUPD 6

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

/* Version entries (20-byte keys) of the user tables in the open tree. */
static long
versions(void)
{
	bt_cursor_t *cur = NULL;
	const void *k, *v;
	uint16_t kl, vl;
	long n = 0;
	uint8_t start[1] = { 0 };
	if (bt_cursor_open(sx_storage_bt(), start, 1, &cur) != XTC_OK)
		return -1;
	while (bt_cursor_next(cur, &k, &kl, &v, &vl) == XTC_OK) {
		const uint8_t *b = k;
		uint32_t tid;
		if (kl != 20)
			continue;
		tid = (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 |
		    (uint32_t)b[2] << 8 | b[3];
		if (tid >= 1 && tid < 0xFFFFFF00u)   /* user tables only */
			n++;
	}
	bt_cursor_close(cur);
	return n;
}

int
main(void)
{
	char path[256], side[300], sql[128];
	static const char *sfx[] = { "", ".xstore", ".xstore-wal", ".xstore.lock" };
	sx_db *db = NULL;
	long nv;
	int fd, i, j;

	printf("test_close_prune\n");
	t_tmpl(path, sizeof path, "sqlxtc-cprune");
	if ((fd = mkstemp(path)) >= 0)
		close(fd);
	CK(sx_init() == SX_OK);
	CK(sx_storage_open(path, 4096) == SX_OK);
	CK(sx_open(":memory:", &db) == SX_OK);
	if (db == NULL)
		return 1;
	CK(sx_exec(db, "CREATE TABLE t(k INTEGER PRIMARY KEY, v INT)", NULL) == SX_OK);
	CK(sx_exec(db, "BEGIN", NULL) == SX_OK);
	for (i = 0; i < NROW; i++) {
		snprintf(sql, sizeof sql, "INSERT INTO t VALUES(%d, 0)", i);
		CK(sx_exec(db, sql, NULL) == SX_OK);
	}
	CK(sx_exec(db, "COMMIT", NULL) == SX_OK);
	for (j = 1; j <= NUPD; j++)
		for (i = 0; i < NROW; i++) {
			snprintf(sql, sizeof sql,
			    "UPDATE t SET v = %d WHERE k = %d", j * 1000 + i, i);
			CK(sx_exec(db, sql, NULL) == SX_OK);
		}
	nv = versions();
	printf("  before close: %ld version entries for %d rows\n", nv, NROW);
	sx_close(db);
	CK(sx_storage_close() == SX_OK);

	/* (1) */
	CK(sx_storage_open(path, 4096) == SX_OK);
	nv = versions();
	printf("  after a clean close: %ld version entries for %d rows\n",
	    nv, NROW);
	CK(nv == NROW);
	CK(sx_open(":memory:", &db) == SX_OK);
	if (db == NULL)
		return 1;
	(void)sx_exec(db, "CREATE TABLE t(k INTEGER PRIMARY KEY, v INT)", NULL);
	CK(q1(db, "SELECT count(*) FROM t") == NROW);
	snprintf(sql, sizeof sql, "SELECT count(*) FROM t WHERE v = %d + k",
	    NUPD * 1000);
	CK(q1(db, sql) == NROW);

	/* (2) */
	CK(sx_exec(db, "DELETE FROM t WHERE k < 500", NULL) == SX_OK);
	sx_close(db);
	CK(sx_storage_close() == SX_OK);
	CK(sx_storage_open(path, 4096) == SX_OK);
	nv = versions();
	printf("  after deleting 500 and a clean close: %ld version entries\n",
	    nv);
	CK(nv == NROW - 500);
	CK(sx_open(":memory:", &db) == SX_OK);
	if (db != NULL) {
		(void)sx_exec(db, "CREATE TABLE t(k INTEGER PRIMARY KEY, v INT)",
		    NULL);
		CK(q1(db, "SELECT count(*) FROM t") == NROW - 500);
		CK(q1(db, "SELECT min(k) FROM t") == 500);
		sx_close(db);
	}
	CK(sx_storage_close() == SX_OK);

	/* (3) v = 2^40 + k: a 6-byte number element */
	CK(sx_storage_open(path, 4096) == SX_OK);
	CK(sx_open(":memory:", &db) == SX_OK);
	if (db == NULL)
		return 1;
	(void)sx_exec(db, "CREATE TABLE t(k INTEGER PRIMARY KEY, v INT)", NULL);
	CK(sx_exec(db, "UPDATE t SET v = 1099511627776 + k", NULL) == SX_OK);
	CK(sx_exec(db, "INSERT INTO t VALUES(9223372036854775802, 1099511627776),"
	    " (9223372036854775803, 1099511627776)", NULL) == SX_OK);
	CK(sx_exec(db, "CREATE INDEX tv ON t(v)", NULL) == SX_OK);
	CK(q1(db, "SELECT count(*) FROM t WHERE v >= 1099511627776") ==
	    NROW - 500 + 2);
	sx_close(db);
	CK(sx_storage_close() == SX_OK);
	CK(sx_storage_open(path, 4096) == SX_OK);
	CK(sx_open(":memory:", &db) == SX_OK);
	if (db != NULL) {
		(void)sx_exec(db, "CREATE TABLE t(k INTEGER PRIMARY KEY, v INT)",
		    NULL);
		uint64_t seeks = xstore_index_seek_count(), miss = 0, stale = 0;
		nv = q1(db, "SELECT count(*) FROM t WHERE v >= 1099511627776");
		printf("  20-byte index entries after a clean close: %ld rows "
		    "found through the index (want %d)\n", nv, NROW - 500 + 2);
		CK(nv == NROW - 500 + 2);
		CK(xstore_index_seek_count() > seeks);    /* via the index */
		CK(xstore_index_check(sx_storage_bt(), &miss, &stale) == 1);
		CK(miss == 0);
		CK(q1(db, "SELECT count(*) FROM t WHERE v = 1099511627776 + 777")
		    == 1);
		nv = q1(db, "SELECT count(*) FROM t WHERE v = 1099511627776");
		printf("  two rows near INT64_MAX with one value: %ld found "
		    "through the index (want 2)\n", nv);
		CK(nv == 2);
		sx_close(db);
	}
	CK(sx_storage_close() == SX_OK);
	(void)sx_shutdown();
	for (i = 0; i < 4; i++) {
		snprintf(side, sizeof side, "%s%s", path, sfx[i]);
		unlink(side);
	}
	if (g_fail) {
		fprintf(stderr, "FAIL: test_close_prune\n");
		return 1;
	}
	printf("  ok   a clean close keeps one version per live row\n");
	return 0;
}
