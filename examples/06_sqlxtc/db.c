/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/db.c
 *	Engine-handle management + result streaming.  Speaks the sx_
 *	engine API only (engine.h); no vendored-engine symbols here.
 */

#include "db.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xtc_int.h"

static int
is_memory(const char *path)
{
	return path == NULL || path[0] == '\0' || strcmp(path, ":memory:") == 0;
}

int
db_create(const db_opts_t *opts, db_t **out)
{
	db_t *db;

	db = (db_t *)calloc(1, sizeof(*db));
	if (!db) return XTC_E_NOMEM;

	db->path = opts->path ? opts->path : ":memory:";
	db->res = opts->res;

	/* An in-memory database has no file to share, so every private
	 * handle would be a separate empty database: force one shared
	 * handle.  A file-backed database defaults to a private handle
	 * per connection -- concurrent executions, WAL readers running
	 * alongside a writer -- unless the caller asked to share one. */
	if (is_memory(db->path))
		db->shared = 1;
	else
		db->shared = opts->shared;

	if (db->shared) {
		int rc = sx_open(db->path, &db->sdb);
		if (rc != SX_OK) {
			fprintf(stderr, "sx_open(%s): %s\n",
			        db->path, sx_errmsg(db->sdb));
			sx_close(db->sdb);
			free(db);
			return XTC_E_INVAL;
		}
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
	if (db->sdb) sx_close(db->sdb);
	free(db);
}

int
db_handle_get(db_t *db, sx_db **out, int *out_owned)
{
	if (db->shared && db->sdb) {
		*out = db->sdb;
		*out_owned = 0;
		return XTC_OK;
	}
	{
		sx_db *h = NULL;
		int rc = sx_open(db->path, &h);
		if (rc != SX_OK) {
			if (h) sx_close(h);
			return XTC_E_INVAL;
		}
		*out = h;
		*out_owned = 1;
		return XTC_OK;
	}
}

void
db_handle_put(db_t *db, sx_db *h, int owned)
{
	(void)db;
	if (owned && h) sx_close(h);
}

int
db_exec(sx_db *h, const char *sql, int64_t limit,
        quack_buf_t *out_buf, int64_t *n_rows, char **err)
{
	sx_stmt *stmt = NULL;
	int rc;
	int ncols;
	int64_t rows = 0;
	int wrote_cols = 0;

	rc = sx_prepare(h, sql, -1, &stmt, NULL);
	if (rc != SX_OK) {
		const char *msg = sx_errmsg(h);
		*err = strdup(msg ? msg : "prepare failed");
		if (stmt) sx_finalize(stmt);
		return -1;
	}

	ncols = sx_column_count(stmt);

	for (;;) {
		rc = sx_step(stmt);
		if (rc == SX_DONE) break;
		if (rc != SX_ROW) {
			const char *msg = sx_errmsg(h);
			*err = strdup(msg ? msg : "step failed");
			sx_finalize(stmt);
			return -1;
		}

		if (!wrote_cols && ncols > 0) {
			int i;
			if (quack_emit_cols_begin(out_buf) < 0) goto oom;
			for (i = 0; i < ncols; i++) {
				const char *name = sx_column_name(stmt, i);
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
				int t = sx_column_type(stmt, i);
				switch (t) {
				case SX_INTEGER:
					if (quack_emit_row_int(out_buf, i,
					    sx_column_int64(stmt, i)) < 0)
						goto oom;
					break;
				case SX_FLOAT:
					if (quack_emit_row_double(out_buf, i,
					    sx_column_double(stmt, i)) < 0)
						goto oom;
					break;
				case SX_TEXT: {
					const char *s = sx_column_text(stmt, i);
					int n = sx_column_bytes(stmt, i);
					if (quack_emit_row_text(out_buf, i,
					    s ? s : "", (size_t)n) < 0)
						goto oom;
					break;
				}
				case SX_BLOB: {
					const void *p = sx_column_blob(stmt, i);
					int n = sx_column_bytes(stmt, i);
					if (quack_emit_row_blob(out_buf, i, p,
					    (size_t)n) < 0)
						goto oom;
					break;
				}
				case SX_NULL:
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
	if (ncols == 0)
		rows = sx_changes(h);

	if (quack_emit_done(out_buf, rows) < 0) goto oom;

	*n_rows = rows;
	sx_finalize(stmt);
	return 0;

oom:
	*err = strdup("oom");
	sx_finalize(stmt);
	return -1;
}
