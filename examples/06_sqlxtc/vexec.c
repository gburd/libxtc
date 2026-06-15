/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/vexec.c
 *	Vectorized execution engine -- V0.  See vexec.h and
 *	docs/M_SQLXTC_VEXEC.md.
 *
 *	V0 recognizes the P1 query shape (single base table, projection of
 *	plain column references or *, optional WHERE of one
 *	column-OP-literal comparison) and runs it as a chunked, push-based
 *	pipeline.  The row source is the reference engine's own cursor (a
 *	SELECT of the needed base columns, no WHERE), so MVCC visibility
 *	and value decoding are identical to the VDBE; vexec applies the
 *	WHERE filter and projection itself in the vectorized loop.  Any
 *	query that does not match falls back to the VDBE (vx_try_prepare
 *	returns 0), so a fallback is never wrong.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "vexec.h"
#include "sql_parse.h"
#include "sql_ast.h"
#include "sql_parse_gen.h"   /* TK_EQ .. TK_GE token ids */

/* ---- chunk (row-major in V0) ------------------------------------- */

#define VX_ARENA_BLK (64 * 1024)

struct vx_arena_blk {
	struct vx_arena_blk *next;
	size_t used, cap;
	unsigned char data[];
};

struct vx_chunk {
	int        ncol;
	int        nrow;                 /* live rows */
	int        cap;                  /* allocated row capacity */
	vx_cell_t *cells;                /* cap*ncol, row-major */
	struct vx_arena_blk *arena;      /* TEXT/BLOB byte storage for this chunk */
};

/* ---- recognized plan --------------------------------------------- */

enum vx_cmp { VX_CMP_NONE = 0, VX_CMP_EQ, VX_CMP_NE,
              VX_CMP_LT, VX_CMP_LE, VX_CMP_GT, VX_CMP_GE };

struct vx_stmt {
	sqlite3      *db;
	sqlite3_stmt *src;               /* SELECT <base cols> FROM <table> */
	int           nsrc_col;          /* columns the source SELECT returns */

	int           nout;
	int          *proj;              /* nout entries: source-column index */

	enum vx_cmp   cmp;
	int           filt_col;          /* source-column index compared (or -1) */
	vx_cell_t     filt_val;          /* literal compared against */
	struct vx_arena_blk *lit_arena;  /* holds filt_val TEXT bytes */

	vx_chunk_t   *chunk;
	int           cur;               /* current row in chunk; -1 before first */
	int           done;
};

/* ---- arena ------------------------------------------------------- */

static void *
arena_alloc(struct vx_arena_blk **head, size_t n)
{
	struct vx_arena_blk *b = *head;
	void *p;
	n = (n + 7u) & ~(size_t)7u;
	if (b == NULL || b->used + n > b->cap) {
		size_t cap = VX_ARENA_BLK;
		if (n > cap) cap = n;
		b = (struct vx_arena_blk *)malloc(sizeof *b + cap);
		if (b == NULL) return NULL;
		b->next = *head; b->used = 0; b->cap = cap;
		*head = b;
	}
	p = b->data + b->used;
	b->used += n;
	return p;
}

static void
arena_free(struct vx_arena_blk *b)
{
	while (b) { struct vx_arena_blk *n = b->next; free(b); b = n; }
}

/* ---- recognizer helpers ------------------------------------------ */

static enum vx_cmp
cmp_from_tk(int tok)
{
	switch (tok) {
	case TK_EQ: return VX_CMP_EQ;
	case TK_NE: return VX_CMP_NE;
	case TK_LT: return VX_CMP_LT;
	case TK_LE: return VX_CMP_LE;
	case TK_GT: return VX_CMP_GT;
	case TK_GE: return VX_CMP_GE;
	default:    return VX_CMP_NONE;
	}
}

/* A plain (unqualified or table-qualified) single column reference ->
 * its trailing name slice, or NULL if the expr is not a plain column. */
static const sql_str_t *
plain_column(const sql_expr_t *e)
{
	if (e == NULL || e->op != SX_E_COLUMN) return NULL;
	if (e->nname < 1 || e->nname > 2) return NULL;
	return &e->name[e->nname - 1];
}

/* SQLite column affinity, derived from the declared type string by
 * SQLite's substring rules (sqlite3AffinityType).  We only need three
 * buckets for V0's no-coercion gate. */
enum vx_aff { VX_AFF_BLOB = 0, VX_AFF_TEXT, VX_AFF_NUMERIC };

