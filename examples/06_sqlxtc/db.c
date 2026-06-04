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
#include <unistd.h>

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

	/* An in-memory database has no file to share, so separate handles
	 * would each be a distinct empty database.  Single-handle mode (the
	 * default) uses one shared handle for all connections.  Parallel
	 * (connection-per-proc) mode instead wants a private handle per
	 * connection running on its own loop; for :memory: that requires a
	 * shared-cache URI so the handles share one in-memory database
	 * (anchored by a keep-alive handle so it outlives any connection).
	 * A file-backed database shares naturally (WAL: concurrent readers
	 * + one writer), so per-connection handles need no special URI. */
	if (opts->parallel) {
		db->shared = 0;
		if (is_memory(db->path)) {
			/* Connection-per-proc needs a backing store the private
			 * per-connection handles can share; a bare :memory: db
			 * cannot span handles, and this build omits shared-cache.
			 * Use a unique temp file in WAL mode (concurrent readers +
			 * one writer); prefer tmpfs (/dev/shm) so it stays RAM-
			 * backed and behaves like :memory: -- ephemeral, removed on
			 * shutdown. */
			char p[160];
			const char *dir = (access("/dev/shm", W_OK) == 0)
			    ? "/dev/shm" : "/tmp";
			(void)snprintf(p, sizeof p, "%s/sqlxtc-%ld-%p.db",
			    dir, (long)getpid(), (void *)db);
			db->open_path = strdup(p);
			if (db->open_path == NULL) { free(db); return XTC_E_NOMEM; }
			db->owns_temp = 1;
			/* Anchor handle: initialise + keep the temp db warm for the
			 * server's lifetime. */
			if (sx_open(db->open_path, &db->anchor) != SX_OK) {
				if (db->anchor) sx_close(db->anchor);
				free(db->open_path); free(db);
				return XTC_E_INVAL;
			}
		} else {
			db->open_path = strdup(db->path);
			if (db->open_path == NULL) { free(db); return XTC_E_NOMEM; }
		}
	} else if (is_memory(db->path)) {
		db->shared = 1;
	} else {
		db->shared = opts->shared;
		if (!db->shared) {
			db->open_path = strdup(db->path);
			if (db->open_path == NULL) { free(db); return XTC_E_NOMEM; }
		}
	}

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
	if (db->anchor) sx_close(db->anchor);   /* drop the temp/anchor handle last */
	/* Remove an ephemeral parallel-:memory: temp file + its WAL/SHM. */
	if (db->owns_temp && db->open_path) {
		char side[176];
		(void)unlink(db->open_path);
		(void)snprintf(side, sizeof side, "%s-wal", db->open_path);
		(void)unlink(side);
		(void)snprintf(side, sizeof side, "%s-shm", db->open_path);
		(void)unlink(side);
	}
	free(db->open_path);
	free(db);
}

int
db_conn_open(db_t *db, sx_db **out, int *out_owned)
{
	if (db->shared && db->sdb) {
		*out = db->sdb;
		*out_owned = 0;
		return XTC_OK;
	}
	{
		sx_db *h = NULL;
		const char *p = db->open_path ? db->open_path : db->path;
		int rc = sx_open(p, &h);
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
db_conn_close(db_t *db, sx_db *h, int owned)
{
	(void)db;
	if (owned && h) sx_close(h);
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

/* Bind ?1..?N from the request's params.  Returns 0 or -1 (*err set). */
static int
bind_params(sx_stmt *stmt, const struct quack_param *params, int n_params,
            char **err)
{
	int pi, rc;
	for (pi = 0; pi < n_params; pi++) {
		const struct quack_param *p = &params[pi];
		int idx = pi + 1;
		switch (p->type) {
		case QUACK_P_INT:
			rc = sx_bind_int64(stmt, idx, p->ival); break;
		case QUACK_P_FLOAT:
			rc = sx_bind_double(stmt, idx, p->dval); break;
		case QUACK_P_TEXT:
			rc = sx_bind_text(stmt, idx, p->sval, (int)p->slen); break;
		case QUACK_P_BLOB: {
			unsigned char *bb = NULL; int bn = 0;
			if (decode_blob(p, &bb, &bn) != 0) {
				*err = strdup("bad blob param"); return -1;
			}
			rc = sx_bind_blob(stmt, idx, bb, bn);
			free(bb);
			break;
		}
		case QUACK_P_NULL:
		default:
			rc = sx_bind_null(stmt, idx); break;
		}
		if (rc != SX_OK) { *err = strdup("bind failed"); return -1; }
	}
	return 0;
}

/* Step a prepared statement to completion.  When `emit` is set, write
 * the column header (once) and each row to out_buf; otherwise step for
 * side effects only (multi-statement: non-final statements).  Sets
 * *rows_out to the streamed row count.  Returns 0 or -1 (*err set). */
static int
exec_stmt(sx_db *h, sx_stmt *stmt, int64_t limit, int emit,
          quack_buf_t *out_buf, int *ncols_out, int64_t *rows_out,
          char **err)
{
	int ncols = sx_column_count(stmt);
	int wrote_cols = 0;
	int64_t rows = 0;
	int rc;

	*ncols_out = ncols;
	for (;;) {
		rc = sx_step(stmt);
		if (rc == SX_DONE) break;
		if (rc != SX_ROW) {
			const char *msg = sx_errmsg(h);
			*err = strdup(msg ? msg : "step failed");
			return -1;
		}
		if (!emit) { rows++; continue; }   /* consume silently */

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
					    sx_column_int64(stmt, i)) < 0) goto oom;
					break;
				case SX_FLOAT:
					if (quack_emit_row_double(out_buf, i,
					    sx_column_double(stmt, i)) < 0) goto oom;
					break;
				case SX_TEXT: {
					const char *s = sx_column_text(stmt, i);
					int n = sx_column_bytes(stmt, i);
					if (quack_emit_row_text(out_buf, i,
					    s ? s : "", (size_t)n) < 0) goto oom;
					break;
				}
				case SX_BLOB: {
					const void *p = sx_column_blob(stmt, i);
					int n = sx_column_bytes(stmt, i);
					if (quack_emit_row_blob(out_buf, i, p,
					    (size_t)n) < 0) goto oom;
					break;
				}
				case SX_NULL:
				default:
					if (quack_emit_row_null(out_buf, i) < 0) goto oom;
					break;
				}
			}
			if (quack_emit_row_end(out_buf) < 0) goto oom;
		}
		rows++;
		if (limit > 0 && rows >= limit) break;
	}
	*rows_out = rows;
	return 0;
