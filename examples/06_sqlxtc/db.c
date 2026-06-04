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

/* Decode a hex or base64 blob param into a freshly malloc'd buffer.
 * Returns 0 and sets *out/*outn (caller frees *out), or -1 on a
 * malformed encoding. */
static int
hexval(int c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static int
b64val(int c)
{
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+') return 62;
	if (c == '/') return 63;
	return -1;
}

static int
decode_blob(const struct quack_param *p, unsigned char **out, int *outn)
{
	*out = NULL; *outn = 0;
	if (!p->blob_b64) {
		/* hex */
		size_t i, n;
		unsigned char *b;
		if ((p->slen & 1u) != 0) return -1;
		n = p->slen / 2;
		b = malloc(n ? n : 1);
		if (b == NULL) return -1;
		for (i = 0; i < n; i++) {
			int hi = hexval((unsigned char)p->sval[2 * i]);
			int lo = hexval((unsigned char)p->sval[2 * i + 1]);
			if (hi < 0 || lo < 0) { free(b); return -1; }
			b[i] = (unsigned char)((hi << 4) | lo);
		}
		*out = b; *outn = (int)n;
		return 0;
	} else {
		/* base64 (no embedded whitespace; optional '=' padding) */
		size_t i = 0, slen = p->slen;
		unsigned char *b;
		size_t bn = 0;
		uint32_t acc = 0; int nbits = 0;
		while (slen > 0 && p->sval[slen - 1] == '=') slen--;
		b = malloc((slen / 4) * 3 + 3);
		if (b == NULL) return -1;
		for (i = 0; i < slen; i++) {
			int v = b64val((unsigned char)p->sval[i]);
			if (v < 0) { free(b); return -1; }
			acc = (acc << 6) | (uint32_t)v;
			nbits += 6;
			if (nbits >= 8) {
				nbits -= 8;
				b[bn++] = (unsigned char)((acc >> nbits) & 0xff);
			}
		}
		*out = b; *outn = (int)bn;
		return 0;
	}
}

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
	return db_exec_params(h, sql, NULL, 0, limit, out_buf, n_rows, err);
}

int
db_exec_params(sx_db *h, const char *sql,
        const struct quack_param *params, int n_params, int64_t limit,
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

	/* Bind parameters (?1..?N) from the request, if any. */
	if (n_params > 0) {
		int pi;
		for (pi = 0; pi < n_params; pi++) {
			const struct quack_param *p = &params[pi];
			int idx = pi + 1;
			switch (p->type) {
			case QUACK_P_INT:
				rc = sx_bind_int64(stmt, idx, p->ival); break;
			case QUACK_P_FLOAT:
				rc = sx_bind_double(stmt, idx, p->dval); break;
			case QUACK_P_TEXT:
				rc = sx_bind_text(stmt, idx, p->sval,
				    (int)p->slen); break;
			case QUACK_P_BLOB: {
				unsigned char *bb = NULL; int bn = 0;
				if (decode_blob(p, &bb, &bn) != 0) {
					*err = strdup("bad blob param");
					sx_finalize(stmt);
					return -1;
				}
				rc = sx_bind_blob(stmt, idx, bb, bn);
				free(bb);
				break;
			}
			case QUACK_P_NULL:
			default:
				rc = sx_bind_null(stmt, idx); break;
			}
			if (rc != SX_OK) {
				*err = strdup("bind failed");
				sx_finalize(stmt);
				return -1;
			}
		}
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
