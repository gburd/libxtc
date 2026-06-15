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

#include "vexec.h"
#include "sql_parse.h"
#include "sql_ast.h"
#include "sql_parse_gen.h"   /* TK_* token ids */

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

/* ---- the vexec statement ----------------------------------------- */

struct vx_stmt {
	sqlite3      *db;
	sqlite3_stmt *src;          /* SELECT <base cols> FROM <table> */
	int           nsrc_col;

	int           nout;
	vx_expr_t   **proj;         /* nout projection expressions */
	vx_expr_t    *filter;       /* WHERE expression, or NULL */

	struct vx_arena_blk *plan_arena;   /* expr tree + literal bytes */

	/* Per-row scratch: the source row materialized as cells. */
	vx_cell_t    *srcrow;       /* nsrc_col cells */

	vx_chunk_t   *chunk;
	int           cur;
	int           done;
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

/* ---- recognizer + plan build ------------------------------------- */

static void
chunk_free(vx_chunk_t *c)
{
	if (!c) return;
	arena_free(c->arena);
	free(c->cells);
	free(c);
}

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

	/* P1/P2: no compound/CTE/DISTINCT/GROUP/HAVING/ORDER/LIMIT/OFFSET. */
	if (sel->with || sel->setop != SX_SET_NONE || sel->distinct ||
	    sel->group || sel->having || sel->order || sel->limit || sel->offset)
		goto fallback;

	/* FROM exactly one base table. */
	src = sel->from;
	if (src == NULL || src->next != NULL || src->subquery != NULL) goto fallback;
	if (src->table.len == 0 || src->table.len >= sizeof tabbuf) goto fallback;
	memcpy(tabbuf, src->table.p, src->table.len);
	tabbuf[src->table.len] = '\0';

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

int
vx_step(vx_stmt_t *st)
{
	if (st == NULL) return SQLITE_MISUSE;

	if (st->chunk != NULL && st->cur + 1 < st->chunk->nrow) {
		st->cur++;
		return SQLITE_ROW;
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
	free(st->proj);
	free(st->srcrow);
	free(st);
}