static int
str_contains_ci(const char *hay, const char *needle)
{
	size_t nl = strlen(needle);
	const char *p;
	if (hay == NULL) return 0;
	for (p = hay; *p; p++) {
		size_t k;
		for (k = 0; k < nl; k++) {
			char a = p[k], b = needle[k];
			if (a >= 'a' && a <= 'z') a -= 32;
			if (b >= 'a' && b <= 'z') b -= 32;
			if (a != b) break;
		}
		if (k == nl) return 1;
		if (p[0] == '\0') break;
	}
	return 0;
}

/* Mirror SQLite's affinity rules (sqlite3AffinityType), collapsed to the
 * three buckets V0 distinguishes.  Order matters: TEXT, then BLOB(none),
 * then REAL/numeric. */
static enum vx_aff
vx_affinity(const char *decltype)
{
	if (decltype == NULL || decltype[0] == '\0')
		return VX_AFF_BLOB;                  /* no type -> BLOB (no) affinity */
	if (str_contains_ci(decltype, "CHAR") ||
	    str_contains_ci(decltype, "CLOB") ||
	    str_contains_ci(decltype, "TEXT"))
		return VX_AFF_TEXT;
	if (str_contains_ci(decltype, "BLOB"))
		return VX_AFF_BLOB;
	/* INT, REAL, FLOA, DOUB, and everything else -> numeric. */
	return VX_AFF_NUMERIC;
}

/* Decode a literal expr into out (INT/REAL/TEXT in V0); TEXT bytes are
 * copied into *arena.  Returns 0 on success, -1 if unsupported. */
static int
literal_cell(struct vx_arena_blk **arena, const sql_expr_t *e, vx_cell_t *out)
{
	char buf[64];
	memset(out, 0, sizeof *out);
	switch (e->op) {
	case SX_E_NUMBER: {
		uint32_t i; int isreal = 0;
		for (i = 0; i < e->lit.len; i++) {
			char c = e->lit.p[i];
			if (c == '.' || c == 'e' || c == 'E') { isreal = 1; break; }
		}
		if (e->lit.len == 0 || e->lit.len >= sizeof buf) return -1;
		memcpy(buf, e->lit.p, e->lit.len); buf[e->lit.len] = '\0';
		if (isreal) { out->type = VX_REAL; out->r = strtod(buf, NULL); }
		else        { out->type = VX_INT;  out->i = strtoll(buf, NULL, 10); }
		return 0;
	}
	case SX_E_STRING: {
		uint8_t *p = (uint8_t *)arena_alloc(arena, e->lit.len + 1);
		if (p == NULL) return -1;
		if (e->lit.len) memcpy(p, e->lit.p, e->lit.len);
		p[e->lit.len] = '\0';
		out->type = VX_TEXT; out->bytes = p; out->nbytes = e->lit.len;
		return 0;
	}
	default:
		return -1;
	}
}

/* Build the "SELECT <cols> FROM <table>" the source cursor runs.  The
 * column set is the projected columns plus the filter column, in a
 * stable order; proj[]/filt_col index into it.  Returns a malloc'd SQL
 * string (caller frees) or NULL on failure; fills *nsrc via out params
 * through the stmt the caller assembles. */

/* For V0 we keep it simple and correct: the source SELECT lists EXACTLY
 * the table's projected columns AND the filter column (deduplicated by
 * textual name).  Names are emitted verbatim from the AST slices. */

struct namevec {
	char  names[16][64];   /* up to 16 distinct base columns in V0 */
	int   n;
};

static int
nv_add(struct namevec *nv, const sql_str_t *s)
{
	int i;
	if (s->len == 0 || s->len >= 64) return -1;
	for (i = 0; i < nv->n; i++)
		if ((int)strlen(nv->names[i]) == (int)s->len &&
		    memcmp(nv->names[i], s->p, s->len) == 0)
			return i;   /* already present */
	if (nv->n >= 16) return -1;
	memcpy(nv->names[nv->n], s->p, s->len);
	nv->names[nv->n][s->len] = '\0';
	return nv->n++;
}

/* ---- recognizer + plan build ------------------------------------- */

