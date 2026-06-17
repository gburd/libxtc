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

/* Worker count for the vexec fast path on the live query path.  Serial
 * (1) by default: vexec's morsel-parallel scan is correct but not yet
 * faster than serial for typical scans (per-worker scan-path overhead,
 * see bench/sqlxtc/VEXEC_RESULTS.md), so the live path stays serial
 * until that is profiled and fixed.  SQLXTC_VEXEC_WORKERS overrides the
 * worker count; SQLXTC_VEXEC=0 disables the fast path entirely (so the
 * VDBE serves every query -- used by the differential test and as an
 * escape hatch).  Returns 0 when disabled, else the worker count. */
static int
db_vexec_workers(void)
{
	const char *off = getenv("SQLXTC_VEXEC");
	const char *e;
	int w;
	if (off != NULL && off[0] == '0' && off[1] == '\0') return 0;
	e = getenv("SQLXTC_VEXEC_WORKERS");
	w = e ? atoi(e) : 1;
	return w > 0 ? w : 1;
}

/* Skip leading whitespace and a leading SQL comment, then case-
 * insensitively test whether `sql` begins with `kw` as a whole keyword.
 * Cheap recognizer for COMMIT / ROLLBACK / END so the live path can
 * flush native buffered writes around transaction control. */
static int
sql_starts_kw(const char *sql, const char *kw)
{
	const char *p = sql;
	size_t n = strlen(kw), i;
	while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
	for (i = 0; i < n; i++) {
		char c = p[i];
		if (c >= 'a' && c <= 'z') c -= 32;
		if (c != kw[i]) return 0;
	}
	{
		char t = p[n];
		if ((t >= 'a' && t <= 'z') || (t >= 'A' && t <= 'Z') ||
		    (t >= '0' && t <= '9') || t == '_')
			return 0;   /* longer identifier, not the bare keyword */
	}
	return 1;
}

/* Before running a COMMIT / ROLLBACK / END statement, flush or discard
 * any native (VDBE-free) writes buffered in this connection's write set
 * so they commit / abort atomically with the transaction.  The SQLite
 * statement then runs to flip autocommit back on (its vtab hook no-ops
 * on the now-closed buffer). */
static void
db_native_txn_end(sx_db *h, const char *sql)
{
	if (sql_starts_kw(sql, "COMMIT") || sql_starts_kw(sql, "END")) {
		sx_vexec_commit(h);
	} else if (sql_starts_kw(sql, "ROLLBACK")) {
		/* ROLLBACK TO <savepoint> is a partial undo, not a transaction
		 * abort -- leave it to SQLite + the vtab savepoint hooks. */
		const char *p = sql;
		while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
		p += 8;   /* past "ROLLBACK" */
		while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
		if (sql_starts_kw(p, "TO")) return;       /* savepoint partial undo */
		sx_vexec_rollback(h);
	}
}

/* Decode a hex or base64 blob param into a freshly malloc'd buffer.
 * Returns 0 and sets out / outn (caller frees *out), or -1 on a
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

/*
 * Transparent CREATE TABLE -> xstore.  When the libxtc-native storage
 * engine is open, a plain CREATE TABLE is rewritten into a CREATE
 * VIRTUAL TABLE on the xstore module, so ordinary DDL lands in the
 * xtc-native engine (cooling buffer pool, MVCC, larger-than-RAM)
 * instead of SQLite's built-in B-tree -- no opt-in.
 *
 * Conservative by design: only a single, well-formed
 *   CREATE TABLE name (col [type/constraints], ..., [table-constraint])
 * is rewritten.  Anything it cannot confidently parse -- a batch, table
 * options (WITHOUT ROWID), trailing statements -- is passed through
 * unchanged to SQLite's native B-tree, the documented escape hatch.
 * The first column becomes the table's INTEGER PRIMARY KEY (xstore's
 * rowid); the rest are the typed payload.  Returns 1 if it rewrote into
 * `out`, else 0.
 */
static int
ci_kw(const char **pp, const char *kw)
{
	const char *p = *pp;
	size_t i;
	for (i = 0; kw[i]; i++) {
		unsigned char c = (unsigned char)p[i];
		if (c >= 'A' && c <= 'Z') c = (unsigned char)(c - 'A' + 'a');
		if (c != (unsigned char)kw[i]) return 0;
	}
	{
		unsigned char c = (unsigned char)p[i];
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		    (c >= '0' && c <= '9') || c == '_')
			return 0;   /* keyword must end at a non-identifier char */
	}
	*pp = p + i;
	return 1;
}

