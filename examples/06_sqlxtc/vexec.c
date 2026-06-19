/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/vexec.c
 *	Vectorized execution engine -- V1.  See vexec.h and
 *	docs/M_SQLXTC_VEXEC.md.
 *
 *	V1 recognizes the P2 query shape (single base table, projection of
 *	scalar expressions, optional WHERE of a scalar boolean expression)
 *	and runs it as a chunked, push-based pipeline.  Projection and
 *	filter are compiled from the Lime AST into a vexec expression tree
 *	(vx_expr) and evaluated per row; the row source is the reference
 *	engine's own cursor (a SELECT of the needed base columns, no
 *	WHERE), so MVCC visibility and value decoding are identical to the
 *	VDBE.  Any expression whose SQLite semantics V1 cannot faithfully
 *	reproduce -- in particular a comparison or arithmetic that would
 *	trigger type-affinity coercion -- makes the whole query fall back
 *	to the VDBE (vx_try_prepare returns 0), so a fallback is never
 *	wrong.
 *
 *	Supported expressions: column references; INTEGER/REAL/TEXT/NULL
 *	literals; arithmetic + - * / % over numeric operands; comparisons
 *	= <> < <= > >= with SQLite 3-valued NULL logic over operands that
 *	need no affinity coercion; AND OR NOT; IS NULL / IS NOT NULL;
 *	string concat ||; and the functions abs, length, lower, upper,
 *	coalesce, ifnull.  Everything else falls back.
 */

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <stdatomic.h>

#include "vexec.h"
#include "sql_parse.h"
#include "sql_ast.h"
#include "sql_parse_gen.h"   /* TK_* token ids */
#include "btree.h"           /* bt_t */
#include "xstore.h"          /* storage-native scan: the VDBE-free row source */

#include "xtc.h"             /* XTC_OK */
#include "xtc_exec.h"        /* V2: morsel-parallel workers, one per loop */

/* ---- arena ------------------------------------------------------- */

#define VX_ARENA_BLK (64 * 1024)

struct vx_arena_blk {
	struct vx_arena_blk *next;
	size_t used, cap;
	unsigned char data[];
};

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

/* ---- chunk (row-major) ------------------------------------------- */

struct vx_chunk {
	int        ncol;
	int        nrow;
	int        cap;
	vx_cell_t *cells;                /* cap*ncol, row-major */
	struct vx_arena_blk *arena;      /* TEXT/BLOB bytes for this chunk */
};

/* ---- compiled expression tree ------------------------------------ */

enum vx_op {
	VXO_COL = 1,    /* source-column reference (.col) */
	VXO_LIT,        /* constant (.lit) */
	VXO_NEG, VXO_NOT, VXO_BITNOT,        /* unary: - NOT ~ */
	VXO_ADD, VXO_SUB, VXO_MUL, VXO_DIV, VXO_MOD,  /* arithmetic */
	VXO_CONCAT,                          /* || */
	VXO_EQ, VXO_NE, VXO_LT, VXO_LE, VXO_GT, VXO_GE,  /* comparison */
	VXO_AND, VXO_OR,                     /* boolean */
	VXO_ISNULL, VXO_NOTNULL,             /* IS [NOT] NULL */
	VXO_IN, VXO_NOTIN,                   /* a IN/NOT IN (list): a in .a, list in args */
	VXO_CASE,                            /* CASE: base in .a (NULL=searched),
	                                      * args[]=when0,then0,when1,then1,...,
	                                      * nargs=2*arms, ELSE in .b (NULL=none) */
	VXO_FUNC                             /* builtin function (.func) */
};

enum vx_func {
	VXF_ABS = 1, VXF_LENGTH, VXF_LOWER, VXF_UPPER, VXF_COALESCE, VXF_IFNULL
};

typedef struct vx_expr {
	enum vx_op    op;
	int           col;        /* VXO_COL: source-column index */
	vx_cell_t     lit;        /* VXO_LIT */
	enum vx_func  func;       /* VXO_FUNC */
	struct vx_expr *a, *b;    /* operands (a for unary; a,b for binary) */
	struct vx_expr **args;    /* VXO_FUNC argument list */
	int           nargs;
} vx_expr_t;

/* ---- aggregation (V3) -------------------------------------------- */

/* Max HAVING-only aggregates (not in the SELECT list) per query. */
#define VX_HAVING_AGG_MAX 8

enum vx_agg_kind {
	VXA_COUNT_STAR = 1,   /* count(*) */
	VXA_COUNT,            /* count(expr): non-NULL inputs */
	VXA_SUM,              /* sum(expr): NULL if all inputs NULL */
	VXA_TOTAL,            /* total(expr): 0.0 if no rows; always REAL */
	VXA_AVG,              /* avg(expr): sum/count over non-NULL, REAL */
	VXA_MIN, VXA_MAX      /* min/max(expr): ignore NULLs */
};

/* A running accumulator for one aggregate within one group. */
typedef struct vx_acc {
	int64_t   cnt;        /* count of non-NULL inputs (all kinds) */
	int       seen;       /* 1 if any non-NULL input seen (min/max/sum) */
	int       is_real;    /* SUM/TOTAL/AVG: accumulator went real */
	int64_t   isum;       /* integer running sum */
	double    rsum;       /* real running sum */
	vx_cell_t ext;        /* MIN/MAX current extreme (when seen) */
} vx_acc_t;

/* A column of the select list under aggregation: either a GROUP BY key
 * expression (output verbatim) or an aggregate over an input expr. */
typedef struct vx_outcol {
	int            is_agg;
	enum vx_agg_kind kind;   /* if is_agg */
	vx_expr_t     *arg;      /* aggregate input expr (NULL for count(*)) */
	vx_expr_t     *key;      /* group-key expr (if !is_agg) */
	const sql_expr_t *ast;   /* HAVING-only agg: the AST node, so
	                          * compile_having can match it (NULL else) */
} vx_outcol_t;

typedef struct vx_aggplan {
	int          ngrp;       /* number of GROUP BY key expressions */
	vx_expr_t  **grp;        /* ngrp key expressions */
	int          nout;       /* output columns (emitted) */
	int          nout_all;   /* outcols including HAVING-only aggregates,
	                          * which are accumulated but not emitted;
	                          * out[nout .. nout_all) are those. */
	vx_outcol_t *out;        /* nout_all output column descriptors */
	int          nagg;       /* number of aggregate outcols (all of them) */
	vx_expr_t   *having;     /* HAVING predicate over the EXTENDED row
	                          * (nout_all cells: emitted + HAVING aggs),
	                          * cols resolve by index; NULL if no HAVING */
} vx_aggplan_t;

/* A hash-table entry: a group, its key cells, and its accumulators. */
typedef struct vx_grp {
	struct vx_grp *next;     /* bucket chain */
	uint64_t       hash;
	vx_cell_t     *keys;     /* ngrp key values (bytes in the ht arena) */
	vx_acc_t      *accs;     /* nagg accumulators */
} vx_grp_t;

typedef struct vx_htab {
	vx_grp_t   **buckets;
	int          nbucket;
	int          ngroup;
	int          ngrp_key;   /* keys per group */
	int          nagg;       /* accumulators per group */
	struct vx_arena_blk *arena;   /* group nodes + key bytes */
} vx_htab_t;

/* ---- join (V5) --------------------------------------------------- *
 *
 * An INNER equi-join of two base tables.  vexec builds a hash table on
 * the BUILD side keyed by its join column, then scans the PROBE side
 * and emits a combined row [build cols | probe cols] for each match,
 * over which the projection and WHERE filter are evaluated.  Column
 * references in proj/filter index the combined row. */
typedef struct vx_joinplan {
	char       build_sql[1100];   /* SELECT <build cols> FROM <build table> */
	char       probe_sql[1100];   /* SELECT <probe cols> FROM <probe table> */
	int        build_ncol;        /* columns the build source returns */
	int        probe_ncol;
	int        build_key;         /* build-side join col (index within build) */
	int        probe_key;         /* probe-side join col (index within probe) */
	int        join_kind;         /* SX_J_INNER / LEFT / RIGHT / FULL */
	/* In the combined row, build columns come first (offset 0), then
	 * probe columns (offset build_ncol).  proj/filter use those indices. */
} vx_joinplan_t;

typedef struct vx_jht vx_jht_t;   /* join build hash table (defined later) */
typedef struct vx_jrow vx_jrow_t;

/* ---- the vexec statement ----------------------------------------- */

struct vx_stmt {
	sqlite3      *db;
	sqlite3_stmt *src;          /* join path metadata stmt; NULL on the
	                             * storage-native single-table path */
	int           nsrc_col;

	/* Storage-native source (single-table scan/agg paths).  bt != NULL
	 * marks the storage path: rows come from xstore_scan over `table`,
	 * and src_pay[i] is source column i's origin -- -1 = rowid (the
	 * INTEGER PRIMARY KEY), else the payload index in the record. */
	bt_t          *bt;
	xstore_scan_t *scan;
	int           src_pay[32];
	uint64_t      snap;         /* 0 = latest committed */

	/* Rowid-range pushdown (the minimal planner): when the WHERE pins the
	 * primary key to a range, the scan is bounded to it instead of full,
	 * so a point/range read seeks rather than scanning the whole table.
	 * The WHERE filter still runs (it may carry other conjuncts); these
	 * bounds only skip rows that cannot match the pk constraint. */
	int64_t       scan_lo, scan_hi;
	int           scan_has_lo, scan_has_hi;

	/* Bound ? parameters for this statement (1-based), used by the
	 * compiler to turn SX_E_PARAM into a literal.  Borrowed (the caller's
	 * array outlives the prepare); NULL when there are no params. */
	const vx_cell_t *binds;
	int              nbinds;

	int           distinct;     /* SELECT DISTINCT: dedup the result */

	int           nout;
	vx_expr_t   **proj;         /* nout projection expressions (non-agg path) */
	vx_expr_t    *filter;       /* WHERE expression, or NULL */

	/* Output column names, derived from the AST select items at compile
	 * time (alias if present; else the bare column's unqualified name).
	 * outname[i][0] == '\0' means "unknown" -- an expression / function /
	 * star column whose SQLite name is its verbatim source text, which
	 * the AST does not record; the caller uses the VDBE-prepared name for
	 * those.  nout entries. */
	char          outname[64][64];

	vx_aggplan_t *agg;          /* non-NULL => aggregating statement (V3) */
	vx_joinplan_t *join;        /* non-NULL => two-table hash join (V5) */

	/* ORDER BY / LIMIT (V4).  norder > 0 => ordered.  Each order key is
	 * either an expression over the SOURCE columns (order_key[i]), or a
	 * 1-based reference to an output column (order_outcol[i] > 0, key
	 * NULL).  order_desc[i] = 1 for DESC. */
	int           norder;
	vx_expr_t   **order_key;    /* norder expressions, or NULL entry */
	int          *order_outcol; /* norder: 1-based output col, or 0 */
	int          *order_desc;   /* norder: 1 = DESC */
	int64_t       limit;        /* -1 = no LIMIT */
	int64_t       offset;       /* 0 if none */

	struct vx_arena_blk *plan_arena;   /* expr tree + literal bytes */

	/* The recognized source, kept so a parallel run can rebuild a
	 * range-scoped source statement that reuses the SAME compiled
	 * plan (the proj/filter trees index the source columns, whose
	 * order is fixed by srccols). */
	char          table[64];
	char          srccols[2100];   /* comma-joined source column list, or "*" */

	/* Per-row scratch: the source row materialized as cells. */
	vx_cell_t    *srcrow;       /* nsrc_col cells */

	vx_chunk_t   *chunk;
	int           cur;
	int           done;

	/* Aggregation execution state (agg path only). */
	vx_htab_t     ht;           /* drained groups */
	int           ht_built;     /* 1 once the source has been drained */

	/* Ordered execution: rows are materialized + sorted on first step. */
	int           ordered_built;

	/* Join execution state (join path only). */
	vx_jht_t     *jht;          /* build-side hash table: key -> row list */
	sqlite3_stmt *probe;        /* probe-side cursor */
	vx_jrow_t    *match;        /* current build match chain for the probe row */
	vx_cell_t    *probe_cells;  /* current probe row's cells */
	struct vx_arena_blk *probe_arena;  /* bytes for the current probe row */
	int           join_built;   /* 1 once build side hashed + probe opened */
	int           probe_done;   /* 1 once the probe cursor is drained */
	int           bscan_b;      /* unmatched-build final scan: bucket idx */
	vx_jrow_t    *bscan_r;      /* unmatched-build final scan: row cursor */
};

/* ---- recognizer: column affinity (SQLite rules, three buckets) --- */

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
	}
	return 0;
}

static enum vx_aff
vx_affinity(const char *decltype)
{
	if (decltype == NULL || decltype[0] == '\0') return VX_AFF_BLOB;
	if (str_contains_ci(decltype, "CHAR") ||
	    str_contains_ci(decltype, "CLOB") ||
	    str_contains_ci(decltype, "TEXT")) return VX_AFF_TEXT;
	if (str_contains_ci(decltype, "BLOB")) return VX_AFF_BLOB;
	return VX_AFF_NUMERIC;
}

/* ---- recognizer: a small column-name table ----------------------- */

struct namevec {
	char names[32][64];
	enum vx_aff aff[32];   /* filled after the source stmt is prepared */
	int  n;
};

static int
nv_add(struct namevec *nv, const sql_str_t *s)
{
	int i;
	if (s == NULL || s->len == 0 || s->len >= 64) return -1;
	for (i = 0; i < nv->n; i++)
		if ((int)strlen(nv->names[i]) == (int)s->len &&
		    memcmp(nv->names[i], s->p, s->len) == 0)
			return i;
	if (nv->n >= 32) return -1;
	memcpy(nv->names[nv->n], s->p, s->len);
	nv->names[nv->n][s->len] = '\0';
	return nv->n++;
}

/* Resolve each source column (by name, in nv order) against the table
 * schema.  Tries the native xstore catalog first (no VDBE, no
 * sqlite_master); falls back to PRAGMA table_info for non-xstore tables
 * or tables without a recorded schema.  Fills nv->aff[i] from the
 * declared type and pay[i] = -1 if the column is the INTEGER PRIMARY KEY
 * (the rowid) else its 0-based payload index.  Returns 0 if every
 * source column resolved, -1 otherwise. */
static int
resolve_schema(sqlite3 *db, const char *table, struct namevec *nv, int *pay)
{
	sqlite3_stmt *ti = NULL;
	char q[128];
	int i, ok = 1, resolved = 0;

	for (i = 0; i < nv->n; i++) pay[i] = -2;   /* unresolved sentinel */

	/* Native catalog first. */
	{
		bt_t *bt = xstore_bt_of(db);
		xstore_col_t cols[64];
		int nc = bt ? xstore_table_schema(bt, table, cols, 64) : 0;
		if (nc > 0) {
			int c;
			for (c = 0; c < nc; c++)
				for (i = 0; i < nv->n; i++) {
					if (pay[i] != -2) continue;
					if (strcmp(nv->names[i], cols[c].name) != 0) continue;
					pay[i] = cols[c].is_pk ? -1 : (c - 1);
					/* The PK is the INTEGER rowid regardless of its declared
					 * type, matching how SQLite reports an INTEGER PRIMARY
					 * KEY -- so its affinity is numeric, not the empty/BLOB
					 * default a bare "k" coldef would give. */
					nv->aff[i] = cols[c].is_pk ? VX_AFF_NUMERIC
					                          : vx_affinity(cols[c].decltype);
					resolved++;
				}
			for (i = 0; i < nv->n; i++) if (pay[i] == -2) ok = 0;
			return (ok && resolved >= nv->n) ? 0 : -1;
		}
	}

	/* Fall back to sqlite_master via PRAGMA table_info. */
	snprintf(q, sizeof q, "PRAGMA table_info(%s)", table);
	if (sqlite3_prepare_v2(db, q, -1, &ti, 0) != SQLITE_OK) return -1;
	while (sqlite3_step(ti) == SQLITE_ROW) {
		int cid = sqlite3_column_int(ti, 0);
		const char *nm = (const char *)sqlite3_column_text(ti, 1);
		const char *ty = (const char *)sqlite3_column_text(ti, 2);
		int pk = sqlite3_column_int(ti, 5);
		if (nm == NULL) continue;
		for (i = 0; i < nv->n; i++) {
			if (pay[i] != -2) continue;
			if (strcmp(nv->names[i], nm) != 0) continue;
			pay[i] = pk ? -1 : (cid - 1);
			nv->aff[i] = vx_affinity(ty);
			resolved++;
		}
	}
	sqlite3_finalize(ti);
	for (i = 0; i < nv->n; i++) if (pay[i] == -2) ok = 0;
	return (ok && resolved >= nv->n) ? 0 : -1;
}

/* ---- expression compiler ----------------------------------------- *
 *
 * Walk the AST expression, collecting referenced base columns into nv
 * and emitting a vx_expr tree.  Returns NULL (and leaves *fail = 1) on
 * any unsupported construct, which the caller turns into a fallback.
 *
 * A compiled node also carries a coarse STATIC TYPE class so the
 * compiler can reject affinity-ambiguous comparisons/arithmetic up
 * front: VX_AFF_NUMERIC (numeric literal/arith/numeric-affinity col),
 * VX_AFF_TEXT (text literal/concat/text-affinity col, text func),
 * VX_AFF_BLOB (unknown -- a blob/none-affinity column).  This class is
 * conservative; when in doubt the compiler fails (fallback). */

/* Join compilation context (V5): two sides, each a table + optional
 * alias contributing a set of source columns to a COMBINED row that is
 * [side0 cols | side1 cols].  A column reference resolves to a combined
 * index by matching its qualifier (alias/table) and name against the
 * sides.  When jc is NULL the compiler is single-table (nv). */
struct vx_joinctx {
	char      tab[2][64];     /* base table name per side */
	char      alias[2][64];   /* alias per side ("" if none) */
	struct namevec col[2];    /* columns collected per side */
	int       base[2];        /* combined-row offset where each side starts */
};

struct vx_compiler {
	struct vx_stmt *st;
	struct namevec *nv;
	struct vx_joinctx *jc;    /* non-NULL on the join path */
	int             fail;
	/* Bound parameters (1-based by ordinal).  A SX_E_PARAM compiles to a
	 * VXO_LIT holding binds[ordinal-1]; binds == NULL means no params
	 * (a ? then fails the compile -> VDBE fallback). */
	const vx_cell_t *binds;
	int              nbinds;
};

/* Slice == C-string (case-sensitive for identifiers, as SQLite folds
 * case only for keywords; column-name matching here is exact). */
static int
str_eq_cstr(const sql_str_t *s, const char *cs)
{
	size_t n = strlen(cs);
	return (size_t)s->len == n && memcmp(s->p, cs, n) == 0;
}

/* Resolve a column reference to a combined-row index in a join context.
 * Handles `col` (unqualified -- must be unambiguous) and `q.col` (q is
 * a table name or alias).  Returns the combined index, or -1 if not
 * found / ambiguous. */
static int
jc_resolve(struct vx_joinctx *jc, const sql_expr_t *e)
{
	const sql_str_t *col;
	const sql_str_t *qual = NULL;
	int side, j, found = -1;

	if (e->op != SX_E_COLUMN || e->nname < 1 || e->nname > 2) return -1;
	if (e->nname == 2) { qual = &e->name[0]; col = &e->name[1]; }
	else col = &e->name[0];

	for (side = 0; side < 2; side++) {
		if (qual) {
			/* Qualifier must match this side's alias (if any) else table. */
			const char *q = jc->alias[side][0] ? jc->alias[side] : jc->tab[side];
			if (!str_eq_cstr(qual, q)) continue;
		}
		for (j = 0; j < jc->col[side].n; j++) {
			if (str_eq_cstr(col, jc->col[side].names[j])) {
				int idx = jc->base[side] + j;
				if (found >= 0) return -1;   /* ambiguous */
				found = idx;
			}
		}
	}
	return found;
}

static vx_expr_t *
expr_node(struct vx_compiler *c, enum vx_op op)
{
	vx_expr_t *e = (vx_expr_t *)arena_alloc(&c->st->plan_arena, sizeof *e);
	if (e == NULL) { c->fail = 1; return NULL; }
	memset(e, 0, sizeof *e);
	e->op = op;
	return e;
}

/* Static type class of a compiled node (best-effort, conservative). */
static enum vx_aff
node_class(const struct vx_compiler *c, const vx_expr_t *e)
{
	switch (e->op) {
	case VXO_LIT:
		if (e->lit.type == VX_INT || e->lit.type == VX_REAL) return VX_AFF_NUMERIC;
		if (e->lit.type == VX_TEXT) return VX_AFF_TEXT;
		return VX_AFF_BLOB;
	case VXO_COL:
		if (c->jc != NULL) {
			/* Combined index -> (side, j) -> that side's affinity. */
			int idx = e->col;
			if (idx >= c->jc->base[1])
				return c->jc->col[1].aff[idx - c->jc->base[1]];
			return c->jc->col[0].aff[idx - c->jc->base[0]];
		}
		return c->nv->aff[e->col];
	case VXO_NEG: case VXO_BITNOT:
	case VXO_ADD: case VXO_SUB: case VXO_MUL: case VXO_DIV: case VXO_MOD:
		return VX_AFF_NUMERIC;
	case VXO_CONCAT:
		return VX_AFF_TEXT;
	case VXO_NOT: case VXO_AND: case VXO_OR:
	case VXO_EQ: case VXO_NE: case VXO_LT: case VXO_LE: case VXO_GT: case VXO_GE:
	case VXO_ISNULL: case VXO_NOTNULL:
	case VXO_IN: case VXO_NOTIN:
		return VX_AFF_NUMERIC;   /* booleans are 0/1 integers */
	case VXO_CASE:
		return VX_AFF_BLOB;      /* dynamic result type: treat as no affinity */
	case VXO_FUNC:
		switch (e->func) {
		case VXF_ABS:    return VX_AFF_NUMERIC;
		case VXF_LENGTH: return VX_AFF_NUMERIC;
		case VXF_LOWER: case VXF_UPPER: return VX_AFF_TEXT;
		default:         return VX_AFF_BLOB;   /* coalesce/ifnull: mixed */
		}
	}
	return VX_AFF_BLOB;
}

/* Case-insensitive exact match of a name slice against a NUL keyword. */
static int
name_is(const sql_str_t *name, const char *kw)
{
	size_t ln = strlen(kw), k;
	if ((size_t)name->len != ln) return 0;
	for (k = 0; k < ln; k++) {
		char x = name->p[k];
		if (x >= 'A' && x <= 'Z') x += 32;
		if (x != kw[k]) return 0;
	}
	return 1;
}

/* Is `name` one of the aggregate functions V3 recognizes? */
static int
is_agg_name(const sql_str_t *name)
{
	return name_is(name, "count") || name_is(name, "sum") ||
	       name_is(name, "total") || name_is(name, "avg") ||
	       name_is(name, "min") || name_is(name, "max");
}

static enum vx_func
func_of(const sql_str_t *name, int *nargs_ok, int nargs)
{
	struct { const char *n; enum vx_func f; int args; } tbl[] = {
		{ "abs", VXF_ABS, 1 }, { "length", VXF_LENGTH, 1 },
		{ "lower", VXF_LOWER, 1 }, { "upper", VXF_UPPER, 1 },
		{ "coalesce", VXF_COALESCE, -1 }, { "ifnull", VXF_IFNULL, 2 }
	};
	size_t i;
	for (i = 0; i < sizeof tbl / sizeof tbl[0]; i++) {
		size_t ln = strlen(tbl[i].n);
		if ((size_t)name->len == ln && str_contains_ci(name->p, tbl[i].n) &&
		    /* exact match, not just substring: lengths already equal */
		    1) {
			/* case-insensitive exact compare */
			size_t k; int ok = 1;
			for (k = 0; k < ln; k++) {
				char x = name->p[k]; if (x >= 'A' && x <= 'Z') x += 32;
				if (x != tbl[i].n[k]) { ok = 0; break; }
			}
			if (!ok) continue;
			if (tbl[i].args >= 0 && tbl[i].args != nargs) { *nargs_ok = 0; return 0; }
			*nargs_ok = 1;
			return tbl[i].f;
		}
	}
	*nargs_ok = 0;
	return 0;
}

static int
tok_to_binop(int tok, enum vx_op *out)
{
	switch (tok) {
	case TK_PLUS: *out = VXO_ADD; return 1;
	case TK_MINUS: *out = VXO_SUB; return 1;
	case TK_STAR: *out = VXO_MUL; return 1;
	case TK_SLASH: *out = VXO_DIV; return 1;
	case TK_PERCENT: *out = VXO_MOD; return 1;
	case TK_CONCAT: *out = VXO_CONCAT; return 1;
	case TK_EQ: *out = VXO_EQ; return 1;
	case TK_NE: *out = VXO_NE; return 1;
	case TK_LT: *out = VXO_LT; return 1;
	case TK_LE: *out = VXO_LE; return 1;
	case TK_GT: *out = VXO_GT; return 1;
	case TK_GE: *out = VXO_GE; return 1;
	case TK_AND: *out = VXO_AND; return 1;
	case TK_OR: *out = VXO_OR; return 1;
	default: return 0;
	}
}

static vx_expr_t *compile_expr(struct vx_compiler *c, const sql_expr_t *e);
static void read_src_cell(sqlite3_stmt *src, int i, vx_cell_t *cell,
                          struct vx_arena_blk **arena);
/* Walk an AST expression collecting referenced base-column NAMES into
 * nv (so the source SELECT can list them) and rejecting any construct
 * the V1 compiler does not support -- WITHOUT applying the affinity
 * gate, which needs column affinities not known until the source is
 * prepared.  Returns 0 if the whole expression is compilable in
 * principle, -1 if it contains an unsupported construct. */