int
vx_try_prepare(sqlite3 *db, const char *sql, vx_stmt_t **out, char **errmsg)
{
	sql_arena_t *ast = NULL;
	sql_stmt_t  *root = NULL;
	const char  *perr = NULL;
	const sql_select_t *sel;
	const sql_src_t *src;
	struct namevec nv;
	struct vx_stmt *st = NULL;
	int proj_star = 0;
	const sql_exprlist_item_t *it;
	char tabbuf[64];
	char *srcsql = NULL;
	int i, rc = 0;

	if (out) *out = NULL;
	if (errmsg) *errmsg = NULL;

	/* Parse with the Lime parser; if it cannot parse, fall back (the
	 * VDBE may still accept it -- e.g. a shape the Lime grammar lacks). */
	if (sql_parse_ast(sql, strlen(sql), &ast, &root, &perr) != 0)
		goto fallback;
	if (root == NULL || root->next != NULL)         /* exactly one statement */
		goto fallback;
	if (root->kind != SQL_KIND_SELECT || root->explain)
		goto fallback;
	sel = root->u.select;
	if (sel == NULL) goto fallback;

	/* P1 only: no compound/CTE/DISTINCT/GROUP/HAVING/ORDER/LIMIT/OFFSET. */
	if (sel->with || sel->setop != SX_SET_NONE || sel->distinct ||
	    sel->group || sel->having || sel->order || sel->limit || sel->offset)
		goto fallback;

	/* FROM exactly one base table (no join, no subquery). */
	src = sel->from;
	if (src == NULL || src->next != NULL || src->subquery != NULL)
		goto fallback;
	if (src->table.len == 0 || src->table.len >= sizeof tabbuf)
		goto fallback;
	memcpy(tabbuf, src->table.p, src->table.len);
	tabbuf[src->table.len] = '\0';

	memset(&nv, 0, sizeof nv);

	/* Projection: each select item must be a plain column or a single
	 * bare '*'.  V0 does not handle expressions, functions, or aliases
	 * that rename (alias is harmless -- output name only). */
	for (it = sel->cols ? sel->cols->head : NULL; it; it = it->next) {
		const sql_expr_t *e = it->expr;
		const sql_str_t *cn;
		if (e == NULL) goto fallback;
		if (e->op == SX_E_STAR) {
			if (sel->cols->n != 1) goto fallback;   /* only "SELECT *" */
			proj_star = 1;
			break;
		}
		cn = plain_column(e);
		if (cn == NULL) goto fallback;              /* not a plain column */
		if (nv_add(&nv, cn) < 0) goto fallback;
	}

	/* WHERE: absent, or exactly one (column OP literal). */
	{
		enum vx_cmp cmp = VX_CMP_NONE;
		int filt_idx = -1;
		vx_cell_t fv; memset(&fv, 0, sizeof fv);
		struct vx_arena_blk *lit_arena = NULL;

		if (sel->where != NULL) {
			const sql_expr_t *w = sel->where;
			const sql_str_t *cn;
			cmp = (w->op == SX_E_BINARY) ? cmp_from_tk(w->op2) : VX_CMP_NONE;
			if (cmp == VX_CMP_NONE) goto fallback;
			cn = plain_column(w->a);
			if (cn == NULL) goto fallback;          /* lhs must be a column */
			if (literal_cell(&lit_arena, w->b, &fv) != 0) { /* rhs literal */
				arena_free(lit_arena);
				goto fallback;
			}
			if (proj_star) {
				/* "SELECT * ..." -- the filter column index is resolved
				 * after we know the source column order; for * we let the
				 * source be "SELECT *" and find the filter column by name
				 * at first step.  V0 keeps it simple: add it to nv so the
				 * explicit-column source includes it. */
			}
			filt_idx = nv_add(&nv, cn);
			if (filt_idx < 0) { arena_free(lit_arena); goto fallback; }
		}

		/* Build the source SELECT. */
		if (proj_star) {
			size_t n = strlen("SELECT * FROM ") + strlen(tabbuf) + 1;
			srcsql = (char *)malloc(n);
			if (!srcsql) { arena_free(lit_arena); goto oom; }
			snprintf(srcsql, n, "SELECT * FROM %s", tabbuf);
		} else {
			char cols[1100]; int off = 0, k;
			if (nv.n == 0) { arena_free(lit_arena); goto fallback; }
			for (k = 0; k < nv.n; k++)
				off += snprintf(cols + off, sizeof cols - (size_t)off,
				                "%s%s", k ? "," : "", nv.names[k]);
			{
				size_t n = strlen("SELECT  FROM ") + (size_t)off +
				           strlen(tabbuf) + 1;
				srcsql = (char *)malloc(n);
				if (!srcsql) { arena_free(lit_arena); goto oom; }
				snprintf(srcsql, n, "SELECT %s FROM %s", cols, tabbuf);
			}
		}

		/* Allocate the vexec statement and its source cursor. */
		st = (struct vx_stmt *)calloc(1, sizeof *st);
		if (!st) { arena_free(lit_arena); goto oom; }
		st->db = db;
		st->cmp = cmp;
		st->filt_val = fv;
		st->lit_arena = lit_arena;
		st->cur = -1;

		if (sqlite3_prepare_v2(db, srcsql, -1, &st->src, 0) != SQLITE_OK) {
			/* The source query failed to prepare (e.g. unknown column) --
			 * fall back so the VDBE produces the authoritative error. */
			vx_finalize(st); st = NULL;
			free(srcsql); srcsql = NULL;
			goto fallback;
		}
		st->nsrc_col = sqlite3_column_count(st->src);

		/* Projection mapping. */
		if (proj_star) {
			st->nout = st->nsrc_col;
			st->proj = (int *)malloc(sizeof(int) * (size_t)st->nout);
			if (!st->proj) { goto oom; }
			for (i = 0; i < st->nout; i++) st->proj[i] = i;
			/* Filter column index: by name within the source columns. */
			if (cmp != VX_CMP_NONE) {
				const sql_str_t *cn = plain_column(sel->where->a);
				st->filt_col = -1;
				for (i = 0; i < st->nsrc_col; i++) {
					const char *nm = sqlite3_column_name(st->src, i);
					if (nm && (int)strlen(nm) == (int)cn->len &&
					    memcmp(nm, cn->p, cn->len) == 0) { st->filt_col = i; break; }
				}
				if (st->filt_col < 0) { goto fallback; }
			} else {
				st->filt_col = -1;
			}
		} else {
			/* proj[] maps each output column to its position in nv (which
			 * is the source column order). */
			int nproj = 0;
			for (it = sel->cols->head; it; it = it->next) nproj++;
			st->nout = nproj;
			st->proj = (int *)malloc(sizeof(int) * (size_t)(nproj > 0 ? nproj : 1));
			if (!st->proj) { goto oom; }
			{
				int oi = 0;
				for (it = sel->cols->head; it; it = it->next) {
					const sql_str_t *cn = plain_column(it->expr);
					int sidx = nv_add(&nv, cn);   /* already present -> its index */
					st->proj[oi++] = sidx;
				}
			}
			st->filt_col = (cmp != VX_CMP_NONE) ? filt_idx : -1;
		}

		/* Affinity no-coercion gate.  SQLite applies the filter column's
		 * affinity to the literal before comparing, which can change the
		 * result (e.g. an INTEGER column vs a '5' text literal matches 5).
		 * V0 does not coerce, so it recognizes a WHERE comparison ONLY
		 * when the column affinity and the literal type need no coercion;
		 * any other combination falls back to the VDBE, which applies
		 * affinity correctly. */
		if (st->cmp != VX_CMP_NONE) {
			enum vx_aff aff = vx_affinity(
			    sqlite3_column_decltype(st->src, st->filt_col));
			int litnum = (st->filt_val.type == VX_INT ||
			              st->filt_val.type == VX_REAL);
			int littext = (st->filt_val.type == VX_TEXT);
			int ok = 0;
			if (aff == VX_AFF_NUMERIC && litnum) ok = 1;
			else if (aff == VX_AFF_TEXT && littext) ok = 1;
			/* BLOB/no affinity: SQLite does no coercion, so a same-class
			 * comparison is safe; but the stored value's class is only
			 * known per-row.  Be conservative: fall back for BLOB-affinity
			 * columns with any literal. */
			if (!ok) { goto fallback; }   /* label frees st + srcsql */
		}
	}

	free(srcsql);
	sql_arena_destroy(ast);
	*out = st;
	return 1;

oom:
	rc = -1;
	if (errmsg) *errmsg = NULL;
	/* fall through to cleanup */
fallback:
	if (srcsql) free(srcsql);
	if (ast) sql_arena_destroy(ast);
	if (st) { vx_finalize(st); }
	(void)perr;
	return rc;   /* 0 = fallback, -1 = error */
}