static const char *
skip_ws(const char *p)
{
	while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++;
	return p;
}

static size_t
read_name(const char **pp, char *out, size_t cap)
{
	const char *p = skip_ws(*pp);
	size_t n = 0;
	char close = 0;
	if (*p == '"') close = '"';
	else if (*p == '`') close = '`';
	else if (*p == '[') close = ']';
	if (close) {
		p++;
		while (*p && *p != close && n + 1 < cap) out[n++] = *p++;
		if (*p == close) p++;
	} else {
		while (((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
		    (*p >= '0' && *p <= '9') || *p == '_') && n + 1 < cap)
			out[n++] = *p++;
	}
	out[n] = '\0';
	*pp = p;
	return n;
}

static int
db_rewrite_create_table(const char *sql, char *out, size_t cap)
{
	const char *p = skip_ws(sql);
	const char *defs, *q, *s;
	char name[128];
	int depth, off, ncol = 0;

	if (!ci_kw(&p, "create")) return 0;
	p = skip_ws(p);
	if (!ci_kw(&p, "table")) return 0;
	p = skip_ws(p);
	if (ci_kw(&p, "if")) {                 /* optional IF NOT EXISTS */
		p = skip_ws(p);
		if (!ci_kw(&p, "not")) return 0;
		p = skip_ws(p);
		if (!ci_kw(&p, "exists")) return 0;
		p = skip_ws(p);
	}
	if (read_name(&p, name, sizeof name) == 0) return 0;
	p = skip_ws(p);
	if (*p != '(') return 0;
	p++;
	defs = p;
	depth = 1;
	for (q = p; *q; q++) {                 /* matching ')' (skip quotes) */
		char c = *q;
		if (c == '\'' || c == '"' || c == '`') {
			char qc = c;
			for (q++; *q && *q != qc; q++) ;
			if (!*q) return 0;
		} else if (c == '(') depth++;
		else if (c == ')') { if (--depth == 0) break; }
	}
	if (depth != 0) return 0;
	{
		const char *r = skip_ws(q + 1);
		if (*r == ';') r = skip_ws(r + 1);
		if (*r != '\0') return 0;       /* trailing options/statements */
	}
	off = snprintf(out, cap, "CREATE VIRTUAL TABLE %s USING xstore(", name);
	if (off < 0 || (size_t)off >= cap) return 0;
	s = defs;
	while (s < q) {                        /* split defs by top-level commas */
		const char *piece = s, *e, *pp, *kw;
		char col[128];
		int d2 = 0;
		for (e = s; e < q; e++) {
			char c = *e;
			if (c == '\'' || c == '"' || c == '`') {
				char qc = c;
				for (e++; e < q && *e != qc; e++) ;
			} else if (c == '(') d2++;
			else if (c == ')') d2--;
			else if (c == ',' && d2 == 0) break;
		}
		s = (e < q) ? e + 1 : q;
		pp = piece; kw = skip_ws(pp);
		if (ci_kw(&kw, "primary") || ci_kw(&kw, "unique") ||
		    ci_kw(&kw, "check") || ci_kw(&kw, "foreign") ||
		    ci_kw(&kw, "constraint"))
			continue;               /* table-level constraint, not a column */
		if (read_name(&pp, col, sizeof col) == 0)
			continue;
		off += snprintf(out + off, cap - (size_t)off, "%s%s",
		    ncol ? ", " : "", col);
		if ((size_t)off >= cap) return 0;
		ncol++;
	}
	if (ncol == 0) return 0;
	off += snprintf(out + off, cap - (size_t)off, ")");
	if ((size_t)off >= cap) return 0;
	return 1;
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
	char    rewrite[1024];

	/* Transparent routing: a plain CREATE TABLE becomes a CREATE VIRTUAL
	 * TABLE on xstore when the native engine is open (DDL takes no
	 * params, so this never collides with the extended/params path). */
	if (n_params == 0 && sx_storage_active() &&
	    db_rewrite_create_table(sql, rewrite, sizeof rewrite))
		cur = sql = rewrite;

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
		db_native_txn_end(h, cur);   /* flush native writes before COMMIT/ROLLBACK */
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

/* Emit a materialized vexec result to the Quack buffer, using column
 * names from the already-prepared statement (so headers are identical
 * to the VDBE path).  Returns 0 or -1 (*err set). */
static int
emit_vexec(sx_vx_result *vr, sx_stmt *names, int64_t limit,
           quack_buf_t *out_buf, int64_t *rows_out, char **err)
{
	int ncol = sx_vexec_ncol(vr);
	int nrow = sx_vexec_nrow(vr);
	int64_t emitted = 0;
	int i, j;

	if (ncol > 0) {
		if (quack_emit_cols_begin(out_buf) < 0) goto oom;
		for (j = 0; j < ncol; j++) {
			/* Prefer vexec's own column name (AS alias / bare column);
			 * fall back to the VDBE-prepared name only for an expression
			 * column, whose SQLite name is its verbatim source text. */
			const char *nm = sx_vexec_name(vr, j);
			if (nm == NULL) nm = sx_column_name(names, j);
			if (quack_emit_cols_name(out_buf, j, nm) < 0) goto oom;
		}
		if (quack_emit_cols_end(out_buf) < 0) goto oom;
	}
	for (i = 0; i < nrow; i++) {
		if (ncol > 0) {
			if (quack_emit_row_begin(out_buf) < 0) goto oom;
			for (j = 0; j < ncol; j++) {
				switch (sx_vexec_type(vr, i, j)) {
				case SX_INTEGER:
					if (quack_emit_row_int(out_buf, j,
					    sx_vexec_int64(vr, i, j)) < 0) goto oom;
					break;
				case SX_FLOAT:
					if (quack_emit_row_double(out_buf, j,
					    sx_vexec_double(vr, i, j)) < 0) goto oom;
					break;
				case SX_TEXT:
					if (quack_emit_row_text(out_buf, j,
					    sx_vexec_text(vr, i, j),
					    (size_t)sx_vexec_bytes(vr, i, j)) < 0) goto oom;
					break;
				case SX_BLOB:
					if (quack_emit_row_blob(out_buf, j,
					    sx_vexec_blob(vr, i, j),
					    (size_t)sx_vexec_bytes(vr, i, j)) < 0) goto oom;
					break;
				case SX_NULL:
				default:
					if (quack_emit_row_null(out_buf, j) < 0) goto oom;
					break;
				}
			}
			if (quack_emit_row_end(out_buf) < 0) goto oom;
		}
		emitted++;
		if (limit > 0 && emitted >= limit) break;
	}
	*rows_out = emitted;
	return 0;
oom:
	*err = strdup("oom");
	return -1;
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

	/* Vectorized-executor fast path: a recognized, param-free read-only
	 * query runs on the libxtc-native vectorized executor over the xstore
	 * B-tree; the prepared statement supplies the column names so the
	 * client sees identical headers.  Anything not recognized falls
	 * through to the VDBE (correct-by-fallback).  The prepared statement
	 * stays cached and reusable either way. */
	if (n_params == 0) {
		int vw = db_vexec_workers();
		sx_vx_result *vr = NULL;
		if (vw > 0 && sx_vexec_try(h, sql, vw, &vr) == 1) {
			int erc = emit_vexec(vr, *pstmt, limit, out_buf, &rows, err);
			sx_vexec_free(vr);
			if (erc != 0) { (void)sx_reset(*pstmt); return -1; }
			if (quack_emit_done(out_buf, rows) < 0) {
				*err = strdup("oom"); (void)sx_reset(*pstmt); return -1;
			}
			*n_rows = rows;
			(void)sx_reset(*pstmt);   /* leave clean + ready for reuse */
			return 0;
		}

		/* Native write fast path: a recognized literal-row INSERT applies
		 * straight to the xstore B-tree, no VDBE / no vtab round-trip.
		 * Emits the same {"done":N} a DML statement does.  Not recognized
		 * (0) -> fall through to the VDBE; the prepared statement is the
		 * fallback and stays cached. */
		if (vw > 0) {
			int64_t nch = 0;
			if (sx_vexec_write(h, sql, &nch) == 1) {
				if (quack_emit_done(out_buf, nch) < 0) {
					*err = strdup("oom"); (void)sx_reset(*pstmt); return -1;
				}
				*n_rows = nch;
				(void)sx_reset(*pstmt);
				return 0;
			}
		}
	}

	db_native_txn_end(h, sql);   /* flush native writes before COMMIT/ROLLBACK */
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