static int
collect_columns(struct namevec *nv, const sql_expr_t *e)
{
	const sql_exprlist_item_t *it;
	if (e == NULL) return -1;
	switch (e->op) {
	case SX_E_NULL: case SX_E_NUMBER: case SX_E_STRING: case SX_E_PARAM:
		return 0;
	case SX_E_COLUMN:
		if (e->nname < 1 || e->nname > 2) return -1;
		return nv_add(nv, &e->name[e->nname - 1]) < 0 ? -1 : 0;
	case SX_E_UNARY:
		if (e->op2 != TK_MINUS && e->op2 != TK_PLUS && e->op2 != TK_NOT)
			return -1;
		return collect_columns(nv, e->a);
	case SX_E_BINARY: {
		enum vx_op dummy;
		if (!tok_to_binop(e->op2, &dummy)) return -1;
		if (collect_columns(nv, e->a) != 0) return -1;
		return collect_columns(nv, e->b);
	}
	case SX_E_IS_NULL:
		return collect_columns(nv, e->a);
	case SX_E_BETWEEN:
		if (collect_columns(nv, e->a) != 0) return -1;
		if (collect_columns(nv, e->b) != 0) return -1;
		return collect_columns(nv, e->c);
	case SX_E_CASE: {
		const sql_case_arm_t *arm;
		if (e->a != NULL && collect_columns(nv, e->a) != 0) return -1;
		for (arm = e->arms; arm; arm = arm->next) {
			if (collect_columns(nv, arm->when) != 0) return -1;
			if (collect_columns(nv, arm->then) != 0) return -1;
		}
		if (e->els != NULL && collect_columns(nv, e->els) != 0) return -1;
		return 0;
	}
	case SX_E_SUBQUERY:
		/* An uncorrelated scalar subquery references no OUTER column (it
		 * is spliced as a literal at compile time); a correlated one is
		 * rejected later when its standalone prepare fails.  Either way it
		 * contributes no column to the outer namevec. */
		return 0;
	case SX_E_IN_SELECT:
		/* a IN (SELECT ...): collect the operand's columns; the subquery
		 * itself contributes none (materialized as a literal list). */
		return collect_columns(nv, e->a);
	case SX_E_IN_LIST: {
		/* a IN (v1, v2, ...): operand + each list element. */
		if (e->a == NULL || e->sel != NULL) return -1;   /* IN (SELECT): not here */
		if (collect_columns(nv, e->a) != 0) return -1;
		for (it = e->list ? e->list->head : NULL; it; it = it->next)
			if (collect_columns(nv, it->expr) != 0) return -1;
		return 0;
	}
	case SX_E_FUNC: {
		int nargs = 0, ok = 0;
		if (e->ival & 3) return -1;   /* DISTINCT or func(*) */
		for (it = e->list ? e->list->head : NULL; it; it = it->next) nargs++;
		(void)func_of(&e->name[0], &ok, nargs);
		if (!ok) return -1;
		for (it = e->list ? e->list->head : NULL; it; it = it->next)
			if (collect_columns(nv, it->expr) != 0) return -1;
		return 0;
	}
	default:
		return -1;   /* BETWEEN/IN/CASE/subquery/param/bool/blob: not V1 */
	}
}

/* Derive the output column name for a select item, matching SQLite:
 * the AS alias if present, else for a bare (optionally qualified) column
 * reference the unqualified column name, else the expression's VERBATIM
 * SOURCE TEXT (its span in the original SQL) -- which is how SQLite
 * names an expression column (a+1, count(*), a || b).  Leaves dst empty
 * only if no name can be derived (the caller then uses the VDBE name). */
static void
item_name(const sql_exprlist_item_t *it, char *dst, size_t cap)
{
	const sql_expr_t *e;
	const char *sp; int sl;
	dst[0] = '\0';
	if (it == NULL) return;
	if (it->alias.len > 0 && it->alias.len < cap) {
		memcpy(dst, it->alias.p, it->alias.len);
		dst[it->alias.len] = '\0';
		return;
	}
	e = it->expr;
	if (e != NULL && e->op == SX_E_COLUMN && e->nname >= 1) {
		const sql_str_t *nm = &e->name[e->nname - 1];   /* unqualified part */
		if (nm->len > 0 && nm->len < cap) {
			memcpy(dst, nm->p, nm->len);
			dst[nm->len] = '\0';
		}
		return;
	}
	/* Expression column: name it by its verbatim source span. */
	if (sql_expr_span(e, &sp, &sl) && sl > 0 && (size_t)sl < cap) {
		memcpy(dst, sp, (size_t)sl);
		dst[sl] = '\0';
	}
}

/* Compile a literal AST node into a VXO_LIT (INT/REAL/TEXT/NULL). */
static vx_expr_t *
compile_literal(struct vx_compiler *c, const sql_expr_t *e)
{
	vx_expr_t *n = expr_node(c, VXO_LIT);
	char buf[64];
	if (n == NULL) return NULL;
	switch (e->op) {
	case SX_E_NULL:
		n->lit.type = VX_NULL;
		return n;
	case SX_E_NUMBER: {
		uint32_t i; int isreal = 0;
		for (i = 0; i < e->lit.len; i++) {
			char ch = e->lit.p[i];
			if (ch == '.' || ch == 'e' || ch == 'E') { isreal = 1; break; }
		}
		if (e->lit.len == 0 || e->lit.len >= sizeof buf) { c->fail = 1; return NULL; }
		memcpy(buf, e->lit.p, e->lit.len); buf[e->lit.len] = '\0';
		if (isreal) { n->lit.type = VX_REAL; n->lit.r = strtod(buf, NULL); }
		else        { n->lit.type = VX_INT;  n->lit.i = strtoll(buf, NULL, 10); }
		return n;
	}
	case SX_E_STRING: {
		uint8_t *p = (uint8_t *)arena_alloc(&c->st->plan_arena, e->lit.len + 1);
		if (p == NULL) { c->fail = 1; return NULL; }
		if (e->lit.len) memcpy(p, e->lit.p, e->lit.len);
		p[e->lit.len] = '\0';
		n->lit.type = VX_TEXT; n->lit.bytes = p; n->lit.nbytes = e->lit.len;
		return n;
	}
	default:
		c->fail = 1;
		return NULL;
	}
}

/* Compile a ? parameter into a VXO_LIT holding its bound value (the live
 * path binds before executing, so the value is known at compile time).
 * TEXT/BLOB bytes are copied into the plan arena.  Fails the compile --
 * VDBE fallback -- if no binds were supplied or the ordinal is out of
 * range. */
static vx_expr_t *
compile_param(struct vx_compiler *c, const sql_expr_t *e)
{
	vx_expr_t *n;
	int ord = (int)e->ival;   /* 1-based */
	const vx_cell_t *v;
	if (c->binds == NULL || ord < 1 || ord > c->nbinds) { c->fail = 1; return NULL; }
	n = expr_node(c, VXO_LIT);
	if (n == NULL) { c->fail = 1; return NULL; }
	v = &c->binds[ord - 1];
	if (v->type == VX_TEXT || v->type == VX_BLOB) {
		uint8_t *p = (uint8_t *)arena_alloc(&c->st->plan_arena,
		                                    (size_t)v->nbytes + 1);
		if (p == NULL) { c->fail = 1; return NULL; }
		if (v->nbytes) memcpy(p, v->bytes, v->nbytes);
		p[v->nbytes] = '\0';
		n->lit.type = v->type; n->lit.bytes = p; n->lit.nbytes = v->nbytes;
	} else {
		n->lit = *v;   /* INT / REAL / NULL: by value */
	}
	return n;
}

/* Compile an UNCORRELATED scalar subquery (SELECT ...) used as an
 * expression.  The subquery is run ONCE via SQLite (the same engine that
 * is the fallback / oracle) and its single scalar result -- first column
 * of the first row, or NULL if it returns no row -- is spliced in as a
 * literal.  Correct for any subquery that does not reference the outer
 * row (no correlation); a correlated subquery would need per-row
 * evaluation and is left to the VDBE.  Recognition is conservative: the
 * subquery must carry its verbatim source span (set by the grammar),
 * return exactly one column, and use no bind parameters (binds are not
 * threaded into the inner run).  Anything else sets c->fail so the whole
 * statement falls back. */

/* Prepare an uncorrelated subquery from its verbatim source span (which
 * includes the wrapping parens).  Strips the outer '(' ')' and prepares
 * the inner SELECT standalone via SQLite.  Returns the prepared stmt, or
 * NULL (correlated / unparseable / has parameters) -- the caller then
 * falls back.  Requires exactly `want_ncol` result columns. */
static sqlite3_stmt *
prepare_subquery(sqlite3 *db, const char *src, uint32_t srclen, int want_ncol)
{
	sqlite3_stmt *q = NULL;
	char *sub, *s;
	size_t len = srclen;
	if (db == NULL || src == NULL || srclen == 0) return NULL;
	sub = (char *)malloc((size_t)srclen + 1);
	if (sub == NULL) return NULL;
	memcpy(sub, src, srclen); sub[srclen] = '\0';
	s = sub;
	while (len > 0 && (*s == ' ' || *s == '\t' || *s == '\n')) { s++; len--; }
	while (len > 0 && (s[len-1]==' '||s[len-1]=='\t'||s[len-1]=='\n')) len--;
	if (len < 2 || s[0] != '(' || s[len-1] != ')') { free(sub); return NULL; }
	s[len-1] = '\0';
	memmove(sub, s + 1, len - 1);   /* drop leading '(' */
	if (sqlite3_prepare_v2(db, sub, -1, &q, NULL) != SQLITE_OK) {
		free(sub); return NULL;   /* correlated / unparseable -> VDBE */
	}
	free(sub);
	if (sqlite3_bind_parameter_count(q) != 0 ||
	    sqlite3_column_count(q) != want_ncol) {
		sqlite3_finalize(q); return NULL;
	}
	return q;
}

static vx_expr_t *
compile_scalar_subquery(struct vx_compiler *c, const sql_expr_t *e)
{
	sqlite3_stmt *q = NULL;
	vx_expr_t *n;
	int step;

	if (c->st == NULL || c->st->db == NULL) { c->fail = 1; return NULL; }
	/* A correlated subquery references an outer column; SQLite's prepare
	 * is the gate -- run standalone, an unknown column fails to prepare
	 * and we fall back.  An uncorrelated subquery prepares + runs here. */
	q = prepare_subquery(c->st->db, e->src, e->srclen, 1);
	if (q == NULL) { c->fail = 1; return NULL; }

	n = expr_node(c, VXO_LIT);
	if (n == NULL) { sqlite3_finalize(q); c->fail = 1; return NULL; }

	step = sqlite3_step(q);
	if (step == SQLITE_DONE) {
		n->lit.type = VX_NULL;            /* empty subquery -> NULL */
	} else if (step == SQLITE_ROW) {
		switch (sqlite3_column_type(q, 0)) {
		case SQLITE_INTEGER:
			n->lit.type = VX_INT; n->lit.i = sqlite3_column_int64(q, 0); break;
		case SQLITE_FLOAT:
			n->lit.type = VX_REAL; n->lit.r = sqlite3_column_double(q, 0); break;
		case SQLITE_TEXT: case SQLITE_BLOB: {
			const void *b = sqlite3_column_blob(q, 0);
			int nb = sqlite3_column_bytes(q, 0);
			uint8_t *p = (uint8_t *)arena_alloc(&c->st->plan_arena, (size_t)nb + 1);
			if (p == NULL) { sqlite3_finalize(q); c->fail = 1; return NULL; }
			if (nb && b) memcpy(p, b, (size_t)nb);
			p[nb] = '\0';
			n->lit.type = (sqlite3_column_type(q, 0) == SQLITE_TEXT) ? VX_TEXT : VX_BLOB;
			n->lit.bytes = p; n->lit.nbytes = nb;
			break; }
		default:
			n->lit.type = VX_NULL; break;
		}
	} else {
		sqlite3_finalize(q); c->fail = 1; return NULL;   /* error -> VDBE */
	}
	sqlite3_finalize(q);
	return n;
}

/* Build a VXO_LIT node from a SQLite result column (copying TEXT/BLOB
 * into the plan arena).  Returns the node, or NULL (sets c->fail). */
static vx_expr_t *
lit_node_from_col(struct vx_compiler *c, sqlite3_stmt *q, int col)
{
	vx_expr_t *n = expr_node(c, VXO_LIT);
	if (n == NULL) { c->fail = 1; return NULL; }
	switch (sqlite3_column_type(q, col)) {
	case SQLITE_INTEGER:
		n->lit.type = VX_INT; n->lit.i = sqlite3_column_int64(q, col); break;
	case SQLITE_FLOAT:
		n->lit.type = VX_REAL; n->lit.r = sqlite3_column_double(q, col); break;
	case SQLITE_TEXT: case SQLITE_BLOB: {
		const void *b = sqlite3_column_blob(q, col);
		int nb = sqlite3_column_bytes(q, col);
		uint8_t *p = (uint8_t *)arena_alloc(&c->st->plan_arena, (size_t)nb + 1);
		if (p == NULL) { c->fail = 1; return NULL; }
		if (nb && b) memcpy(p, b, (size_t)nb);
		p[nb] = '\0';
		n->lit.type = (sqlite3_column_type(q, col) == SQLITE_TEXT) ? VX_TEXT : VX_BLOB;
		n->lit.bytes = p; n->lit.nbytes = nb;
		break; }
	default:
		n->lit.type = VX_NULL; break;
	}
	return n;
}

/* Max rows materialized from an IN (SELECT ...) subquery; a larger
 * result set falls back to the VDBE rather than build an unbounded
 * literal list. */
#define VX_IN_SELECT_MAX 4096

/* Compile a IN (SELECT ...) (or NOT IN) against an UNCORRELATED, single-
 * column subquery.  The subquery is run ONCE, its values materialized as
 * a literal list, and the membership test reuses the VXO_IN / VXO_NOTIN
 * machinery (same as IN (list)).  Each value must share the operand's
 * affinity class.  Returns the node, or NULL (sets c->fail -> VDBE) for
 * a correlated / parametrized / too-large / mixed-affinity subquery. */
static vx_expr_t *
compile_in_select(struct vx_compiler *c, const sql_expr_t *e)
{
	sqlite3_stmt *q = NULL;
	vx_expr_t *n, *a;
	enum vx_aff acls;
	int step, cnt = 0, cap = 16;

	if (c->st == NULL || c->st->db == NULL) { c->fail = 1; return NULL; }
	a = compile_expr(c, e->a);
	if (a == NULL) return NULL;
	acls = node_class(c, a);

	q = prepare_subquery(c->st->db, e->src, e->srclen, 1);
	if (q == NULL) { c->fail = 1; return NULL; }

	n = expr_node(c, e->ival ? VXO_NOTIN : VXO_IN);   /* ival = NOT IN */
	if (n == NULL) { sqlite3_finalize(q); c->fail = 1; return NULL; }
	n->a = a;
	n->args = (vx_expr_t **)arena_alloc(&c->st->plan_arena,
	                                    sizeof(vx_expr_t *) * (size_t)cap);
	if (n->args == NULL) { sqlite3_finalize(q); c->fail = 1; return NULL; }

	while ((step = sqlite3_step(q)) == SQLITE_ROW) {
		vx_expr_t *v;
		if (cnt >= VX_IN_SELECT_MAX) { sqlite3_finalize(q); c->fail = 1; return NULL; }
		if (cnt == cap) {
			int nc = cap * 2;
			vx_expr_t **na = (vx_expr_t **)arena_alloc(&c->st->plan_arena,
			                                  sizeof(vx_expr_t *) * (size_t)nc);
			if (na == NULL) { sqlite3_finalize(q); c->fail = 1; return NULL; }
			memcpy(na, n->args, sizeof(vx_expr_t *) * (size_t)cnt);
			n->args = na; cap = nc;
		}
		v = lit_node_from_col(c, q, 0);
		if (v == NULL) { sqlite3_finalize(q); return NULL; }
		/* A NULL subquery value participates in IN's 3-valued logic; the
		 * VXO_IN eval already treats a NULL list element correctly, but
		 * the affinity gate only applies to non-NULL values. */
		if (v->lit.type != VX_NULL && node_class(c, v) != acls) {
			sqlite3_finalize(q); c->fail = 1; return NULL;
		}
		n->args[cnt++] = v;
	}
	if (step != SQLITE_DONE) { sqlite3_finalize(q); c->fail = 1; return NULL; }
	sqlite3_finalize(q);

	if (cnt == 0) {
		/* IN (empty) is always false; NOT IN (empty) always true.  The
		 * VXO_IN eval needs >=1 arg, so leave the empty case to the VDBE. */
		c->fail = 1; return NULL;
	}
	n->nargs = cnt;
	return n;
}

static vx_expr_t *
compile_expr(struct vx_compiler *c, const sql_expr_t *e)
{
	if (c->fail || e == NULL) { c->fail = 1; return NULL; }

	switch (e->op) {
	case SX_E_SUBQUERY:
		return compile_scalar_subquery(c, e);
	case SX_E_IN_SELECT:
		return compile_in_select(c, e);
	case SX_E_NULL: case SX_E_NUMBER: case SX_E_STRING:
		return compile_literal(c, e);
	case SX_E_PARAM:
		return compile_param(c, e);

	case SX_E_COLUMN: {
		const sql_str_t *cn;
		vx_expr_t *n;
		int idx;
		if (e->nname < 1 || e->nname > 2) { c->fail = 1; return NULL; }
		if (c->jc != NULL) {
			idx = jc_resolve(c->jc, e);   /* combined-row index */
		} else {
			cn = &e->name[e->nname - 1];
			idx = nv_add(c->nv, cn);
		}
		if (idx < 0) { c->fail = 1; return NULL; }
		n = expr_node(c, VXO_COL);
		if (n) n->col = idx;
		return n;
	}

	case SX_E_UNARY: {
		vx_expr_t *n, *a = compile_expr(c, e->a);
		if (a == NULL) return NULL;
		if (e->op2 == TK_MINUS) {
			if (node_class(c, a) != VX_AFF_NUMERIC) { c->fail = 1; return NULL; }
			n = expr_node(c, VXO_NEG);
		} else if (e->op2 == TK_NOT) {
			n = expr_node(c, VXO_NOT);
		} else if (e->op2 == TK_PLUS) {
			return a;   /* unary plus is identity for numerics */
		} else {
			c->fail = 1; return NULL;   /* ~ etc. not in V1 */
		}
		if (n) n->a = a;
		return n;
	}

	case SX_E_BINARY: {
		enum vx_op op;
		vx_expr_t *n, *a, *b;
		if (!tok_to_binop(e->op2, &op)) { c->fail = 1; return NULL; }
		a = compile_expr(c, e->a);
		b = compile_expr(c, e->b);
		if (a == NULL || b == NULL) return NULL;

		/* Affinity safety for arithmetic and comparison. */
		switch (op) {
		case VXO_ADD: case VXO_SUB: case VXO_MUL: case VXO_DIV: case VXO_MOD:
			/* Both operands must be numeric (no text->number coercion). */
			if (node_class(c, a) != VX_AFF_NUMERIC ||
			    node_class(c, b) != VX_AFF_NUMERIC) { c->fail = 1; return NULL; }
			break;
		case VXO_CONCAT:
			/* || coerces operands to text; allow numeric or text, not blob. */
			if (node_class(c, a) == VX_AFF_BLOB ||
			    node_class(c, b) == VX_AFF_BLOB) { c->fail = 1; return NULL; }
			break;
		case VXO_EQ: case VXO_NE: case VXO_LT: case VXO_LE:
		case VXO_GT: case VXO_GE: {
			/* No-coercion gate: both numeric, or both text.  Anything
			 * with a blob/none-affinity operand or a numeric-vs-text
			 * pairing could coerce -> fall back. */
			enum vx_aff ca = node_class(c, a), cb = node_class(c, b);
			if (!((ca == VX_AFF_NUMERIC && cb == VX_AFF_NUMERIC) ||
			      (ca == VX_AFF_TEXT && cb == VX_AFF_TEXT))) {
				c->fail = 1; return NULL;
			}
			break;
		}
		default: break;   /* AND/OR: operands are boolean-ish, fine */
		}

		n = expr_node(c, op);
		if (n) { n->a = a; n->b = b; }
		return n;
	}

	case SX_E_IS_NULL: {
		vx_expr_t *n = expr_node(c, e->ival ? VXO_NOTNULL : VXO_ISNULL);
		vx_expr_t *a = compile_expr(c, e->a);
		if (a == NULL) return NULL;
		if (n) n->a = a;
		return n;
	}

	case SX_E_BETWEEN: {
		/* a BETWEEN lo AND hi  ==  (a >= lo) AND (a <= hi).  Desugar so
		 * it reuses the comparison ops + the no-coercion affinity gate
		 * (each comparison's operands must be both-numeric or both-text).
		 * SQLite evaluates `a` once, but with no side effects in the
		 * supported expression set, evaluating it twice is equivalent. */
		vx_expr_t *operand, *operand2, *lo, *hi, *gel, *leh, *conj;
		enum vx_aff ca, clo, chi;
		operand = compile_expr(c, e->a);
		operand2 = compile_expr(c, e->a);   /* second copy: avoid node sharing */
		lo = compile_expr(c, e->b);
		hi = compile_expr(c, e->c);
		if (operand == NULL || operand2 == NULL || lo == NULL || hi == NULL)
			return NULL;
		ca = node_class(c, operand); clo = node_class(c, lo); chi = node_class(c, hi);
		/* Both bounds must compare to the operand under the no-coercion
		 * rule (both numeric, or both text). */
		if (!(((ca == VX_AFF_NUMERIC && clo == VX_AFF_NUMERIC) ||
		       (ca == VX_AFF_TEXT && clo == VX_AFF_TEXT)) &&
		      ((ca == VX_AFF_NUMERIC && chi == VX_AFF_NUMERIC) ||
		       (ca == VX_AFF_TEXT && chi == VX_AFF_TEXT)))) {
			c->fail = 1; return NULL;
		}
		gel = expr_node(c, VXO_GE);
		leh = expr_node(c, VXO_LE);
		conj = expr_node(c, VXO_AND);
		if (gel == NULL || leh == NULL || conj == NULL) return NULL;
		gel->a = operand; gel->b = lo;
		leh->a = operand2; leh->b = hi;
		conj->a = gel; conj->b = leh;
		return conj;
	}

	case SX_E_CASE: {
		/* CASE [base] WHEN w THEN t ... [ELSE e] END.
		 * Searched form (no base): each WHEN is a boolean predicate.
		 * Simple form (CASE x WHEN v ...): x = v per arm; compile the
		 * base once and an equality per arm.  args[] holds the arms as
		 * when0,then0,when1,then1,...; nargs = 2*arms; ELSE in .b.
		 * The THEN/ELSE values may be any type (result class is dynamic,
		 * so node_class returns BLOB -> a CASE cannot be a comparison
		 * operand, only a projection / boolean-tested value). */
		const sql_case_arm_t *arm;
		vx_expr_t *n, *base = NULL;
		int narm = 0, k;
		for (arm = e->arms; arm; arm = arm->next) narm++;
		if (narm == 0 || narm > 32) { c->fail = 1; return NULL; }
		if (e->a != NULL) {
			base = compile_expr(c, e->a);
			if (base == NULL) return NULL;
		}
		n = expr_node(c, VXO_CASE);
		if (n == NULL) return NULL;
		n->a = base;
		n->args = (vx_expr_t **)arena_alloc(&c->st->plan_arena,
		                                    sizeof(vx_expr_t *) * (size_t)(narm * 2));
		if (n->args == NULL) { c->fail = 1; return NULL; }
		k = 0;
		for (arm = e->arms; arm; arm = arm->next) {
			vx_expr_t *w, *t;
			if (base != NULL) {
				/* simple form: compile (base = arm->when) with the
				 * comparison affinity gate. */
				vx_expr_t *base2 = compile_expr(c, e->a);
				vx_expr_t *val = compile_expr(c, arm->when);
				enum vx_aff cb, cv;
				if (base2 == NULL || val == NULL) return NULL;
				cb = node_class(c, base2); cv = node_class(c, val);
				if (!((cb == VX_AFF_NUMERIC && cv == VX_AFF_NUMERIC) ||
				      (cb == VX_AFF_TEXT && cv == VX_AFF_TEXT))) {
					c->fail = 1; return NULL;
				}
				w = expr_node(c, VXO_EQ);
				if (w == NULL) return NULL;
				w->a = base2; w->b = val;
			} else {
				/* searched form: WHEN is a boolean predicate. */
				w = compile_expr(c, arm->when);
				if (w == NULL) return NULL;
			}
			t = compile_expr(c, arm->then);
			if (t == NULL) return NULL;
			n->args[k++] = w;
			n->args[k++] = t;
		}
		n->nargs = narm * 2;
		if (e->els != NULL) {
			n->b = compile_expr(c, e->els);
			if (n->b == NULL) return NULL;
		}
		return n;
	}

	case SX_E_IN_LIST: {
		/* a IN (v1, v2, ...) -> VXO_IN with operand .a and list args[].		 * Each list element must share the operand's affinity class
		 * (the comparison gate), so the membership test is numeric/
		 * numeric or text/text -- otherwise fall back. */
		const sql_exprlist_item_t *it;
		vx_expr_t *n, *a;
		enum vx_aff acls;
		int cnt = 0, k;
		if (e->sel != NULL) { c->fail = 1; return NULL; }   /* IN (SELECT) */
		a = compile_expr(c, e->a);
		if (a == NULL) return NULL;
		acls = node_class(c, a);
		for (it = e->list ? e->list->head : NULL; it; it = it->next) cnt++;
		if (cnt == 0) { c->fail = 1; return NULL; }   /* empty IN list: VDBE */
		n = expr_node(c, e->ival ? VXO_NOTIN : VXO_IN);   /* ival = NOT IN */
		if (n == NULL) return NULL;
		n->a = a;
		n->args = (vx_expr_t **)arena_alloc(&c->st->plan_arena,
		                                    sizeof(vx_expr_t *) * (size_t)cnt);
		if (n->args == NULL) { c->fail = 1; return NULL; }
		k = 0;
		for (it = e->list->head; it; it = it->next) {
			vx_expr_t *v = compile_expr(c, it->expr);
			if (v == NULL) return NULL;
			if (node_class(c, v) != acls) { c->fail = 1; return NULL; }
			n->args[k++] = v;
		}
		n->nargs = cnt;
		return n;
	}

	case SX_E_FUNC: {
		int nargs = 0, ok = 0;
		const sql_exprlist_item_t *it;
		enum vx_func f;
		vx_expr_t *n;
		int k;
		if (e->ival & 1) { c->fail = 1; return NULL; }   /* DISTINCT agg */
		if (e->ival & 2) { c->fail = 1; return NULL; }   /* func(*) */
		for (it = e->list ? e->list->head : NULL; it; it = it->next) nargs++;
		f = func_of(&e->name[0], &ok, nargs);
		if (!ok) { c->fail = 1; return NULL; }
		n = expr_node(c, VXO_FUNC);
		if (n == NULL) return NULL;
		n->func = f;
		n->nargs = nargs;
		n->args = (vx_expr_t **)arena_alloc(&c->st->plan_arena,
		    sizeof(vx_expr_t *) * (size_t)(nargs > 0 ? nargs : 1));
		if (n->args == NULL) { c->fail = 1; return NULL; }
		k = 0;
		for (it = e->list ? e->list->head : NULL; it; it = it->next) {
			n->args[k] = compile_expr(c, it->expr);
			if (n->args[k] == NULL) return NULL;
			k++;
		}
		/* abs requires a numeric argument (no text coercion). */
		if (f == VXF_ABS && node_class(c, n->args[0]) != VX_AFF_NUMERIC) {
			c->fail = 1; return NULL;
		}
		return n;
	}

	default:
		c->fail = 1;
		return NULL;   /* BETWEEN, IN, CASE, subquery, params: not in V1 */
	}
}

/* ---- evaluation -------------------------------------------------- */

/* A transient evaluation result; TEXT/BLOB bytes live in the chunk
 * arena (the caller passes it through). */
static void
num_of(const vx_cell_t *c, double *d, int64_t *i, int *is_int)
{
	if (c->type == VX_INT) { *i = c->i; *d = (double)c->i; *is_int = 1; }
	else { *d = c->r; *i = (int64_t)c->r; *is_int = 0; }
}

static void eval(const struct vx_stmt *st, const vx_expr_t *e,
                 const vx_cell_t *row, struct vx_arena_blk **arena,
                 vx_cell_t *out);