/* ---- execution: pull a chunk from the source, filtered + projected -- */

static int
cmp_cells(const vx_cell_t *a, const vx_cell_t *b)
{
	/* Returns <0, 0, >0 like memcmp; type-aware for the V0 classes.
	 * SQLite type-order (NULL<INT/REAL<TEXT<BLOB) is approximated: V0
	 * only compares like-with-like for the supported literal types, and
	 * a type mismatch sorts by class so the predicate just excludes the
	 * row (matching SQLite's "different storage class -> not equal"). */
	if (a->type != b->type) {
		/* Numeric cross-compare: INT vs REAL. */
		if ((a->type == VX_INT || a->type == VX_REAL) &&
		    (b->type == VX_INT || b->type == VX_REAL)) {
			double x = (a->type == VX_INT) ? (double)a->i : a->r;
			double y = (b->type == VX_INT) ? (double)b->i : b->r;
			return (x < y) ? -1 : (x > y) ? 1 : 0;
		}
		return (int)a->type - (int)b->type;
	}
	switch (a->type) {
	case VX_INT:  return (a->i < b->i) ? -1 : (a->i > b->i) ? 1 : 0;
	case VX_REAL: return (a->r < b->r) ? -1 : (a->r > b->r) ? 1 : 0;
	case VX_TEXT:
	case VX_BLOB: {
		uint32_t n = a->nbytes < b->nbytes ? a->nbytes : b->nbytes;
		int c = n ? memcmp(a->bytes, b->bytes, n) : 0;
		if (c) return c;
		return (int)a->nbytes - (int)b->nbytes;
	}
	default: return 0;   /* NULL == NULL for this internal helper */
	}
}

