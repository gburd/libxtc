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
} vx_outcol_t;

typedef struct vx_aggplan {
	int          ngrp;       /* number of GROUP BY key expressions */
	vx_expr_t  **grp;        /* ngrp key expressions */
	int          nout;       /* output columns */
	vx_outcol_t *out;        /* nout output column descriptors */
	int          nagg;       /* number of aggregate output columns */
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

/* ---- the vexec statement ----------------------------------------- */

struct vx_stmt {
	sqlite3      *db;
	sqlite3_stmt *src;          /* SELECT <base cols> FROM <table> */
	int           nsrc_col;

	int           nout;
	vx_expr_t   **proj;         /* nout projection expressions (non-agg path) */
	vx_expr_t    *filter;       /* WHERE expression, or NULL */

	vx_aggplan_t *agg;          /* non-NULL => aggregating statement (V3) */

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

struct vx_compiler {
	struct vx_stmt *st;
	struct namevec *nv;
	int             fail;
};

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
		return c->nv->aff[e->col];
	case VXO_NEG: case VXO_BITNOT:
	case VXO_ADD: case VXO_SUB: case VXO_MUL: case VXO_DIV: case VXO_MOD:
		return VX_AFF_NUMERIC;
	case VXO_CONCAT:
		return VX_AFF_TEXT;
	case VXO_NOT: case VXO_AND: case VXO_OR:
	case VXO_EQ: case VXO_NE: case VXO_LT: case VXO_LE: case VXO_GT: case VXO_GE:
	case VXO_ISNULL: case VXO_NOTNULL:
		return VX_AFF_NUMERIC;   /* booleans are 0/1 integers */
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
	case SX_E_NULL: case SX_E_NUMBER: case SX_E_STRING:
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

static vx_expr_t *
compile_expr(struct vx_compiler *c, const sql_expr_t *e)
{
	if (c->fail || e == NULL) { c->fail = 1; return NULL; }

	switch (e->op) {
	case SX_E_NULL: case SX_E_NUMBER: case SX_E_STRING:
		return compile_literal(c, e);

	case SX_E_COLUMN: {
		const sql_str_t *cn;
		vx_expr_t *n;
		int idx;
		if (e->nname < 1 || e->nname > 2) { c->fail = 1; return NULL; }
		cn = &e->name[e->nname - 1];
		idx = nv_add(c->nv, cn);
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
                              vx_stmt_t **out, char **errmsg);

int
vx_try_prepare(sqlite3 *db, const char *sql, vx_stmt_t **out, char **errmsg)
{
	sql_arena_t *ast = NULL;
	sql_stmt_t  *root = NULL;
	const char  *perr = NULL;
	const sql_select_t *sel;
	const sql_src_t *src;
	struct namevec nv;
	struct vx_compiler comp;
	struct vx_stmt *st = NULL;
	const sql_exprlist_item_t *it;
	char tabbuf[64];
	char *srcsql = NULL;
	int i, nproj = 0, rc = 0, proj_star = 0;

	if (out) *out = NULL;
	if (errmsg) *errmsg = NULL;

	if (sql_parse_ast(sql, strlen(sql), &ast, &root, &perr) != 0)
		goto fallback;
	if (root == NULL || root->next != NULL) goto fallback;
	if (root->kind != SQL_KIND_SELECT || root->explain) goto fallback;
	sel = root->u.select;
	if (sel == NULL) goto fallback;

	/* Common gates: no compound/CTE/DISTINCT/HAVING.  GROUP BY is
	 * allowed only on the aggregation path; ORDER BY / LIMIT / OFFSET
	 * are allowed only on the non-aggregating path (handled below). */
	if (sel->with || sel->setop != SX_SET_NONE || sel->distinct ||
	    sel->having)
		goto fallback;

	/* FROM exactly one base table. */
	src = sel->from;
	if (src == NULL || src->next != NULL || src->subquery != NULL) goto fallback;
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
			/* Hand off the AST (still alive) to the agg builder, which
			 * destroys it before returning. */
			return vx_try_prepare_agg(db, ast, sel, tabbuf, out, errmsg);
		}
	}
	if (sel->group) goto fallback;   /* defensive: GROUP only on agg path */

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

	memset(&nv, 0, sizeof nv);
	comp.st = st; comp.nv = &nv; comp.fail = 0;

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
		/* Verify the WHERE (if any) is V1-compilable in principle; its
		 * column names are resolved against the "SELECT *" source below. */
		struct namevec scratch; memset(&scratch, 0, sizeof scratch);
		if (sel->where && collect_columns(&scratch, sel->where) != 0)
			goto fallback;
	}

	/* Build the source SELECT. */
	if (proj_star) {
		size_t n = strlen("SELECT * FROM ") + strlen(tabbuf) + 1;
		srcsql = (char *)malloc(n);
		if (!srcsql) goto oom;
		snprintf(srcsql, n, "SELECT * FROM %s", tabbuf);
		snprintf(st->table, sizeof st->table, "%s", tabbuf);
		snprintf(st->srccols, sizeof st->srccols, "*");
	} else {
		char cols[2100]; int off = 0, k;
		if (nv.n == 0 || nv.n > 32) goto fallback;
		for (k = 0; k < nv.n; k++) {
			int r = snprintf(cols + off, sizeof cols - (size_t)off,
			                 "%s%s", k ? "," : "", nv.names[k]);
			if (r < 0 || (size_t)(off + r) >= sizeof cols) goto fallback;
			off += r;
		}
		{
			size_t n = strlen("SELECT  FROM ") + (size_t)off + strlen(tabbuf) + 1;
			srcsql = (char *)malloc(n);
			if (!srcsql) goto oom;
			snprintf(srcsql, n, "SELECT %s FROM %s", cols, tabbuf);
		}
		snprintf(st->table, sizeof st->table, "%s", tabbuf);
		snprintf(st->srccols, sizeof st->srccols, "%s", cols);
	}

	if (sqlite3_prepare_v2(db, srcsql, -1, &st->src, 0) != SQLITE_OK)
		goto fallback;   /* let the VDBE produce the authoritative error */
	st->nsrc_col = sqlite3_column_count(st->src);
	if (st->nsrc_col > 32) goto fallback;

	/* For "*", the source columns ARE the namevec, in order; record their
	 * names so the WHERE compiler can resolve column references. */
	if (proj_star) {
		for (i = 0; i < st->nsrc_col; i++) {
			const char *nm = sqlite3_column_name(st->src, i);
			if (nm == NULL || strlen(nm) >= 64) goto fallback;
			strcpy(nv.names[i], nm);
		}
		nv.n = st->nsrc_col;
		nproj = st->nsrc_col;
	} else if (st->nsrc_col != nv.n) {
		goto fallback;   /* defensive: column set mismatch */
	}

	st->nout = nproj;
	if (nproj > 32) goto fallback;   /* worker out[] / result row bound */
	st->proj = (vx_expr_t **)calloc((size_t)(nproj > 0 ? nproj : 1),
	                                sizeof(vx_expr_t *));
	if (!st->proj) goto oom;

	/* Fill real column affinities for the gate. */
	for (i = 0; i < st->nsrc_col; i++)
		nv.aff[i] = vx_affinity(sqlite3_column_decltype(st->src, i));

	/* Pass 2: compile the projection + filter with the affinity gate
	 * active.  Any affinity-ambiguous comparison/arithmetic fails here
	 * and the query falls back. */
	{
		int k = 0;
		comp.fail = 0;
		if (proj_star) {
			/* Identity projection: one column reference per source column. */
			for (k = 0; k < st->nsrc_col; k++) {
				vx_expr_t *n = expr_node(&comp, VXO_COL);
				if (n == NULL) goto fallback;
				n->col = k;
				st->proj[k] = n;
			}
		} else {
			for (it = sel->cols->head; it; it = it->next, k++) {
				st->proj[k] = compile_expr(&comp, it->expr);
				if (comp.fail) goto fallback;
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
	/* On the "SELECT *" path, a non-position ORDER BY key would need a
	 * column resolved by name against the source after prepare, which the
	 * key compiler (indexing nv) does not do; fall back conservatively
	 * (a bare position key is fine). */
	if (proj_star && sel->order) {
		const sql_exprlist_item_t *o;
		for (o = sel->order->head; o; o = o->next)
			if (!(o->expr && o->expr->op == SX_E_NUMBER)) goto fallback;
	}
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
	(void)perr;
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
	default: return 0;
	}
}

static int
vx_try_prepare_agg(sqlite3 *db, sql_arena_t *ast, const sql_select_t *sel,
                   const char *tabbuf, vx_stmt_t **out, char **errmsg)
{
	struct vx_stmt *st = NULL;
	struct namevec nv;
	struct vx_compiler comp;
	vx_aggplan_t *ap = NULL;
	const sql_exprlist_item_t *it;
	char *srcsql = NULL;
	int nout = 0, ngrp = 0, nagg = 0, i, k, rc = 0;

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
	ap = (vx_aggplan_t *)calloc(1, sizeof *ap);
	if (!ap) goto oom;
	ap->nout = nout;
	ap->ngrp = ngrp;
	ap->out = (vx_outcol_t *)calloc((size_t)nout, sizeof(vx_outcol_t));
	ap->grp = (vx_expr_t **)calloc((size_t)(ngrp > 0 ? ngrp : 1), sizeof(vx_expr_t *));
	if (!ap->out || !ap->grp) goto oom;
	st->agg = ap;

	memset(&nv, 0, sizeof nv);
	comp.st = st; comp.nv = &nv; comp.fail = 0;

	/* Pass 1: collect referenced base columns (group keys, agg args,
	 * WHERE), and classify each select item as key or aggregate. */
	for (it = sel->group ? sel->group->head : NULL; it; it = it->next)
		if (collect_columns(&nv, it->expr) != 0) goto fallback;
	{
		int oi = 0;
		for (it = sel->cols->head; it; it = it->next, oi++) {
			const sql_expr_t *e = it->expr;
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
	ap->nagg = nagg;
	if (sel->where && collect_columns(&nv, sel->where) != 0) goto fallback;

	/* Build the source SELECT over the collected base columns.  A pure
	 * count(*) references no columns; select _rowid_ so there is a row
	 * to count (and so the source has at least one column). */
	{
		char cols[2100]; int off = 0;
		int nocols = (nv.n == 0);
		if (nv.n > 32) goto fallback;
		if (nocols) {
			off = snprintf(cols, sizeof cols, "_rowid_");
		} else {
			for (k = 0; k < nv.n; k++) {
				int r = snprintf(cols + off, sizeof cols - (size_t)off,
				                 "%s%s", k ? "," : "", nv.names[k]);
				if (r < 0 || (size_t)(off + r) >= sizeof cols) goto fallback;
				off += r;
			}
		}
		{
			size_t n = strlen("SELECT  FROM ") + (size_t)off + strlen(tabbuf) + 1;
			srcsql = (char *)malloc(n);
			if (!srcsql) goto oom;
			snprintf(srcsql, n, "SELECT %s FROM %s", cols, tabbuf);
		}
		snprintf(st->table, sizeof st->table, "%s", tabbuf);
		snprintf(st->srccols, sizeof st->srccols, "%s", cols);
	}

	if (sqlite3_prepare_v2(db, srcsql, -1, &st->src, 0) != SQLITE_OK)
		goto fallback;
	st->nsrc_col = sqlite3_column_count(st->src);
	/* With no referenced columns the source has the synthetic _rowid_
	 * column (nsrc_col == 1, nv.n == 0); otherwise the counts match. */
	if (st->nsrc_col > 32) goto fallback;
	if (nv.n != 0 && st->nsrc_col != nv.n) goto fallback;

	for (i = 0; i < st->nsrc_col; i++)
		nv.aff[i] = vx_affinity(sqlite3_column_decltype(st->src, i));

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
	}
	if (sel->where) {
		st->filter = compile_expr(&comp, sel->where);
		if (comp.fail) goto fallback;
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

	while (c->nrow < c->cap) {
		int j;
		rc = sqlite3_step(st->src);
		if (rc == SQLITE_DONE) { *done = 1; break; }
		if (rc != SQLITE_ROW) { chunk_free(c); return NULL; }

		for (j = 0; j < st->nsrc_col; j++)
			read_src_cell(st->src, j, &st->srcrow[j], &c->arena);

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

	/* Drain. */
	for (;;) {
		int step = sqlite3_step(st->src);
		vx_cell_t keys[16];
		vx_grp_t *grp;
		int ai;
		if (step == SQLITE_DONE) break;
		if (step != SQLITE_ROW) { arena_free(rowarena); return -1; }

		for (j = 0; j < st->nsrc_col; j++)
			read_src_cell(st->src, j, &st->srcrow[j], &rowarena);

		if (st->filter != NULL &&
		    eval_bool(st, st->filter, st->srcrow, &rowarena) != 1)
			continue;

		for (j = 0; j < ap->ngrp; j++)
			eval(st, ap->grp[j], st->srcrow, &rowarena, &keys[j]);
		grp = htab_group(&st->ht, keys);
		if (grp == NULL) { arena_free(rowarena); return -1; }

		ai = 0;
		for (j = 0; j < ap->nout; j++) {
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
			int ki = 0, ai = 0;
			for (j = 0; j < ap->nout; j++) {
				if (ap->out[j].is_agg) {
					vx_cell_t fv;
					acc_final(&g->accs[ai], ap->out[j].kind, &fv);
					/* TEXT/BLOB extremes live in the ht arena, which
					 * outlives the chunk (freed in vx_finalize after the
					 * chunk); copy into the chunk arena for safety. */
					if ((fv.type == VX_TEXT || fv.type == VX_BLOB) && fv.nbytes) {
						(void)cell_dup(&c->arena, &fv, &dst[j]);
					} else dst[j] = fv;
					ai++;
				} else {
					vx_cell_t kv = g->keys[ki++];
					if ((kv.type == VX_TEXT || kv.type == VX_BLOB) && kv.nbytes)
						(void)cell_dup(&c->arena, &kv, &dst[j]);
					else dst[j] = kv;
				}
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
	       (st->norder > 0 || st->limit >= 0 || st->offset > 0);
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
		int step = sqlite3_step(st->src);
		vx_cell_t outrow[32], keyrow[16];
		if (step == SQLITE_DONE) break;
		if (step != SQLITE_ROW) goto cleanup;

		for (j = 0; j < st->nsrc_col; j++)
			read_src_cell(st->src, j, &st->srcrow[j], &rowarena);
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

	/* Apply OFFSET / LIMIT. */
	off = st->offset;
	lim = st->limit;   /* -1 = unbounded */
	if (off > nrow) off = nrow;
	emit_n = nrow - (int)off;
	if (lim >= 0 && emit_n > (int)lim) emit_n = (int)lim;

	c = (vx_chunk_t *)calloc(1, sizeof *c);
	if (!c) goto cleanup;
	c->ncol = st->nout;
	c->cap = emit_n > 0 ? emit_n : 1;
	c->cells = (vx_cell_t *)malloc(sizeof(vx_cell_t) * (size_t)c->cap * (size_t)(st->nout > 0 ? st->nout : 1));
	if (!c->cells) { free(c); c = NULL; goto cleanup; }
	for (e = 0; e < emit_n; e++) {
		int si = idx[(int)off + e];
		vx_cell_t *dst = &c->cells[(size_t)c->nrow * (size_t)c->ncol];
		for (j = 0; j < st->nout; j++) {
			vx_cell_t v = rows[(size_t)si * (size_t)st->nout + (size_t)j];
			if ((v.type == VX_TEXT || v.type == VX_BLOB) && v.nbytes)
				(void)cell_dup(&c->arena, &v, &dst[j]);
			else dst[j] = v;
		}
		c->nrow++;
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

int
vx_step(vx_stmt_t *st)
{
	if (st == NULL) return SQLITE_MISUSE;

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
		c = next_chunk(st, &done);
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
	if (st->chunk) chunk_free(st->chunk);
	if (st->plan_arena) arena_free(st->plan_arena);
	htab_free(&st->ht);
	if (st->agg) { free(st->agg->out); free(st->agg->grp); free(st->agg); }
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

/* Shared context across the morsel workers. */
struct vx_par {
	const struct vx_stmt *plan;   /* compiled template (proj/filter/cols/table) */
	const char  *db_path;
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
	sqlite3 *db = NULL;
	sqlite3_stmt *src = NULL;
	char sql[2400];
	struct vx_stmt w;     /* lightweight per-worker execution state */
	struct vx_arena_blk *rowarena = NULL;
	vx_cell_t *srcrow = NULL;
	int rc_ok = 0;

	(void)self;

	if (sqlite3_open_v2(par->db_path, &db,
	        SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
		goto done;

	/* Range-scoped source over rowid; reuse the plan's exact column list
	 * so the compiled proj/filter (which index source columns) match. */
	snprintf(sql, sizeof sql,
	    "SELECT %s FROM %s WHERE _rowid_ >= ?1 AND _rowid_ < ?2",
	    par->plan->srccols, par->plan->table);
	if (sqlite3_prepare_v2(db, sql, -1, &src, 0) != SQLITE_OK) goto done;

	/* The worker borrows the plan's compiled trees; its own scratch. */
	memset(&w, 0, sizeof w);
	w.nout = par->plan->nout;
	w.proj = par->plan->proj;
	w.filter = par->plan->filter;
	w.agg = par->plan->agg;   /* borrowed (immutable plan) */
	w.nsrc_col = sqlite3_column_count(src);
	srcrow = (vx_cell_t *)calloc((size_t)(w.nsrc_col > 0 ? w.nsrc_col : 1),
	                             sizeof(vx_cell_t));
	if (srcrow == NULL) goto done;
	w.srcrow = srcrow;

	for (;;) {
		int64_t lo = atomic_fetch_add_explicit(&par->cursor, par->morsel,
		                                       memory_order_relaxed);
		int64_t mh;
		if (lo >= par->hi) break;
		mh = lo + par->morsel;
		if (mh > par->hi) mh = par->hi;

		sqlite3_reset(src);
		sqlite3_bind_int64(src, 1, lo);
		sqlite3_bind_int64(src, 2, mh);

		for (;;) {
			int step = sqlite3_step(src), j;
			if (step == SQLITE_DONE) break;
			if (step != SQLITE_ROW) { atomic_store(&par->error, 1); goto done; }

			for (j = 0; j < w.nsrc_col; j++)
				read_src_cell(src, j, &w.srcrow[j], &rowarena);

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
				if (grp == NULL) { atomic_store(&par->error, 1); goto done; }
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
					atomic_store(&par->error, 1); goto done;
				}
			}
		}
		arena_free(rowarena); rowarena = NULL;
	}
	rc_ok = 1;

done:
	if (rowarena) arena_free(rowarena);
	free(srcrow);
	if (src) sqlite3_finalize(src);
	if (db) sqlite3_close(db);
	if (!rc_ok) atomic_store(&par->error, 1);
	return 0;   /* task DONE */
}

int
vx_run_parallel(const char *db_path, const char *sql, int n_workers,
                vx_result_t **res, char **errmsg)
{
	sqlite3 *coord = NULL;
	vx_stmt_t *plan = NULL;
	struct vx_par par;
	struct vx_result *out = NULL;
	xtc_exec_t *exec = NULL;
	struct { struct vx_par *par; int idx; } *args = NULL;
	int i, rc = 0, recog;
	int64_t lo = 0, hi = 0;

	if (res) *res = NULL;
	if (errmsg) *errmsg = NULL;
	if (n_workers < 1) n_workers = 1;

	/* Recognize the plan once on a coordinator connection. */
	if (sqlite3_open_v2(db_path, &coord, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK)
		return -1;
	recog = vx_try_prepare(coord, sql, &plan, errmsg);
	if (recog != 1) { sqlite3_close(coord); return recog; }   /* 0 fallback / <0 err */

	/* ORDER BY / LIMIT / OFFSET need a global sort across all workers'
	 * output; the parallel combine here only concatenates, so an ordered
	 * query is not run in parallel.  Signal not-recognized so the caller
	 * runs it on the single-threaded vexec path (which sorts) or the
	 * VDBE.  (A parallel merge-sort combine is a later refinement.) */
	if (plan->norder > 0 || plan->limit >= 0 || plan->offset > 0) {
		vx_finalize(plan); sqlite3_close(coord); return 0;
	}

	/* rowid bounds: [min, max] of the table (NULL table -> empty). */
	{
		char q[128]; sqlite3_stmt *b = NULL;
		snprintf(q, sizeof q, "SELECT min(_rowid_), max(_rowid_) FROM %s",
		         plan->table);
		if (sqlite3_prepare_v2(coord, q, -1, &b, 0) == SQLITE_OK &&
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

	memset(&par, 0, sizeof par);
	par.plan = plan;
	par.db_path = db_path;
	atomic_store(&par.cursor, lo);
	par.hi = hi;
	/* Morsel size: aim for a few morsels per worker so the atomic cursor
	 * load-balances; floor at 1. */
	{
		int64_t span = hi - lo;
		int64_t target = span / (int64_t)(n_workers * 4 > 0 ? n_workers * 4 : 1);
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
	if (coord) sqlite3_close(coord);
	return rc;
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