/* Three-valued: returns 1 true, 0 false, -1 NULL/unknown. */
static int
eval_bool(const struct vx_stmt *st, const vx_expr_t *e,
          const vx_cell_t *row, struct vx_arena_blk **arena)
{
	vx_cell_t v;
	if (e->op == VXO_AND) {
		int x = eval_bool(st, e->a, row, arena);
		int y = eval_bool(st, e->b, row, arena);
		if (x == 0 || y == 0) return 0;             /* F AND anything = F */
		if (x == 1 && y == 1) return 1;
		return -1;
	}
	if (e->op == VXO_OR) {
		int x = eval_bool(st, e->a, row, arena);
		int y = eval_bool(st, e->b, row, arena);
		if (x == 1 || y == 1) return 1;             /* T OR anything = T */
		if (x == 0 && y == 0) return 0;
		return -1;
	}
	if (e->op == VXO_NOT) {
		int x = eval_bool(st, e->a, row, arena);
		return (x < 0) ? -1 : !x;
	}
	eval(st, e, row, arena, &v);
	if (v.type == VX_NULL) return -1;
	if (v.type == VX_INT) return v.i != 0;
	if (v.type == VX_REAL) return v.r != 0.0;
	/* TEXT/BLOB as boolean: SQLite treats non-numeric text as 0/false in
	 * a boolean context.  Be conservative: only numeric truthiness here
	 * (compiler keeps WHERE operands numeric/boolean). */
	return 0;
}

/* Compare two non-NULL cells of the SAME affinity class (the compiler
 * guarantees comparisons are numeric/numeric or text/text). */
static int
cmp_vals(const vx_cell_t *a, const vx_cell_t *b)
{
	if ((a->type == VX_INT || a->type == VX_REAL) &&
	    (b->type == VX_INT || b->type == VX_REAL)) {
		if (a->type == VX_INT && b->type == VX_INT)
			return (a->i < b->i) ? -1 : (a->i > b->i) ? 1 : 0;
		{
			double x, y; int64_t xi, yi; int ii;
			num_of(a, &x, &xi, &ii); num_of(b, &y, &yi, &ii);
			return (x < y) ? -1 : (x > y) ? 1 : 0;
		}
	}
	/* text/blob: BINARY collation (memcmp). */
	{
		uint32_t n = a->nbytes < b->nbytes ? a->nbytes : b->nbytes;
		int r = n ? memcmp(a->bytes, b->bytes, n) : 0;
		if (r) return r;
		return (int)a->nbytes - (int)b->nbytes;
	}
}

/* Full storage-class-aware comparison for ORDER BY, matching SQLite's
 * sqlite3MemCompare: NULL sorts first, then numbers (int/real compared
 * by value), then TEXT (BINARY collation), then BLOB. */
static int
sort_cmp(const vx_cell_t *a, const vx_cell_t *b)
{
	int na = (a->type == VX_NULL), nb = (b->type == VX_NULL);
	int numa, numb;
	if (na || nb) return nb - na;   /* both NULL -> 0; NULL is smallest */
	numa = (a->type == VX_INT || a->type == VX_REAL);
	numb = (b->type == VX_INT || b->type == VX_REAL);
	if (numa || numb) {
		if (numa && numb) return cmp_vals(a, b);
		return numa ? -1 : 1;       /* number sorts before text/blob */
	}
	/* both are TEXT or BLOB: TEXT sorts before BLOB. */
	if (a->type != b->type)
		return (a->type == VX_TEXT) ? -1 : 1;
	{
		uint32_t n = a->nbytes < b->nbytes ? a->nbytes : b->nbytes;
		int r = n ? memcmp(a->bytes, b->bytes, n) : 0;
		if (r) return r;
		return (int)a->nbytes - (int)b->nbytes;
	}
}

static void
eval(const struct vx_stmt *st, const vx_expr_t *e,
     const vx_cell_t *row, struct vx_arena_blk **arena, vx_cell_t *out)
{
	memset(out, 0, sizeof *out);

	switch (e->op) {
	case VXO_COL:   *out = row[e->col]; return;
	case VXO_LIT:   *out = e->lit; return;

	case VXO_NEG: {
		vx_cell_t a; eval(st, e->a, row, arena, &a);
		if (a.type == VX_NULL) { out->type = VX_NULL; return; }
		if (a.type == VX_INT) { out->type = VX_INT; out->i = -a.i; }
		else { out->type = VX_REAL; out->r = -a.r; }
		return;
	}
	case VXO_NOT: {
		int x = eval_bool(st, e->a, row, arena);
		if (x < 0) { out->type = VX_NULL; return; }
		out->type = VX_INT; out->i = !x;
		return;
	}

	case VXO_ADD: case VXO_SUB: case VXO_MUL: case VXO_DIV: case VXO_MOD: {
		vx_cell_t a, b; double da, db; int64_t ia, ib; int aint, bint;
		eval(st, e->a, row, arena, &a);
		eval(st, e->b, row, arena, &b);
		if (a.type == VX_NULL || b.type == VX_NULL) { out->type = VX_NULL; return; }
		num_of(&a, &da, &ia, &aint);
		num_of(&b, &db, &ib, &bint);
		if (e->op == VXO_DIV || e->op == VXO_MOD) {
			if ((bint && ib == 0) || (!bint && db == 0.0)) {
				out->type = VX_NULL; return;   /* SQLite: x/0 = NULL */
			}
		}
		if (aint && bint && e->op != VXO_DIV) {
			out->type = VX_INT;
			switch (e->op) {
			case VXO_ADD: out->i = ia + ib; break;
			case VXO_SUB: out->i = ia - ib; break;
			case VXO_MUL: out->i = ia * ib; break;
			case VXO_MOD: out->i = ib ? (ia % ib) : 0; break;
			default: break;
			}
		} else if (aint && bint && e->op == VXO_DIV) {
			out->type = VX_INT; out->i = ia / ib;   /* SQLite integer division */
		} else {
			out->type = VX_REAL;
			switch (e->op) {
			case VXO_ADD: out->r = da + db; break;
			case VXO_SUB: out->r = da - db; break;
			case VXO_MUL: out->r = da * db; break;
			case VXO_DIV: out->r = da / db; break;
			case VXO_MOD: out->r = fmod(da, db); break;
			default: break;
			}
		}
		return;
	}

	case VXO_CONCAT: {
		vx_cell_t a, b;
		char ta[64], tb[64];
		const uint8_t *pa, *pb; uint32_t na, nb;
		uint8_t *buf;
		eval(st, e->a, row, arena, &a);
		eval(st, e->b, row, arena, &b);
		if (a.type == VX_NULL || b.type == VX_NULL) { out->type = VX_NULL; return; }
		/* Render each operand to text (SQLite TEXT coercion of numerics). */
		if (a.type == VX_INT) { na = (uint32_t)snprintf(ta, sizeof ta, "%lld", (long long)a.i); pa = (uint8_t *)ta; }
		else if (a.type == VX_REAL) { na = (uint32_t)snprintf(ta, sizeof ta, "%g", a.r); pa = (uint8_t *)ta; }
		else { pa = a.bytes; na = a.nbytes; }
		if (b.type == VX_INT) { nb = (uint32_t)snprintf(tb, sizeof tb, "%lld", (long long)b.i); pb = (uint8_t *)tb; }
		else if (b.type == VX_REAL) { nb = (uint32_t)snprintf(tb, sizeof tb, "%g", b.r); pb = (uint8_t *)tb; }
		else { pb = b.bytes; nb = b.nbytes; }
		buf = (uint8_t *)arena_alloc(arena, (size_t)na + nb + 1);
		if (buf == NULL) { out->type = VX_NULL; return; }
		if (na) memcpy(buf, pa, na);
		if (nb) memcpy(buf + na, pb, nb);
		buf[na + nb] = '\0';
		out->type = VX_TEXT; out->bytes = buf; out->nbytes = na + nb;
		return;
	}

	case VXO_EQ: case VXO_NE: case VXO_LT: case VXO_LE:
	case VXO_GT: case VXO_GE: {
		vx_cell_t a, b; int c;
		eval(st, e->a, row, arena, &a);
		eval(st, e->b, row, arena, &b);
		if (a.type == VX_NULL || b.type == VX_NULL) { out->type = VX_NULL; return; }
		c = cmp_vals(&a, &b);
		out->type = VX_INT;
		switch (e->op) {
		case VXO_EQ: out->i = (c == 0); break;
		case VXO_NE: out->i = (c != 0); break;
		case VXO_LT: out->i = (c < 0); break;
		case VXO_LE: out->i = (c <= 0); break;
		case VXO_GT: out->i = (c > 0); break;
		case VXO_GE: out->i = (c >= 0); break;
		default: break;
		}
		return;
	}

	case VXO_AND: case VXO_OR: {
		int x = eval_bool(st, e, row, arena);
		if (x < 0) out->type = VX_NULL;
		else { out->type = VX_INT; out->i = x; }
		return;
	}

	case VXO_ISNULL: {
		vx_cell_t a; eval(st, e->a, row, arena, &a);
		out->type = VX_INT; out->i = (a.type == VX_NULL);
		return;
	}
	case VXO_NOTNULL: {
		vx_cell_t a; eval(st, e->a, row, arena, &a);
		out->type = VX_INT; out->i = (a.type != VX_NULL);
		return;
	}

	case VXO_IN:
	case VXO_NOTIN: {
		/* a IN (list).  SQL 3-valued: NULL operand -> NULL; a match -> 1;
		 * no match but a NULL element present -> NULL; else 0.  NOT IN is
		 * the logical negation (NULL stays NULL).  The compiler ensured
		 * operand and elements share an affinity class, so cmp_vals
		 * applies. */
		vx_cell_t a; int k, saw_null = 0, res;
		eval(st, e->a, row, arena, &a);
		if (a.type == VX_NULL) { out->type = VX_NULL; return; }
		res = 0;
		for (k = 0; k < e->nargs; k++) {
			vx_cell_t v; eval(st, e->args[k], row, arena, &v);
			if (v.type == VX_NULL) { saw_null = 1; continue; }
			if (cmp_vals(&a, &v) == 0) { res = 1; break; }
		}
		if (res == 0 && saw_null) { out->type = VX_NULL; return; }
		out->type = VX_INT;
		out->i = (e->op == VXO_NOTIN) ? !res : res;
		return;
	}

	case VXO_CASE: {
		/* Evaluate each WHEN (a boolean predicate -- for the simple form
		 * the compiler turned base=val into VXO_EQ) and return its THEN on
		 * the first that is TRUE.  A WHEN that is NULL or false is skipped
		 * (SQL CASE semantics).  No arm matched -> the ELSE, or NULL. */
		int k;
		for (k = 0; k + 1 < e->nargs; k += 2) {
			if (eval_bool(st, e->args[k], row, arena) == 1) {
				eval(st, e->args[k + 1], row, arena, out);
				return;
			}
		}
		if (e->b != NULL) { eval(st, e->b, row, arena, out); return; }
		out->type = VX_NULL;
		return;
	}

	case VXO_FUNC: {
		vx_cell_t a;
		switch (e->func) {
		case VXF_ABS:
			eval(st, e->args[0], row, arena, &a);
			if (a.type == VX_NULL) { out->type = VX_NULL; return; }
			if (a.type == VX_INT) { out->type = VX_INT; out->i = a.i < 0 ? -a.i : a.i; }
			else { out->type = VX_REAL; out->r = a.r < 0 ? -a.r : a.r; }
			return;
		case VXF_LENGTH:
			eval(st, e->args[0], row, arena, &a);
			if (a.type == VX_NULL) { out->type = VX_NULL; return; }
			out->type = VX_INT;
			if (a.type == VX_TEXT) {
				/* SQLite length() of text = number of characters; for
				 * pure ASCII this equals byte length.  Be conservative:
				 * count UTF-8 lead bytes. */
				uint32_t k, n = 0;
				for (k = 0; k < a.nbytes; k++)
					if ((a.bytes[k] & 0xc0) != 0x80) n++;
				out->i = n;
			} else if (a.type == VX_BLOB) {
				out->i = a.nbytes;
			} else {
				/* numeric: SQLite length(123) = length('123'); render. */
				char tmp[64]; int m;
				if (a.type == VX_INT) m = snprintf(tmp, sizeof tmp, "%lld", (long long)a.i);
				else m = snprintf(tmp, sizeof tmp, "%g", a.r);
				out->i = m;
			}
			return;
		case VXF_LOWER: case VXF_UPPER: {
			uint8_t *buf; uint32_t k;
			eval(st, e->args[0], row, arena, &a);
			if (a.type != VX_TEXT) {
				/* lower/upper of non-text returns the value unchanged in
				 * SQLite only for NULL; numerics get text-rendered.  The
				 * compiler keeps the arg text-classed, but a numeric-typed
				 * stored value could appear; pass NULL through, else copy. */
				if (a.type == VX_NULL) { out->type = VX_NULL; return; }
				*out = a; return;
			}
			buf = (uint8_t *)arena_alloc(arena, (size_t)a.nbytes + 1);
			if (buf == NULL) { out->type = VX_NULL; return; }
			for (k = 0; k < a.nbytes; k++) {
				uint8_t ch = a.bytes[k];
				if (e->func == VXF_LOWER) { if (ch >= 'A' && ch <= 'Z') ch += 32; }
				else { if (ch >= 'a' && ch <= 'z') ch -= 32; }
				buf[k] = ch;
			}
			buf[a.nbytes] = '\0';
			out->type = VX_TEXT; out->bytes = buf; out->nbytes = a.nbytes;
			return;
		}
		case VXF_COALESCE: case VXF_IFNULL: {
			int k;
			for (k = 0; k < e->nargs; k++) {
				eval(st, e->args[k], row, arena, out);
				if (out->type != VX_NULL) return;
			}
			out->type = VX_NULL;
			return;
		}
		}
		out->type = VX_NULL;
		return;
	}

	default:
		out->type = VX_NULL;
		return;
	}
}

/* ---- aggregation engine (V3) ------------------------------------- */

/* Cell hash + equality for GROUP BY keys.  Two NULLs group together;
 * INT and REAL with the same numeric value group together (SQLite
 * groups 1 and 1.0); TEXT/BLOB compare by bytes. */
static uint64_t
cell_hash(const vx_cell_t *c)
{
	uint64_t h = 1469598103934665603ULL;   /* FNV-1a */
	const uint8_t *p; uint32_t n, i;
	double d;
	switch (c->type) {
	case VX_NULL: return 0x9e3779b97f4a7c15ULL;
	case VX_INT:  d = (double)c->i; goto num;
	case VX_REAL: d = c->r; goto num;
	default:
		p = c->bytes; n = c->nbytes;
		for (i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211ULL; }
		return h ^ (c->type == VX_TEXT ? 0x7777ULL : 0x4242ULL);
	}
num:
	{
		uint64_t bits; memcpy(&bits, &d, 8);
		return bits * 0x9e3779b97f4a7c15ULL + 0x1234567ULL;
	}
}

static int
key_eq(const vx_cell_t *a, const vx_cell_t *b)
{
	if (a->type == VX_NULL || b->type == VX_NULL)
		return a->type == VX_NULL && b->type == VX_NULL;
	if ((a->type == VX_INT || a->type == VX_REAL) &&
	    (b->type == VX_INT || b->type == VX_REAL)) {
		double x = (a->type == VX_INT) ? (double)a->i : a->r;
		double y = (b->type == VX_INT) ? (double)b->i : b->r;
		return x == y;
	}
	if (a->type != b->type) return 0;
	return a->nbytes == b->nbytes &&
	       (a->nbytes == 0 || memcmp(a->bytes, b->bytes, a->nbytes) == 0);
}

static int
htab_init(vx_htab_t *h, int ngrp_key, int nagg)
{
	memset(h, 0, sizeof *h);
	h->nbucket = 1024;
	h->ngrp_key = ngrp_key;
	h->nagg = nagg;
	h->buckets = (vx_grp_t **)calloc((size_t)h->nbucket, sizeof(vx_grp_t *));
	return h->buckets ? 0 : -1;
}

static void
htab_free(vx_htab_t *h)
{
	if (h == NULL) return;
	arena_free(h->arena);
	free(h->buckets);
	h->buckets = NULL; h->arena = NULL;
}

/* Copy a cell into the htab arena (so TEXT/BLOB bytes outlive the row). */
static int
cell_dup(struct vx_arena_blk **arena, const vx_cell_t *src, vx_cell_t *dst)
{
	*dst = *src;
	if ((src->type == VX_TEXT || src->type == VX_BLOB) && src->nbytes) {
		uint8_t *p = (uint8_t *)arena_alloc(arena, (size_t)src->nbytes + 1);
		if (p == NULL) return -1;
		memcpy(p, src->bytes, src->nbytes); p[src->nbytes] = '\0';
		dst->bytes = p;
	}
	return 0;
}

/* Find or create the group for key cells `keys` (ngrp_key of them). */
static vx_grp_t *
htab_group(vx_htab_t *h, const vx_cell_t *keys)
{
	uint64_t hv = 1469598103934665603ULL;
	int k;
	uint32_t b;
	vx_grp_t *g;
	for (k = 0; k < h->ngrp_key; k++)
		hv ^= cell_hash(&keys[k]) * (uint64_t)(k + 1) * 0x100000001b3ULL;
	b = (uint32_t)(hv % (uint64_t)h->nbucket);
	for (g = h->buckets[b]; g; g = g->next) {
		if (g->hash != hv) continue;
		for (k = 0; k < h->ngrp_key; k++)
			if (!key_eq(&g->keys[k], &keys[k])) break;
		if (k == h->ngrp_key) return g;
	}
	/* New group. */
	g = (vx_grp_t *)arena_alloc(&h->arena, sizeof *g);
	if (g == NULL) return NULL;
	g->hash = hv;
	g->keys = (vx_cell_t *)arena_alloc(&h->arena,
	    sizeof(vx_cell_t) * (size_t)(h->ngrp_key > 0 ? h->ngrp_key : 1));
	g->accs = (vx_acc_t *)arena_alloc(&h->arena,
	    sizeof(vx_acc_t) * (size_t)(h->nagg > 0 ? h->nagg : 1));
	if ((h->ngrp_key && g->keys == NULL) || (h->nagg && g->accs == NULL))
		return NULL;
	for (k = 0; k < h->ngrp_key; k++)
		if (cell_dup(&h->arena, &keys[k], &g->keys[k]) != 0) return NULL;
	for (k = 0; k < h->nagg; k++) memset(&g->accs[k], 0, sizeof(vx_acc_t));
	g->next = h->buckets[b];
	h->buckets[b] = g;
	h->ngroup++;
	return g;
}

/* Fold one input value into an accumulator. */
static void
acc_step(vx_acc_t *a, enum vx_agg_kind kind, const vx_cell_t *v,
         struct vx_arena_blk **arena)
{
	if (kind == VXA_COUNT_STAR) { a->cnt++; return; }
	if (v->type == VX_NULL) return;        /* all others ignore NULL inputs */
	a->cnt++;
	switch (kind) {
	case VXA_COUNT:
		break;
	case VXA_SUM: case VXA_TOTAL: case VXA_AVG:
		if (v->type == VX_REAL) {
			if (!a->is_real) { a->rsum = (double)a->isum; a->is_real = 1; }
			a->rsum += v->r;
		} else if (a->is_real) {
			a->rsum += (double)v->i;
		} else {
			a->isum += v->i;
		}
		a->seen = 1;
		break;
	case VXA_MIN: case VXA_MAX:
		if (!a->seen) {
			(void)cell_dup(arena, v, &a->ext); a->seen = 1;
		} else {
			int c = cmp_vals(v, &a->ext);
			if ((kind == VXA_MIN && c < 0) || (kind == VXA_MAX && c > 0))
				(void)cell_dup(arena, v, &a->ext);
		}
		break;
	default: break;
	}
}

/* Merge accumulator `s` (from another worker) into `d`. */
static void
acc_merge(vx_acc_t *d, const vx_acc_t *s, enum vx_agg_kind kind,
          struct vx_arena_blk **arena)
{
	if (kind == VXA_COUNT_STAR || kind == VXA_COUNT) { d->cnt += s->cnt; return; }
	if (!s->seen) return;
	d->cnt += s->cnt;
	switch (kind) {
	case VXA_SUM: case VXA_TOTAL: case VXA_AVG: {
		double sv = s->is_real ? s->rsum : (double)s->isum;
		if (s->is_real || d->is_real) {
			if (!d->is_real) { d->rsum = (double)d->isum; d->is_real = 1; }
			d->rsum += sv;
		} else {
			d->isum += s->isum;
		}
		d->seen = 1;
		break;
	}
	case VXA_MIN: case VXA_MAX:
		if (!d->seen) { (void)cell_dup(arena, &s->ext, &d->ext); d->seen = 1; }
		else {
			int c = cmp_vals(&s->ext, &d->ext);
			if ((kind == VXA_MIN && c < 0) || (kind == VXA_MAX && c > 0))
				(void)cell_dup(arena, &s->ext, &d->ext);
		}
		break;
	default: break;
	}
}

/* Final value of an accumulator. */
static void
acc_final(const vx_acc_t *a, enum vx_agg_kind kind, vx_cell_t *out)
{
	memset(out, 0, sizeof *out);
	switch (kind) {
	case VXA_COUNT_STAR: case VXA_COUNT:
		out->type = VX_INT; out->i = a->cnt; return;
	case VXA_TOTAL:
		out->type = VX_REAL;
		out->r = a->is_real ? a->rsum : (double)a->isum;
		return;
	case VXA_SUM:
		if (!a->seen) { out->type = VX_NULL; return; }   /* all-NULL -> NULL */
		if (a->is_real) { out->type = VX_REAL; out->r = a->rsum; }
		else { out->type = VX_INT; out->i = a->isum; }
		return;
	case VXA_AVG:
		if (a->cnt == 0) { out->type = VX_NULL; return; }
		out->type = VX_REAL;
		out->r = (a->is_real ? a->rsum : (double)a->isum) / (double)a->cnt;
		return;
	case VXA_MIN: case VXA_MAX:
		if (!a->seen) { out->type = VX_NULL; return; }
		*out = a->ext; return;
	default: out->type = VX_NULL; return;
	}
}

/* ---- recognizer + plan build ------------------------------------- */

static void
chunk_free(vx_chunk_t *c)
{
	if (!c) return;
	arena_free(c->arena);
	free(c->cells);
	free(c);
}

static int vx_try_prepare_agg(sqlite3 *db, sql_arena_t *ast,
                              const sql_select_t *sel, const char *tabbuf,
                              const vx_cell_t *binds, int nbinds,
                              vx_stmt_t **out, char **errmsg);
static int vx_try_prepare_join(sqlite3 *db, sql_arena_t *ast,
                               const sql_select_t *sel,
                               const vx_cell_t *binds, int nbinds,
                               vx_stmt_t **out, char **errmsg);
static int vx_try_prepare_binds(sqlite3 *db, const char *sql,
                                const vx_cell_t *binds, int nbinds,
                                vx_stmt_t **out, char **errmsg);
static int vx_prepare_select(sqlite3 *db, sql_arena_t *ast,
                             const sql_select_t *sel,
                             const vx_cell_t *binds, int nbinds,
                             vx_stmt_t **out, char **errmsg);

/* Parse a NUMBER expr (with an optional leading unary minus) as an
 * integer literal.  Returns 1 with *v set, or 0 (not an integer
 * literal). */
static int
ast_int_lit(const sql_expr_t *e, int64_t *v)
{
	int neg = 0;
	char buf[32]; int i;
	if (e == NULL) return 0;
	if (e->op == SX_E_UNARY && e->op2 == TK_MINUS) { neg = 1; e = e->a; }
	if (e == NULL || e->op != SX_E_NUMBER || e->lit.len == 0 ||
	    e->lit.len >= sizeof buf) return 0;
	for (i = 0; i < (int)e->lit.len; i++) {
		char c = e->lit.p[i];
		if (c == '.' || c == 'e' || c == 'E') return 0;   /* real, not int */
	}
	memcpy(buf, e->lit.p, e->lit.len); buf[e->lit.len] = '\0';
	*v = strtoll(buf, NULL, 10);
	if (neg) *v = -*v;
	return 1;
}

/* Does `e` name the column `pk` (unqualified or table-qualified)? */
static int
ast_is_named_col(const sql_expr_t *e, const char *pk)
{
	return e && e->op == SX_E_COLUMN && e->nname >= 1 &&
	    e->name[e->nname - 1].len == strlen(pk) &&
	    strncmp(e->name[e->nname - 1].p, pk, strlen(pk)) == 0;
}

/* Tighten a rowid range [lo, hi] (per has_lo / has_hi) with any
 * primary-key comparison found among the AND-conjuncts of `w`: pk
 * = / < / <= / > / >= an int literal, or pk BETWEEN.  Non-pk and
 * non-recognized conjuncts are ignored (the row filter still enforces
 * them), so this is a safe pushdown -- it only narrows the scan.  The
 * read planner uses it to seek instead of full-scanning. */
static void
where_rowid_bound(const sql_expr_t *w, const char *pk,
                  int64_t *lo, int *has_lo, int64_t *hi, int *has_hi)
{
	if (w == NULL) return;
	if (w->op == SX_E_BINARY && w->op2 == TK_AND) {
		where_rowid_bound(w->a, pk, lo, has_lo, hi, has_hi);
		where_rowid_bound(w->b, pk, lo, has_lo, hi, has_hi);
		return;
	}
	if (w->op == SX_E_BETWEEN && ast_is_named_col(w->a, pk)) {
		int64_t b, c;
		if (ast_int_lit(w->b, &b) && ast_int_lit(w->c, &c)) {
			if (!*has_lo || b > *lo) { *lo = b; *has_lo = 1; }
			if (!*has_hi || c < *hi) { *hi = c; *has_hi = 1; }
		}
		return;
	}
	if (w->op == SX_E_BINARY && w->a && w->b) {
		const sql_expr_t *lit; int op = w->op2, swapped = 0;
		int64_t v;
		if (ast_is_named_col(w->a, pk)) lit = w->b;
		else if (ast_is_named_col(w->b, pk)) { lit = w->a; swapped = 1; }
		else return;
		if (!ast_int_lit(lit, &v)) return;
		if (swapped) {
			switch (op) {
			case TK_LT: op = TK_GT; break;  case TK_GT: op = TK_LT; break;
			case TK_LE: op = TK_GE; break;  case TK_GE: op = TK_LE; break;
			default: break;
			}
		}
		switch (op) {
		case TK_EQ:
			if (!*has_lo || v > *lo) { *lo = v; *has_lo = 1; }
			if (!*has_hi || v < *hi) { *hi = v; *has_hi = 1; }
			break;
		case TK_GT:
			if (!*has_lo || v + 1 > *lo) { *lo = v + 1; *has_lo = 1; }
			break;
		case TK_GE:
			if (!*has_lo || v > *lo) { *lo = v; *has_lo = 1; }
			break;
		case TK_LT:
			if (!*has_hi || v - 1 < *hi) { *hi = v - 1; *has_hi = 1; }
			break;
		case TK_LE:
			if (!*has_hi || v < *hi) { *hi = v; *has_hi = 1; }
			break;
		default: break;
		}
	}
}

int
vx_try_prepare(sqlite3 *db, const char *sql, vx_stmt_t **out, char **errmsg)
{
	return vx_try_prepare_binds(db, sql, NULL, 0, out, errmsg);
}