oom:
	*err = strdup("oom");
	return -1;
}

/* True if `s` holds another statement (non-whitespace, non-';'). */
static int
has_more_sql(const char *s)
{
	if (s == NULL) return 0;
	while (*s) {
		if (*s != ' ' && *s != '\t' && *s != '\r' &&
		    *s != '\n' && *s != ';')
			return 1;
		s++;
	}
	return 0;
}

int
db_exec_params(sx_db *h, const char *sql,
        const struct quack_param *params, int n_params, int64_t limit,
        quack_buf_t *out_buf, int64_t *n_rows, char **err)
{
	const char *cur = sql;
	int64_t rows = 0;
	int     last_ncols = 0;
	int     ran_any = 0;

	for (;;) {
		sx_stmt    *stmt = NULL;
		const char *tail = NULL;
		int         is_last, rc;

		rc = sx_prepare(h, cur, -1, &stmt, &tail);
		if (rc != SX_OK) {
			const char *msg = sx_errmsg(h);
			*err = strdup(msg ? msg : "prepare failed");
			if (stmt) sx_finalize(stmt);
			return -1;
		}
		if (stmt == NULL) {
			/* Blank / comment-only fragment; advance or finish. */
			if (!has_more_sql(tail)) break;
			cur = tail;
			continue;
		}
		is_last = !has_more_sql(tail);

		if (n_params > 0 && !is_last) {
			*err = strdup("parameters require a single statement");
			sx_finalize(stmt);
			return -1;
		}
		if (n_params > 0 && bind_params(stmt, params, n_params, err) != 0) {
			sx_finalize(stmt);
			return -1;
		}

		/* Stream rows only for the final statement; earlier ones run
		 * for side effects (multi-statement batch). */
		if (exec_stmt(h, stmt, limit, is_last, out_buf,
		    &last_ncols, &rows, err) != 0) {
			sx_finalize(stmt);
			return -1;
		}
		sx_finalize(stmt);
		ran_any = 1;
		if (is_last) break;
		cur = tail;
	}

	if (!ran_any) {
		*err = strdup("empty query");
		return -1;
	}

	/* DML/DDL final statement: report changes() instead of a row count. */
	if (last_ncols == 0)
		rows = sx_changes(h);

	if (quack_emit_done(out_buf, rows) < 0) {
		*err = strdup("oom");
		return -1;
	}
	*n_rows = rows;
	return 0;
}

int
db_exec_cached(sx_db *h, sx_stmt **pstmt, const char *sql,
        const struct quack_param *params, int n_params, int64_t limit,
        quack_buf_t *out_buf, int64_t *n_rows, char **err)
{
	int     ncols = 0;
	int64_t rows = 0;
	int     rc;

	if (*pstmt == NULL) {
		const char *tail = NULL;
		rc = sx_prepare(h, sql, -1, pstmt, &tail);
		if (rc != SX_OK) {
			const char *msg = sx_errmsg(h);
			*err = strdup(msg ? msg : "prepare failed");
			if (*pstmt) { sx_finalize(*pstmt); *pstmt = NULL; }
			return -1;
		}
		/* Empty or multi-statement: not cacheable -- finalize and
		 * fall back to the general (looping) path. */
		if (*pstmt == NULL || has_more_sql(tail)) {
			if (*pstmt) { sx_finalize(*pstmt); *pstmt = NULL; }
			return db_exec_params(h, sql, params, n_params, limit,
			    out_buf, n_rows, err);
		}
	} else {
		(void)sx_reset(*pstmt);
		(void)sx_clear_bindings(*pstmt);
	}

	if (n_params > 0 && bind_params(*pstmt, params, n_params, err) != 0) {
		sx_finalize(*pstmt); *pstmt = NULL;
		return -1;
	}
	if (exec_stmt(h, *pstmt, limit, 1, out_buf, &ncols, &rows, err) != 0) {
		sx_finalize(*pstmt); *pstmt = NULL;
		return -1;
	}
	if (ncols == 0)
		rows = sx_changes(h);
	if (quack_emit_done(out_buf, rows) < 0) {
		*err = strdup("oom");
		sx_finalize(*pstmt); *pstmt = NULL;
		return -1;
	}
	*n_rows = rows;
	(void)sx_reset(*pstmt);   /* leave clean + ready for reuse */
	return 0;
}
