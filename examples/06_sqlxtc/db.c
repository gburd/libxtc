/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/db.c
 *	SQLite handle management + result streaming.
 */

#include "db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xtc_int.h"

int
db_create(const db_opts_t *opts, db_t **out)
{
	db_t *db;
	int   rc;

	db = (db_t *)calloc(1, sizeof(*db));
	if (!db) return XTC_E_NOMEM;

	db->path = opts->path ? opts->path : ":memory:";
	db->shared = opts->shared;
	db->res = opts->res;

	if (db->shared) {
		rc = sqlite3_open(db->path, &db->sdb);
		if (rc != SQLITE_OK) {
			fprintf(stderr, "sqlite3_open(%s): %s\n",
			        db->path, sqlite3_errmsg(db->sdb));
			sqlite3_close(db->sdb);
			free(db);
			return XTC_E_INVAL;
		}
		/* Pragma tuning -- enable WAL on file-backed databases.
		 * :memory: ignores this. */
		(void)sqlite3_exec(db->sdb,
		    "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
		(void)sqlite3_exec(db->sdb,
		    "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);
	}

	if (xtc_lrlock_create(sizeof(db_catalog_t), NULL, NULL,
	                      "sqlxtc.cat", &db->cat) == XTC_OK) {
		db_catalog_t *c = (db_catalog_t *)xtc_lrlock_write_begin(db->cat);
		if (c) {
			c->n_tables = 0;
			c->generation = 1;
			xtc_lrlock_publish_full_sync(db->cat);
			xtc_lrlock_write_end(db->cat);
		}
		xtc_lrlock_mark_ready(db->cat);
	}

	*out = db;
	return XTC_OK;
}

void
db_destroy(db_t *db)
{
	if (!db) return;
	if (db->cat) xtc_lrlock_destroy(db->cat);
	if (db->sdb) sqlite3_close(db->sdb);
	free(db);
}

int
db_handle_get(db_t *db, sqlite3 **out, int *out_owned)
{
	if (db->shared && db->sdb) {
		*out = db->sdb;
		*out_owned = 0;
		return XTC_OK;
	}
	{
		sqlite3 *h = NULL;
		int rc = sqlite3_open(db->path, &h);
		if (rc != SQLITE_OK) {
			if (h) sqlite3_close(h);
			return XTC_E_INVAL;
		}
		*out = h;
		*out_owned = 1;
		return XTC_OK;
	}
}

void
db_handle_put(db_t *db, sqlite3 *h, int owned)
{
	(void)db;
	if (owned && h) sqlite3_close(h);
}

int
db_exec(sqlite3 *h, const char *sql, int64_t limit,
        quack_buf_t *out_buf, int64_t *n_rows, char **err)
{
	sqlite3_stmt *stmt = NULL;
	int rc;
	int ncols;
	int64_t rows = 0;
	int wrote_cols = 0;

	rc = sqlite3_prepare_v2(h, sql, -1, &stmt, NULL);
	if (rc != SQLITE_OK) {
		const char *msg = sqlite3_errmsg(h);
		*err = strdup(msg ? msg : "prepare failed");
		if (stmt) sqlite3_finalize(stmt);
		return -1;
	}

	ncols = sqlite3_column_count(stmt);

	for (;;) {
		rc = sqlite3_step(stmt);
		if (rc == SQLITE_DONE) break;
		if (rc != SQLITE_ROW) {
			const char *msg = sqlite3_errmsg(h);
			*err = strdup(msg ? msg : "step failed");
			sqlite3_finalize(stmt);
			return -1;
		}

		if (!wrote_cols && ncols > 0) {
			int i;
			if (quack_emit_cols_begin(out_buf) < 0) goto oom;
			for (i = 0; i < ncols; i++) {
				const char *name = sqlite3_column_name(stmt, i);
				if (quack_emit_cols_name(out_buf, i, name) < 0)
					goto oom;
			}
			if (quack_emit_cols_end(out_buf) < 0) goto oom;
			wrote_cols = 1;
		}

		if (ncols > 0) {
			int i;
			if (quack_emit_row_begin(out_buf) < 0) goto oom;
			for (i = 0; i < ncols; i++) {
				int t = sqlite3_column_type(stmt, i);
				switch (t) {
				case SQLITE_INTEGER:
					if (quack_emit_row_int(out_buf, i,
					    sqlite3_column_int64(stmt, i)) < 0)
						goto oom;
					break;
				case SQLITE_FLOAT:
					if (quack_emit_row_double(out_buf, i,
					    sqlite3_column_double(stmt, i)) < 0)
						goto oom;
					break;
				case SQLITE_TEXT: {
					const unsigned char *s =
					    sqlite3_column_text(stmt, i);
					int n = sqlite3_column_bytes(stmt, i);
					if (quack_emit_row_text(out_buf, i,
					    (const char *)(s ? s :
					    (const unsigned char *)""), (size_t)n) < 0)
						goto oom;
					break;
				}
				case SQLITE_BLOB: {
					const void *p = sqlite3_column_blob(stmt, i);
					int n = sqlite3_column_bytes(stmt, i);
					if (quack_emit_row_blob(out_buf, i, p,
					    (size_t)n) < 0)
						goto oom;
					break;
				}
				case SQLITE_NULL:
				default:
					if (quack_emit_row_null(out_buf, i) < 0)
						goto oom;
					break;
				}
			}
			if (quack_emit_row_end(out_buf) < 0) goto oom;
		}

		rows++;
		if (limit > 0 && rows >= limit) break;
	}

	/* For DML/DDL with no result set, return changes() as the count. */
	if (ncols == 0) {
		rows = (int64_t)sqlite3_changes64(h);
	}

	if (quack_emit_done(out_buf, rows) < 0) goto oom;

	*n_rows = rows;
	sqlite3_finalize(stmt);
	return 0;

oom:
	*err = strdup("oom");
	sqlite3_finalize(stmt);
	return -1;
}