static int
vx_try_prepare_binds(sqlite3 *db, const char *sql,
                     const vx_cell_t *binds, int nbinds,
                     vx_stmt_t **out, char **errmsg)
{
	sql_arena_t *ast = NULL;
	sql_stmt_t  *root = NULL;
	const char  *perr = NULL;

	if (out) *out = NULL;
	if (errmsg) *errmsg = NULL;

	if (sql_parse_ast(sql, strlen(sql), &ast, &root, &perr) != 0)
		return 0;
	if (root == NULL || root->next != NULL ||
	    root->kind != SQL_KIND_SELECT || root->explain || root->u.select == NULL) {
		sql_arena_destroy(ast);
		return 0;
	}
	/* A compound (set-op) select is not a single plan; vx_run_p handles
	 * it.  Here, only a non-compound SELECT becomes one vx_stmt. */
	if (root->u.select->setop != SX_SET_NONE) { sql_arena_destroy(ast); return 0; }
	return vx_prepare_select(db, ast, root->u.select, binds, nbinds, out, errmsg);
}

/* Recognize ONE (non-compound) SELECT given its already-parsed AST and
 * arena (which it takes ownership of -- freed on fallback, kept on the
 * returned plan).  This is the post-parse body shared by the string
 * entry point and the set-op runner. */
static int
vx_prepare_select(sqlite3 *db, sql_arena_t *ast, const sql_select_t *sel,
                  const vx_cell_t *binds, int nbinds,
                  vx_stmt_t **out, char **errmsg)
{
	const sql_src_t *src;
	struct namevec nv;
	struct vx_compiler comp;
	struct vx_stmt *st = NULL;
	const sql_exprlist_item_t *it;
	char tabbuf[64];
	char *srcsql = NULL;
	int nproj = 0, rc = 0, proj_star = 0;

	if (out) *out = NULL;
	if (errmsg) *errmsg = NULL;
	if (sel == NULL) goto fallback;

	/* Common gates: no CTE/DISTINCT/HAVING.  GROUP BY is
	 * allowed only on the aggregation path; ORDER BY / LIMIT / OFFSET
	 * are allowed only on the non-aggregating path (handled below).
	 * HAVING is allowed only on the aggregation path (gated there). */
	if (sel->with)
		goto fallback;
	if (sel->having && sel->group == NULL) {
		/* HAVING without GROUP BY -- only valid with an aggregate select
		 * list, which the agg path detects below; a HAVING here that is
		 * not aggregated would be handled there too.  Defer the decision
		 * to the path split. */
	}

	/* FROM: one base table, or exactly two base tables joined (V5). */
	src = sel->from;
	if (src == NULL) goto fallback;
	if (src->next != NULL) {
		/* Two-table INNER equi-join (no GROUP BY / ORDER BY / aggregates
		 * on the join path yet -- those compose later). */
		if (src->next->next != NULL) goto fallback;       /* >2 tables */
		if (sel->group || sel->order || sel->limit || sel->offset || sel->distinct)
			goto fallback;
		return vx_try_prepare_join(db, ast, sel, binds, nbinds, out, errmsg);
	}
	if (src->subquery != NULL) goto fallback;
	if (src->table.len == 0 || src->table.len >= sizeof tabbuf) goto fallback;
	memcpy(tabbuf, src->table.p, src->table.len);
	tabbuf[src->table.len] = '\0';

	/* Aggregation (P3): a GROUP BY, or any select item that is an
	 * aggregate call.  Routed to the dedicated builder. */
	{
		int has_agg = (sel->group != NULL);
		for (it = sel->cols ? sel->cols->head : NULL; it && !has_agg; it = it->next) {
			const sql_expr_t *e = it->expr;
			if (e && e->op == SX_E_FUNC && is_agg_name(&e->name[0]))
				has_agg = 1;
		}
		if (has_agg) {
			if (sel->distinct) goto fallback;   /* DISTINCT over aggregates: VDBE */
			/* Hand off the AST (still alive) to the agg builder, which
			 * destroys it before returning. */
			return vx_try_prepare_agg(db, ast, sel, tabbuf, binds, nbinds, out, errmsg);
		}
	}
	if (sel->group) goto fallback;   /* defensive: GROUP only on agg path */
	if (sel->having) goto fallback;  /* HAVING only with aggregation */

	/* Projection: either a single "*" (expand to all table columns) or a
	 * list of scalar expressions.  Detect "*" up front. */
	{
		const sql_exprlist_item_t *p0 = sel->cols ? sel->cols->head : NULL;
		if (p0 && p0->expr && p0->expr->op == SX_E_STAR) {
			if (sel->cols->n != 1) goto fallback;   /* only a bare "*" */
			proj_star = 1;
		}
	}
	if (!proj_star) {
		for (it = sel->cols ? sel->cols->head : NULL; it; it = it->next) {
			if (it->expr == NULL || it->expr->op == SX_E_STAR) goto fallback;
			nproj++;
		}
		if (nproj == 0) goto fallback;
	}

	st = (struct vx_stmt *)calloc(1, sizeof *st);
	if (!st) goto oom;
	st->db = db;
	st->cur = -1;
	st->binds = binds; st->nbinds = nbinds;
	st->distinct = sel->distinct;

	memset(&nv, 0, sizeof nv);
	comp.st = st; comp.nv = &nv; comp.jc = NULL; comp.fail = 0;
	comp.binds = binds; comp.nbinds = nbinds;

	/* Pass 1: collect referenced base columns and reject unsupported
	 * constructs, WITHOUT the affinity gate (affinities are unknown
	 * until the source is prepared).  For "*" the source is the whole
	 * table, so only the WHERE columns need collecting here -- but the
	 * WHERE column names must resolve against the source columns, which
	 * we do by name after preparing "SELECT *". */
	if (!proj_star) {
		for (it = sel->cols->head; it; it = it->next)
			if (collect_columns(&nv, it->expr) != 0) goto fallback;
		if (sel->where && collect_columns(&nv, sel->where) != 0) goto fallback;
		/* ORDER BY key columns must be in the source too.  A bare integer
		 * key is an output-column position (collected via the output);
		 * any other key expression is collected here. */
		if (sel->order) {
			const sql_exprlist_item_t *o;
			for (o = sel->order->head; o; o = o->next) {
				if (o->expr && o->expr->op == SX_E_NUMBER) continue;
				if (collect_columns(&nv, o->expr) != 0) goto fallback;
			}
		}
	} else {
		/* SELECT *: expand to every table column (in declared order)
		 * from the native catalog, so the source carries all of them and
		 * the projection is the identity.  Needs the native schema (an
		 * xstore table); a non-xstore SELECT * falls back. */
		bt_t *sbt = xstore_bt_of(db);
		xstore_col_t scols[64];
		int snc = sbt ? xstore_table_schema(sbt, tabbuf, scols, 64) : 0, sc;
		if (snc <= 0) goto fallback;
		for (sc = 0; sc < snc; sc++) {
			sql_str_t s; s.p = scols[sc].name; s.len = (uint32_t)strlen(scols[sc].name);
			if (nv_add(&nv, &s) < 0) goto fallback;
		}
		if (sel->where && collect_columns(&nv, sel->where) != 0) goto fallback;
		nproj = snc;
	}

	/* Storage-native source.  SELECT * is expanded above into the column
	 * list; explicit-column queries scan the B-tree directly. */
	if (nv.n == 0 || nv.n > 32) goto fallback;

	st->bt = xstore_bt_of(db);
	if (st->bt == NULL) goto fallback;   /* not an xstore-backed connection */
	snprintf(st->table, sizeof st->table, "%s", tabbuf);
	st->nsrc_col = nv.n;
	st->snap = 0;   /* latest committed (vexec is the read fast path) */

	/* Resolve each source column to its record origin + affinity from
	 * the schema (prepare-time only; no data row stepped). */
	if (resolve_schema(db, tabbuf, &nv, st->src_pay) != 0)
		goto fallback;

	/* Rowid-range pushdown (the minimal planner): if the WHERE pins the
	 * primary key (the source column whose origin is the rowid) to a
	 * range, bound the scan to it so a point/range read seeks instead of
	 * full-scanning.  The WHERE filter still runs unchanged. */
	{
		int pk = -1, i;
		for (i = 0; i < nv.n; i++) if (st->src_pay[i] == -1) { pk = i; break; }
		if (pk >= 0 && sel->where)
			where_rowid_bound(sel->where, nv.names[pk],
			                  &st->scan_lo, &st->scan_has_lo,
			                  &st->scan_hi, &st->scan_has_hi);
	}

	st->nout = nproj;
	if (nproj > 32) goto fallback;   /* worker out[] / result row bound */
	st->proj = (vx_expr_t **)calloc((size_t)(nproj > 0 ? nproj : 1),
	                                sizeof(vx_expr_t *));
	if (!st->proj) goto oom;

	/* Pass 2: compile the projection + filter with the affinity gate
	 * active.  Any affinity-ambiguous comparison/arithmetic fails here
	 * and the query falls back. */
	{
		int k = 0;
		comp.fail = 0;
		if (proj_star) {
			/* Identity projection over every source column, named from
			 * the schema (the nv column order is the declared order). */
			for (k = 0; k < nproj; k++) {
				vx_expr_t *n = expr_node(&comp, VXO_COL);
				if (n == NULL) goto oom;
				n->col = k;
				st->proj[k] = n;
				snprintf(st->outname[k], sizeof st->outname[0], "%.*s",
				         (int)(sizeof st->outname[0] - 1), nv.names[k]);
			}
		} else {
			for (it = sel->cols->head; it; it = it->next, k++) {
				st->proj[k] = compile_expr(&comp, it->expr);
				if (comp.fail) goto fallback;
				item_name(it, st->outname[k], sizeof st->outname[0]);
			}
		}
		if (sel->where) {
			st->filter = compile_expr(&comp, sel->where);
			if (comp.fail) goto fallback;
		} else {
			st->filter = NULL;
		}
	}

	/* ORDER BY / LIMIT / OFFSET (V4).  Order keys compile over the
	 * source columns (reusing the compiler + nv); a bare integer order
	 * key is treated as a 1-based output-column reference (ORDER BY 2).
	 * LIMIT/OFFSET must be non-negative integer literals -- expressions
	 * or parameters fall back. */
	st->limit = -1;
	st->offset = 0;
	if (sel->order) {
		const sql_exprlist_item_t *o;
		int no = 0, oi = 0;
		for (o = sel->order->head; o; o = o->next) no++;
		if (no == 0 || no > 16) goto fallback;
		st->norder = no;
		st->order_key = (vx_expr_t **)calloc((size_t)no, sizeof(vx_expr_t *));
		st->order_outcol = (int *)calloc((size_t)no, sizeof(int));
		st->order_desc = (int *)calloc((size_t)no, sizeof(int));
		if (!st->order_key || !st->order_outcol || !st->order_desc) goto oom;
		for (o = sel->order->head; o; o = o->next, oi++) {
			const sql_expr_t *e = o->expr;
			st->order_desc[oi] = (o->sort == 2);
			if (e && e->op == SX_E_NUMBER) {
				/* ORDER BY <int>: output-column position (1-based). */
				char buf[32]; long pos;
				if (e->lit.len == 0 || e->lit.len >= sizeof buf) goto fallback;
				memcpy(buf, e->lit.p, e->lit.len); buf[e->lit.len] = '\0';
				if (strchr(buf, '.') || strchr(buf, 'e') || strchr(buf, 'E'))
					goto fallback;
				pos = strtol(buf, NULL, 10);
				if (pos < 1 || pos > st->nout) goto fallback;
				st->order_outcol[oi] = (int)pos;
			} else {
				comp.fail = 0;
				st->order_key[oi] = compile_expr(&comp, e);
				if (comp.fail) goto fallback;
			}
		}
	}
	if (sel->limit) {
		if (sel->limit->op != SX_E_NUMBER) goto fallback;
		{
			char buf[32];
			if (sel->limit->lit.len >= sizeof buf) goto fallback;
			memcpy(buf, sel->limit->lit.p, sel->limit->lit.len);
			buf[sel->limit->lit.len] = '\0';
			if (strchr(buf, '.') || strchr(buf, 'e') || strchr(buf, 'E'))
				goto fallback;
			st->limit = strtoll(buf, NULL, 10);
			if (st->limit < 0) st->limit = 0;
		}
	}
	if (sel->offset) {
		if (sel->offset->op != SX_E_NUMBER) goto fallback;
		{
			char buf[32];
			if (sel->offset->lit.len >= sizeof buf) goto fallback;
			memcpy(buf, sel->offset->lit.p, sel->offset->lit.len);
			buf[sel->offset->lit.len] = '\0';
			if (strchr(buf, '.') || strchr(buf, 'e') || strchr(buf, 'E'))
				goto fallback;
			st->offset = strtoll(buf, NULL, 10);
			if (st->offset < 0) st->offset = 0;
		}
	}

	/* DISTINCT + LIMIT/OFFSET is handled (dedup into a kept list, then
	 * OFFSET/LIMIT over the DISTINCT set) -- but only with an ORDER BY,
	 * so which DISTINCT rows survive the limit is deterministic and
	 * matches the VDBE.  DISTINCT + LIMIT without ORDER BY picks an
	 * unspecified subset (valid in SQL, but not byte-reproducible), so
	 * leave that to the VDBE. */
	if (st->distinct && (st->limit >= 0 || st->offset > 0) && st->norder == 0)
		goto fallback;

	st->srcrow = (vx_cell_t *)calloc((size_t)(st->nsrc_col > 0 ? st->nsrc_col : 1),
	                                 sizeof(vx_cell_t));
	if (!st->srcrow) goto oom;

	free(srcsql);
	sql_arena_destroy(ast);
	*out = st;
	return 1;

oom:
	rc = -1;
fallback:
	if (srcsql) free(srcsql);
	if (ast) sql_arena_destroy(ast);
	if (st) vx_finalize(st);
	return rc;
}

/* ---- aggregation plan builder (P3) ------------------------------- *
 *
 * Validates and compiles SELECT [keys,] agg(expr)... FROM t [WHERE ...]
 * [GROUP BY keys].  Conservative: each select item is either a GROUP BY
 * key expression (which must appear in GROUP BY, compared structurally
 * to a key) or a single non-DISTINCT aggregate over a scalar argument;
 * no expressions over aggregates, no HAVING (gated earlier).  Anything
 * else falls back. */

/* Map an aggregate function name + arg shape to a kind, or 0. */
static enum vx_agg_kind
agg_kind_of(const sql_expr_t *e)
{
	int star = (e->ival & 2) != 0;
	int nargs = 0;
	const sql_exprlist_item_t *it;
	for (it = e->list ? e->list->head : NULL; it; it = it->next) nargs++;
	if (e->ival & 1) return 0;                 /* DISTINCT aggregate: fallback */
	if (name_is(&e->name[0], "count")) {
		if (star) return VXA_COUNT_STAR;
		return nargs == 1 ? VXA_COUNT : 0;
	}
	if (star) return 0;                        /* sum(*) etc. invalid */
	if (nargs != 1) return 0;
	if (name_is(&e->name[0], "sum"))   return VXA_SUM;
	if (name_is(&e->name[0], "total")) return VXA_TOTAL;
	if (name_is(&e->name[0], "avg"))   return VXA_AVG;
	if (name_is(&e->name[0], "min"))   return VXA_MIN;
	if (name_is(&e->name[0], "max"))   return VXA_MAX;
	return 0;
}

/* Structural equality of two AST scalar expressions (so a select-list
 * key can be matched to a GROUP BY key).  Conservative: only the shapes
 * the key compiler supports. */
static int
expr_same(const sql_expr_t *a, const sql_expr_t *b)
{
	if (a == NULL || b == NULL) return a == b;
	if (a->op != b->op) return 0;
	switch (a->op) {
	case SX_E_COLUMN:
		if (a->nname != b->nname) return 0;
		{ int k; for (k = 0; k < a->nname; k++)
			if (a->name[k].len != b->name[k].len ||
			    memcmp(a->name[k].p, b->name[k].p, a->name[k].len) != 0)
				return 0;
		  return 1; }
	case SX_E_NUMBER: case SX_E_STRING:
		return a->lit.len == b->lit.len &&
		       memcmp(a->lit.p, b->lit.p, a->lit.len) == 0;
	case SX_E_UNARY:
		return a->op2 == b->op2 && expr_same(a->a, b->a);
	case SX_E_BINARY:
		return a->op2 == b->op2 && expr_same(a->a, b->a) && expr_same(a->b, b->b);
	case SX_E_FUNC: {
		/* same function name, same flags (DISTINCT/star), same args. */
		const sql_exprlist_item_t *ia, *ib;
		if (a->ival != b->ival) return 0;
		if (a->name[0].len != b->name[0].len ||
		    memcmp(a->name[0].p, b->name[0].p, a->name[0].len) != 0) return 0;
		ia = a->list ? a->list->head : NULL;
		ib = b->list ? b->list->head : NULL;
		for (; ia && ib; ia = ia->next, ib = ib->next)
			if (!expr_same(ia->expr, ib->expr)) return 0;
		return ia == NULL && ib == NULL;
	}
	default: return 0;
	}
}

/* Compile a HAVING predicate into an expression over the aggregation
 * OUTPUT row: any sub-expression that matches a SELECT item (an
 * aggregate call or a group-key reference, by expr_same) becomes a
 * VXO_COL referencing that output column; comparisons and AND/OR/NOT
 * recurse; literals/params compile as usual.  Returns NULL with
 * *c->fail set when the HAVING references something not in the SELECT
 * list (e.g. an aggregate that is not output) -- the caller falls back.
 * This covers the common HAVING (a condition on an output aggregate or
 * key) without adding extra accumulators. */
/* Pass 1 for HAVING: walk the HAVING AST and, for each aggregate call
 * that is NOT already a SELECT output item, append a HAVING-only
 * aggregate outcol to ap->out (past ap->nout, growing ap->nout_all and
 * counting it in *nagg) and collect its argument columns into nv.
 * Returns 0, or -1 (unsupported aggregate / too many / column-collect
 * failure -> caller falls back). */
static int
having_collect_aggs(const sql_select_t *sel, const sql_expr_t *e,
                    vx_aggplan_t *ap, struct namevec *nv, int *nagg)
{
	const sql_exprlist_item_t *it;
	if (e == NULL) return 0;
	if (e->op == SX_E_FUNC && is_agg_name(&e->name[0])) {
		enum vx_agg_kind kk;
		int matched = 0, oi = 0;
		/* Already a SELECT output aggregate?  Then it reuses that accs. */
		for (it = sel->cols ? sel->cols->head : NULL; it; it = it->next, oi++)
			if (expr_same(e, it->expr)) { matched = 1; break; }
		if (matched) return 0;
		kk = agg_kind_of(e);
		if (kk == 0) return -1;
		if (ap->nout_all - ap->nout >= VX_HAVING_AGG_MAX) return -1;
		ap->out[ap->nout_all].is_agg = 1;
		ap->out[ap->nout_all].kind = kk;
		ap->out[ap->nout_all].ast = e;
		if (kk != VXA_COUNT_STAR) {
			if (e->list == NULL || e->list->head == NULL) return -1;
			if (collect_columns(nv, e->list->head->expr) != 0) return -1;
		}
		ap->nout_all++;
		(*nagg)++;
		return 0;
	}
	/* Recurse into the predicate structure. */
	if (having_collect_aggs(sel, e->a, ap, nv, nagg) != 0) return -1;
	if (having_collect_aggs(sel, e->b, ap, nv, nagg) != 0) return -1;
	if (having_collect_aggs(sel, e->c, ap, nv, nagg) != 0) return -1;
	for (it = e->list ? e->list->head : NULL; it; it = it->next)
		if (having_collect_aggs(sel, it->expr, ap, nv, nagg) != 0) return -1;
	return 0;
}

static vx_expr_t *
compile_having(struct vx_compiler *c, const sql_select_t *sel,
               const sql_expr_t *e)
{
	const sql_exprlist_item_t *it;
	int oi;
	enum vx_op op;
	if (c->fail || e == NULL) { c->fail = 1; return NULL; }

	/* First, does this whole sub-expression match a SELECT output item?
	 * (Matches an aggregate call like count(*) or a group key.) */
	oi = 0;
	for (it = sel->cols ? sel->cols->head : NULL; it; it = it->next, oi++) {
		if (expr_same(e, it->expr)) {
			vx_expr_t *n = expr_node(c, VXO_COL);
			if (n == NULL) return NULL;
			n->col = oi;   /* index into the extended output row */
			return n;
		}
	}
	/* Or a HAVING-only aggregate appended past nout (its value lands in
	 * the extended row at its outcol index). */
	if (c->st->agg != NULL) {
		vx_aggplan_t *ap = c->st->agg;
		int i;
		for (i = ap->nout; i < ap->nout_all; i++)
			if (ap->out[i].ast != NULL && expr_same(e, ap->out[i].ast)) {
				vx_expr_t *n = expr_node(c, VXO_COL);
				if (n == NULL) return NULL;
				n->col = i;
				return n;
			}
	}

	switch (e->op) {
	case SX_E_NULL: case SX_E_NUMBER: case SX_E_STRING:
		return compile_literal(c, e);
	case SX_E_PARAM:
		return compile_param(c, e);
	case SX_E_UNARY:
		if (e->op2 == TK_NOT) {
			vx_expr_t *n = expr_node(c, VXO_NOT);
			vx_expr_t *a = compile_having(c, sel, e->a);
			if (a == NULL) return NULL;
			if (n) n->a = a;
			return n;
		}
		c->fail = 1; return NULL;
	case SX_E_BINARY: {
		vx_expr_t *n, *a, *b;
		if (!tok_to_binop(e->op2, &op)) { c->fail = 1; return NULL; }
		a = compile_having(c, sel, e->a);
		if (a == NULL) return NULL;
		b = compile_having(c, sel, e->b);
		if (b == NULL) return NULL;
		n = expr_node(c, op);
		if (n == NULL) return NULL;
		n->a = a; n->b = b;
		return n;
	}
	case SX_E_IS_NULL: {
		vx_expr_t *n = expr_node(c, e->ival ? VXO_NOTNULL : VXO_ISNULL);
		vx_expr_t *a = compile_having(c, sel, e->a);
		if (a == NULL) return NULL;
		if (n) n->a = a;
		return n;
	}
	default:
		c->fail = 1; return NULL;
	}
}

static int
vx_try_prepare_agg(sqlite3 *db, sql_arena_t *ast, const sql_select_t *sel,
                   const char *tabbuf, const vx_cell_t *binds, int nbinds,
                   vx_stmt_t **out, char **errmsg)
{
	struct vx_stmt *st = NULL;
	struct namevec nv;
	struct vx_compiler comp;
	vx_aggplan_t *ap = NULL;
	const sql_exprlist_item_t *it;
	char *srcsql = NULL;
	int nout = 0, ngrp = 0, nagg = 0, rc = 0;

	if (errmsg) *errmsg = NULL;

	/* V4 ORDER BY / LIMIT over aggregated output is not supported yet;
	 * fall back so the VDBE handles it. */
	if (sel->order || sel->limit || sel->offset) goto fallback;

	/* Count output columns and GROUP BY keys. */
	for (it = sel->cols ? sel->cols->head : NULL; it; it = it->next) {
		if (it->expr == NULL || it->expr->op == SX_E_STAR) {
			/* count(*) is SX_E_FUNC with ival bit1, not SX_E_STAR; a bare
			 * '*' in an aggregate query is not supported. */
			goto fallback;
		}
		nout++;
	}
	if (nout == 0 || nout > 32) goto fallback;
	for (it = sel->group ? sel->group->head : NULL; it; it = it->next) ngrp++;
	if (ngrp > 16) goto fallback;

	st = (struct vx_stmt *)calloc(1, sizeof *st);
	if (!st) goto oom;
	st->db = db;
	st->cur = -1;
	st->limit = -1;   /* agg path has no LIMIT/ORDER BY (gated above) */
	st->binds = binds; st->nbinds = nbinds;
	ap = (vx_aggplan_t *)calloc(1, sizeof *ap);
	if (!ap) goto oom;
	ap->nout = nout;
	ap->nout_all = nout;
	ap->ngrp = ngrp;
	/* Over-allocate out[] to leave room for up to VX_HAVING_AGG_MAX
	 * HAVING-only aggregates appended past nout. */
	ap->out = (vx_outcol_t *)calloc((size_t)(nout + VX_HAVING_AGG_MAX),
	                                sizeof(vx_outcol_t));
	ap->grp = (vx_expr_t **)calloc((size_t)(ngrp > 0 ? ngrp : 1), sizeof(vx_expr_t *));
	if (!ap->out || !ap->grp) goto oom;
	st->agg = ap;

	memset(&nv, 0, sizeof nv);
	comp.st = st; comp.nv = &nv; comp.jc = NULL; comp.fail = 0;
	comp.binds = binds; comp.nbinds = nbinds;

	/* Pass 1: collect referenced base columns (group keys, agg args,
	 * WHERE), and classify each select item as key or aggregate. */
	for (it = sel->group ? sel->group->head : NULL; it; it = it->next)
		if (collect_columns(&nv, it->expr) != 0) goto fallback;
	{
		int oi = 0;
		for (it = sel->cols->head; it; it = it->next, oi++) {
			const sql_expr_t *e = it->expr;
			item_name(it, st->outname[oi], sizeof st->outname[0]);
			if (e->op == SX_E_FUNC && is_agg_name(&e->name[0])) {
				enum vx_agg_kind kk = agg_kind_of(e);
				if (kk == 0) goto fallback;
				ap->out[oi].is_agg = 1;
				ap->out[oi].kind = kk;
				if (kk != VXA_COUNT_STAR) {
					const sql_expr_t *arg = e->list->head->expr;
					if (collect_columns(&nv, arg) != 0) goto fallback;
				}
				nagg++;
			} else {
				/* A grouping-key output: must match a GROUP BY key. */
				const sql_exprlist_item_t *g;
				int matched = 0;
				for (g = sel->group ? sel->group->head : NULL; g; g = g->next)
					if (expr_same(e, g->expr)) { matched = 1; break; }
				if (!matched) goto fallback;   /* bare column not in GROUP BY */
				ap->out[oi].is_agg = 0;
				if (collect_columns(&nv, e) != 0) goto fallback;
			}
		}
	}
	/* HAVING-only aggregates (not in SELECT) get appended outcols +
	 * accumulators, and their args collected, in this same pass. */
	if (sel->having &&
	    having_collect_aggs(sel, sel->having, ap, &nv, &nagg) != 0)
		goto fallback;
	ap->nagg = nagg;
	if (sel->where && collect_columns(&nv, sel->where) != 0) goto fallback;

	/* Build the source SELECT over the collected base columns.  A pure
	 * count(*) references no columns; select _rowid_ so there is a row
	 * to count (and so the source has at least one column). */
	/* Storage-native source.  A pure count(*) references no columns
	 * (nv.n == 0) -- the scan still yields every rowid to count, no
	 * record decode needed. */
	if (nv.n > 32) goto fallback;
	st->bt = xstore_bt_of(db);
	if (st->bt == NULL) goto fallback;
	snprintf(st->table, sizeof st->table, "%s", tabbuf);
	st->nsrc_col = nv.n;
	st->snap = 0;
	if (nv.n > 0 && resolve_schema(db, tabbuf, &nv, st->src_pay) != 0)
		goto fallback;

	/* Pass 2: compile group-key expressions, aggregate-arg expressions,
	 * and the filter, with the affinity gate active. */
	comp.fail = 0;
	{
		int gi = 0;
		for (it = sel->group ? sel->group->head : NULL; it; it = it->next, gi++) {
			ap->grp[gi] = compile_expr(&comp, it->expr);
			if (comp.fail) goto fallback;
		}
	}
	{
		int oi = 0;
		for (it = sel->cols->head; it; it = it->next, oi++) {
			if (ap->out[oi].is_agg) {
				if (ap->out[oi].kind != VXA_COUNT_STAR) {
					ap->out[oi].arg = compile_expr(&comp, it->expr->list->head->expr);
					if (comp.fail) goto fallback;
				}
			} else {
				ap->out[oi].key = compile_expr(&comp, it->expr);
				if (comp.fail) goto fallback;
			}
		}
		/* Compile the appended HAVING-only aggregate args. */
		for (oi = ap->nout; oi < ap->nout_all; oi++) {
			if (ap->out[oi].kind != VXA_COUNT_STAR && ap->out[oi].ast != NULL) {
				const sql_expr_t *ag = ap->out[oi].ast;
				if (ag->list == NULL || ag->list->head == NULL) goto fallback;
				ap->out[oi].arg = compile_expr(&comp, ag->list->head->expr);
				if (comp.fail) goto fallback;
			}
		}
	}
	if (sel->where) {
		st->filter = compile_expr(&comp, sel->where);
		if (comp.fail) goto fallback;
	}
	if (sel->having) {
		ap->having = compile_having(&comp, sel, sel->having);
		if (comp.fail || ap->having == NULL) goto fallback;
	}

	st->nout = nout;
	st->srcrow = (vx_cell_t *)calloc((size_t)(st->nsrc_col > 0 ? st->nsrc_col : 1),
	                                 sizeof(vx_cell_t));
	if (!st->srcrow) goto oom;

	free(srcsql);
	sql_arena_destroy(ast);
	*out = st;
	return 1;

oom:
	rc = -1;
fallback:
	if (srcsql) free(srcsql);
	if (ast) sql_arena_destroy(ast);
	if (st) { free(ap ? ap->out : NULL); free(ap ? ap->grp : NULL); free(ap); st->agg = NULL; vx_finalize(st); }
	else free(ap);
	return rc;
}

