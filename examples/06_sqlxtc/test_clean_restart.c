/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_clean_restart.c
 *	Prove the clean-restart fast path actually TRUSTS the base rather
 *	than silently rebuilding.  We populate a store, shut it down
 *	cleanly (which flushes the base and marks its superblock clean),
 *	then DELETE the write-ahead log so a rebuild is impossible, and
 *	reopen.  If the rows are still there, recovery must have trusted
 *	the on-disk base and skipped replay -- the ARIES clean-restart
 *	case.  A second cycle confirms a normal clean reopen (log intact)
 *	also serves the data, and that new commits after a trusted reopen
 *	remain durable.  Plain asserts; off-loop (synchronous bufmgr I/O).
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "engine.h"
#include "t_tmp.h"


/* Open a fresh handle, count rows in t (creating the vtab binding). -1 on err. */
static int
count_rows(void)
{
	sx_db *h = NULL;
	sx_stmt *st = NULL;
	int n = -1;
	if (sx_open(":memory:", &h) != SX_OK)
		return -1;
	(void)sx_exec(h, "CREATE VIRTUAL TABLE t USING xstore;", NULL);
	if (sx_prepare(h, "SELECT count(*) FROM t", -1, &st, NULL) == SX_OK) {
		if (sx_step(st) == SX_ROW) n = (int)sx_column_int64(st, 0);
		sx_finalize(st);
	}
	sx_close(h);
	return n;
}

int
main(void)
{
	char path[256], wal[300];
	sx_db *h = NULL;
	int i;

	t_tmpl(path, sizeof path, "cleanrestart");
	{ int fd = mkstemp(path); if (fd >= 0) close(fd); }
	snprintf(wal, sizeof wal, "%s-wal", path);

	CK(sx_init() == SX_OK);

	/* ---- populate, then shut down cleanly ---- */
	CK(sx_storage_open(path, 64) == SX_OK);
	CK(sx_open(":memory:", &h) == SX_OK);
	CK(sx_exec(h, "CREATE VIRTUAL TABLE t USING xstore;", NULL) == SX_OK);
	for (i = 0; i < 50; i++) {
		char sql[64];
		snprintf(sql, sizeof sql, "INSERT INTO t(k,v) VALUES(%d,'row%d');", i, i);
		CK(sx_exec(h, sql, NULL) == SX_OK);
	}
	sx_close(h);
	sx_storage_close();                       /* flushes base + marks clean */

	/* ---- delete the WAL: a rebuild is now impossible ---- */
	CK(unlink(wal) == 0);

	/* ---- reopen: the only way the rows survive is trusting the base ---- */
	CK(sx_storage_open(path, 64) == SX_OK);
	CK(count_rows() == 50);                   /* trusted the clean base */

	/* new commits after a trusted reopen are durable too */
	CK(sx_open(":memory:", &h) == SX_OK);
	(void)sx_exec(h, "CREATE VIRTUAL TABLE t USING xstore;", NULL);
	for (i = 50; i < 60; i++) {
		char sql[64];
		snprintf(sql, sizeof sql, "INSERT INTO t(k,v) VALUES(%d,'row%d');", i, i);
		CK(sx_exec(h, sql, NULL) == SX_OK);
	}
	sx_close(h);
	sx_storage_close();

	/* ---- ordinary clean reopen (log intact) serves all 60 ---- */
	CK(sx_storage_open(path, 64) == SX_OK);
	CK(count_rows() == 60);
	sx_storage_close();

	(void)sx_shutdown();

	unlink(path); unlink(wal);
	{ char s[300]; snprintf(s, sizeof s, "%s.dwb", path); unlink(s); }

	if (g_fail) { fprintf(stderr, "test_clean_restart: FAILED\n"); return 1; }
	printf("test_clean_restart: clean base trusted (no replay); commits durable\n");
	return 0;
}