static int
filter_pass(const struct vx_stmt *st, const vx_cell_t *row)
{
	const vx_cell_t *lhs;
	int c;
	if (st->cmp == VX_CMP_NONE) return 1;
	lhs = &row[st->filt_col];
	/* SQLite: any comparison with NULL is NULL (not true) -> row excluded. */
	if (lhs->type == VX_NULL || st->filt_val.type == VX_NULL) return 0;
	c = cmp_cells(lhs, &st->filt_val);
	switch (st->cmp) {
	case VX_CMP_EQ: return c == 0;
	case VX_CMP_NE: return c != 0;
	case VX_CMP_LT: return c < 0;
	case VX_CMP_LE: return c <= 0;
	case VX_CMP_GT: return c > 0;
	case VX_CMP_GE: return c >= 0;
	default:        return 1;
	}
}

/* Read source column `i` of the current src row into cell. */
static void
read_src_cell(sqlite3_stmt *src, int i, vx_cell_t *cell, struct vx_arena_blk **arena)
{
	memset(cell, 0, sizeof *cell);
	switch (sqlite3_column_type(src, i)) {
	case SQLITE_NULL:    cell->type = VX_NULL; break;
	case SQLITE_INTEGER: cell->type = VX_INT;  cell->i = sqlite3_column_int64(src, i); break;
	case SQLITE_FLOAT:   cell->type = VX_REAL; cell->r = sqlite3_column_double(src, i); break;
	case SQLITE_TEXT: {
		const unsigned char *t = sqlite3_column_text(src, i);
		int n = sqlite3_column_bytes(src, i);
		uint8_t *p = (uint8_t *)arena_alloc(arena, (size_t)n + 1);
		if (p) { if (n) memcpy(p, t, (size_t)n); p[n] = '\0'; }
		cell->type = VX_TEXT; cell->bytes = p; cell->nbytes = (uint32_t)n;
		break;
	}
	case SQLITE_BLOB: {
		const void *bb = sqlite3_column_blob(src, i);
		int n = sqlite3_column_bytes(src, i);
		uint8_t *p = (uint8_t *)arena_alloc(arena, (size_t)(n > 0 ? n : 1));
		if (p && n) memcpy(p, bb, (size_t)n);
		cell->type = VX_BLOB; cell->bytes = p; cell->nbytes = (uint32_t)n;
		break;
	}
	}
}

static void
chunk_free(vx_chunk_t *c)
{
	if (!c) return;
	arena_free(c->arena);
	free(c->cells);
	free(c);
}

/* Build one chunk by pulling source rows until VEXEC_VECTOR_SIZE rows
 * pass the filter or the source is exhausted.  Returns the chunk (nrow
 * may be 0 at true end) or NULL on error; *done set when source ended. */