/* ---- join (V5): builder + hash table + execution ----------------- */

/* A build-side row stored in the join hash table: the row's cells plus
 * the next row sharing the same key (a key can match many rows). */
struct vx_jrow {
	struct vx_jrow *next;     /* next build row with the same key */
	struct vx_jrow *bnext;    /* bucket chain (distinct keys) */
	uint64_t        hash;
	vx_cell_t       key;      /* join key value */
	vx_cell_t      *cells;    /* build_ncol cells */
	int             matched;  /* probe matched this build row (LEFT/FULL) */
};

struct vx_jht {
	vx_jrow_t **buckets;
	int         nbucket;
	struct vx_arena_blk *arena;
};

/* Find the first build row matching key (a probe), or NULL. */
static vx_jrow_t *
jht_find(vx_jht_t *h, const vx_cell_t *key)
{
	uint64_t hv = cell_hash(key);
	uint32_t b = (uint32_t)(hv % (uint64_t)h->nbucket);
	vx_jrow_t *r;
	for (r = h->buckets[b]; r; r = r->bnext)
		if (r->hash == hv && key_eq(&r->key, key)) return r;
	return NULL;
}

/* Insert a build row under key (prepending to its key's row list). */
static int
jht_insert(vx_jht_t *h, const vx_cell_t *key, const vx_cell_t *cells, int ncol)
{
	uint64_t hv = cell_hash(key);
	uint32_t b = (uint32_t)(hv % (uint64_t)h->nbucket);
	vx_jrow_t *head = jht_find(h, key);
	vx_jrow_t *r = (vx_jrow_t *)arena_alloc(&h->arena, sizeof *r);
	int j;
	if (r == NULL) return -1;
	r->hash = hv;
	r->matched = 0;
	if (cell_dup(&h->arena, key, &r->key) != 0) return -1;
	r->cells = (vx_cell_t *)arena_alloc(&h->arena,
	    sizeof(vx_cell_t) * (size_t)(ncol > 0 ? ncol : 1));
	if (r->cells == NULL) return -1;
	for (j = 0; j < ncol; j++)
		if (cell_dup(&h->arena, &cells[j], &r->cells[j]) != 0) return -1;
	if (head != NULL) {
		/* Same key already present: chain onto its row list. */
		r->next = head->next; head->next = r;
		r->bnext = NULL;   /* not a new bucket entry */
	} else {
		r->next = NULL;
		r->bnext = h->buckets[b]; h->buckets[b] = r;
	}
	return 0;
}

/* A plain `q.col` or `col` column reference, else NULL. */
static const sql_expr_t *
is_col_ref(const sql_expr_t *e)
{
	return (e && e->op == SX_E_COLUMN && e->nname >= 1 && e->nname <= 2) ? e : NULL;
}

/* Collect referenced columns of an expression into a join context's
 * per-side namevecs (resolving the qualifier to a side).  Returns 0 if
 * every column resolves to exactly one side, -1 otherwise.  Mirrors
 * collect_columns' supported-construct gate. */
static int
jc_collect(struct vx_joinctx *jc, const sql_expr_t *e)
{
	const sql_exprlist_item_t *it;
	if (e == NULL) return -1;
	switch (e->op) {
	case SX_E_NULL: case SX_E_NUMBER: case SX_E_STRING: return 0;
	case SX_E_COLUMN: {
		const sql_str_t *qual = (e->nname == 2) ? &e->name[0] : NULL;
		const sql_str_t *col = &e->name[e->nname - 1];
		int side, hit = -1;
		for (side = 0; side < 2; side++) {
			if (qual) {
				const char *q = jc->alias[side][0] ? jc->alias[side] : jc->tab[side];
				if (!str_eq_cstr(qual, q)) continue;
			}
			if (hit >= 0 && !qual) {
				/* unqualified and could match either side -> add to the
				 * side that already has it / both; ambiguity is rejected
				 * at resolve time, so just record on each candidate side. */
			}
			if (nv_add(&jc->col[side], col) < 0) return -1;
			hit = side;
		}
		return hit >= 0 ? 0 : -1;
	}
	case SX_E_UNARY:
		if (e->op2 != TK_MINUS && e->op2 != TK_PLUS && e->op2 != TK_NOT) return -1;
		return jc_collect(jc, e->a);
	case SX_E_BINARY: {
		enum vx_op d;
		if (!tok_to_binop(e->op2, &d)) return -1;
		if (jc_collect(jc, e->a) != 0) return -1;
		return jc_collect(jc, e->b);
	}
	case SX_E_IS_NULL: return jc_collect(jc, e->a);
	case SX_E_FUNC: {
		int na = 0, ok = 0;
		if (e->ival & 3) return -1;
		for (it = e->list ? e->list->head : NULL; it; it = it->next) na++;
		(void)func_of(&e->name[0], &ok, na);
		if (!ok) return -1;
		for (it = e->list ? e->list->head : NULL; it; it = it->next)
			if (jc_collect(jc, it->expr) != 0) return -1;
		return 0;
	}
	default: return -1;
	}
}

static int
vx_try_prepare_join(sqlite3 *db, sql_arena_t *ast, const sql_select_t *sel,
                    const vx_cell_t *binds, int nbinds,
                    vx_stmt_t **out, char **errmsg)
{
	struct vx_stmt *st = NULL;
	struct vx_joinctx jc;
	struct vx_compiler comp;
	vx_joinplan_t *jp = NULL;
	const sql_src_t *s0 = sel->from, *s1 = sel->from->next;
	const sql_expr_t *on, *lhs, *rhs;
	const sql_exprlist_item_t *it;
	sqlite3_stmt *bsrc = NULL, *psrc = NULL;
	char bcols[1000], pcols[1000];
	int nproj = 0, i, side, rc = 0, lkey, rkey;

	if (errmsg) *errmsg = NULL;

	/* INNER / LEFT / RIGHT / FULL (and comma/cross) supported; ON must be
	 * present for the outer kinds (a join key is required for hashing). */
	if (s1->join != SX_J_INNER && s1->join != SX_J_NONE &&
	    s1->join != SX_J_CROSS && s1->join != SX_J_LEFT &&
	    s1->join != SX_J_RIGHT && s1->join != SX_J_FULL)
		goto fallback;
	if (s0->subquery || s1->subquery) goto fallback;
	if (s0->table.len == 0 || s0->table.len >= 64) goto fallback;
	if (s1->table.len == 0 || s1->table.len >= 64) goto fallback;
	on = s1->on;
	if (on == NULL || on->op != SX_E_BINARY || on->op2 != TK_EQ) goto fallback;
	lhs = is_col_ref(on->a); rhs = is_col_ref(on->b);
	if (lhs == NULL || rhs == NULL) goto fallback;   /* only col = col */

	memset(&jc, 0, sizeof jc);
	memcpy(jc.tab[0], s0->table.p, s0->table.len); jc.tab[0][s0->table.len] = '\0';
	memcpy(jc.tab[1], s1->table.p, s1->table.len); jc.tab[1][s1->table.len] = '\0';
	if (s0->alias.len && s0->alias.len < 64) { memcpy(jc.alias[0], s0->alias.p, s0->alias.len); jc.alias[0][s0->alias.len] = '\0'; }
	if (s1->alias.len && s1->alias.len < 64) { memcpy(jc.alias[1], s1->alias.p, s1->alias.len); jc.alias[1][s1->alias.len] = '\0'; }

	/* SELECT * is not supported on the join path (column expansion +
	 * dedup across two tables); fall back. */
	for (it = sel->cols ? sel->cols->head : NULL; it; it = it->next) {
		if (it->expr == NULL || it->expr->op == SX_E_STAR) goto fallback;
		if (it->expr->op == SX_E_FUNC && is_agg_name(&it->expr->name[0])) goto fallback;
		nproj++;
	}
	if (nproj == 0 || nproj > 32) goto fallback;

	/* Pass 1: collect referenced columns into per-side namevecs, from
	 * the projection, the WHERE, and the ON's two columns. */
	for (it = sel->cols->head; it; it = it->next)
		if (jc_collect(&jc, it->expr) != 0) goto fallback;
	if (sel->where && jc_collect(&jc, sel->where) != 0) goto fallback;
	if (jc_collect(&jc, on->a) != 0) goto fallback;
	if (jc_collect(&jc, on->b) != 0) goto fallback;

	/* Each side must contribute at least one column and have its join
	 * key.  Determine which ON column belongs to which side. */
	{
		int sa = -1, sb = -1;
		struct vx_joinctx tmp = jc;   /* jc_resolve works on combined idx; here resolve sides */
		/* Resolve lhs/rhs to a side by qualifier/name. */
		for (side = 0; side < 2; side++) {
			const sql_str_t *q = (lhs->nname == 2) ? &lhs->name[0] : NULL;
			const char *sq = jc.alias[side][0] ? jc.alias[side] : jc.tab[side];
			if (q && !str_eq_cstr(q, sq)) continue;
			{ int j; for (j = 0; j < jc.col[side].n; j++)
				if (str_eq_cstr(&lhs->name[lhs->nname-1], jc.col[side].names[j])) { sa = side; break; } }
		}
		for (side = 0; side < 2; side++) {
			const sql_str_t *q = (rhs->nname == 2) ? &rhs->name[0] : NULL;
			const char *sq = jc.alias[side][0] ? jc.alias[side] : jc.tab[side];
			if (q && !str_eq_cstr(q, sq)) continue;
			{ int j; for (j = 0; j < jc.col[side].n; j++)
				if (str_eq_cstr(&rhs->name[rhs->nname-1], jc.col[side].names[j])) { sb = side; break; } }
		}
		(void)tmp;
		if (sa < 0 || sb < 0 || sa == sb) goto fallback;   /* one col per side */
	}
	if (jc.col[0].n == 0 || jc.col[1].n == 0) goto fallback;

	/* Build the per-side source SELECTs. */
	for (side = 0; side < 2; side++) {
		char *buf = side ? pcols : bcols; int off = 0, j;
		if (jc.col[side].n > 16) goto fallback;
		for (j = 0; j < jc.col[side].n; j++) {
			int r = snprintf(buf + off, 1000 - (size_t)off, "%s%s",
			                 j ? "," : "", jc.col[side].names[j]);
			if (r < 0 || (size_t)(off + r) >= 1000) goto fallback;
			off += r;
		}
	}

	st = (struct vx_stmt *)calloc(1, sizeof *st);
	if (!st) goto oom;
	st->db = db; st->cur = -1; st->limit = -1;
	st->binds = binds; st->nbinds = nbinds;
	jp = (vx_joinplan_t *)calloc(1, sizeof *jp);
	if (!jp) goto oom;
	st->join = jp;
	/* A bare comma / CROSS join with an equality ON behaves as INNER. */
	jp->join_kind = (s1->join == SX_J_LEFT || s1->join == SX_J_RIGHT ||
	                 s1->join == SX_J_FULL) ? s1->join : SX_J_INNER;
	st->nout = nproj;
	st->proj = (vx_expr_t **)calloc((size_t)nproj, sizeof(vx_expr_t *));
	if (!st->proj) goto oom;

	snprintf(jp->build_sql, sizeof jp->build_sql, "SELECT %s FROM %s", bcols, jc.tab[0]);
	snprintf(jp->probe_sql, sizeof jp->probe_sql, "SELECT %s FROM %s", pcols, jc.tab[1]);

	/* Prepare both sources to learn column counts + affinities. */
	if (sqlite3_prepare_v2(db, jp->build_sql, -1, &bsrc, 0) != SQLITE_OK) goto fallback;
	if (sqlite3_prepare_v2(db, jp->probe_sql, -1, &psrc, 0) != SQLITE_OK) goto fallback;
	jp->build_ncol = sqlite3_column_count(bsrc);
	jp->probe_ncol = sqlite3_column_count(psrc);
	if (jp->build_ncol != jc.col[0].n || jp->probe_ncol != jc.col[1].n) goto fallback;
	jc.base[0] = 0;
	jc.base[1] = jp->build_ncol;
	for (i = 0; i < jp->build_ncol; i++)
		jc.col[0].aff[i] = vx_affinity(sqlite3_column_decltype(bsrc, i));
	for (i = 0; i < jp->probe_ncol; i++)
		jc.col[1].aff[i] = vx_affinity(sqlite3_column_decltype(psrc, i));

	/* The join keys, as combined-row indices, then split per side. */
	lkey = jc_resolve(&jc, lhs);
	rkey = jc_resolve(&jc, rhs);
	if (lkey < 0 || rkey < 0) goto fallback;
	/* Side 0 (build) key is whichever of lkey/rkey is < base[1]. */
	if (lkey < jc.base[1]) { jp->build_key = lkey; jp->probe_key = rkey - jc.base[1]; }
	else                   { jp->build_key = rkey; jp->probe_key = lkey - jc.base[1]; }
	if (jp->build_key < 0 || jp->build_key >= jp->build_ncol ||
	    jp->probe_key < 0 || jp->probe_key >= jp->probe_ncol) goto fallback;

	/* Pass 2: compile projection + filter over the combined row. */
	comp.st = st; comp.nv = NULL; comp.jc = &jc; comp.fail = 0;
	comp.binds = binds; comp.nbinds = nbinds;
	{
		int k = 0;
		for (it = sel->cols->head; it; it = it->next, k++) {
			st->proj[k] = compile_expr(&comp, it->expr);
			if (comp.fail) goto fallback;
			item_name(it, st->outname[k], sizeof st->outname[0]);
		}
		if (sel->where) {
			st->filter = compile_expr(&comp, sel->where);
			if (comp.fail) goto fallback;
		}
	}

	sqlite3_finalize(bsrc); bsrc = NULL;
	sqlite3_finalize(psrc); psrc = NULL;

	/* Combined-row scratch lives in srcrow (build_ncol + probe_ncol). */
	st->nsrc_col = jp->build_ncol + jp->probe_ncol;
	st->srcrow = (vx_cell_t *)calloc((size_t)st->nsrc_col, sizeof(vx_cell_t));
	if (!st->srcrow) goto oom;

	sql_arena_destroy(ast);
	*out = st;
	return 1;

oom:
	rc = -1;
fallback:
	if (bsrc) sqlite3_finalize(bsrc);
	if (psrc) sqlite3_finalize(psrc);
	if (ast) sql_arena_destroy(ast);
	if (st) { free(st->join); st->join = NULL; vx_finalize(st); }
	else free(jp);
	return rc;
}

/* Build the hash table from the build side, open the probe cursor. */
static int
join_build(struct vx_stmt *st, vx_jht_t *bh)
{
	vx_joinplan_t *jp = st->join;
	sqlite3_stmt *bsrc = NULL;
	struct vx_arena_blk *tmp = NULL;
	vx_cell_t rowcells[16];
	int j, rc = -1;

	memset(bh, 0, sizeof *bh);
	bh->nbucket = 1024;
	bh->buckets = (vx_jrow_t **)calloc((size_t)bh->nbucket, sizeof(vx_jrow_t *));
	if (bh->buckets == NULL) return -1;

	if (sqlite3_prepare_v2(st->db, jp->build_sql, -1, &bsrc, 0) != SQLITE_OK) goto done;
	for (;;) {
		int step = sqlite3_step(bsrc);
		if (step == SQLITE_DONE) break;
		if (step != SQLITE_ROW) goto done;
		for (j = 0; j < jp->build_ncol; j++)
			read_src_cell(bsrc, j, &rowcells[j], &tmp);
		/* SQLite equi-join: a NULL key never matches.  For INNER / RIGHT
		 * (build side not preserved) such a build row can never appear, so
		 * skip it.  For LEFT / FULL the build side is preserved, so insert
		 * it anyway -- it will never be probed (NULL never key_eq) and so
		 * surfaces in the final unmatched-build scan, NULL-extended. */
		if (rowcells[jp->build_key].type == VX_NULL &&
		    jp->join_kind != SX_J_LEFT && jp->join_kind != SX_J_FULL)
			continue;
		if (jht_insert(bh, &rowcells[jp->build_key], rowcells, jp->build_ncol) != 0)
			goto done;
	}
	rc = 0;
done:
	arena_free(tmp);
	if (bsrc) sqlite3_finalize(bsrc);
	return rc;
}

/* ---- execution --------------------------------------------------- */

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

/* Fill the storage-native source row: for each source column, the rowid
 * (src_pay == -1) or a payload column decoded from the scan record.
 * TEXT/BLOB bytes are copied into `arena` so they outlive the record. */
static void
read_rec_row(struct vx_stmt *st, int64_t rowid, const uint8_t *rec, int reclen,
             struct vx_arena_blk **arena)
{
	int i;
	for (i = 0; i < st->nsrc_col; i++) {
		vx_cell_t *cell = &st->srcrow[i];
		memset(cell, 0, sizeof *cell);
		if (st->src_pay[i] < 0) {
			cell->type = VX_INT; cell->i = rowid;
			continue;
		}
		{
			int64_t iv = 0; double dv = 0; const uint8_t *p = NULL; int n = 0;
			int cls = xstore_rec_col(rec, reclen, st->src_pay[i],
			                         &iv, &dv, &p, &n);
			switch (cls) {
			case XSTORE_C_INT:  cell->type = VX_INT; cell->i = iv; break;
			case XSTORE_C_REAL: cell->type = VX_REAL; cell->r = dv; break;
			case XSTORE_C_TEXT:
			case XSTORE_C_BLOB: {
				uint8_t *b = (uint8_t *)arena_alloc(arena, (size_t)(n > 0 ? n : 1) + 1);
				if (b) { if (n) memcpy(b, p, (size_t)n); b[n > 0 ? n : 0] = '\0'; }
				cell->type = (cls == XSTORE_C_TEXT) ? VX_TEXT : VX_BLOB;
				cell->bytes = b; cell->nbytes = (uint32_t)(n > 0 ? n : 0);
				break;
			}
			default: cell->type = VX_NULL; break;
			}
		}
	}
}

static vx_chunk_t *
next_chunk(struct vx_stmt *st, int *done)
{
	vx_chunk_t *c;
	int rc;

	*done = 0;
	c = (vx_chunk_t *)calloc(1, sizeof *c);
	if (!c) return NULL;
	c->ncol = st->nout;
	c->cap = VEXEC_VECTOR_SIZE;
	c->cells = (vx_cell_t *)malloc(sizeof(vx_cell_t) * (size_t)c->cap *
	                               (size_t)(c->ncol > 0 ? c->ncol : 1));
	if (!c->cells) { free(c); return NULL; }

	/* Open the storage scan lazily on first chunk. */
	if (st->scan == NULL) {
		st->scan = xstore_scan_open(st->bt, st->table, st->snap,
		                            st->scan_lo, st->scan_has_lo,
		                            st->scan_hi, st->scan_has_hi);
		if (st->scan == NULL) { chunk_free(c); return NULL; }
	}

	while (c->nrow < c->cap) {
		int j;
		int64_t rowid; const uint8_t *rec; int reclen;
		rc = xstore_scan_next(st->scan, &rowid, &rec, &reclen);
		if (rc == 0) { *done = 1; break; }
		if (rc != 1) { chunk_free(c); return NULL; }

		read_rec_row(st, rowid, rec, reclen, &c->arena);

		/* Filter (3-valued: only a TRUE keeps the row). */
		if (st->filter != NULL) {
			int b = eval_bool(st, st->filter, st->srcrow, &c->arena);
			if (b != 1) continue;
		}

		/* Project. */
		{
			vx_cell_t *dst = &c->cells[(size_t)c->nrow * (size_t)c->ncol];
			for (j = 0; j < st->nout; j++)
				eval(st, st->proj[j], st->srcrow, &c->arena, &dst[j]);
		}
		c->nrow++;
	}
	return c;
}

/* Drain the source into the hash table and build one result chunk with
 * one row per group.  Used by the single-threaded agg path (vx_step).
 * Returns 0 on success, -1 on error. */
static int
agg_materialize(struct vx_stmt *st)
{
	vx_aggplan_t *ap = st->agg;
	vx_chunk_t *c;
	struct vx_arena_blk *rowarena = NULL;
	int j;
	vx_grp_t *g;
	int b;

	if (htab_init(&st->ht, ap->ngrp, ap->nagg) != 0) return -1;
	if (st->scan == NULL) {
		st->scan = xstore_scan_open(st->bt, st->table, st->snap,
		                            st->scan_lo, st->scan_has_lo,
		                            st->scan_hi, st->scan_has_hi);
		if (st->scan == NULL) return -1;
	}

	/* Drain. */
	for (;;) {
		int64_t rowid; const uint8_t *rec; int reclen;
		int step = xstore_scan_next(st->scan, &rowid, &rec, &reclen);
		vx_cell_t keys[16];
		vx_grp_t *grp;
		int ai;
		if (step == 0) break;
		if (step != 1) { arena_free(rowarena); return -1; }

		read_rec_row(st, rowid, rec, reclen, &rowarena);

		if (st->filter != NULL &&
		    eval_bool(st, st->filter, st->srcrow, &rowarena) != 1)
			continue;

		for (j = 0; j < ap->ngrp; j++)
			eval(st, ap->grp[j], st->srcrow, &rowarena, &keys[j]);
		grp = htab_group(&st->ht, keys);
		if (grp == NULL) { arena_free(rowarena); return -1; }

		ai = 0;
		for (j = 0; j < ap->nout_all; j++) {
			if (!ap->out[j].is_agg) continue;
			if (ap->out[j].kind == VXA_COUNT_STAR) {
				acc_step(&grp->accs[ai], VXA_COUNT_STAR, NULL, &st->ht.arena);
			} else {
				vx_cell_t v;
				eval(st, ap->out[j].arg, st->srcrow, &rowarena, &v);
				acc_step(&grp->accs[ai], ap->out[j].kind, &v, &st->ht.arena);
			}
			ai++;
		}
	}
	arena_free(rowarena);

	/* A no-GROUP-BY aggregate over zero rows still yields one row (the
	 * empty-group aggregates: count=0, sum=NULL, etc.).  Create it. */
	if (ap->ngrp == 0 && st->ht.ngroup == 0) {
		vx_cell_t nokeys[1];
		memset(nokeys, 0, sizeof nokeys);
		if (htab_group(&st->ht, nokeys) == NULL) return -1;
	}

	/* Materialize one chunk row per group. */
	c = (vx_chunk_t *)calloc(1, sizeof *c);
	if (c == NULL) return -1;
	c->ncol = ap->nout;
	c->cap = st->ht.ngroup > 0 ? st->ht.ngroup : 1;
	c->cells = (vx_cell_t *)malloc(sizeof(vx_cell_t) * (size_t)c->cap *
	                               (size_t)(c->ncol > 0 ? c->ncol : 1));
	if (c->cells == NULL) { free(c); return -1; }

	for (b = 0; b < st->ht.nbucket; b++) {
		for (g = st->ht.buckets[b]; g; g = g->next) {
			vx_cell_t *dst = &c->cells[(size_t)c->nrow * (size_t)c->ncol];
			vx_cell_t erow[64];   /* extended row: emitted cols + HAVING aggs */
			int ki = 0, ai = 0;
			/* Compute every outcol (emitted + HAVING-only) into erow; the
			 * accumulator index ai advances over all is_agg outcols in
			 * order, matching how they were stepped. */
			for (j = 0; j < ap->nout_all && j < 64; j++) {
				if (ap->out[j].is_agg) {
					acc_final(&g->accs[ai], ap->out[j].kind, &erow[j]);
					ai++;
				} else {
					erow[j] = g->keys[ki++];
				}
			}
			/* HAVING: a predicate over the extended row.  Skip the group
			 * (do not advance c->nrow) when it is not true. */
			if (ap->having != NULL) {
				struct vx_arena_blk *hr = NULL;
				int hb = eval_bool(st, ap->having, erow, &hr);
				arena_free(hr);
				if (hb != 1) continue;
			}
			/* Emit only the first nout columns; copy TEXT/BLOB into the
			 * chunk arena (the ht arena is freed after the chunk). */
			for (j = 0; j < ap->nout; j++) {
				vx_cell_t v = erow[j];
				if ((v.type == VX_TEXT || v.type == VX_BLOB) && v.nbytes)
					(void)cell_dup(&c->arena, &v, &dst[j]);
				else dst[j] = v;
			}
			c->nrow++;
		}
	}
	st->chunk = c;
	st->cur = -1;
	st->ht_built = 1;
	return 0;
}

/* Is this an ordered/limited query (non-agg path)? */
static int
is_ordered(const struct vx_stmt *st)
{
	return st->agg == NULL &&
	       (st->norder > 0 || st->limit >= 0 || st->offset > 0 || st->distinct);
}

/* qsort context: the order keys per row + directions.  Keys are stored
 * row-major: nkey cells per row, in a flat array parallel to the output
 * rows.  We sort an index array to keep the output rows paired. */
struct sort_ctx {
	const vx_cell_t *keys;   /* nrow * nkey */
	int              nkey;
	const int       *desc;   /* nkey */
};
static struct sort_ctx *g_sort_ctx;   /* qsort_r is not portable; serialize */

