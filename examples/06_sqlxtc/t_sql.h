/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/t_sql.h
 *	Test-only helpers shared by the sqlxtc test programs (the ones
 *	that drive SQL through xsql + the xstore virtual table over a
 *	bufmgr-backed B-tree).  Collects what was being copy-pasted into
 *	every test: the CK() check macro and its g_fail flag, the
 *	bm_create + bt_open / teardown pair, and the small SQL query
 *	helpers (exec, sel_v, val_of, eval_int, eval_int64).
 *
 *	Header-only and static-inline so each test still links
 *	standalone.  Include t_tmp.h alongside this for the temp-path
 *	template.  A test that needs a variant (a different sel_v
 *	signature, a custom bm_opts) just defines its own locally; these
 *	cover the common cases.
 */
#ifndef SQLXTC_T_SQL_H
#define SQLXTC_T_SQL_H

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sqlite3.h"
#include "bufmgr.h"
#include "btree.h"
#include "t_tmp.h"     /* CK() + g_fail + t_tmpl() */

/* -------------------------------------------------------------------------
 * Storage open / close.  Most tests open a bufmgr over a fresh temp file
 * and a B-tree on top.  t_open does both; t_close tears them down and
 * unlinks the file.  page_size / n_frames / cool_pct are the common
 * defaults; a test wanting other geometry calls bm_create / bt_open
 * itself.
 * ----------------------------------------------------------------------- */
#define T_PAGE_SZ   4096
#define T_N_FRAMES  64

static inline int
t_open(const char *path, bm_t **bm_out, bt_t **bt_out)
{
	bm_opts_t bo = BM_OPTS_DEFAULT;
	bm_t *bm = NULL;
	bt_t *bt = NULL;

	bo.path = path;
	bo.page_size = T_PAGE_SZ;
	bo.n_frames = T_N_FRAMES;
	bo.cool_pct = 25;
	if (bm_create(&bo, &bm) != XTC_OK)
		return -1;
	if (bt_open(bm, &bt) != XTC_OK) {
		bm_destroy(bm);
		return -1;
	}
	*bm_out = bm;
	*bt_out = bt;
	return 0;
}

static inline void
t_close(bm_t *bm, bt_t *bt, const char *path)
{
	if (bt != NULL)
		bt_close(bt);
	if (bm != NULL)
		bm_destroy(bm);
	if (path != NULL) {
		char side[300];
		(void)remove(path);
		(void)snprintf(side, sizeof side, "%s-wal", path);
		(void)remove(side);
		(void)snprintf(side, sizeof side, "%s.dwb", path);
		(void)remove(side);
	}
}

/* -------------------------------------------------------------------------
 * SQL query helpers.  These run a statement and pull a single result.
 * ----------------------------------------------------------------------- */

/* Run a statement for effect; on error print it and set g_fail. */
static inline void
t_exec(xsql *db, const char *sql)
{
	char *err = NULL;
	if (xsql_exec(db, sql, 0, 0, &err) != SQLITE_OK) {
		fprintf(stderr, "FAIL exec [%s]: %s\n", sql, err ? err : "?");
		g_fail = 1;
	}
	if (err != NULL)
		xsql_free(err);
}

/* First column of the first row as int (32-bit); -1 if no row/error. */
static inline int
t_eval_int(xsql *db, const char *sql)
{
	xsql_stmt *st = NULL;
	int v = -1;
	if (xsql_prepare_v2(db, sql, -1, &st, 0) != SQLITE_OK)
		return -1;
	if (xsql_step(st) == SQLITE_ROW)
		v = xsql_column_int(st, 0);
	xsql_finalize(st);
	return v;
}

/* Like t_eval_int but 64-bit (commit timestamps exceed 32 bits). */
static inline int64_t
t_eval_int64(xsql *db, const char *sql)
{
	xsql_stmt *st = NULL;
	int64_t v = -1;
	if (xsql_prepare_v2(db, sql, -1, &st, 0) != SQLITE_OK)
		return -1;
	if (xsql_step(st) == SQLITE_ROW)
		v = xsql_column_int64(st, 0);
	xsql_finalize(st);
	return v;
}

/* Value of v for key k in table t, as a NUL-terminated string in out.
 * Returns 1 on a hit (out filled), 0 on a miss, -1 on a prepare error. */
static inline int
t_sel_v(xsql *db, int64_t k, char *out, size_t cap)
{
	xsql_stmt *st = NULL;
	int got = 0;
	if (xsql_prepare_v2(db, "SELECT v FROM t WHERE k=?", -1, &st, 0)
	    != SQLITE_OK)
		return -1;
	xsql_bind_int64(st, 1, k);
	if (xsql_step(st) == SQLITE_ROW) {
		const unsigned char *t = xsql_column_text(st, 0);
		size_t n = (size_t)xsql_column_bytes(st, 0);
		if (n >= cap)
			n = cap - 1;
		if (t != NULL)
			memcpy(out, t, n);
		out[n] = '\0';
		got = 1;
	}
	xsql_finalize(st);
	return got;
}

/* Integer value of v for key k in table t, or -1 if absent. */
static inline int64_t
t_val_of(xsql *db, int64_t k)
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

/* Open a :memory: connection that shares the engine B-tree `bt` (each
 * connection re-declares the xstore virtual table `t`; the table-id
 * catalog keeps them consistent).  Returns NULL on failure.  Used by
 * the multi-connection isolation/MVCC tests. */
static inline xsql *
t_open_conn(bt_t *bt)
{
	xsql *db = NULL;
	if (xsql_open(":memory:", &db) != SQLITE_OK)
		return NULL;
	if (xstore_register(db, bt) != SQLITE_OK) {
		xsql_close(db);
		return NULL;
	}
	(void)xsql_exec(db, "CREATE VIRTUAL TABLE t USING xstore;", 0, 0, 0);
	return db;
}

#endif /* SQLXTC_T_SQL_H */
