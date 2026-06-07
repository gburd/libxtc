/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_steal.c
 *	STEAL: a transaction larger than the in-memory write-buffer
 *	threshold spills its buffered payloads to the on-disk staging
 *	area (and the log) during execution, bounding its resident
 *	footprint.  This test drives transactions well past the spill
 *	threshold and checks three outcomes:
 *
 *	  commit   -- every row is present after commit and after a
 *	              reopen (the spilled payloads were re-materialized
 *	              and committed under the commit timestamp).
 *	  rollback -- no rows remain.
 *	  crash    -- a large transaction that never commits is undone by
 *	              recovery, writing a compensation record (CLR) per
 *	              spilled payload.  xstore_undo_clrs proves the undo
 *	              pass ran for real, and the rows are absent.
 *
 *	Off-loop (synchronous bufmgr/WAL I/O); plain asserts.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "engine.h"
#include "t_tmp.h"

extern unsigned long long xstore_undo_clrs(void);   /* uint64_t */

static int g_fail;
#define CK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

#define NROW   3000
#define VLEN   250            /* row payload; NROW*VLEN ~ 730 KB >> 256 KB spill */

static char g_val[VLEN + 1];

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

/* Insert NROW big rows inside one explicit transaction on a fresh handle.
 * fin: "COMMIT", "ROLLBACK", or NULL (leave open -- caller crashes). */
static sx_db *
big_txn(const char *fin, int do_create, int base)
{
	sx_db *h = NULL;
	int i;
	if (sx_open(":memory:", &h) != SX_OK)
		return NULL;
	if (do_create)
		(void)sx_exec(h, "CREATE VIRTUAL TABLE t USING xstore;", NULL);
	(void)sx_exec(h, "BEGIN;", NULL);
	for (i = 0; i < NROW; i++) {
		char sql[VLEN + 64];
		char *err = NULL;
		int rc;
		snprintf(sql, sizeof sql, "INSERT INTO t(k,v) VALUES(%d,'%s');",
		    base + i, g_val);
		rc = sx_exec(h, sql, &err);
		if (rc != SX_OK) { fprintf(stderr, "insert %d rc=%d err=%s\n", base+i, rc, err?err:"-"); CK(0); break; }
	}
	if (fin != NULL) {
		char b[16];
		snprintf(b, sizeof b, "%s;", fin);
		CK(sx_exec(h, b, NULL) == SX_OK);
	}
	return h;
}

int
main(void)
{
	char path[256], wal[300], dwb[300];
	sx_db *h;
	unsigned long long clrs0;

	memset(g_val, 'x', VLEN); g_val[VLEN] = '\0';
	t_tmpl(path, sizeof path, "steal");
	{ int fd = mkstemp(path); if (fd >= 0) close(fd); }
	snprintf(wal, sizeof wal, "%s-wal", path);
	snprintf(dwb, sizeof dwb, "%s.dwb", path);

	CK(sx_init() == SX_OK);

	/* ---- commit a large (spilling) transaction ---- */
	CK(sx_storage_open(path, 64) == SX_OK);
	h = big_txn("COMMIT", 1, 0);
	CK(h != NULL);
	sx_close(h);
	CK(count_rows() == NROW);                 /* present after commit */
	sx_storage_close();
	CK(sx_storage_open(path, 64) == SX_OK);
	CK(count_rows() == NROW);                 /* present after reopen */
	sx_storage_close();

	/* ---- rollback a large transaction ---- */
	CK(sx_storage_open(path, 64) == SX_OK);
	h = big_txn("ROLLBACK", 1, 10000);        /* distinct keys */
	CK(h != NULL);
	sx_close(h);
	CK(count_rows() == NROW);                 /* still just the committed set */
	sx_storage_close();

	/* ---- crash mid large transaction: recovery undoes it with CLRs ---- */
	clrs0 = xstore_undo_clrs();
	CK(sx_storage_open(path, 64) == SX_OK);
	h = big_txn(NULL, 1, 20000);               /* leave the txn open */
	CK(h != NULL);
	sx_storage_abandon();                      /* CRASH: no commit, no checkpoint */
	/* h now dangles (its storage is gone); intentionally not closed. */

	CK(sx_storage_open(path, 64) == SX_OK);    /* reopen -> recovery */
	CK(xstore_undo_clrs() > clrs0);            /* CLRs were written during undo */
	CK(count_rows() == NROW);                  /* the crashed txn left no rows */
	sx_storage_close();

	(void)sx_shutdown();
	unlink(path); unlink(wal); unlink(dwb);

	if (g_fail) { fprintf(stderr, "test_steal: FAILED\n"); return 1; }
	printf("test_steal: large txn spills; commit/rollback correct; crash undone with CLRs\n");
	return 0;
}