static int
sort_index_cmp(const void *pa, const void *pb)
{
	int ia = *(const int *)pa, ib = *(const int *)pb;
	const struct sort_ctx *c = g_sort_ctx;
	int k;
	for (k = 0; k < c->nkey; k++) {
		const vx_cell_t *ka = &c->keys[(size_t)ia * (size_t)c->nkey + (size_t)k];
		const vx_cell_t *kb = &c->keys[(size_t)ib * (size_t)c->nkey + (size_t)k];
		int r = sort_cmp(ka, kb);
		if (c->desc[k]) r = -r;
		if (r) return r;
	}
	/* Stable-ish tiebreak by original index so equal-key rows keep a
	 * deterministic order (the differential oracle compares positionally,
	 * and SQLite leaves equal-key order unspecified, so any consistent
	 * tiebreak that the oracle also applies is fine -- but we keep input
	 * order here and the oracle compares the FULL row, so ties on the
	 * key still must match SQLite; see the test's note). */
	return ia - ib;
}

/* Materialize, sort, and offset/limit a non-agg ordered query into one
 * chunk that vx_step then walks. */
static int
ordered_materialize(struct vx_stmt *st)
{
	struct vx_arena_blk *rowarena = NULL;   /* transient per-row source bytes */
	struct vx_arena_blk *keep = NULL;        /* output + key bytes (kept) */
	vx_cell_t *rows = NULL;   /* cap * nout */
	vx_cell_t *keys = NULL;   /* cap * nkey */
	int *idx = NULL;
	int nkey = st->norder;
	int cap = 0, nrow = 0, j, rc = -1;
	vx_chunk_t *c = NULL;
	int64_t lim, off;
	int emit_n, e;

	for (;;) {
		int64_t rowid; const uint8_t *rec; int reclen;
		int step;
		vx_cell_t outrow[32], keyrow[16];
		if (st->scan == NULL) {
			st->scan = xstore_scan_open(st->bt, st->table, st->snap,
		                            st->scan_lo, st->scan_has_lo,
		                            st->scan_hi, st->scan_has_hi);
			if (st->scan == NULL) goto cleanup;
		}
		step = xstore_scan_next(st->scan, &rowid, &rec, &reclen);
		if (step == 0) break;
		if (step != 1) goto cleanup;

		read_rec_row(st, rowid, rec, reclen, &rowarena);
		if (st->filter != NULL &&
		    eval_bool(st, st->filter, st->srcrow, &rowarena) != 1)
			continue;

		/* Project the output row. */
		for (j = 0; j < st->nout; j++)
			eval(st, st->proj[j], st->srcrow, &rowarena, &outrow[j]);
		/* Compute the sort keys. */
		for (j = 0; j < nkey; j++) {
			if (st->order_outcol[j] > 0)
				keyrow[j] = outrow[st->order_outcol[j] - 1];
			else
				eval(st, st->order_key[j], st->srcrow, &rowarena, &keyrow[j]);
		}

		if (nrow == cap) {
			int nc = cap ? cap * 2 : 256;
			vx_cell_t *nr = realloc(rows, sizeof(vx_cell_t) * (size_t)nc * (size_t)(st->nout > 0 ? st->nout : 1));
			vx_cell_t *nk = realloc(keys, sizeof(vx_cell_t) * (size_t)nc * (size_t)(nkey > 0 ? nkey : 1));
			if (!nr || !nk) { free(nr); free(nk); goto cleanup; }
			rows = nr; keys = nk; cap = nc;
		}
		/* Copy output + key cells into the kept arena (row bytes are
		 * transient -- the next sqlite3_step reuses the source row). */
		for (j = 0; j < st->nout; j++)
			if (cell_dup(&keep, &outrow[j], &rows[(size_t)nrow * (size_t)st->nout + (size_t)j]) != 0) goto cleanup;
		for (j = 0; j < nkey; j++)
			if (cell_dup(&keep, &keyrow[j], &keys[(size_t)nrow * (size_t)nkey + (size_t)j]) != 0) goto cleanup;
		nrow++;
		arena_free(rowarena); rowarena = NULL;
	}

	/* Sort the index array by keys (if any ORDER BY). */
	idx = (int *)malloc(sizeof(int) * (size_t)(nrow > 0 ? nrow : 1));
	if (!idx) goto cleanup;
	for (j = 0; j < nrow; j++) idx[j] = j;
	if (nkey > 0) {
		struct sort_ctx sc; sc.keys = keys; sc.nkey = nkey; sc.desc = st->order_desc;
		g_sort_ctx = &sc;
		qsort(idx, (size_t)nrow, sizeof(int), sort_index_cmp);
		g_sort_ctx = NULL;
	}

	/* Build the surviving index list (sorted order), deduping first when
	 * DISTINCT so OFFSET/LIMIT apply to the DISTINCT result -- not the
	 * raw rows.  kept[] holds row indices into rows[] in emit order. */
	{
		int *kept = (int *)malloc(sizeof(int) * (size_t)(nrow > 0 ? nrow : 1));
		int nkept = 0, p;
		if (!kept) goto cleanup;
		for (p = 0; p < nrow; p++) {
			int si = idx[p];
			if (st->distinct) {
				int d, dup = 0;
				for (d = 0; d < nkept && !dup; d++) {
					int cc, same = 1;
					for (cc = 0; cc < st->nout; cc++)
						if (!key_eq(&rows[(size_t)kept[d] * (size_t)st->nout + (size_t)cc],
						            &rows[(size_t)si * (size_t)st->nout + (size_t)cc])) {
							same = 0; break;
						}
					if (same) dup = 1;
				}
				if (dup) continue;
			}
			kept[nkept++] = si;
		}

		/* OFFSET / LIMIT over the kept (deduped) list. */
		off = st->offset;
		lim = st->limit;   /* -1 = unbounded */
		if (off > nkept) off = nkept;
		emit_n = nkept - (int)off;
		if (lim >= 0 && emit_n > (int)lim) emit_n = (int)lim;

		c = (vx_chunk_t *)calloc(1, sizeof *c);
		if (!c) { free(kept); goto cleanup; }
		c->ncol = st->nout;
		c->cap = emit_n > 0 ? emit_n : 1;
		c->cells = (vx_cell_t *)malloc(sizeof(vx_cell_t) * (size_t)c->cap * (size_t)(st->nout > 0 ? st->nout : 1));
		if (!c->cells) { free(kept); free(c); c = NULL; goto cleanup; }
		for (e = 0; e < emit_n; e++) {
			int si = kept[(int)off + e];
			vx_cell_t *dst = &c->cells[(size_t)c->nrow * (size_t)c->ncol];
			for (j = 0; j < st->nout; j++) {
				vx_cell_t v = rows[(size_t)si * (size_t)st->nout + (size_t)j];
				if ((v.type == VX_TEXT || v.type == VX_BLOB) && v.nbytes)
					(void)cell_dup(&c->arena, &v, &dst[j]);
				else dst[j] = v;
			}
			c->nrow++;
		}
		free(kept);
	}
	st->chunk = c; c = NULL;
	st->cur = -1;
	st->ordered_built = 1;
	rc = 0;

cleanup:
	arena_free(rowarena);
	arena_free(keep);
	free(rows); free(keys); free(idx);
	if (c) chunk_free(c);
	return rc;
}

/* Produce one chunk of joined rows by probing.  Advances the current
 * build-match chain, fetching the next probe row when the chain is
 * exhausted.  *done set at end of the probe input. */
/* Run st->srcrow (the combined build|probe row already laid out) through
 * the join's filter + projection, writing one output row into the chunk.
 * Returns 1 if a row was emitted (c->nrow advanced), 0 if the filter
 * rejected it, -1 on error. */
static int
join_emit_row(struct vx_stmt *st, vx_chunk_t *c)
{
	int j;
	vx_cell_t *dst;
	if (st->filter != NULL &&
	    eval_bool(st, st->filter, st->srcrow, &c->arena) != 1)
		return 0;
	dst = &c->cells[(size_t)c->nrow * (size_t)c->ncol];
	for (j = 0; j < st->nout; j++) {
		eval(st, st->proj[j], st->srcrow, &c->arena, &dst[j]);
		/* eval may return a cell pointing into the build (jht,
		 * statement-lifetime -- safe) or probe arena (reset per probe
		 * row).  Deep-copy any TEXT/BLOB into the chunk arena so it
		 * survives the next probe row and chunk reuse. */
		if ((dst[j].type == VX_TEXT || dst[j].type == VX_BLOB) &&
		    dst[j].nbytes) {
			vx_cell_t cp;
			if (cell_dup(&c->arena, &dst[j], &cp) == 0) dst[j] = cp;
		}
	}
	c->nrow++;
	return 1;
}

static vx_chunk_t *
join_next_chunk(struct vx_stmt *st, int *done)
{
	vx_joinplan_t *jp = st->join;
	int left_outer = (jp->join_kind == SX_J_LEFT || jp->join_kind == SX_J_FULL);
	int right_outer = (jp->join_kind == SX_J_RIGHT || jp->join_kind == SX_J_FULL);
	vx_chunk_t *c;
	int j;

	*done = 0;
	c = (vx_chunk_t *)calloc(1, sizeof *c);
	if (!c) return NULL;
	c->ncol = st->nout;
	c->cap = VEXEC_VECTOR_SIZE;
	c->cells = (vx_cell_t *)malloc(sizeof(vx_cell_t) * (size_t)c->cap *
	                               (size_t)(c->ncol > 0 ? c->ncol : 1));
	if (!c->cells) { free(c); return NULL; }

	while (c->nrow < c->cap) {
		/* Phase 2 (LEFT / FULL): once the probe is drained, walk the
		 * build hash emitting unmatched build rows NULL-extended on the
		 * probe side. */
		if (st->probe_done) {
			if (!left_outer) { *done = 1; break; }
			for (;;) {
				if (st->bscan_r == NULL) {
					if (st->bscan_b >= st->jht->nbucket) { *done = 1; goto out; }
					st->bscan_r = st->jht->buckets[st->bscan_b++];
					continue;
				}
				/* Walk this bucket's distinct keys (bnext) AND each key's
				 * same-key chain (next): every build row is reachable. */
				{
					vx_jrow_t *r = st->bscan_r;
					vx_jrow_t *chain;
					st->bscan_r = r->bnext;
					for (chain = r; chain; chain = chain->next) {
						if (chain->matched) continue;
						for (j = 0; j < jp->build_ncol; j++)
							st->srcrow[j] = chain->cells[j];
						for (j = 0; j < jp->probe_ncol; j++)
							st->srcrow[jp->build_ncol + j].type = VX_NULL;
						if (join_emit_row(st, c) < 0) { chunk_free(c); return NULL; }
						if (c->nrow >= c->cap) goto out;
					}
				}
			}
		}

		/* Phase 1: probe.  Need a probe row + its matching build chain? */
		if (st->match == NULL) {
			int step;
			vx_cell_t pk;
			arena_free(st->probe_arena); st->probe_arena = NULL;
			step = sqlite3_step(st->probe);
			if (step == SQLITE_DONE) { st->probe_done = 1; continue; }
			if (step != SQLITE_ROW) { chunk_free(c); return NULL; }
			if (st->probe_cells == NULL)
				st->probe_cells = (vx_cell_t *)calloc((size_t)jp->probe_ncol,
				                                      sizeof(vx_cell_t));
			if (st->probe_cells == NULL) { chunk_free(c); return NULL; }
			for (j = 0; j < jp->probe_ncol; j++)
				read_src_cell(st->probe, j, &st->probe_cells[j], &st->probe_arena);
			pk = st->probe_cells[jp->probe_key];
			st->match = (pk.type == VX_NULL) ? NULL : jht_find(st->jht, &pk);
			if (st->match == NULL) {
				/* Probe row matched nothing.  RIGHT / FULL: emit it once
				 * with the build side NULL.  INNER / LEFT: skip. */
				if (right_outer) {
					for (j = 0; j < jp->build_ncol; j++)
						st->srcrow[j].type = VX_NULL;
					for (j = 0; j < jp->probe_ncol; j++)
						st->srcrow[jp->build_ncol + j] = st->probe_cells[j];
					if (join_emit_row(st, c) < 0) { chunk_free(c); return NULL; }
				}
				continue;
			}
		}
		/* Emit the combined row for the current match, then advance. */
		st->match->matched = 1;   /* this build row found a probe (LEFT/FULL) */
		for (j = 0; j < jp->build_ncol; j++)
			st->srcrow[j] = st->match->cells[j];
		for (j = 0; j < jp->probe_ncol; j++)
			st->srcrow[jp->build_ncol + j] = st->probe_cells[j];
		st->match = st->match->next;
		if (join_emit_row(st, c) < 0) { chunk_free(c); return NULL; }
	}
out:
	return c;
}

int
vx_step(vx_stmt_t *st)
{
	if (st == NULL) return SQLITE_MISUSE;

	/* Join: build the hash table + open the probe cursor on first step. */
	if (st->join != NULL && !st->join_built) {
		st->jht = (vx_jht_t *)calloc(1, sizeof *st->jht);
		if (st->jht == NULL) return SQLITE_ERROR;
		if (join_build(st, st->jht) != 0) return SQLITE_ERROR;
		if (sqlite3_prepare_v2(st->db, st->join->probe_sql, -1, &st->probe, 0)
		    != SQLITE_OK) return SQLITE_ERROR;
		st->join_built = 1;
	}

	/* Aggregation: build all groups on first step, then walk the chunk. */
	if (st->agg != NULL && !st->ht_built) {
		if (agg_materialize(st) != 0) return SQLITE_ERROR;
	}
	/* Ordered/limited (non-agg): materialize + sort on first step. */
	if (is_ordered(st) && !st->ordered_built) {
		if (ordered_materialize(st) != 0) return SQLITE_ERROR;
	}

	if (st->chunk != NULL && st->cur + 1 < st->chunk->nrow) {
		st->cur++;
		return SQLITE_ROW;
	}
	if (st->agg != NULL || is_ordered(st)) {
		/* All rows are in the one materialized chunk; once consumed, done. */
		if (st->chunk != NULL && st->cur + 1 >= st->chunk->nrow)
			return SQLITE_DONE;
	}
	for (;;) {
		int done = 0;
		vx_chunk_t *c;
		if (st->done) return SQLITE_DONE;
		c = st->join ? join_next_chunk(st, &done) : next_chunk(st, &done);
		if (c == NULL) return SQLITE_ERROR;
		if (st->chunk) chunk_free(st->chunk);
		st->chunk = c;
		st->cur = -1;
		st->done = done;
		if (c->nrow > 0) { st->cur = 0; return SQLITE_ROW; }
		if (done) return SQLITE_DONE;
	}
}

/* ---- column accessors -------------------------------------------- */

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
	if (st->scan) xstore_scan_close(st->scan);
	if (st->chunk) chunk_free(st->chunk);
	if (st->plan_arena) arena_free(st->plan_arena);
	htab_free(&st->ht);
	if (st->agg) { free(st->agg->out); free(st->agg->grp); free(st->agg); }
	if (st->join) {
		if (st->jht) { arena_free(st->jht->arena); free(st->jht->buckets); free(st->jht); }
		if (st->probe) sqlite3_finalize(st->probe);
		arena_free(st->probe_arena);
		free(st->probe_cells);
		free(st->join);
	}
	free(st->order_key);
	free(st->order_outcol);
	free(st->order_desc);
	free(st->proj);
	free(st->srcrow);
	free(st);
}

/* ================================================================== *
 * V2: morsel-parallel execution
 * ================================================================== */

/* A collected result: row-major cells + an arena owning TEXT/BLOB. */
struct vx_result {
	int        nrow, ncol, cap;
	vx_cell_t *cells;
	struct vx_arena_blk *arena;
	int        nworkers;
	char       name[64][64];   /* output column names from the plan; an
	                            * empty entry means "use the VDBE name" */
};

static int
result_push(struct vx_result *r, const vx_cell_t *row)
{
	int j;
	if (r->nrow == r->cap) {
		int nc = r->cap ? r->cap * 2 : 256;
		vx_cell_t *nn = (vx_cell_t *)realloc(r->cells,
		    sizeof(vx_cell_t) * (size_t)nc * (size_t)(r->ncol > 0 ? r->ncol : 1));
		if (nn == NULL) return -1;
		r->cells = nn; r->cap = nc;
	}
	/* Copy TEXT/BLOB bytes into the result arena so they outlive the
	 * worker chunk they came from. */
	for (j = 0; j < r->ncol; j++) {
		vx_cell_t c = row[j];
		if ((c.type == VX_TEXT || c.type == VX_BLOB) && c.nbytes) {
			uint8_t *p = (uint8_t *)arena_alloc(&r->arena, (size_t)c.nbytes + 1);
			if (p == NULL) return -1;
			memcpy(p, c.bytes, c.nbytes); p[c.nbytes] = '\0';
			c.bytes = p;
		}
		r->cells[(size_t)r->nrow * (size_t)r->ncol + (size_t)j] = c;
	}
	r->nrow++;
	return 0;
}

/* Hash a whole result row (combine the per-cell hashes). */
static uint64_t
row_hash(const vx_cell_t *row, int ncol)
{
	uint64_t h = 1469598103934665603ULL;
	int j;
	for (j = 0; j < ncol; j++)
		h ^= cell_hash(&row[j]) * (uint64_t)(j + 1) * 0x100000001b3ULL;
	return h;
}

/* Two result rows are equal as a SQL row value: every column compares
 * equal under key_eq (NULLs equal, numeric cross-type equal). */
static int
row_cells_eq(const vx_cell_t *a, const vx_cell_t *b, int ncol)
{
	int j;
	for (j = 0; j < ncol; j++)
		if (!key_eq(&a[j], &b[j])) return 0;
	return 1;
}

/* A hash index over result rows, for dedup / set membership.  Stores
 * (hash, row-index) pairs; lookups verify with row_cells_eq against the
 * backing result's cells. */
struct row_index {
	struct { uint64_t h; int row; } *e;
	int n, cap;
};

static void row_index_free(struct row_index *ix) { free(ix->e); ix->e = NULL; ix->n = ix->cap = 0; }

/* Is `row` present among the rows already added to `ix` (which index
 * into `r`)?  Returns 1 if found. */
static int
row_index_has(const struct row_index *ix, const struct vx_result *r,
              const vx_cell_t *row, uint64_t h)
{
	int i;
	for (i = 0; i < ix->n; i++) {
		if (ix->e[i].h != h) continue;
		if (row_cells_eq(&r->cells[(size_t)ix->e[i].row * (size_t)r->ncol],
		                 row, r->ncol))
			return 1;
	}
	return 0;
}

static int
row_index_add(struct row_index *ix, int rowno, uint64_t h)
{
	if (ix->n == ix->cap) {
		int nc = ix->cap ? ix->cap * 2 : 64;
		void *nn = realloc(ix->e, sizeof *ix->e * (size_t)nc);
		if (nn == NULL) return -1;
		ix->e = nn; ix->cap = nc;
	}
	ix->e[ix->n].h = h; ix->e[ix->n].row = rowno; ix->n++;
	return 0;
}

/* Remove duplicate rows from `r` in place, keeping the first occurrence
 * (SQL DISTINCT / UNION semantics).  Returns 0 or -1 on OOM. */
static int
result_dedup(struct vx_result *r)
{
	struct row_index ix; int i, w = 0, rc = 0;
	memset(&ix, 0, sizeof ix);
	for (i = 0; i < r->nrow; i++) {
		const vx_cell_t *row = &r->cells[(size_t)i * (size_t)r->ncol];
		uint64_t h = row_hash(row, r->ncol);
		/* Build the index against the COMPACTED prefix [0,w). */
		if (row_index_has(&ix, r, row, h)) continue;
		if (w != i)
			memmove(&r->cells[(size_t)w * (size_t)r->ncol], row,
			        sizeof(vx_cell_t) * (size_t)r->ncol);
		if (row_index_add(&ix, w, h) != 0) { rc = -1; break; }
		w++;
	}
	r->nrow = w;
	row_index_free(&ix);
	return rc;
}

/* Shared context across the morsel workers. */
struct vx_par {
	const struct vx_stmt *plan;   /* compiled template (proj/filter/cols/table) */
	_Atomic int64_t cursor;       /* next rowid to hand out */
	int64_t      hi;              /* one past the max rowid */
	int64_t      morsel;          /* rowids per morsel */
	int          nworkers;
	struct vx_result *parts;      /* nworkers result buffers (non-agg) */
	vx_htab_t   *wht;             /* nworkers per-worker hash tables (agg) */
	_Atomic int  error;           /* any worker failed */
};

/* One worker: own connection, own range-scoped source statement, reuse
 * the shared compiled plan (its proj/filter trees are immutable). */
static int
par_worker(xtc_task_t *self, void *user)
{
	struct { struct vx_par *par; int idx; } *a = user;
	struct vx_par *par = a->par;
	int widx = a->idx;
	struct vx_stmt w;     /* lightweight per-worker execution state */
	struct vx_arena_blk *rowarena = NULL;
	vx_cell_t *srcrow = NULL;
	int rc_ok = 0;

	(void)self;

	/* The worker borrows the plan's compiled trees + storage source
	 * (the bt is a shared process pointer; each worker opens its OWN
	 * xstore_scan over a disjoint rowid range, so the cursors are
	 * independent -- the B-link tree's latch-free reads make concurrent
	 * range scans safe). */
	memset(&w, 0, sizeof w);
	w.nout = par->plan->nout;
	w.proj = par->plan->proj;
	w.filter = par->plan->filter;
	w.agg = par->plan->agg;        /* borrowed (immutable plan) */
	w.bt = par->plan->bt;
	w.snap = par->plan->snap;
	w.nsrc_col = par->plan->nsrc_col;
	memcpy(w.src_pay, par->plan->src_pay, sizeof w.src_pay);
	snprintf(w.table, sizeof w.table, "%s", par->plan->table);
	srcrow = (vx_cell_t *)calloc((size_t)(w.nsrc_col > 0 ? w.nsrc_col : 1),
	                             sizeof(vx_cell_t));
	if (srcrow == NULL) goto done;
	w.srcrow = srcrow;

	for (;;) {
		int64_t lo = atomic_fetch_add_explicit(&par->cursor, par->morsel,
		                                       memory_order_relaxed);
		int64_t mh;
		xstore_scan_t *ms;
		if (lo >= par->hi) break;
		mh = lo + par->morsel;
		if (mh > par->hi) mh = par->hi;

		/* A morsel = the rowid range [lo, mh-1]. */
		ms = xstore_scan_open(w.bt, w.table, w.snap, lo, 1, mh - 1, 1);
		if (ms == NULL) { atomic_store(&par->error, 1); goto done; }

		for (;;) {
			int64_t rowid; const uint8_t *rec; int reclen, j;
			int step = xstore_scan_next(ms, &rowid, &rec, &reclen);
			if (step == 0) break;
			if (step != 1) { xstore_scan_close(ms); atomic_store(&par->error, 1); goto done; }

			read_rec_row(&w, rowid, rec, reclen, &rowarena);

			if (w.filter != NULL &&
			    eval_bool(&w, w.filter, w.srcrow, &rowarena) != 1)
				continue;

			if (w.agg != NULL) {
				/* Accumulate into this worker's hash table. */
				vx_aggplan_t *ap = w.agg;
				vx_cell_t keys[16]; vx_grp_t *grp; int ai = 0;
				for (j = 0; j < ap->ngrp; j++)
					eval(&w, ap->grp[j], w.srcrow, &rowarena, &keys[j]);
				grp = htab_group(&par->wht[widx], keys);
				if (grp == NULL) { xstore_scan_close(ms); atomic_store(&par->error, 1); goto done; }
				for (j = 0; j < ap->nout; j++) {
					if (!ap->out[j].is_agg) continue;
					if (ap->out[j].kind == VXA_COUNT_STAR)
						acc_step(&grp->accs[ai], VXA_COUNT_STAR, NULL,
						         &par->wht[widx].arena);
					else {
						vx_cell_t v;
						eval(&w, ap->out[j].arg, w.srcrow, &rowarena, &v);
						acc_step(&grp->accs[ai], ap->out[j].kind, &v,
						         &par->wht[widx].arena);
					}
					ai++;
				}
			} else {
				vx_cell_t out[32];
				for (j = 0; j < w.nout; j++)
					eval(&w, w.proj[j], w.srcrow, &rowarena, &out[j]);
				if (result_push(&par->parts[widx], out) != 0) {
					xstore_scan_close(ms);
					atomic_store(&par->error, 1); goto done;
				}
			}
		}
		xstore_scan_close(ms);
		arena_free(rowarena); rowarena = NULL;
	}
	rc_ok = 1;

done:
	if (rowarena) arena_free(rowarena);
	free(srcrow);
	if (!rc_ok) atomic_store(&par->error, 1);
	return 0;   /* task DONE */
}

/* True when a recognized plan can run on the morsel-parallel storage
 * scan: a single-table, unordered, unlimited, unjoined, storage-backed
 * scan or aggregation.  Ordered/limited/joined/non-storage plans run
 * single-threaded. */
static int
is_parallelizable_plan(const vx_stmt_t *plan)
{
	return plan->norder == 0 && plan->limit < 0 && plan->offset <= 0 &&
	    plan->join == NULL && plan->bt != NULL && !plan->distinct &&
	    (plan->agg == NULL || plan->agg->having == NULL);
}

/* Run an already-recognized, already-checked-parallelizable plan on the
 * morsel-parallel storage scan.  Takes ownership of plan (finalizes it).
 * Returns 1 with *res set, or <0 on error. */