static vx_chunk_t *
next_chunk(struct vx_stmt *st, int *done)
{
	vx_chunk_t *c;
	vx_cell_t srcrow[64];
	int rc;

	*done = 0;
	c = (vx_chunk_t *)calloc(1, sizeof *c);
	if (!c) return NULL;
	c->ncol = st->nout;
	c->cap = VEXEC_VECTOR_SIZE;
	c->cells = (vx_cell_t *)malloc(sizeof(vx_cell_t) * (size_t)c->cap * (size_t)(c->ncol > 0 ? c->ncol : 1));
	if (!c->cells) { free(c); return NULL; }

	if (st->nsrc_col > 64) { chunk_free(c); return NULL; }

	while (c->nrow < c->cap) {
		int j;
		rc = sqlite3_step(st->src);
		if (rc == SQLITE_DONE) { *done = 1; break; }
		if (rc != SQLITE_ROW) { chunk_free(c); return NULL; }

		/* Materialize the source row into srcrow[] (chunk arena owns bytes). */
		for (j = 0; j < st->nsrc_col; j++)
			read_src_cell(st->src, j, &srcrow[j], &c->arena);

		if (!filter_pass(st, srcrow))
			continue;

		/* Project into the chunk row. */
		{
			vx_cell_t *dst = &c->cells[(size_t)c->nrow * (size_t)c->ncol];
			for (j = 0; j < st->nout; j++)
				dst[j] = srcrow[st->proj[j]];
		}
		c->nrow++;
	}
	return c;
}

int
vx_step(vx_stmt_t *st)
{
	if (st == NULL) return SQLITE_MISUSE;
	if (st->done && (st->chunk == NULL || st->cur + 1 >= st->chunk->nrow))
		return SQLITE_DONE;

	/* Advance within the current chunk. */
	if (st->chunk != NULL && st->cur + 1 < st->chunk->nrow) {
		st->cur++;
		return SQLITE_ROW;
	}

	/* Need a new chunk. */
	for (;;) {
		int done = 0;
		vx_chunk_t *c;
		if (st->done) return SQLITE_DONE;
		c = next_chunk(st, &done);
		if (c == NULL) return SQLITE_ERROR;
		if (st->chunk) chunk_free(st->chunk);
		st->chunk = c;
		st->cur = -1;
		st->done = done;
		if (c->nrow > 0) { st->cur = 0; return SQLITE_ROW; }
		if (done) return SQLITE_DONE;   /* exhausted, no surviving rows */
		/* else: a full chunk was filtered to empty; loop for the next. */
	}
}

/* ---- column accessors (current row of the current chunk) --------- */

static const vx_cell_t *
cur_cell(vx_stmt_t *st, int i)
{
	if (st == NULL || st->chunk == NULL || st->cur < 0 ||
	    st->cur >= st->chunk->nrow || i < 0 || i >= st->nout)
		return NULL;
	return &st->chunk->cells[(size_t)st->cur * (size_t)st->nout + (size_t)i];
}

int       vx_column_count(vx_stmt_t *st) { return st ? st->nout : 0; }

vx_type_t vx_column_type(vx_stmt_t *st, int i)
{ const vx_cell_t *c = cur_cell(st, i); return c ? c->type : VX_NULL; }

int64_t   vx_column_int64(vx_stmt_t *st, int i)
{ const vx_cell_t *c = cur_cell(st, i);
  if (!c) return 0;
  if (c->type == VX_INT) return c->i;
  if (c->type == VX_REAL) return (int64_t)c->r;
  return 0; }

double    vx_column_double(vx_stmt_t *st, int i)
{ const vx_cell_t *c = cur_cell(st, i);
  if (!c) return 0;
  if (c->type == VX_REAL) return c->r;
  if (c->type == VX_INT) return (double)c->i;
  return 0; }

const char *vx_column_text(vx_stmt_t *st, int i)
{ const vx_cell_t *c = cur_cell(st, i);
  return (c && (c->type == VX_TEXT || c->type == VX_BLOB)) ? (const char *)c->bytes : NULL; }

const void *vx_column_blob(vx_stmt_t *st, int i)
{ const vx_cell_t *c = cur_cell(st, i);
  return (c && (c->type == VX_TEXT || c->type == VX_BLOB)) ? (const void *)c->bytes : NULL; }

int vx_column_bytes(vx_stmt_t *st, int i)
{ const vx_cell_t *c = cur_cell(st, i);
  return (c && (c->type == VX_TEXT || c->type == VX_BLOB)) ? (int)c->nbytes : 0; }

void
vx_finalize(vx_stmt_t *st)
{
	if (st == NULL) return;
	if (st->src) sqlite3_finalize(st->src);
	if (st->chunk) chunk_free(st->chunk);
	if (st->lit_arena) arena_free(st->lit_arena);
	free(st->proj);
	free(st);
}
