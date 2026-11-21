/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_drop_id.c
 *	A dropped table's or index's id is never handed out again, and a
 *	new table never shows a dropped table's rows.  Ids were allocated
 *	as one past the largest catalog rowid; DROP TABLE only tombstoned
 *	the catalog row and left the rows, and both the clean close's
 *	version GC and the log dump of a crash rebuild removed the
 *	tombstone.  The next CREATE then took the dropped id and read the
 *	dropped table's rows as its own.
 *
 *	Four cases, each on a fresh store: a clean close and reopen; a
 *	crash after a log compaction (the rebuild replays the dump); a
 *	crash with the whole log (the rebuild replays the DROP itself);
 *	and a store that already lost a dropped table's tombstone while
 *	its rows stayed (what an older release leaves), whose next CREATE
 *	must still get a fresh id.
 *	Off a loop.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "btree.h"
#include "engine.h"
#include "xstore.h"
#include "xtc.h"
#include "t_tmp.h"

enum { REOPEN_CLEAN, REOPEN_CRASH_DUMP, REOPEN_CRASH_LOG };

static const char *const k_mode[] = { "clean close", "crash after a dump",
	"crash with the whole log" };

/* First column of the first row as an integer; -2 when the statement
 * fails (at prepare or at step). */
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

/* The id of table `name`, or 0 if it is unknown. */
static uint32_t
table_id(const char *name)
{
	uint32_t id = 0;
	return xstore_table_id(sx_storage_bt(), name, &id) ? id : 0;
}

/* The id of index `name` on `table`, or 0 if there is none. */
static uint32_t
index_id(const char *table, const char *name)
{
	xstore_index_info_t ix[8];
	int n = xstore_index_list(sx_storage_bt(), table, ix, 8), i;
	for (i = 0; i < n; i++)
		if (strcmp(ix[i].name, name) == 0) return ix[i].idx_id;
	return 0;
}

/* Close the store the way `mode` names, then open it again. */
static void
reopen(const char *path, int mode)
{
	if (mode == REOPEN_CLEAN)
		CK(sx_storage_close() == SX_OK);
	else {
		if (mode == REOPEN_CRASH_DUMP)
			CK(sx_storage_checkpoint() == SX_OK);
		sx_storage_abandon();
	}
	CK(sx_storage_open(path, 256) == SX_OK);
}

/* Remove a store's files. */
static void
unlink_store(const char *path)
{
	char p[320];
	unlink(path);
	snprintf(p, sizeof p, "%s-wal", path); unlink(p);
	snprintf(p, sizeof p, "%s.dwb", path); unlink(p);
}

/*
 * Tables a and b (b holds a row and an index), an extra index on a;
 * drop b and the extra index, close and reopen as `mode` says, then a
 * new table and a new index must take ids past every one handed out,
 * and the new table must be empty.
 */
static void
drop_case(int mode)
{
	char path[256];
	sx_db *h = NULL;
	uint32_t ta, tb, ia, ib, ix, td, inew;
	int fd;

	t_tmpl(path, sizeof path, "sqlxtc-dropid");
	if ((fd = mkstemp(path)) < 0) { CK(0); return; }
	close(fd);
	CK(sx_storage_open(path, 256) == SX_OK);
	CK(sx_open(":memory:", &h) == SX_OK);
	CK(sx_exec(h, "CREATE TABLE a(k INTEGER PRIMARY KEY, v TEXT)", NULL) == SX_OK);
	CK(sx_exec(h, "CREATE TABLE b(k INTEGER PRIMARY KEY, v TEXT)", NULL) == SX_OK);
	CK(sx_exec(h, "INSERT INTO b VALUES(1, 'old-b-row')", NULL) == SX_OK);
	CK(sx_exec(h, "CREATE INDEX a_v ON a(v)", NULL) == SX_OK);
	CK(sx_exec(h, "CREATE INDEX b_v ON b(v)", NULL) == SX_OK);
	CK(sx_exec(h, "CREATE INDEX a_k ON a(k)", NULL) == SX_OK);
	ta = table_id("a"); tb = table_id("b");
	ia = index_id("a", "a_v"); ib = index_id("b", "b_v");
	ix = index_id("a", "a_k");
	CK(ta != 0 && tb > ta && ia != 0 && ib > ia && ix > ib);
	CK(sx_exec(h, "DROP TABLE b", NULL) == SX_OK);
	CK(sx_exec(h, "DROP INDEX a_k", NULL) == SX_OK);
	sx_close(h);
	reopen(path, mode);

	CK(sx_open(":memory:", &h) == SX_OK);
	CK(sx_exec(h, "CREATE TABLE d(k INTEGER PRIMARY KEY, v TEXT)", NULL) == SX_OK);
	CK(sx_exec(h, "CREATE INDEX a_k2 ON a(k)", NULL) == SX_OK);
	td = table_id("d");
	inew = index_id("a", "a_k2");
	printf("  %-24s dropped table %u, index %u; new table %u, index %u\n",
	    k_mode[mode], tb, ix, td, inew);
	CK(td > tb);
	CK(inew > ix);
	CK(one(h, "SELECT count(*) FROM d") == 0);
	CK(one(h, "SELECT count(*) FROM b") == -2);
	sx_close(h);
	CK(sx_storage_close() == SX_OK);
	unlink_store(path);
}

/*
 * A store whose dropped table already lost its tombstone with its rows
 * left behind: rows under an id the catalog does not name.  The next
 * CREATE must not take that id.
 */
static void
orphan_case(void)
{
	char path[256];
	sx_db *h = NULL;
	uint8_t rec[8] = { 0 };
	uint32_t ta, td;
	int fd;

	t_tmpl(path, sizeof path, "sqlxtc-dropid");
	if ((fd = mkstemp(path)) < 0) { CK(0); return; }
	close(fd);
	CK(sx_storage_open(path, 256) == SX_OK);
	CK(sx_open(":memory:", &h) == SX_OK);
	CK(sx_exec(h, "CREATE TABLE a(k INTEGER PRIMARY KEY, v TEXT)", NULL) == SX_OK);
	ta = table_id("a");
	CK(xstore_put_rec(sx_storage_bt(), ta + 4, 1, rec, (int)sizeof rec) == 0);
	sx_close(h);
	CK(sx_storage_close() == SX_OK);
	CK(sx_storage_open(path, 256) == SX_OK);
	CK(sx_open(":memory:", &h) == SX_OK);
	CK(sx_exec(h, "CREATE TABLE d(k INTEGER PRIMARY KEY, v TEXT)", NULL) == SX_OK);
	td = table_id("d");
	printf("  %-24s rows left under table %u; new table %u\n",
	    "lost tombstone", ta + 4, td);
	CK(td > ta + 4);
	CK(one(h, "SELECT count(*) FROM d") == 0);
	sx_close(h);
	CK(sx_storage_close() == SX_OK);
	unlink_store(path);
}

int
main(void)
{
	CK(sx_init() == SX_OK);
	drop_case(REOPEN_CLEAN);
	drop_case(REOPEN_CRASH_DUMP);
	drop_case(REOPEN_CRASH_LOG);
	orphan_case();
	if (g_fail) { fprintf(stderr, "FAIL: test_drop_id\n"); return 1; }
	printf("  ok   a dropped table's or index's id is never reused\n");
	return 0;
}