static int
run_parallel_plan(vx_stmt_t *plan, sqlite3 *db, int n_workers,
                  vx_result_t **res, char **errmsg)
{
	struct vx_par par;
	struct vx_result *out = NULL;
	xtc_exec_t *exec = NULL;
	struct { struct vx_par *par; int idx; } *args = NULL;
	int i, rc = 0;
	int64_t lo = 0, hi = 0;

	(void)errmsg;
	if (n_workers < 1) n_workers = 1;

	/* rowid bounds: [min, max] of the table (empty -> no rows). */
	{
		char q[128]; sqlite3_stmt *b = NULL;
		snprintf(q, sizeof q, "SELECT min(_rowid_), max(_rowid_) FROM %s",
		         plan->table);
		if (sqlite3_prepare_v2(db, q, -1, &b, 0) == SQLITE_OK &&
		    sqlite3_step(b) == SQLITE_ROW &&
		    sqlite3_column_type(b, 0) != SQLITE_NULL) {
			lo = sqlite3_column_int64(b, 0);
			hi = sqlite3_column_int64(b, 1) + 1;   /* one past max */
		}
		if (b) sqlite3_finalize(b);
	}

	out = (struct vx_result *)calloc(1, sizeof *out);
	if (out == NULL) { rc = -1; goto cleanup; }
	out->ncol = plan->nout;
	out->nworkers = n_workers;
	memcpy(out->name, plan->outname, sizeof out->name);

	memset(&par, 0, sizeof par);
	par.plan = plan;
	atomic_store(&par.cursor, lo);
	par.hi = hi;
	/* Morsel size: each xstore_scan_open re-descends the B-tree from the
	 * root to seek the morsel's start, so morsels must be LARGE for that
	 * cost to amortize over a long scan.  Aim for ~2 morsels per worker
	 * (enough to load-balance a skewed range, few enough that descents
	 * are negligible), with a floor so tiny tables do not over-split. */
	{
		int64_t span = hi - lo;
		int64_t target = span / (int64_t)(n_workers * 2 > 0 ? n_workers * 2 : 1);
		if (target < 65536) target = 65536;   /* descents amortize over >=64K rows */
		par.morsel = target > 0 ? target : 1;
	}
	par.nworkers = n_workers;
	par.parts = (struct vx_result *)calloc((size_t)n_workers, sizeof(struct vx_result));
	if (par.parts == NULL) { rc = -1; goto cleanup; }
	for (i = 0; i < n_workers; i++) par.parts[i].ncol = plan->nout;
	/* Aggregating plan: a per-worker hash table accumulates partials. */
	if (plan->agg != NULL) {
		par.wht = (vx_htab_t *)calloc((size_t)n_workers, sizeof(vx_htab_t));
		if (par.wht == NULL) { rc = -1; goto cleanup; }
		for (i = 0; i < n_workers; i++)
			if (htab_init(&par.wht[i], plan->agg->ngrp, plan->agg->nagg) != 0) {
				rc = -1; goto cleanup;
			}
	}

	/* Run one worker per loop on a libxtc executor. */
	if (xtc_exec_init(&exec, n_workers) != XTC_OK) { rc = -1; goto cleanup; }
	out->nworkers = xtc_exec_n_loops(exec);
	args = (void *)calloc((size_t)n_workers, sizeof *args);
	if (args == NULL) { rc = -1; goto cleanup; }
	for (i = 0; i < n_workers; i++) {
		args[i].par = &par; args[i].idx = i;
		if (xtc_exec_spawn_on(exec, i % xtc_exec_n_loops(exec),
		        par_worker, &args[i], NULL) != XTC_OK) {
			rc = -1; goto cleanup;
		}
	}
	(void)xtc_exec_run(exec);   /* blocks until all workers DONE */

	if (atomic_load(&par.error)) { rc = -1; goto cleanup; }

	if (plan->agg != NULL) {
		/* Combine the per-worker hash tables into worker 0's, merging
		 * accumulators group by group, then emit one final row each. */
		vx_aggplan_t *ap = plan->agg;
		vx_htab_t *m = &par.wht[0];
		int b;
		for (i = 1; i < n_workers; i++) {
			for (b = 0; b < par.wht[i].nbucket; b++) {
				vx_grp_t *g;
				for (g = par.wht[i].buckets[b]; g; g = g->next) {
					vx_grp_t *d = htab_group(m, g->keys);
					int ai = 0, j;
					if (d == NULL) { rc = -1; goto cleanup; }
					for (j = 0; j < ap->nout; j++) {
						if (!ap->out[j].is_agg) continue;
						acc_merge(&d->accs[ai], &g->accs[ai],
						          ap->out[j].kind, &m->arena);
						ai++;
					}
				}
			}
		}
		/* Empty input with no GROUP BY still yields one row. */
		if (ap->ngrp == 0 && m->ngroup == 0) {
			vx_cell_t nokeys[1]; memset(nokeys, 0, sizeof nokeys);
			if (htab_group(m, nokeys) == NULL) { rc = -1; goto cleanup; }
		}
		for (b = 0; b < m->nbucket; b++) {
			vx_grp_t *g;
			for (g = m->buckets[b]; g; g = g->next) {
				vx_cell_t row[32];
				int ki = 0, ai = 0, j;
				for (j = 0; j < ap->nout; j++) {
					if (ap->out[j].is_agg)
						acc_final(&g->accs[ai++], ap->out[j].kind, &row[j]);
					else
						row[j] = g->keys[ki++];
				}
				if (result_push(out, row) != 0) { rc = -1; goto cleanup; }
			}
		}
	} else {
		/* Combine: concatenate per-worker buffers (multiset). */
		for (i = 0; i < n_workers; i++) {
			int k;
			for (k = 0; k < par.parts[i].nrow; k++) {
				const vx_cell_t *row =
				    &par.parts[i].cells[(size_t)k * (size_t)out->ncol];
				if (result_push(out, row) != 0) { rc = -1; goto cleanup; }
			}
		}
	}

	*res = out; out = NULL;
	rc = 1;

cleanup:
	if (exec) xtc_exec_fini(exec);
	if (par.parts) {
		for (i = 0; i < n_workers; i++) {
			arena_free(par.parts[i].arena);
			free(par.parts[i].cells);
		}
		free(par.parts);
	}
	free(args);
	if (par.wht) {
		for (i = 0; i < n_workers; i++) htab_free(&par.wht[i]);
		free(par.wht);
	}
	if (out) { arena_free(out->arena); free(out->cells); free(out); }
	if (plan) vx_finalize(plan);
	return rc;
}

/* Collect a recognized serial plan (vx_step) into a materialized result,
 * for plans that vexec recognizes but cannot run on the parallel storage
 * scan (ordered/limited/joined/single-table at one worker).  Takes
 * ownership of plan (finalizes it).  Returns 1 with *res, or <0. */
static int
collect_serial(vx_stmt_t *plan, vx_result_t **res)
{
	struct vx_result *out;
	int ncol, step, rc = 1, i, distinct = plan->distinct;
	vx_cell_t rowbuf[64];

	ncol = vx_column_count(plan);
	if (ncol > (int)(sizeof rowbuf / sizeof rowbuf[0])) { vx_finalize(plan); return 0; }
	out = (struct vx_result *)calloc(1, sizeof *out);
	if (out == NULL) { vx_finalize(plan); return -1; }
	out->ncol = ncol;
	out->nworkers = 1;
	memcpy(out->name, plan->outname, sizeof out->name);

	while ((step = vx_step(plan)) == SQLITE_ROW) {
		for (i = 0; i < ncol; i++) {
			vx_cell_t c; memset(&c, 0, sizeof c);
			switch (vx_column_type(plan, i)) {
			case VX_INT:  c.type = VX_INT;  c.i = vx_column_int64(plan, i); break;
			case VX_REAL: c.type = VX_REAL; c.r = vx_column_double(plan, i); break;
			case VX_TEXT: {
				const char *t = vx_column_text(plan, i);
				c.type = VX_TEXT; c.bytes = (const uint8_t *)t;
				c.nbytes = vx_column_bytes(plan, i);
				break; }
			case VX_BLOB:
				c.type = VX_BLOB; c.bytes = vx_column_blob(plan, i);
				c.nbytes = vx_column_bytes(plan, i);
				break;
			default: c.type = VX_NULL; break;
			}
			rowbuf[i] = c;
		}
		if (result_push(out, rowbuf) != 0) { rc = -1; break; }
	}
	if (step != SQLITE_DONE && rc == 1) rc = -1;
	if (rc == 1 && distinct && result_dedup(out) != 0) rc = -1;

	if (rc == 1) { *res = out; }
	else { arena_free(out->arena); free(out->cells); free(out); }
	vx_finalize(plan);
	return rc;
}

int
vx_run_parallel(sqlite3 *db, const char *sql, int n_workers,
                vx_result_t **res, char **errmsg)
{
	vx_stmt_t *plan = NULL;
	int recog;

	if (res) *res = NULL;
	if (errmsg) *errmsg = NULL;

	/* Recognize the plan once on the caller's connection (its bt is the
	 * one the workers' storage scans run over -- the workers share that
	 * process pointer and open their own range cursors on it). */
	recog = vx_try_prepare(db, sql, &plan, errmsg);
	if (recog != 1) return recog;             /* 0 fallback / <0 err */

	if (!is_parallelizable_plan(plan)) { vx_finalize(plan); return 0; }
	return run_parallel_plan(plan, db, n_workers, res, errmsg);
}

/* ---- native autocommit write path (VDBE-free INSERT) ------------- */

/*
 * One column of the target table, in declared order.  is_pk marks the
 * INTEGER PRIMARY KEY (xstore's rowid); the other columns are the
 * payload, encoded in declared order (payload index = declared index
 * with the pk column removed).
 */
struct wschema {
	char    name[64][64];
	int     is_pk[64];
	int     n;
	int     pk_col;        /* declared index of the pk, or -1 */
};

static int
load_wschema(sqlite3 *db, const char *table, struct wschema *ws)
{
	sqlite3_stmt *ti = NULL;
	char q[128];
	ws->n = 0; ws->pk_col = -1;

	/* Native catalog first (no VDBE / no sqlite_master). */
	{
		bt_t *bt = xstore_bt_of(db);
		xstore_col_t cols[64];
		int nc = bt ? xstore_table_schema(bt, table, cols, 64) : 0, c;
		if (nc > 0) {
			for (c = 0; c < nc && ws->n < 64; c++) {
				snprintf(ws->name[ws->n], sizeof ws->name[0], "%.*s",
				         (int)(sizeof ws->name[0] - 1), cols[c].name);
				ws->is_pk[ws->n] = cols[c].is_pk;
				if (cols[c].is_pk) ws->pk_col = ws->n;
				ws->n++;
			}
			return ws->n > 0 ? 0 : -1;
		}
	}

	snprintf(q, sizeof q, "PRAGMA table_info(%s)", table);
	if (sqlite3_prepare_v2(db, q, -1, &ti, 0) != SQLITE_OK) return -1;
	while (sqlite3_step(ti) == SQLITE_ROW && ws->n < 64) {
		const char *nm = (const char *)sqlite3_column_text(ti, 1);
		int pk = sqlite3_column_int(ti, 5);
		if (nm == NULL) { sqlite3_finalize(ti); return -1; }
		snprintf(ws->name[ws->n], sizeof ws->name[0], "%s", nm);
		ws->is_pk[ws->n] = pk;
		if (pk) ws->pk_col = ws->n;
		ws->n++;
	}
	sqlite3_finalize(ti);
	return ws->n > 0 ? 0 : -1;
}

/* Evaluate an INSERT VALUES literal into a vx_cell.  Returns 0 on a
 * supported literal (INT/REAL/TEXT/NULL), or -1 (the statement is left
 * to the VDBE).  No params, no expressions, no functions. */
static int
lit_cell(struct vx_arena_blk **arena, const sql_expr_t *e, vx_cell_t *out,
         const vx_cell_t *binds, int nbinds)
{
	memset(out, 0, sizeof *out);
	if (e == NULL) { out->type = VX_NULL; return 0; }
	switch (e->op) {
	case SX_E_NULL:
		out->type = VX_NULL; return 0;
	case SX_E_PARAM: {
		int ord = (int)e->ival;   /* 1-based */
		const vx_cell_t *v;
		if (binds == NULL || ord < 1 || ord > nbinds) return -1;
		v = &binds[ord - 1];
		if (v->type == VX_TEXT || v->type == VX_BLOB) {
			uint8_t *p = (uint8_t *)arena_alloc(arena, (size_t)v->nbytes + 1);
			if (p == NULL) return -1;
			if (v->nbytes) memcpy(p, v->bytes, v->nbytes);
			p[v->nbytes] = '\0';
			out->type = v->type; out->bytes = p; out->nbytes = v->nbytes;
		} else {
			*out = *v;   /* INT / REAL / NULL by value */
		}
		return 0;
	}
	case SX_E_NUMBER: {
		char buf[64]; int i, isreal = 0;
		if (e->lit.len == 0 || e->lit.len >= sizeof buf) return -1;
		for (i = 0; i < (int)e->lit.len; i++) {
			char ch = e->lit.p[i];
			if (ch == '.' || ch == 'e' || ch == 'E') isreal = 1;
		}
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
		out->type = VX_TEXT; out->bytes = p; out->nbytes = (int)e->lit.len;
		return 0;
	}
	case SX_E_UNARY:
		/* a leading minus on a numeric literal (-5) */
		if (e->op2 == TK_MINUS && e->a && e->a->op == SX_E_NUMBER) {
			if (lit_cell(arena, e->a, out, binds, nbinds) != 0) return -1;
			if (out->type == VX_INT)  out->i = -out->i;
			else if (out->type == VX_REAL) out->r = -out->r;
			return 0;
		}
		return -1;
	default:
		return -1;
	}
}

/* Encode a row's payload cells into the xstore record format
 * ([npay:u8] then per-column [type][payload]) -- the SAME format the
 * vtab xUpdate path writes via xs_rec_encode, so a native insert and a
 * VDBE insert are byte-identical.  Returns the record length or -1. */
static int
encode_payload(const vx_cell_t *cells, int npay, uint8_t *out, int cap)
{
	int off = 0, i, b;
	if (npay < 0 || npay > 255 || cap < 1) return -1;
	out[off++] = (uint8_t)npay;
	for (i = 0; i < npay; i++) {
		const vx_cell_t *c = &cells[i];
		if (c->type == VX_NULL) {
			if (off + 1 > cap) return -1;
			out[off++] = 0;
		} else if (c->type == VX_INT) {
			int64_t x = c->i;
			if (off + 9 > cap) return -1;
			out[off++] = 1;
			for (b = 0; b < 8; b++) out[off++] = (uint8_t)(x >> (8 * b));
		} else if (c->type == VX_REAL) {
			uint64_t bits; memcpy(&bits, &c->r, 8);
			if (off + 9 > cap) return -1;
			out[off++] = 2;
			for (b = 0; b < 8; b++) out[off++] = (uint8_t)(bits >> (8 * b));
		} else {
			int n = c->nbytes;
			if (n < 0) n = 0;
			if (off + 5 + n > cap) return -1;
			out[off++] = (c->type == VX_TEXT) ? 3 : 4;
			out[off++] = (uint8_t)(n & 0xff);
			out[off++] = (uint8_t)((n >> 8) & 0xff);
			out[off++] = (uint8_t)((n >> 16) & 0xff);
			out[off++] = (uint8_t)((n >> 24) & 0xff);
			if (n) memcpy(out + off, c->bytes, (size_t)n);
			off += n;
		}
	}
	return off;
}

/* Native payload-record buffer cap.  Matches xstore's row payload limit
 * (XS_VMAX, 4096): the engine truncates a longer payload, so a record
 * never exceeds this, and a SET / VALUES literal that would not fit is
 * rejected by encode_payload (-> VDBE fallback) rather than truncated. */
#define XS_REC_MAX  4096

/* Upper bound on rows a single native DELETE / UPDATE will touch.  A
 * range matching more than this falls back to the VDBE rather than run
 * unbounded work (and an unbounded native buffer) on one statement. */
#define XS_NATIVE_MAX_ROWS  100000

/* Does `e` name the primary-key column (unqualified or table-qualified)? */
static int
is_pk_col(const sql_expr_t *e, const char *pkname)
{
	return e != NULL && e->op == SX_E_COLUMN && e->nname >= 1 &&
	    e->name[e->nname - 1].len == strlen(pkname) &&
	    strncmp(e->name[e->nname - 1].p, pkname, strlen(pkname)) == 0;
}

/* Evaluate a literal expression to an integer, or fail. */
static int
lit_int(const sql_expr_t *e, int64_t *out, const vx_cell_t *binds, int nbinds)
{
	struct vx_arena_blk *tmp = NULL;
	vx_cell_t c;
	int ok = 0;
	if (lit_cell(&tmp, e, &c, binds, nbinds) == 0 && c.type == VX_INT) {
		*out = c.i; ok = 1;
	}
	arena_free(tmp);
	return ok;
}

/* Recognize a WHERE clause the native write path can turn into an
 * inclusive rowid range over the primary key: no WHERE (whole table),
 * pk = / pk < / pk <= / pk > / pk >= an int literal, or pk BETWEEN
 * <int> AND <int>.  Returns 1 with lo / hi set to the inclusive bound
 * and has_lo / has_hi marking which bounds are present; 0 otherwise
 * (the statement goes to the VDBE).  An equality yields lo == hi.  This
 * is a SUPERSET-safe recognizer: any predicate
 * it does not understand falls back. */
static int
where_pk_range(const sql_expr_t *w, const char *pkname,
               int64_t *lo, int *has_lo, int64_t *hi, int *has_hi,
               const vx_cell_t *binds, int nbinds)
{
	*has_lo = *has_hi = 0;
	*lo = 0; *hi = 0;
	if (w == NULL) return 1;   /* no WHERE: the whole table */

	if (w->op == SX_E_BETWEEN) {
		int64_t b, c;
		if (!is_pk_col(w->a, pkname)) return 0;
		if (!lit_int(w->b, &b, binds, nbinds) || !lit_int(w->c, &c, binds, nbinds)) return 0;
		*lo = b; *has_lo = 1; *hi = c; *has_hi = 1;
		return 1;
	}
	if (w->op == SX_E_BINARY && w->a && w->b) {
		const sql_expr_t *col, *lit;
		int op = w->op2, swapped = 0;
		int64_t v;
		if (is_pk_col(w->a, pkname)) { col = w->a; lit = w->b; }
		else if (is_pk_col(w->b, pkname)) { col = w->b; lit = w->a; swapped = 1; }
		else return 0;
		(void)col;
		if (!lit_int(lit, &v, binds, nbinds)) return 0;
		/* Normalize the operator if the column was on the right. */
		if (swapped) {
			switch (op) {
			case TK_LT: op = TK_GT; break;  case TK_GT: op = TK_LT; break;
			case TK_LE: op = TK_GE; break;  case TK_GE: op = TK_LE; break;
			default: break;   /* EQ unchanged */
			}
		}
		switch (op) {
		case TK_EQ: *lo = v; *has_lo = 1; *hi = v; *has_hi = 1; return 1;
		case TK_LT: *hi = v - 1; *has_hi = 1; return 1;
		case TK_LE: *hi = v;     *has_hi = 1; return 1;
		case TK_GT: *lo = v + 1; *has_lo = 1; return 1;
		case TK_GE: *lo = v;     *has_lo = 1; return 1;
		default: return 0;
		}
	}
	return 0;
}

/* Collect up to `cap` rowids visible in the pk range [lo,hi] (bounds per
 * has_lo/has_hi) into `out`.  Returns the count, or -1 if more than
 * `cap` rows match (the caller falls back so a huge range does not run
 * unbounded natively).  -2 on a scan error. */
static int
collect_rowids(bt_t *bt, const char *table, int64_t lo, int has_lo,
               int64_t hi, int has_hi, int64_t *out, int cap)
{
	xstore_scan_t *s;
	int64_t rid; const uint8_t *rec; int reclen, n = 0, step;
	s = xstore_scan_open(bt, table, 0, lo, has_lo, hi, has_hi);
	if (s == NULL) return -2;
	while ((step = xstore_scan_next(s, &rid, &rec, &reclen)) == 1) {
		if (n >= cap) { xstore_scan_close(s); return -1; }
		out[n++] = rid;
	}
	xstore_scan_close(s);
	return step < 0 ? -2 : n;
}

/* Collect rowids matching an arbitrary WHERE predicate, by compiling the
 * predicate with vexec's own expression compiler and scanning the whole
 * table evaluating it per row -- the general (non-pk) DELETE/UPDATE
 * filter path.  Returns the count into `out` (<= cap), -1 if more than
 * cap rows match OR the predicate is not vexec-compilable (clean VDBE
 * fallback), or -2 on a scan/setup error.  The affinity gate inside the
 * compiler guarantees the predicate is evaluated with SQLite semantics
 * or rejected.  rowid_out, when non-NULL, also captures whether a row
 * existed at all (unused here; the scan only yields live rows). */
static int
collect_matching(sqlite3 *db, bt_t *bt, const char *table,
                 const sql_expr_t *where, int64_t *out, int cap,
                 const vx_cell_t *binds, int nbinds)
{
	struct vx_stmt *st;
	struct vx_compiler comp;
	struct namevec nv;
	xstore_scan_t *s = NULL;
	struct vx_arena_blk *rowarena = NULL;
	int64_t rid; const uint8_t *rec; int reclen, step;
	int n = 0, ret;

	if (where == NULL) return -1;   /* no WHERE -> the pk-range path handles it */

	st = (struct vx_stmt *)calloc(1, sizeof *st);
	if (st == NULL) return -2;
	st->db = db;
	st->bt = bt;
	st->snap = 0;
	snprintf(st->table, sizeof st->table, "%s", table);

	memset(&nv, 0, sizeof nv);
	comp.st = st; comp.nv = &nv; comp.jc = NULL; comp.fail = 0;
	comp.binds = binds; comp.nbinds = nbinds;

	/* Pass 1: collect the predicate's columns. */
	if (collect_columns(&nv, where) != 0 || nv.n == 0 || nv.n > 32) {
		ret = -1; goto out;
	}
	st->nsrc_col = nv.n;
	if (resolve_schema(db, table, &nv, st->src_pay) != 0) { ret = -1; goto out; }

	/* Pass 2: compile the predicate with the affinity gate active. */
	comp.fail = 0;
	st->filter = compile_expr(&comp, where);
	if (comp.fail || st->filter == NULL) { ret = -1; goto out; }

	st->srcrow = (vx_cell_t *)calloc((size_t)nv.n, sizeof(vx_cell_t));
	if (st->srcrow == NULL) { ret = -2; goto out; }

	s = xstore_scan_open(bt, table, 0, 0, 0, 0, 0);   /* whole table */
	if (s == NULL) { ret = -2; goto out; }
	while ((step = xstore_scan_next(s, &rid, &rec, &reclen)) == 1) {
		int b;
		arena_free(rowarena); rowarena = NULL;   /* per-row scratch */
		read_rec_row(st, rid, rec, reclen, &rowarena);
		b = eval_bool(st, st->filter, st->srcrow, &rowarena);
		if (b == 1) {
			if (n >= cap) { ret = -1; goto out; }   /* too many -> VDBE */
			out[n++] = rid;
		}
	}
	ret = (step < 0) ? -2 : n;

out:
	if (s) xstore_scan_close(s);
	arena_free(rowarena);
	if (st->plan_arena) arena_free(st->plan_arena);
	free(st->srcrow);
	free(st);
	return ret;
}

/* Read the current visible payload record of one rowid into buf (latest
 * snapshot).  Returns the record length (>=1) if the row exists, 0 if it
 * does not (or is tombstoned), or -1 on error. */
static int
read_one_row(bt_t *bt, const char *table, int64_t rowid, uint8_t *buf, int cap)
{
	xstore_scan_t *s;
	int64_t rid; const uint8_t *rec; int reclen, got = 0;

	s = xstore_scan_open(bt, table, 0, rowid, 1, rowid, 1);
	if (s == NULL) return -1;
	if (xstore_scan_next(s, &rid, &rec, &reclen) == 1 && rid == rowid) {
		if (reclen > cap) { xstore_scan_close(s); return -1; }
		memcpy(buf, rec, (size_t)reclen);
		got = reclen;
	}
	xstore_scan_close(s);
	return got;
}

int
vx_run_write(sqlite3 *db, const char *sql, int64_t *nchanges, char **errmsg)
{
	return vx_run_write_p(db, sql, NULL, 0, nchanges, errmsg);
}

int
vx_run_write_p(sqlite3 *db, const char *sql,
               const vx_cell_t *binds, int nbinds,
               int64_t *nchanges, char **errmsg)
{
	sql_arena_t *arena = NULL;
	sql_stmt_t *root = NULL;
	const char *perr = NULL;
	sql_insert_t *ins = NULL;
	const sql_str_t *tname = NULL;
	char tabbuf[64];
	bt_t *bt;
	uint32_t tableid;
	struct wschema ws;
	struct vx_arena_blk *cell_arena = NULL;
	int rc = 0, r, applied = 0;
	int64_t maxr;

	if (nchanges) *nchanges = 0;
	if (errmsg) *errmsg = NULL;

	if (sql_parse_ast(sql, strlen(sql), &arena, &root, &perr) != 0)
		return 0;                         /* unparseable -> VDBE */
	/* Exactly one write statement (INSERT / DELETE / UPDATE). */
	if (root == NULL || root->next != NULL) { sql_arena_destroy(arena); return 0; }
	switch (root->kind) {
	case SQL_KIND_INSERT:
		ins = root->u.insert;
		if (ins == NULL || ins->def_values) { sql_arena_destroy(arena); return 0; }
		/* Either VALUES rows or a SELECT source, not neither. */
		if (ins->select == NULL && (ins->rows == NULL || ins->n_rows < 1)) {
			sql_arena_destroy(arena); return 0; }
		tname = &ins->table;
		break;
	case SQL_KIND_DELETE:
		if (root->u.del == NULL) { sql_arena_destroy(arena); return 0; }
		tname = &root->u.del->table;
		break;
	case SQL_KIND_UPDATE:
		if (root->u.update == NULL || root->u.update->sets == NULL) {
			sql_arena_destroy(arena); return 0; }
		tname = &root->u.update->table;
		break;
	default:
		sql_arena_destroy(arena); return 0;
	}
	if (tname->len == 0 || tname->len >= sizeof tabbuf) {
		sql_arena_destroy(arena); return 0;
	}
	memcpy(tabbuf, tname->p, tname->len);
	tabbuf[tname->len] = '\0';

	bt = xstore_bt_of(db);
	if (bt == NULL || !xstore_table_id(bt, tabbuf, &tableid)) {
		sql_arena_destroy(arena); return 0;   /* not an xstore table -> VDBE */
	}
	if (load_wschema(db, tabbuf, &ws) != 0 || ws.pk_col != 0) {
		/* The native path assumes the PK is declared column 0 (xstore's
		 * convention); anything else -> VDBE. */
		sql_arena_destroy(arena); return 0;
	}

	/* DELETE / UPDATE inside an open transaction reads the rows to
	 * change.  The native read path scans the COMMITTED B-tree at the
	 * txn snapshot; it does not merge this transaction's own buffered
	 * writes (the wbuf).  That is safe -- and matches the vtab cursor,
	 * which also does not merge the wbuf into a RANGE scan -- EXCEPT
	 * when the txn has already written to this very table, where the
	 * vtab's point path (xs_filter / wbuf_find) could diverge.  So a
	 * transactional DELETE/UPDATE stays native while this table is
	 * clean in the txn, and falls back once the txn has buffered a
	 * write to it.  INSERT needs no such read and is always native. */
	if ((root->kind == SQL_KIND_DELETE || root->kind == SQL_KIND_UPDATE) &&
	    xstore_txn_table_dirty(db, tableid)) {
		sql_arena_destroy(arena); return 0;
	}

	/* ---- DELETE [WHERE pk-range or general predicate] ---------------- */
	if (root->kind == SQL_KIND_DELETE) {
		int64_t lo, hi; int has_lo, has_hi;
		int64_t *rids;
		int nr, i;
		rids = (int64_t *)malloc(sizeof(int64_t) * XS_NATIVE_MAX_ROWS);
		if (rids == NULL) { rc = -1; goto done; }
		if (where_pk_range(root->u.del->where, ws.name[0],
		                   &lo, &has_lo, &hi, &has_hi, binds, nbinds)) {
			nr = collect_rowids(bt, tabbuf, lo, has_lo, hi, has_hi,
			                    rids, XS_NATIVE_MAX_ROWS);
		} else {
			/* General predicate: compile + scan-evaluate per row. */
			nr = collect_matching(db, bt, tabbuf, root->u.del->where,
			                      rids, XS_NATIVE_MAX_ROWS, binds, nbinds);
		}
		if (nr == -1) { free(rids); sql_arena_destroy(arena); return 0; }  /* too many / not compilable -> VDBE */
		if (nr < 0) { free(rids); rc = -1; goto done; }
		for (i = 0; i < nr; i++) {
			if (xstore_write_txn(db, tableid, rids[i], NULL, 0, 1) != 0) {
				free(rids);
				if (errmsg) *errmsg = strdup("native delete failed");
				rc = -1; goto done;
			}
		}
		free(rids);
		if (nchanges) *nchanges = nr;   /* SQLite changes(): rows deleted */
		rc = 1; goto done;
	}

	/* ---- UPDATE SET col=<lit>,... [WHERE pk-range] ------------------- */
	if (root->kind == SQL_KIND_UPDATE) {
		sql_update_t *up = root->u.update;
		sql_assign_t *a;
		int64_t lo, hi; int has_lo, has_hi;
		int nr, i, ci;
		/* Validate the SET list ONCE up front (the same literal columns
		 * apply to every matched row); record which payload column each
		 * SET targets and its new cell. */
		vx_cell_t setval[64]; int setcol[64], nset = 0;
		int64_t *rids;

		for (a = up->sets; a; a = a->next) {
			int col = -1, k;
			for (k = 0; k < ws.n; k++)
				if (a->col.len == strlen(ws.name[k]) &&
				    strncmp(a->col.p, ws.name[k], a->col.len) == 0) { col = k; break; }
			if (col <= 0) { sql_arena_destroy(arena); return 0; } /* unknown / pk -> VDBE */
			if (nset >= 64) { sql_arena_destroy(arena); return 0; }
			if (lit_cell(&cell_arena, a->val, &setval[nset], binds, nbinds) != 0) {
				sql_arena_destroy(arena); return 0;  /* non-literal SET -> VDBE */
			}
			setcol[nset] = col;   /* declared index (>=1) */
			nset++;
		}
		rids = (int64_t *)malloc(sizeof(int64_t) * XS_NATIVE_MAX_ROWS);
		if (rids == NULL) { rc = -1; goto done; }
		if (where_pk_range(up->where, ws.name[0], &lo, &has_lo, &hi, &has_hi, binds, nbinds)) {
			nr = collect_rowids(bt, tabbuf, lo, has_lo, hi, has_hi,
			                    rids, XS_NATIVE_MAX_ROWS);
		} else {
			nr = collect_matching(db, bt, tabbuf, up->where,
			                      rids, XS_NATIVE_MAX_ROWS, binds, nbinds);
		}
		if (nr == -1) { free(rids); sql_arena_destroy(arena); return 0; }  /* too many / not compilable -> VDBE */
		if (nr < 0) { free(rids); rc = -1; goto done; }
		for (i = 0; i < nr; i++) {
			uint8_t cur[XS_REC_MAX]; int curlen;
			vx_cell_t cells[64];
			uint8_t rec[XS_REC_MAX]; int reclen;
			curlen = read_one_row(bt, tabbuf, rids[i], cur, (int)sizeof cur);
			if (curlen < 0) { free(rids); rc = -1; goto done; }
			if (curlen == 0) continue;          /* vanished under us: skip */
			for (ci = 1; ci < ws.n; ci++) {
				int64_t iv; double rv; const uint8_t *pv; int pn;
				int cls = xstore_rec_col(cur, curlen, ci - 1, &iv, &rv, &pv, &pn);
				vx_cell_t *c = &cells[ci - 1];
				memset(c, 0, sizeof *c);
				switch (cls) {
				case XSTORE_C_INT:  c->type = VX_INT;  c->i = iv; break;
				case XSTORE_C_REAL: c->type = VX_REAL; c->r = rv; break;
				case XSTORE_C_TEXT: c->type = VX_TEXT; c->bytes = pv; c->nbytes = pn; break;
				case XSTORE_C_BLOB: c->type = VX_BLOB; c->bytes = pv; c->nbytes = pn; break;
				default:            c->type = VX_NULL; break;
				}
			}
			for (ci = 0; ci < nset; ci++)
				cells[setcol[ci] - 1] = setval[ci];   /* overlay the SET values */
			reclen = encode_payload(cells, ws.n - 1, rec, (int)sizeof rec);
			if (reclen < 0) {
				/* Row too large to encode.  If nothing applied yet, fall
				 * back cleanly to the VDBE; otherwise it is too late to
				 * fall back (the VDBE would re-update applied rows and
				 * misreport changes()), so report an error. */
				if (applied == 0) { free(rids); sql_arena_destroy(arena); return 0; }
				free(rids);
				if (errmsg) *errmsg = strdup("native update row too large");
				rc = -1; goto done;
			}
			if (xstore_write_txn(db, tableid, rids[i], rec, reclen, 0) != 0) {
				free(rids);
				if (errmsg) *errmsg = strdup("native update failed");
				rc = -1; goto done;
			}
			applied++;
		}
		free(rids);
		if (nchanges) *nchanges = applied;
		rc = 1; goto done;
	}

	/* ---- INSERT INTO t [(col,...)] VALUES (...) ... ----------------- */
	maxr = xstore_max_rowid(bt, tableid);

	/* Build the column-position map.  With no explicit column list the
	 * VALUES tuples are positional (declared order, all columns).  With
	 * an explicit list (c1, c3, ...) each VALUES element maps to that
	 * column's DECLARED index; omitted columns are filled with NULL.
	 * colmap[v] = declared index of the v-th VALUES element; ncolmap =
	 * number of VALUES elements expected per row. */
	int colmap[64];
	int ncolmap;
	{
		int i;
		if (ins->cols == NULL) {
			/* Positional: element i -> declared column i. */
			ncolmap = ws.n;
			for (i = 0; i < ws.n && i < 64; i++) colmap[i] = i;
		} else {
			sql_exprlist_item_t *ci;
			ncolmap = 0;
			for (ci = ins->cols->head; ci; ci = ci->next) {
				const sql_expr_t *ce = ci->expr;
				int col = -1, k;
				const sql_str_t *nm;
				if (ce == NULL || ce->op != SX_E_COLUMN || ce->nname < 1) {
					sql_arena_destroy(arena); return 0;
				}
				nm = &ce->name[ce->nname - 1];
				for (k = 0; k < ws.n; k++)
					if (nm->len == strlen(ws.name[k]) &&
					    strncmp(nm->p, ws.name[k], nm->len) == 0) { col = k; break; }
				if (col < 0 || ncolmap >= 64) { sql_arena_destroy(arena); return 0; }
				colmap[ncolmap++] = col;
			}
			if (ncolmap == 0) { sql_arena_destroy(arena); return 0; }
		}
	}

	/* ---- INSERT INTO t [(cols)] SELECT ... --------------------------
	 * Run the source SELECT once via SQLite (the fallback / oracle
	 * engine; the SELECT's own tables may be xstore vtabs or anything
	 * SQLite knows), then insert each result row natively.  The SELECT
	 * must return exactly ncolmap columns (one per target slot).  An
	 * in-txn INSERT..SELECT, a parametrized SELECT, an arity mismatch,
	 * a non-integer PK, a duplicate PK, or a too-large row falls back
	 * (writing nothing first).  REPLACE..SELECT overwrites on conflict. */
	if (ins->select != NULL) {
		sqlite3_stmt *q = NULL;
		const char *ssrc = ins->select->src;
		int scol, step, applied2 = 0;
		struct { int64_t rowid; uint8_t rec[XS_REC_MAX]; int reclen; } *buf2 = NULL;
		int nb2 = 0, cap2 = 0;

		/* Read-your-writes: a SELECT run standalone via SQLite would not
		 * see this transaction's uncommitted buffered rows, so defer to
		 * the VDBE inside a txn. */
		if (xstore_in_txn(db)) { rc = 0; goto done; }
		if (ssrc == NULL) { rc = 0; goto done; }
		/* The SELECT is the trailing clause of the INSERT, so its text
		 * runs from its keyword to the end of the statement. */
		if (sqlite3_prepare_v2(db, ssrc, -1, &q, NULL) != SQLITE_OK) {
			rc = 0; goto done;
		}
		if (sqlite3_bind_parameter_count(q) != 0) {
			sqlite3_finalize(q); rc = 0; goto done;
		}
		scol = sqlite3_column_count(q);
		if (scol != ncolmap) {
			sqlite3_finalize(q); rc = 0; goto done;
		}

		while ((step = sqlite3_step(q)) == SQLITE_ROW) {
			vx_cell_t cells[64];
			int ci, reclen;
			int64_t rowid;
			for (ci = 0; ci < ws.n; ci++) { cells[ci].type = VX_NULL; cells[ci].nbytes = 0; }
			for (ci = 0; ci < scol; ci++) {
				vx_cell_t *dst = &cells[colmap[ci]];
				switch (sqlite3_column_type(q, ci)) {
				case SQLITE_INTEGER: dst->type = VX_INT; dst->i = sqlite3_column_int64(q, ci); break;
				case SQLITE_FLOAT:   dst->type = VX_REAL; dst->r = sqlite3_column_double(q, ci); break;
				case SQLITE_TEXT: case SQLITE_BLOB: {
					const void *b = sqlite3_column_blob(q, ci);
					int n = sqlite3_column_bytes(q, ci);
					uint8_t *pp = (uint8_t *)arena_alloc(&cell_arena, (size_t)n + 1);
					if (pp == NULL) { free(buf2); sqlite3_finalize(q); rc = -1; goto done; }
					if (n && b) memcpy(pp, b, (size_t)n);
					pp[n] = '\0';
					dst->type = (sqlite3_column_type(q, ci) == SQLITE_TEXT) ? VX_TEXT : VX_BLOB;
					dst->bytes = pp; dst->nbytes = n;
					break; }
				default: dst->type = VX_NULL; break;
				}
			}
			if (cells[0].type == VX_INT) {
				rowid = cells[0].i; if (rowid > maxr) maxr = rowid;
			} else if (cells[0].type == VX_NULL) {
				rowid = ++maxr;
			} else { free(buf2); sqlite3_finalize(q); rc = 0; goto done; }

			if (cells[0].type == VX_INT && !ins->replace) {
				uint8_t probe[XS_REC_MAX]; int pl, bi;
				pl = read_one_row(bt, tabbuf, rowid, probe, (int)sizeof probe);
				if (pl < 0) { free(buf2); sqlite3_finalize(q); rc = -1; goto done; }
				if (pl > 0) { free(buf2); sqlite3_finalize(q); rc = 0; goto done; }
				for (bi = 0; bi < nb2; bi++)
					if (buf2[bi].rowid == rowid) { free(buf2); sqlite3_finalize(q); rc = 0; goto done; }
			}

			if (nb2 == cap2) {
				int nc = cap2 ? cap2 * 2 : 64;
				void *nn = realloc(buf2, sizeof *buf2 * (size_t)nc);
				if (nn == NULL) { free(buf2); sqlite3_finalize(q); rc = -1; goto done; }
				buf2 = nn; cap2 = nc;
			}
			if (nb2 >= XS_NATIVE_MAX_ROWS) { free(buf2); sqlite3_finalize(q); rc = 0; goto done; }
			reclen = encode_payload(&cells[1], ws.n - 1, buf2[nb2].rec, (int)sizeof buf2[nb2].rec);
			if (reclen < 0) { free(buf2); sqlite3_finalize(q); rc = 0; goto done; }
			buf2[nb2].rowid = rowid; buf2[nb2].reclen = reclen; nb2++;
		}
		if (step != SQLITE_DONE) { free(buf2); sqlite3_finalize(q); rc = -1; goto done; }
		sqlite3_finalize(q);

		for (r = 0; r < nb2; r++) {
			if (xstore_write_txn(db, tableid, buf2[r].rowid,
			                     buf2[r].rec, buf2[r].reclen, 0) != 0) {
				free(buf2);
				if (errmsg) *errmsg = strdup("native insert-select failed");
				rc = -1; goto done;
			}
			applied2++;
		}
		free(buf2);
		if (nchanges) *nchanges = applied2;
		rc = 1; goto done;
	}

	/* Two passes: validate + buffer EVERY row first (no writes); only if
	 * all rows are literal-only with the right arity and an integer/NULL
	 * PK do we apply.  Any unsupported row -> return 0 with zero writes,
	 * a clean VDBE fallback (never a partial native apply). */
	{
		struct { int64_t rowid; uint8_t rec[XS_REC_MAX]; int reclen; } *buf;
		int nb = 0;

		buf = (void *)calloc((size_t)ins->n_rows, sizeof *buf);
		if (buf == NULL) { rc = -1; goto done; }

		for (r = 0; r < ins->n_rows; r++) {
			sql_exprlist_t *row = ins->rows[r];
			vx_cell_t cells[64];
			int reclen, vidx = 0, ci;
			int64_t rowid;
			sql_exprlist_item_t *it;

			/* Start every declared column NULL (omitted columns stay NULL),
			 * then place each VALUES element at its mapped declared index. */
			for (ci = 0; ci < ws.n; ci++) { cells[ci].type = VX_NULL; cells[ci].nbytes = 0; }
			for (it = row ? row->head : NULL; it; it = it->next) {
				if (vidx >= ncolmap) { vidx = -1; break; }   /* too many values */
				if (lit_cell(&cell_arena, it->expr, &cells[colmap[vidx]],
				             binds, nbinds) != 0) {
					vidx = -1; break;             /* non-literal -> VDBE */
				}
				vidx++;
			}
			if (vidx != ncolmap) { free(buf); rc = 0; goto done; }  /* arity / non-lit */

			if (cells[0].type == VX_INT) {
				rowid = cells[0].i;
				if (rowid > maxr) maxr = rowid;
			} else if (cells[0].type == VX_NULL) {
				rowid = ++maxr;
			} else { free(buf); rc = 0; goto done; }   /* non-int PK -> VDBE */

			/* PRIMARY KEY uniqueness: a plain INSERT onto an existing
			 * rowid is a constraint violation in SQLite (REPLACE would
			 * overwrite -- handled separately).  When the target PK
			 * already exists (committed B-tree) or repeats within THIS
			 * statement, fall back to the VDBE, which produces the
			 * canonical UNIQUE-constraint error -- the native path has
			 * written nothing yet, so the fallback is clean.  An explicit
			 * PK inside an open transaction may also collide with an
			 * uncommitted buffered row the committed scan cannot see, so
			 * that case also falls back.  An autovalued (NULL) PK never
			 * collides. */
			if (cells[0].type == VX_INT) {
				uint8_t probe[XS_REC_MAX]; int pl, bi;
				if (ins->replace) {
					/* REPLACE deliberately overwrites a conflicting PK; no
					 * uniqueness check.  An intra-statement repeat keeps the
					 * last row, which xstore's put-by-rowid achieves since
					 * the rows apply in order. */
				} else if (xstore_in_txn(db)) {
					free(buf); rc = 0; goto done;
				} else {
					pl = read_one_row(bt, tabbuf, rowid, probe, (int)sizeof probe);
					if (pl < 0) { free(buf); rc = -1; goto done; }
					if (pl > 0) { free(buf); rc = 0; goto done; }
					for (bi = 0; bi < nb; bi++)
						if (buf[bi].rowid == rowid) {
							free(buf); rc = 0; goto done;
						}
				}
			}

			reclen = encode_payload(&cells[1], ws.n - 1, buf[nb].rec,
			                        (int)sizeof buf[nb].rec);
			if (reclen < 0) { free(buf); rc = 0; goto done; }   /* too big -> VDBE */
			buf[nb].rowid = rowid;
			buf[nb].reclen = reclen;
			nb++;
		}

		/* All rows validated: apply.  xstore_write_txn buffers into the
		 * open transaction (atomic at COMMIT) or autocommits each row. */
		for (r = 0; r < nb; r++) {
			if (xstore_write_txn(db, tableid, buf[r].rowid,
			                     buf[r].rec, buf[r].reclen, 0) != 0) {
				/* A put failed after some applied: report an error rather
				 * than fall back (the VDBE would re-insert the applied
				 * rows).  Storage-level failure, not a recognition miss. */
				free(buf);
				if (errmsg) *errmsg = strdup("native insert failed");
				rc = -1; goto done;
			}
			applied++;
		}
		free(buf);
	}

	if (nchanges) *nchanges = applied;
	rc = 1;

done:
	arena_free(cell_arena);
	sql_arena_destroy(arena);
	return rc;
}


/* Cheap case-insensitive test for a set-operation keyword (UNION /
 * INTERSECT / EXCEPT) in the SQL, so the set-op runner is only invoked
 * (with its parse) when one is plausibly present.  A false positive
 * costs one extra parse that then falls back; never a correctness
 * issue. */
static int
ci_word(const char *s, const char *w)
{
	size_t n = strlen(w), i;
	for (; s && *s; s++) {
		for (i = 0; i < n; i++) {
			char c = s[i];
			if (c >= 'a' && c <= 'z') c -= 32;
			if (c != w[i]) break;
		}
		if (i == n) return 1;
	}
	return 0;
}

static int
sql_has_setop(const char *s)
{
	return ci_word(s, "UNION") || ci_word(s, "INTERSECT") || ci_word(s, "EXCEPT");
}

/* Run a compound SELECT (set operation).  Handles UNION ALL: recognize
 * and run each side independently, then concatenate the results (both
 * sides must have the same column count, as SQL requires).  Returns 1
 * with *res, 0 to fall back to the VDBE (either side unrecognized, a
 * non-UNION-ALL operator, mismatched arity, or a chained compound), or
 * <0 on error.  UNION / INTERSECT / EXCEPT (which dedup / intersect)
 * fall back for now. */
static int
run_setop(sqlite3 *db, const char *sql, const vx_cell_t *binds, int nbinds,
          vx_result_t **res, char **errmsg)
{
	sql_arena_t *ast = NULL;
	sql_stmt_t *root = NULL;
	const char *perr = NULL;
	sql_select_t *lhs, *rhs;
	sql_setop_t op;
	vx_stmt_t *lp = NULL, *rp = NULL;
	vx_result_t *lr = NULL, *rr = NULL;
	int rc = 0, i, j;

	if (sql_parse_ast(sql, strlen(sql), &ast, &root, &perr) != 0) return 0;
	if (root == NULL || root->next != NULL ||
	    root->kind != SQL_KIND_SELECT || root->u.select == NULL) {
		sql_arena_destroy(ast); return 0;
	}
	lhs = root->u.select;
	if (lhs->setop == SX_SET_NONE || lhs->rhs == NULL) {
		sql_arena_destroy(ast); return 0;   /* not a set operation */
	}
	if (lhs->setop != SX_SET_UNION_ALL && lhs->setop != SX_SET_UNION &&
	    lhs->setop != SX_SET_INTERSECT && lhs->setop != SX_SET_EXCEPT) {
		sql_arena_destroy(ast); return 0;
	}
	rhs = lhs->rhs;
	if (rhs->setop != SX_SET_NONE) { sql_arena_destroy(ast); return 0; }  /* chained */
	/* An ORDER BY / LIMIT on the compound applies to the whole result;
	 * not handled here -- fall back. */
	if (lhs->order || lhs->limit || lhs->offset) { sql_arena_destroy(ast); return 0; }

	/* Recognize each side WITHOUT giving away the shared arena (pass
	 * NULL so vx_prepare_select does not free it; run_setop owns it). */
	if (vx_prepare_select(db, NULL, lhs, binds, nbinds, &lp, errmsg) != 1) {
		sql_arena_destroy(ast); return 0;
	}
	if (vx_prepare_select(db, NULL, rhs, binds, nbinds, &rp, errmsg) != 1) {
		vx_finalize(lp); sql_arena_destroy(ast); return 0;
	}
	/* SQL requires equal arity on both sides of a compound. */
	if (lp->nout != rp->nout) {
		vx_finalize(lp); vx_finalize(rp); sql_arena_destroy(ast); return 0;
	}

	/* The plans no longer reference the AST; capture the operator, then
	 * free it before executing (lhs points into the arena). */
	op = lhs->setop;
	sql_arena_destroy(ast); ast = NULL;

	if (collect_serial(lp, &lr) != 1) { rc = -1; goto out; }
	if (collect_serial(rp, &rr) != 1) { rc = -1; goto out; }
	lp = rp = NULL;   /* collect_serial finalized them */

	if (op == SX_SET_UNION_ALL || op == SX_SET_UNION) {
		/* Concatenate rr onto lr (order preserved). */
		for (i = 0; i < rr->nrow; i++) {
			vx_cell_t rowbuf[64];
			for (j = 0; j < rr->ncol; j++)
				rowbuf[j] = rr->cells[(size_t)i * (size_t)rr->ncol + (size_t)j];
			if (result_push(lr, rowbuf) != 0) { rc = -1; goto out; }
		}
		if (op == SX_SET_UNION && result_dedup(lr) != 0) { rc = -1; goto out; }
	} else if (op == SX_SET_INTERSECT) {
		/* Keep distinct left rows that also appear in the right. */
		struct vx_result *outr = (struct vx_result *)calloc(1, sizeof *outr);
		struct row_index seen; int k;
		if (outr == NULL) { rc = -1; goto out; }
		outr->ncol = lr->ncol;
		memcpy(outr->name, lr->name, sizeof outr->name);
		memset(&seen, 0, sizeof seen);
		for (i = 0; i < lr->nrow; i++) {
			const vx_cell_t *row = &lr->cells[(size_t)i * (size_t)lr->ncol];
			uint64_t h = row_hash(row, lr->ncol);
			int inr = 0;
			if (row_index_has(&seen, outr, row, h)) continue;   /* already emitted */
			for (k = 0; k < rr->nrow; k++)
				if (row_cells_eq(row, &rr->cells[(size_t)k * (size_t)rr->ncol],
				                 lr->ncol)) { inr = 1; break; }
			if (!inr) continue;
			if (result_push(outr, row) != 0 ||
			    row_index_add(&seen, outr->nrow - 1, h) != 0) {
				row_index_free(&seen); vx_result_free(outr); rc = -1; goto out;
			}
		}
		row_index_free(&seen);
		vx_result_free(lr); lr = outr;
	} else {   /* SX_SET_EXCEPT */
		/* Keep distinct left rows that do NOT appear in the right. */
		struct vx_result *outr = (struct vx_result *)calloc(1, sizeof *outr);
		struct row_index seen; int k;
		if (outr == NULL) { rc = -1; goto out; }
		outr->ncol = lr->ncol;
		memcpy(outr->name, lr->name, sizeof outr->name);
		memset(&seen, 0, sizeof seen);
		for (i = 0; i < lr->nrow; i++) {
			const vx_cell_t *row = &lr->cells[(size_t)i * (size_t)lr->ncol];
			uint64_t h = row_hash(row, lr->ncol);
			int inr = 0;
			if (row_index_has(&seen, outr, row, h)) continue;
			for (k = 0; k < rr->nrow; k++)
				if (row_cells_eq(row, &rr->cells[(size_t)k * (size_t)rr->ncol],
				                 lr->ncol)) { inr = 1; break; }
			if (inr) continue;
			if (result_push(outr, row) != 0 ||
			    row_index_add(&seen, outr->nrow - 1, h) != 0) {
				row_index_free(&seen); vx_result_free(outr); rc = -1; goto out;
			}
		}
		row_index_free(&seen);
		vx_result_free(lr); lr = outr;
	}
	*res = lr; lr = NULL;
	rc = 1;

out:
	if (lp) vx_finalize(lp);
	if (rp) vx_finalize(rp);
	if (lr) vx_result_free(lr);
	if (rr) vx_result_free(rr);
	if (ast) sql_arena_destroy(ast);
	return rc;
}

int
vx_run(sqlite3 *db, const char *sql, int n_workers,
       vx_result_t **res, char **errmsg)
{
	return vx_run_p(db, sql, NULL, 0, n_workers, res, errmsg);
}

int
vx_run_p(sqlite3 *db, const char *sql, const vx_cell_t *binds, int nbinds,
         int n_workers, vx_result_t **res, char **errmsg)
{
	vx_stmt_t *plan = NULL;
	int recog;

	if (res) *res = NULL;
	if (errmsg) *errmsg = NULL;
	if (n_workers < 1) n_workers = 1;
	/* A parametrized query runs serial: the morsel-parallel path
	 * re-prepares a range-scoped source per worker, which does not carry
	 * the binds, so force one worker when binds are present. */
	if (binds != NULL && nbinds > 0) n_workers = 1;

	/* A compound (set-op) SELECT is composed of independent sub-selects;
	 * the set-op runner recognizes and concatenates them (UNION ALL).
	 * Only probe when the text plausibly contains a UNION, so the common
	 * path does not pay an extra parse. */
	if (sql_has_setop(sql)) {
		int sr = run_setop(db, sql, binds, nbinds, res, errmsg);
		if (sr != 0) return sr;   /* 1 handled, <0 error */
	}

	/* Recognize once, then route: the morsel-parallel storage scan is the
	 * committed path for parallelizable single-table scans/aggregations;
	 * other recognized plans (ordered, limited, joined, or run at one
	 * worker) collect from the serial vectorized path; anything not
	 * recognized returns 0 so the caller runs the VDBE. */
	recog = vx_try_prepare_binds(db, sql, binds, nbinds, &plan, errmsg);
	if (recog != 1) return recog;

	if (n_workers > 1 && is_parallelizable_plan(plan))
		return run_parallel_plan(plan, db, n_workers, res, errmsg);
	return collect_serial(plan, res);
}

/* ---- result accessors -------------------------------------------- */

static const vx_cell_t *
res_cell(const vx_result_t *r, int row, int col)
{
	if (r == NULL || row < 0 || row >= r->nrow || col < 0 || col >= r->ncol)
		return NULL;
	return &r->cells[(size_t)row * (size_t)r->ncol + (size_t)col];
}

int vx_result_nrow(const vx_result_t *r) { return r ? r->nrow : 0; }
int vx_result_ncol(const vx_result_t *r) { return r ? r->ncol : 0; }
int vx_result_nworkers(const vx_result_t *r) { return r ? r->nworkers : 0; }

const char *
vx_result_name(const vx_result_t *r, int col)
{
	if (r == NULL || col < 0 || col >= r->ncol) return NULL;
	return r->name[col][0] ? r->name[col] : NULL;   /* NULL => use VDBE name */
}

vx_type_t vx_result_type(const vx_result_t *r, int row, int col)
{ const vx_cell_t *c = res_cell(r, row, col); return c ? c->type : VX_NULL; }

int64_t vx_result_int64(const vx_result_t *r, int row, int col)
{
	const vx_cell_t *c = res_cell(r, row, col);
	if (!c) return 0;
	if (c->type == VX_INT) return c->i;
	if (c->type == VX_REAL) return (int64_t)c->r;
	return 0;
}

double vx_result_double(const vx_result_t *r, int row, int col)
{
	const vx_cell_t *c = res_cell(r, row, col);
	if (!c) return 0;
	if (c->type == VX_REAL) return c->r;
	if (c->type == VX_INT) return (double)c->i;
	return 0;
}

const char *vx_result_text(const vx_result_t *r, int row, int col)
{
	const vx_cell_t *c = res_cell(r, row, col);
	return (c && (c->type == VX_TEXT || c->type == VX_BLOB))
	    ? (const char *)c->bytes : NULL;
}

int vx_result_bytes(const vx_result_t *r, int row, int col)
{
	const vx_cell_t *c = res_cell(r, row, col);
	return (c && (c->type == VX_TEXT || c->type == VX_BLOB)) ? (int)c->nbytes : 0;
}

void
vx_result_free(vx_result_t *r)
{
	if (r == NULL) return;
	arena_free(r->arena);
	free(r->cells);
	free(r);
}
