/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/vexec.c
 *	Vectorized execution engine -- V1.  See vexec.h and
 *	the design notes.
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
#include <strings.h>     /* strcasecmp: SQL identifiers are case-insensitive */
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdatomic.h>

#include "engine.h"      /* SX_* result codes */
#include "vexec.h"
#include "sql_parse.h"
#include "sql_ast.h"
#include "sql_parse_gen.h"   /* TK_* token ids */
#include "btree.h"           /* bt_t */
#include "xstore.h"          /* storage-native scan: the VDBE-free row source */
#include "vxfunc.h"          /* date/time, printf, math, ... (VXF_EXT) */
#include "qsel.h"            /* QuickSelect: the bounded ORDER BY ... LIMIT */

#include "xtc.h"             /* XTC_OK */
#include "xtc_exec.h"        /* V2: morsel-parallel workers, one per loop */

/* Native ASCII pattern matchers (no SQLite).  xs_like: '%' matches any
 * run, '_' any one byte, ASCII case-insensitive (SQLite's default LIKE).
 * xs_glob: '*' any run, '?' any one byte, '[set]' a class (with ranges
 * and a leading '^' negation), case-sensitive.  Both return 1 on a full
 * match.  Recursive on the wildcard, iterative otherwise -- patterns
 * here are short. */
static int
xs_like(const char *p, const char *s, char esc)
{
	while (*p) {
		int lit = 0;
		char a, b;
		if (esc != '\0' && *p == esc) {   /* ESCAPE: next char is literal */
			if (*++p == '\0') return 0;
			lit = 1;
		} else if (*p == '%') {
			while (*p == '%') p++;          /* collapse runs */
			if (*p == '\0') return 1;        /* trailing % matches rest */
			for (; *s; s++)
				if (xs_like(p, s, esc)) return 1;
			return 0;
		}
		if (*s == '\0') return 0;
		if (lit || *p != '_') {
			a = *p; b = *s;
			if (a >= 'A' && a <= 'Z') a += 32;
			if (b >= 'A' && b <= 'Z') b += 32;
			if (a != b) return 0;
		}
		p++; s++;
	}
	return *s == '\0';
}
static int
xs_glob(const char *p, const char *s)
{
	while (*p) {
		if (*p == '*') {
			while (*p == '*') p++;
			if (*p == '\0') return 1;
			for (; *s; s++)
				if (xs_glob(p, s)) return 1;
			return 0;
		}
		if (*s == '\0') return 0;
		if (*p == '?') { p++; s++; continue; }
		if (*p == '[') {
			const char *q = p + 1;
			int neg = 0, matched = 0;
			if (*q == '^') { neg = 1; q++; }
			for (; *q && *q != ']'; q++) {
				if (q[1] == '-' && q[2] && q[2] != ']') {
					if ((unsigned char)*s >= (unsigned char)q[0] &&
					    (unsigned char)*s <= (unsigned char)q[2]) matched = 1;
					q += 2;
				} else if (*q == *s) matched = 1;
			}
			if (*q != ']') return 0;          /* malformed class */
			if (matched == neg) return 0;
			p = q + 1; s++; continue;
		}
		if (*p != *s) return 0;
		p++; s++;
	}
	return *s == '\0';
}

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

/* Empty an arena but keep its newest block for reuse: a per-row scratch
 * arena otherwise costs a 64 KiB malloc + free for every row that has a
 * TEXT or BLOB column. */
static void
arena_reset(struct vx_arena_blk **head)
{
	if (*head == NULL) return;
	arena_free((*head)->next);
	(*head)->next = NULL;
	(*head)->used = 0;
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
	VXO_LIKE, VXO_GLOB,                  /* a LIKE/GLOB b: operand in .a, pattern in .b;
	                                      * LIKE ... ESCAPE: escape expr in args[0] */
	VXO_IS,                              /* a IS b: NULL-safe equality, never NULL */
	VXO_CORRSUBQ,                        /* correlated scalar subquery: the stmt
	                                      * st->corrsql[col] re-run per row via vx_run_p, binding
	                                      * ? from srcrow[bindcol[i]] */
	VXO_CORRIN,                          /* operand .a IN (correlated select):
	                                      * st->corrsql[col] re-run per row via vx_run_p (binds
	                                      * as VXO_CORRSUBQ), membership over its
	                                      * single-column rows; lit.i != 0 = NOT IN */
	VXO_EXISTS,                          /* EXISTS (correlated select): true iff
	                                      * st->corrsql[col] re-run per row yields
	                                      * >= 1 row; binds as VXO_CORRSUBQ */
	VXO_FUNC                             /* builtin function (.func) */
};

enum vx_func {
	VXF_ABS = 1, VXF_LENGTH, VXF_LOWER, VXF_UPPER, VXF_COALESCE, VXF_IFNULL,
	VXF_TYPEOF, VXF_SUBSTR, VXF_NULLIF, VXF_MIN, VXF_MAX, VXF_REPLACE,
	VXF_TRIM, VXF_LTRIM, VXF_RTRIM, VXF_INSTR, VXF_ROUND, VXF_CAST,
	VXF_EXT                 /* a vxfunc.c function: its id in .col */
};

typedef struct vx_expr {
	enum vx_op    op;
	int           col;        /* VXO_COL: source-column index;
	                           * VXO_CORRSUBQ/CORRIN: index into st->corrsql[] */
	vx_cell_t     lit;        /* VXO_LIT */
	enum vx_func  func;       /* VXO_FUNC */
	struct vx_expr *a, *b;    /* operands (a for unary; a,b for binary) */
	struct vx_expr **args;    /* VXO_FUNC argument list */
	int           nargs;      /* VXO_CORRSUBQ: number of correlated binds */
	int          *bindcol;    /* VXO_CORRSUBQ: srcrow index feeding each ? */
	/* VXO_IN / VXO_NOTIN fast membership: when every list element is a
	 * constant literal of one comparable class, the values are sorted
	 * once at compile time so eval does a binary search instead of an
	 * O(nargs) linear scan.  in_sorted == NULL means fall back to the
	 * linear path (mixed/non-literal elements). */
	vx_cell_t    *in_sorted;  /* nargs_sorted non-NULL elements, ascending */
	int           in_nsorted;
	int           in_has_null; /* a NULL element was present in the list */
	/* VXO_LIT compiled from a bound ? (VXO_PARAM in effect): pord is its
	 * ordinal (0 = a real literal); pcoerce the comparison-affinity
	 * coercion cmp_affinity_gate applied (0 none, 1 numeric, 2 text).
	 * A cached plan re-reads .lit from the new binds (vx_plan_rebind). */
	int           pord;
	int           pcoerce;
	struct vx_expr *pnext;    /* the plan's chain of such nodes */
	int           noaff;      /* VX_NOAFF_* when the node's affinity is not
	                           * what its op implies (+x; a subquery value) */
} vx_expr_t;

/* ---- aggregation (V3) -------------------------------------------- */

enum vx_agg_kind {
	VXA_COUNT_STAR = 1,   /* count(*) */
	VXA_COUNT,            /* count(expr): non-NULL inputs */
	VXA_COUNT_DISTINCT,   /* count(DISTINCT expr): distinct non-NULL inputs */
	VXA_SUM,              /* sum(expr): NULL if all inputs NULL */
	VXA_TOTAL,            /* total(expr): 0.0 if no rows; always REAL */
	VXA_AVG,              /* avg(expr): sum/count over non-NULL, REAL */
	VXA_MIN, VXA_MAX,     /* min/max(expr): ignore NULLs */
	VXA_GROUP_CONCAT      /* group_concat(expr [,sep]): non-NULL inputs
	                       * joined by sep (default ',') in scan order */
};

/* An open-chained hash set of distinct cell values, used by
 * count(DISTINCT).  Nodes and key bytes are allocated from the group
 * accumulator's htab arena (the same arena passed to acc_step), so the
 * set lives exactly as long as its group and needs no separate free. */
typedef struct vx_dnode {
	struct vx_dnode *next;
	uint64_t         hash;
	vx_cell_t        val;
} vx_dnode_t;

typedef struct vx_dset {
	vx_dnode_t **buckets;
	int          nbucket;
	int64_t      count;
} vx_dset_t;

/* A running accumulator for one aggregate within one group. */
typedef struct vx_acc {
	int64_t   cnt;        /* count of non-NULL inputs (all kinds) */
	int       seen;       /* 1 if any non-NULL input seen (min/max/sum) */
	int       is_real;    /* SUM/TOTAL/AVG: accumulator went real */
	int64_t   isum;       /* integer running sum */
	double    rsum;       /* real running sum */
	vx_cell_t ext;        /* MIN/MAX current extreme (when seen) */
	vx_dset_t *dset;      /* COUNT_DISTINCT: distinct-value set (lazy) */
	/* GROUP_CONCAT: growable text buffer in the group's htab arena.
	 * gc_len/gc_cap track its extent; gc_sep/gc_seplen hold the
	 * separator (default ',').  seen distinguishes the first element
	 * (no leading separator). */
	char     *gc_buf;
	uint32_t  gc_len;
	uint32_t  gc_cap;
	const char *gc_sep;
	uint32_t  gc_seplen;
} vx_acc_t;

/* One accumulator of an aggregate plan: an aggregate call over the
 * source row.  Identical calls (expr_same) share one accumulator. */
typedef struct vx_aggacc {
	enum vx_agg_kind kind;
	vx_expr_t       *arg;      /* input expr over the source row (NULL: count(*)) */
	const sql_expr_t *ast;     /* the call, to match repeats */
	const char      *gc_sep;   /* GROUP_CONCAT separator bytes (NULL = ',') */
	uint32_t         gc_seplen;
} vx_aggacc_t;

/* Max GROUP BY keys plus accumulators: the GROUP ROW, a namevec. */
#define VX_GROW_MAX 32

/*
 * An aggregate plan.  Each source row is reduced into its group (keys
 * grp[] and accumulators acc[], both over the SOURCE row).  Each group
 * then forms a GROUP ROW -- [key 0 .. ngrp-1 | acc final 0 .. nagg-1] --
 * and HAVING, the outputs and the ORDER-BY-only columns are ordinary
 * expressions over it.
 */
typedef struct vx_aggplan {
	int          ngrp;       /* GROUP BY keys */
	vx_expr_t  **grp;        /* ngrp key expressions over the source row */
	int          nagg;       /* accumulators */
	vx_aggacc_t *acc;        /* nagg accumulators */
	int          nout;       /* output columns */
	int          nemit;      /* nout + ORDER-BY-only columns */
	vx_expr_t  **proj;       /* nemit expressions over the group row */
	vx_expr_t   *having;     /* over the group row, or NULL */
	int          has_gc;     /* a group_concat (order-sensitive: serial only) */
	int          distinct;   /* SELECT DISTINCT over the outputs */
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

/* Column affinity class.  The compiler's comparison gates use three
 * buckets (BLOB / TEXT / NUMERIC); vx_affinity() also yields the two
 * SQLite store affinities INTEGER and REAL, which coerce_to_affinity uses
 * and node_class() folds back to NUMERIC. */
enum vx_aff { VX_AFF_BLOB = 0, VX_AFF_TEXT, VX_AFF_NUMERIC,
              VX_AFF_INTEGER, VX_AFF_REAL };

/* Materialized result of running a plan (also the row source when a
 * plan runs as a nested derived table / subquery).  Defined here so the
 * recursive-execution sites can read it directly. */
struct vx_result {
	int        nrow, ncol, cap;
	vx_cell_t *cells;
	struct vx_arena_blk *arena;
	int        nworkers;
	char       name[64][64];   /* output column names from the plan; an
	                            * empty entry means "use the VDBE name" */
	enum vx_aff aff[64];       /* per-column affinity class (from the plan) */
	unsigned char noaff[64];   /* VX_NOAFF_*: 0 the column has aff[] */
};

/*
 * A join side's row source.  When the table is xstore-backed (the
 * common case) the side is read NATIVELY via xstore_scan_* + the
 * payload map `pay` -- no SQLite cursor.  Otherwise (a plain SQLite
 * table, as the join oracle test uses) `native` is 0 and the side is
 * read through a prepared SELECT (the SQL is in the plan).  `pay[i]` is
 * column i's origin: -1 = rowid, else payload column index.
 */
typedef struct vx_jsrc {
	int   native;          /* 1: xstore scan; 0: SQLite cursor */
	char  table[64];       /* xstore table name (native only) */
	int   pay[16];         /* per-column payload map (native only) */
	enum vx_aff aff[16];   /* per-column affinity (native only) */
	int   ncol;
} vx_jsrc_t;

struct vx_stmt;   /* fwd */
static void decode_rec_cells(const int *pay, int ncol, int64_t rowid,
                             const uint8_t *rec, int reclen,
                             vx_cell_t *out, struct vx_arena_blk **arena);
static int jsrc_build(struct xsql *db, const char *table, char names[][64],
                      int ncol, vx_jsrc_t *js);

typedef struct vx_joinplan {
	char       build_sql[1100];   /* SELECT <build cols> FROM <build table> */
	char       probe_sql[1100];   /* SELECT <probe cols> FROM <probe table> */
	int        build_ncol;        /* columns the build source returns */
	int        probe_ncol;
	int        build_key;         /* build-side join col (index within build) */
	int        probe_key;         /* probe-side join col (index within probe) */
	int        join_kind;         /* SX_J_INNER / LEFT / RIGHT / FULL */
	vx_jsrc_t  build_src;         /* build side row source (native or SQLite) */
	vx_jsrc_t  probe_src;         /* probe side row source */
	/* In the combined row, build columns come first (offset 0), then
	 * probe columns (offset build_ncol).  proj/filter use those indices. */
} vx_joinplan_t;

typedef struct vx_jht vx_jht_t;   /* join build hash table (defined later) */
typedef struct vx_jrow vx_jrow_t;

/* ---- access path (sqlite-audit gap #4) ---------------------------- *
 *
 * How a single source's candidate rows are fetched: a full rowid scan,
 * a rowid range (an INTEGER PRIMARY KEY bound), or an index range (an
 * equality prefix plus an optional range on the next column, walked
 * through xstore_iscan: MVCC-visible versions incl. this txn's writes).
 * The bound values are compiled expressions evaluated when the path is
 * opened, so a path chosen once is re-openable (plan reuse, join inner
 * sides re-opened per outer row).  The full WHERE is always re-checked
 * on every row; a path only narrows which rows are looked at. */
#define VX_AP_EQMAX   16
#define VX_AP_TERMMAX 64

enum vx_ap_kind { VX_AP_SCAN = 0, VX_AP_ROWID, VX_AP_INDEX };

typedef struct vx_apath {
	enum vx_ap_kind kind;
	uint32_t        idx_id;               /* INDEX */
	int             neq;                  /* INDEX: equality prefix */
	const struct vx_expr *eq[VX_AP_EQMAX];
	const struct vx_expr *lo, *hi;        /* INDEX: range on column neq */
	int             lo_incl, hi_incl;
	int             nrt;                  /* ROWID: rowid op value terms */
	int             rtop[8];
	const struct vx_expr *rtv[8];
	int             reverse;              /* INDEX: walk descending */
	int             ordered;              /* walk yields the wanted order */
	int             rowmax;               /* SCAN: open as the one newest live
	                                       * rowid (MAX(rowid), no WHERE) */
} vx_apath_t;

/* An open row source over one table (src_open / src_next / src_close). */
typedef struct vx_src {
	xstore_scan_t  *scan;
	xstore_iscan_t *iscan;
	int             open;
	int             empty;    /* a bound is NULL: no row can match */
	int             ordered;  /* this walk yields the path's order */
} vx_src_t;

/* Max tables in a single native equi-join (t0 .. t[VX_JOIN_MAX-1]). */
#define VX_JOIN_MAX 6

/*
 * N-way INNER equi-join plan (3 .. VX_JOIN_MAX tables).  Side 0 is the
 * streamed (outer) table; sides 1 .. nside-1 are each hashed (build) on
 * their own join key.  A row is produced by streaming side 0 and, for
 * each build side in order, probing its hash with the value of the
 * combined row at probe_outidx[s] -- a column of an EARLIER side (0 ..
 * s-1) that this side's ON clause equates to its own key column
 * key_local[s].  The combined row is [side0][side1]...[sideN-1] at the
 * offsets base[].  INNER only (no NULL-extension), which is what makes
 * the streamed pipeline correct without per-side matched tracking.
 */
typedef struct vx_njoinplan {
	int   nside;
	char  src_sql[VX_JOIN_MAX][512];  /* SELECT <cols> FROM <table> per side */
	vx_jsrc_t src[VX_JOIN_MAX];       /* per-side row source (native or SQLite) */
	int   ncol[VX_JOIN_MAX];          /* columns each side returns */
	int   base[VX_JOIN_MAX];          /* combined-row offset of each side */
	/* For build side s (1 .. nside-1): probe its hash with the combined
	 * row's cell at probe_outidx[s]; the build row's key is its local
	 * column key_local[s]. */
	int   probe_outidx[VX_JOIN_MAX];  /* combined index of the probe value */
	int   key_local[VX_JOIN_MAX];     /* build-side local key column */
	int   cross[VX_JOIN_MAX];         /* side is a keyless CROSS/comma join:
	                                   * all rows stored under one sentinel
	                                   * key so every probe yields them all */
} vx_njoinplan_t;

/*
 * Index nested-loop INNER join (sqlite-audit gap #6): the sides are
 * walked in loop order ord[0] (outermost) .. ord[nside-1], each through
 * its own access path; an inner side's bounds read the columns of the
 * outer sides already placed in the combined row (ON c_w_id = w_id
 * seeks customer per warehouse row instead of hashing all of customer).
 * The WHERE AND every ON is re-checked on each complete combined row.
 * Chosen only when every inner side is an equality seek.
 */
typedef struct vx_nlplan {
	int        nside;
	char       table[VX_JOIN_MAX][64];
	int        pay[VX_JOIN_MAX][16];
	int        ncol[VX_JOIN_MAX];
	int        base[VX_JOIN_MAX];
	int        ord[VX_JOIN_MAX];
	vx_apath_t ap[VX_JOIN_MAX];
} vx_nlplan_t;

/* ---- the vexec statement ----------------------------------------- */

struct vx_stmt {
	struct xsql *db;
	int           nsrc_col;

	/* Storage-native source (single-table scan/agg paths).  bt != NULL
	 * marks the storage path: rows come from xstore_scan over `table`,
	 * and src_pay[i] is source column i's origin -- -1 = rowid (the
	 * INTEGER PRIMARY KEY), else the payload index in the record. */
	bt_t          *bt;
	xstore_scan_t *scan;
	int           src_pay[32];
	uint64_t      snap;         /* 0 = latest committed */

	/* Derived-table (FROM subquery) source: the inner SELECT is run once
	 * through vexec (vx_run) and its result materialized; rows are
	 * streamed from derived_res into srcrow (cursor derived_cur).
	 * derived_res != NULL marks this path (mutually exclusive with
	 * bt/scan). */
	vx_result_t  *derived_res;
	int           derived_cur;

	/* The chosen access path (ap_choose) and its open state: scan or
	 * iscan is the live cursor; src_empty when a bound evaluated to NULL
	 * (no row can match); src_ordered when the open walk yields the
	 * ORDER BY / MIN-MAX order (a fallback scan does not). */
	vx_apath_t     ap;
	vx_src_t       src;
	int            ap_minmax;   /* agg: all MIN (or all MAX) of one column:
	                             * on an ordered walk, the first row decides */

	/* Bound ? parameters for this statement (1-based), used by the
	 * compiler to turn SX_E_PARAM into a literal.  Borrowed (the caller's
	 * array outlives the prepare); NULL when there are no params. */
	const vx_cell_t *binds;
	int              nbinds;

	int           distinct;     /* SELECT DISTINCT: dedup the result */
	int64_t       now_ms;       /* 'now' for this statement (unix ms), 0 = unused */

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

	/* Per-output-column affinity class, computed at compile time (when the
	 * compiler -- and its nv->aff -- is alive).  Carried to vx_result so a
	 * caller running this plan as a nested subquery (recursive execution)
	 * can compile the OUTER query's comparison gate against these without
	 * a SQLite prepare.  nout entries. */
	enum vx_aff   outaff[64];
	unsigned char outnoaff[64];   /* VX_NOAFF_* per output column */

	vx_aggplan_t *agg;          /* non-NULL => aggregating statement (V3) */
	struct vx_stmt *agg_in;     /* agg over a join: the join, emitting the
	                             * combined row (identity projection) that
	                             * is this plan's source row */
	vx_joinplan_t *join;        /* non-NULL => two-table hash join (V5) */
	int           join_emit;    /* join: columns to emit into the chunk
	                             * (nout user cols + sort-only extras); 0
	                             * means "use nout". */

	/* ORDER BY / LIMIT (V4).  norder > 0 => ordered.  Each order key is
	 * either an expression over the SOURCE columns (order_key[i]), or a
	 * 1-based reference to an output column (order_outcol[i] > 0, key
	 * NULL).  order_desc[i]: bit0 = DESC, bit1 = NULLS FIRST/LAST
	 * overrides the default (NULL sorts first ASC, last DESC). */
	int           norder;
	vx_expr_t   **order_key;    /* norder expressions, or NULL entry */
	int          *order_outcol; /* norder: 1-based output col, or 0 */
	int          *order_desc;   /* norder: VX_ORD_* bits */
	int64_t       limit;        /* -1 = no LIMIT */
	int64_t       offset;       /* 0 if none */

	struct vx_arena_blk *plan_arena;   /* expr tree + literal bytes */
	vx_expr_t    *params;       /* chain of bound-? literals (pnext) */
	int           noreuse;      /* compile baked in data or a bind VALUE
	                             * (a spliced subquery, a view, an IN index
	                             * over ?): rebuild from the AST each run */
	struct vx_arena_blk *bind_arena;   /* rebound TEXT/BLOB bytes */

	/* Correlated-subquery sources (one per VXO_CORRSUBQ / VXO_CORRIN
	 * node).  Each is the parameterized inner SELECT text (outer refs
	 * rewritten to ? binds), run per row through vexec (vx_run_p) -- no
	 * SQLite.  The text is plan_arena-owned; the nodes reference it by
	 * index. */
	const char   *corrsql[8];
	int           corr_ncol[8];   /* result columns each must return (1) */
	int           ncorr;

	/* The recognized source, kept so a parallel run can rebuild a
	 * range-scoped source statement that reuses the SAME compiled
	 * plan (the proj/filter trees index the source columns, whose
	 * order is fixed by srccols). */
	char          table[64];
	char          corr_alias[64];   /* outer correlation name: the FROM
	                                 * alias if present, else the table name
	                                 * -- the qualifier a correlated
	                                 * subquery uses to reference this
	                                 * (single-table read path) row. */
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
	xstore_scan_t *probe_scan;  /* probe-side cursor (native source) */
	vx_jrow_t    *match;        /* current build match chain for the probe row */
	vx_cell_t    *probe_cells;  /* current probe row's cells */
	struct vx_arena_blk *probe_arena;  /* bytes for the current probe row */
	int           join_built;   /* 1 once build side hashed + probe opened */
	int           probe_done;   /* 1 once the probe cursor is drained */
	int           bscan_b;      /* unmatched-build final scan: bucket idx */
	vx_jrow_t    *bscan_r;      /* unmatched-build final scan: row cursor */

	/* Index nested-loop join (takes precedence over join / njoin). */
	vx_nlplan_t    *nl;
	vx_src_t        nlsrc[VX_JOIN_MAX];
	struct vx_arena_blk *nlar[VX_JOIN_MAX];
	int             nl_level, nl_started;

	/* N-way INNER join execution state (njoin path only). */
	vx_njoinplan_t *njoin;            /* non-NULL => N-way INNER join */
	vx_jht_t       *njht[VX_JOIN_MAX];/* per-build-side hash tables (1..) */
	xstore_scan_t  *nstream_scan;     /* side-0 stream cursor (native source) */
	vx_jrow_t      *ncur[VX_JOIN_MAX];/* current match row per build side */
	struct vx_arena_blk *nstream_arena;
	int             nbuilt;           /* 1 once hashes built + stream open */
	int             nstream_done;     /* 1 once the stream cursor is drained */
};

/* ---- recognizer: column affinity (SQLite rules, three buckets) --- */

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

/* SQLite's five affinity rules, in order (datatype3.html 3.1). */
static enum vx_aff
vx_affinity(const char *decltype)
{
	if (decltype == NULL || decltype[0] == '\0') return VX_AFF_BLOB;
	if (str_contains_ci(decltype, "INT")) return VX_AFF_INTEGER;
	if (str_contains_ci(decltype, "CHAR") ||
	    str_contains_ci(decltype, "CLOB") ||
	    str_contains_ci(decltype, "TEXT")) return VX_AFF_TEXT;
	if (str_contains_ci(decltype, "BLOB")) return VX_AFF_BLOB;
	if (str_contains_ci(decltype, "REAL") ||
	    str_contains_ci(decltype, "FLOA") ||
	    str_contains_ci(decltype, "DOUB")) return VX_AFF_REAL;
	return VX_AFF_NUMERIC;
}

/* The comparison-gate class of an affinity (INTEGER/REAL are numeric). */
static enum vx_aff
aff_class(enum vx_aff a)
{
	return (a == VX_AFF_INTEGER || a == VX_AFF_REAL) ? VX_AFF_NUMERIC : a;
}

/* ---- recognizer: a small column-name table ----------------------- */

/* A column's affinity for comparisons.  SQLite gives a column of a
 * derived table the affinity of its expression: a bare column keeps the
 * column's, CAST has its type's, anything else (arithmetic, a function,
 * an aggregate, a literal, +x, CASE) has none.  UNKNOWN marks a column
 * whose affinity vexec cannot state (a compound whose arms differ, a
 * scalar subquery value): a comparison that could convert declines. */
#define VX_NOAFF_NO      1   /* no affinity: never converts */
#define VX_NOAFF_UNKNOWN 2

struct namevec {
	char names[32][64];
	enum vx_aff aff[32];   /* filled after the source stmt is prepared */
	unsigned char noaff[32];   /* VX_NOAFF_*, 0 = has aff[] */
	int  n;
	/* Live resolution (the aggregate builder): when sc is set, a new name
	 * is resolved against that table schema as it is added -- pay[] gets
	 * its record origin, aff[] its affinity -- and a name the table does
	 * not have fails.  fixed: the names are a closed set (a materialized
	 * source); nothing can be added. */
	const xstore_col_t *sc;
	int  nsc;
	int *pay;
	int  fixed;
	/* Correlation (a single-table prepare's pass 1 only, else NULL):
	 * collect_columns also gathers the OUTER-column references inside a
	 * correlated subquery -- columns qualified by otab, the outer table
	 * name or alias -- so they get srcrow slots; bt is the connection's
	 * tree, for an EXISTS inner table's column names.  Per prepare, not
	 * file-scoped: see vx_prepare_select. */
	const char *otab;
	bt_t       *bt;
};

static int
nv_find(const struct namevec *nv, const sql_str_t *s)
{
	int i;
	for (i = 0; i < nv->n; i++)
		if ((int)strlen(nv->names[i]) == (int)s->len &&
		    strncasecmp(nv->names[i], s->p, s->len) == 0)
			return i;
	return -1;
}

static int
nv_add(struct namevec *nv, const sql_str_t *s)
{
	int i, c;
	if (s == NULL || s->len == 0 || s->len >= 64) return -1;
	if ((i = nv_find(nv, s)) >= 0) return i;
	if (nv->n >= 32 || nv->fixed) return -1;
	if (nv->sc != NULL) {
		/* Live resolution: the column's record origin and affinity now. */
		for (c = 0; c < nv->nsc; c++)
			if (strlen(nv->sc[c].name) == s->len &&
			    strncasecmp(nv->sc[c].name, s->p, s->len) == 0) break;
		if (c < nv->nsc) {
			nv->pay[nv->n] = nv->sc[c].is_pk ? -1 : c - 1;
			nv->aff[nv->n] = nv->sc[c].is_pk ? VX_AFF_NUMERIC
			    : aff_class(vx_affinity(nv->sc[c].decltype));
		} else if ((s->len == 5 && strncasecmp(s->p, "rowid", 5) == 0) ||
		    (s->len == 7 && strncasecmp(s->p, "_rowid_", 7) == 0) ||
		    (s->len == 3 && strncasecmp(s->p, "oid", 3) == 0)) {
			nv->pay[nv->n] = -1;
			nv->aff[nv->n] = VX_AFF_NUMERIC;
		} else
			return -1;   /* not a column of the table */
	}
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
resolve_schema(struct xsql *db, const char *table, struct namevec *nv, int *pay)
{
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
					if (strcasecmp(nv->names[i], cols[c].name) != 0) continue;
					pay[i] = cols[c].is_pk ? -1 : (c - 1);
					/* The PK is the INTEGER rowid regardless of its declared
					 * type, matching how SQLite reports an INTEGER PRIMARY
					 * KEY -- so its affinity is numeric, not the empty/BLOB
					 * default a bare "k" coldef would give. */
					nv->aff[i] = cols[c].is_pk ? VX_AFF_NUMERIC
					                          : aff_class(vx_affinity(cols[c].decltype));
					resolved++;
				}
			/* The special names rowid / _rowid_ / oid are aliases for the
			 * INTEGER rowid in BOTH explicit-PK and implicit-rowid tables
			 * (SQLite semantics), so resolve any still-unresolved such
			 * reference to the rowid (pay = -1). */
			for (i = 0; i < nv->n; i++) {
				if (pay[i] != -2) continue;
				if (strcasecmp(nv->names[i], "rowid") == 0 ||
				    strcasecmp(nv->names[i], "_rowid_") == 0 ||
				    strcasecmp(nv->names[i], "oid") == 0) {
					pay[i] = -1;
					nv->aff[i] = VX_AFF_NUMERIC;
					resolved++;
				}
			}
			for (i = 0; i < nv->n; i++) if (pay[i] == -2) ok = 0;
			return (ok && resolved >= nv->n) ? 0 : -1;
		}
	}

	/* No native catalog entry: this is not an xstore-backed table, so a
	 * storage-native scan of it is meaningless -- fall back to the VDBE
	 * rather than resolve a schema (via PRAGMA table_info / sqlite_master)
	 * for a table the native scan path cannot read anyway. */
	return -1;
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
	int       nside;          /* number of join sides (2 .. VX_JOIN_MAX) */
	char      tab[VX_JOIN_MAX][64];     /* base table name per side */
	char      alias[VX_JOIN_MAX][64];   /* alias per side ("" if none) */
	struct namevec col[VX_JOIN_MAX];    /* columns collected per side */
	int       base[VX_JOIN_MAX];        /* combined-row offset where each side starts */
	/* Each side's catalog schema (heap; jc_schema_load / _free), so an
	 * UNQUALIFIED column is recorded only on the side that has it. */
	xstore_col_t *sc[VX_JOIN_MAX];
	int       nsc[VX_JOIN_MAX];
	/* Collecting for an aggregate over the join: descend into aggregate
	 * calls, and let an unresolvable bare name be an output alias. */
	const sql_select_t *aggsel;
};

struct vx_compiler {
	struct vx_stmt *st;
	struct namevec *nv;
	struct vx_joinctx *jc;    /* non-NULL on the join path */
	int             fail;
	/* Bound parameters (1-based by ordinal).  A SX_E_PARAM compiles to a
	 * VXO_LIT holding binds[ordinal-1]; a ? past nbinds (or with binds ==
	 * NULL) was never bound and is NULL, as in SQLite. */
	const vx_cell_t *binds;
	int              nbinds;
	struct vx_aggctx *ag;     /* non-NULL while compiling an aggregate */
};

/*
 * Aggregate compile context.  In "group mode" (grouping == 1) an
 * expression compiles over the GROUP ROW [GROUP BY keys | accumulator
 * results]: an aggregate call becomes its accumulator's slot (created on
 * first use), a GROUP BY term or a grouped column its key's slot, and in
 * HAVING / ORDER BY an output alias its expression.  Accumulator
 * arguments and the keys themselves compile over the source row
 * (grouping == 0).
 */
#define AGX_SEL    0
#define AGX_HAVING 1
#define AGX_ORDER  2
struct vx_aggctx {
	const sql_select_t *sel;
	vx_aggplan_t *ap;
	int grouping;
	int ctx;                       /* AGX_*: the clause being compiled */
	int depth;                     /* output-alias expansion depth */
	const sql_expr_t *gast[16];    /* GROUP BY terms, positions/aliases resolved */
	enum vx_aff aff[VX_GROW_MAX];  /* group-row class per slot */
	unsigned char noaff[VX_GROW_MAX];
};

/* Identifier slice == C-string.  ASCII case-insensitive: SQLite resolves
 * table, alias and column names without regard to (ASCII) case. */
static int
str_eq_cstr(const sql_str_t *s, const char *cs)
{
	size_t n = strlen(cs);
	return (size_t)s->len == n && strncasecmp(s->p, cs, n) == 0;
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

	for (side = 0; side < jc->nside; side++) {
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

/* CAST target-type codes (SQLite affinity of the type name).  Stored in
 * the VXF_CAST node's .col.  Derived from the type name using the same
 * substring rules SQLite uses to assign column affinity. */
enum vx_cast_class {
	VX_CAST_BLOB = 0,   /* no numeric/text keyword -> BLOB affinity (no-op) */
	VX_CAST_TEXT,
	VX_CAST_INT,        /* INTEGER: truncate toward zero */
	VX_CAST_REAL,
	VX_CAST_NUMERIC     /* INT if integral, else REAL */
};

static enum vx_cast_class
cast_type_class(const sql_str_t *ty)
{
	char buf[64];
	uint32_t n = ty->len;
	if (ty->p == NULL || n == 0) return VX_CAST_BLOB;
	if (n >= sizeof buf) n = sizeof buf - 1;
	memcpy(buf, ty->p, n); buf[n] = '\0';
	/* SQLite type-affinity rules (from the type name), in order. */
	if (str_contains_ci(buf, "INT")) return VX_CAST_INT;
	if (str_contains_ci(buf, "CHAR") || str_contains_ci(buf, "CLOB") ||
	    str_contains_ci(buf, "TEXT")) return VX_CAST_TEXT;
	if (str_contains_ci(buf, "BLOB")) return VX_CAST_BLOB;
	if (str_contains_ci(buf, "REAL") || str_contains_ci(buf, "FLOA") ||
	    str_contains_ci(buf, "DOUB")) return VX_CAST_REAL;
	return VX_CAST_NUMERIC;   /* NUMERIC / DECIMAL / anything else */
}

/* A compiled node that is a literal NULL (VXO_LIT of type VX_NULL).
 * Such an operand makes arithmetic / comparison unconditionally NULL,
 * so it is always safe past the no-coercion affinity gate (the runtime
 * eval short-circuits a NULL operand to NULL). */
static int
is_null_lit(const vx_expr_t *e)
{
	return e != NULL && e->op == VXO_LIT && e->lit.type == VX_NULL;
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
		if (c->ag != NULL && c->ag->grouping) return c->ag->aff[e->col];
		if (c->jc != NULL) {
			/* Combined index -> (side, j) -> that side's affinity.  Find
			 * the side whose [base, base+col.n) range contains idx. */
			int idx = e->col, s;
			for (s = c->jc->nside - 1; s >= 0; s--)
				if (idx >= c->jc->base[s])
					return c->jc->col[s].aff[idx - c->jc->base[s]];
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
	case VXO_LIKE: case VXO_GLOB: case VXO_IS:
		return VX_AFF_NUMERIC;   /* match result is a 0/1 integer */
	case VXO_CASE:
		return VX_AFF_BLOB;      /* dynamic result type: treat as no affinity */
	case VXO_CORRSUBQ:
		return VX_AFF_BLOB;      /* dynamic subquery result */
	case VXO_CORRIN:
		return VX_AFF_NUMERIC;   /* membership result is a 0/1 integer */
	case VXO_EXISTS:
		return VX_AFF_NUMERIC;   /* EXISTS result is a 0/1 integer */
	case VXO_FUNC:
		switch (e->func) {
		case VXF_ABS:    return VX_AFF_NUMERIC;
		case VXF_LENGTH: return VX_AFF_NUMERIC;
		case VXF_INSTR:  return VX_AFF_NUMERIC;
		case VXF_ROUND:  return VX_AFF_NUMERIC;
		case VXF_LOWER: case VXF_UPPER: return VX_AFF_TEXT;
		case VXF_SUBSTR:         /* BLOB in -> BLOB out */
			return (e->nargs >= 1 && e->args[0] != NULL &&
			    node_class(c, e->args[0]) == VX_AFF_BLOB) ?
			    VX_AFF_BLOB : VX_AFF_TEXT;
		case VXF_REPLACE: return VX_AFF_TEXT;
		case VXF_TRIM: case VXF_LTRIM: case VXF_RTRIM: return VX_AFF_TEXT;
		case VXF_TYPEOF: return VX_AFF_TEXT;
		case VXF_EXT:
			switch (vxf_class_of(e->col)) {
			case VXF_C_TEXT: return VX_AFF_TEXT;
			case VXF_C_NUM:  return VX_AFF_NUMERIC;
			default:         return VX_AFF_BLOB;
			}
		case VXF_CAST:
			switch ((enum vx_cast_class)e->col) {
			case VX_CAST_TEXT: return VX_AFF_TEXT;
			case VX_CAST_INT: case VX_CAST_REAL: case VX_CAST_NUMERIC:
				return VX_AFF_NUMERIC;
			default: return VX_AFF_BLOB;
			}
		default:         return VX_AFF_BLOB;   /* coalesce/ifnull/min/max/nullif: mixed */
		}
	}
	return VX_AFF_BLOB;
}

/* VX_NOAFF_* for a compiled expression as a result column, by SQLite's
 * rule: a column reference keeps its column's affinity (or lack of one),
 * CAST has its target type's, a bound/scalar-subquery value is unknown
 * to vexec, and every other expression has none. */
static int
expr_noaff(const struct vx_compiler *c, const vx_expr_t *e)
{
	if (e->noaff) return e->noaff;
	switch (e->op) {
	case VXO_COL:
		if (c->ag != NULL && c->ag->grouping) return c->ag->noaff[e->col];
		if (c->jc != NULL) return 0;
		return c->nv ? c->nv->noaff[e->col] : 0;
	case VXO_FUNC:
		return e->func == VXF_CAST ? 0 : VX_NOAFF_NO;
	case VXO_CORRSUBQ:
		return VX_NOAFF_UNKNOWN;
	default:
		return VX_NOAFF_NO;
	}
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
	       name_is(name, "min") || name_is(name, "max") ||
	       name_is(name, "group_concat");
}

/* Is expression `e` an aggregate function CALL (not just a name match)?
 * min()/max() are both an aggregate (1 arg, over rows) and a scalar
 * (>= 2 args); disambiguate by arg count exactly as SQLite does.  Every
 * other aggregate name is an aggregate regardless of arg count. */
static int
is_agg_call(const sql_expr_t *e)
{
	int nargs = 0;
	const sql_exprlist_item_t *it;
	if (e == NULL || e->op != SX_E_FUNC) return 0;
	if (!is_agg_name(&e->name[0])) return 0;
	if (name_is(&e->name[0], "min") || name_is(&e->name[0], "max")) {
		for (it = e->list ? e->list->head : NULL; it; it = it->next) nargs++;
		return nargs <= 1;   /* >= 2 args is the scalar form */
	}
	return 1;
}

/* Does expression `e` contain an aggregate CALL anywhere in its tree?
 * Used to tell a plain grouping-key output from a computed expression
 * over aggregates (SUM(x)+1). */
static int
expr_has_agg(const sql_expr_t *e)
{
	const sql_exprlist_item_t *it;
	const sql_case_arm_t *arm;
	if (e == NULL) return 0;
	if (is_agg_call(e)) return 1;
	if (expr_has_agg(e->a) || expr_has_agg(e->b) || expr_has_agg(e->c) ||
	    expr_has_agg(e->els)) return 1;
	for (it = e->list ? e->list->head : NULL; it; it = it->next)
		if (expr_has_agg(it->expr)) return 1;
	for (arm = e->arms; arm; arm = arm->next)
		if (expr_has_agg(arm->when) || expr_has_agg(arm->then)) return 1;
	return 0;
}

/* The 2-argument like(P,X) / glob(P,X) scalar forms, which SQLite
 * defines as identical to "X LIKE P" / "X GLOB P".  Returns VXO_LIKE /
 * VXO_GLOB (a truthy op) for those names with exactly two args, else 0.
 * In practice only glob(P,X) reaches here as a call: LIKE is a reserved
 * keyword so like(...) is a parse error (infix "X LIKE P" is handled in
 * compile_expr).  The 3-arg like(P,X,E) (ESCAPE) is left to the VDBE. */
static enum vx_op
likeglob_func_op(const sql_str_t *name, int nargs)
{
	if (nargs != 2) return 0;
	if (name_is(name, "like")) return VXO_LIKE;
	if (name_is(name, "glob")) return VXO_GLOB;
	return 0;
}

static enum vx_func
func_of(const sql_str_t *name, int *nargs_ok, int nargs)
{
	struct { const char *n; enum vx_func f; int args; } tbl[] = {
		{ "abs", VXF_ABS, 1 }, { "length", VXF_LENGTH, 1 },
		{ "lower", VXF_LOWER, 1 }, { "upper", VXF_UPPER, 1 },
		{ "coalesce", VXF_COALESCE, -1 }, { "ifnull", VXF_IFNULL, 2 },
		{ "typeof", VXF_TYPEOF, 1 },
		{ "substr", VXF_SUBSTR, -1 }, { "substring", VXF_SUBSTR, -1 },
		{ "nullif", VXF_NULLIF, 2 },
		{ "min", VXF_MIN, -1 }, { "max", VXF_MAX, -1 },
		{ "replace", VXF_REPLACE, 3 },
		{ "trim", VXF_TRIM, -1 }, { "ltrim", VXF_LTRIM, -1 },
		{ "rtrim", VXF_RTRIM, -1 },
		{ "instr", VXF_INSTR, 2 }, { "round", VXF_ROUND, -1 }
	};
	size_t i;
	for (i = 0; i < sizeof tbl / sizeof tbl[0]; i++) {
		if (!name_is(name, tbl[i].n)) continue;
		/* Variadic-with-limits builtins: verify the arg count SQLite
		 * accepts (substr 2..3; min/max scalar >=2; trim family 1..2;
		 * round 1..2). */
		switch (tbl[i].f) {
		case VXF_SUBSTR:
			if (nargs != 2 && nargs != 3) { *nargs_ok = 0; return 0; }
			break;
		case VXF_MIN: case VXF_MAX:
			/* Scalar min/max requires >= 2 args (1 arg is the aggregate,
			 * handled elsewhere). */
			if (nargs < 2) { *nargs_ok = 0; return 0; }
			break;
		case VXF_TRIM: case VXF_LTRIM: case VXF_RTRIM:
		case VXF_ROUND:
			if (nargs != 1 && nargs != 2) { *nargs_ok = 0; return 0; }
			break;
		default:
			if (tbl[i].args >= 0 && tbl[i].args != nargs) {
				*nargs_ok = 0; return 0;
			}
			if (tbl[i].f == VXF_COALESCE && nargs < 1) {
				*nargs_ok = 0; return 0;
			}
			break;
		}
		*nargs_ok = 1;
		return tbl[i].f;
	}
	*nargs_ok = vxf_lookup(name->p, name->len, nargs) > 0;
	return *nargs_ok ? VXF_EXT : 0;
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
	case TK_IS: *out = VXO_IS; return 1;
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

/* An outer-column reference inside a correlated subquery (defined with
 * its collector below); forward-declared for collect_columns. */
struct corr_ref { const char *p; uint32_t len; const sql_str_t *col; };
#define VX_CORR_MAX 8
static int corr_collect(const sql_expr_t *e, const char *otab,
                        const char *const *inner, int ninner,
                        struct corr_ref *refs, int n);

static int
collect_columns(struct namevec *nv, const sql_expr_t *e)
{
	const sql_exprlist_item_t *it;
	if (e == NULL) return -1;
	switch (e->op) {
	case SX_E_NULL: case SX_E_NUMBER: case SX_E_STRING: case SX_E_PARAM:
	case SX_E_BLOB:
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
		if (e->op2 != TK_LIKE && !tok_to_binop(e->op2, &dummy)) return -1;
		if (collect_columns(nv, e->a) != 0) return -1;
		if (e->c != NULL && collect_columns(nv, e->c) != 0) return -1;   /* ESCAPE */
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
		/* An uncorrelated scalar subquery references no OUTER column.  A
		 * correlated one (single-table path, nv->otab set) references the
		 * outer table's columns -- collect those so they get srcrow slots;
		 * the compiler later rewrites them to ? binds. */
		if (nv->otab != NULL && e->sel != NULL) {
			struct corr_ref refs[VX_CORR_MAX];
			int nr = 0, i;
			const sql_select_t *q = e->sel;
			const sql_exprlist_item_t *it;
			for (it = q->cols ? q->cols->head : NULL; it; it = it->next)
				if ((nr = corr_collect(it->expr, nv->otab, NULL, 0, refs, nr)) < 0) return 0;
			if ((nr = corr_collect(q->where, nv->otab, NULL, 0, refs, nr)) < 0) return 0;
			if ((nr = corr_collect(q->having, nv->otab, NULL, 0, refs, nr)) < 0) return 0;
			for (i = 0; i < nr; i++)
				if (nv_add(nv, refs[i].col) < 0) return -1;
		}
		return 0;
	case SX_E_IN_SELECT:
		/* a IN (SELECT ...): collect the operand's columns.  An
		 * uncorrelated subquery contributes none (materialized as a
		 * literal list); a correlated one (nv->otab set) references the
		 * outer table's columns -- collect those so they get srcrow slots
		 * (the compiler later rewrites them to ? binds), exactly as the
		 * scalar-subquery case does.  EXISTS (a == NULL, ival bit1) has no
		 * operand -- only its correlated outer refs matter. */
		if (e->a != NULL && collect_columns(nv, e->a) != 0) return -1;
		if (nv->otab != NULL && e->sel != NULL) {
			struct corr_ref refs[VX_CORR_MAX];
			int nr = 0, i;
			const sql_select_t *q = e->sel;
			const sql_exprlist_item_t *it;
			const char *innames[64];
			const char *const *inner = NULL;
			xstore_col_t *icols = NULL;
			int inc = 0;
			int is_exists = (e->a == NULL && (e->ival & 2));
			/* For EXISTS, allow unqualified outer refs by excluding the
			 * inner table's own columns (mirrors compile_exists).  The
			 * schema buffer is heap-allocated -- collect_columns recurses
			 * over a small fiber stack, so a 64-entry stack array here
			 * risks overflow on a deep expression. */
			if (is_exists && nv->bt != NULL && q->from != NULL &&
			    q->from->next == NULL && q->from->subquery == NULL &&
			    q->from->table.len > 0 && q->from->table.len < 64) {
				char itab[64];
				icols = (xstore_col_t *)malloc(sizeof(xstore_col_t) * 64);
				if (icols == NULL) return -1;
				memcpy(itab, q->from->table.p, q->from->table.len);
				itab[q->from->table.len] = '\0';
				inc = xstore_table_schema(nv->bt, itab, icols, 64);
				for (i = 0; i < inc && i < 64; i++) innames[i] = icols[i].name;
				if (inc > 0) inner = innames;
			}
			for (it = q->cols ? q->cols->head : NULL; it; it = it->next)
				if ((nr = corr_collect(it->expr, nv->otab, inner, inc, refs, nr)) < 0) { free(icols); return 0; }
			if ((nr = corr_collect(q->where, nv->otab, inner, inc, refs, nr)) < 0) { free(icols); return 0; }
			if ((nr = corr_collect(q->having, nv->otab, inner, inc, refs, nr)) < 0) { free(icols); return 0; }
			free(icols);
			for (i = 0; i < nr; i++)
				if (nv_add(nv, refs[i].col) < 0) return -1;
		}
		return 0;
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
		/* CAST(expr AS type): name[0]="cast", name[1]=type -- accept. */
		if (e->nname == 2 && name_is(&e->name[0], "cast") && nargs == 1) {
			return collect_columns(nv, e->list->head->expr);
		}
		if (likeglob_func_op(&e->name[0], nargs) == 0) {
			(void)func_of(&e->name[0], &ok, nargs);
			if (!ok) return -1;
		}
		for (it = e->list ? e->list->head : NULL; it; it = it->next)
			if (collect_columns(nv, it->expr) != 0) return -1;
		return 0;
	}
	default:
		return -1;   /* BETWEEN/IN/CASE/subquery/param/bool/blob: not V1 */
	}
}

/* ORDER BY key flags (vx_stmt.order_desc[]). */
#define VX_ORD_DESC  1   /* DESC */
#define VX_ORD_NULLS 2   /* NULLS FIRST under DESC / NULLS LAST under ASC */

static int
ord_flags(const sql_exprlist_item_t *o)
{
	int desc = (o->sort == 2);
	int flip = (o->nulls == 1 && desc) || (o->nulls == 2 && !desc);
	return (desc ? VX_ORD_DESC : 0) | (flip ? VX_ORD_NULLS : 0);
}

/* If order-key expression `e` is a bare (unqualified) column reference
 * whose name matches a SELECT-list output ALIAS (SELECT x AS q ...
 * ORDER BY q), return that output column's 1-based position; else 0.
 * SQLite resolves an ORDER BY name against output aliases before base
 * columns.  Only AS-aliases are matched here (a plain column name is
 * left to normal source-column resolution). */
static int
order_key_alias_pos(const sql_select_t *sel, const sql_expr_t *e)
{
	const sql_exprlist_item_t *it;
	int pos = 0;
	if (e == NULL || e->op != SX_E_COLUMN || e->nname != 1) return 0;
	for (it = sel->cols ? sel->cols->head : NULL; it; it = it->next) {
		pos++;
		if (it->alias.len > 0 && it->alias.len == e->name[0].len &&
		    strncasecmp(it->alias.p, e->name[0].p, e->name[0].len) == 0)
			return pos;
	}
	return 0;
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

static int lit_cell(struct vx_arena_blk **arena, const sql_expr_t *e,
                    vx_cell_t *out, const vx_cell_t *binds, int nbinds);

/* Compile a literal AST node into a VXO_LIT (INT/REAL/TEXT/BLOB/NULL). */
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
	case SX_E_BLOB:   /* x'..': decoded into the plan arena */
		if (lit_cell(&c->st->plan_arena, e, &n->lit, NULL, 0) != 0) {
			c->fail = 1; return NULL;
		}
		return n;
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
	if (ord < 1) { c->fail = 1; return NULL; }
	n = expr_node(c, VXO_LIT);
	if (n == NULL) { c->fail = 1; return NULL; }
	n->pord = ord;
	n->pnext = c->st->params;
	c->st->params = n;
	if (c->binds == NULL || ord > c->nbinds) {
		n->lit.type = VX_NULL;   /* an unset ? is NULL (SQLite) */
		return n;
	}
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

/* Finish a VXF_EXT call node (args already compiled): record the
 * vxfunc id in .col, decline a 'localtime'/'utc' modifier, and stamp the
 * statement's 'now' once (SQLite: one time value per statement) when the
 * call can read the clock.  Returns 0, or -1 to decline. */
static int
compile_ext(struct vx_compiler *c, const sql_expr_t *e, vx_expr_t *n)
{
	const vx_cell_t *lits[16];
	vx_cell_t negs[16];
	const vx_expr_t *x;
	int k;
	n->col = vxf_lookup(e->name[0].p, e->name[0].len, n->nargs);
	if (n->col <= 0 || n->nargs > 16) return -1;
	for (k = 0; k < n->nargs; k++) {
		x = n->args[k];
		if (x->pord != 0) c->st->noreuse = 1;   /* value checked below */
		/* A column or expression of numeric class may hold a REAL. */
		lits[k] = x->op == VXO_LIT ? &x->lit : NULL;
		/* A negated numeric literal (-5, -1e300) is a constant too. */
		if (x->op == VXO_NEG && x->a->op == VXO_LIT) {
			if (x->a->pord != 0) c->st->noreuse = 1;
			negs[k] = x->a->lit;
			if (negs[k].type == VX_INT && negs[k].i != INT64_MIN) {
				negs[k].i = -negs[k].i;
				lits[k] = &negs[k];
			} else if (negs[k].type == VX_REAL) {
				negs[k].r = -negs[k].r;
				lits[k] = &negs[k];
			}
		}
		if (!vxf_arg_ok(n->col, k, lits[k],
		    node_class(c, n->args[k]) != VX_AFF_TEXT)) return -1;
	}
	if (vxf_unsupported_mod(n->col, lits, n->nargs)) return -1;
	if (vxf_wants_now(n->col, lits, n->nargs) && c->st->now_ms == 0)
		c->st->now_ms = vxf_now_ms();
	return 0;
}

static void *
ext_alloc(void *ctx, size_t n)
{
	return arena_alloc((struct vx_arena_blk **)ctx, n);
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

/* Walk an expression collecting whether any column reference is
 * qualified by a table/alias name NOT in `intab` (the inner FROM set):
 * such a reference is to an OUTER query, i.e. the subquery is
 * correlated.  Sets *corr = 1 on the first such reference. */
static void
corr_scan_expr(const sql_expr_t *e, char intab[][64], int nintab, int *corr)
{
	const sql_exprlist_item_t *it;
	const sql_case_arm_t *arm;
	if (e == NULL || *corr) return;
	if (e->op == SX_E_COLUMN && e->nname >= 2) {
		/* qualified: e->name[nname-2] is the table/alias qualifier */
		const sql_str_t *q = &e->name[e->nname - 2];
		int i, found = 0;
		for (i = 0; i < nintab; i++)
			if (q->len == strlen(intab[i]) &&
			    strncasecmp(q->p, intab[i], q->len) == 0) { found = 1; break; }
		if (!found) { *corr = 1; return; }
	}
	corr_scan_expr(e->a, intab, nintab, corr);
	corr_scan_expr(e->b, intab, nintab, corr);
	corr_scan_expr(e->c, intab, nintab, corr);
	corr_scan_expr(e->els, intab, nintab, corr);
	for (it = e->list ? e->list->head : NULL; it; it = it->next)
		corr_scan_expr(it->expr, intab, nintab, corr);
	for (arm = e->arms; arm; arm = arm->next) {
		corr_scan_expr(arm->when, intab, nintab, corr);
		corr_scan_expr(arm->then, intab, nintab, corr);
	}
	/* A nested subquery is checked by its own gate when compiled; here
	 * we only need to know whether THIS level references an outer name. */
}

/*
 * Native correlation/shape gate for an uncorrelated scalar/IN subquery,
 * replacing the SQLite prepare-as-gate so catalog-only tables work.
 * Parses the inner SELECT (src/srclen INCLUDE the wrapping parens),
 * and returns 1 iff it is a single non-compound SELECT, has no "?"
 * parameter, projects exactly `want_ncol` columns (no STAR), and is
 * UNCORRELATED -- every qualified column reference uses a table/alias
 * named in the inner FROM.  Returns 0 otherwise (the caller then treats
 * it as correlated / unsupported).
 */
/* The shape/correlation gate on an already-parsed subquery `q`: one
 * non-compound SELECT projecting want_ncol columns, with no qualified
 * reference to a table outside its own FROM.  1 = uncorrelated. */
static int
native_subquery_gate_sel(const sql_select_t *q, int want_ncol)
{
	char intab[8][64]; int nintab = 0, ncol = 0, corr = 0;
	const sql_src_t *fr;
	const sql_exprlist_item_t *it;
	const sql_exprlist_item_t *gi;
	if (q == NULL || q->setop != 0 || q->rhs != NULL) return 0;   /* compound */
	/* Inner FROM table names + aliases.  An aliased source contributes
	 * ONLY its alias (SQLite requires the alias for qualification; the
	 * base name then refers to an outer query -- the correlated case). */
	for (fr = q->from; fr; fr = fr->next) {
		if (fr->subquery) return 0;   /* nested derived: not gated here */
		if (fr->alias.len > 0 && fr->alias.len < 64 && nintab < 8) {
			memcpy(intab[nintab], fr->alias.p, fr->alias.len);
			intab[nintab][fr->alias.len] = '\0'; nintab++;
		} else if (fr->table.len > 0 && fr->table.len < 64 && nintab < 8) {
			memcpy(intab[nintab], fr->table.p, fr->table.len);
			intab[nintab][fr->table.len] = '\0'; nintab++;
		}
	}
	/* Output column count (no STAR). */
	for (it = q->cols ? q->cols->head : NULL; it; it = it->next) {
		if (it->expr == NULL || it->expr->op == SX_E_STAR) return 0;
		ncol++;
	}
	if (ncol != want_ncol) return 0;
	/* Correlation: any qualified ref to a non-inner table -> correlated. */
	for (it = q->cols->head; it; it = it->next)
		corr_scan_expr(it->expr, intab, nintab, &corr);
	corr_scan_expr(q->where, intab, nintab, &corr);
	corr_scan_expr(q->having, intab, nintab, &corr);
	for (gi = q->group ? q->group->head : NULL; gi; gi = gi->next)
		corr_scan_expr(gi->expr, intab, nintab, &corr);
	return !corr;
}

/* Walk a subquery expression collecting outer-column references: an
 * SX_E_COLUMN qualified by `otab` (the outer table name).  Appends to
 * refs[] (cap VX_CORR_MAX).  Returns the count, or -1 on overflow /
 * unsupported shape.  A column qualified by anything else is assumed to
 * belong to the subquery's own FROM (left alone).  When inner is
 * non-NULL (an EXISTS: the inner table's ninner column names), an
 * UNQUALIFIED column that is not one of them is an outer reference too. */
static int
corr_collect(const sql_expr_t *e, const char *otab,
             const char *const *inner, int ninner,
             struct corr_ref *refs, int n)
{
	const sql_exprlist_item_t *it;
	const sql_case_arm_t *arm;
	if (e == NULL) return n;
	if (e->op == SX_E_COLUMN && e->nname == 1 && e->src && e->srclen &&
	    inner != NULL) {
		/* Unqualified column: an outer ref iff it is not an inner column. */
		int i, isin = 0;
		for (i = 0; i < ninner; i++)
			if (str_eq_cstr(&e->name[0], inner[i])) { isin = 1; break; }
		if (!isin) {
			if (n >= VX_CORR_MAX) return -1;
			refs[n].p = e->src; refs[n].len = e->srclen;
			refs[n].col = &e->name[0];
			return n + 1;
		}
		return n;
	}
	if (e->op == SX_E_COLUMN && e->nname == 2 && e->src && e->srclen) {
		if (str_eq_cstr(&e->name[0], otab)) {
			if (n >= VX_CORR_MAX) return -1;
			refs[n].p = e->src; refs[n].len = e->srclen;
			refs[n].col = &e->name[1];
			return n + 1;
		}
		return n;
	}
	if ((n = corr_collect(e->a, otab, inner, ninner, refs, n)) < 0) return -1;
	if ((n = corr_collect(e->b, otab, inner, ninner, refs, n)) < 0) return -1;
	if ((n = corr_collect(e->c, otab, inner, ninner, refs, n)) < 0) return -1;
	for (it = e->list ? e->list->head : NULL; it; it = it->next)
		if ((n = corr_collect(it->expr, otab, inner, ninner, refs, n)) < 0) return -1;
	for (arm = e->arms; arm; arm = arm->next) {
		if ((n = corr_collect(arm->when, otab, inner, ninner, refs, n)) < 0) return -1;
		if ((n = corr_collect(arm->then, otab, inner, ninner, refs, n)) < 0) return -1;
	}
	if ((n = corr_collect(e->els, otab, inner, ninner, refs, n)) < 0) return -1;
	if (e->sel != NULL) return -1;   /* nested subquery: not handled */
	return n;
}

static int corr_ref_cmp(const void *a, const void *b)
{
	const struct corr_ref *x = a, *y = b;
	return (x->p < y->p) ? -1 : (x->p > y->p) ? 1 : 0;
}

/*
 * Shared machinery for a CORRELATED subquery on the single-table read
 * path (used by both the scalar form and the IN form).  Collects outer
 * references (columns qualified by the outer table) from the subquery's
 * expressions, rewrites each to a ? parameter in the subquery text,
 * validates it via a trial vx_run_p and records its text in
 * st->corrsql[].  On success returns
 * 0 and sets *pidx (the st->corrsql[] slot), *pbindcol (a plan-arena array
 * of srcrow indices feeding each ?), and *pnref; the prepared stmt's
 * column count is left to the caller to validate.  Returns -1 (caller
 * sets c->fail) when correlation cannot be turned into the supported
 * shape.  ncol_want is the required result column count (1). */
static int
build_corr_stmt(struct vx_compiler *c, const sql_expr_t *e,
                const char *const *inner, int ninner,
                int ncol_want, int *pidx, int **pbindcol, int *pnref)
{
	struct corr_ref refs[VX_CORR_MAX];
	const char *otab;
	char *sub = NULL, *out = NULL, *s;
	size_t len, oi;
	int nref, i, ci, *bindcol;

	/* Single-table read path only (need the outer table name + nv). */
	if (c->jc != NULL || c->nv == NULL || c->st == NULL ||
	    c->st->db == NULL || c->st->table[0] == '\0' ||
	    e->src == NULL || e->srclen == 0)
		return -1;
	if (c->st->ncorr >= (int)(sizeof c->st->corrsql / sizeof c->st->corrsql[0]))
		return -1;
	otab = c->st->corr_alias[0] ? c->st->corr_alias : c->st->table;

	/* Collect outer-column references from the subquery's expressions. */
	nref = 0;
	if (e->sel) {
		const sql_select_t *q2 = e->sel;
		const sql_exprlist_item_t *it;
		for (it = q2->cols ? q2->cols->head : NULL; it; it = it->next)
			if ((nref = corr_collect(it->expr, otab, inner, ninner, refs, nref)) < 0) return -1;
		if ((nref = corr_collect(q2->where, otab, inner, ninner, refs, nref)) < 0) return -1;
		if ((nref = corr_collect(q2->having, otab, inner, ninner, refs, nref)) < 0) return -1;
	}
	if (nref == 0) return -1;   /* not actually correlated */
	/* shortcut: the per-row re-run binds only the outer columns, so a ?
	 * of the statement inside the subquery would read as NULL: decline
	 * (correlated subqueries with binds are not in TPROC-C). */
	{
		const char *q;
		for (q = e->src; q < e->src + e->srclen; q++)
			if (*q == '?' || *q == ':' || *q == '@' || *q == '$') return -1;
	}

	/* Build the parameterized subquery text: strip the wrapping parens,
	 * then splice ? in place of each outer-ref span (left to right). */
	sub = (char *)malloc((size_t)e->srclen + 1);
	if (sub == NULL) return -1;
	memcpy(sub, e->src, e->srclen); sub[e->srclen] = '\0';
	s = sub; len = e->srclen;
	while (len > 0 && (*s == ' ' || *s == '\t' || *s == '\n')) { s++; len--; }
	while (len > 0 && (s[len-1]==' '||s[len-1]=='\t'||s[len-1]=='\n')) len--;
	if (len < 2 || s[0] != '(' || s[len-1] != ')') { free(sub); return -1; }
	qsort(refs, (size_t)nref, sizeof refs[0], corr_ref_cmp);
	out = (char *)malloc((size_t)e->srclen + 1);
	if (out == NULL) { free(sub); return -1; }
	oi = 0;
	{
		const char *cur = e->src + 1;            /* past '(' */
		const char *end = e->src + e->srclen - 1; /* before ')' */
		for (i = 0; i < nref; i++) {
			size_t pre = (size_t)(refs[i].p - cur);
			if (refs[i].p < cur || refs[i].p + refs[i].len > end) {
				free(sub); free(out); return -1;
			}
			memcpy(out + oi, cur, pre); oi += pre;
			out[oi++] = '?';
			cur = refs[i].p + refs[i].len;
		}
		memcpy(out + oi, cur, (size_t)(end - cur)); oi += (size_t)(end - cur);
		out[oi] = '\0';
	}
	free(sub);

	/* Map each ? to its outer source column FIRST, so the trial run can
	 * type its dummy binds to match (a NULL bind would trip vexec's
	 * comparison affinity gate and falsely reject a runnable inner). */
	bindcol = (int *)arena_alloc(&c->st->plan_arena, sizeof(int) * (size_t)nref);
	if (bindcol == NULL) { free(out); return -1; }
	for (i = 0; i < nref; i++) {
		ci = nv_add(c->nv, refs[i].col);   /* outer column -> srcrow index */
		if (ci < 0) { free(out); return -1; }
		bindcol[i] = ci;
	}

	/* Validate the parameterized inner SELECT is vexec-runnable and
	 * returns the required number of columns, by a trial run (no SQLite).
	 * Type each dummy bind to its outer column's affinity so the inner's
	 * comparison gate sees a compatible operand; NUMERIC -> INT, TEXT ->
	 * a 1-char string, else NULL.  A run vexec cannot recognize (rc != 1)
	 * or with the wrong shape -> fall back. */
	{
		vx_cell_t tb[VX_CORR_MAX];
		vx_result_t *tr = NULL;
		int trc, tnc;
		for (i = 0; i < nref; i++) {
			memset(&tb[i], 0, sizeof tb[i]);
			switch (c->nv->aff[bindcol[i]]) {
			case VX_AFF_NUMERIC: tb[i].type = VX_INT; tb[i].i = 0; break;
			case VX_AFF_TEXT:    tb[i].type = VX_TEXT;
			                     tb[i].bytes = (const uint8_t *)""; tb[i].nbytes = 0; break;
			default:             tb[i].type = VX_NULL; break;
			}
		}
		trc = vx_run_p(c->st->db, out, tb, nref, 1, &tr, NULL);
		if (trc != 1 || tr == NULL) { if (tr) vx_result_free(tr); free(out); return -1; }
		tnc = vx_result_ncol(tr);
		vx_result_free(tr);
		if (ncol_want >= 0 && tnc != ncol_want) { free(out); return -1; }
	}

	/* Keep the parameterized text (plan-arena owned) for per-row runs. */
	{
		size_t tl = strlen(out) + 1;
		char *kept = (char *)arena_alloc(&c->st->plan_arena, tl);
		if (kept == NULL) { free(out); return -1; }
		memcpy(kept, out, tl);
		free(out);
		*pidx = c->st->ncorr;
		c->st->corrsql[c->st->ncorr] = kept;
		c->st->corr_ncol[c->st->ncorr] = ncol_want;
		c->st->ncorr++;
	}
	*pbindcol = bindcol;
	*pnref = nref;
	return 0;
}

/*
 * Compile a CORRELATED scalar subquery on the single-table read path.
 * Outer references (columns qualified by the outer table) are rewritten
 * to ? parameters in the subquery text and bound from the current row
 * at eval time.  Returns a VXO_CORRSUBQ node, or NULL (sets c->fail)
 * when correlation cannot be turned into the supported shape (multiple
 * outer tables, a join, too many refs, > 1 result column, etc.).
 */
static vx_expr_t *
compile_corr_subquery(struct vx_compiler *c, const sql_expr_t *e)
{
	int idx, nref, *bindcol;
	vx_expr_t *n;

	if (build_corr_stmt(c, e, NULL, 0, 1, &idx, &bindcol, &nref) != 0) {
		c->fail = 1; return NULL;
	}
	n = expr_node(c, VXO_CORRSUBQ);
	if (n == NULL) { c->fail = 1; return NULL; }
	n->bindcol = bindcol;
	n->nargs = nref;
	n->col = idx;
	return n;
}

/*
 * Compile a CORRELATED IN (SELECT) -- operand IN (subquery referencing
 * an outer column).  Like the correlated scalar form: the inner select
 * is parameterized on its outer refs and prepared once; at eval it is
 * re-run for the current row and the operand tested for membership in
 * its single-column result, with SQL 3-valued NULL semantics.  Returns
 * a VXO_CORRIN node (lit.i set for NOT IN), or NULL (sets c->fail). */
static vx_expr_t *
compile_corr_in_select(struct vx_compiler *c, const sql_expr_t *e)
{
	int idx, nref, *bindcol;
	vx_expr_t *n, *a;
	enum vx_aff acls;

	/* The operand must have a fixed (numeric or text) affinity so the
	 * per-row membership comparison has no ambiguous coercion -- the same
	 * gate the literal-list IN uses. */
	a = compile_expr(c, e->a);
	if (a == NULL) return NULL;
	acls = node_class(c, a);
	if (acls != VX_AFF_NUMERIC && acls != VX_AFF_TEXT) { c->fail = 1; return NULL; }

	if (build_corr_stmt(c, e, NULL, 0, 1, &idx, &bindcol, &nref) != 0) {
		c->fail = 1; return NULL;
	}
	n = expr_node(c, VXO_CORRIN);
	if (n == NULL) { c->fail = 1; return NULL; }
	n->a = a;
	n->bindcol = bindcol;
	n->nargs = nref;
	n->col = idx;
	n->lit.type = VX_INT;
	n->lit.i = e->ival ? 1 : 0;   /* ival = NOT IN */
	return n;
}

/* Compile EXISTS (correlated select) on the single-table read path.
 * Reuses build_corr_stmt (rewriting outer refs to ? binds) with no
 * required column count; the executor tests whether the per-row run
 * yields any row.  For correlation via UNQUALIFIED outer names, the
 * inner table's column set is provided so an unqualified subquery
 * column that the inner table lacks is treated as an outer ref (SQLite
 * name resolution).  Falls back (c->fail) if the subquery is not in the
 * supported single-table shape. */
static vx_expr_t *
compile_exists(struct vx_compiler *c, const sql_expr_t *e)
{
	int idx, nref = 0, *bindcol;
	vx_expr_t *n;
	const char *innames[64];
	xstore_col_t *icols = NULL;
	int inc = 0, i, rc;

	/* Gather the inner FROM table's column names (single-table inner
	 * only) so unqualified outer refs can be recognised.  The schema
	 * buffer is heap-allocated (not on the stack) because this runs on a
	 * nested-prepare chain over a small fiber stack. */
	if (e->sel != NULL && e->sel->from != NULL && e->sel->from->next == NULL &&
	    e->sel->from->subquery == NULL && e->sel->from->table.len > 0 &&
	    e->sel->from->table.len < 64 && c->st != NULL) {
		char itab[64];
		bt_t *bt = xstore_bt_of(c->st->db);
		icols = (xstore_col_t *)malloc(sizeof(xstore_col_t) * 64);
		if (icols == NULL) { c->fail = 1; return NULL; }
		memcpy(itab, e->sel->from->table.p, e->sel->from->table.len);
		itab[e->sel->from->table.len] = '\0';
		inc = bt ? xstore_table_schema(bt, itab, icols, 64) : 0;
		for (i = 0; i < inc && i < 64; i++) innames[i] = icols[i].name;
	}

	rc = build_corr_stmt(c, e, inc > 0 ? innames : NULL, inc, -1, &idx,
	    &bindcol, &nref);
	free(icols);
	if (rc != 0) { c->fail = 1; return NULL; }
	n = expr_node(c, VXO_EXISTS);
	if (n == NULL) { c->fail = 1; return NULL; }
	n->bindcol = bindcol;
	n->nargs = nref;
	n->col = idx;
	return n;
}

static int vx_run_sel(struct xsql *db, const sql_select_t *sel,
                      const vx_cell_t *binds, int nbinds, vx_result_t **res);

static vx_expr_t *
compile_scalar_subquery(struct vx_compiler *c, const sql_expr_t *e)
{
	vx_expr_t *n;
	vx_result_t *r = NULL;
	int rc, nrow;

	if (c->st == NULL || c->st->db == NULL || e->sel == NULL) {
		c->fail = 1; return NULL;
	}
	/* Correlation/shape gate on the parsed subquery: one column, no outer
	 * reference.  A correlated subquery fails it and falls through to
	 * compile_corr_subquery. */
	if (!native_subquery_gate_sel(e->sel, 1)) { c->fail = 1; return NULL; }

	/* Run the inner SELECT from its AST (no re-parse; its ? ordinals are
	 * the statement's, so it takes the whole bind array).  It must yield
	 * one column and at most one row (SQLite would too); anything vexec
	 * cannot run declines the whole statement. */
	rc = vx_run_sel(c->st->db, e->sel, c->binds, c->nbinds, &r);
	if (rc != 1 || r == NULL) { if (r) vx_result_free(r); c->fail = 1; return NULL; }
	c->st->noreuse = 1;   /* its value is spliced in as a literal */
	if (vx_result_ncol(r) != 1) { vx_result_free(r); c->fail = 1; return NULL; }
	nrow = vx_result_nrow(r);
	if (nrow > 1) { vx_result_free(r); c->fail = 1; return NULL; }

	n = expr_node(c, VXO_LIT);
	if (n == NULL) { vx_result_free(r); c->fail = 1; return NULL; }
	if (nrow == 0) {
		n->lit.type = VX_NULL;            /* empty subquery -> NULL */
	} else {
		switch (vx_result_type(r, 0, 0)) {
		case VX_INT:
			n->lit.type = VX_INT; n->lit.i = vx_result_int64(r, 0, 0); break;
		case VX_REAL:
			n->lit.type = VX_REAL; n->lit.r = vx_result_double(r, 0, 0); break;
		case VX_TEXT: case VX_BLOB: {
			const char *b = vx_result_text(r, 0, 0);
			int nb = vx_result_bytes(r, 0, 0);
			uint8_t *p = (uint8_t *)arena_alloc(&c->st->plan_arena, (size_t)nb + 1);
			if (p == NULL) { vx_result_free(r); c->fail = 1; return NULL; }
			if (nb && b) memcpy(p, b, (size_t)nb);
			p[nb] = '\0';
			n->lit.type = (vx_result_type(r, 0, 0) == VX_TEXT) ? VX_TEXT : VX_BLOB;
			n->lit.bytes = p; n->lit.nbytes = (uint32_t)nb;
			break; }
		default:
			n->lit.type = VX_NULL; break;
		}
	}
	vx_result_free(r);
	return n;
}

/* Max rows materialized from an IN (SELECT ...) subquery; a larger
 * result set falls back to the VDBE rather than build an unbounded
 * literal list. */
#define VX_IN_SELECT_MAX 4096

static int cmp_vals(const vx_cell_t *a, const vx_cell_t *b);   /* fwd */
static int sort_cmp(const vx_cell_t *a, const vx_cell_t *b);   /* fwd */

static int in_sorted_cmp(const void *pa, const void *pb)
{
	return cmp_vals((const vx_cell_t *)pa, (const vx_cell_t *)pb);
}

/* Build the sorted membership index for a VXO_IN / VXO_NOTIN node whose
 * list (e->args[0..nargs-1]) is entirely constant literals.  Sorts the
 * non-NULL element values into n->in_sorted (arena-allocated, plan
 * lifetime) so eval can binary-search; records whether a NULL element
 * was present.  A non-literal element leaves the node on the linear
 * path (in_sorted stays NULL).  Best-effort: on allocation failure the
 * node simply keeps the linear path. */
static void
build_in_index(struct vx_compiler *c, vx_expr_t *n)
{
	int k, m = 0;
	vx_cell_t *arr;
	for (k = 0; k < n->nargs; k++)
		if (n->args[k] == NULL || n->args[k]->op != VXO_LIT ||
		    n->args[k]->pord != 0)   /* a bound ?: its value changes per run */
			return;
	if (n->nargs <= 0) return;
	arr = (vx_cell_t *)arena_alloc(&c->st->plan_arena,
	    sizeof(vx_cell_t) * (size_t)n->nargs);
	if (arr == NULL) return;
	for (k = 0; k < n->nargs; k++) {
		if (n->args[k]->lit.type == VX_NULL) { n->in_has_null = 1; continue; }
		arr[m++] = n->args[k]->lit;
	}
	if (m > 1) qsort(arr, (size_t)m, sizeof(vx_cell_t), in_sorted_cmp);
	n->in_sorted = arr;
	n->in_nsorted = m;
}

/* Compile a IN (SELECT ...) (or NOT IN) against an UNCORRELATED, single-
 * column subquery.  The subquery is run ONCE, its values materialized as
 * a literal list, and the membership test reuses the VXO_IN / VXO_NOTIN
 * machinery (same as IN (list)).  Each value must share the operand's
 * affinity class.  Returns the node, or NULL (sets c->fail -> VDBE) for
 * a correlated / parametrized / too-large / mixed-affinity subquery. */
static vx_expr_t *
compile_in_select(struct vx_compiler *c, const sql_expr_t *e)
{
	vx_result_t *r = NULL;
	vx_expr_t *n, *a;
	enum vx_aff acls;
	int cnt = 0, cap = 16, nrow, ri, rc;

	if (c->st == NULL || c->st->db == NULL || e->sel == NULL) {
		c->fail = 1; return NULL;
	}
	a = compile_expr(c, e->a);
	if (a == NULL) return NULL;
	acls = node_class(c, a);

	/* NATIVE gate + run: an uncorrelated single-column subquery, run once
	 * through vexec (no SQLite), its values materialized as a literal
	 * list.  A correlated subquery fails the gate and the caller tries
	 * compile_corr_subquery. */
	if (!native_subquery_gate_sel(e->sel, 1)) { c->fail = 1; return NULL; }
	rc = vx_run_sel(c->st->db, e->sel, c->binds, c->nbinds, &r);
	if (rc != 1 || r == NULL || vx_result_ncol(r) != 1) {
		if (r) vx_result_free(r);
		c->fail = 1; return NULL;
	}
	c->st->noreuse = 1;   /* its values are spliced in as literals */

	n = expr_node(c, e->ival ? VXO_NOTIN : VXO_IN);   /* ival = NOT IN */
	if (n == NULL) { vx_result_free(r); c->fail = 1; return NULL; }
	n->a = a;
	n->args = (vx_expr_t **)arena_alloc(&c->st->plan_arena,
	                                    sizeof(vx_expr_t *) * (size_t)cap);
	if (n->args == NULL) { vx_result_free(r); c->fail = 1; return NULL; }

	nrow = vx_result_nrow(r);
	for (ri = 0; ri < nrow; ri++) {
		vx_expr_t *v;
		if (cnt >= VX_IN_SELECT_MAX) { vx_result_free(r); c->fail = 1; return NULL; }
		if (cnt == cap) {
			int nc = cap * 2;
			vx_expr_t **na = (vx_expr_t **)arena_alloc(&c->st->plan_arena,
			                                  sizeof(vx_expr_t *) * (size_t)nc);
			if (na == NULL) { vx_result_free(r); c->fail = 1; return NULL; }
			memcpy(na, n->args, sizeof(vx_expr_t *) * (size_t)cnt);
			n->args = na; cap = nc;
		}
		v = expr_node(c, VXO_LIT);
		if (v == NULL) { vx_result_free(r); c->fail = 1; return NULL; }
		switch (vx_result_type(r, ri, 0)) {
		case VX_INT:  v->lit.type = VX_INT;  v->lit.i = vx_result_int64(r, ri, 0); break;
		case VX_REAL: v->lit.type = VX_REAL; v->lit.r = vx_result_double(r, ri, 0); break;
		case VX_TEXT: case VX_BLOB: {
			const char *b = vx_result_text(r, ri, 0);
			int nb = vx_result_bytes(r, ri, 0);
			uint8_t *pp = (uint8_t *)arena_alloc(&c->st->plan_arena, (size_t)nb + 1);
			if (pp == NULL) { vx_result_free(r); c->fail = 1; return NULL; }
			if (nb && b) memcpy(pp, b, (size_t)nb);
			pp[nb] = '\0';
			v->lit.type = (vx_result_type(r, ri, 0) == VX_TEXT) ? VX_TEXT : VX_BLOB;
			v->lit.bytes = pp; v->lit.nbytes = (uint32_t)nb;
			break; }
		default: v->lit.type = VX_NULL; v->lit.nbytes = 0; break;
		}
		if (v->lit.type != VX_NULL && node_class(c, v) != acls) {
			vx_result_free(r); c->fail = 1; return NULL;
		}
		n->args[cnt++] = v;
	}
	vx_result_free(r);

	if (cnt == 0) {
		/* IN (empty) is false and NOT IN (empty) true for every operand,
		 * NULL included (SQLite); the stock-level IN over an empty order
		 * range must not be rejected. */
		n->op = VXO_LIT;
		n->a = NULL;
		n->lit.type = VX_INT;
		n->lit.i = e->ival ? 1 : 0;
		return n;
	}
	n->nargs = cnt;
	build_in_index(c, n);   /* sorted membership: subquery values are literals */
	return n;
}

static void coerce_to_affinity(vx_cell_t *v, enum vx_aff aff,
                               struct vx_arena_blk **arena);
static void coerce_numeric(vx_cell_t *v);

/*
 * SQLite's comparison affinity for a COLUMN compared with a LITERAL or
 * bound ? (which has no affinity): a column with numeric affinity
 * applies numeric affinity to the other side, a TEXT column text
 * affinity, a BLOB/none column none.  Applied once, here, to the
 * literal; eval then compares in SQLite's cross-class order (number <
 * text < blob).  Returns 1 when the pair is now safe (literal coerced as
 * SQLite would), 0 when this rule does not apply (the caller's other
 * gates decide), -1 never.  The Turso Order-Status binds c_last (TEXT)
 * into "c_id = ?": SQLite matches nothing, a gate rejection failed the
 * whole transaction.
 */
static int
cmp_affinity_gate(struct vx_compiler *c, vx_expr_t *a, vx_expr_t *b)
{
	vx_expr_t *col, *lit;
	enum vx_aff aff;
	if (a->op == VXO_COL && b->op == VXO_LIT) { col = a; lit = b; }
	else if (b->op == VXO_COL && a->op == VXO_LIT) { col = b; lit = a; }
	else return 0;
	if (lit->lit.type == VX_NULL) return 1;
	/* A column with no affinity is compared as stored (SQLite: neither
	 * side converts); one whose affinity vexec cannot state declines. */
	if (col->noaff == VX_NOAFF_NO) return 1;
	if (col->noaff == VX_NOAFF_UNKNOWN) return -1;
	aff = node_class(c, col);
	if (aff == VX_AFF_NUMERIC) {
		lit->pcoerce = 1;
		if (lit->lit.type == VX_TEXT) coerce_numeric(&lit->lit);
	} else if (aff == VX_AFF_TEXT) {
		lit->pcoerce = 2;
		if (lit->lit.type == VX_INT || lit->lit.type == VX_REAL)
			coerce_to_affinity(&lit->lit, VX_AFF_TEXT, &c->st->plan_arena);
	}
	return 1;
}

static vx_expr_t *agg_resolve(struct vx_compiler *c, const sql_expr_t *e,
                              int *handled);

static vx_expr_t *
compile_expr(struct vx_compiler *c, const sql_expr_t *e)
{
	if (c->fail || e == NULL) { c->fail = 1; return NULL; }

	if (c->ag != NULL && c->ag->grouping) {
		int handled;
		vx_expr_t *n = agg_resolve(c, e, &handled);
		if (handled) return n;
	} else if (is_agg_call(e)) {
		c->fail = 1;   /* an aggregate outside an aggregate context */
		return NULL;
	}

	switch (e->op) {
	case SX_E_SUBQUERY: {
		/* Try the uncorrelated form first (run once, splice a literal);
		 * if that fails (the subquery references an outer column, so its
		 * standalone prepare fails), try the correlated form (rewrite
		 * outer refs to ? and re-run per row). */
		vx_expr_t *u;
		int save = c->fail;
		c->fail = 0;
		u = compile_scalar_subquery(c, e);
		if (!c->fail && u != NULL) return u;
		c->fail = save;
		return compile_corr_subquery(c, e);
	}
	case SX_E_IN_SELECT: {
		/* EXISTS (select): a == NULL and ival bit1 set -- test whether the
		 * (correlated) subquery yields any row. */
		if (e->a == NULL && (e->ival & 2))
			return compile_exists(c, e);
		/* Uncorrelated first (run once, materialize a literal list); if
		 * that fails because the subquery references an outer column,
		 * try the correlated form (re-run per row, test membership). */
		vx_expr_t *u;
		int save = c->fail;
		c->fail = 0;
		u = compile_in_select(c, e);
		if (!c->fail && u != NULL) return u;
		c->fail = save;
		return compile_corr_in_select(c, e);
	}
	case SX_E_NULL: case SX_E_NUMBER: case SX_E_STRING: case SX_E_BLOB:
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
		if (n) {
			n->col = idx;
			if (c->jc == NULL) n->noaff = c->nv->noaff[idx];
		}
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
			/* Unary plus is the value, but drops a column's affinity. */
			if (a->op == VXO_COL) {
				n = expr_node(c, VXO_COL);
				if (n) { n->col = a->col; n->noaff = VX_NOAFF_NO; }
				return n;
			}
			return a;
		} else {
			c->fail = 1; return NULL;   /* ~ etc. not in V1 */
		}
		if (n) n->a = a;
		return n;
	}

	case SX_E_BINARY: {
		enum vx_op op;
		vx_expr_t *n, *a, *b;
		int dyn_coerce = 0;
		if (e->op2 == TK_LIKE) {
			/* x LIKE y  ==  like(y, x): operand in .a, pattern in .b.
			 * SQLite's LIKE is ASCII case-insensitive with no ESCAPE
			 * here (the grammar has no ESCAPE clause).  Both operands
			 * must be text-affinity (no numeric->text coercion). */
			a = compile_expr(c, e->a);   /* the value being matched */
			b = compile_expr(c, e->b);   /* the pattern */
			if (a == NULL || b == NULL) return NULL;
			/* A literal NULL side makes the match NULL (eval checks). */
			if ((node_class(c, a) != VX_AFF_TEXT && !is_null_lit(a)) ||
			    (node_class(c, b) != VX_AFF_TEXT && !is_null_lit(b))) { c->fail = 1; return NULL; }
			n = expr_node(c, VXO_LIKE);
			if (n) { n->a = a; n->b = b; }
			if (n && e->c != NULL) {     /* LIKE ... ESCAPE x */
				vx_expr_t *x = compile_expr(c, e->c);
				if (x == NULL) return NULL;
				/* SQLite errors on an escape that is not one character;
				 * declining (an error here too) keeps the two in step. */
				if (node_class(c, x) != VX_AFF_TEXT ||
				    (x->op == VXO_LIT && x->lit.nbytes != 1)) { c->fail = 1; return NULL; }
				if (x->pord != 0) c->st->noreuse = 1;   /* length checked */
				n->args = (vx_expr_t **)arena_alloc(&c->st->plan_arena,
				    sizeof(vx_expr_t *));
				if (n->args == NULL) { c->fail = 1; return NULL; }
				n->args[0] = x; n->nargs = 1;
			}
			return n;
		}
		if (!tok_to_binop(e->op2, &op)) { c->fail = 1; return NULL; }
		a = compile_expr(c, e->a);
		b = compile_expr(c, e->b);
		if (a == NULL || b == NULL) return NULL;

		/* Affinity safety for arithmetic and comparison. */
		switch (op) {
		case VXO_ADD: case VXO_SUB: case VXO_MUL: case VXO_DIV: case VXO_MOD:
			/* Both operands must be numeric (no text->number coercion) --
			 * OR a literal NULL, which makes the whole expression NULL
			 * regardless of the other operand (SQLite semantics). */
			if ((node_class(c, a) != VX_AFF_NUMERIC && !is_null_lit(a)) ||
			    (node_class(c, b) != VX_AFF_NUMERIC && !is_null_lit(b)))
				{ c->fail = 1; return NULL; }
			break;
		case VXO_CONCAT:
			/* || coerces operands to text; allow numeric or text, not blob. */
			if (node_class(c, a) == VX_AFF_BLOB ||
			    node_class(c, b) == VX_AFF_BLOB) { c->fail = 1; return NULL; }
			break;
		case VXO_IS:
			/* IS against a literal NULL is IS NULL (no coercion). */
			if (is_null_lit(a) || is_null_lit(b)) break;
			/* FALLTHROUGH */
		case VXO_EQ: case VXO_NE: case VXO_LT: case VXO_LE:
		case VXO_GT: case VXO_GE: {
			int g = cmp_affinity_gate(c, a, b);
			if (g < 0) { c->fail = 1; return NULL; }
			if (g > 0) break;
			/* No-coercion gate: both numeric, or both text.  Anything
			 * with a blob/none-affinity operand or a numeric-vs-text
			 * pairing could coerce -> fall back.  EXCEPTION: a value of
			 * dynamic type (a correlated subquery or a CASE) carries no
			 * fixed affinity; SQLite applies the OTHER operand's affinity
			 * to the comparison, so it is safe to compare against a
			 * numeric or text operand (the dynamic side adopts that
			 * class).  Compared against a blob (or two dynamic sides) we
			 * still fall back. */
			enum vx_aff ca = node_class(c, a), cb = node_class(c, b);
			int a_dyn = (a->op == VXO_CORRSUBQ || a->op == VXO_CASE);
			int b_dyn = (b->op == VXO_CORRSUBQ || b->op == VXO_CASE);
			int ok = (ca == VX_AFF_NUMERIC && cb == VX_AFF_NUMERIC) ||
			         (ca == VX_AFF_TEXT && cb == VX_AFF_TEXT);
			if (!ok && a_dyn && !b_dyn &&
			    (cb == VX_AFF_NUMERIC || cb == VX_AFF_TEXT)) ok = 1;
			if (!ok && b_dyn && !a_dyn &&
			    (ca == VX_AFF_NUMERIC || ca == VX_AFF_TEXT)) ok = 1;
			/* A NULL literal (e.g. an empty scalar subquery) makes the
			 * comparison NULL whatever the other side: no coercion. */
			if (!ok && (is_null_lit(a) || is_null_lit(b))) ok = 1;
			if (!ok) { c->fail = 1; return NULL; }
			dyn_coerce = a_dyn || b_dyn;
			/* A CASE has no affinity; against a literal (none either)
			 * nothing converts.  (A subquery value has its column's.) */
			if ((a->op == VXO_CASE && b->op == VXO_LIT) ||
			    (b->op == VXO_CASE && a->op == VXO_LIT))
				dyn_coerce = 0;
			break;
		}
		default: break;   /* AND/OR: operands are boolean-ish, fine */
		}

		n = expr_node(c, op);
		if (n) { n->a = a; n->b = b; n->col = dyn_coerce; }
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
		 * rule (both numeric, or both text), or under SQLite's affinity
		 * rule for a literal bound against a column. */
		if (!((((ca == VX_AFF_NUMERIC && clo == VX_AFF_NUMERIC) ||
		        (ca == VX_AFF_TEXT && clo == VX_AFF_TEXT)) ||
		       cmp_affinity_gate(c, operand, lo) > 0) &&
		      (((ca == VX_AFF_NUMERIC && chi == VX_AFF_NUMERIC) ||
		        (ca == VX_AFF_TEXT && chi == VX_AFF_TEXT)) ||
		       cmp_affinity_gate(c, operand2, hi) > 0))) {
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
		build_in_index(c, n);   /* sorted membership when all elements literal */
		return n;
	}

	case SX_E_FUNC: {
		int nargs = 0, ok = 0;
		const sql_exprlist_item_t *it;
		enum vx_func f;
		enum vx_op lg;
		vx_expr_t *n;
		int k;
		if (e->ival & 1) { c->fail = 1; return NULL; }   /* DISTINCT agg */
		if (e->ival & 2) { c->fail = 1; return NULL; }   /* func(*) */
		for (it = e->list ? e->list->head : NULL; it; it = it->next) nargs++;
		/* CAST(expr AS type): name[0]="cast", name[1]=target type, one arg. */
		if (e->nname == 2 && name_is(&e->name[0], "cast") && nargs == 1) {
			vx_expr_t *arg = compile_expr(c, e->list->head->expr);
			enum vx_cast_class cc = cast_type_class(&e->name[1]);
			if (arg == NULL) return NULL;
			/* CAST to BLOB is left unsupported: the wire protocol base64-
			 * encodes a BLOB result, so it cannot compare byte-equal to
			 * SQLite's raw blob in the differential test -- reject cleanly
			 * rather than emit a mismatched representation. */
			if (cc == VX_CAST_BLOB) { c->fail = 1; return NULL; }
			n = expr_node(c, VXO_FUNC);
			if (n == NULL) return NULL;
			n->func = VXF_CAST;
			n->nargs = 1;
			n->args = (vx_expr_t **)arena_alloc(&c->st->plan_arena, sizeof(vx_expr_t *));
			if (n->args == NULL) { c->fail = 1; return NULL; }
			n->args[0] = arg;
			n->col = (int)cc;   /* target affinity code */
			return n;
		}
		/* like(P,X) / glob(P,X): SQLite defines these as "X LIKE P" /
		 * "X GLOB P".  Arg 0 is the pattern, arg 1 is the value; build
		 * a VXO_LIKE/VXO_GLOB node (operand in .a, pattern in .b). */
		lg = likeglob_func_op(&e->name[0], nargs);
		if (lg != 0) {
			vx_expr_t *pat, *val;
			it = e->list->head;
			pat = compile_expr(c, it->expr);
			val = compile_expr(c, it->next->expr);
			if (pat == NULL || val == NULL) return NULL;
			if (node_class(c, pat) != VX_AFF_TEXT ||
			    node_class(c, val) != VX_AFF_TEXT) { c->fail = 1; return NULL; }
			n = expr_node(c, lg);
			if (n) { n->a = val; n->b = pat; }
			return n;
		}
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
		if (f == VXF_EXT && compile_ext(c, e, n) != 0) { c->fail = 1; return NULL; }
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
	if (e->op >= VXO_EQ && e->op <= VXO_GE && e->col == 0 &&
	    (e->a->op == VXO_COL || e->a->op == VXO_LIT) &&
	    (e->b->op == VXO_COL || e->b->op == VXO_LIT)) {
		/* The common filter term "col op literal": two leaves.  Both
		 * INTEGER compares the values directly (what sort_cmp does for
		 * two INTs); anything else takes the general path. */
		const vx_cell_t *x = e->a->op == VXO_COL ? &row[e->a->col] : &e->a->lit;
		const vx_cell_t *y = e->b->op == VXO_COL ? &row[e->b->col] : &e->b->lit;
		if (x->type == VX_INT && y->type == VX_INT) {
			switch (e->op) {
			case VXO_EQ: return x->i == y->i;
			case VXO_NE: return x->i != y->i;
			case VXO_LT: return x->i < y->i;
			case VXO_LE: return x->i <= y->i;
			case VXO_GT: return x->i > y->i;
			default:     return x->i >= y->i;
			}
		}
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

/* Render a REAL the way SQLite turns one into TEXT ("%!.15g", sqlite
 * 3.46): 15 significant digits after SQLite's own 18-digit decode, ".0"
 * on a value with no '.', "1.0e+20" style exponent, -0.0 -> "0.0",
 * Inf -> "Inf".  The one implementation lives in vxfunc.c. */
static int
real_text(double r, char *buf, size_t cap)
{
	return vxf_real_text(r, buf, cap);
}

/* SQLite numeric affinity on a TEXT cell: if the WHOLE string parses as
 * an integer or real (optional sign, decimal, exponent), rewrite the
 * cell in place to VX_INT / VX_REAL; otherwise leave it as text.  Used
 * only when a dynamic operand (subquery / CASE) is compared against a
 * numeric operand, mirroring SQLite's comparison-affinity coercion. */
static void
coerce_numeric(vx_cell_t *v)
{
	char buf[64];
	char *end = NULL;
	uint32_t n = v->nbytes, i = 0;
	int isreal = 0, nd = 0;
	if (v->type != VX_TEXT || v->bytes == NULL) return;
	if (n == 0 || n >= sizeof buf) return;
	memcpy(buf, v->bytes, n); buf[n] = '\0';
	/* SQLite numeric affinity tolerates leading AND trailing spaces
	 * ('1.0  ' -> REAL).  strtod/strtoll skip leading whitespace but
	 * stop at trailing, so trim the trailing run before the "whole
	 * string consumed" check (else 'end == buf + n' spuriously fails). */
	while (n > 0 && (buf[n - 1] == ' ' || buf[n - 1] == '\t' ||
			 buf[n - 1] == '\n' || buf[n - 1] == '\r' ||
			 buf[n - 1] == '\f' || buf[n - 1] == '\v')) {
		n--;
		buf[n] = '\0';
	}
	/* Accept only SQLite's numeric grammar: [sign] digits [. digits]
	 * [e [sign] digits], with a digit somewhere in the mantissa.  strtod
	 * alone would also take inf / nan / 0x10, which SQLite keeps as text. */
	while (i < n && isspace((unsigned char)buf[i])) i++;
	if (i < n && (buf[i] == '+' || buf[i] == '-')) i++;
	for (; i < n && isdigit((unsigned char)buf[i]); i++) nd++;
	if (i < n && buf[i] == '.') {
		isreal = 1;
		for (i++; i < n && isdigit((unsigned char)buf[i]); i++) nd++;
	}
	if (nd == 0) return;
	if (i < n && (buf[i] == 'e' || buf[i] == 'E')) {
		isreal = 1;
		i++;
		if (i < n && (buf[i] == '+' || buf[i] == '-')) i++;
		if (i >= n || !isdigit((unsigned char)buf[i])) return;
		while (i < n && isdigit((unsigned char)buf[i])) i++;
	}
	if (i != n) return;
	if (!isreal) {
		long long ll;
		errno = 0;
		ll = strtoll(buf, &end, 10);
		if (errno == 0 && end == buf + n) {
			v->type = VX_INT; v->i = (int64_t)ll;
			return;
		}
		/* Out of int64 range: SQLite keeps it as a REAL. */
	}
	v->type = VX_REAL; v->r = strtod(buf, NULL);
}

/* Apply SQLite column affinity to a value being stored (INSERT/UPDATE).
 * TEXT converts INTEGER/REAL to its text form.  NUMERIC / INTEGER / REAL
 * convert a numeric-looking TEXT to a number; NUMERIC and INTEGER also
 * turn an integral in-range REAL into an INTEGER (3.0 -> 3, as SQLite's
 * applyAffinity / IntegerAffinity do); REAL stores any number as REAL
 * (3 -> 3.0, the read-back view SQLite gives a REAL column).  BLOB/none
 * leaves the value as-is.  NULL is never coerced.  Text produced for an
 * int/real is allocated in `arena`. */
static void
coerce_to_affinity(vx_cell_t *v, enum vx_aff aff, struct vx_arena_blk **arena)
{
	if (v->type == VX_NULL) return;
	if (aff == VX_AFF_TEXT) {
		char buf[64];
		int n;
		if (v->type == VX_TEXT || v->type == VX_BLOB) return;
		if (v->type == VX_INT)
			n = snprintf(buf, sizeof buf, "%lld", (long long)v->i);
		else if (v->type == VX_REAL)
			n = real_text(v->r, buf, sizeof buf);
		else return;
		if (n < 0 || n >= (int)sizeof buf) return;
		{
			uint8_t *p = (uint8_t *)arena_alloc(arena, (size_t)n + 1);
			if (p == NULL) return;
			memcpy(p, buf, (size_t)n + 1);
			v->type = VX_TEXT; v->bytes = p; v->nbytes = (uint32_t)n;
		}
	} else if (aff == VX_AFF_NUMERIC || aff == VX_AFF_INTEGER ||
	           aff == VX_AFF_REAL) {
		if (v->type == VX_TEXT) (void)coerce_numeric(v);
		if (aff == VX_AFF_REAL && v->type == VX_INT) {
			v->type = VX_REAL; v->r = (double)v->i;
		} else if (aff != VX_AFF_REAL && v->type == VX_REAL &&
		    v->r > -9223372036854775808.0 && v->r < 9223372036854775807.0 &&
		    v->r == (double)(int64_t)v->r) {
			v->type = VX_INT; v->i = (int64_t)v->r;
		}
	}
	/* VX_AFF_BLOB (none affinity): no conversion. */
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

/* CAST(text AS REAL): parse the longest leading numeric prefix as a
 * double (SQLite consumes optional sign, digits, '.', exponent).  A
 * string that does not start with a number yields 0.0. */
static double
cast_text_to_real(const uint8_t *p, uint32_t n)
{
	char buf[128];
	uint32_t i = 0;
	if (p == NULL || n == 0) return 0.0;
	while (i < n && (p[i] == ' ' || p[i] == '\t' || p[i] == '\n' ||
	                 p[i] == '\r' || p[i] == '\f' || p[i] == '\v')) i++;
	if (n - i >= sizeof buf) n = i + (uint32_t)sizeof buf - 1;
	if (n > i) memcpy(buf, p + i, n - i);
	buf[n - i] = '\0';
	return strtod(buf, NULL);   /* leading numeric prefix, else 0.0 */
}

/* CAST(text AS INTEGER): SQLite parses the leading numeric prefix as a
 * real then truncates toward zero (so '3.9' -> 3, '12abc' -> 12). */
static int64_t
cast_text_to_int(const uint8_t *p, uint32_t n)
{
	return (int64_t)cast_text_to_real(p, n);
}

/* Render a non-NULL cell to TEXT bytes (SQLite text coercion of a
 * numeric), copied into `arena`.  Returns the byte pointer and sets
 * *n; a text/blob cell is copied verbatim.  Caller must have excluded
 * VX_NULL (there is no text for NULL). */
static const uint8_t *
cell_text(const vx_cell_t *v, struct vx_arena_blk **arena, uint32_t *n)
{
	char tmp[64];
	int m;
	uint8_t *p;
	if (v->type == VX_TEXT || v->type == VX_BLOB) {
		*n = v->nbytes;
		return v->bytes;
	}
	if (v->type == VX_INT) m = snprintf(tmp, sizeof tmp, "%lld", (long long)v->i);
	else                   m = real_text(v->r, tmp, sizeof tmp);
	if (m < 0) { *n = 0; return (const uint8_t *)""; }
	p = (uint8_t *)arena_alloc(arena, (size_t)m + 1);
	if (p == NULL) { *n = 0; return NULL; }
	memcpy(p, tmp, (size_t)m); p[m] = '\0';
	*n = (uint32_t)m;
	return p;
}

/* UTF-8 character count of a byte range (lead bytes). */
static int64_t
utf8_len(const uint8_t *p, uint32_t nbytes)
{
	uint32_t k; int64_t nc = 0;
	for (k = 0; k < nbytes; k++)
		if ((p[k] & 0xc0) != 0x80) nc++;
	return nc;
}

static void
eval(const struct vx_stmt *st, const vx_expr_t *e,
     const vx_cell_t *row, struct vx_arena_blk **arena, vx_cell_t *out)
{
	/* The two leaf kinds overwrite *out whole: no clearing first. */
	if (e->op == VXO_COL) { *out = row[e->col]; return; }
	if (e->op == VXO_LIT) { *out = e->lit; return; }
	memset(out, 0, sizeof *out);

	switch (e->op) {

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
		else if (a.type == VX_REAL) { na = (uint32_t)real_text(a.r, ta, sizeof ta); pa = (uint8_t *)ta; }
		else { pa = a.bytes; na = a.nbytes; }
		if (b.type == VX_INT) { nb = (uint32_t)snprintf(tb, sizeof tb, "%lld", (long long)b.i); pb = (uint8_t *)tb; }
		else if (b.type == VX_REAL) { nb = (uint32_t)real_text(b.r, tb, sizeof tb); pb = (uint8_t *)tb; }
		else { pb = b.bytes; nb = b.nbytes; }
		buf = (uint8_t *)arena_alloc(arena, (size_t)na + nb + 1);
		if (buf == NULL) { out->type = VX_NULL; return; }
		if (na) memcpy(buf, pa, na);
		if (nb) memcpy(buf + na, pb, nb);
		buf[na + nb] = '\0';
		out->type = VX_TEXT; out->bytes = buf; out->nbytes = na + nb;
		return;
	}

	case VXO_IS:
	case VXO_EQ: case VXO_NE: case VXO_LT: case VXO_LE:
	case VXO_GT: case VXO_GE: {
		vx_cell_t a, b; int c;
		eval(st, e->a, row, arena, &a);
		eval(st, e->b, row, arena, &b);
		if (e->op == VXO_IS && (a.type == VX_NULL || b.type == VX_NULL)) {
			out->type = VX_INT;     /* IS: NULL-safe, never NULL */
			out->i = (a.type == VX_NULL && b.type == VX_NULL);
			return;
		}
		if (a.type == VX_NULL || b.type == VX_NULL) { out->type = VX_NULL; return; }
		/* SQLite applies numeric affinity when one side is numeric and the
		 * other text (reachable here only via a dynamic operand -- a
		 * subquery / CASE -- since the compile gate forbids a fixed
		 * numeric-vs-text pairing).  Coerce the text side to a number when
		 * it looks numeric, matching SQLite; a non-numeric string keeps
		 * its text value (SQLite then orders number < text). */
		if (e->col) {   /* a dynamic (subquery / CASE) operand */
			int a_num = (a.type == VX_INT || a.type == VX_REAL);
			int b_num = (b.type == VX_INT || b.type == VX_REAL);
			if (a_num && b.type == VX_TEXT) (void)coerce_numeric(&b);
			else if (b_num && a.type == VX_TEXT) (void)coerce_numeric(&a);
		}
		/* SQLite order across storage classes: number < text < blob. */
		c = sort_cmp(&a, &b);
		out->type = VX_INT;
		switch (e->op) {
		case VXO_IS:
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
		if (e->in_sorted != NULL) {
			/* Fast path: binary search the pre-sorted literal list. */
			int lo = 0, hi = e->in_nsorted - 1;
			saw_null = e->in_has_null;
			while (lo <= hi) {
				int mid = lo + (hi - lo) / 2;
				int cr = cmp_vals(&a, &e->in_sorted[mid]);
				if (cr == 0) { res = 1; break; }
				if (cr < 0) hi = mid - 1; else lo = mid + 1;
			}
		} else {
			for (k = 0; k < e->nargs; k++) {
				vx_cell_t v; eval(st, e->args[k], row, arena, &v);
				if (v.type == VX_NULL) { saw_null = 1; continue; }
				if (cmp_vals(&a, &v) == 0) { res = 1; break; }
			}
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

	case VXO_CORRSUBQ: {
		/* Re-run the parameterized correlated subquery through vexec: bind
		 * each ? from the current outer row, run, take the scalar (first
		 * column of the first row, or NULL if no row). */
		vx_cell_t binds[VX_CORR_MAX];
		vx_result_t *r = NULL;
		int i, rc;
		for (i = 0; i < e->nargs && i < VX_CORR_MAX; i++)
			binds[i] = row[e->bindcol[i]];
		rc = vx_run_p(st->db, st->corrsql[e->col], binds, e->nargs, 1, &r, NULL);
		if (rc != 1 || r == NULL) { if (r) vx_result_free(r); out->type = VX_NULL; return; }
		if (vx_result_nrow(r) == 0) { out->type = VX_NULL; vx_result_free(r); return; }
		switch (vx_result_type(r, 0, 0)) {
		case VX_INT:  out->type = VX_INT; out->i = vx_result_int64(r, 0, 0); break;
		case VX_REAL: out->type = VX_REAL; out->r = vx_result_double(r, 0, 0); break;
		case VX_TEXT: case VX_BLOB: {
			const char *b = vx_result_text(r, 0, 0);
			int nb = vx_result_bytes(r, 0, 0);
			uint8_t *p = (uint8_t *)arena_alloc(arena, (size_t)nb + 1);
			if (p == NULL) { out->type = VX_NULL; break; }
			if (nb && b) memcpy(p, b, (size_t)nb);
			p[nb] = '\0';
			out->type = (vx_result_type(r, 0, 0) == VX_TEXT) ? VX_TEXT : VX_BLOB;
			out->bytes = p; out->nbytes = (uint32_t)nb;
			break; }
		default: out->type = VX_NULL; break;
		}
		vx_result_free(r);
		return;
	}

	case VXO_CORRIN: {
		/* operand IN (correlated select).  Bind ? from the current row,
		 * run the parameterized inner through vexec, and test the operand
		 * for membership over every result row with SQL 3-valued logic: a
		 * NULL operand -> NULL; a match -> 1; no match but a NULL element
		 * seen -> NULL; else 0.  NOT IN (lit.i) negates, keeping NULL. */
		vx_cell_t binds[VX_CORR_MAX];
		vx_result_t *r = NULL;
		vx_cell_t a; int i, nr, res = 0, saw_null = 0, rc;
		eval(st, e->a, row, arena, &a);
		if (a.type == VX_NULL) { out->type = VX_NULL; return; }
		for (i = 0; i < e->nargs && i < VX_CORR_MAX; i++)
			binds[i] = row[e->bindcol[i]];
		rc = vx_run_p(st->db, st->corrsql[e->col], binds, e->nargs, 1, &r, NULL);
		if (rc != 1 || r == NULL) { if (r) vx_result_free(r); out->type = VX_NULL; return; }
		nr = vx_result_nrow(r);
		for (i = 0; i < nr; i++) {
			vx_cell_t v;
			memset(&v, 0, sizeof v);
			switch (vx_result_type(r, i, 0)) {
			case VX_INT:  v.type = VX_INT; v.i = vx_result_int64(r, i, 0); break;
			case VX_REAL: v.type = VX_REAL; v.r = vx_result_double(r, i, 0); break;
			case VX_NULL: v.type = VX_NULL; break;
			default: {
				const char *b = vx_result_text(r, i, 0);
				int nb = vx_result_bytes(r, i, 0);
				uint8_t *p = (uint8_t *)arena_alloc(arena, (size_t)nb + 1);
				if (p == NULL) { v.type = VX_NULL; break; }
				if (nb && b) memcpy(p, b, (size_t)nb);
				p[nb] = '\0';
				v.type = (vx_result_type(r, i, 0) == VX_TEXT) ? VX_TEXT : VX_BLOB;
				v.bytes = p; v.nbytes = (uint32_t)nb;
				break; }
			}
			if (v.type == VX_NULL) { saw_null = 1; continue; }
			/* Apply numeric affinity when the operand is numeric and the
			 * subquery value is text (matching SQLite's IN coercion). */
			if ((a.type == VX_INT || a.type == VX_REAL) && v.type == VX_TEXT)
				(void)coerce_numeric(&v);
			if (cmp_vals(&a, &v) == 0) { res = 1; break; }
		}
		vx_result_free(r);
		if (res == 0 && saw_null) { out->type = VX_NULL; return; }
		out->type = VX_INT;
		out->i = e->lit.i ? !res : res;
		return;
	}

	case VXO_EXISTS: {
		/* EXISTS (correlated select): bind ? from the current outer row,
		 * run the parameterized inner, result is 1 if it yields any row
		 * else 0.  Never NULL (SQL EXISTS is always 0/1). */
		vx_cell_t binds[VX_CORR_MAX];
		vx_result_t *r = NULL;
		int i, rc;
		for (i = 0; i < e->nargs && i < VX_CORR_MAX; i++)
			binds[i] = row[e->bindcol[i]];
		rc = vx_run_p(st->db, st->corrsql[e->col], binds, e->nargs, 1, &r, NULL);
		if (rc != 1 || r == NULL) { if (r) vx_result_free(r); out->type = VX_NULL; return; }
		out->type = VX_INT;
		out->i = (vx_result_nrow(r) > 0) ? 1 : 0;
		vx_result_free(r);
		return;
	}

	case VXO_LIKE:
	case VXO_GLOB: {
		/* a LIKE/GLOB b: .a is the value being matched, .b is the
		 * pattern.  3-valued: a NULL operand or NULL pattern yields
		 * NULL.  Delegate the match to the vendored engine so the
		 * semantics are byte-identical to the VDBE (LIKE is ASCII
		 * case-insensitive, GLOB is case-sensitive with glob
		 * wildcards).  Both operands are text-classed by the
		 * compiler; render to NUL-terminated C strings (TEXT bytes
		 * may be a non-terminated slice). */
		vx_cell_t val, pat, ex;
		char *vs, *ps, esc = '\0';
		int m;
		eval(st, e->a, row, arena, &val);
		eval(st, e->b, row, arena, &pat);
		if (val.type == VX_NULL || pat.type == VX_NULL) {
			out->type = VX_NULL; return;
		}
		if (e->op == VXO_LIKE && e->nargs == 1) {   /* LIKE ... ESCAPE */
			eval(st, e->args[0], row, arena, &ex);
			if (ex.type == VX_NULL) { out->type = VX_NULL; return; }
			/* Literal escapes are checked at compile time; a computed
			 * one that is not a single byte yields NULL (SQLite errors). */
			if (ex.type != VX_TEXT || ex.nbytes != 1) { out->type = VX_NULL; return; }
			esc = (char)ex.bytes[0];
		}
		if (val.type != VX_TEXT || pat.type != VX_TEXT) {
			/* The compiler kept both text-classed; a stored value of a
			 * different storage class should not reach here, but guard. */
			out->type = VX_NULL; return;
		}
		ps = (char *)arena_alloc(arena, (size_t)pat.nbytes + 1);
		vs = (char *)arena_alloc(arena, (size_t)val.nbytes + 1);
		if (ps == NULL || vs == NULL) { out->type = VX_NULL; return; }
		if (pat.nbytes) memcpy(ps, pat.bytes, pat.nbytes);
		ps[pat.nbytes] = '\0';
		if (val.nbytes) memcpy(vs, val.bytes, val.nbytes);
		vs[val.nbytes] = '\0';
		if (e->op == VXO_LIKE)
			m = xs_like(ps, vs, esc);
		else
			m = xs_glob(ps, vs);
		out->type = VX_INT; out->i = m;
		return;
	}

	case VXO_FUNC: {
		vx_cell_t a;
		switch (e->func) {
		case VXF_EXT: {
			vx_cell_t av[16];
			int k;
			for (k = 0; k < e->nargs && k < 16; k++)
				eval(st, e->args[k], row, arena, &av[k]);
			vxf_eval(e->col, av, e->nargs, st->now_ms, ext_alloc, arena, out);
			return;
		}
		case VXF_TYPEOF: {
			/* SQLite storage-class name of the argument. */
			const char *tn;
			eval(st, e->args[0], row, arena, &a);
			switch (a.type) {
			case VX_INT:  tn = "integer"; break;
			case VX_REAL: tn = "real";    break;
			case VX_TEXT: tn = "text";    break;
			case VX_BLOB: tn = "blob";    break;
			default:      tn = "null";    break;
			}
			out->type = VX_TEXT;
			out->bytes = (const uint8_t *)tn;
			out->nbytes = (uint32_t)strlen(tn);
			return;
		}
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
				else m = real_text(a.r, tmp, sizeof tmp);
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
		case VXF_NULLIF: {
			/* nullif(a,b): NULL when a == b, else a.  A NULL a returns NULL
			 * (a == b is unknown, so SQLite returns a which is NULL).
			 * Compare with numeric-affinity coercion when one side is a
			 * number and the other numeric-looking text, matching = . */
			vx_cell_t bcell;
			eval(st, e->args[0], row, arena, &a);
			if (a.type == VX_NULL) { out->type = VX_NULL; return; }
			eval(st, e->args[1], row, arena, &bcell);
			if (bcell.type == VX_NULL) { *out = a; return; }
			{
				int a_num = (a.type == VX_INT || a.type == VX_REAL);
				int b_num = (bcell.type == VX_INT || bcell.type == VX_REAL);
				vx_cell_t ca = a, cb = bcell;
				if (a_num && cb.type == VX_TEXT) (void)coerce_numeric(&cb);
				else if (b_num && ca.type == VX_TEXT) (void)coerce_numeric(&ca);
				if (cmp_vals(&ca, &cb) == 0) { out->type = VX_NULL; return; }
			}
			*out = a;
			return;
		}
		case VXF_MIN: case VXF_MAX: {
			/* Scalar min/max: NULL if any argument is NULL; else the
			 * least/greatest by SQLite comparison order. */
			vx_cell_t best; int k, have = 0;
			memset(&best, 0, sizeof best);
			for (k = 0; k < e->nargs; k++) {
				vx_cell_t v; eval(st, e->args[k], row, arena, &v);
				if (v.type == VX_NULL) { out->type = VX_NULL; return; }
				if (!have) { best = v; have = 1; continue; }
				{
					int cmp = cmp_vals(&v, &best);
					if ((e->func == VXF_MIN && cmp < 0) ||
					    (e->func == VXF_MAX && cmp > 0))
						best = v;
				}
			}
			if (!have) { out->type = VX_NULL; return; }
			*out = best;
			return;
		}
		case VXF_SUBSTR: {
			/* substr(X, Y [, Z]): SQLite's substrFunc (func.c) step for
			 * step, in int64 so a huge Y or Z clamps instead of
			 * overflowing.  Characters for TEXT, bytes for BLOB (and a
			 * BLOB in gives a BLOB out). */
			vx_cell_t sy, sz; const uint8_t *sp, *z, *z2, *end;
			uint32_t sn; int64_t len = 0, p1, p2; uint8_t *buf; int isblob;
			double dv; int64_t iv; int ii;
			eval(st, e->args[0], row, arena, &a);
			if (a.type == VX_NULL) { out->type = VX_NULL; return; }
			eval(st, e->args[1], row, arena, &sy);
			if (e->nargs >= 3) {
				eval(st, e->args[2], row, arena, &sz);
				if (sz.type == VX_NULL) { out->type = VX_NULL; return; }
			}
			isblob = a.type == VX_BLOB;
			if (isblob) { sp = a.bytes; sn = a.nbytes; len = sn; }
			else {
				sp = cell_text(&a, arena, &sn);
				if (sp == NULL) { out->type = VX_NULL; return; }
			}
			num_of(&sy, &dv, &iv, &ii);
			p1 = sy.type == VX_NULL ? 0 : ii ? iv : (int64_t)dv;
			if (!isblob && p1 < 0) len = utf8_len(sp, sn);
			if (e->nargs >= 3) {
				num_of(&sz, &dv, &iv, &ii);
				p2 = ii ? iv : (int64_t)dv;
			} else
				p2 = INT64_C(1000000000);      /* SQLITE_LIMIT_LENGTH */
			if (p1 == 0 && sy.type == VX_NULL) { out->type = VX_NULL; return; }
			if (p1 < 0) {
				p1 += len;
				if (p1 < 0) {
					p2 = p2 < 0 ? 0 : p2 + p1;
					p1 = 0;
				}
			} else if (p1 > 0) {
				p1--;
			} else if (p2 > 0) {
				p2--;
			}
			if (p2 < 0) {
				p2 = p2 < -p1 ? p1 : -p2;
				p1 -= p2;
			}
			if (isblob) {
				if (p1 >= len) p1 = p2 = 0;
				else if (p2 > len - p1) p2 = len - p1;
				buf = (uint8_t *)arena_alloc(arena, (size_t)p2 + 1);
				if (buf == NULL) { out->type = VX_NULL; return; }
				if (p2 > 0) memcpy(buf, sp + p1, (size_t)p2);
				out->type = VX_BLOB; out->bytes = buf; out->nbytes = (uint32_t)p2;
				return;
			}
			/* TEXT: skip p1 characters, then take p2 characters. */
			end = sp + sn;
			for (z = sp; z < end && p1 > 0; p1--) {
				z++;
				while (z < end && (*z & 0xC0) == 0x80) z++;
			}
			for (z2 = z; z2 < end && p2 > 0; p2--) {
				z2++;
				while (z2 < end && (*z2 & 0xC0) == 0x80) z2++;
			}
			buf = (uint8_t *)arena_alloc(arena, (size_t)(z2 - z) + 1);
			if (buf == NULL) { out->type = VX_NULL; return; }
			if (z2 > z) memcpy(buf, z, (size_t)(z2 - z));
			buf[z2 - z] = '\0';
			out->type = VX_TEXT; out->bytes = buf; out->nbytes = (uint32_t)(z2 - z);
			return;
		}
		case VXF_INSTR: {
			/* instr(H, N): 1-based char position of the first N in H, else
			 * 0.  NULL if either is NULL.  Byte substring search, converted
			 * to a character index (SQLite counts characters). */
			vx_cell_t nc; const uint8_t *hp, *np; uint32_t hn, nn; uint32_t k;
			eval(st, e->args[0], row, arena, &a);
			if (a.type == VX_NULL) { out->type = VX_NULL; return; }
			eval(st, e->args[1], row, arena, &nc);
			if (nc.type == VX_NULL) { out->type = VX_NULL; return; }
			hp = cell_text(&a, arena, &hn);
			np = cell_text(&nc, arena, &nn);
			if (hp == NULL || np == NULL) { out->type = VX_NULL; return; }
			out->type = VX_INT;
			if (nn == 0) { out->i = 1; return; }   /* SQLite: empty needle -> 1 */
			for (k = 0; k + nn <= hn; k++) {
				if (memcmp(hp + k, np, nn) == 0) {
					out->i = utf8_len(hp, k) + 1;
					return;
				}
			}
			out->i = 0;
			return;
		}
		case VXF_REPLACE: {
			/* replace(S, F, R): every non-overlapping F in S -> R.  NULL if
			 * any argument is NULL; an empty F returns S unchanged. */
			vx_cell_t fc, rc; const uint8_t *sp, *fp, *rp;
			uint32_t sn, fn, rn, i; uint8_t *buf; size_t cap, oi;
			eval(st, e->args[0], row, arena, &a);
			if (a.type == VX_NULL) { out->type = VX_NULL; return; }
			eval(st, e->args[1], row, arena, &fc);
			if (fc.type == VX_NULL) { out->type = VX_NULL; return; }
			eval(st, e->args[2], row, arena, &rc);
			if (rc.type == VX_NULL) { out->type = VX_NULL; return; }
			sp = cell_text(&a, arena, &sn);
			fp = cell_text(&fc, arena, &fn);
			rp = cell_text(&rc, arena, &rn);
			if (sp == NULL || fp == NULL || rp == NULL) { out->type = VX_NULL; return; }
			if (fn == 0) {
				uint8_t *b2 = (uint8_t *)arena_alloc(arena, (size_t)sn + 1);
				if (b2 == NULL) { out->type = VX_NULL; return; }
				if (sn) memcpy(b2, sp, sn);
				b2[sn] = '\0';
				out->type = VX_TEXT; out->bytes = b2; out->nbytes = sn;
				return;
			}
			/* Worst case: every char replaced by R (rn >= fn possible). */
			cap = (size_t)sn * (rn > fn ? rn : 1) + rn + 1;
			buf = (uint8_t *)arena_alloc(arena, cap);
			if (buf == NULL) { out->type = VX_NULL; return; }
			oi = 0;
			for (i = 0; i < sn; ) {
				if (i + fn <= sn && memcmp(sp + i, fp, fn) == 0) {
					if (rn) memcpy(buf + oi, rp, rn);
					oi += rn; i += fn;
				} else {
					buf[oi++] = sp[i++];
				}
			}
			buf[oi] = '\0';
			out->type = VX_TEXT; out->bytes = buf; out->nbytes = (uint32_t)oi;
			return;
		}
		case VXF_TRIM: case VXF_LTRIM: case VXF_RTRIM: {
			/* trim/ltrim/rtrim(S [, chars]): strip leading/trailing runs of
			 * any character in `chars` (default a single space).  Byte-wise
			 * strip against the (ASCII) trim set -- SQLite matches whole
			 * characters but for the common single-byte trim set this is
			 * identical. */
			const uint8_t *sp, *cp = (const uint8_t *)" ";
			uint32_t sn, cn = 1, lo, hi; uint8_t *buf;
			eval(st, e->args[0], row, arena, &a);
			if (a.type == VX_NULL) { out->type = VX_NULL; return; }
			sp = cell_text(&a, arena, &sn);
			if (sp == NULL) { out->type = VX_NULL; return; }
			if (e->nargs >= 2) {
				vx_cell_t cc; eval(st, e->args[1], row, arena, &cc);
				if (cc.type == VX_NULL) { out->type = VX_NULL; return; }
				cp = cell_text(&cc, arena, &cn);
				if (cp == NULL) { out->type = VX_NULL; return; }
			}
			lo = 0; hi = sn;
			if (e->func != VXF_RTRIM) {
				while (lo < hi) {
					uint32_t j; int hit = 0;
					for (j = 0; j < cn; j++) if (sp[lo] == cp[j]) { hit = 1; break; }
					if (!hit) break;
					lo++;
				}
			}
			if (e->func != VXF_LTRIM) {
				while (hi > lo) {
					uint32_t j; int hit = 0;
					for (j = 0; j < cn; j++) if (sp[hi - 1] == cp[j]) { hit = 1; break; }
					if (!hit) break;
					hi--;
				}
			}
			buf = (uint8_t *)arena_alloc(arena, (size_t)(hi - lo) + 1);
			if (buf == NULL) { out->type = VX_NULL; return; }
			if (hi > lo) memcpy(buf, sp + lo, hi - lo);
			buf[hi - lo] = '\0';
			out->type = VX_TEXT; out->bytes = buf; out->nbytes = hi - lo;
			return;
		}
		case VXF_ROUND: {
			/* round(X [, N]): round X to N decimal places (default 0).
			 * SQLite returns a REAL.  NULL X -> NULL. */
			vx_cell_t nc; double x, scale; int64_t nd = 0;
			eval(st, e->args[0], row, arena, &a);
			if (a.type == VX_NULL) { out->type = VX_NULL; return; }
			{ double d; int64_t i; int ii; num_of(&a, &d, &i, &ii); x = d; }
			if (e->nargs >= 2) {
				eval(st, e->args[1], row, arena, &nc);
				if (nc.type == VX_NULL) { out->type = VX_NULL; return; }
				{ double d; int64_t i; int ii; num_of(&nc, &d, &i, &ii); nd = ii ? i : (int64_t)d; }
			}
			if (nd < 0) nd = 0;
			if (nd > 30) nd = 30;
			scale = pow(10.0, (double)nd);
			/* SQLite rounds half away from zero. */
			if (x < 0) x = -floor(-x * scale + 0.5) / scale;
			else       x = floor(x * scale + 0.5) / scale;
			out->type = VX_REAL; out->r = x;
			return;
		}
		case VXF_CAST: {
			/* CAST(arg AS type) -- .col holds the target class. */
			eval(st, e->args[0], row, arena, &a);
			if (a.type == VX_NULL) { out->type = VX_NULL; return; }
			switch ((enum vx_cast_class)e->col) {
			case VX_CAST_TEXT: {
				uint32_t tn; const uint8_t *tp;
				if (a.type == VX_TEXT) { *out = a; return; }
				tp = cell_text(&a, arena, &tn);
				if (tp == NULL) { out->type = VX_NULL; return; }
				out->type = VX_TEXT; out->bytes = tp; out->nbytes = tn;
				return;
			}
			case VX_CAST_INT: {
				if (a.type == VX_INT) { *out = a; return; }
				if (a.type == VX_REAL) {
					out->type = VX_INT; out->i = (int64_t)a.r;   /* trunc toward zero */
					return;
				}
				/* TEXT/BLOB -> integer prefix (SQLite reads the leading
				 * numeric run; a real prefix truncates toward zero). */
				out->type = VX_INT; out->i = cast_text_to_int(a.bytes, a.nbytes);
				return;
			}
			case VX_CAST_REAL: {
				if (a.type == VX_REAL) { *out = a; return; }
				if (a.type == VX_INT) { out->type = VX_REAL; out->r = (double)a.i; return; }
				out->type = VX_REAL; out->r = cast_text_to_real(a.bytes, a.nbytes);
				return;
			}
			case VX_CAST_NUMERIC: {
				/* SQLite NUMERIC: a number stays as-is; text parses its
				 * leading numeric prefix -- INT if that prefix is integral,
				 * else REAL ('123.5abc' -> 123.5, '12abc' -> 12). */
				double d; int64_t iv;
				if (a.type == VX_INT || a.type == VX_REAL) { *out = a; return; }
				if (a.type == VX_TEXT) {
					vx_cell_t t = a;
					(void)coerce_numeric(&t);
					if (t.type != VX_TEXT) { *out = t; return; }   /* fully numeric */
				}
				d = cast_text_to_real(a.bytes, a.nbytes);
				iv = (int64_t)d;
				if ((double)iv == d) { out->type = VX_INT; out->i = iv; }
				else { out->type = VX_REAL; out->r = d; }
				return;
			}
			default:   /* VX_CAST_BLOB: reject -- the wire protocol encodes a
			           * BLOB result (base64) so it cannot compare byte-equal to
			           * SQLite's raw blob here; leave to a clean rejection. */
				out->type = VX_NULL;
				return;
			}
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
		/* Mix every bit down (splitmix64's finalizer): a small integer
		 * as a double has all-zero low mantissa bits, so a bare multiply
		 * left the low bits -- the bucket index of every table here --
		 * constant, and all integer keys shared ONE bucket (a 20,000-key
		 * count(DISTINCT) cost 440 ms, quadratic).  -0.0 hashes as 0.0:
		 * they compare equal. */
		uint64_t bits;
		if (d == 0.0) d = 0.0;
		memcpy(&bits, &d, 8);
		bits ^= bits >> 30; bits *= 0xbf58476d1ce4e5b9ULL;
		bits ^= bits >> 27; bits *= 0x94d049bb133111ebULL;
		bits ^= bits >> 31;
		return bits;
	}
}

static int
key_eq(const vx_cell_t *a, const vx_cell_t *b)
{
	if (a->type == VX_INT && b->type == VX_INT)
		return a->i == b->i;
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
	h->nbucket = 1024;             /* a power of two: htab_group masks */
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
	/* nbucket is a power of two (htab_init); fold the high bits in, as
	 * the multiplicative cell hash puts its entropy there. */
	b = (uint32_t)((hv ^ (hv >> 32)) & (uint64_t)(h->nbucket - 1));
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

/* Add a non-NULL value to a count(DISTINCT) set (lazily allocating the
 * set and its buckets from `arena`).  Returns 0 on success, -1 on OOM.
 * A value already present (key_eq) is ignored; a new one bumps count.
 * Uses the same hash/equality as group keys, so DISTINCT-ness matches
 * GROUP BY grouping -- which is how SQLite defines count(DISTINCT). */
static int
dset_add(vx_dset_t **pset, const vx_cell_t *v, struct vx_arena_blk **arena)
{
	vx_dset_t *s = *pset;
	uint64_t hv;
	uint32_t b;
	vx_dnode_t *n;
	if (s == NULL) {
		s = (vx_dset_t *)arena_alloc(arena, sizeof *s);
		if (s == NULL) return -1;
		s->nbucket = 256;
		s->count = 0;
		s->buckets = (vx_dnode_t **)arena_alloc(arena,
		    sizeof(vx_dnode_t *) * (size_t)s->nbucket);
		if (s->buckets == NULL) return -1;
		memset(s->buckets, 0, sizeof(vx_dnode_t *) * (size_t)s->nbucket);
		*pset = s;
	}
	hv = cell_hash(v);
	b = (uint32_t)(hv % (uint64_t)s->nbucket);
	for (n = s->buckets[b]; n; n = n->next)
		if (n->hash == hv && key_eq(&n->val, v)) return 0;   /* already seen */
	n = (vx_dnode_t *)arena_alloc(arena, sizeof *n);
	if (n == NULL) return -1;
	n->hash = hv;
	if (cell_dup(arena, v, &n->val) != 0) return -1;
	n->next = s->buckets[b];
	s->buckets[b] = n;
	s->count++;
	return 0;
}

/* Fold one input value into an accumulator. */
static void
acc_step(vx_acc_t *a, enum vx_agg_kind kind, const vx_cell_t *v,
         struct vx_arena_blk **arena)
{
	if (kind == VXA_COUNT_STAR) { a->cnt++; return; }
	if (v->type == VX_NULL) return;        /* all others ignore NULL inputs */
	if (kind == VXA_COUNT_DISTINCT) {
		/* Distinct non-NULL inputs: the set tracks the count. */
		(void)dset_add(&a->dset, v, arena);
		return;
	}
	if (kind == VXA_GROUP_CONCAT) {
		/* Append the separator (except before the first element) then the
		 * value rendered to text, into a growable arena buffer.  The
		 * separator (gc_sep/gc_seplen) is set on the outcol before the
		 * first step; default ',' when unset. */
		char tmp[64]; const char *vp; uint32_t vn;
		const char *sep = a->gc_sep ? a->gc_sep : ",";
		uint32_t seplen = a->gc_sep ? a->gc_seplen : 1;
		if (v->type == VX_TEXT || v->type == VX_BLOB) { vp = (const char *)v->bytes; vn = v->nbytes; }
		else if (v->type == VX_INT) { vn = (uint32_t)snprintf(tmp, sizeof tmp, "%lld", (long long)v->i); vp = tmp; }
		else { vn = (uint32_t)real_text(v->r, tmp, sizeof tmp); vp = tmp; }
		{
			uint32_t add = (a->seen ? seplen : 0) + vn;
			if (a->gc_len + add + 1 > a->gc_cap) {
				uint32_t nc = a->gc_cap ? a->gc_cap * 2 : 64;
				char *nb;
				while (nc < a->gc_len + add + 1) nc *= 2;
				nb = (char *)arena_alloc(arena, nc);
				if (nb == NULL) return;
				if (a->gc_len) memcpy(nb, a->gc_buf, a->gc_len);
				a->gc_buf = nb; a->gc_cap = nc;
			}
			if (a->seen && seplen) { memcpy(a->gc_buf + a->gc_len, sep, seplen); a->gc_len += seplen; }
			if (vn) memcpy(a->gc_buf + a->gc_len, vp, vn);
			a->gc_len += vn;
			a->gc_buf[a->gc_len] = '\0';
		}
		a->cnt++;
		a->seen = 1;
		return;
	}
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
	if (kind == VXA_COUNT_DISTINCT) {
		/* Union the source distinct set into the dest by re-adding each
		 * source value (dedup against what dest already has). */
		int bi;
		if (s->dset == NULL) return;
		for (bi = 0; bi < s->dset->nbucket; bi++) {
			vx_dnode_t *n;
			for (n = s->dset->buckets[bi]; n; n = n->next)
				(void)dset_add(&d->dset, &n->val, arena);
		}
		return;
	}
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
	case VXA_COUNT_DISTINCT:
		out->type = VX_INT; out->i = a->dset ? a->dset->count : 0; return;
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
	case VXA_GROUP_CONCAT:
		if (!a->seen) { out->type = VX_NULL; return; }   /* no rows -> NULL */
		out->type = VX_TEXT;
		out->bytes = (const uint8_t *)(a->gc_buf ? a->gc_buf : "");
		out->nbytes = a->gc_len;
		return;
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

static int vx_try_prepare_agg(struct xsql *db, sql_arena_t *ast,
                              const sql_select_t *sel, const char *tabbuf,
                              const vx_cell_t *binds, int nbinds,
                              vx_stmt_t **out, char **errmsg);
static int vx_try_prepare_join(struct xsql *db, sql_arena_t *ast,
                               const sql_select_t *sel,
                               const vx_cell_t *binds, int nbinds,
                               vx_stmt_t **out, char **errmsg);
static int vx_try_prepare_njoin(struct xsql *db, sql_arena_t *ast,
                                const sql_select_t *sel,
                                const vx_cell_t *binds, int nbinds,
                                vx_stmt_t **out, char **errmsg);
static int vx_try_prepare_mat(struct xsql *db, sql_arena_t *ast,
    const sql_select_t *sel, const sql_select_t *src_sel, const char *src_sql,
    const sql_str_t *rename, int nrename, const vx_cell_t *binds, int nbinds,
    vx_stmt_t **out, char **errmsg);
static int run_setop_sel(struct xsql *db, const sql_select_t *lhs,
    const vx_cell_t *binds, int nbinds, vx_result_t **res, char **errmsg);
static int sel_is_agg(const sql_select_t *sel);
static int vx_try_prepare_binds(struct xsql *db, const char *sql,
                                const vx_cell_t *binds, int nbinds,
                                vx_stmt_t **out, char **errmsg);
static int vx_prepare_select(struct xsql *db, sql_arena_t *ast,
                             const sql_select_t *sel,
                             const vx_cell_t *binds, int nbinds,
                             vx_stmt_t **out, char **errmsg);

/* ---- access-path chooser (sqlite-audit gap #4) -------------------- *
 *
 * Works on the COMPILED predicate: each AND-conjunct "col op v" where v
 * is a constant for the scan (no column / correlated reference: a
 * literal, a bound ?, "? - 20", a spliced uncorrelated subquery) is a
 * sargable term.  The comparison literal has already been coerced by
 * cmp_affinity_gate, so a bound value compares exactly as the row
 * filter would; the full WHERE is re-checked on every fetched row, so a
 * path can only make the scan look at fewer rows, never change the
 * answer.  Candidate paths: the rowid (INTEGER PRIMARY KEY) bounds, and
 * for every index of the table its longest equality prefix plus an
 * optional range on the next column. */

struct vx_apterm {
	int                   col;    /* source column (VXO_COL index) */
	int                   op;     /* VXO_EQ / LT / LE / GT / GE */
	const vx_expr_t      *val;
};

/* Which compiled columns a bound value may read: in a join, the columns
 * of the sides already placed (outer rows); single table: none.  The
 * scanned side owns combined-row slots [base, base + n). */
struct ap_scope {
	int        base, n;
	const int *side_of;     /* combined col -> side, or NULL */
	unsigned   placed;      /* bit s: side s is bound */
};

/* Is `e` constant for one scan of the side (it reads no column of the
 * side itself, nor of a side not yet bound)? */
static int
ap_const(const vx_expr_t *e, const struct ap_scope *sc)
{
	int i;
	if (e == NULL) return 1;
	switch (e->op) {
	case VXO_COL:
		return sc->side_of != NULL &&
		    ((sc->placed >> sc->side_of[e->col]) & 1u) != 0;
	case VXO_CORRSUBQ: case VXO_CORRIN: case VXO_EXISTS:
		return 0;
	default: break;
	}
	if (!ap_const(e->a, sc) || !ap_const(e->b, sc)) return 0;
	for (i = 0; i < e->nargs; i++)
		if (e->args && !ap_const(e->args[i], sc)) return 0;
	return 1;
}

/* Flip a comparison for "v op col" -> "col op' v". */
static int
ap_flip(int op)
{
	switch (op) {
	case VXO_LT: return VXO_GT;
	case VXO_LE: return VXO_GE;
	case VXO_GT: return VXO_LT;
	case VXO_GE: return VXO_LE;
	default: return op;
	}
}

static int
ap_own(const vx_expr_t *e, const struct ap_scope *sc)
{
	return e->op == VXO_COL && e->col >= sc->base && e->col < sc->base + sc->n;
}

static void
ap_terms(const vx_expr_t *e, struct vx_apterm *t, int *nt,
         const struct ap_scope *sc)
{
	const vx_expr_t *col, *val;
	int op;
	if (e == NULL || *nt >= VX_AP_TERMMAX) return;
	if (e->op == VXO_AND) {
		ap_terms(e->a, t, nt, sc);
		ap_terms(e->b, t, nt, sc);
		return;
	}
	op = e->op;
	if (op != VXO_EQ && op != VXO_LT && op != VXO_LE && op != VXO_GT &&
	    op != VXO_GE) return;
	if (e->col) return;                 /* dynamic coercion at eval: skip */
	if (ap_own(e->a, sc) && ap_const(e->b, sc)) { col = e->a; val = e->b; }
	else if (ap_own(e->b, sc) && ap_const(e->a, sc)) {
		col = e->b; val = e->a; op = ap_flip(op);
	} else return;
	t[*nt].col = col->col - sc->base; t[*nt].op = op; t[*nt].val = val;
	(*nt)++;
}

/* Source-column index (srcrow slot) of a column name, or -1. */
static int
ap_col_of(char names[][64], int n, const char *name)
{
	int i;
	for (i = 0; i < n; i++)
		if (strcasecmp(names[i], name) == 0) return i;
	return -1;
}

/*
 * Choose the access path for one table scan with source columns
 * names[0..n) (record origins pay[]) and compiled predicate `filter`.
 * `order`/`norder`/`desc` are ORDER BY keys over the SOURCE row (NULL
 * entries are not source columns), used to prefer -- and mark -- a path
 * whose walk yields that order; minmax_col >= 0 asks for a walk ordered
 * by that column (MIN/MAX).  Fills *ap.
 */
static int
ap_choose_in(struct xsql *db, const char *table, char names[][64], int n,
             const int *pay, const vx_expr_t *filter, vx_expr_t *const *order,
             int norder, const int *desc, int minmax_col,
             const struct ap_scope *sc, vx_apath_t *ap)
{
	struct vx_apterm t[VX_AP_TERMMAX];
	xstore_index_info_t *ix = NULL;
	bt_t *bt = xstore_bt_of(db);
	int nt = 0, i, j, nix = 0, best_score = 0;
	int pk = -1;

	memset(ap, 0, sizeof *ap);
	ap->kind = VX_AP_SCAN;
	if (bt == NULL) return 0;
	ap_terms(filter, t, &nt, sc);
	for (i = 0; i < n; i++) if (pay[i] == -1) { pk = i; break; }

	/* Rowid path: every term on the rowid column. */
	if (pk >= 0) {
		int has_eq = 0, has_rng = 0;
		for (i = 0; i < nt && ap->nrt < 8; i++)
			if (t[i].col == pk) {
				ap->rtop[ap->nrt] = t[i].op;
				ap->rtv[ap->nrt++] = t[i].val;
				if (t[i].op == VXO_EQ) has_eq = 1; else has_rng = 1;
			}
		if (has_eq) best_score = 1000;
		else if (has_rng) best_score = 50;
		if (best_score > 0) {
			ap->kind = VX_AP_ROWID;
			/* A rowid walk is ascending: ORDER BY rowid ASC is free. */
			ap->ordered = (norder == 1 && order[0] != NULL &&
			    order[0]->op == VXO_COL && order[0]->col == pk &&
			    !(desc[0] & 1));
		}
	}
	if (best_score >= 1000) return best_score;   /* a rowid point beats any index */

	ix = (xstore_index_info_t *)malloc(sizeof *ix * 32);
	if (ix == NULL) return best_score;
	nix = xstore_index_list(bt, table, ix, 32);
	for (j = 0; j < nix; j++) {
		vx_apath_t c;
		int k, score, neq = 0, rcol, ord = 0, rev = 0;
		memset(&c, 0, sizeof c);
		c.kind = VX_AP_INDEX;
		c.idx_id = ix[j].idx_id;
		for (k = 0; k < ix[j].ncol && k < VX_AP_EQMAX; k++) {
			int sc = ap_col_of(names, n, ix[j].col[k]), m;
			const vx_expr_t *v = NULL;
			if (sc < 0) break;
			for (m = 0; m < nt; m++)
				if (t[m].col == sc && t[m].op == VXO_EQ) { v = t[m].val; break; }
			if (v == NULL) break;
			c.eq[neq++] = v;
		}
		c.neq = neq;
		/* An optional range on the next (ASC) column. */
		rcol = -1;
		if (neq < ix[j].ncol && !((ix[j].descmask >> neq) & 1u)) {
			int m;
			rcol = ap_col_of(names, n, ix[j].col[neq]);
			for (m = 0; rcol >= 0 && m < nt; m++) {
				if (t[m].col != rcol) continue;
				if ((t[m].op == VXO_GT || t[m].op == VXO_GE) && c.lo == NULL) {
					c.lo = t[m].val; c.lo_incl = (t[m].op == VXO_GE);
				} else if ((t[m].op == VXO_LT || t[m].op == VXO_LE) &&
				    c.hi == NULL) {
					c.hi = t[m].val; c.hi_incl = (t[m].op == VXO_LE);
				}
			}
		}
		/* Order the walk provides: index columns neq.. in key order (a
		 * reverse walk flips every direction). */
		if (norder > 0 && neq + norder <= ix[j].ncol) {
			int ok = 1, dir = -1;
			for (k = 0; k < norder && ok; k++) {
				int ic = neq + k;
				int kdesc = (desc[k] & 1) ^ (int)((ix[j].descmask >> ic) & 1u);
				if (order[k] == NULL || order[k]->op != VXO_COL ||
				    (desc[k] & 2) ||   /* explicit NULLS placement */
				    order[k]->col != ap_col_of(names, n, ix[j].col[ic]))
					ok = 0;
				else if (dir < 0) dir = kdesc;
				else if (dir != kdesc) ok = 0;
			}
			if (ok) { ord = 1; rev = dir; }
		}
		if (minmax_col >= 0 && neq < ix[j].ncol &&
		    ap_col_of(names, n, ix[j].col[neq]) == minmax_col) {
			ord = 1;
			rev = (int)((ix[j].descmask >> neq) & 1u);  /* ascending value walk */
		}
		if (neq == 0 && c.lo == NULL && c.hi == NULL && !ord) continue;
		/* Score: a fully bound unique key is a point; then equality
		 * columns; a range and a provided order break ties. */
		if (ix[j].unique && neq == ix[j].ncol) score = 900;
		else score = 100 * neq + ((c.lo || c.hi) ? 40 : 0) + (ord ? 20 : 0) +
		    (ix[j].unique ? 5 : 0) - ix[j].ncol;
		if (neq == 0 && c.lo == NULL && c.hi == NULL) score = 10;  /* order only */
		if (score > best_score) {
			c.reverse = rev;
			c.ordered = ord;
			*ap = c;
			best_score = score;
		}
	}
	free(ix);
	return best_score;
}

/* Single-table form: the bounds may read no column. */
static void
ap_choose(struct xsql *db, const char *table, char names[][64], int n,
          const int *pay, const vx_expr_t *filter, vx_expr_t *const *order,
          int norder, const int *desc, int minmax_col, vx_apath_t *ap)
{
	struct ap_scope sc;
	sc.base = 0; sc.n = n; sc.side_of = NULL; sc.placed = 0;
	(void)ap_choose_in(db, table, names, n, pay, filter, order, norder, desc,
	    minmax_col, &sc, ap);
}

/* a AND b (either may be NULL). */
static vx_expr_t *
ap_and(struct vx_compiler *c, vx_expr_t *a, vx_expr_t *b)
{
	vx_expr_t *n;
	if (a == NULL) return b;
	if (b == NULL) return a;
	n = expr_node(c, VXO_AND);
	if (n) { n->a = a; n->b = b; }
	return n;
}

/* Does the path seek by an equality (a rowid point or an index prefix)? */
static int
ap_is_seek(const vx_apath_t *ap)
{
	int i;
	if (ap->kind == VX_AP_INDEX) return ap->neq > 0;
	if (ap->kind == VX_AP_ROWID)
		for (i = 0; i < ap->nrt; i++)
			if (ap->rtop[i] == VXO_EQ) return 1;
	return 0;
}

/*
 * Plan an index nested-loop INNER join over the jc sides with row
 * sources srcs[] and full predicate `cond` (WHERE AND every ON, over the
 * combined row).  Greedy: the outermost side is the one with the best
 * standalone path; each next side is the one with the best path given
 * the sides already placed, and must be an equality seek (else the hash
 * join, which reads every side once, stays).  On success sets st->nl and
 * st->filter = cond and returns 1.
 */
static int
nl_plan(struct xsql *db, struct vx_stmt *st, struct vx_joinctx *jc,
        const vx_jsrc_t *const *srcs, vx_expr_t *cond)
{
	vx_nlplan_t *nl;
	int side_of[64], level, s, k, first;
	unsigned placed = 0, tried = 0;
	if (cond == NULL || jc->nside < 2 || jc->nside > VX_JOIN_MAX) return 0;
	for (s = 0; s < jc->nside; s++)
		for (k = 0; k < jc->col[s].n; k++) {
			if (jc->base[s] + k >= 64) return 0;
			side_of[jc->base[s] + k] = s;
		}
	nl = (vx_nlplan_t *)calloc(1, sizeof *nl);
	if (nl == NULL) return 0;
	nl->nside = jc->nside;
	for (s = 0; s < jc->nside; s++) {
		snprintf(nl->table[s], sizeof nl->table[s], "%s", jc->tab[s]);
		memcpy(nl->pay[s], srcs[s]->pay, sizeof nl->pay[s]);
		nl->ncol[s] = jc->col[s].n;
		nl->base[s] = jc->base[s];
	}
	/* Try each side as the outermost (best standalone path first); the
	 * rest are placed greedily and must each seek. */
	for (first = 0; first < jc->nside; first++) {
		int top = -1, top_score = -1, ok = 1;
		for (s = 0; s < jc->nside; s++) {
			struct ap_scope sc;
			vx_apath_t a;
			int score;
			if ((tried >> s) & 1u) continue;
			sc.base = jc->base[s]; sc.n = jc->col[s].n;
			sc.side_of = side_of; sc.placed = 0;
			score = ap_choose_in(db, jc->tab[s], jc->col[s].names, jc->col[s].n,
			    srcs[s]->pay, cond, NULL, 0, NULL, -1, &sc, &a);
			if (score > top_score) { top_score = score; top = s; nl->ap[s] = a; }
		}
		if (top < 0) break;
		tried |= 1u << top;
		nl->ord[0] = top;
		placed = 1u << top;
		for (level = 1; level < jc->nside && ok; level++) {
			int best = -1, best_score = -1;
			vx_apath_t best_ap;
			memset(&best_ap, 0, sizeof best_ap);
			for (s = 0; s < jc->nside; s++) {
				struct ap_scope sc;
				vx_apath_t a;
				int score;
				if ((placed >> s) & 1u) continue;
				sc.base = jc->base[s]; sc.n = jc->col[s].n;
				sc.side_of = side_of; sc.placed = placed;
				score = ap_choose_in(db, jc->tab[s], jc->col[s].names,
				    jc->col[s].n, srcs[s]->pay, cond, NULL, 0, NULL, -1, &sc, &a);
				if (!ap_is_seek(&a)) continue;
				if (score > best_score) { best_score = score; best = s; best_ap = a; }
			}
			if (best < 0) { ok = 0; break; }   /* an inner side would scan */
			nl->ord[level] = best;
			nl->ap[best] = best_ap;
			placed |= 1u << best;
		}
		if (ok) break;
	}
	if (placed != (1u << jc->nside) - 1u) { free(nl); return 0; }
	st->nl = nl;
	st->filter = cond;
	return 1;
}

/* An expression value for a bound: evaluated against no row (it is
 * constant for the scan).  NULL -> the comparison can never be true. */
static int
ap_val(const struct vx_stmt *st, const vx_expr_t *e, vx_cell_t *out,
       struct vx_arena_blk **arena)
{
	eval(st, e, st->srcrow, arena, out);   /* a join bound reads outer slots */
	return out->type != VX_NULL;
}

/* Convert a bound cell to an index value; 0 if not encodable. */
static int
ap_xval(const vx_cell_t *c, xstore_val_t *x)
{
	memset(x, 0, sizeof *x);
	switch (c->type) {
	case VX_INT:  x->cls = XSTORE_C_INT; x->i = c->i; return 1;
	case VX_REAL: x->cls = XSTORE_C_REAL; x->r = c->r; return 1;
	case VX_TEXT: x->cls = XSTORE_C_TEXT; x->p = c->bytes; x->n = (int)c->nbytes; return 1;
	case VX_BLOB: x->cls = XSTORE_C_BLOB; x->p = c->bytes; x->n = (int)c->nbytes; return 1;
	default: return 0;
	}
}

/* Open the row source for path `ap` over st->table.  A rowid bound that
 * is not an integer is folded conservatively (a REAL rounds outward, a
 * TEXT/BLOB bound leaves that end open; the filter still decides). */
static int
src_open(struct vx_stmt *st, const char *table, const vx_apath_t *ap,
         vx_src_t *s)
{
	struct vx_arena_blk *ar = NULL;
	vx_cell_t v;
	int i, rc = 0;
	memset(s, 0, sizeof *s);
	s->open = 1;
	if (ap->kind == VX_AP_INDEX) {
		xstore_val_t eq[VX_AP_EQMAX], lo, hi;
		vx_cell_t eqc[VX_AP_EQMAX], loc, hic;
		int haslo = 0, hashi = 0;
		for (i = 0; i < ap->neq; i++) {
			if (!ap_val(st, ap->eq[i], &eqc[i], &ar)) { s->empty = 1; goto out; }
			if (!ap_xval(&eqc[i], &eq[i])) goto scan;
		}
		if (ap->lo != NULL) {
			if (!ap_val(st, ap->lo, &loc, &ar)) { s->empty = 1; goto out; }
			if (!ap_xval(&loc, &lo)) goto scan;
			haslo = 1;
		}
		if (ap->hi != NULL) {
			if (!ap_val(st, ap->hi, &hic, &ar)) { s->empty = 1; goto out; }
			if (!ap_xval(&hic, &hi)) goto scan;
			hashi = 1;
		}
		s->iscan = xstore_iscan_open((struct xsql *)st->db, table,
		    ap->idx_id, eq, ap->neq, haslo ? &lo : NULL, ap->lo_incl,
		    hashi ? &hi : NULL, ap->hi_incl, ap->reverse);
		if (s->iscan != NULL) {
			s->ordered = ap->ordered;
			/* A walk that stops early (MIN/MAX, or LIMIT over the
			 * index order) should not read rows ahead it never takes. */
			if (ap == &st->ap && st->ap_minmax)
				xstore_iscan_hint(s->iscan, 1);
			else if (ap == &st->ap && st->agg == NULL &&
			    st->limit >= 0 && !st->distinct &&
			    (ap->ordered || st->norder == 0))
				xstore_iscan_hint(s->iscan, st->offset + st->limit);
			goto out;
		}
		goto scan;
	}
	if (ap->rowmax) {
		int64_t m;
		if (!xstore_max_live_rowid((struct xsql *)st->db, table, st->snap, 0, 0, &m)) {
			s->empty = 1;
			goto out;
		}
		s->scan = xstore_scan_open_txn((struct xsql *)st->db, table,
		    st->snap, m, 1, m, 1);
		if (s->scan == NULL) rc = -1;
		s->ordered = 1;
		goto out;
	}
	if (ap->kind == VX_AP_ROWID) {
		int64_t lo = 0, hi = 0;
		int has_lo = 0, has_hi = 0;
		for (i = 0; i < ap->nrt; i++) {
			int64_t fl, ce, nlo = 0, nhi = 0;
			int setlo = 0, sethi = 0;
			if (!ap_val(st, ap->rtv[i], &v, &ar)) { s->empty = 1; goto out; }
			if (v.type == VX_INT) {
				fl = ce = v.i;
			} else if (v.type == VX_REAL && v.r > -9.2e18 && v.r < 9.2e18) {
				fl = (int64_t)floor(v.r); ce = (int64_t)ceil(v.r);
			} else continue;               /* text/blob bound: leave open */
			switch (ap->rtop[i]) {
			case VXO_EQ:
				if (fl != ce) { s->empty = 1; goto out; }
				nlo = nhi = fl; setlo = sethi = 1; break;
			case VXO_GT:
				if (fl == INT64_MAX) { s->empty = 1; goto out; }
				nlo = fl + 1; setlo = 1; break;
			case VXO_GE: nlo = ce; setlo = 1; break;
			case VXO_LT:
				if (ce == INT64_MIN) { s->empty = 1; goto out; }
				nhi = ce - 1; sethi = 1; break;
			case VXO_LE: nhi = fl; sethi = 1; break;
			default: break;
			}
			if (setlo && (!has_lo || nlo > lo)) { lo = nlo; has_lo = 1; }
			if (sethi && (!has_hi || nhi < hi)) { hi = nhi; has_hi = 1; }
		}
		if (has_lo && has_hi && lo > hi) { s->empty = 1; goto out; }
		s->scan = xstore_scan_open_txn((struct xsql *)st->db, table,
		    st->snap, lo, has_lo, hi, has_hi);
		if (s->scan == NULL) rc = -1;
		s->ordered = ap->ordered;   /* txn scans merge buffered rows in order */
		goto out;
	}
scan:
	s->scan = xstore_scan_open_txn((struct xsql *)st->db, table, st->snap,
	    0, 0, 0, 0);
	if (s->scan == NULL) rc = -1;
	/* A full scan walks rowids ascending (txn scans merge buffered rows
	 * in order); an index path that fell back here does not keep ITS
	 * order. */
	if (ap->kind == VX_AP_SCAN) s->ordered = ap->ordered;
out:
	arena_free(ar);
	return rc;
}

/* Next candidate row: 1 with the row, 0 at the end, <0 on error. */
static int
src_next(vx_src_t *s, int64_t *rowid, const uint8_t **rec, int *reclen)
{
	if (s->empty) return 0;
	if (s->iscan != NULL) return xstore_iscan_next(s->iscan, rowid, rec, reclen);
	if (s->scan != NULL) return xstore_scan_next(s->scan, rowid, rec, reclen);
	return 0;
}

static void
src_close(vx_src_t *s)
{
	if (s->iscan != NULL) xstore_iscan_close(s->iscan);
	if (s->scan != NULL) xstore_scan_close(s->scan);
	memset(s, 0, sizeof *s);
}

/* Lazily open the statement's single-table source on first use. */
static int
st_src_next(struct vx_stmt *st, int64_t *rowid, const uint8_t **rec, int *reclen)
{
	if (!st->src.open && src_open(st, st->table, &st->ap, &st->src) != 0) return -1;
	return src_next(&st->src, rowid, rec, reclen);
}

/* The ORDER BY keys of a plain single-table plan as source-column
 * expressions (an output-column reference resolves through its
 * projection), for ap_choose. */
static void
ap_order_keys(const struct vx_stmt *st, vx_expr_t **keys)
{
	int k;
	for (k = 0; k < st->norder && k < 16; k++) {
		if (st->order_key && st->order_key[k] != NULL) keys[k] = st->order_key[k];
		else if (st->order_outcol && st->order_outcol[k] > 0 &&
		    st->order_outcol[k] <= st->nout)
			keys[k] = st->proj[st->order_outcol[k] - 1];
		else keys[k] = NULL;
	}
}

int
vx_try_prepare(struct xsql *db, const char *sql, vx_stmt_t **out, char **errmsg)
{
	return vx_try_prepare_binds(db, sql, NULL, 0, out, errmsg);
}

static int
vx_try_prepare_binds(struct xsql *db, const char *sql,
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
static int vx_prepare_select_body(struct xsql *db, sql_arena_t *ast,
    const sql_select_t *sel, const vx_cell_t *binds, int nbinds,
    vx_stmt_t **out, char **errmsg);

/* A prepare keeps all of its state in its own frames (the correlation
 * state is in struct namevec), so prepares run concurrently.  They ran
 * under one global recursive amutex while that state was file-scoped,
 * which serialized every SELECT prepare in the process, including the
 * scalar subquery a noreuse plan runs at each execution: at 64 clients
 * the ledger blend's analytic query held it 0.5-2 ms per run and 299 of
 * 320 sampled client stacks waited on it (34k txn/s; 135k without). */
static int
vx_prepare_select(struct xsql *db, sql_arena_t *ast, const sql_select_t *sel,
                  const vx_cell_t *binds, int nbinds,
                  vx_stmt_t **out, char **errmsg)
{
	return vx_prepare_select_body(db, ast, sel, binds, nbinds, out, errmsg);
}

/* test_prepare_concurrent parks one prepare here (it #includes this file). */
#ifndef VX_PREPARE_TEST_HOOK
#define VX_PREPARE_TEST_HOOK() ((void)0)
#endif

static int
vx_prepare_select_body(struct xsql *db, sql_arena_t *ast, const sql_select_t *sel,
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

	VX_PREPARE_TEST_HOOK();
	if (out) *out = NULL;
	if (errmsg) *errmsg = NULL;
	if (sel == NULL) goto fallback;

	/* Common gates: no CTE/DISTINCT/HAVING.  GROUP BY is
	 * allowed only on the aggregation path; ORDER BY / LIMIT / OFFSET
	 * are allowed only on the non-aggregating path (handled below).
	 * HAVING is allowed only on the aggregation path (gated there). */
	/* A non-recursive single CTE referenced once by name in the outer
	 * FROM is handled by materializing its SELECT (like a view) and
	 * renaming its output columns to the CTE's column list.  Anything
	 * more (WITH RECURSIVE, multiple CTEs, a CTE joined/aggregated) falls
	 * back. */
	if (sel->with) {
		const sql_cte_t *cte = sel->with;
		const sql_src_t *fsrc = sel->from;
		if (sel->with_recursive || cte->next != NULL) goto fallback;
		if (cte->select == NULL || cte->src == NULL || cte->srclen == 0) goto fallback;
		if (fsrc == NULL || fsrc->next != NULL || fsrc->subquery != NULL) goto fallback;
		if (!(fsrc->table.len == cte->name.len &&
		      strncasecmp(fsrc->table.p, cte->name.p, cte->name.len) == 0))
			goto fallback;   /* outer FROM is not this CTE */
		{
			sql_str_t rename[32];
			int nren = 0;
			const sql_exprlist_item_t *ci;
			for (ci = cte->cols ? cte->cols->head : NULL; ci; ci = ci->next) {
				if (nren >= 32 || ci->expr == NULL ||
				    ci->expr->op != SX_E_COLUMN) goto fallback;
				rename[nren++] = ci->expr->name[ci->expr->nname - 1];
			}
			return vx_try_prepare_mat(db, ast, sel, cte->select, NULL,
			                          cte->cols ? rename : NULL, nren,
			                          binds, nbinds, out, errmsg);
		}
	}
	if (sel->having && sel->group == NULL) {
		/* HAVING without GROUP BY -- only valid with an aggregate select
		 * list, which the agg path detects below; a HAVING here that is
		 * not aggregated would be handled there too.  Defer the decision
		 * to the path split. */
	}

	/* FROM: one base table, or exactly two base tables joined (V5). */
	src = sel->from;
	if (src == NULL)   /* no FROM: one row of no columns */
		return vx_try_prepare_mat(db, ast, sel, NULL, NULL, NULL, 0,
		                          binds, nbinds, out, errmsg);
	if (src->next != NULL) {
		const sql_src_t *js;
		/* NATURAL needs the common-column set from both schemas, and a
		 * USING the grammar could not turn into an ON (no name to
		 * qualify a side by) would otherwise run as a keyless cross
		 * join -- a wrong result.  Refuse both; USING into a 3+-way
		 * chain too, where its column may come from any earlier table. */
		for (js = src->next; js != NULL; js = js->next) {
			if (js->join == SX_J_NATURAL) goto fallback;
			if (js->using_cols != NULL &&
			    (js->on == NULL || src->next->next != NULL))
				goto fallback;
		}
		/* Multi-table equi-join (no GROUP BY / ORDER BY / aggregates on
		 * the join path yet -- those compose later).  A post-join ORDER BY
		 * has the same tie-order non-reproducibility as the agg path AND
		 * two streaming join executors (2-table + N-way, native +
		 * non-native sources) to thread it through; deferred as a distinct
		 * step.  Falls back correctly via the VDBE today. */
		/* DISTINCT dedups the collected rows (collect_plan), which is
		 * after any LIMIT: only without one. */
		if (sel->distinct && !sel_is_agg(sel) && (sel->limit || sel->offset))
			goto fallback;
		/* ORDER BY / LIMIT / OFFSET on the join path are applied as a
		 * post-collection sort over the output columns (collect_serial),
		 * exactly like the aggregation path -- deterministic for unique
		 * sort keys. */
		if (src->next->next == NULL) {
			/* Two tables: the hash-join path needs an equi-join ON; a
			 * keyless CROSS / comma join has none, so route those to the
			 * N-way pipeline (which does a Cartesian product). */
			if (src->next->on != NULL)
				return vx_try_prepare_join(db, ast, sel, binds, nbinds, out, errmsg);
			return vx_try_prepare_njoin(db, ast, sel, binds, nbinds, out, errmsg);
		}
		/* 3+ tables: INNER/comma/cross N-way pipeline.  ORDER BY / LIMIT /
		 * OFFSET are applied as a post-collection sort, like the 2-table
		 * join (join_ordered_materialize threads st->njoin). */
		return vx_try_prepare_njoin(db, ast, sel, binds, nbinds, out, errmsg);
	}
	if (src->subquery != NULL)   /* FROM ( select ) [AS x] */
		return vx_try_prepare_mat(db, ast, sel, src->subquery, NULL, NULL, 0,
		                          binds, nbinds, out, errmsg);
	if (src->table.len == 0 || src->table.len >= sizeof tabbuf) goto fallback;
	memcpy(tabbuf, src->table.p, src->table.len);
	tabbuf[src->table.len] = '\0';

	/* A view reference in FROM: expand it as a derived table over the
	 * view's stored SELECT (vx_try_prepare_view runs the view SELECT via
	 * vexec and applies the outer projection / filter / ORDER BY). */
	{
		char vsql[1024];
		if (xstore_view_sql(xstore_bt_of(db), tabbuf, vsql, (int)sizeof vsql))
			return vx_try_prepare_mat(db, ast, sel, NULL, vsql, NULL, 0,
			                          binds, nbinds, out, errmsg);
	}

	/* Aggregation (P3): a GROUP BY, or any select item that is an
	 * aggregate call.  Routed to the dedicated builder. */
	{
		int has_agg = (sel->group != NULL);
		for (it = sel->cols ? sel->cols->head : NULL; it && !has_agg; it = it->next) {
			const sql_expr_t *e = it->expr;
			if (e && expr_has_agg(e))
				has_agg = 1;
		}
		if (has_agg)   /* the agg builder takes over (and frees) the AST */
			return vx_try_prepare_agg(db, ast, sel, tabbuf, binds, nbinds, out, errmsg);
	}
	if (sel->group) goto fallback;   /* defensive: GROUP only on agg path */
	if (sel->having) goto fallback;  /* HAVING only with aggregation */

	/* Projection: a list of scalar expressions, any of which may be a
	 * bare "*" (expand to all table columns, in declared order).  A star
	 * mixed with expressions (SELECT *, col) or repeated (SELECT *, *) is
	 * supported: each star expands to the full schema column set.  A
	 * qualified "t.*" is not handled here (falls back). */
	xstore_col_t proj_scols[64];
	int proj_nsc = 0, proj_has_star = 0;
	{
		const sql_exprlist_item_t *p;
		for (p = sel->cols ? sel->cols->head : NULL; p; p = p->next) {
			if (p->expr == NULL) goto fallback;
			if (p->expr->op == SX_E_STAR) {
				if (p->expr->nname > 0) goto fallback;  /* t.* not handled */
				proj_has_star = 1;
			}
		}
	}
	if (proj_has_star) {
		/* Fetch the schema once (needs the native xstore catalog). */
		bt_t *sbt = xstore_bt_of(db);
		int sc;
		proj_nsc = sbt ? xstore_table_schema(sbt, tabbuf, proj_scols, 64) : 0;
		if (proj_nsc <= 0) goto fallback;
		/* Count output columns: each star -> visible schema columns,
		 * each expression -> one. */
		{
			int nvis = 0;
			for (sc = 0; sc < proj_nsc; sc++)
				if (!proj_scols[sc].hidden) nvis++;
			for (it = sel->cols->head; it; it = it->next)
				nproj += (it->expr->op == SX_E_STAR) ? nvis : 1;
		}
		/* A single bare "*" keeps the fast identity path (proj_star). */
		proj_star = (sel->cols->n == 1);
	} else {
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
	/* Outer correlation name: the FROM alias if present (a correlated
	 * subquery must reference the outer row by the alias, e.g.
	 * FROM t x ... WHERE ... = (SELECT ... WHERE inner.k <= x.k)), else
	 * the bare table name. */
	if (src->alias.len > 0 && src->alias.len < sizeof st->corr_alias) {
		memcpy(st->corr_alias, src->alias.p, src->alias.len);
		st->corr_alias[src->alias.len] = '\0';
	} else {
		snprintf(st->corr_alias, sizeof st->corr_alias, "%s", tabbuf);
	}

	memset(&nv, 0, sizeof nv);
	comp.st = st; comp.nv = &nv; comp.jc = NULL; comp.fail = 0;
	comp.binds = binds; comp.nbinds = nbinds; comp.ag = NULL;

	/* Pass 1: collect referenced base columns and reject unsupported
	 * constructs, WITHOUT the affinity gate (affinities are unknown
	 * until the source is prepared).  For "*" the source is the whole
	 * table, so only the WHERE columns need collecting here -- but the
	 * WHERE column names must resolve against the source columns, which
	 * we do by name after preparing "SELECT *". */
	if (!proj_has_star) {
		nv.otab = st->corr_alias;   /* collect correlated-subquery outer refs */
		nv.bt = xstore_bt_of(db);   /* inner-schema access for EXISTS refs */
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
				if (order_key_alias_pos(sel, o->expr)) continue;
				if (collect_columns(&nv, o->expr) != 0) goto fallback;
			}
		}
		nv.otab = NULL;   /* pass 1 done: stop collecting outer refs */
		nv.bt = NULL;
	} else {
		/* A star (bare "*", possibly mixed with expressions) expands to
		 * every visible table column, in declared order, from the native
		 * catalog.  Add them all first (so their srcrow slots come before
		 * any extra columns referenced only by an accompanying
		 * expression), then collect the expression / WHERE columns.  A
		 * non-xstore SELECT * falls back. */
		int sc;
		for (sc = 0; sc < proj_nsc; sc++) {
			sql_str_t s;
			if (proj_scols[sc].hidden) continue;  /* implicit rowid: not in * */
			s.p = proj_scols[sc].name; s.len = (uint32_t)strlen(proj_scols[sc].name);
			if (nv_add(&nv, &s) < 0) goto fallback;
		}
		if (!proj_star) {
			/* Mixed star + expressions: collect the non-star items too. */
			for (it = sel->cols->head; it; it = it->next)
				if (it->expr->op != SX_E_STAR &&
				    collect_columns(&nv, it->expr) != 0) goto fallback;
		}
		if (sel->where && collect_columns(&nv, sel->where) != 0) goto fallback;
		if (sel->order) {
			const sql_exprlist_item_t *o;
			for (o = sel->order->head; o; o = o->next) {
				if (o->expr && o->expr->op == SX_E_NUMBER) continue;
				if (order_key_alias_pos(sel, o->expr)) continue;
				if (collect_columns(&nv, o->expr) != 0) goto fallback;
			}
		}
	}

	/* Storage-native source.  SELECT * is expanded above into the column
	 * list; explicit-column queries scan the B-tree directly. */
	if (nv.n == 0) {
		/* SELECT 1 FROM t: no column is read, but every row counts.
		 * (proj_scols is free here: a star would have added columns.) */
		bt_t *sbt = xstore_bt_of(db);
		sql_str_t nm;
		if (sbt == NULL || xstore_table_schema(sbt, tabbuf, proj_scols, 64) <= 0)
			goto fallback;
		nm.p = proj_scols[0].name; nm.len = (uint32_t)strlen(proj_scols[0].name);
		if (nv_add(&nv, &nm) < 0) goto fallback;
	}
	if (nv.n > 32) goto fallback;

	st->bt = xstore_bt_of(db);
	if (st->bt == NULL) goto fallback;   /* not an xstore-backed connection */
	snprintf(st->table, sizeof st->table, "%s", tabbuf);
	st->nsrc_col = nv.n;
	st->snap = 0;   /* latest committed (vexec is the read fast path) */

	/* Resolve each source column to its record origin + affinity from
	 * the schema (prepare-time only; no data row stepped). */
	if (resolve_schema(db, tabbuf, &nv, st->src_pay) != 0)
		goto fallback;

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
		} else if (proj_has_star) {
			/* Mixed star + expressions: expand each "*" to a VXO_COL per
			 * visible schema column (resolved by name to its srcrow slot),
			 * and compile each non-star item normally. */
			for (it = sel->cols->head; it; it = it->next) {
				if (it->expr->op == SX_E_STAR) {
					int sc;
					for (sc = 0; sc < proj_nsc; sc++) {
						vx_expr_t *n; sql_str_t s; int idx;
						if (proj_scols[sc].hidden) continue;
						s.p = proj_scols[sc].name;
						s.len = (uint32_t)strlen(proj_scols[sc].name);
						idx = nv_add(&nv, &s);
						if (idx < 0 || k >= nproj) goto fallback;
						n = expr_node(&comp, VXO_COL);
						if (n == NULL) goto oom;
						n->col = idx;
						st->proj[k] = n;
						snprintf(st->outname[k], sizeof st->outname[0],
						         "%.*s", (int)(sizeof st->outname[0] - 1),
						         proj_scols[sc].name);
						k++;
					}
				} else {
					if (k >= nproj) goto fallback;
					st->proj[k] = compile_expr(&comp, it->expr);
					if (comp.fail) goto fallback;
					item_name(it, st->outname[k], sizeof st->outname[0]);
					k++;
				}
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
		/* Record each output column's affinity while the compiler is
		 * live (for recursive execution as a nested subquery). */
		for (k = 0; k < nproj && k < 64; k++) {
			st->outaff[k] = st->proj[k] ? node_class(&comp, st->proj[k]) : VX_AFF_BLOB;
			st->outnoaff[k] = st->proj[k] ?
			    (unsigned char)expr_noaff(&comp, st->proj[k]) : VX_NOAFF_UNKNOWN;
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
			int apos;
			st->order_desc[oi] = ord_flags(o);
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
			} else if ((apos = order_key_alias_pos(sel, e)) != 0) {
				/* ORDER BY <output alias>: an output-column position. */
				st->order_outcol[oi] = apos;
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

	/* Access path: rowid range, or an index prefix (+ range), chosen on
	 * the compiled WHERE; an index walk can also provide the ORDER BY. */
	{
		vx_expr_t *okeys[16];
		ap_order_keys(st, okeys);
		ap_choose(db, tabbuf, nv.names, nv.n, st->src_pay, st->filter,
		    okeys, st->norder, st->order_desc, -1, &st->ap);
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
	return rc;
}

/* ---- aggregation plan builder ------------------------------------ */

/* Map an aggregate function name + arg shape to a kind, or 0. */
static enum vx_agg_kind
agg_kind_of(const sql_expr_t *e)
{
	int star = (e->ival & 2) != 0;
	int distinct = (e->ival & 1) != 0;
	int nargs = 0;
	const sql_exprlist_item_t *it;
	for (it = e->list ? e->list->head : NULL; it; it = it->next) nargs++;
	if (name_is(&e->name[0], "count")) {
		if (star) return distinct ? 0 : VXA_COUNT_STAR;  /* count(DISTINCT *) invalid */
		if (nargs != 1) return 0;
		return distinct ? VXA_COUNT_DISTINCT : VXA_COUNT;
	}
	if (distinct) return 0;                    /* DISTINCT only for count */
	if (star) return 0;                        /* sum(*) etc. invalid */
	/* group_concat(expr [, sep]): 1 or 2 args. */
	if (name_is(&e->name[0], "group_concat"))
		return (nargs == 1 || nargs == 2) ? VXA_GROUP_CONCAT : 0;
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
			    strncasecmp(a->name[k].p, b->name[k].p, a->name[k].len) != 0)
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
		    strncasecmp(a->name[0].p, b->name[0].p, a->name[0].len) != 0) return 0;
		ia = a->list ? a->list->head : NULL;
		ib = b->list ? b->list->head : NULL;
		for (; ia && ib; ia = ia->next, ib = ib->next)
			if (!expr_same(ia->expr, ib->expr)) return 0;
		return ia == NULL && ib == NULL;
	}
	default: return 0;
	}
}

/* ---- aggregation: group-mode compile ------------------------------ */

static int lit_int(const sql_expr_t *e, int64_t *out, const vx_cell_t *binds,
                   int nbinds);

/* The output item named `nm` (an AS alias), or NULL. */
static const sql_exprlist_item_t *
agg_alias(const sql_select_t *sel, const sql_str_t *nm)
{
	const sql_exprlist_item_t *it;
	for (it = sel->cols ? sel->cols->head : NULL; it; it = it->next)
		if (it->alias.len == nm->len && nm->len > 0 &&
		    strncasecmp(it->alias.p, nm->p, nm->len) == 0)
			return it;
	return NULL;
}

/* Does `e` name a column of the source?  (So a HAVING / ORDER BY name
 * that is both a column and an output alias means the column, as in
 * SQLite.)  Base table: the live namevec schema; join: the join sides;
 * materialized: the closed column set. */
static int
agg_is_src_col(struct vx_compiler *c, const sql_expr_t *e)
{
	const sql_str_t *nm = &e->name[e->nname - 1];
	int i;
	if (c->jc != NULL) return jc_resolve(c->jc, e) >= 0;
	if (nv_find(c->nv, nm) >= 0) return 1;
	if (c->nv->sc != NULL) {
		for (i = 0; i < c->nv->nsc; i++)
			if (strlen(c->nv->sc[i].name) == nm->len &&
			    strncasecmp(c->nv->sc[i].name, nm->p, nm->len) == 0)
				return 1;
		return (nm->len == 5 && strncasecmp(nm->p, "rowid", 5) == 0) ||
		    (nm->len == 7 && strncasecmp(nm->p, "_rowid_", 7) == 0) ||
		    (nm->len == 3 && strncasecmp(nm->p, "oid", 3) == 0);
	}
	return 0;
}

static vx_expr_t *compile_expr(struct vx_compiler *c, const sql_expr_t *e);

static vx_expr_t *
agg_slot(struct vx_compiler *c, int slot)
{
	vx_expr_t *n = expr_node(c, VXO_COL);
	if (n) { n->col = slot; n->noaff = c->ag->noaff[slot]; }
	return n;
}

/*
 * Group mode: resolve `e` against the group row before compile_expr
 * looks at it.  Returns a node, or NULL with *handled = 0 (compile `e`
 * normally: its children come back here) or *handled = 1 (an error was
 * set in c->fail).
 */
static vx_expr_t *
agg_resolve(struct vx_compiler *c, const sql_expr_t *e, int *handled)
{
	struct vx_aggctx *ag = c->ag;
	vx_aggplan_t *ap = ag->ap;
	const sql_exprlist_item_t *al;
	int k;

	*handled = 1;
	/* A GROUP BY term, or a column reference to the column a key is: its
	 * key slot. */
	for (k = 0; k < ap->ngrp; k++)
		if (expr_same(e, ag->gast[k]))
			return agg_slot(c, k);
	if (e->op == SX_E_COLUMN && e->nname >= 1 && e->nname <= 2) {
		int col = c->jc != NULL ? jc_resolve(c->jc, e)
		    : nv_find(c->nv, &e->name[e->nname - 1]);
		for (k = 0; col >= 0 && k < ap->ngrp; k++)
			if (ap->grp[k]->op == VXO_COL && ap->grp[k]->col == col &&
			    !ap->grp[k]->noaff)
				return agg_slot(c, k);
	}
	if (is_agg_call(e)) {
		enum vx_agg_kind kind = agg_kind_of(e);
		enum vx_aff argcls = VX_AFF_BLOB;
		vx_aggacc_t *a;
		for (k = 0; k < ap->nagg; k++)
			if (expr_same(e, ap->acc[k].ast))
				return agg_slot(c, ap->ngrp + k);
		if (kind == 0 || ap->ngrp + ap->nagg >= VX_GROW_MAX) {
			c->fail = 1; return NULL;
		}
		a = &ap->acc[ap->nagg];
		memset(a, 0, sizeof *a);
		a->kind = kind; a->ast = e;
		if (kind != VXA_COUNT_STAR) {
			const sql_expr_t *arg = e->list->head->expr;
			if (expr_has_agg(arg)) { c->fail = 1; return NULL; }   /* nested */
			ag->grouping = 0;
			a->arg = compile_expr(c, arg);
			argcls = a->arg ? node_class(c, a->arg) : VX_AFF_BLOB;
			ag->grouping = 1;
			if (a->arg == NULL || c->fail) { c->fail = 1; return NULL; }
		}
		if (kind == VXA_GROUP_CONCAT) {
			ap->has_gc = 1;
			if (e->list->head->next != NULL) {
				const sql_expr_t *sp = e->list->head->next->expr;
				char *b;
				if (sp == NULL || sp->op != SX_E_STRING) { c->fail = 1; return NULL; }
				b = (char *)arena_alloc(&c->st->plan_arena, sp->lit.len + 1);
				if (b == NULL) { c->fail = 1; return NULL; }
				memcpy(b, sp->lit.p, sp->lit.len); b[sp->lit.len] = '\0';
				a->gc_sep = b; a->gc_seplen = sp->lit.len;
			}
		}
		/* SQLite: count/sum/total/avg are numbers; min/max the argument's
		 * value; group_concat text.  None has an affinity. */
		k = ap->ngrp + ap->nagg;
		ag->noaff[k] = VX_NOAFF_NO;
		ag->aff[k] = (kind == VXA_GROUP_CONCAT) ? VX_AFF_TEXT
		    : (kind == VXA_MIN || kind == VXA_MAX) ? argcls : VX_AFF_NUMERIC;
		ap->nagg++;
		return agg_slot(c, k);
	}
	if (e->op == SX_E_COLUMN) {
		/* An output alias stands for its expression: in ORDER BY first
		 * (SQLite resolves ORDER BY names against the result columns),
		 * in HAVING only when no source column has the name. */
		if (e->nname == 1 && ag->depth < 4 &&
		    (ag->ctx == AGX_ORDER ||
		     (ag->ctx == AGX_HAVING && !agg_is_src_col(c, e))) &&
		    (al = agg_alias(ag->sel, &e->name[0])) != NULL) {
			vx_expr_t *n;
			int sctx = ag->ctx;
			ag->depth++;
			ag->ctx = AGX_SEL;
			n = compile_expr(c, al->expr);
			ag->ctx = sctx;
			ag->depth--;
			if (n == NULL) c->fail = 1;
			return n;
		}
		/* A bare column not in GROUP BY.  SQLite takes it from an
		 * arbitrary row of the group; vexec declines (its value is not
		 * reproducible). */
		c->fail = 1; return NULL;
	}
	/* A subquery over the group row: only an uncorrelated one, run once
	 * into a constant (a correlated one would bind source-row columns). */
	if (e->op == SX_E_SUBQUERY) {
		vx_expr_t *n = compile_scalar_subquery(c, e);
		if (n == NULL || c->fail) { c->fail = 1; return NULL; }
		return n;
	}
	if (e->op == SX_E_IN_SELECT) {
		vx_expr_t *n;
		if (e->a == NULL) { c->fail = 1; return NULL; }   /* EXISTS */
		n = compile_in_select(c, e);
		if (n == NULL || c->fail) { c->fail = 1; return NULL; }
		return n;
	}
	*handled = 0;
	return NULL;
}

/* Close the source schema: from here on no new source column may be
 * referenced (a materialized source is a fixed set anyway). */
static void
agg_freeze(struct vx_compiler *c)
{
	if (c->nv) c->nv->fixed = 1;
}

/*
 * Build the aggregate part of `st` for SELECT `sel`, whose source row is
 * already described by the compiler (comp->nv: a base table with live
 * schema resolution, or a materialized source's columns; comp->jc: a
 * join).  WHERE is compiled by the caller.  Fills st->agg, outname,
 * outaff, the ORDER BY / LIMIT, and the min/max shortcut hint.  Returns
 * 1, 0 to decline, -1 on OOM.
 */
static int
agg_build(struct vx_compiler *comp, const sql_select_t *sel, int nout)
{
	struct vx_stmt *st = comp->st;
	struct vx_aggctx ag;
	vx_aggplan_t *ap;
	const sql_exprlist_item_t *it;
	int k, ngrp = 0;

	memset(&ag, 0, sizeof ag);
	ag.sel = sel;
	for (it = sel->group ? sel->group->head : NULL; it; it = it->next) {
		const sql_expr_t *g = it->expr;
		if (ngrp >= 16 || g == NULL) return 0;
		/* GROUP BY <n>: the n-th output column; GROUP BY <alias>: that
		 * output's expression (unless a source column has the name). */
		int64_t v;
		if (g->op == SX_E_NUMBER && lit_int(g, &v, NULL, 0)) {
			const sql_exprlist_item_t *ci;
			if (v < 1 || v > nout) return 0;          /* SQLite: out of range */
			for (ci = sel->cols->head; v > 1; v--) ci = ci->next;
			if (expr_has_agg(ci->expr)) return 0;    /* SQLite: an error */
			g = ci->expr;
		} else if (g->op == SX_E_UNARY && g->a != NULL && g->a->op == SX_E_NUMBER) {
			return 0;   /* GROUP BY -1 / +1: SQLite reads a position */
		} else if (g->op == SX_E_COLUMN && g->nname == 1 &&
		    !agg_is_src_col(comp, g)) {
			const sql_exprlist_item_t *al = agg_alias(sel, &g->name[0]);
			if (al == NULL || expr_has_agg(al->expr)) return 0;
			g = al->expr;
		}
		if (expr_has_agg(g)) return 0;
		ag.gast[ngrp++] = g;
	}
	if (nout > 32) return 0;
	ap = (vx_aggplan_t *)calloc(1, sizeof *ap);
	if (ap == NULL) return -1;
	st->agg = ap;
	ap->ngrp = ngrp;
	ap->nout = nout;
	ap->grp = (vx_expr_t **)calloc((size_t)(ngrp > 0 ? ngrp : 1), sizeof(vx_expr_t *));
	ap->acc = (vx_aggacc_t *)calloc(VX_GROW_MAX, sizeof(vx_aggacc_t));
	ap->proj = (vx_expr_t **)calloc(32, sizeof(vx_expr_t *));
	if (ap->grp == NULL || ap->acc == NULL || ap->proj == NULL) return -1;
	ag.ap = ap;

	/* The keys, over the source row. */
	comp->fail = 0;
	for (k = 0; k < ngrp; k++) {
		ap->grp[k] = compile_expr(comp, ag.gast[k]);
		if (comp->fail || ap->grp[k] == NULL) return 0;
		ag.aff[k] = node_class(comp, ap->grp[k]);
		ag.noaff[k] = (unsigned char)expr_noaff(comp, ap->grp[k]);
	}

	/* Outputs, then HAVING, over the group row. */
	comp->ag = &ag;
	ag.grouping = 1;
	ag.ctx = AGX_SEL;
	k = 0;
	for (it = sel->cols->head; it; it = it->next, k++) {
		if (it->expr == NULL || it->expr->op == SX_E_STAR) goto decline;
		ap->proj[k] = compile_expr(comp, it->expr);
		if (comp->fail || ap->proj[k] == NULL) goto decline;
		item_name(it, st->outname[k], sizeof st->outname[0]);
		st->outaff[k] = node_class(comp, ap->proj[k]);
		st->outnoaff[k] = (unsigned char)expr_noaff(comp, ap->proj[k]);
	}
	if (sel->having) {
		ag.ctx = AGX_HAVING;
		ap->having = compile_expr(comp, sel->having);
		if (comp->fail || ap->having == NULL) goto decline;
	}

	/* ORDER BY: an output position, an output alias, an expression equal
	 * to an output, or any other group-row expression (a sort-only
	 * column appended after the outputs). */
	st->limit = -1;
	st->offset = 0;
	ap->nemit = nout;
	ap->distinct = sel->distinct;
	if (sel->order) {
		const sql_exprlist_item_t *o;
		int no = 0, oi = 0;
		for (o = sel->order->head; o; o = o->next) no++;
		if (no > 16) goto decline;
		st->norder = no;
		st->order_key = (vx_expr_t **)calloc((size_t)no, sizeof(vx_expr_t *));
		st->order_outcol = (int *)calloc((size_t)no, sizeof(int));
		st->order_desc = (int *)calloc((size_t)no, sizeof(int));
		if (!st->order_key || !st->order_outcol || !st->order_desc) {
			comp->ag = NULL; return -1;
		}
		ag.ctx = AGX_ORDER;
		for (o = sel->order->head; o; o = o->next, oi++) {
			const sql_expr_t *e = o->expr;
			const sql_exprlist_item_t *ci;
			int pos = 0;
			st->order_desc[oi] = ord_flags(o);
			if (e && e->op == SX_E_NUMBER) {
				int64_t v;
				if (!lit_int(e, &v, NULL, 0) || v < 1 || v > nout) goto decline;
				pos = (int)v;
			} else if ((pos = order_key_alias_pos(sel, e)) != 0) {
				;   /* ORDER BY <output alias> */
			} else {
				k = 1;
				for (ci = sel->cols->head; ci; ci = ci->next, k++)
					if (expr_same(e, ci->expr)) { pos = k; break; }
			}
			if (pos == 0) {
				if (ap->nemit >= 32) goto decline;
				ap->proj[ap->nemit] = compile_expr(comp, e);
				if (comp->fail || ap->proj[ap->nemit] == NULL) goto decline;
				pos = ++ap->nemit;
			}
			st->order_outcol[oi] = pos;
		}
	}
	comp->ag = NULL;
	agg_freeze(comp);
	/* ORDER BY + LIMIT / OFFSET picks the groups SQLite picks only under a
	 * total order: the ORDER BY must name every GROUP BY key (groups are
	 * unique on their keys).  Which tied groups survive is unspecified in
	 * SQL and SQLite's sorter breaks ties differently by direction. */
	if (st->norder > 0 && (sel->limit || sel->offset) && ap->ngrp > 0) {
		for (k = 0; k < ap->ngrp; k++) {
			int oi, cov = 0;
			for (oi = 0; oi < st->norder && !cov; oi++) {
				const vx_expr_t *ok = ap->proj[st->order_outcol[oi] - 1];
				cov = (ok->op == VXO_COL && ok->col == k);
			}
			if (!cov) return 0;
		}
	}
	if (sel->limit && !lit_int(sel->limit, &st->limit, comp->binds, comp->nbinds))
		return 0;
	if (sel->limit && st->limit < 0) st->limit = -1;   /* SQLite: no limit */
	if (sel->offset) {
		if (!lit_int(sel->offset, &st->offset, comp->binds, comp->nbinds)) return 0;
		if (st->offset < 0) st->offset = 0;
	}
	if ((sel->limit && sel->limit->op == SX_E_PARAM) ||
	    (sel->offset && sel->offset->op == SX_E_PARAM))
		st->noreuse = 1;   /* the bound value is baked into the plan */
	st->nout = nout;
	return 1;

decline:
	comp->ag = NULL;
	return 0;
}

/* Every output of an aggregate is evaluated over the group row, so the
 * plan's projection slots (st->proj) are not used; the morsel-parallel
 * path reads ap->proj.  MIN/MAX shortcut: no GROUP BY, no HAVING, every
 * aggregate MIN(c) (or every one MAX(c)) of one bare column c. */
static int
agg_minmax_col(const vx_aggplan_t *ap, int *kind)
{
	int k, col = -1;
	*kind = 0;
	if (ap->ngrp != 0 || ap->having != NULL || ap->nagg == 0) return -1;
	for (k = 0; k < ap->nagg; k++) {
		const vx_aggacc_t *a = &ap->acc[k];
		if ((a->kind != VXA_MIN && a->kind != VXA_MAX) || a->arg == NULL ||
		    a->arg->op != VXO_COL || (*kind && *kind != (int)a->kind) ||
		    (col >= 0 && col != a->arg->col))
			return -1;
		*kind = (int)a->kind; col = a->arg->col;
	}
	return col;
}

/*
 * SELECT ... FROM <base table> with an aggregate or GROUP BY.  One
 * compile pass with live schema resolution: each column is given its
 * payload slot and affinity the moment anything (WHERE, a key, an
 * aggregate argument, a correlated subquery's outer reference) names it.
 */
static int
vx_try_prepare_agg(struct xsql *db, sql_arena_t *ast, const sql_select_t *sel,
                   const char *tabbuf, const vx_cell_t *binds, int nbinds,
                   vx_stmt_t **out, char **errmsg)
{
	struct vx_stmt *st = NULL;
	struct namevec nv;
	struct vx_compiler comp;
	xstore_col_t *sc = NULL;
	const sql_exprlist_item_t *it;
	int nout = 0, rc = 0, nsc, r;

	if (errmsg) *errmsg = NULL;
	for (it = sel->cols ? sel->cols->head : NULL; it; it = it->next) {
		if (it->expr == NULL || it->expr->op == SX_E_STAR) goto fallback;
		nout++;
	}
	if (nout == 0 || nout > 32) goto fallback;

	st = (struct vx_stmt *)calloc(1, sizeof *st);
	if (!st) goto oom;
	st->db = db;
	st->cur = -1;
	st->limit = -1;
	st->binds = binds; st->nbinds = nbinds;
	st->bt = xstore_bt_of(db);
	if (st->bt == NULL) goto fallback;
	snprintf(st->table, sizeof st->table, "%s", tabbuf);
	if (sel->from->alias.len > 0 && sel->from->alias.len < sizeof st->corr_alias) {
		memcpy(st->corr_alias, sel->from->alias.p, sel->from->alias.len);
		st->corr_alias[sel->from->alias.len] = '\0';
	} else
		snprintf(st->corr_alias, sizeof st->corr_alias, "%s", tabbuf);
	sc = (xstore_col_t *)malloc(sizeof(xstore_col_t) * 64);
	if (sc == NULL) goto oom;
	nsc = xstore_table_schema(st->bt, tabbuf, sc, 64);
	if (nsc <= 0) goto fallback;

	memset(&nv, 0, sizeof nv);
	nv.sc = sc; nv.nsc = nsc; nv.pay = st->src_pay;
	comp.st = st; comp.nv = &nv; comp.jc = NULL; comp.fail = 0;
	comp.binds = binds; comp.nbinds = nbinds; comp.ag = NULL;

	if (sel->where) {
		st->filter = compile_expr(&comp, sel->where);
		if (comp.fail || st->filter == NULL) goto fallback;
	}
	r = agg_build(&comp, sel, nout);
	if (r < 0) goto oom;
	if (r == 0) goto fallback;
	st->nsrc_col = nv.n;
	st->snap = 0;

	/* MIN/MAX shortcut: a walk ordered by c (ascending for MIN,
	 * descending for MAX) stops at the first row that passes the WHERE
	 * with a non-NULL c: it holds the answer (SQLite's min/max
	 * optimization).  Outputs over the aggregates read the same slot. */
	{
		int kind, mmcol = agg_minmax_col(st->agg, &kind);
		ap_choose(db, tabbuf, nv.names, nv.n, st->src_pay, st->filter,
		    NULL, 0, NULL, mmcol, &st->ap);
		if (mmcol >= 0 && st->ap.ordered && st->ap.kind == VX_AP_INDEX) {
			if (kind == VXA_MAX) st->ap.reverse = !st->ap.reverse;
			st->ap_minmax = 1;
		} else if (mmcol >= 0 && st->src_pay[mmcol] == -1 &&
		    (st->ap.kind == VX_AP_SCAN || st->ap.kind == VX_AP_ROWID)) {
			/* MIN/MAX of the rowid: a rowid walk is ascending, so the
			 * first passing row holds MIN; MAX with no WHERE is the
			 * newest live rowid, one descent per rowid downward
			 * (xstore_max_live_rowid) instead of a whole-table scan. */
			if (kind == VXA_MIN) {
				st->ap.ordered = 1;
				st->ap_minmax = 1;
			} else if (st->filter == NULL && st->ap.kind == VX_AP_SCAN) {
				st->ap.rowmax = 1;
				st->ap.ordered = 1;
				st->ap_minmax = 1;
			}
		}
		st->ap.ordered = (mmcol >= 0) ? st->ap.ordered : 0;
	}
	st->srcrow = (vx_cell_t *)calloc((size_t)(st->nsrc_col > 0 ? st->nsrc_col : 1),
	                                 sizeof(vx_cell_t));
	if (!st->srcrow) goto oom;

	free(sc);
	sql_arena_destroy(ast);
	*out = st;
	return 1;

oom:
	rc = -1;
fallback:
	free(sc);
	if (ast) sql_arena_destroy(ast);
	if (st) vx_finalize(st);
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
/* Load each side's catalog schema into jc (heap).  A side whose schema
 * is unknown keeps nsc == 0 (unqualified names then fail to resolve
 * there).  Pair with jc_schema_free on every exit. */
static void
jc_schema_load(struct xsql *db, struct vx_joinctx *jc)
{
	bt_t *bt = xstore_bt_of(db);
	int side, n;
	for (side = 0; side < jc->nside; side++) {
		jc->sc[side] = NULL;
		jc->nsc[side] = 0;
		if (bt == NULL) continue;
		jc->sc[side] = (xstore_col_t *)malloc(sizeof(xstore_col_t) * 64);
		if (jc->sc[side] == NULL) continue;
		n = xstore_table_schema(bt, jc->tab[side], jc->sc[side], 64);
		jc->nsc[side] = n > 0 ? n : 0;
	}
}

static void
jc_schema_free(struct vx_joinctx *jc)
{
	int side;
	for (side = 0; side < VX_JOIN_MAX; side++) {
		free(jc->sc[side]);
		jc->sc[side] = NULL;
		jc->nsc[side] = 0;
	}
}

/* Does join side `side` have a column named `col` (or a rowid alias)? */
static int
jc_side_has(const struct vx_joinctx *jc, int side, const sql_str_t *col)
{
	int c;
	for (c = 0; c < jc->nsc[side]; c++)
		if (!jc->sc[side][c].hidden && str_eq_cstr(col, jc->sc[side][c].name))
			return 1;
	return 0;
}

static const sql_exprlist_item_t *agg_alias(const sql_select_t *sel,
                                            const sql_str_t *nm);

static int
jc_collect(struct vx_joinctx *jc, const sql_expr_t *e)
{
	const sql_exprlist_item_t *it;
	const sql_case_arm_t *arm;
	if (e == NULL) return -1;
	if (jc->aggsel != NULL && is_agg_call(e)) {
		if (e->ival & 2) return 0;   /* count(*) */
		if (e->list == NULL || e->list->head == NULL) return -1;
		return jc_collect(jc, e->list->head->expr);   /* the argument */
	}
	switch (e->op) {
	case SX_E_NULL: case SX_E_NUMBER: case SX_E_STRING: case SX_E_PARAM:
		return 0;
	case SX_E_COLUMN: {
		const sql_str_t *qual = (e->nname == 2) ? &e->name[0] : NULL;
		const sql_str_t *col = &e->name[e->nname - 1];
		int side, hit = -1;
		if (e->nname < 1 || e->nname > 2) return -1;
		for (side = 0; side < jc->nside; side++) {
			if (qual) {
				const char *q = jc->alias[side][0] ? jc->alias[side] : jc->tab[side];
				if (!str_eq_cstr(qual, q)) continue;
			} else if (!jc_side_has(jc, side, col)) {
				continue;   /* unqualified: only the side that has it */
			}
			if (!qual && hit >= 0)
				return -1;  /* unqualified on two sides: ambiguous */
			if (nv_add(&jc->col[side], col) < 0) return -1;
			hit = side;
		}
		if (hit < 0 && !qual && jc->aggsel != NULL &&
		    agg_alias(jc->aggsel, col) != NULL)
			return 0;   /* an output alias (its expression is collected) */
		return hit >= 0 ? 0 : -1;
	}
	case SX_E_UNARY:
		if (e->op2 != TK_MINUS && e->op2 != TK_PLUS && e->op2 != TK_NOT) return -1;
		return jc_collect(jc, e->a);
	case SX_E_BINARY: {
		enum vx_op d;
		if (e->op2 != TK_LIKE && !tok_to_binop(e->op2, &d)) return -1;
		if (jc_collect(jc, e->a) != 0) return -1;
		return jc_collect(jc, e->b);
	}
	case SX_E_BETWEEN:
		if (jc_collect(jc, e->a) != 0 || jc_collect(jc, e->b) != 0) return -1;
		return jc_collect(jc, e->c);
	case SX_E_IN_LIST:
		if (e->a == NULL || e->sel != NULL || jc_collect(jc, e->a) != 0)
			return -1;
		for (it = e->list ? e->list->head : NULL; it; it = it->next)
			if (jc_collect(jc, it->expr) != 0) return -1;
		return 0;
	case SX_E_CASE:
		if (e->a != NULL && jc_collect(jc, e->a) != 0) return -1;
		for (arm = e->arms; arm; arm = arm->next)
			if (jc_collect(jc, arm->when) != 0 ||
			    jc_collect(jc, arm->then) != 0) return -1;
		if (e->els != NULL && jc_collect(jc, e->els) != 0) return -1;
		return 0;
	case SX_E_IS_NULL: return jc_collect(jc, e->a);
	case SX_E_FUNC: {
		int na = 0, ok = 0;
		if (e->ival & 3) return -1;
		for (it = e->list ? e->list->head : NULL; it; it = it->next) na++;
		if (e->nname == 2 && name_is(&e->name[0], "cast") && na == 1)
			ok = 1;
		else
			(void)func_of(&e->name[0], &ok, na);
		if (!ok) return -1;
		for (it = e->list ? e->list->head : NULL; it; it = it->next)
			if (jc_collect(jc, it->expr) != 0) return -1;
		return 0;
	}
	default: return -1;
	}
}

/* A join side the query names no column of (SELECT count(*) FROM a, b)
 * still produces rows: read its rowid (schema column 0) so it has one.
 * 0, or -1 if the side's schema is unknown. */
static int
jc_fill_empty(struct vx_joinctx *jc)
{
	int side;
	sql_str_t nm;
	for (side = 0; side < jc->nside; side++) {
		if (jc->col[side].n > 0) continue;
		if (jc->nsc[side] <= 0) return -1;
		nm.p = jc->sc[side][0].name; nm.len = (uint32_t)strlen(nm.p);
		if (nv_add(&jc->col[side], &nm) < 0) return -1;
	}
	return 0;
}

/* Does `sel` aggregate (a GROUP BY, HAVING, or an aggregate output)? */
static int
sel_is_agg(const sql_select_t *sel)
{
	const sql_exprlist_item_t *it;
	if (sel->group != NULL || sel->having != NULL) return 1;
	for (it = sel->cols ? sel->cols->head : NULL; it; it = it->next)
		if (it->expr != NULL && expr_has_agg(it->expr)) return 1;
	return 0;
}

/* Aggregate over a join, pass 1: collect every column the outputs (and
 * aggregate arguments), GROUP BY, HAVING and ORDER BY name.  A positional
 * term names an output, which is collected already. */
static int
jc_collect_agg(struct vx_joinctx *jc, const sql_select_t *sel)
{
	const sql_exprlist_item_t *it;
	jc->aggsel = sel;
	for (it = sel->cols->head; it; it = it->next)
		if (it->expr == NULL || it->expr->op == SX_E_STAR ||
		    jc_collect(jc, it->expr) != 0) return -1;
	for (it = sel->group ? sel->group->head : NULL; it; it = it->next)
		if (it->expr->op != SX_E_NUMBER && jc_collect(jc, it->expr) != 0)
			return -1;
	if (sel->having && jc_collect(jc, sel->having) != 0) return -1;
	for (it = sel->order ? sel->order->head : NULL; it; it = it->next)
		if (it->expr->op != SX_E_NUMBER && jc_collect(jc, it->expr) != 0)
			return -1;
	return 0;
}

/*
 * Aggregate over a join, the plan: `js` is the join, built to emit the
 * whole combined row (identity projection, WHERE applied in the join);
 * the returned statement aggregates those rows, its expressions compiled
 * against the join context.  Takes ownership of js.  1, 0 decline, -1.
 */
static int
join_agg_wrap(struct vx_stmt *js, struct vx_joinctx *jc, const sql_select_t *sel,
              const vx_cell_t *binds, int nbinds, vx_stmt_t **out)
{
	struct vx_stmt *st;
	struct vx_compiler comp;
	const sql_exprlist_item_t *it;
	int nout = 0, r;

	for (it = sel->cols->head; it; it = it->next) nout++;
	st = (struct vx_stmt *)calloc(1, sizeof *st);
	if (st == NULL) { vx_finalize(js); return -1; }
	st->db = js->db; st->cur = -1; st->limit = -1;
	st->binds = binds; st->nbinds = nbinds;
	st->agg_in = js;
	comp.st = st; comp.nv = NULL; comp.jc = jc; comp.fail = 0;
	comp.binds = binds; comp.nbinds = nbinds; comp.ag = NULL;
	jc->aggsel = NULL;
	r = agg_build(&comp, sel, nout);
	/* group_concat concatenates in row order, and a join's row order is
	 * its executor's (hash build / probe), not SQLite's loop nest. */
	if (r == 1 && st->agg->has_gc) r = 0;
	if (r != 1) { vx_finalize(st); return r; }
	/* The plan cache must not keep a plan whose join was rebound under it. */
	st->noreuse |= js->noreuse;
	*out = st;
	return 1;
}

/* Give the join statement the identity projection of its combined row
 * (total columns), for an aggregate over it. */
static int
join_identity_proj(struct vx_stmt *st, struct vx_compiler *comp, int total)
{
	int k;
	if (total > 32) return 0;
	for (k = 0; k < total; k++) {
		vx_expr_t *n = expr_node(comp, VXO_COL);
		if (n == NULL) return -1;
		n->col = k;
		st->proj[k] = n;
	}
	st->nout = total;
	return 1;
}

static int
vx_try_prepare_join(struct xsql *db, sql_arena_t *ast, const sql_select_t *sel,
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
	char bcols[1000], pcols[1000];
	int nproj = 0, i, side, rc = 0, lkey, rkey, agg = sel_is_agg(sel);

	if (errmsg) *errmsg = NULL;
	memset(&jc, 0, sizeof jc);

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

	jc.nside = 2;
	memcpy(jc.tab[0], s0->table.p, s0->table.len); jc.tab[0][s0->table.len] = '\0';
	memcpy(jc.tab[1], s1->table.p, s1->table.len); jc.tab[1][s1->table.len] = '\0';
	if (s0->alias.len && s0->alias.len < 64) { memcpy(jc.alias[0], s0->alias.p, s0->alias.len); jc.alias[0][s0->alias.len] = '\0'; }
	if (s1->alias.len && s1->alias.len < 64) { memcpy(jc.alias[1], s1->alias.p, s1->alias.len); jc.alias[1][s1->alias.len] = '\0'; }
	jc_schema_load(db, &jc);

	/* SELECT * is not supported on the join path (column expansion +
	 * dedup across two tables); fall back. */
	for (it = sel->cols ? sel->cols->head : NULL; it; it = it->next) {
		if (it->expr == NULL || it->expr->op == SX_E_STAR) goto fallback;
		nproj++;
	}
	if (nproj == 0 || nproj > 32) goto fallback;

	/* Pass 1: collect referenced columns into per-side namevecs, from
	 * the projection (or, aggregating, every clause), the WHERE, and the
	 * ON's two columns. */
	if (agg) {
		if (jc_collect_agg(&jc, sel) != 0) goto fallback;
	} else {
		for (it = sel->cols->head; it; it = it->next)
			if (jc_collect(&jc, it->expr) != 0) goto fallback;
	}
	if (sel->where && jc_collect(&jc, sel->where) != 0) goto fallback;
	if (jc_collect(&jc, on->a) != 0) goto fallback;
	if (jc_collect(&jc, on->b) != 0) goto fallback;
	/* ORDER BY columns must be sourced too (a key that is not an output
	 * becomes a sort-only column), unless an ordinal or output alias. */
	if (sel->order && !agg) {
		const sql_exprlist_item_t *o;
		for (o = sel->order->head; o; o = o->next) {
			if (o->expr && o->expr->op == SX_E_NUMBER) continue;
			if (order_key_alias_pos(sel, o->expr)) continue;
			if (jc_collect(&jc, o->expr) != 0) goto fallback;
		}
	}

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
	if (jc_fill_empty(&jc) != 0) goto fallback;

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
	st->distinct = !agg && sel->distinct;
	jp = (vx_joinplan_t *)calloc(1, sizeof *jp);
	if (!jp) goto oom;
	st->join = jp;
	/* A bare comma / CROSS join with an equality ON behaves as INNER. */
	jp->join_kind = (s1->join == SX_J_LEFT || s1->join == SX_J_RIGHT ||
	                 s1->join == SX_J_FULL) ? s1->join : SX_J_INNER;
	st->nout = nproj;
	/* Room for ORDER-BY-only columns past nproj (up to 32 in all). */
	st->proj = (vx_expr_t **)calloc(32, sizeof(vx_expr_t *));
	if (!st->proj) goto oom;

	snprintf(jp->build_sql, sizeof jp->build_sql, "SELECT %s FROM %s", bcols, jc.tab[0]);
	snprintf(jp->probe_sql, sizeof jp->probe_sql, "SELECT %s FROM %s", pcols, jc.tab[1]);

	/* Native row sources when the sides are xstore-backed (no SQLite
	 * cursor); else read through the prepared SELECT.  -1 = unresolved. */
	{
		int bn = jsrc_build(db, jc.tab[0], jc.col[0].names, jc.col[0].n, &jp->build_src);
		int pn = jsrc_build(db, jc.tab[1], jc.col[1].names, jc.col[1].n, &jp->probe_src);
		if (bn < 0 || pn < 0) goto fallback;

		jp->build_ncol = jc.col[0].n;
		jp->probe_ncol = jc.col[1].n;
		jc.base[0] = 0;
		jc.base[1] = jp->build_ncol;

		/* Both join sides are xstore-native (jsrc_build requires it). */
		if (!jp->build_src.native || !jp->probe_src.native) goto fallback;
		for (i = 0; i < jp->build_ncol; i++) jc.col[0].aff[i] = jp->build_src.aff[i];
		for (i = 0; i < jp->probe_ncol; i++) jc.col[1].aff[i] = jp->probe_src.aff[i];
	}

	/* The join keys, as combined-row indices, then split per side. */
	lkey = jc_resolve(&jc, lhs);
	rkey = jc_resolve(&jc, rhs);
	if (lkey < 0 || rkey < 0) goto fallback;
	/* Side 0 (build) key is whichever of lkey/rkey is < base[1]. */
	if (lkey < jc.base[1]) { jp->build_key = lkey; jp->probe_key = rkey - jc.base[1]; }
	else                   { jp->build_key = rkey; jp->probe_key = lkey - jc.base[1]; }
	if (jp->build_key < 0 || jp->build_key >= jp->build_ncol ||
	    jp->probe_key < 0 || jp->probe_key >= jp->probe_ncol) goto fallback;

	/* Pass 2: compile projection + filter over the combined row (an
	 * aggregate's input: the whole combined row). */
	comp.st = st; comp.nv = NULL; comp.jc = &jc; comp.fail = 0;
	comp.binds = binds; comp.nbinds = nbinds; comp.ag = NULL;
	if (agg) {
		int r = join_identity_proj(st, &comp, jp->build_ncol + jp->probe_ncol);
		if (r < 0) goto oom;
		if (r == 0) goto fallback;
		nproj = st->nout;
	} else {
		int k = 0;
		for (it = sel->cols->head; it; it = it->next, k++) {
			st->proj[k] = compile_expr(&comp, it->expr);
			if (comp.fail) goto fallback;
			item_name(it, st->outname[k], sizeof st->outname[0]);
		}
	}
	if (sel->where) {
		st->filter = compile_expr(&comp, sel->where);
		if (comp.fail) goto fallback;
	}
	for (i = 0; i < nproj; i++) {
		st->outaff[i] = node_class(&comp, st->proj[i]);
		st->outnoaff[i] = (unsigned char)expr_noaff(&comp, st->proj[i]);
	}
	/* INNER: an index nested loop when the inner side can seek by the
	 * join key (the predicate is WHERE AND ON, re-checked per row). */
	if (jp->join_kind == SX_J_INNER) {
		const vx_jsrc_t *srcs[2];
		vx_expr_t *onx;
		srcs[0] = &jp->build_src; srcs[1] = &jp->probe_src;
		comp.fail = 0;
		onx = compile_expr(&comp, on);
		if (!comp.fail && onx != NULL)
			(void)nl_plan(db, st, &jc, srcs, ap_and(&comp, st->filter, onx));
		comp.fail = 0;
	}

	/* ORDER BY / LIMIT / OFFSET: order keys reference OUTPUT columns (a
	 * positional integer or a select item matched by expr_same), applied
	 * as a post-collection sort in collect_serial -- deterministic for a
	 * total order. */
	st->limit = -1;
	st->offset = 0;
	if (sel->order && !agg) {
		const sql_exprlist_item_t *o;
		int no = 0, oi = 0, nextra = 0;
		for (o = sel->order->head; o; o = o->next) no++;
		if (no == 0 || no > 16) goto fallback;
		st->norder = no;
		st->order_key = (vx_expr_t **)calloc((size_t)no, sizeof(vx_expr_t *));
		st->order_outcol = (int *)calloc((size_t)no, sizeof(int));
		st->order_desc = (int *)calloc((size_t)no, sizeof(int));
		if (!st->order_key || !st->order_outcol || !st->order_desc) goto oom;
		for (o = sel->order->head; o; o = o->next, oi++) {
			const sql_expr_t *e = o->expr;
			st->order_desc[oi] = ord_flags(o);
			if (e && e->op == SX_E_NUMBER) {
				char buf[32]; long pos;
				if (e->lit.len == 0 || e->lit.len >= sizeof buf) goto fallback;
				memcpy(buf, e->lit.p, e->lit.len); buf[e->lit.len] = '\0';
				if (strchr(buf, '.') || strchr(buf, 'e') || strchr(buf, 'E')) goto fallback;
				pos = strtol(buf, NULL, 10);
				if (pos < 1 || pos > nproj) goto fallback;
				st->order_outcol[oi] = (int)pos;
			} else {
				const sql_exprlist_item_t *ci; int pos = 0, found = 0;
				for (ci = sel->cols->head; ci; ci = ci->next, pos++)
					if (expr_same(e, ci->expr)) { st->order_outcol[oi] = pos + 1; found = 1; break; }
				if (!found) {
					/* The sort key is not an output column: compile it as
					 * an extra projection column appended after the user
					 * columns (the join emits nproj + extras; the result
					 * keeps only the first nproj). */
					int slot = nproj + nextra;
					if (slot >= 32) goto fallback;
					comp.fail = 0;
					st->proj[slot] = compile_expr(&comp, e);
					if (comp.fail) goto fallback;
					st->order_outcol[oi] = slot + 1;   /* 1-based extra col */
					nextra++;
				}
			}
		}
		if (nextra > 0)
			st->join_emit = nproj + nextra;   /* emit user + sort-only cols */
		/* DISTINCT ordered by a column it drops: which row's value orders
		 * a duplicate is unspecified. */
		if (nextra > 0 && st->distinct) goto fallback;
	}
	if (sel->limit && !agg) {
		char buf[32];
		if (sel->limit->op != SX_E_NUMBER || sel->limit->lit.len >= sizeof buf) goto fallback;
		memcpy(buf, sel->limit->lit.p, sel->limit->lit.len); buf[sel->limit->lit.len] = '\0';
		if (strchr(buf, '.') || strchr(buf, 'e') || strchr(buf, 'E')) goto fallback;
		st->limit = strtoll(buf, NULL, 10);
		if (st->limit < 0) st->limit = 0;
	}
	if (sel->offset && !agg) {
		char buf[32];
		if (sel->offset->op != SX_E_NUMBER || sel->offset->lit.len >= sizeof buf) goto fallback;
		memcpy(buf, sel->offset->lit.p, sel->offset->lit.len); buf[sel->offset->lit.len] = '\0';
		if (strchr(buf, '.') || strchr(buf, 'e') || strchr(buf, 'E')) goto fallback;
		st->offset = strtoll(buf, NULL, 10);
		if (st->offset < 0) st->offset = 0;
	}

	/* Combined-row scratch lives in srcrow (build_ncol + probe_ncol). */
	st->nsrc_col = jp->build_ncol + jp->probe_ncol;
	st->srcrow = (vx_cell_t *)calloc((size_t)st->nsrc_col, sizeof(vx_cell_t));
	if (!st->srcrow) goto oom;

	if (agg) {
		int r = join_agg_wrap(st, &jc, sel, binds, nbinds, out);
		st = NULL; jp = NULL;   /* the wrapper owns them (freed on failure) */
		if (r != 1) { rc = r; goto fallback; }
		jc_schema_free(&jc);
		sql_arena_destroy(ast);
		return 1;
	}
	jc_schema_free(&jc);
	sql_arena_destroy(ast);
	*out = st;
	return 1;

oom:
	rc = -1;
fallback:
	jc_schema_free(&jc);
	if (ast) sql_arena_destroy(ast);
	if (st) vx_finalize(st);   /* frees st->join */
	else free(jp);
	return rc;
}

/*
 * N-way INNER equi-join (3 .. VX_JOIN_MAX base tables).  Side 0 streams;
 * sides 1.. are each hashed on their own key and probed with a column
 * of an earlier side that their ON clause equates to.  Outer joins,
 * subquery sources, SELECT *, and aggregates fall back (return 0).
 */
static int
vx_try_prepare_njoin(struct xsql *db, sql_arena_t *ast, const sql_select_t *sel,
                     const vx_cell_t *binds, int nbinds,
                     vx_stmt_t **out, char **errmsg)
{
	struct vx_stmt *st = NULL;
	struct vx_joinctx jc;
	struct vx_compiler comp;
	vx_njoinplan_t *jp = NULL;
	const sql_src_t *src[VX_JOIN_MAX];
	const sql_expr_t *on_lhs[VX_JOIN_MAX], *on_rhs[VX_JOIN_MAX];
	int cross_side[VX_JOIN_MAX];
	const sql_src_t *s;
	const sql_exprlist_item_t *it;
	int nside = 0, nproj = 0, side, rc = 0, total = 0, agg = sel_is_agg(sel);

	if (errmsg) *errmsg = NULL;
	memset(&jc, 0, sizeof jc);

	/* Gather the FROM sources: all base tables, all INNER / comma /
	 * cross; each after the first carries an equi-join ON (col = col). */
	for (s = sel->from; s != NULL; s = s->next) {
		if (nside >= VX_JOIN_MAX) goto fallback;
		if (s->subquery) goto fallback;
		if (s->table.len == 0 || s->table.len >= 64) goto fallback;
		cross_side[nside] = 0;
		on_lhs[nside] = on_rhs[nside] = NULL;
		if (nside > 0) {
			const sql_expr_t *on = s->on, *l, *r;
			if (s->join != SX_J_INNER && s->join != SX_J_NONE &&
			    s->join != SX_J_CROSS)
				goto fallback;   /* outer joins: N-way not supported */
			if (on == NULL) {
				/* Keyless CROSS / comma join: full Cartesian product. */
				cross_side[nside] = 1;
			} else {
				if (on->op != SX_E_BINARY || on->op2 != TK_EQ)
					goto fallback;
				l = is_col_ref(on->a); r = is_col_ref(on->b);
				if (l == NULL || r == NULL) goto fallback;
				on_lhs[nside] = l; on_rhs[nside] = r;
			}
		}
		src[nside] = s;
		nside++;
	}
	if (nside < 2) goto fallback;   /* single table goes elsewhere */

	jc.nside = nside;
	for (side = 0; side < nside; side++) {
		memcpy(jc.tab[side], src[side]->table.p, src[side]->table.len);
		jc.tab[side][src[side]->table.len] = '\0';
		if (src[side]->alias.len && src[side]->alias.len < 64) {
			memcpy(jc.alias[side], src[side]->alias.p, src[side]->alias.len);
			jc.alias[side][src[side]->alias.len] = '\0';
		}
	}
	jc_schema_load(db, &jc);

	/* SELECT * + aggregates fall back on the join path. */
	for (it = sel->cols ? sel->cols->head : NULL; it; it = it->next) {
		if (it->expr == NULL || it->expr->op == SX_E_STAR) goto fallback;
		nproj++;
	}
	if (nproj == 0 || nproj > 32) goto fallback;

	/* Pass 1: collect columns from the projection (aggregating: every
	 * clause), WHERE, and every ON. */
	if (agg) {
		if (jc_collect_agg(&jc, sel) != 0) goto fallback;
	} else {
		for (it = sel->cols->head; it; it = it->next)
			if (jc_collect(&jc, it->expr) != 0) goto fallback;
	}
	if (sel->where && jc_collect(&jc, sel->where) != 0) goto fallback;
	for (side = 1; side < nside; side++) {
		if (cross_side[side]) continue;   /* keyless: no ON columns */
		if (jc_collect(&jc, on_lhs[side]) != 0) goto fallback;
		if (jc_collect(&jc, on_rhs[side]) != 0) goto fallback;
	}
	/* ORDER BY key columns must also be sourced (unless a bare ordinal or
	 * an output-column expression matched at build time). */
	if (sel->order && !agg) {
		const sql_exprlist_item_t *o;
		for (o = sel->order->head; o; o = o->next) {
			if (o->expr && o->expr->op == SX_E_NUMBER) continue;
			if (order_key_alias_pos(sel, o->expr)) continue;
			if (jc_collect(&jc, o->expr) != 0) goto fallback;
		}
	}
	if (jc_fill_empty(&jc) != 0) goto fallback;
	for (side = 0; side < nside; side++)
		if (jc.col[side].n > 16) goto fallback;

	st = (struct vx_stmt *)calloc(1, sizeof *st);
	if (!st) goto oom;
	st->db = db; st->cur = -1; st->limit = -1;
	st->binds = binds; st->nbinds = nbinds;
	st->distinct = !agg && sel->distinct;
	jp = (vx_njoinplan_t *)calloc(1, sizeof *jp);
	if (!jp) goto oom;
	st->njoin = jp;
	jp->nside = nside;
	st->nout = nproj;
	/* Over-allocate proj[] for ORDER-BY-only extra columns (like the
	 * 2-table join): up to 32 total output+sort columns. */
	st->proj = (vx_expr_t **)calloc(32, sizeof(vx_expr_t *));
	if (!st->proj) goto oom;

	/* Build each side's source SELECT, prepare it for ncol + affinity,
	 * and lay out the combined-row offsets. */
	for (side = 0; side < nside; side++) {
		char cols[400]; int off = 0, j;
		for (j = 0; j < jc.col[side].n; j++) {
			int w = snprintf(cols + off, sizeof cols - (size_t)off, "%s%s",
			                 j ? "," : "", jc.col[side].names[j]);
			if (w < 0 || (size_t)(off + w) >= sizeof cols) goto fallback;
			off += w;
		}
		snprintf(jp->src_sql[side], sizeof jp->src_sql[side],
		         "SELECT %s FROM %s", cols, jc.tab[side]);
		if (jsrc_build(db, jc.tab[side], jc.col[side].names, jc.col[side].n,
		               &jp->src[side]) < 0)
			goto fallback;
		jp->ncol[side] = jc.col[side].n;
		jp->base[side] = total;
		jc.base[side] = total;
		/* Column affinity from the native catalog (the side is xstore). */
		if (!jp->src[side].native) goto fallback;
		for (j = 0; j < jp->ncol[side]; j++)
			jc.col[side].aff[j] = jp->src[side].aff[j];
		total += jp->ncol[side];
	}
	if (total > 64) goto fallback;   /* combined-row scratch bound */

	/* For each build side, resolve its ON to a (probe-from-earlier,
	 * key-on-this-side) pair.  One ON column must land on side `side`
	 * (its local key); the other must land on an EARLIER side (the probe
	 * value).  Reject anything else (e.g. both on the same side, or a
	 * forward reference). */
	for (side = 1; side < nside; side++) {
		int li, ri, this_lo, this_hi, local = -1, probe = -1;
		if (cross_side[side]) {
			/* Keyless CROSS/comma side: no probe.  Every stored row
			 * matches -- executed as an all-rows chain. */
			jp->cross[side] = 1;
			jp->key_local[side] = 0;
			jp->probe_outidx[side] = 0;
			continue;
		}
		li = jc_resolve(&jc, on_lhs[side]);
		ri = jc_resolve(&jc, on_rhs[side]);
		this_lo = jp->base[side]; this_hi = jp->base[side] + jp->ncol[side];
		if (li < 0 || ri < 0) goto fallback;
		if (li >= this_lo && li < this_hi) { local = li; probe = ri; }
		else if (ri >= this_lo && ri < this_hi) { local = ri; probe = li; }
		else goto fallback;   /* neither ON column is on this side */
		if (probe >= this_lo) goto fallback;   /* probe must come from an earlier side */
		jp->key_local[side] = local - this_lo;
		jp->probe_outidx[side] = probe;
	}

	/* Pass 2: compile projection + filter over the combined row. */
	comp.st = st; comp.nv = NULL; comp.jc = &jc; comp.fail = 0;
	comp.binds = binds; comp.nbinds = nbinds; comp.ag = NULL;
	if (agg) {
		int r = join_identity_proj(st, &comp, total);
		if (r < 0) goto oom;
		if (r == 0) goto fallback;
		nproj = st->nout;
	} else {
		int k = 0;
		for (it = sel->cols->head; it; it = it->next, k++) {
			st->proj[k] = compile_expr(&comp, it->expr);
			if (comp.fail) goto fallback;
			item_name(it, st->outname[k], sizeof st->outname[0]);
		}
	}
	if (sel->where) {
		st->filter = compile_expr(&comp, sel->where);
		if (comp.fail) goto fallback;
	}
	for (side = 0; side < nproj; side++) {
		st->outaff[side] = node_class(&comp, st->proj[side]);
		st->outnoaff[side] = (unsigned char)expr_noaff(&comp, st->proj[side]);
	}
	/* An index nested loop when every inner side can seek: the predicate
	 * is WHERE AND every ON (all INNER here), re-checked per row. */
	{
		const vx_jsrc_t *srcs[VX_JOIN_MAX];
		vx_expr_t *cond = st->filter;
		for (side = 0; side < nside; side++) srcs[side] = &jp->src[side];
		for (side = 1; side < nside && !comp.fail; side++) {
			vx_expr_t *l, *r, *eqn;
			if (cross_side[side]) continue;
			l = compile_expr(&comp, on_lhs[side]);
			r = compile_expr(&comp, on_rhs[side]);
			eqn = expr_node(&comp, VXO_EQ);
			if (comp.fail || l == NULL || r == NULL || eqn == NULL) break;
			eqn->a = l; eqn->b = r;
			cond = ap_and(&comp, cond, eqn);
		}
		if (!comp.fail)
			(void)nl_plan(db, st, &jc, srcs, cond);
		comp.fail = 0;
	}

	/* ORDER BY / LIMIT / OFFSET over the N-way join output, applied as a
	 * post-collection sort (join_ordered_materialize), exactly like the
	 * 2-table join.  Order keys reference OUTPUT columns (an ordinal, an
	 * output alias, or a select item matched by expr_same); a key that is
	 * not an output column is compiled as an extra sort-only projection
	 * appended after the user columns. */
	st->limit = -1;
	st->offset = 0;
	if (sel->order && !agg) {
		const sql_exprlist_item_t *o;
		int no = 0, oi = 0, nextra = 0;
		for (o = sel->order->head; o; o = o->next) no++;
		if (no == 0 || no > 16) goto fallback;
		st->norder = no;
		st->order_key = (vx_expr_t **)calloc((size_t)no, sizeof(vx_expr_t *));
		st->order_outcol = (int *)calloc((size_t)no, sizeof(int));
		st->order_desc = (int *)calloc((size_t)no, sizeof(int));
		if (!st->order_key || !st->order_outcol || !st->order_desc) goto oom;
		for (o = sel->order->head; o; o = o->next, oi++) {
			const sql_expr_t *e = o->expr;
			int apos;
			st->order_desc[oi] = ord_flags(o);
			if (e && e->op == SX_E_NUMBER) {
				char buf[32]; long pos;
				if (e->lit.len == 0 || e->lit.len >= sizeof buf) goto fallback;
				memcpy(buf, e->lit.p, e->lit.len); buf[e->lit.len] = '\0';
				if (strchr(buf, '.') || strchr(buf, 'e') || strchr(buf, 'E')) goto fallback;
				pos = strtol(buf, NULL, 10);
				if (pos < 1 || pos > nproj) goto fallback;
				st->order_outcol[oi] = (int)pos;
			} else if ((apos = order_key_alias_pos(sel, e)) != 0) {
				st->order_outcol[oi] = apos;
			} else {
				const sql_exprlist_item_t *ci; int pos = 0, found = 0;
				for (ci = sel->cols->head; ci; ci = ci->next, pos++)
					if (expr_same(e, ci->expr)) { st->order_outcol[oi] = pos + 1; found = 1; break; }
				if (!found) {
					int slot = nproj + nextra;
					if (slot >= 32) goto fallback;
					comp.fail = 0;
					st->proj[slot] = compile_expr(&comp, e);
					if (comp.fail) goto fallback;
					st->order_outcol[oi] = slot + 1;
					nextra++;
				}
			}
		}
		if (nextra > 0)
			st->join_emit = nproj + nextra;
		if (nextra > 0 && st->distinct) goto fallback;   /* see the 2-way join */
	}
	if (sel->limit && !agg) {
		char buf[32];
		if (sel->limit->op != SX_E_NUMBER || sel->limit->lit.len >= sizeof buf) goto fallback;
		memcpy(buf, sel->limit->lit.p, sel->limit->lit.len); buf[sel->limit->lit.len] = '\0';
		if (strchr(buf, '.') || strchr(buf, 'e') || strchr(buf, 'E')) goto fallback;
		st->limit = strtoll(buf, NULL, 10);
		if (st->limit < 0) st->limit = 0;
	}
	if (sel->offset && !agg) {
		char buf[32];
		if (sel->offset->op != SX_E_NUMBER || sel->offset->lit.len >= sizeof buf) goto fallback;
		memcpy(buf, sel->offset->lit.p, sel->offset->lit.len); buf[sel->offset->lit.len] = '\0';
		if (strchr(buf, '.') || strchr(buf, 'e') || strchr(buf, 'E')) goto fallback;
		st->offset = strtoll(buf, NULL, 10);
		if (st->offset < 0) st->offset = 0;
	}

	st->nsrc_col = total;
	st->srcrow = (vx_cell_t *)calloc((size_t)total, sizeof(vx_cell_t));
	if (!st->srcrow) goto oom;

	if (agg) {
		int r = join_agg_wrap(st, &jc, sel, binds, nbinds, out);
		st = NULL; jp = NULL;   /* the wrapper owns them (freed on failure) */
		if (r != 1) { rc = r; goto fallback; }
		jc_schema_free(&jc);
		sql_arena_destroy(ast);
		return 1;
	}
	jc_schema_free(&jc);
	sql_arena_destroy(ast);
	*out = st;
	return 1;

oom:
	rc = -1;
fallback:
	jc_schema_free(&jc);
	if (ast) sql_arena_destroy(ast);
	if (st) vx_finalize(st);   /* frees st->njoin */
	else free(jp);
	return rc;
}

/*
 * A SELECT over a MATERIALIZED source: a derived table FROM ( select ),
 * the single CTE or a view named in FROM (both a stored select), or no
 * FROM at all (one row of no columns).  The source select runs once
 * through vexec -- from its AST, with the statement's binds, so its own
 * ? keep their ordinals -- and its result rows are the source rows.
 * Over them the outer query is a plain projection (WHERE, DISTINCT,
 * ORDER BY, LIMIT) or an aggregate (GROUP BY, HAVING, aggregates).
 *
 * src_sel != NULL: run that select (an AST owned by the caller's arena);
 * else src_sql (a stored view text, parsed here); neither: no FROM.
 * rename (n entries) renames the source's columns (a CTE column list).
 */
static int
vx_try_prepare_mat(struct xsql *db, sql_arena_t *ast, const sql_select_t *sel,
                   const sql_select_t *src_sel, const char *src_sql,
                   const sql_str_t *rename, int nrename,
                   const vx_cell_t *binds, int nbinds,
                   vx_stmt_t **out, char **errmsg)
{
	struct vx_stmt *st = NULL;
	struct vx_compiler comp;
	struct namevec nv;
	const sql_exprlist_item_t *it;
	vx_result_t *ires = NULL;
	sql_arena_t *vast = NULL;
	int nproj = 0, i, k, rc = 0, ncol, has_agg = (sel->group != NULL), r;

	if (errmsg) *errmsg = NULL;
	for (it = sel->cols ? sel->cols->head : NULL; it; it = it->next) {
		if (it->expr == NULL) goto fallback;
		if (it->expr->op == SX_E_STAR && it->expr->nname > 0) goto fallback;
		if (expr_has_agg(it->expr)) has_agg = 1;
	}
	if (sel->having && !has_agg) goto fallback;   /* SQLite: an error */

	/* Run the source. */
	if (src_sel == NULL && src_sql != NULL) {
		sql_stmt_t *root = NULL;
		const char *perr = NULL;
		if (sql_parse_ast(src_sql, strlen(src_sql), &vast, &root, &perr) != 0 ||
		    root == NULL || root->next != NULL || root->kind != SQL_KIND_SELECT ||
		    root->u.select == NULL)
			goto fallback;
		src_sel = root->u.select;
		binds = NULL; nbinds = 0;   /* a view has no parameters of its own */
	}
	if (src_sel != NULL) {
		r = src_sel->setop != SX_SET_NONE
		    ? run_setop_sel(db, src_sel, binds, nbinds, &ires, NULL)
		    : vx_run_sel(db, src_sel, binds, nbinds, &ires);
		if (r < 0) { rc = r; goto fallback; }
		if (r != 1 || ires == NULL) goto fallback;
		ncol = vx_result_ncol(ires);
	} else {
		ires = (vx_result_t *)calloc(1, sizeof *ires);
		if (ires == NULL) { rc = -1; goto fallback; }
		ires->nrow = 1;   /* no FROM: one row of no columns */
		ncol = 0;
	}
	if (ncol > 32) goto fallback;
	if (rename != NULL) {
		if (nrename != ncol) goto fallback;
		for (i = 0; i < ncol; i++) {
			if (rename[i].len == 0 || rename[i].len >= sizeof ires->name[0])
				goto fallback;
			memcpy(ires->name[i], rename[i].p, rename[i].len);
			ires->name[i][rename[i].len] = '\0';
		}
	}

	/* The source columns, by name (a closed set), with their affinity. */
	memset(&nv, 0, sizeof nv);
	for (i = 0; i < ncol; i++) {
		const char *cn = vx_result_name(ires, i);
		sql_str_t snm;
		if (cn == NULL) goto fallback;
		snm.p = cn; snm.len = (uint32_t)strlen(cn);
		if (nv_add(&nv, &snm) != i) goto fallback;   /* duplicate name */
		nv.aff[i] = ires->aff[i];
		nv.noaff[i] = ires->noaff[i];
	}
	nv.fixed = 1;

	/* SELECT * / mixed with expressions: every source column, in order. */
	for (it = sel->cols ? sel->cols->head : NULL; it; it = it->next)
		nproj += (it->expr->op == SX_E_STAR) ? ncol : 1;
	if (nproj == 0 || nproj > 32) goto fallback;
	if (has_agg) {
		for (it = sel->cols->head; it; it = it->next)
			if (it->expr->op == SX_E_STAR) goto fallback;
	}

	st = (struct vx_stmt *)calloc(1, sizeof *st);
	if (!st) { rc = -1; goto fallback; }
	st->db = db; st->cur = -1; st->limit = -1;
	st->binds = binds; st->nbinds = nbinds;
	st->nsrc_col = ncol;
	st->proj = (vx_expr_t **)calloc(32, sizeof(vx_expr_t *));
	if (!st->proj) { rc = -1; goto fallback; }
	comp.st = st; comp.nv = &nv; comp.jc = NULL; comp.fail = 0;
	comp.binds = binds; comp.nbinds = nbinds; comp.ag = NULL;

	if (sel->where) {
		st->filter = compile_expr(&comp, sel->where);
		if (comp.fail || st->filter == NULL) goto fallback;
	}
	if (has_agg) {
		r = agg_build(&comp, sel, nproj);
		if (r < 0) { rc = -1; goto fallback; }
		if (r == 0) goto fallback;
	} else {
		k = 0;
		for (it = sel->cols->head; it; it = it->next) {
			if (it->expr->op == SX_E_STAR) {
				for (i = 0; i < ncol; i++, k++) {
					vx_expr_t *n = expr_node(&comp, VXO_COL);
					if (n == NULL) goto fallback;
					n->col = i; n->noaff = nv.noaff[i];
					st->proj[k] = n;
					snprintf(st->outname[k], sizeof st->outname[0], "%s", nv.names[i]);
					st->outaff[k] = nv.aff[i];
					st->outnoaff[k] = nv.noaff[i];
				}
				continue;
			}
			st->proj[k] = compile_expr(&comp, it->expr);
			if (comp.fail || st->proj[k] == NULL) goto fallback;
			item_name(it, st->outname[k], sizeof st->outname[0]);
			st->outaff[k] = node_class(&comp, st->proj[k]);
			st->outnoaff[k] = (unsigned char)expr_noaff(&comp, st->proj[k]);
			k++;
		}
		st->nout = nproj;
		st->distinct = sel->distinct;
		/* ORDER BY: an output position or alias, an expression equal to
		 * an output, or any source expression (a sort-only column). */
		if (sel->order) {
			const sql_exprlist_item_t *o;
			int no = 0, oi = 0, nemit = nproj;
			for (o = sel->order->head; o; o = o->next) no++;
			if (no > 16) goto fallback;
			st->norder = no;
			st->order_key = (vx_expr_t **)calloc((size_t)no, sizeof(vx_expr_t *));
			st->order_outcol = (int *)calloc((size_t)no, sizeof(int));
			st->order_desc = (int *)calloc((size_t)no, sizeof(int));
			if (!st->order_key || !st->order_outcol || !st->order_desc) {
				rc = -1; goto fallback;
			}
			for (o = sel->order->head; o; o = o->next, oi++) {
				const sql_expr_t *e = o->expr;
				const sql_exprlist_item_t *ci;
				int64_t v;
				int pos = 0;
				st->order_desc[oi] = ord_flags(o);
				if (e && e->op == SX_E_NUMBER) {
					if (!lit_int(e, &v, NULL, 0) || v < 1 || v > nproj) goto fallback;
					pos = (int)v;
				} else if ((pos = order_key_alias_pos(sel, e)) == 0) {
					k = 1;
					for (ci = sel->cols->head; ci; ci = ci->next, k++)
						if (ci->expr->op != SX_E_STAR && expr_same(e, ci->expr)) {
							pos = k; break;
						}
				}
				if (pos == 0) {
					/* A sort-only column; DISTINCT would dedup on it too. */
					if (st->distinct || nemit >= 32) goto fallback;
					st->proj[nemit] = compile_expr(&comp, e);
					if (comp.fail || st->proj[nemit] == NULL) goto fallback;
					pos = ++nemit;
				}
				st->order_outcol[oi] = pos;
			}
			if (nemit > nproj) st->join_emit = nemit;
		}
		st->limit = -1;
		if (sel->limit) {
			if (!lit_int(sel->limit, &st->limit, binds, nbinds)) goto fallback;
			if (st->limit < 0) st->limit = -1;
		}
		if (sel->offset) {
			if (!lit_int(sel->offset, &st->offset, binds, nbinds)) goto fallback;
			if (st->offset < 0) st->offset = 0;
		}
		/* DISTINCT + LIMIT keeps which rows survive reproducible only
		 * under an ORDER BY (as on the base-table path). */
		if (st->distinct && (st->limit >= 0 || st->offset > 0) && st->norder == 0)
			goto fallback;
	}

	st->srcrow = (vx_cell_t *)calloc((size_t)(ncol > 0 ? ncol : 1), sizeof(vx_cell_t));
	if (!st->srcrow) { rc = -1; goto fallback; }
	st->derived_res = ires;   /* the row source; vx_finalize frees it */
	st->noreuse = 1;          /* materialized data: rebuild each run */
	st->derived_cur = 0;
	ires = NULL;

	sql_arena_destroy(vast);
	sql_arena_destroy(ast);
	*out = st;
	return 1;

fallback:
	if (ires) vx_result_free(ires);
	sql_arena_destroy(vast);
	if (ast) sql_arena_destroy(ast);
	if (st) { st->derived_res = NULL; vx_finalize(st); }
	return rc;
}

static int
join_build(struct vx_stmt *st, vx_jht_t *bh)
{
	vx_joinplan_t *jp = st->join;
	xstore_scan_t *scan = NULL;
	struct vx_arena_blk *tmp = NULL;
	vx_cell_t rowcells[16];
	int rc = -1;

	memset(bh, 0, sizeof *bh);
	bh->nbucket = 1024;
	bh->buckets = (vx_jrow_t **)calloc((size_t)bh->nbucket, sizeof(vx_jrow_t *));
	if (bh->buckets == NULL) return -1;

	if (!jp->build_src.native) goto done;
	scan = xstore_scan_open_txn((struct xsql *)st->db,   /* txn view */
	                            jp->build_src.table, 0, 0, 0, 0, 0);
	if (scan == NULL) goto done;
	for (;;) {
		{
			int64_t rid; const uint8_t *rec; int reclen;
			int step = xstore_scan_next(scan, &rid, &rec, &reclen);
			if (step == 0) break;
			if (step != 1) goto done;
			decode_rec_cells(jp->build_src.pay, jp->build_ncol, rid, rec, reclen,
			                 rowcells, &tmp);
		}
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
	if (scan) xstore_scan_close(scan);
	return rc;
}

/* ---- execution --------------------------------------------------- */

/* Decode an xstore record into `out` (ncol cells) per the payload map
 * `pay` (pay[i] == -1 -> the rowid; else payload column pay[i]).
 * TEXT/BLOB bytes are copied into `arena` so they outlive the record.
 * Shared by the single-table source (read_rec_row) and the native join
 * sources. */
static void
decode_rec_cells(const int *pay, int ncol, int64_t rowid,
                 const uint8_t *rec, int reclen,
                 vx_cell_t *out, struct vx_arena_blk **arena)
{
	xstore_val_t col[64], far;  /* bounded: this runs on fiber stacks */
	int i, nrec, need = 0;

	/* One pass over the record's first `need` columns (the highest one
	 * the map reads, at most 64); the map then picks them.  A column
	 * past col[] (a table that wide is rare) is decoded on its own. */
	for (i = 0; i < ncol; i++)
		if (pay[i] >= need) need = pay[i] + 1;
	nrec = xstore_rec_cols(rec, reclen, col, need < 64 ? need : 64);
	for (i = 0; i < ncol; i++) {
		vx_cell_t *cell = &out[i];
		const xstore_val_t *v;
		memset(cell, 0, sizeof *cell);
		if (pay[i] < 0) {
			cell->type = VX_INT; cell->i = rowid;
			continue;
		}
		if (pay[i] >= 64) {
			memset(&far, 0, sizeof far);
			far.cls = xstore_rec_col(rec, reclen, pay[i], &far.i,
			    &far.r, &far.p, &far.n);
			v = &far;
		} else if (pay[i] >= nrec) {
			cell->type = VX_NULL;
			continue;
		} else {
			v = &col[pay[i]];
		}
		switch (v->cls) {
		case XSTORE_C_INT:  cell->type = VX_INT; cell->i = v->i; break;
		case XSTORE_C_REAL: cell->type = VX_REAL; cell->r = v->r; break;
		case XSTORE_C_TEXT:
		case XSTORE_C_BLOB: {
			int n = v->n;
			uint8_t *b = (uint8_t *)arena_alloc(arena, (size_t)(n > 0 ? n : 1) + 1);
			if (b) { if (n) memcpy(b, v->p, (size_t)n); b[n > 0 ? n : 0] = '\0'; }
			cell->type = (v->cls == XSTORE_C_TEXT) ? VX_TEXT : VX_BLOB;
			cell->bytes = b; cell->nbytes = (uint32_t)(n > 0 ? n : 0);
			break;
		}
		default: cell->type = VX_NULL; break;
		}
	}
}

/* Populate a join side's row source for `table` over the columns named
 * in `names` (ncol of them).  If the table is xstore-backed, builds the
 * native payload map (pay[i] = -1 for the pk/rowid, else payload index)
 * and sets native=1.  Returns 1 if native, 0 if not xstore-backed (the
 * caller reads the side through SQLite), or -1 on overflow/mismatch. */
static int
jsrc_build(struct xsql *db, const char *table, char names[][64], int ncol,
           vx_jsrc_t *js)
{
	bt_t *bt = xstore_bt_of(db);
	xstore_col_t cols[64];
	int nc, i, c;
	memset(js, 0, sizeof *js);
	js->ncol = ncol;
	if (ncol > (int)(sizeof js->pay / sizeof js->pay[0])) return -1;
	if (bt == NULL) return -1;
	nc = xstore_table_schema(bt, table, cols, 64);
	if (nc <= 0) return -1;   /* not an xstore table -> fall back (no SQLite source) */
	for (i = 0; i < ncol; i++) js->pay[i] = -2;
	for (i = 0; i < ncol; i++)
		for (c = 0; c < nc; c++)
			if (strcasecmp(names[i], cols[c].name) == 0) {
				js->pay[i] = cols[c].is_pk ? -1 : (c - 1);
				/* The PK is the INTEGER rowid (numeric affinity)
				 * regardless of its declared type, matching SQLite. */
				js->aff[i] = cols[c].is_pk ? VX_AFF_NUMERIC
				                          : aff_class(vx_affinity(cols[c].decltype));
				break;
			}
	for (i = 0; i < ncol; i++) if (js->pay[i] == -2) return -1;   /* unresolved */
	snprintf(js->table, sizeof js->table, "%s", table);
	js->native = 1;
	return 1;
}

/* Fill the storage-native single-table source row from a scan record. */
static void
read_rec_row(struct vx_stmt *st, int64_t rowid, const uint8_t *rec, int reclen,
             struct vx_arena_blk **arena)
{
	decode_rec_cells(st->src_pay, st->nsrc_col, rowid, rec, reclen,
	                 st->srcrow, arena);
}

static vx_chunk_t *
next_chunk(struct vx_stmt *st, int *done)
{
	vx_chunk_t *c;
	int rc;

	*done = 0;
	c = (vx_chunk_t *)calloc(1, sizeof *c);
	if (!c) return NULL;
	/* Outputs plus any sort-only columns (an ordered materialized source;
	 * join_ordered_materialize keeps the first nout). */
	c->ncol = st->join_emit > 0 ? st->join_emit : st->nout;
	c->cap = VEXEC_VECTOR_SIZE;
	c->cells = (vx_cell_t *)malloc(sizeof(vx_cell_t) * (size_t)c->cap *
	                               (size_t)(c->ncol > 0 ? c->ncol : 1));
	if (!c->cells) { free(c); return NULL; }

	while (c->nrow < c->cap) {
		int j;
		if (st->derived_res != NULL) {
			/* Derived table: take the next materialized inner row into
			 * srcrow (a shallow copy; the bytes live in the inner result's
			 * arena, which outlives this scan via st->derived_res). */
			vx_result_t *ir = st->derived_res;
			if (st->derived_cur >= ir->nrow) { *done = 1; break; }
			for (j = 0; j < st->nsrc_col; j++)
				st->srcrow[j] = ir->cells[(size_t)st->derived_cur * (size_t)ir->ncol + (size_t)j];
			st->derived_cur++;
		} else {
			int64_t rowid; const uint8_t *rec; int reclen;
			rc = st_src_next(st, &rowid, &rec, &reclen);
			if (rc == 0) { *done = 1; break; }
			if (rc != 1) { chunk_free(c); return NULL; }
			read_rec_row(st, rowid, rec, reclen, &c->arena);
		}

		/* Filter (3-valued: only a TRUE keeps the row). */
		if (st->filter != NULL) {
			int b = eval_bool(st, st->filter, st->srcrow, &c->arena);
			if (b != 1) continue;
		}

		/* Project. */
		{
			vx_cell_t *dst = &c->cells[(size_t)c->nrow * (size_t)c->ncol];
			for (j = 0; j < c->ncol; j++)
				eval(st, st->proj[j], st->srcrow, &c->arena, &dst[j]);
		}
		c->nrow++;
	}
	return c;
}

/* Sort an aggregated result chunk's rows in place by the ORDER BY keys
 * (output-column references) and apply OFFSET/LIMIT.  Defined after the
 * sort machinery below. */
static int agg_order_limit(struct vx_stmt *st, vx_chunk_t *c);

/* Fold one source row into its group: evaluate the keys, find or create
 * the group, step every accumulator.  The row's bytes are scratch in
 * `arena`; the group copies what it keeps into ht's arena.  Returns the
 * group, or NULL on OOM. */
static vx_grp_t *
agg_step_row(const struct vx_stmt *st, const vx_aggplan_t *ap, vx_htab_t *ht,
             const vx_cell_t *row, struct vx_arena_blk **arena)
{
	vx_cell_t keys[16];
	vx_grp_t *grp;
	int k;
	for (k = 0; k < ap->ngrp; k++)
		eval(st, ap->grp[k], row, arena, &keys[k]);
	grp = htab_group(ht, keys);
	if (grp == NULL) return NULL;
	for (k = 0; k < ap->nagg; k++) {
		const vx_aggacc_t *a = &ap->acc[k];
		vx_cell_t v;
		if (a->kind == VXA_COUNT_STAR) {
			acc_step(&grp->accs[k], VXA_COUNT_STAR, NULL, &ht->arena);
			continue;
		}
		if (a->kind == VXA_GROUP_CONCAT && a->gc_sep) {
			grp->accs[k].gc_sep = a->gc_sep;
			grp->accs[k].gc_seplen = a->gc_seplen;
		}
		eval(st, a->arg, row, arena, &v);
		acc_step(&grp->accs[k], a->kind, &v, &ht->arena);
	}
	return grp;
}

/* Emit group `g` into `dst` (ap->nemit cells, TEXT/BLOB copied into
 * `keep`): its group row, HAVING over it, then the outputs.  Returns 1
 * emitted, 0 HAVING rejected it. */
static int
agg_emit_group(const struct vx_stmt *st, const vx_aggplan_t *ap,
               const vx_grp_t *g, vx_cell_t *dst, struct vx_arena_blk **keep)
{
	vx_cell_t grow[VX_GROW_MAX];
	struct vx_arena_blk *tmp = NULL;
	int k, ok = 1;
	for (k = 0; k < ap->ngrp; k++) grow[k] = g->keys[k];
	for (k = 0; k < ap->nagg; k++)
		acc_final(&g->accs[k], ap->acc[k].kind, &grow[ap->ngrp + k]);
	if (ap->having != NULL && eval_bool(st, ap->having, grow, &tmp) != 1)
		ok = 0;
	for (k = 0; ok && k < ap->nemit; k++) {
		vx_cell_t v;
		eval(st, ap->proj[k], grow, &tmp, &v);
		if (cell_dup(keep, &v, &dst[k]) != 0) dst[k].type = VX_NULL;
	}
	arena_free(tmp);
	return ok;
}

/* Groups are emitted in GROUP BY key order (sort_cmp, as SQLite's GROUP
 * BY sorter or index walk returns them), so a LIMIT without ORDER BY, and
 * ORDER BY ties, pick the groups SQLite picks.  qsort has no context
 * argument; the key count is per thread (qsort does not yield). */
static _Thread_local int g_grp_nkey;

static int
grp_key_cmp(const void *pa, const void *pb)
{
	const vx_grp_t *a = *(const vx_grp_t *const *)pa;
	const vx_grp_t *b = *(const vx_grp_t *const *)pb;
	int k, r;
	for (k = 0; k < g_grp_nkey; k++)
		if ((r = sort_cmp(&a->keys[k], &b->keys[k])) != 0) return r;
	return 0;
}

static int row_cells_eq(const vx_cell_t *a, const vx_cell_t *b, int ncol);

static vx_chunk_t *nl_next_chunk(struct vx_stmt *st, int *done);
static vx_chunk_t *join_next_chunk(struct vx_stmt *st, int *done);
static vx_chunk_t *njoin_next_chunk(struct vx_stmt *st, int *done);
static int vx_join_open(struct vx_stmt *st);

/* Drain the source into the hash table and build one result chunk with
 * one row per group.  The source is the base-table scan, a materialized
 * row set (derived table, view, CTE, no FROM), or a join's combined rows
 * (st->agg_in).  Returns 0 on success, -1 on error. */
static int
agg_materialize(struct vx_stmt *st)
{
	vx_aggplan_t *ap = st->agg;
	vx_chunk_t *c;
	struct vx_arena_blk *rowarena = NULL;
	vx_grp_t *g, **gs;
	int b, r, ng = 0;

	if (htab_init(&st->ht, ap->ngrp, ap->nagg) != 0) return -1;

	if (st->agg_in != NULL) {
		/* A join: each chunk row is the whole combined row. */
		struct vx_stmt *js = st->agg_in;
		int done = 0;
		if (vx_join_open(js) != 0) return -1;
		while (!done) {
			vx_chunk_t *jc = js->nl ? nl_next_chunk(js, &done)
			    : js->join ? join_next_chunk(js, &done)
			    : njoin_next_chunk(js, &done);
			if (jc == NULL) return -1;
			for (r = 0; r < jc->nrow; r++) {
				const vx_cell_t *row = &jc->cells[(size_t)r * (size_t)jc->ncol];
				arena_reset(&rowarena);
				if (st->filter != NULL &&
				    eval_bool(st, st->filter, row, &rowarena) != 1)
					continue;
				if (agg_step_row(st, ap, &st->ht, row, &rowarena) == NULL) {
					chunk_free(jc); arena_free(rowarena); return -1;
				}
			}
			chunk_free(jc);
		}
	} else for (;;) {
		/* Per-row scratch: keys and accumulator values are copied into
		 * the hash table's own arena (cell_dup). */
		vx_grp_t *grp;
		arena_reset(&rowarena);
		if (st->derived_res != NULL) {
			vx_result_t *ir = st->derived_res;
			if (st->derived_cur >= ir->nrow) break;
			for (r = 0; r < st->nsrc_col; r++)
				st->srcrow[r] = ir->cells[(size_t)st->derived_cur *
				    (size_t)ir->ncol + (size_t)r];
			st->derived_cur++;
		} else {
			int64_t rowid; const uint8_t *rec; int reclen;
			int step = st_src_next(st, &rowid, &rec, &reclen);
			if (step == 0) break;
			if (step != 1) { arena_free(rowarena); return -1; }
			read_rec_row(st, rowid, rec, reclen, &rowarena);
		}
		if (st->filter != NULL &&
		    eval_bool(st, st->filter, st->srcrow, &rowarena) != 1)
			continue;
		grp = agg_step_row(st, ap, &st->ht, st->srcrow, &rowarena);
		if (grp == NULL) { arena_free(rowarena); return -1; }
		/* MIN/MAX on a walk ordered by the argument: the first non-NULL
		 * value is the answer. */
		if (st->ap_minmax && st->src.ordered && grp->accs[0].seen) break;
	}
	arena_free(rowarena);

	/* A no-GROUP-BY aggregate over zero rows still yields one row (the
	 * empty-group aggregates: count=0, sum=NULL, etc.).  Create it. */
	if (ap->ngrp == 0 && st->ht.ngroup == 0) {
		vx_cell_t nokeys[1];
		memset(nokeys, 0, sizeof nokeys);
		if (htab_group(&st->ht, nokeys) == NULL) return -1;
	}

	/* One chunk row per group that passes HAVING: the outputs, then any
	 * ORDER-BY-only columns. */
	c = (vx_chunk_t *)calloc(1, sizeof *c);
	if (c == NULL) return -1;
	c->ncol = ap->nemit;
	c->cap = st->ht.ngroup > 0 ? st->ht.ngroup : 1;
	c->cells = (vx_cell_t *)malloc(sizeof(vx_cell_t) * (size_t)c->cap *
	                               (size_t)(c->ncol > 0 ? c->ncol : 1));
	if (c->cells == NULL) { free(c); return -1; }
	gs = (vx_grp_t **)malloc(sizeof *gs * (size_t)c->cap);
	if (gs == NULL) { chunk_free(c); return -1; }
	for (b = 0; b < st->ht.nbucket; b++)
		for (g = st->ht.buckets[b]; g; g = g->next) gs[ng++] = g;
	if (ap->ngrp > 0 && ng > 1) {
		g_grp_nkey = ap->ngrp;
		qsort(gs, (size_t)ng, sizeof *gs, grp_key_cmp);
	}
	for (r = 0; r < ng; r++) {
		vx_cell_t *dst = &c->cells[(size_t)c->nrow * (size_t)c->ncol];
		int d;
		if (!agg_emit_group(st, ap, gs[r], dst, &c->arena)) continue;
		/* DISTINCT over the outputs: keep a row's first occurrence. */
		for (d = 0; ap->distinct && d < c->nrow; d++)
			if (row_cells_eq(&c->cells[(size_t)d * (size_t)c->ncol], dst,
			    ap->nout)) break;
		if (ap->distinct && d < c->nrow) continue;
		c->nrow++;
	}
	free(gs);
	/* ORDER BY / LIMIT / OFFSET over the group rows (sorted in place),
	 * then keep only the output columns. */
	if (st->norder > 0 || st->limit >= 0 || st->offset > 0)
		if (agg_order_limit(st, c) != 0) { chunk_free(c); return -1; }
	if (ap->nemit > ap->nout) {
		for (r = 0; r < c->nrow; r++)
			memmove(&c->cells[(size_t)r * (size_t)ap->nout],
			    &c->cells[(size_t)r * (size_t)ap->nemit],
			    sizeof(vx_cell_t) * (size_t)ap->nout);
		c->ncol = ap->nout;
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

/* Compare two order-key cells under one key's VX_ORD_* flags.  DESC
 * negates the value order; a NULLS clause that differs from the default
 * (FIRST under DESC, LAST under ASC) re-places NULLs without touching the
 * non-NULL order. */
static int
ord_cmp(const vx_cell_t *a, const vx_cell_t *b, int flags)
{
	int r = sort_cmp(a, b);
	if ((flags & VX_ORD_NULLS) && (a->type == VX_NULL) != (b->type == VX_NULL))
		return (flags & VX_ORD_DESC) ? r : -r;
	return (flags & VX_ORD_DESC) ? -r : r;
}

/* qsort context: the order keys per row + directions.  Keys are stored
 * row-major: nkey cells per row, in a flat array parallel to the output
 * rows.  We sort an index array to keep the output rows paired. */
struct sort_ctx {
	const vx_cell_t *keys;   /* nrow * nkey */
	int              nkey;
	const int       *desc;   /* nkey */
};
/* qsort_r is not portable, so the comparator reads its context from
 * here.  Thread-local: ORDER BY queries run concurrently on different
 * loops (--threads N), and qsort runs to completion on one thread with
 * no fiber switch, so a per-thread slot is exact.  (A process-global
 * slot was overwritten and then NULLed by a concurrent sort: SIGSEGV in
 * sort_index_cmp under a 4-loop reader workload.) */
static _Thread_local struct sort_ctx *g_sort_ctx;
static _Atomic uint64_t g_vx_sorts;   /* sorts run (metric) */
uint64_t vx_sort_count(void) { return atomic_load(&g_vx_sorts); }

static int
sort_index_cmp(const void *pa, const void *pb)
{
	int ia = *(const int *)pa, ib = *(const int *)pb;
	const struct sort_ctx *c = g_sort_ctx;
	int k;
	for (k = 0; k < c->nkey; k++) {
		const vx_cell_t *ka = &c->keys[(size_t)ia * (size_t)c->nkey + (size_t)k];
		const vx_cell_t *kb = &c->keys[(size_t)ib * (size_t)c->nkey + (size_t)k];
		int r = ord_cmp(ka, kb, c->desc[k]);
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

/* Compare two key-rows (nkey cells each) under the ORDER BY direction
 * vector, returning <0 if row A sorts before row B, >0 if after, 0 if
 * equal on every key.  The bounded top-N path breaks the ties on the
 * rows' arrival numbers (topn_index_cmp), not here. */
static int
topn_cmp(const vx_cell_t *ka, const vx_cell_t *kb, int nkey, const int *desc)
{
	int k;
	for (k = 0; k < nkey; k++) {
		int r = ord_cmp(&ka[k], &kb[k], desc[k]);
		if (r) return r;
	}
	return 0;
}

/* The top-N buffer reuses slots, so a slot number is not the arrival
 * order; equal keys tie on the row's arrival sequence instead, which is
 * SQLite's sorter rule: LIMIT K is the first K rows of the unlimited
 * ORDER BY, ties in scan order. */
struct topn_ctx {
	const vx_cell_t *keys;
	const int64_t   *seq;
	int              nkey;
	const int       *desc;
};
static _Thread_local const struct topn_ctx *g_topn_ctx;

static int
topn_slot_cmp(int ia, int ib, void *ctx)
{
	const struct topn_ctx *c = ctx;
	int r = topn_cmp(&c->keys[(size_t)ia * (size_t)c->nkey],
	    &c->keys[(size_t)ib * (size_t)c->nkey], c->nkey, c->desc);
	if (r) return r;
	return c->seq[ia] < c->seq[ib] ? -1 : c->seq[ia] > c->seq[ib];
}

static int
topn_index_cmp(const void *pa, const void *pb)
{
	return topn_slot_cmp(*(const int *)pa, *(const int *)pb,
	    (void *)(uintptr_t)g_topn_ctx);
}

/* Bytes held by an arena's blocks, and the TEXT/BLOB bytes n cells
 * reference (cell_dup's rounded footprint). */
static size_t
arena_bytes(const struct vx_arena_blk *b)
{
	size_t n = 0;
	for (; b != NULL; b = b->next) n += b->cap;
	return n;
}

static size_t
topn_bytes(const vx_cell_t *c, size_t n)
{
	size_t t = 0, i;
	for (i = 0; i < n; i++)
		if ((c[i].type == VX_TEXT || c[i].type == VX_BLOB) && c[i].nbytes)
			t += ((size_t)c[i].nbytes + 8u) & ~(size_t)7u;
	return t;
}

/* Slot of the last row under (keys, arrival) among slots 0..nrow. */
static int
topn_worst(const struct topn_ctx *tc, int nrow)
{
	int r, w = 0;
	for (r = 1; r < nrow; r++)
		if (topn_slot_cmp(r, w, (void *)(uintptr_t)tc) > 0) w = r;
	return w;
}

/*
 * Cut the top-N buffer (nrow kept rows, slots 0..nrow) back to its k
 * best under (keys, arrival) with QuickSelect, and move them into slots
 * 0..k-1.  `v` is scratch for nrow ints.  Returns the slot of the k-th
 * best (the admission threshold): a later row is kept only if it sorts
 * ahead of it.  The dropped rows' bytes stay in the keep arena.
 */
static int
topn_cut(const struct topn_ctx *tc, vx_cell_t *rows, int nout, vx_cell_t *keys,
    int64_t *seq, int nrow, int k, int *v, uint64_t *rng)
{
	int i, hole = 0, worst, nk = tc->nkey;

	for (i = 0; i < nrow; i++) v[i] = i;
	qsel_nth(v, nrow, k, topn_slot_cmp, (void *)(uintptr_t)tc, rng);
	worst = v[k - 1];
	/* Flag the kept slots v[0..k) by negating their arrival numbers
	 * (always >= 1), then move each kept slot at or past k into a
	 * dropped slot below k: there are exactly as many of each. */
	for (i = 0; i < k; i++) seq[v[i]] = -seq[v[i]];
	for (i = k; i < nrow; i++) {
		if (seq[i] > 0) continue;              /* dropped */
		while (seq[hole] < 0) hole++;           /* a dropped slot below k */
		memcpy(&rows[(size_t)hole * (size_t)nout], &rows[(size_t)i * (size_t)nout],
		    sizeof *rows * (size_t)nout);
		memcpy(&keys[(size_t)hole * (size_t)nk], &keys[(size_t)i * (size_t)nk],
		    sizeof *keys * (size_t)nk);
		seq[hole] = seq[i];
		if (worst == i) worst = hole;
		hole++;
	}
	for (i = 0; i < k; i++) seq[i] = -seq[i];
	return worst;
}

/* Sort an aggregated result chunk (c->nrow rows, c->ncol cells each, in
 * c->cells) in place by the ORDER BY keys and apply OFFSET/LIMIT.  The
 * agg ORDER BY keys all reference OUTPUT columns: order_outcol[k] > 0 is
 * a 1-based output position; order_outcol[k] == 0 means order_key[k] is
 * a select-item expression resolved to an output column at compile time
 * (stored as a VXO_COL over the output row -- see the agg gate).  Sorts
 * an index permutation, then rewrites c->cells in the surviving order. */
static int
agg_order_limit(struct vx_stmt *st, vx_chunk_t *c)
{
	int nkey = st->norder, nr = c->nrow, nc = c->ncol;
	vx_cell_t *keys = NULL;
	int *idx = NULL;
	vx_cell_t *nrows = NULL;
	int64_t off, lim;
	int i, k, e, emit_n, rc = -1;

	if (nr <= 0) { /* still apply: empty stays empty */
		return 0;
	}
	/* Build the per-row key array from the output columns. */
	if (nkey > 0) {
		keys = (vx_cell_t *)malloc(sizeof(vx_cell_t) * (size_t)nr * (size_t)nkey);
		if (keys == NULL) goto done;
		for (i = 0; i < nr; i++)
			for (k = 0; k < nkey; k++) {
				int oc = st->order_outcol[k];   /* 1-based output col, or 0 */
				if (oc < 1 || oc > nc) { rc = -1; goto done; }
				keys[(size_t)i * (size_t)nkey + (size_t)k] =
				    c->cells[(size_t)i * (size_t)nc + (size_t)(oc - 1)];
			}
	}
	idx = (int *)malloc(sizeof(int) * (size_t)nr);
	if (idx == NULL) goto done;
	for (i = 0; i < nr; i++) idx[i] = i;
	if (nkey > 0) {
		struct sort_ctx sc; sc.keys = keys; sc.nkey = nkey; sc.desc = st->order_desc;
		g_sort_ctx = &sc;
		qsort(idx, (size_t)nr, sizeof(int), sort_index_cmp);
		g_sort_ctx = NULL;
	}
	/* OFFSET / LIMIT over the sorted rows. */
	off = st->offset > 0 ? st->offset : 0;
	lim = st->limit;
	if (off > nr) off = nr;
	emit_n = nr - (int)off;
	if (lim >= 0 && lim < emit_n) emit_n = (int)lim;
	if (emit_n < 0) emit_n = 0;
	/* Rewrite c->cells in surviving order (cells reference the chunk
	 * arena, which we keep -- a shallow reorder is correct). */
	nrows = (vx_cell_t *)malloc(sizeof(vx_cell_t) * (size_t)(emit_n > 0 ? emit_n : 1) *
	                            (size_t)(nc > 0 ? nc : 1));
	if (nrows == NULL) goto done;
	for (e = 0; e < emit_n; e++) {
		int src = idx[(int)off + e];
		for (k = 0; k < nc; k++)
			nrows[(size_t)e * (size_t)nc + (size_t)k] =
			    c->cells[(size_t)src * (size_t)nc + (size_t)k];
	}
	free(c->cells);
	c->cells = nrows; nrows = NULL;
	c->nrow = emit_n;
	c->cap = emit_n > 0 ? emit_n : 1;
	rc = 0;
done:
	free(keys); free(idx); free(nrows);
	return rc;
}

/* Materialize, sort, and offset/limit a non-agg ordered query into one
 * chunk that vx_step then walks.
 *
 * When LIMIT is present (and there is no DISTINCT), the scan keeps only
 * the best K = OFFSET + LIMIT rows rather than materializing the whole
 * input and sorting it.  Rows go into a buffer of 2K (at least K + 32);
 * when it fills, QuickSelect cuts it back to the K best and the K-th
 * becomes the admission threshold: a later row is buffered only if it
 * sorts ahead of it.  Expected O(N + K log K) comparisons for any
 * arrival order and memory for 2K rows.  (The previous scheme replaced
 * the worst kept row and rescanned all K for the next worst: O(N*K)
 * when rows arrive in reverse order, e.g. newest first over an
 * append-ordered table.)  Order is (keys, arrival), SQLite's: LIMIT K
 * is the first K rows of the unlimited ORDER BY. */
static int
ordered_materialize(struct vx_stmt *st)
{
	struct vx_arena_blk *rowarena = NULL;   /* transient per-row source bytes */
	struct vx_arena_blk *keep = NULL;        /* output + key bytes (kept) */
	vx_cell_t *rows = NULL;   /* cap * nout */
	vx_cell_t *keys = NULL;   /* cap * nkey */
	int64_t *seq = NULL, *ns; /* cap: arrival number of each kept row (top-N) */
	int64_t nseen = 0;        /* rows that passed the WHERE so far */
	int *idx = NULL;
	int nkey = st->norder;
	int cap = 0, nrow = 0, j, rc = -1;
	vx_chunk_t *c = NULL;
	int64_t lim, off;
	int emit_n, e;
	/* Bounded top-N: eligible when LIMIT is set, there are sort keys,
	 * and no DISTINCT (DISTINCT dedups the kept set, so a row dropped
	 * early could change which distinct values survive). */
	int64_t topn_k = -1;
	int topn = (st->limit >= 0 && nkey > 0 && !st->distinct);
	int topn_cap = 0;      /* buffer rows: a cut runs when it fills */
	int topn_thr = -1;     /* slot bounding the K-th best; -1 = none yet */
	int *topn_v = NULL;    /* QuickSelect scratch, topn_cap ints */
	uint64_t topn_rng = 0x9E3779B97F4A7C15ULL;   /* pivots only: the cut is a total order */
	struct topn_ctx tcx;
	int presorted;
	if (topn) {
		topn_k = st->offset + st->limit;
		if (topn_k < 0 || topn_k > INT_MAX) topn = 0;   /* overflow guard */
		else if (topn_k == 0) topn_k = 1;         /* keep room for >=1 */
		topn_cap = topn_k > INT_MAX / 2 ? INT_MAX :
		    (int)(topn_k + (topn_k > 32 ? topn_k : 32));
	}

	/* An index walk that yields the ORDER BY needs no sort, and a LIMIT
	 * over it (or over no ORDER BY at all) stops once OFFSET + LIMIT rows
	 * passed the WHERE. */
	if (!st->src.open && src_open(st, st->table, &st->ap, &st->src) != 0) goto cleanup;
	presorted = (nkey == 0 || st->src.ordered);
	if (presorted) topn = 0;

	for (;;) {
		int64_t rowid; const uint8_t *rec; int reclen;
		int step;
		vx_cell_t outrow[32], keyrow[16];
		if (presorted && !st->distinct && st->limit >= 0 &&
		    (int64_t)nrow >= st->offset + st->limit)
			break;
		step = st_src_next(st, &rowid, &rec, &reclen);
		if (step == 0) break;
		if (step != 1) goto cleanup;

		arena_reset(&rowarena);   /* a row the WHERE dropped kept its bytes */
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

		nseen++;
		/* Top-N: being the latest arrival, this row sorts ahead of the
		 * threshold only if it is strictly smaller on the keys. */
		if (topn && topn_thr >= 0 &&
		    topn_cmp(keyrow, &keys[(size_t)topn_thr * (size_t)nkey], nkey,
		    st->order_desc) >= 0) {
			arena_reset(&rowarena);
			continue;
		}
		if (topn && nrow == topn_cap) {
			struct vx_arena_blk *fresh = NULL;
			size_t nro = (size_t)topn_k * (size_t)st->nout, nke = (size_t)topn_k * (size_t)nkey, q;
			tcx.keys = keys; tcx.seq = seq; tcx.nkey = nkey; tcx.desc = st->order_desc;
			topn_thr = topn_cut(&tcx, rows, st->nout, keys, seq, nrow,
			    (int)topn_k, topn_v, &topn_rng);
			nrow = (int)topn_k;
			/* Once the keep arena holds more than twice the survivors'
			 * bytes, move them to a fresh arena and drop the cut rows'
			 * bytes with the old one: live bytes stay O(K), and the
			 * copying is amortized O(1) per admitted byte. */
			if (arena_bytes(keep) > 2 * topn_bytes(rows, nro) +
			    2 * topn_bytes(keys, nke) + VX_ARENA_BLK) {
				for (q = 0; q < nro; q++)
					if (cell_dup(&fresh, &rows[q], &rows[q]) != 0) { arena_free(fresh); goto cleanup; }
				for (q = 0; q < nke; q++)
					if (cell_dup(&fresh, &keys[q], &keys[q]) != 0) { arena_free(fresh); goto cleanup; }
				arena_free(keep);
				keep = fresh;
			}
			if (topn_cmp(keyrow, &keys[(size_t)topn_thr * (size_t)nkey], nkey,
			    st->order_desc) >= 0) {
				arena_reset(&rowarena);
				continue;
			}
		}

		if (nrow == cap) {
			int nc = cap ? cap * 2 : 256;
			/* Under top-N never grow past the cut size. */
			if (topn && nc > topn_cap) nc = topn_cap;
			vx_cell_t *nr = realloc(rows, sizeof(vx_cell_t) * (size_t)nc * (size_t)(st->nout > 0 ? st->nout : 1));
			vx_cell_t *nk = realloc(keys, sizeof(vx_cell_t) * (size_t)nc * (size_t)(nkey > 0 ? nkey : 1));
			if (nr) rows = nr;
			if (nk) keys = nk;
			if (!nr || !nk) goto cleanup;
			if (topn) {
				int *nv;
				if ((ns = realloc(seq, sizeof *seq * (size_t)nc)) == NULL) goto cleanup;
				seq = ns;
				if ((nv = realloc(topn_v, sizeof *topn_v * (size_t)nc)) == NULL) goto cleanup;
				topn_v = nv;
			}
			cap = nc;
		}
		/* Copy output + key cells into the kept arena (row bytes are
		 * transient -- the next scan step reuses the source row). */
		for (j = 0; j < st->nout; j++)
			if (cell_dup(&keep, &outrow[j], &rows[(size_t)nrow * (size_t)st->nout + (size_t)j]) != 0) goto cleanup;
		for (j = 0; j < nkey; j++)
			if (cell_dup(&keep, &keyrow[j], &keys[(size_t)nrow * (size_t)nkey + (size_t)j]) != 0) goto cleanup;
		if (topn) seq[nrow] = nseen;
		nrow++;
		/* The first K rows bound the K-th best: admit only rows ahead of
		 * their worst, so input already in the requested order never
		 * fills the buffer to a cut. */
		if (topn && topn_thr < 0 && nrow == topn_k) {
			tcx.keys = keys; tcx.seq = seq; tcx.nkey = nkey; tcx.desc = st->order_desc;
			topn_thr = topn_worst(&tcx, nrow);
		}
		arena_reset(&rowarena);
	}
	if (topn && nrow > topn_k) {
		tcx.keys = keys; tcx.seq = seq; tcx.nkey = nkey; tcx.desc = st->order_desc;
		(void)topn_cut(&tcx, rows, st->nout, keys, seq, nrow, (int)topn_k,
		    topn_v, &topn_rng);
		nrow = (int)topn_k;
	}

	/* Sort the index array by keys (if any ORDER BY). */
	idx = (int *)malloc(sizeof(int) * (size_t)(nrow > 0 ? nrow : 1));
	if (!idx) goto cleanup;
	for (j = 0; j < nrow; j++) idx[j] = j;
	if (nkey > 0 && !presorted && topn) {
		struct topn_ctx tc;
		tc.keys = keys; tc.seq = seq; tc.nkey = nkey; tc.desc = st->order_desc;
		g_topn_ctx = &tc;
		atomic_fetch_add(&g_vx_sorts, 1);
		qsort(idx, (size_t)nrow, sizeof(int), topn_index_cmp);
		g_topn_ctx = NULL;
	} else if (nkey > 0 && !presorted) {
		struct sort_ctx sc; sc.keys = keys; sc.nkey = nkey; sc.desc = st->order_desc;
		g_sort_ctx = &sc;
		atomic_fetch_add(&g_vx_sorts, 1);
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
	free(rows); free(keys); free(seq); free(topn_v); free(idx);
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
	int j, nemit = st->join_emit > 0 ? st->join_emit : st->nout;
	vx_cell_t *dst;
	if (st->filter != NULL &&
	    eval_bool(st, st->filter, st->srcrow, &c->arena) != 1)
		return 0;
	dst = &c->cells[(size_t)c->nrow * (size_t)c->ncol];
	for (j = 0; j < nemit; j++) {
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
	c->ncol = st->join_emit > 0 ? st->join_emit : st->nout;
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
			vx_cell_t pk;
			arena_free(st->probe_arena); st->probe_arena = NULL;
			if (st->probe_cells == NULL)
				st->probe_cells = (vx_cell_t *)calloc((size_t)jp->probe_ncol,
				                                      sizeof(vx_cell_t));
			if (st->probe_cells == NULL) { chunk_free(c); return NULL; }
			{
				int64_t rid; const uint8_t *rec; int reclen;
				int step = xstore_scan_next(st->probe_scan, &rid, &rec, &reclen);
				if (step == 0) { st->probe_done = 1; continue; }
				if (step != 1) { chunk_free(c); return NULL; }
				decode_rec_cells(jp->probe_src.pay, jp->probe_ncol, rid, rec, reclen,
				                 st->probe_cells, &st->probe_arena);
			}
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

/* ---- N-way INNER join executor ----------------------------------- */

/* Build the hash table for one build side from its source SELECT. */
static int
njoin_build_side(struct vx_stmt *st, int side)
{
	vx_njoinplan_t *jp = st->njoin;
	xstore_scan_t *scan = NULL;
	struct vx_arena_blk *tmp = NULL;
	vx_jht_t *h;
	vx_cell_t rowcells[16];
	int rc = -1;
	int klocal = jp->key_local[side];
	int nc = jp->ncol[side];
	vx_jsrc_t *js = &jp->src[side];
	(void)side;   /* GCC -Wextra miscounts an index-only parameter as unused */

	h = (vx_jht_t *)calloc(1, sizeof *h);
	if (h == NULL) return -1;
	h->nbucket = 1024;
	h->buckets = (vx_jrow_t **)calloc((size_t)h->nbucket, sizeof(vx_jrow_t *));
	if (h->buckets == NULL) { free(h); return -1; }
	st->njht[side] = h;

	if (!js->native) goto done;
	scan = xstore_scan_open_txn((struct xsql *)st->db, js->table, 0, 0, 0, 0, 0);
	if (scan == NULL) goto done;
	for (;;) {
		vx_cell_t sentinel;
		{
			int64_t rid; const uint8_t *rec; int reclen;
			int step = xstore_scan_next(scan, &rid, &rec, &reclen);
			if (step == 0) break;
			if (step != 1) goto done;
			decode_rec_cells(js->pay, nc, rid, rec, reclen, rowcells, &tmp);
		}
		if (jp->cross[side]) {
			/* Keyless side: store every row under one sentinel key so any
			 * probe (the same sentinel) yields the whole set. */
			memset(&sentinel, 0, sizeof sentinel);
			sentinel.type = VX_INT; sentinel.i = 0;
			if (jht_insert(h, &sentinel, rowcells, nc) != 0)
				goto done;
			continue;
		}
		/* INNER: a NULL key never matches, so it is never probed -- skip. */
		if (rowcells[klocal].type == VX_NULL)
			continue;
		if (jht_insert(h, &rowcells[klocal], rowcells, nc) != 0)
			goto done;
	}
	rc = 0;
done:
	arena_free(tmp);
	if (scan) xstore_scan_close(scan);
	return rc;
}

/* Place build side `side`'s current match (st->ncur[side]) into the
 * combined srcrow at that side's base offset. */
static void
njoin_place(struct vx_stmt *st, int side)
{
	vx_njoinplan_t *jp = st->njoin;
	int j;
	(void)side;   /* GCC -Wextra miscounts an index-only parameter as unused */
	for (j = 0; j < jp->ncol[side]; j++)
		st->srcrow[jp->base[side] + j] = st->ncur[side]->cells[j];
}

/*
 * Produce one chunk of the N-way INNER join.  Side 0 is streamed; each
 * build side keeps a current-match cursor (ncur[]) into its hash chain.
 * The cursor stack advances depth-first: the deepest side advances
 * first, backtracking to shallower sides (and ultimately a new stream
 * row) when a chain is exhausted.  A full combined row exists when
 * every build side has a live match.
 */
static vx_chunk_t *
njoin_next_chunk(struct vx_stmt *st, int *done)
{
	vx_njoinplan_t *jp = st->njoin;
	vx_chunk_t *c;
	

	*done = 0;
	c = (vx_chunk_t *)calloc(1, sizeof *c);
	if (!c) return NULL;
	c->ncol = st->join_emit > 0 ? st->join_emit : st->nout;
	c->cap = VEXEC_VECTOR_SIZE;
	c->cells = (vx_cell_t *)malloc(sizeof(vx_cell_t) * (size_t)c->cap *
	                               (size_t)(c->ncol > 0 ? c->ncol : 1));
	if (!c->cells) { free(c); return NULL; }

	while (c->nrow < c->cap) {
		int side;
		/* If no build side has an open cursor, fetch the next stream row
		 * and seed side 1's probe. */
		if (st->ncur[1] == NULL) {
			vx_cell_t pv;
			arena_free(st->nstream_arena); st->nstream_arena = NULL;
			{
				int64_t rid; const uint8_t *rec; int reclen;
				int step = xstore_scan_next(st->nstream_scan, &rid, &rec, &reclen);
				if (step == 0) { *done = 1; break; }
				if (step != 1) { chunk_free(c); return NULL; }
				decode_rec_cells(jp->src[0].pay, jp->ncol[0], rid, rec, reclen,
				                 &st->srcrow[jp->base[0]], &st->nstream_arena);
			}
			if (jp->cross[1]) {
				memset(&pv, 0, sizeof pv); pv.type = VX_INT; pv.i = 0;
			} else {
				pv = st->srcrow[jp->probe_outidx[1]];
			}
			st->ncur[1] = (pv.type == VX_NULL) ? NULL
			                                   : jht_find(st->njht[1], &pv);
			if (st->ncur[1] == NULL) continue;   /* no match: next stream row */
			njoin_place(st, 1);
		}

		/* Descend: for each side beyond 1 with no open cursor, probe it
		 * using a value from an earlier (already-placed) side.  If any
		 * side has no match, backtrack. */
		side = 2;
		while (side < jp->nside) {
			if (st->ncur[side] == NULL) {
				vx_cell_t pv;
				if (jp->cross[side]) {
					memset(&pv, 0, sizeof pv); pv.type = VX_INT; pv.i = 0;
				} else {
					pv = st->srcrow[jp->probe_outidx[side]];
				}
				st->ncur[side] = (pv.type == VX_NULL) ? NULL
				                 : jht_find(st->njht[side], &pv);
				if (st->ncur[side] == NULL) {
					/* No match at this side: backtrack to the previous
					 * build side and advance its chain. */
					side--;
					while (side >= 1) {
						st->ncur[side] = st->ncur[side]->next;
						if (st->ncur[side] != NULL) { njoin_place(st, side); break; }
						side--;
					}
					if (side < 1) break;   /* exhausted: new stream row */
					side++;
					continue;
				}
				njoin_place(st, side);
			}
			side++;
		}
		if (side < jp->nside) continue;   /* backtracked past side 1: restream */
		if (st->ncur[1] == NULL) continue;

		/* All sides matched: emit the combined row, then advance the
		 * deepest cursor for the next iteration. */
		if (join_emit_row(st, c) < 0) { chunk_free(c); return NULL; }
		{
			int d = jp->nside - 1;
			while (d >= 1) {
				st->ncur[d] = st->ncur[d]->next;
				if (st->ncur[d] != NULL) { njoin_place(st, d); break; }
				d--;
			}
			/* d < 1 leaves ncur[1] == NULL -> next loop fetches a new
			 * stream row.  Otherwise re-probe the sides below d next
			 * iteration (they were left non-NULL; clear them so the
			 * descend loop re-probes with the new upper-side values). */
			if (d >= 1) {
				int k;
				for (k = d + 1; k < jp->nside; k++) st->ncur[k] = NULL;
			}
		}
	}
	return c;
}

/*
 * One chunk of the index nested-loop join.  Level L walks side ord[L]
 * through its access path, whose bounds are evaluated against the
 * combined row as placed by levels 0 .. L-1 when the level is (re)opened.
 * A row at the deepest level completes a combined row, which then runs
 * the full predicate + projection (join_emit_row).
 */
static vx_chunk_t *
nl_next_chunk(struct vx_stmt *st, int *done)
{
	vx_nlplan_t *nl = st->nl;
	vx_chunk_t *c;

	*done = 0;
	c = (vx_chunk_t *)calloc(1, sizeof *c);
	if (!c) return NULL;
	c->ncol = st->join_emit > 0 ? st->join_emit : st->nout;
	c->cap = VEXEC_VECTOR_SIZE;
	c->cells = (vx_cell_t *)malloc(sizeof(vx_cell_t) * (size_t)c->cap *
	                               (size_t)(c->ncol > 0 ? c->ncol : 1));
	if (!c->cells) { free(c); return NULL; }
	if (!st->nl_started) {
		st->nl_started = 1;
		st->nl_level = 0;
		if (src_open(st, nl->table[nl->ord[0]], &nl->ap[nl->ord[0]],
		    &st->nlsrc[0]) != 0) { chunk_free(c); return NULL; }
	}
	while (c->nrow < c->cap) {
		int lv = st->nl_level, sd = nl->ord[lv], r;
		int64_t rid; const uint8_t *rec; int reclen;
		if (lv < 0) { *done = 1; break; }
		r = src_next(&st->nlsrc[lv], &rid, &rec, &reclen);
		if (r < 0) { chunk_free(c); return NULL; }
		if (r == 0) {                       /* level exhausted: back up */
			src_close(&st->nlsrc[lv]);
			st->nl_level--;
			continue;
		}
		arena_free(st->nlar[lv]); st->nlar[lv] = NULL;
		decode_rec_cells(nl->pay[sd], nl->ncol[sd], rid, rec, reclen,
		    &st->srcrow[nl->base[sd]], &st->nlar[lv]);
		if (lv + 1 < nl->nside) {           /* descend: seek the next side */
			int nx = nl->ord[lv + 1];
			if (src_open(st, nl->table[nx], &nl->ap[nx], &st->nlsrc[lv + 1]) != 0) {
				chunk_free(c); return NULL;
			}
			st->nl_level = lv + 1;
			continue;
		}
		if (join_emit_row(st, c) < 0) { chunk_free(c); return NULL; }
	}
	return c;
}

/* Materialize, sort, and offset/limit a JOIN with ORDER BY into one
 * chunk.  Drains the join's streaming chunks (join_next_chunk /
 * njoin_next_chunk), copies each output row into a growable array, sorts
 * by the ORDER BY keys (which reference OUTPUT columns via
 * order_outcol), then applies OFFSET/LIMIT -- the same post-collection
 * model the aggregation path uses.  Deterministic for a total order. */
static int
join_ordered_materialize(struct vx_stmt *st)
{
	struct vx_arena_blk *keep = NULL;   /* output bytes (kept) */
	vx_cell_t *rows = NULL;             /* cap * ncollect */
	int *idx = NULL;
	int nkey = st->norder;
	int ncollect = st->join_emit > 0 ? st->join_emit : st->nout;
	int cap = 0, nrow = 0, j, rc = -1;
	vx_chunk_t *c = NULL;
	int64_t lim, off;
	int emit_n, e, done = 0;

	/* Drain every output chunk into rows[] (ncollect cols each: the nout
	 * user columns followed by any sort-only extras).  The producer is
	 * the two-table join, the N-way join, or -- for an ordered
	 * derived/view query -- the standard chunk path. */
	while (!done) {
		vx_chunk_t *jc = st->nl ? nl_next_chunk(st, &done)
		              : st->join ? join_next_chunk(st, &done)
		              : st->njoin ? njoin_next_chunk(st, &done)
		              : next_chunk(st, &done);
		int r;
		if (jc == NULL) { if (done) break; goto cleanup; }
		for (r = 0; r < jc->nrow; r++) {
			if (nrow == cap) {
				int nc = cap ? cap * 2 : 256;
				vx_cell_t *nr = realloc(rows, sizeof(vx_cell_t) *
				    (size_t)nc * (size_t)(ncollect > 0 ? ncollect : 1));
				if (!nr) { chunk_free(jc); goto cleanup; }
				rows = nr; cap = nc;
			}
			for (j = 0; j < ncollect; j++) {
				vx_cell_t v = jc->cells[(size_t)r * (size_t)jc->ncol + (size_t)j];
				if (cell_dup(&keep, &v, &rows[(size_t)nrow * (size_t)ncollect + (size_t)j]) != 0) {
					chunk_free(jc); goto cleanup;
				}
			}
			nrow++;
		}
		chunk_free(jc);
	}

	/* Sort an index array by the ORDER BY keys (output columns, which may
	 * be sort-only extras past nout). */
	idx = (int *)malloc(sizeof(int) * (size_t)(nrow > 0 ? nrow : 1));
	if (!idx) goto cleanup;
	for (j = 0; j < nrow; j++) idx[j] = j;
	if (nkey > 0) {
		vx_cell_t *keys = (vx_cell_t *)malloc(sizeof(vx_cell_t) *
		    (size_t)(nrow > 0 ? nrow : 1) * (size_t)nkey);
		struct sort_ctx sc;
		int p;
		if (!keys) goto cleanup;
		for (p = 0; p < nrow; p++)
			for (j = 0; j < nkey; j++)
				keys[(size_t)p * (size_t)nkey + (size_t)j] =
				    rows[(size_t)p * (size_t)ncollect + (size_t)(st->order_outcol[j] - 1)];
		sc.keys = keys; sc.nkey = nkey; sc.desc = st->order_desc;
		g_sort_ctx = &sc;
		atomic_fetch_add(&g_vx_sorts, 1);
		qsort(idx, (size_t)nrow, sizeof(int), sort_index_cmp);
		g_sort_ctx = NULL;
		free(keys);
	}

	/* OFFSET / LIMIT over the sorted rows. */
	off = st->offset; lim = st->limit;
	if (off > nrow) off = nrow;
	emit_n = nrow - (int)off;
	if (lim >= 0 && emit_n > lim) emit_n = (int)lim;
	if (emit_n < 0) emit_n = 0;

	c = (vx_chunk_t *)calloc(1, sizeof *c);
	if (!c) goto cleanup;
	c->ncol = st->nout;          /* emit only the user columns */
	c->cap = emit_n > 0 ? emit_n : 1;
	c->cells = (vx_cell_t *)malloc(sizeof(vx_cell_t) * (size_t)c->cap *
	    (size_t)(st->nout > 0 ? st->nout : 1));
	if (!c->cells) { chunk_free(c); c = NULL; goto cleanup; }
	for (e = 0; e < emit_n; e++) {
		int si = idx[(int)off + e];
		vx_cell_t *dst = &c->cells[(size_t)c->nrow * (size_t)c->ncol];
		for (j = 0; j < st->nout; j++) {
			vx_cell_t v = rows[(size_t)si * (size_t)ncollect + (size_t)j];
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
	arena_free(keep);
	free(rows); free(idx);
	if (c) chunk_free(c);
	return rc;
}

/* Open a join for streaming (idempotent): a hash join builds its table
 * and opens the probe cursor, an N-way join builds every build side and
 * opens the stream; an index nested loop needs neither.  0 or -1. */
static int
vx_join_open(struct vx_stmt *st)
{
	if (st->join != NULL && st->nl == NULL && !st->join_built) {
		st->jht = (vx_jht_t *)calloc(1, sizeof *st->jht);
		if (st->jht == NULL) return -1;
		if (join_build(st, st->jht) != 0) return -1;
		if (!st->join->probe_src.native) return -1;
		st->probe_scan = xstore_scan_open_txn((struct xsql *)st->db,
		                  st->join->probe_src.table, 0, 0, 0, 0, 0);
		if (st->probe_scan == NULL) return -1;
		st->join_built = 1;
	}
	if (st->njoin != NULL && st->nl == NULL && !st->nbuilt) {
		int side;
		for (side = 1; side < st->njoin->nside; side++)
			if (njoin_build_side(st, side) != 0) return -1;
		if (!st->njoin->src[0].native) return -1;
		st->nstream_scan = xstore_scan_open_txn((struct xsql *)st->db,
		                   st->njoin->src[0].table, 0, 0, 0, 0, 0);
		if (st->nstream_scan == NULL) return -1;
		st->nbuilt = 1;
	}
	return 0;
}

int
vx_step(vx_stmt_t *st)
{
	if (st == NULL) return SX_MISUSE;

	if (vx_join_open(st) != 0) return SX_ERROR;

	/* Aggregation: build all groups on first step, then walk the chunk. */
	if (st->agg != NULL && !st->ht_built) {
		if (agg_materialize(st) != 0) return SX_ERROR;
	}
	/* Ordered/limited (non-agg): materialize + sort on first step.  A
	 * join or a derived/view source uses the collecting collector (it
	 * drains the streaming producer); a single-table scan uses
	 * ordered_materialize. */
	if (is_ordered(st) && !st->ordered_built) {
		if (st->join != NULL || st->njoin != NULL || st->nl != NULL ||
		    st->derived_res != NULL) {
			if (join_ordered_materialize(st) != 0) return SX_ERROR;
		} else if (ordered_materialize(st) != 0) return SX_ERROR;
	}

	if (st->chunk != NULL && st->cur + 1 < st->chunk->nrow) {
		st->cur++;
		return SX_ROW;
	}
	if (st->agg != NULL || is_ordered(st)) {
		/* All rows are in the one materialized chunk; once consumed, done. */
		if (st->chunk != NULL && st->cur + 1 >= st->chunk->nrow)
			return SX_DONE;
	}
	for (;;) {
		int done = 0;
		vx_chunk_t *c;
		if (st->done) return SX_DONE;
		c = st->nl ? nl_next_chunk(st, &done) :
		    st->join ? join_next_chunk(st, &done) :
		    st->njoin ? njoin_next_chunk(st, &done) : next_chunk(st, &done);
		if (c == NULL) return SX_ERROR;
		if (st->chunk) chunk_free(st->chunk);
		st->chunk = c;
		st->cur = -1;
		st->done = done;
		if (c->nrow > 0) { st->cur = 0; return SX_ROW; }
		if (done) return SX_DONE;
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

/*
 * Return an executed plan to its just-prepared state, keeping everything
 * compiled (expressions, access paths, join plans): cursors closed, hash
 * tables and result chunks freed, cursors and flags zeroed.  A plan with
 * a cached access path re-evaluates its bounds when its source reopens.
 */
static void
vx_exec_reset(vx_stmt_t *st)
{
	int lv;
	if (st->scan) { xstore_scan_close(st->scan); st->scan = NULL; }
	src_close(&st->src);
	for (lv = 0; lv < VX_JOIN_MAX; lv++) {
		src_close(&st->nlsrc[lv]);
		arena_free(st->nlar[lv]); st->nlar[lv] = NULL;
	}
	st->nl_level = 0; st->nl_started = 0;
	if (st->chunk) { chunk_free(st->chunk); st->chunk = NULL; }
	st->cur = -1; st->done = 0;
	htab_free(&st->ht);
	memset(&st->ht, 0, sizeof st->ht);
	st->ht_built = 0; st->ordered_built = 0;
	if (st->jht) {
		arena_free(st->jht->arena); free(st->jht->buckets); free(st->jht);
		st->jht = NULL;
	}
	if (st->probe_scan) { xstore_scan_close(st->probe_scan); st->probe_scan = NULL; }
	arena_free(st->probe_arena); st->probe_arena = NULL;
	st->match = NULL;
	st->join_built = 0; st->probe_done = 0; st->bscan_b = 0; st->bscan_r = NULL;
	for (lv = 1; lv < VX_JOIN_MAX; lv++) {
		if (st->njht[lv]) {
			arena_free(st->njht[lv]->arena);
			free(st->njht[lv]->buckets);
			free(st->njht[lv]);
			st->njht[lv] = NULL;
		}
		st->ncur[lv] = NULL;
	}
	if (st->nstream_scan) { xstore_scan_close(st->nstream_scan); st->nstream_scan = NULL; }
	arena_free(st->nstream_arena); st->nstream_arena = NULL;
	st->nbuilt = 0; st->nstream_done = 0;
	st->derived_cur = 0;
	if (st->agg_in != NULL) vx_exec_reset(st->agg_in);
}

/* Coerce a rebound literal the way cmp_affinity_gate coerced it. */
static void
vx_param_coerce(vx_expr_t *n, struct vx_arena_blk **arena)
{
	if (n->lit.type == VX_NULL) return;
	if (n->pcoerce == 1 && n->lit.type == VX_TEXT) coerce_numeric(&n->lit);
	else if (n->pcoerce == 2 && (n->lit.type == VX_INT || n->lit.type == VX_REAL))
		coerce_to_affinity(&n->lit, VX_AFF_TEXT, arena);
}

/*
 * Rebind a cached plan to new parameter values: every bound-? literal
 * re-reads its value (with its comparison coercion), so the compiled
 * plan and its access path are reused as is.  The compile-time checks
 * that looked at a bind depend only on its storage class, which the
 * plan cache's signature pins.
 */
static void
vx_plan_rebind(vx_stmt_t *st, const vx_cell_t *binds, int nbinds)
{
	vx_expr_t *n;
	arena_free(st->bind_arena); st->bind_arena = NULL;
	st->binds = binds; st->nbinds = nbinds;
	if (st->now_ms != 0) st->now_ms = vxf_now_ms();   /* one 'now' per run */
	for (n = st->params; n != NULL; n = n->pnext) {
		const vx_cell_t *v;
		memset(&n->lit, 0, sizeof n->lit);
		if (binds == NULL || n->pord > nbinds) { n->lit.type = VX_NULL; continue; }
		v = &binds[n->pord - 1];
		if (v->type == VX_TEXT || v->type == VX_BLOB) {
			uint8_t *p = (uint8_t *)arena_alloc(&st->bind_arena,
			    (size_t)v->nbytes + 1);
			if (p == NULL) { n->lit.type = VX_NULL; continue; }
			if (v->nbytes) memcpy(p, v->bytes, v->nbytes);
			p[v->nbytes] = '\0';
			n->lit.type = v->type; n->lit.bytes = p; n->lit.nbytes = v->nbytes;
		} else {
			n->lit = *v;
		}
		vx_param_coerce(n, &st->bind_arena);
	}
	/* An aggregate over a join: the join's WHERE / ON hold their own. */
	if (st->agg_in != NULL) vx_plan_rebind(st->agg_in, binds, nbinds);
}

void
vx_finalize(vx_stmt_t *st)
{
	if (st == NULL) return;
	arena_free(st->bind_arena);
	if (st->scan) xstore_scan_close(st->scan);
	src_close(&st->src);
	if (st->nl) {
		int lv;
		for (lv = 0; lv < VX_JOIN_MAX; lv++) {
			src_close(&st->nlsrc[lv]);
			arena_free(st->nlar[lv]);
		}
		free(st->nl);
	}
	if (st->derived_res) vx_result_free(st->derived_res);
	if (st->chunk) chunk_free(st->chunk);
	if (st->plan_arena) arena_free(st->plan_arena);
	/* corrsql[] texts are plan_arena-owned (freed above); nothing else. */
	htab_free(&st->ht);
	if (st->agg) { free(st->agg->acc); free(st->agg->grp); free(st->agg->proj); free(st->agg); }
	if (st->agg_in) vx_finalize(st->agg_in);
	if (st->join) {
		if (st->jht) { arena_free(st->jht->arena); free(st->jht->buckets); free(st->jht); }
		if (st->probe_scan) xstore_scan_close(st->probe_scan);
		arena_free(st->probe_arena);
		free(st->probe_cells);
		free(st->join);
	}
	if (st->njoin) {
		int side;
		for (side = 1; side < VX_JOIN_MAX; side++)
			if (st->njht[side]) {
				arena_free(st->njht[side]->arena);
				free(st->njht[side]->buckets);
				free(st->njht[side]);
			}
		if (st->nstream_scan) xstore_scan_close(st->nstream_scan);
		arena_free(st->nstream_arena);
		free(st->njoin);
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

/* A collected result: row-major cells + an arena owning TEXT/BLOB.
 * (struct vx_result is defined near the top for recursive execution.) */
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
	_Atomic int  next;            /* next morsel to hand out */
	int          nmorsel;         /* morsel m = rowids [bound[m], bound[m+1]) */
	int64_t     *bound;           /* nmorsel + 1 ascending rowid boundaries */
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
	w.now_ms = par->plan->now_ms;  /* one 'now' per statement, every worker */
	w.nsrc_col = par->plan->nsrc_col;
	memcpy(w.src_pay, par->plan->src_pay, sizeof w.src_pay);
	snprintf(w.table, sizeof w.table, "%s", par->plan->table);
	srcrow = (vx_cell_t *)calloc((size_t)(w.nsrc_col > 0 ? w.nsrc_col : 1),
	                             sizeof(vx_cell_t));
	if (srcrow == NULL) goto done;
	w.srcrow = srcrow;

	for (;;) {
		int m = atomic_fetch_add_explicit(&par->next, 1,
		                                  memory_order_relaxed);
		int64_t lo, mh;
		xstore_scan_t *ms;
		if (m >= par->nmorsel) break;
		lo = par->bound[m];
		mh = par->bound[m + 1];

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
				if (agg_step_row(&w, w.agg, &par->wht[widx], w.srcrow,
				    &rowarena) == NULL) {
					xstore_scan_close(ms); atomic_store(&par->error, 1); goto done;
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

/*
 * Morsel boundaries for a parallel scan of `tid`'s rowids [lo, hi).
 * Each xstore_scan_open re-descends the B-tree from the root to seek the
 * morsel's start, so morsels must be LARGE for that cost to amortize.
 * Aim for ~2 morsels per worker (enough to load-balance, few enough that
 * descents are negligible), and never split a span below 64K rowids per
 * morsel.  The cuts come from the B-tree's upper separators
 * (xstore_rowid_split_points: RethinkDB get_distribution, O(fanout) page
 * reads), which divide the table's row versions about evenly whatever
 * the rowid distribution; a uniform split of the rowid span (the 1.56
 * rule, which unbalances morsels on sparse or skewed rowids) is the
 * fallback when the tree is too small to yield enough separators.
 * Writes a malloc'd array of nmorsel + 1 boundaries to *out (bound[0] =
 * lo, bound[nmorsel] = hi) and returns nmorsel (0 for an empty span),
 * or -1 on allocation failure.
 */
static int
morsel_bounds(bt_t *bt, uint32_t tid, int64_t lo, int64_t hi, int n_workers,
              int64_t **out)
{
	int64_t span = hi - lo, target, *b;
	int nm, got = 0, i;

	*out = NULL;
	if (span <= 0) return 0;
	target = span / (int64_t)(n_workers * 2 > 0 ? n_workers * 2 : 1);
	if (target < 65536) target = 65536;   /* descents amortize over >=64K rows */
	nm = (int)((span + target - 1) / target);
	b = (int64_t *)malloc(sizeof *b * (size_t)(nm + 1));
	if (b == NULL) return -1;
	b[0] = lo;
	if (nm > 1)
		got = xstore_rowid_split_points(bt, tid, lo, hi, nm - 1, b + 1);
	if (got != nm - 1)                     /* too few separators: uniform */
		for (i = 1; i < nm; i++) b[i] = lo + (int64_t)i * target;
	b[nm] = hi;
	*out = b;
	return nm;
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
	    plan->ap.kind == VX_AP_SCAN &&   /* a seek is a serial point/range walk */
	    (plan->agg == NULL || (!plan->agg->has_gc && plan->ncorr == 0));
}

/* Run an already-recognized, already-checked-parallelizable plan on the
 * morsel-parallel storage scan.  Takes ownership of plan (finalizes it).
 * Returns 1 with *res set, or <0 on error. */
static int
run_parallel_plan(vx_stmt_t *plan, struct xsql *db, int n_workers,
                  vx_result_t **res, char **errmsg)
{
	struct vx_par par;
	struct vx_result *out = NULL;
	xtc_exec_t *exec = NULL;
	struct { struct vx_par *par; int idx; } *args = NULL;
	int i, rc = 0;
	int64_t lo = 0, hi = 0;
	bt_t *bt = xstore_bt_of(db);
	uint32_t tid = 0;

	(void)errmsg;
	if (n_workers < 1) n_workers = 1;
	memset(&par, 0, sizeof par);

	/* rowid scan range [lo, hi): the morsel cursor partitions this span
	 * across workers, each of which re-checks per-row visibility, so any
	 * span covering every live rowid is correct.  Use the native max
	 * rowid (counts tombstones -> a safe upper bound) + 1 for hi and the
	 * start of the table for lo -- no SQLite call.  An empty table gives
	 * max 0 -> hi 0 -> no rows. */
	if (bt != NULL && xstore_table_id(bt, plan->table, &tid)) {
		int64_t mx = xstore_max_rowid(bt, tid);
		lo = (mx > 0) ? 1 : 0;
		hi = (mx > 0) ? mx + 1 : 0;   /* empty -> no scan */
	}

	out = (struct vx_result *)calloc(1, sizeof *out);
	if (out == NULL) { rc = -1; goto cleanup; }
	out->ncol = plan->nout;
	out->nworkers = n_workers;
	memcpy(out->name, plan->outname, sizeof out->name);
	memcpy(out->aff, plan->outaff, sizeof out->aff);
	memcpy(out->noaff, plan->outnoaff, sizeof out->noaff);

	par.plan = plan;
	atomic_store(&par.next, 0);
	par.nmorsel = morsel_bounds(bt, tid, lo, hi, n_workers, &par.bound);
	if (par.nmorsel < 0) { rc = -1; goto cleanup; }
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
					int j;
					if (d == NULL) { rc = -1; goto cleanup; }
					for (j = 0; j < ap->nagg; j++)
						acc_merge(&d->accs[j], &g->accs[j], ap->acc[j].kind,
						          &m->arena);
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
				struct vx_arena_blk *keep = NULL;
				int e = agg_emit_group(plan, ap, g, row, &keep);
				if (e && result_push(out, row) != 0) {
					arena_free(keep); rc = -1; goto cleanup;
				}
				arena_free(keep);
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
	free(par.bound);
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
static int collect_plan(vx_stmt_t *plan, vx_result_t **res);

static int
collect_serial(vx_stmt_t *plan, vx_result_t **res)
{
	int rc = collect_plan(plan, res);
	vx_finalize(plan);
	return rc;
}

/* collect_serial without the finalize (a cached plan is kept). */
static int
collect_plan(vx_stmt_t *plan, vx_result_t **res)
{
	struct vx_result *out;
	int ncol, step, rc = 1, i, distinct = plan->distinct;
	vx_cell_t rowbuf[64];

	ncol = vx_column_count(plan);
	if (ncol > (int)(sizeof rowbuf / sizeof rowbuf[0])) return 0;
	out = (struct vx_result *)calloc(1, sizeof *out);
	if (out == NULL) return -1;
	out->ncol = ncol;
	out->nworkers = 1;
	/* Only the result's own columns: the whole 64 x 64 name table is
	 * 4 KiB, copied for every point read (readers check col < ncol). */
	memcpy(out->name, plan->outname, sizeof out->name[0] * (size_t)ncol);
	memcpy(out->aff, plan->outaff, sizeof out->aff[0] * (size_t)ncol);
	memcpy(out->noaff, plan->outnoaff, sizeof out->noaff[0] * (size_t)ncol);

	while ((step = vx_step(plan)) == SX_ROW) {
		/* The current chunk row as is: the cells vx_column_* would
		 * rebuild one call per cell (result_push copies the bytes). */
		const vx_cell_t *row = cur_cell(plan, 0);
		if (row == NULL) {
			for (i = 0; i < ncol; i++) rowbuf[i].type = VX_NULL;
			row = rowbuf;
		}
		if (result_push(out, row) != 0) { rc = -1; break; }
	}
	if (step != SX_DONE && rc == 1) rc = -1;
	if (rc == 1 && distinct && result_dedup(out) != 0) rc = -1;

	if (rc == 1) { *res = out; }
	else { arena_free(out->arena); free(out->cells); free(out); }
	return rc;
}

int
vx_run_parallel(struct xsql *db, const char *sql, int n_workers,
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
	char    decltype[64][32];   /* declared type per column (for affinity) */
	int     is_pk[64];
	int     hidden[64];    /* synthetic implicit-rowid column (storage 0) */
	int     n;
	int     pk_col;        /* declared index of the pk, or -1 */
	int     implicit_rowid;/* table has a hidden __rowid__ at index 0 */
};

static int
load_wschema(struct xsql *db, const char *table, struct wschema *ws)
{
	ws->n = 0; ws->pk_col = -1; ws->implicit_rowid = 0;

	/* Native catalog first (no VDBE / no sqlite_master). */
	{
		bt_t *bt = xstore_bt_of(db);
		xstore_col_t cols[64];
		int nc = bt ? xstore_table_schema(bt, table, cols, 64) : 0, c;
		if (nc > 0) {
			for (c = 0; c < nc && ws->n < 64; c++) {
				snprintf(ws->name[ws->n], sizeof ws->name[0], "%.*s",
				         (int)(sizeof ws->name[0] - 1), cols[c].name);
				snprintf(ws->decltype[ws->n], sizeof ws->decltype[0], "%.*s",
				         (int)(sizeof ws->decltype[0] - 1), cols[c].decltype);
				ws->is_pk[ws->n] = cols[c].is_pk;
				ws->hidden[ws->n] = cols[c].hidden;
				if (cols[c].hidden) ws->implicit_rowid = 1;
				if (cols[c].is_pk) ws->pk_col = ws->n;
				ws->n++;
			}
			return ws->n > 0 ? 0 : -1;
		}
	}

	/* No native catalog entry: not an xstore-backed table.  The native
	 * write path only reaches here for a table xstore knows (the caller
	 * resolves xstore_table_id first), so a miss means fall back to the
	 * VDBE rather than read the schema via PRAGMA table_info. */
	return -1;
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
		if (ord < 1) return -1;
		if (binds == NULL || ord > nbinds) {
			out->type = VX_NULL;   /* an unset ? is NULL (SQLite) */
			return 0;
		}
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
	case SX_E_BLOB: {
		/* x'..hex..' literal: lit holds the full token text including the
		 * x' prefix and ' suffix.  Decode the hex body into raw bytes. */
		const char *t = e->lit.p; uint32_t tn = e->lit.len, i, nb;
		uint8_t *p;
		if (tn < 3 || (t[0] != 'x' && t[0] != 'X') || t[1] != '\'' ||
		    t[tn - 1] != '\'') return -1;
		tn -= 3; t += 2;                /* the hex body, tn chars */
		if (tn % 2 != 0) return -1;
		nb = tn / 2;
		p = (uint8_t *)arena_alloc(arena, nb ? nb : 1);
		if (p == NULL) return -1;
		for (i = 0; i < nb; i++) {
			int hi = t[i * 2], lo = t[i * 2 + 1];
			#define HEXV(ch) ((ch) >= '0' && (ch) <= '9' ? (ch) - '0' : \
			    ((ch) | 0x20) >= 'a' && ((ch) | 0x20) <= 'f' ? ((ch) | 0x20) - 'a' + 10 : -1)
			int h = HEXV(hi), l = HEXV(lo);
			#undef HEXV
			if (h < 0 || l < 0) return -1;
			p[i] = (uint8_t)((h << 4) | l);
		}
		out->type = VX_BLOB; out->bytes = p; out->nbytes = nb;
		return 0;
	}
	case SX_E_FUNC: {
		/* zeroblob(<literal>) is a constant (INSERT ... VALUES
		 * (zeroblob(N))); vxfunc caps N (vxf_arg_ok). */
		vx_cell_t arg;
		int id;
		if (e->nname != 1 || !name_is(&e->name[0], "zeroblob") ||
		    e->list == NULL || e->list->head == NULL ||
		    e->list->head->next != NULL ||
		    (id = vxf_lookup(e->name[0].p, e->name[0].len, 1)) <= 0 ||
		    lit_cell(arena, e->list->head->expr, &arg, binds, nbinds) != 0 ||
		    !vxf_arg_ok(id, 0, &arg, 0))
			return -1;
		vxf_eval(id, &arg, 1, 0, ext_alloc, arena, out);
		return out->type == VX_BLOB ? 0 : -1;
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
 * ([npay:u8] then one column per cell, xstore_rec_put) -- the SAME
 * encoder the vtab xUpdate path uses, so a native insert and a VDBE
 * insert are byte-identical.  Returns the record length or -1. */
static int
encode_payload(const vx_cell_t *cells, int npay, uint8_t *out, int cap)
{
	int off = 0, i, cls;
	if (npay < 0 || npay > 255 || cap < 1) return -1;
	out[off++] = (uint8_t)npay;
	for (i = 0; i < npay && off >= 0; i++) {
		const vx_cell_t *c = &cells[i];
		switch (c->type) {
		case VX_NULL: cls = XSTORE_C_NULL; break;
		case VX_INT:  cls = XSTORE_C_INT; break;
		case VX_REAL: cls = XSTORE_C_REAL; break;
		case VX_TEXT: cls = XSTORE_C_TEXT; break;
		default:      cls = XSTORE_C_BLOB; break;
		}
		off = xstore_rec_put(out, off, cap, cls, c->i, c->r, c->bytes,
		    (int)c->nbytes);
	}
	return off;
}

/* Native payload-record buffer cap.  Matches xstore's row payload limit
 * (XS_VMAX, 4096): the engine truncates a longer payload, so a record
 * never exceeds this, and a SET / VALUES literal that would not fit is
 * rejected by encode_payload (-> VDBE fallback) rather than truncated. */
#define XS_REC_MAX  4096
/* The error a write gets for a row over XS_REC_MAX (engine.c passes it
 * through instead of "unsupported SQL"). */
#define VX_MSG_TOOBIG "row too large: the native engine stores at most " \
	"4096 bytes per row (all columns, encoded)"

/* Upper bound on rows a single native DELETE / UPDATE will touch.  A
 * range matching more than this falls back to the VDBE rather than run
 * unbounded work (and an unbounded native buffer) on one statement. */
#define XS_NATIVE_MAX_ROWS  100000

/* Does `e` name the primary-key column (unqualified or table-qualified)? */
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

/* Collect rowids matching a WHERE predicate (NULL = every row): the
 * predicate is compiled with vexec's own expression compiler (affinity
 * gate on: SQLite semantics or rejected), the access-path chooser picks
 * a rowid range or index prefix from it, and every candidate row is
 * re-checked against the full predicate.  The rows come from the
 * read-your-writes view at the statement snapshot (xstore_stmt_begin).
 * Returns the count into `out` (<= cap), -1 if more than cap rows match
 * OR the predicate is not compilable (the statement is declined), or -2
 * on a scan/setup error. */
static int
collect_matching(struct xsql *db, bt_t *bt, const char *table,
                 const sql_expr_t *where, int64_t *out, int cap,
                 const vx_cell_t *binds, int nbinds)
{
	struct vx_stmt *st;
	struct vx_compiler comp;
	struct namevec nv;
	struct vx_arena_blk *rowarena = NULL;
	int64_t rid; const uint8_t *rec; int reclen, step;
	int n = 0, ret;

	st = (struct vx_stmt *)calloc(1, sizeof *st);
	if (st == NULL) return -2;
	st->db = db;
	st->bt = bt;
	st->snap = 0;
	st->binds = binds; st->nbinds = nbinds;
	snprintf(st->table, sizeof st->table, "%s", table);

	memset(&nv, 0, sizeof nv);
	comp.st = st; comp.nv = &nv; comp.jc = NULL; comp.fail = 0;
	comp.binds = binds; comp.nbinds = nbinds; comp.ag = NULL;

	if (where != NULL) {
		/* Pass 1: collect the predicate's columns. */
		if (collect_columns(&nv, where) != 0 || nv.n == 0 || nv.n > 32) {
			ret = -1; goto out;
		}
		st->nsrc_col = nv.n;
		if (resolve_schema(db, table, &nv, st->src_pay) != 0) { ret = -1; goto out; }
		/* Pass 2: compile the predicate with the affinity gate active. */
		comp.fail = 0;
		st->filter = compile_expr(&comp, where);
		if (comp.fail || st->filter == NULL || nv.n != st->nsrc_col) {
			ret = -1; goto out;
		}
		ap_choose(db, table, nv.names, nv.n, st->src_pay, st->filter,
		    NULL, 0, NULL, -1, &st->ap);
	}
	st->srcrow = (vx_cell_t *)calloc((size_t)(nv.n > 0 ? nv.n : 1), sizeof(vx_cell_t));
	if (st->srcrow == NULL) { ret = -2; goto out; }

	while ((step = st_src_next(st, &rid, &rec, &reclen)) == 1) {
		arena_free(rowarena); rowarena = NULL;   /* per-row scratch */
		if (st->filter != NULL) {
			read_rec_row(st, rid, rec, reclen, &rowarena);
			if (eval_bool(st, st->filter, st->srcrow, &rowarena) != 1)
				continue;
		}
		if (n >= cap) { ret = -1; goto out; }   /* too many: decline */
		out[n++] = rid;
	}
	ret = (step < 0) ? -2 : n;

out:
	src_close(&st->src);
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
read_one_row(struct xsql *db, bt_t *bt, const char *table, int64_t rowid, uint8_t *buf, int cap)
{
	xstore_scan_t *s;
	int64_t rid; const uint8_t *rec; int reclen, got = 0;
	(void)bt;
	s = xstore_scan_open_txn(db, table, 0, rowid, 1, rowid, 1);
	if (s == NULL) return -1;
	if (xstore_scan_next(s, &rid, &rec, &reclen) == 1 && rid == rowid) {
		if (reclen > cap) { xstore_scan_close(s); return -1; }
		memcpy(buf, rec, (size_t)reclen);
		got = reclen;
	}
	xstore_scan_close(s);
	return got;
}

/*
 * Evaluate an UPDATE SET right-hand-side expression against ONE row,
 * for the (common) case where the RHS is not a bare literal: a column
 * reference (the rowid, or a payload column), a numeric/text/NULL
 * literal, a unary minus, or a binary arithmetic op (+ - * /) over those
 * -- enough for SET v = v * 2, v = v + 1, etc.  `pay` holds the row's
 * payload cells (declared column i maps to pay[i-1]); `rowid` is the
 * INTEGER primary key.  ws gives column names.  Writes the result to
 * *out and returns 0; returns -1 for any shape this mini-evaluator does
 * not handle (the caller then declines the whole UPDATE).  Arithmetic
 * follows SQLite: integer op integer is integer (except / which is
 * integer division for two ints), any real operand yields real, a NULL
 * operand yields NULL.
 */
static int
eval_set_expr(const sql_expr_t *e, const vx_cell_t *pay, int npay,
              int64_t rowid, const struct wschema *ws, vx_cell_t *out)
{
	if (e == NULL) return -1;
	switch (e->op) {
	case SX_E_NULL:
		out->type = VX_NULL; out->nbytes = 0; return 0;
	case SX_E_NUMBER: {
		char buf[64];
		if (e->lit.len == 0 || e->lit.len >= sizeof buf) return -1;
		memcpy(buf, e->lit.p, e->lit.len); buf[e->lit.len] = '\0';
		if (strchr(buf, '.') || strchr(buf, 'e') || strchr(buf, 'E')) {
			out->type = VX_REAL; out->r = strtod(buf, NULL);
		} else { out->type = VX_INT; out->i = strtoll(buf, NULL, 10); }
		return 0;
	}
	case SX_E_STRING:
		/* The literal text is already unquoted in e->lit; it points into
		 * the SQL buffer (stable for this call). */
		out->type = VX_TEXT; out->bytes = (const uint8_t *)e->lit.p;
		out->nbytes = e->lit.len; return 0;
	case SX_E_COLUMN: {
		const sql_str_t *nm = &e->name[e->nname - 1];
		int k;
		if (nm->len == 5 && strncasecmp(nm->p, "rowid", 5) == 0) {
			out->type = VX_INT; out->i = rowid; return 0;
		}
		for (k = 0; k < ws->n; k++)
			if (nm->len == strlen(ws->name[k]) &&
			    strncasecmp(nm->p, ws->name[k], nm->len) == 0) {
				if (k == 0) { out->type = VX_INT; out->i = rowid; return 0; }
				if (k - 1 >= npay) return -1;
				*out = pay[k - 1]; return 0;
			}
		return -1;
	}
	case SX_E_UNARY:
		if (e->op2 == TK_MINUS) {
			vx_cell_t v;
			if (eval_set_expr(e->a, pay, npay, rowid, ws, &v) != 0) return -1;
			if (v.type == VX_INT) { out->type = VX_INT; out->i = -v.i; return 0; }
			if (v.type == VX_REAL) { out->type = VX_REAL; out->r = -v.r; return 0; }
			if (v.type == VX_NULL) { out->type = VX_NULL; out->nbytes = 0; return 0; }
		}
		return -1;
	case SX_E_BINARY: {
		vx_cell_t a, b;
		double x, y; int both_int;
		if (e->op2 != TK_PLUS && e->op2 != TK_MINUS &&
		    e->op2 != TK_STAR && e->op2 != TK_SLASH) return -1;
		if (eval_set_expr(e->a, pay, npay, rowid, ws, &a) != 0) return -1;
		if (eval_set_expr(e->b, pay, npay, rowid, ws, &b) != 0) return -1;
		if (a.type == VX_NULL || b.type == VX_NULL) {
			out->type = VX_NULL; out->nbytes = 0; return 0;
		}
		if (a.type != VX_INT && a.type != VX_REAL) return -1;
		if (b.type != VX_INT && b.type != VX_REAL) return -1;
		both_int = (a.type == VX_INT && b.type == VX_INT);
		x = (a.type == VX_INT) ? (double)a.i : a.r;
		y = (b.type == VX_INT) ? (double)b.i : b.r;
		if (e->op2 == TK_SLASH) {
			if (both_int) {
				if (b.i == 0) { out->type = VX_NULL; out->nbytes = 0; return 0; }
				out->type = VX_INT; out->i = a.i / b.i; return 0;
			}
			if (y == 0.0) { out->type = VX_NULL; out->nbytes = 0; return 0; }
			out->type = VX_REAL; out->r = x / y; return 0;
		}
		if (both_int) {
			out->type = VX_INT;
			out->i = (e->op2 == TK_PLUS) ? a.i + b.i :
			         (e->op2 == TK_MINUS) ? a.i - b.i : a.i * b.i;
			return 0;
		}
		out->type = VX_REAL;
		out->r = (e->op2 == TK_PLUS) ? x + y :
		         (e->op2 == TK_MINUS) ? x - y : x * y;
		return 0;
	}
	default: return -1;
	}
}

int
vx_run_write(struct xsql *db, const char *sql, int64_t *nchanges, char **errmsg)
{
	return vx_run_write_p(db, sql, NULL, 0, nchanges, errmsg);
}

/* The canonical duplicate-key error ("UNIQUE constraint failed: t.id"). */
static void
vx_unique_err(char **errmsg, const char *table, const char *pk)
{
	char m[160];
	if (errmsg == NULL) return;
	free(*errmsg);
	snprintf(m, sizeof m, "UNIQUE constraint failed: %s.%s", table, pk);
	*errmsg = strdup(m);
}

/* A refused INSERT: a UNIQUE index violation reports its own columns
 * (xstore_uniq_err), a rowid collision the rowid column. */
static void
vx_write_uniq_err(struct xsql *db, char **errmsg, const char *table,
    const char *pk)
{
	const char *u = xstore_uniq_err(db);
	if (u[0] != '\0') {
		if (errmsg == NULL) return;
		free(*errmsg);
		*errmsg = strdup(u);
		return;
	}
	vx_unique_err(errmsg, table, pk);
}

/* Apply one INSERTed row.  REPLACE overwrites; a plain INSERT must not
 * find the key, checked atomically with the write (xstore_insert_txn),
 * so two concurrent INSERTs of one key cannot both succeed.  Returns 0,
 * or -1 with *errmsg set. */
static int
vx_insert_row(struct xsql *db, uint32_t tableid, int64_t rowid,
    const uint8_t *rec, int reclen, int replace, const char *table,
    const char *pk, char **errmsg)
{
	int rc;
	if (replace)
		rc = xstore_write_txn(db, tableid, rowid, rec, reclen, 0);
	else
		rc = xstore_insert_txn(db, tableid, rowid, rec, reclen);
	if (rc == 0) return 0;
	if (rc == 1)
		vx_write_uniq_err(db, errmsg, table, pk);
	else if (errmsg != NULL && *errmsg == NULL)
		*errmsg = strdup("native insert failed");
	return -1;
}

/* Report a refused UPDATE / DELETE row write: a UNIQUE violation keeps
 * its message, anything else gets `what`. */
static void
vx_write_err(struct xsql *db, int rc, char **errmsg, const char *what)
{
	if (errmsg == NULL) return;
	free(*errmsg);
	*errmsg = NULL;
	if (rc == 1 && xstore_uniq_err(db)[0] != '\0')
		*errmsg = strdup(xstore_uniq_err(db));
	else
		*errmsg = strdup(what);
}

static int vx_run_write_ast(struct xsql *db, sql_arena_t *arena,
    const sql_stmt_t *root, const vx_cell_t *binds, int nbinds,
    int64_t *nchanges, char **errmsg);

int
vx_run_write_p(struct xsql *db, const char *sql,
               const vx_cell_t *binds, int nbinds,
               int64_t *nchanges, char **errmsg)
{
	sql_arena_t *arena = NULL;
	sql_stmt_t *root = NULL;
	const char *perr = NULL;

	if (nchanges) *nchanges = 0;
	if (errmsg) *errmsg = NULL;
	if (sql_parse_ast(sql, strlen(sql), &arena, &root, &perr) != 0)
		return 0;                         /* unparseable -> VDBE */
	return vx_run_write_ast(db, arena, root, binds, nbinds, nchanges, errmsg);
}

/* The write path over a parsed statement.  Takes ownership of `arena`
 * (destroyed before returning) when it is non-NULL; a cached statement
 * passes NULL and keeps its AST. */
static int
vx_run_write_ast(struct xsql *db, sql_arena_t *arena, const sql_stmt_t *root,
                 const vx_cell_t *binds, int nbinds,
                 int64_t *nchanges, char **errmsg)
{
	sql_insert_t *ins = NULL;
	const sql_str_t *tname = NULL;
	char tabbuf[64];
	bt_t *bt;
	uint32_t tableid;
	struct wschema ws;
	struct vx_arena_blk *cell_arena = NULL, *rowarena = NULL;
	struct vx_stmt *sst = NULL;     /* UPDATE: SET-expression plan */
	int rc = 0, r, applied = 0;
	int64_t maxr;

	if (nchanges) *nchanges = 0;
	if (errmsg) *errmsg = NULL;

	/* Exactly one write statement (INSERT / DELETE / UPDATE). */
	if (root == NULL || root->next != NULL) { sql_arena_destroy(arena); return 0; }
	switch (root->kind) {
	case SQL_KIND_INSERT:
		ins = root->u.insert;
		if (ins == NULL) { sql_arena_destroy(arena); return 0; }
		/* DEFAULT VALUES, a VALUES list, or a SELECT source -- exactly
		 * one.  DEFAULT VALUES inserts a single all-default row; an
		 * xstore vtab ignores any DEFAULT clause (its column args carry
		 * none it honors), so every non-PK column is NULL and the PK is
		 * auto-assigned -- the same row the VDBE produces. */
		if (!ins->def_values && ins->select == NULL &&
		    (ins->rows == NULL || ins->n_rows < 1)) {
			sql_arena_destroy(arena); return 0; }
		if (ins->def_values && (ins->select != NULL || ins->cols != NULL ||
		    (ins->rows != NULL && ins->n_rows > 0))) {
			sql_arena_destroy(arena); return 0; }   /* malformed -> VDBE */
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
	 * change.  The native collect path uses a read-your-writes range
	 * scan (xstore_scan_open_txn) that overlays this transaction's
	 * buffered writes (UPDATE replaces, DELETE hides, INSERT appended),
	 * so a transactional DELETE/UPDATE stays native even after the txn
	 * has buffered writes to the same table.  INSERT needs no such
	 * read and is always native. */

	/* ---- DELETE [WHERE pk-range or general predicate] ---------------- */
	if (root->kind == SQL_KIND_DELETE) {
		int64_t *rids;
		int nr, i;
		rids = (int64_t *)malloc(sizeof(int64_t) * XS_NATIVE_MAX_ROWS);
		if (rids == NULL) { rc = -1; goto done; }
		nr = collect_matching(db, bt, tabbuf, root->u.del->where,
		                      rids, XS_NATIVE_MAX_ROWS, binds, nbinds);
		if (nr == -1) { free(rids); sql_arena_destroy(arena); return 0; }  /* too many / not compilable -> VDBE */
		if (nr < 0) { free(rids); rc = -1; goto done; }
		for (i = 0; i < nr; i++) {
			if (xstore_write_txn(db, tableid, rids[i], NULL, 0, 1) != 0) {
				free(rids);
				vx_write_err(db, -1, errmsg, "native delete failed");
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
		int nr, i, ci;
		/* Validate the SET list ONCE up front (the same literal columns
		 * apply to every matched row); record which payload column each
		 * SET targets and its new cell.  A SET on the primary key column
		 * (col 0) is a pk reassign, captured separately and applied as a
		 * tombstone-old + insert-new move. */
		vx_cell_t setval[64]; int setcol[64], nset = 0;
		const sql_expr_t *setast[64];   /* non-literal RHS, evaluated per row */
		vx_expr_t *setx[64];            /* setast compiled over the old row */
		struct namevec snv;
		struct vx_compiler scomp;
		int pk_reassign = 0; vx_cell_t pk_newval;
		int64_t *rids;
		memset(&pk_newval, 0, sizeof pk_newval);

		for (a = up->sets; a; a = a->next) {
			int col = -1, k;
			for (k = 0; k < ws.n; k++)
				if (a->col.len == strlen(ws.name[k]) &&
				    strncasecmp(a->col.p, ws.name[k], a->col.len) == 0) { col = k; break; }
			if (col < 0) { rc = 0; goto done; } /* unknown col -> VDBE */
			if (col == 0) {
				if (pk_reassign) { rc = 0; goto done; }  /* SET k twice */
				if (lit_cell(&cell_arena, a->val, &pk_newval, binds, nbinds) != 0 ||
				    pk_newval.type != VX_INT) {
					rc = 0; goto done;  /* non-int / non-literal pk -> VDBE */
				}
				pk_reassign = 1;
				continue;
			}
			if (nset >= 64) { rc = 0; goto done; }
			if (lit_cell(&cell_arena, a->val, &setval[nset], binds, nbinds) == 0) {
				setast[nset] = NULL;          /* literal: same value every row */
			} else {
				setast[nset] = a->val;        /* expression: eval per row */
			}
			setcol[nset] = col;   /* declared index (>=1) */
			nset++;
		}
		/* Compile every non-literal SET RHS once with the real
		 * expression compiler over the table's columns (binds, CASE,
		 * functions, %, || ...); evaluated per row against the OLD row.
		 * A RHS the compiler's affinity gate rejects (arithmetic on an
		 * untyped column) keeps the mini-evaluator below. */
		memset(&snv, 0, sizeof snv);
		sst = (struct vx_stmt *)calloc(1, sizeof *sst);
		if (sst == NULL) { rc = -1; goto done; }
		sst->db = db; sst->bt = bt; sst->binds = binds; sst->nbinds = nbinds;
		snprintf(sst->table, sizeof sst->table, "%s", tabbuf);
		scomp.st = sst; scomp.nv = &snv; scomp.jc = NULL; scomp.fail = 0;
		scomp.binds = binds; scomp.nbinds = nbinds; scomp.ag = NULL;
		for (ci = 0; ci < nset && !scomp.fail; ci++)
			if (setast[ci] != NULL && collect_columns(&snv, setast[ci]) != 0)
				scomp.fail = 1;
		if (!scomp.fail && snv.n > 0 &&
		    resolve_schema(db, tabbuf, &snv, sst->src_pay) != 0)
			scomp.fail = 1;
		sst->nsrc_col = snv.n;
		for (ci = 0; ci < nset && !scomp.fail; ci++) {
			setx[ci] = NULL;
			if (setast[ci] != NULL)
				setx[ci] = compile_expr(&scomp, setast[ci]);
		}
		if (snv.n != sst->nsrc_col)
			scomp.fail = 1;   /* compile added an unresolved column */
		if (!scomp.fail) {
			sst->srcrow = (vx_cell_t *)calloc((size_t)(snv.n > 0 ? snv.n : 1),
			    sizeof(vx_cell_t));
			if (sst->srcrow == NULL) { rc = -1; goto done; }
		}
		rids = (int64_t *)malloc(sizeof(int64_t) * XS_NATIVE_MAX_ROWS);
		if (rids == NULL) { rc = -1; goto done; }
		nr = collect_matching(db, bt, tabbuf, up->where,
		                      rids, XS_NATIVE_MAX_ROWS, binds, nbinds);
		if (nr == -1) { free(rids); rc = 0; goto done; }  /* too many / not compilable -> VDBE */
		if (nr < 0) { free(rids); rc = -1; goto done; }

		/* A pk reassign sets the primary key to a single constant.  More
		 * than one matched row would all collide on that one new pk (a
		 * UNIQUE violation) -- defer to the VDBE.  For exactly one row,
		 * verify the new pk is free (no committed row there, unless it is
		 * the row being moved); a collision also falls back. */
		if (pk_reassign) {
			int64_t newpk = pk_newval.i;
			if (nr > 1) { free(rids); rc = 0; goto done; }
			if (nr == 1 && newpk != rids[0]) {
				uint8_t probe[XS_REC_MAX]; int pl;
				if (xstore_in_txn(db)) { free(rids); rc = 0; goto done; }  /* committed-scan check unsafe in txn */
				pl = read_one_row((struct xsql *)db, bt, tabbuf, newpk, probe, (int)sizeof probe);
				if (pl < 0) { free(rids); rc = -1; goto done; }
				if (pl > 0) { free(rids); rc = 0; goto done; }  /* collision -> VDBE */
			}
		}
		for (i = 0; i < nr; i++) {
			uint8_t cur[XS_REC_MAX]; int curlen;
			vx_cell_t cells[64];
			uint8_t rec[XS_REC_MAX]; int reclen;
			curlen = read_one_row((struct xsql *)db, bt, tabbuf, rids[i], cur, (int)sizeof cur);
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
			if (!scomp.fail)
				decode_rec_cells(sst->src_pay, snv.n, rids[i], cur, curlen,
				    sst->srcrow, &rowarena);
			for (ci = 0; ci < nset; ci++) {
				/* Evaluate against the OLD row (SQLite evaluates every
				 * SET RHS against the pre-update values), then overlay.
				 * A literal SET has setast == NULL (setval precomputed);
				 * an expression is evaluated per row. */
				vx_cell_t nvval;
				if (setast[ci] == NULL) {
					nvval = setval[ci];
				} else if (!scomp.fail) {
					eval(sst, setx[ci], sst->srcrow, &rowarena, &nvval);
				} else if (eval_set_expr(setast[ci], cells, ws.n - 1,
				                         rids[i], &ws, &nvval) != 0) {
					/* Unsupported RHS shape: clean fallback only if
					 * nothing applied yet. */
					if (applied == 0) { free(rids); rc = 0; goto done; }
					free(rids);
					if (errmsg) *errmsg = strdup("native update expr unsupported");
					rc = -1; goto done;
				}
				/* Apply the target column's declared affinity. */
				coerce_to_affinity(&nvval,
				    vx_affinity(ws.decltype[setcol[ci]]), &cell_arena);
				cells[setcol[ci] - 1] = nvval;
			}
			reclen = encode_payload(cells, ws.n - 1, rec, (int)sizeof rec);
			if (reclen < 0) {
				/* Row too large to encode.  If nothing applied yet, fall
				 * back cleanly to the VDBE; otherwise it is too late to
				 * fall back (the VDBE would re-update applied rows and
				 * misreport changes()), so report an error. */
				free(rids);
				vx_write_err(db, 0, errmsg, VX_MSG_TOOBIG);
				rc = applied == 0 ? 0 : -1; goto done;
			}
			if (pk_reassign && pk_newval.i != rids[i]) {
				/* Move: write the new payload at the new pk and tombstone
				 * the old key. */
				int wr = xstore_write_txn(db, tableid, rids[i], NULL, 0, 1);
				if (wr == 0)
					wr = xstore_write_txn(db, tableid, pk_newval.i, rec, reclen, 0);
				if (wr != 0) {
					free(rids);
					vx_write_err(db, wr, errmsg, "native pk-reassign failed");
					rc = -1; goto done;
				}
			} else {
				int wr = xstore_write_txn(db, tableid, rids[i], rec, reclen, 0);
				if (wr != 0) {
					free(rids);
					vx_write_err(db, wr, errmsg, "native update failed");
					rc = -1; goto done;
				}
			}
			arena_free(rowarena); rowarena = NULL;
			applied++;
		}
		free(rids);
		if (nchanges) *nchanges = applied;
		rc = 1; goto done;
	}

	/* ---- INSERT INTO t [(col,...)] VALUES (...) ... ----------------- */
	/* Auto-rowid base includes this txn's buffered rows (so an in-txn
	 * auto-PK INSERT does not reuse a buffered rowid); outside a txn this
	 * is the committed max. */
	maxr = xstore_max_rowid_txn(db, tableid);

	/* ---- INSERT INTO t DEFAULT VALUES -------------------------------
	 * One row: PK auto-assigned (max committed + 1), every non-PK column
	 * NULL.  An xstore table honors no DEFAULT clause, so this is exactly
	 * what the VDBE inserts.  Inside a txn with an explicit-PK collision
	 * risk there is none (the PK is auto), so this is always native --
	 * the same as a positional INSERT of a single NULL tuple. */
	if (ins->def_values) {
		vx_cell_t cells[64];
		uint8_t rec[XS_REC_MAX];
		int ci, reclen;
		int64_t rowid = xstore_next_rowid(db, tableid, maxr);
		for (ci = 0; ci < ws.n; ci++) { cells[ci].type = VX_NULL; cells[ci].nbytes = 0; }
		reclen = encode_payload(&cells[1], ws.n - 1, rec, (int)sizeof rec);
		if (reclen < 0) {   /* unexpected */
			vx_write_err(db, 0, errmsg, VX_MSG_TOOBIG);
			rc = 0; goto done;
		}
		if (vx_insert_row(db, tableid, rowid, rec, reclen, 0, tabbuf,
		    ws.name[0], errmsg) != 0) {
			rc = -1; goto done;
		}
		if (nchanges) *nchanges = 1;
		rc = 1; goto done;
	}

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
			/* Positional: element i -> declared column i.  For an
			 * implicit-rowid table the hidden __rowid__ is storage
			 * column 0 and is NOT supplied by the user, so the values
			 * map to columns 1..n-1 (rowid stays NULL -> auto). */
			if (ws.implicit_rowid) {
				ncolmap = ws.n - 1;
				for (i = 0; i < ncolmap && i < 64; i++) colmap[i] = i + 1;
			} else {
				ncolmap = ws.n;
				for (i = 0; i < ws.n && i < 64; i++) colmap[i] = i;
			}
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
					    strncasecmp(nm->p, ws.name[k], nm->len) == 0) { col = k; break; }
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
		vx_result_t *sres = NULL;
		const char *ssrc = ins->select->src;
		int scol, srow, sr, applied2 = 0;
		struct { int64_t rowid; uint8_t rec[XS_REC_MAX]; int reclen; } *buf2 = NULL;
		int nb2 = 0, cap2 = 0;

		/* Read-your-writes: a SELECT run as a nested query would not see
		 * this transaction's uncommitted buffered rows, so defer to the
		 * VDBE inside a txn. */
		if (xstore_in_txn(db)) { rc = 0; goto done; }
		if (ssrc == NULL) { rc = 0; goto done; }
		/* Run the source SELECT through vexec (no SQLite); if vexec cannot
		 * recognize it, fall the whole INSERT...SELECT back to the VDBE. */
		if (vx_run(db, ssrc, 1, &sres, NULL) != 1 || sres == NULL) {
			if (sres) vx_result_free(sres);
			rc = 0; goto done;
		}
		scol = vx_result_ncol(sres);
		if (scol != ncolmap) { vx_result_free(sres); rc = 0; goto done; }
		srow = vx_result_nrow(sres);

		for (sr = 0; sr < srow; sr++) {
			vx_cell_t cells[64];
			int ci, reclen;
			int64_t rowid;
			for (ci = 0; ci < ws.n; ci++) { cells[ci].type = VX_NULL; cells[ci].nbytes = 0; }
			for (ci = 0; ci < scol; ci++) {
				vx_cell_t *dst = &cells[colmap[ci]];
				switch (vx_result_type(sres, sr, ci)) {
				case VX_INT:  dst->type = VX_INT; dst->i = vx_result_int64(sres, sr, ci); break;
				case VX_REAL: dst->type = VX_REAL; dst->r = vx_result_double(sres, sr, ci); break;
				case VX_TEXT: case VX_BLOB: {
					const char *b = vx_result_text(sres, sr, ci);
					int n = vx_result_bytes(sres, sr, ci);
					uint8_t *pp = (uint8_t *)arena_alloc(&cell_arena, (size_t)n + 1);
					if (pp == NULL) { free(buf2); vx_result_free(sres); rc = -1; goto done; }
					if (n && b) memcpy(pp, b, (size_t)n);
					pp[n] = '\0';
					dst->type = (vx_result_type(sres, sr, ci) == VX_TEXT) ? VX_TEXT : VX_BLOB;
					dst->bytes = pp; dst->nbytes = (uint32_t)n;
					break; }
				default: dst->type = VX_NULL; break;
				}
				/* Store affinity, exactly as INSERT VALUES applies it. */
				coerce_to_affinity(dst, vx_affinity(ws.decltype[colmap[ci]]),
				    &cell_arena);
			}
			if (cells[0].type == VX_INT) {
				rowid = cells[0].i; if (rowid > maxr) maxr = rowid;
			} else if (cells[0].type == VX_NULL) {
				rowid = maxr = xstore_next_rowid(db, tableid, maxr);
			} else { free(buf2); vx_result_free(sres); rc = 0; goto done; }

			if (cells[0].type == VX_INT && !ins->replace) {
				uint8_t probe[XS_REC_MAX]; int pl, bi;
				pl = read_one_row((struct xsql *)db, bt, tabbuf, rowid, probe, (int)sizeof probe);
				if (pl < 0) { free(buf2); vx_result_free(sres); rc = -1; goto done; }
				for (bi = 0; pl == 0 && bi < nb2; bi++)
					if (buf2[bi].rowid == rowid) pl = 1;
				if (pl > 0) {              /* duplicate key: nothing written yet */
					free(buf2); vx_result_free(sres);
					vx_unique_err(errmsg, tabbuf, ws.name[0]);
					rc = -1; goto done;
				}
			}

			if (nb2 == cap2) {
				int nc = cap2 ? cap2 * 2 : 64;
				void *nn = realloc(buf2, sizeof *buf2 * (size_t)nc);
				if (nn == NULL) { free(buf2); vx_result_free(sres); rc = -1; goto done; }
				buf2 = nn; cap2 = nc;
			}
			if (nb2 >= XS_NATIVE_MAX_ROWS) { free(buf2); vx_result_free(sres); rc = 0; goto done; }
			reclen = encode_payload(&cells[1], ws.n - 1, buf2[nb2].rec, (int)sizeof buf2[nb2].rec);
			if (reclen < 0) {
				free(buf2); vx_result_free(sres);
				vx_write_err(db, 0, errmsg, VX_MSG_TOOBIG);
				rc = 0; goto done;
			}
			buf2[nb2].rowid = rowid; buf2[nb2].reclen = reclen; nb2++;
		}
		vx_result_free(sres);

		for (r = 0; r < nb2; r++) {
			if (vx_insert_row(db, tableid, buf2[r].rowid, buf2[r].rec,
			    buf2[r].reclen, ins->replace, tabbuf, ws.name[0],
			    errmsg) != 0) {
				free(buf2);
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
				/* Apply the target column's declared affinity (SQLite stores
				 * 42 into a TEXT column as '42', a numeric-looking string
				 * into a NUMERIC column as a number, etc.). */
				coerce_to_affinity(&cells[colmap[vidx]],
				    vx_affinity(ws.decltype[colmap[vidx]]), &cell_arena);
				vidx++;
			}
			if (vidx != ncolmap) { free(buf); rc = 0; goto done; }  /* arity / non-lit */

			if (cells[0].type == VX_INT) {
				rowid = cells[0].i;
				if (rowid > maxr) maxr = rowid;
			} else if (cells[0].type == VX_NULL) {
				rowid = maxr = xstore_next_rowid(db, tableid, maxr);
			} else { free(buf); rc = 0; goto done; }   /* non-int PK -> VDBE */

			/* PRIMARY KEY uniqueness: a plain INSERT onto an existing
			 * rowid is a UNIQUE constraint violation (REPLACE overwrites).
			 * A key that already exists (committed, or buffered in this
			 * txn) or repeats within THIS statement fails the statement
			 * before anything is written.  This early check gives the
			 * error cheaply; the apply below re-checks atomically with the
			 * write, which is what stops a concurrent INSERT of the same
			 * key from winning too. */
			if (cells[0].type == VX_INT && !ins->replace) {
				uint8_t probe[XS_REC_MAX]; int pl, bi;
				if (xstore_in_txn(db))
					pl = xstore_row_exists_txn(db, tableid, rowid);
				else
					pl = read_one_row((struct xsql *)db, bt, tabbuf, rowid,
					    probe, (int)sizeof probe);
				if (pl < 0) { free(buf); rc = -1; goto done; }
				for (bi = 0; pl == 0 && bi < nb; bi++)
					if (buf[bi].rowid == rowid) pl = 1;
				if (pl > 0) {
					free(buf);
					vx_unique_err(errmsg, tabbuf, ws.name[0]);
					rc = -1; goto done;
				}
			}

			reclen = encode_payload(&cells[1], ws.n - 1, buf[nb].rec,
			                        (int)sizeof buf[nb].rec);
			if (reclen < 0) {   /* too big: VDBE, or the error */
				free(buf);
				vx_write_err(db, 0, errmsg, VX_MSG_TOOBIG);
				rc = 0; goto done;
			}
			buf[nb].rowid = rowid;
			buf[nb].reclen = reclen;
			nb++;
		}

		/* All rows validated: apply.  In a transaction each row is
		 * buffered (atomic at COMMIT); in autocommit each row commits on
		 * its own, and a key taken concurrently since the check above
		 * fails the statement with a UNIQUE error (rows before it stay). */
		for (r = 0; r < nb; r++) {
			if (vx_insert_row(db, tableid, buf[r].rowid, buf[r].rec,
			    buf[r].reclen, ins->replace, tabbuf, ws.name[0],
			    errmsg) != 0) {
				free(buf);
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
	arena_free(rowarena);
	if (sst != NULL) {
		arena_free(sst->plan_arena);
		free(sst->srcrow);
		free(sst);
	}
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
/* The compound's ORDER BY must name result columns, as in SQLite: a
 * 1-based position, or a bare name matching a column/alias of the FIRST
 * select (any table qualifier is ignored, as SQLite does there).  Fills
 * key[] (0-based output column) and flags[] (VX_ORD_*), and LIMIT /
 * OFFSET from integer literals or binds.  Returns 0, or -1 with *errmsg
 * set (SQLite's error text) or left NULL (decline). */
static int
setop_order_plan(const sql_select_t *first, const sql_exprlist_t *order,
                 const sql_expr_t *limit, const sql_expr_t *offset,
                 const vx_cell_t *binds, int nbinds, int *key, int *flags,
                 int *nkey, int64_t *lim, int64_t *off, char **errmsg)
{
	const sql_exprlist_item_t *o, *c;
	int ncol = 0, k = 0, pos, term = 0, ci;
	int64_t v;
	char name[64], buf[96];
	for (c = first->cols ? first->cols->head : NULL; c; c = c->next) {
		if (c->expr == NULL || c->expr->op == SX_E_STAR) return -1;   /* decline */
		ncol++;
	}
	for (o = order ? order->head : NULL; o; o = o->next) {
		const sql_expr_t *e = o->expr;
		term++;
		if (k >= 16) return -1;
		pos = 0;
		if (e != NULL && e->op == SX_E_NUMBER && lit_int(e, &v, NULL, 0)) {
			if (v >= 1 && v <= ncol) pos = (int)v;
		} else if (e != NULL && e->op == SX_E_COLUMN && e->nname >= 1) {
			const sql_str_t *nm = &e->name[e->nname - 1];
			ci = 0;
			for (c = first->cols->head; c; c = c->next) {
				ci++;
				item_name(c, name, sizeof name);
				if (strlen(name) == nm->len &&
				    strncasecmp(name, nm->p, nm->len) == 0) { pos = ci; break; }
			}
		}
		if (pos == 0) {
			snprintf(buf, sizeof buf, "%d%s ORDER BY term does not match any "
			    "column in the result set", term,
			    term == 1 ? "st" : term == 2 ? "nd" : term == 3 ? "rd" : "th");
			if (errmsg) *errmsg = strdup(buf);
			return -1;
		}
		key[k] = pos - 1;
		flags[k] = ord_flags(o);
		k++;
	}
	*nkey = k;
	*lim = -1; *off = 0;
	if (limit != NULL) {
		if (!lit_int(limit, lim, binds, nbinds)) return -1;
		if (*lim < 0) *lim = -1;   /* a negative LIMIT is no limit (SQLite) */
	}
	if (offset != NULL) {
		if (!lit_int(offset, off, binds, nbinds)) return -1;
		if (*off < 0) *off = 0;
	}
	return 0;
}

/* Sort a materialized compound result by output-column keys (stable,
 * VX_ORD_* flags) and keep rows [off, off+lim).  0, or -1 on OOM. */
static int
setop_sort_slice(struct vx_result *r, const int *key, const int *flags,
                 int nkey, int64_t lim, int64_t off)
{
	vx_cell_t *keys = NULL, *cells = NULL;
	int *idx = NULL, i, j, n = r->nrow, nc = r->ncol, from, to;
	struct sort_ctx sc;
	if (nkey > 0 && n > 1) {
		keys = (vx_cell_t *)malloc(sizeof *keys * (size_t)n * (size_t)nkey);
		idx = (int *)malloc(sizeof *idx * (size_t)n);
		cells = (vx_cell_t *)malloc(sizeof *cells * (size_t)n * (size_t)nc);
		if (keys == NULL || idx == NULL || cells == NULL) {
			free(keys); free(idx); free(cells); return -1;
		}
		for (i = 0; i < n; i++) {
			idx[i] = i;
			for (j = 0; j < nkey; j++)
				keys[(size_t)i * (size_t)nkey + (size_t)j] =
				    r->cells[(size_t)i * (size_t)nc + (size_t)key[j]];
		}
		sc.keys = keys; sc.nkey = nkey; sc.desc = flags;
		g_sort_ctx = &sc;
		qsort(idx, (size_t)n, sizeof *idx, sort_index_cmp);
		g_sort_ctx = NULL;
		for (i = 0; i < n; i++)
			memcpy(&cells[(size_t)i * (size_t)nc],
			    &r->cells[(size_t)idx[i] * (size_t)nc], sizeof *cells * (size_t)nc);
		memcpy(r->cells, cells, sizeof *cells * (size_t)n * (size_t)nc);
		free(keys); free(idx); free(cells);
	}
	from = off > n ? n : (int)off;
	to = (lim < 0 || lim > (int64_t)(n - from)) ? n : from + (int)lim;
	if (from > 0 && to > from)
		memmove(r->cells, &r->cells[(size_t)from * (size_t)nc],
		    sizeof *r->cells * (size_t)(to - from) * (size_t)nc);
	r->nrow = to - from;
	return 0;
}

static int run_setop_sel(struct xsql *db, const sql_select_t *lhs,
    const vx_cell_t *binds, int nbinds, vx_result_t **res, char **errmsg);

static int
run_setop(struct xsql *db, const char *sql, const vx_cell_t *binds, int nbinds,
          vx_result_t **res, char **errmsg)
{
	sql_arena_t *ast = NULL;
	sql_stmt_t *root = NULL;
	const char *perr = NULL;
	int rc;

	if (errmsg) *errmsg = NULL;
	if (sql_parse_ast(sql, strlen(sql), &ast, &root, &perr) != 0) return 0;
	if (root == NULL || root->next != NULL ||
	    root->kind != SQL_KIND_SELECT || root->u.select == NULL) {
		sql_arena_destroy(ast); return 0;
	}
	rc = run_setop_sel(db, root->u.select, binds, nbinds, res, errmsg);
	sql_arena_destroy(ast);
	return rc;
}

/* A compound SELECT from its AST (the caller owns it; it is not
 * modified).  1 with *res, 0 declined, <0 error. */
static int
run_setop_sel(struct xsql *db, const sql_select_t *lhs,
              const vx_cell_t *binds, int nbinds, vx_result_t **res,
              char **errmsg)
{
	sql_select_t rcore;
	const sql_select_t *rhs;
	sql_setop_t op;
	vx_stmt_t *lp = NULL, *rp = NULL;
	vx_result_t *lr = NULL, *rr = NULL;
	int rc = 0, i, j;
	const sql_exprlist_t *corder;
	const sql_expr_t *climit, *coffset;
	int okey[16], oflags[16], nkey = 0;
	int64_t lim = -1, off = 0;
	enum vx_aff laff[64];
	unsigned char lnoaff[64];

	if (lhs->setop == SX_SET_NONE || lhs->rhs == NULL)
		return 0;   /* not a set operation */
	if (lhs->setop != SX_SET_UNION_ALL && lhs->setop != SX_SET_UNION &&
	    lhs->setop != SX_SET_INTERSECT && lhs->setop != SX_SET_EXCEPT)
		return 0;
	if (lhs->rhs->setop != SX_SET_NONE) return 0;   /* chained */
	/* The grammar attaches a trailing ORDER BY / LIMIT / OFFSET to the
	 * LAST select_core, but SQL applies it to the WHOLE compound: run rhs
	 * as a copy without it, and apply it to the combined result below.
	 * An ORDER BY on the lhs itself would be a syntax error in SQLite and
	 * cannot reach here. */
	if (lhs->order || lhs->limit || lhs->offset) return 0;
	rcore = *lhs->rhs;
	corder = rcore.order; climit = rcore.limit; coffset = rcore.offset;
	rcore.order = NULL; rcore.limit = NULL; rcore.offset = NULL;
	rhs = &rcore;
	if (setop_order_plan(lhs, corder, climit, coffset, binds, nbinds,
	    okey, oflags, &nkey, &lim, &off, errmsg) != 0)
		return (errmsg && *errmsg) ? -1 : 0;

	/* Recognize each side WITHOUT giving away the shared arena (pass
	 * NULL so vx_prepare_select does not free it; the caller owns it).
	 * vx_prepare_select takes a mutable view of the AST it never writes. */
	if (vx_prepare_select(db, NULL, lhs, binds, nbinds, &lp, errmsg) != 1)
		return 0;
	if (vx_prepare_select(db, NULL, rhs, binds, nbinds, &rp, errmsg) != 1) {
		vx_finalize(lp); return 0;
	}
	/* SQL requires equal arity on both sides of a compound. */
	if (lp->nout != rp->nout) {
		vx_finalize(lp); vx_finalize(rp); return 0;
	}
	op = lhs->setop;
	/* The compound's column affinity (SQLite): UNION / UNION ALL take the
	 * left arm's when it has one, else the right's; INTERSECT / EXCEPT
	 * the left arm's.  vexec states it only where both arms agree or the
	 * rule is unambiguous; otherwise the column is UNKNOWN. */
	for (i = 0; i < lp->nout && i < 64; i++) {
		laff[i] = lp->outaff[i]; lnoaff[i] = lp->outnoaff[i];
		if ((op == SX_SET_UNION || op == SX_SET_UNION_ALL) && lnoaff[i] &&
		    !rp->outnoaff[i]) {
			laff[i] = rp->outaff[i]; lnoaff[i] = 0;
		} else if ((op == SX_SET_UNION || op == SX_SET_UNION_ALL) && !lnoaff[i] &&
		    !rp->outnoaff[i] && laff[i] != rp->outaff[i]) {
			lnoaff[i] = VX_NOAFF_UNKNOWN;
		}
	}

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
	if ((nkey > 0 || lim >= 0 || off > 0) &&
	    setop_sort_slice(lr, okey, oflags, nkey, lim, off) != 0) {
		rc = -1; goto out;
	}
	for (i = 0; i < lr->ncol && i < 64; i++) {
		lr->aff[i] = laff[i]; lr->noaff[i] = lnoaff[i];
	}
	*res = lr; lr = NULL;
	rc = 1;

out:
	if (lp) vx_finalize(lp);
	if (rp) vx_finalize(rp);
	if (lr) vx_result_free(lr);
	if (rr) vx_result_free(rr);
	return rc;
}

int
vx_run(struct xsql *db, const char *sql, int n_workers,
       vx_result_t **res, char **errmsg)
{
	return vx_run_p(db, sql, NULL, 0, n_workers, res, errmsg);
}

int
vx_run_p(struct xsql *db, const char *sql, const vx_cell_t *binds, int nbinds,
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
	/* The morsel workers scan the latest committed state; a transaction
	 * reads its own snapshot and writes: serial. */
	if (n_workers > 1 && xstore_in_txn(db)) n_workers = 1;

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

/* Run an already-parsed, non-compound SELECT (an uncorrelated subquery
 * of a statement being compiled) serially, with the statement's binds;
 * the AST stays owned by the caller.  1 with *res, 0 not runnable, <0. */
static int
vx_run_sel(struct xsql *db, const sql_select_t *sel, const vx_cell_t *binds,
           int nbinds, vx_result_t **res)
{
	vx_stmt_t *plan = NULL;
	int recog;
	*res = NULL;
	if (sel == NULL || sel->setop != SX_SET_NONE) return 0;
	recog = vx_prepare_select(db, NULL, sel, binds, nbinds, &plan, NULL);
	if (recog != 1) return recog;
	return collect_serial(plan, res);
}

/* ---- plan cache ---------------------------------------------------- */

struct vx_pcache {
	char         *sql;
	sql_arena_t  *ast;
	sql_stmt_t   *root;
	vx_stmt_t    *plan;          /* SELECT: kept plan, or NULL */
	uint64_t      gen;           /* catalog generation it was built at */
	int           nsig;
	unsigned char sig[32];       /* bind storage classes it was built with */
};

static _Atomic uint64_t g_plan_builds;

uint64_t
vx_plan_build_count(void)
{
	return atomic_load_explicit(&g_plan_builds, memory_order_relaxed);
}

vx_pcache_t *
vx_pcache_new(const char *sql)
{
	vx_pcache_t *pc = (vx_pcache_t *)calloc(1, sizeof *pc);
	const char *perr = NULL;
	if (pc == NULL) return NULL;
	pc->sql = strdup(sql);
	if (pc->sql == NULL ||
	    sql_parse_ast(pc->sql, strlen(pc->sql), &pc->ast, &pc->root, &perr) != 0 ||
	    pc->root == NULL) {
		free(pc->sql); free(pc);
		return NULL;
	}
	return pc;
}

void
vx_pcache_free(vx_pcache_t *pc)
{
	if (pc == NULL) return;
	if (pc->plan) vx_finalize(pc->plan);
	sql_arena_destroy(pc->ast);
	free(pc->sql);
	free(pc);
}

static int
vx_pcache_sig_ok(const vx_pcache_t *pc, const vx_cell_t *binds, int nbinds)
{
	int i;
	if (nbinds != pc->nsig) return 0;
	for (i = 0; i < nbinds; i++)
		if (pc->sig[i] != (unsigned char)binds[i].type) return 0;
	return 1;
}

int
vx_pcache_run(vx_pcache_t *pc, struct xsql *db, const vx_cell_t *binds,
              int nbinds, vx_result_t **res, char **errmsg)
{
	const sql_select_t *sel;
	vx_stmt_t *plan = NULL;
	int recog, i, rc;

	if (res) *res = NULL;
	if (errmsg) *errmsg = NULL;
	if (pc->root->next != NULL || pc->root->kind != SQL_KIND_SELECT ||
	    pc->root->explain || pc->root->u.select == NULL)
		return 0;
	sel = pc->root->u.select;
	if (sel->setop != SX_SET_NONE)       /* compound: the text path */
		return vx_run_p(db, pc->sql, binds, nbinds, 1, res, errmsg);
	if (nbinds > 32) return vx_run_p(db, pc->sql, binds, nbinds, 1, res, errmsg);
	/* A kept plan is reused when nothing it depends on changed. */
	if (pc->plan != NULL && (pc->gen != xstore_catalog_gen() ||
	    !vx_pcache_sig_ok(pc, binds, nbinds))) {
		vx_finalize(pc->plan);
		pc->plan = NULL;
	}
	if (pc->plan != NULL) {
		vx_exec_reset(pc->plan);
		vx_plan_rebind(pc->plan, binds, nbinds);
		rc = collect_plan(pc->plan, res);
		vx_exec_reset(pc->plan);
		return rc;
	}
	pc->gen = xstore_catalog_gen();
	atomic_fetch_add_explicit(&g_plan_builds, 1, memory_order_relaxed);
	recog = vx_prepare_select(db, NULL, sel, binds, nbinds, &plan, errmsg);
	if (recog != 1) return recog;
	if (plan->noreuse) return collect_serial(plan, res);
	pc->nsig = nbinds;
	for (i = 0; i < nbinds; i++) pc->sig[i] = (unsigned char)binds[i].type;
	pc->plan = plan;
	rc = collect_plan(plan, res);
	vx_exec_reset(plan);
	return rc;
}

int
vx_pcache_write(vx_pcache_t *pc, struct xsql *db, const vx_cell_t *binds,
                int nbinds, int64_t *nchanges, char **errmsg)
{
	return vx_run_write_ast(db, NULL, pc->root, binds, nbinds, nchanges, errmsg);
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

/*
 * Build the result of PRAGMA table_info(<table>) NATIVELY from the
 * xstore catalog -- the six standard columns (cid, name, type, notnull,
 * dflt_value, pk) -- with no VDBE / sqlite_master.  Returns 1 with *res
 * owned by the caller, 0 if the table is unknown (no native schema), or
 * <0 on allocation failure.  notnull is reported 0 and dflt_value NULL
 * for every column (the native catalog records neither yet; matches the
 * common CREATE TABLE ... INTEGER PRIMARY KEY corpus, where SQLite also
 * reports notnull=0 / dflt=NULL for those columns).
 */
int
vx_pragma_table_info(struct xsql *db, const char *table, vx_result_t **res)
{
	bt_t *bt = xstore_bt_of((struct xsql *)db);
	xstore_col_t cols[64];
	vx_result_t *r;
	int nc, i;
	static const char *const cn[6] = {
		"cid", "name", "type", "notnull", "dflt_value", "pk" };

	*res = NULL;
	if (bt == NULL || table == NULL)
		return 0;
	nc = xstore_table_schema(bt, table, cols, 64);
	if (nc <= 0)
		return 0;                       /* unknown table */

	/* Count user-visible columns (skip a synthetic implicit-rowid). */
	{
		int vis = 0, c;
		for (c = 0; c < nc; c++) if (!cols[c].hidden) vis++;
		r = (vx_result_t *)calloc(1, sizeof *r);
		if (r == NULL) return -1;
		r->ncol = 6;
		r->nrow = vis;
		r->cap = vis * 6;
	}
	r->cells = (vx_cell_t *)calloc((size_t)r->cap, sizeof *r->cells);
	if (r->cells == NULL) { free(r); return -1; }
	for (i = 0; i < 6; i++) {
		snprintf(r->name[i], sizeof r->name[i], "%s", cn[i]);
		r->aff[i] = (i == 0 || i == 3 || i == 5) ? VX_AFF_NUMERIC : VX_AFF_TEXT;
	}
	{
	int cid = 0;
	for (i = 0; i < nc; i++) {
		vx_cell_t *row;
		size_t nlen, tlen;
		char *nbuf, *tbuf;
		if (cols[i].hidden) continue;   /* implicit rowid: not reported */
		row = &r->cells[(size_t)cid * 6];
		nlen = strlen(cols[i].name);
		tlen = strlen(cols[i].decltype);
		nbuf = (char *)arena_alloc(&r->arena, nlen + 1);
		tbuf = (char *)arena_alloc(&r->arena, tlen + 1);
		if (nbuf == NULL || tbuf == NULL) { vx_result_free(r); return -1; }
		memcpy(nbuf, cols[i].name, nlen + 1);
		memcpy(tbuf, cols[i].decltype, tlen + 1);
		row[0].type = VX_INT;  row[0].i = cid;               /* cid */
		row[1].type = VX_TEXT; row[1].bytes = (const uint8_t *)nbuf;
		row[1].nbytes = (uint32_t)nlen;                      /* name */
		row[2].type = VX_TEXT; row[2].bytes = (const uint8_t *)tbuf;
		row[2].nbytes = (uint32_t)tlen;                      /* type */
		row[3].type = VX_INT;  row[3].i = 0;                 /* notnull */
		row[4].type = VX_NULL;                               /* dflt_value */
		row[5].type = VX_INT;  row[5].i = cols[i].is_pk ? 1 : 0; /* pk */
		cid++;
	}
	}
	*res = r;
	return 1;
}

/*
 * For a failed native SELECT/DML, return the name of the first FROM
 * base table that is NOT in the catalog (and not a view) -- i.e. the
 * "no such table" culprit -- or NULL if every referenced table exists
 * (the decline was for some other unsupported reason).  The name is
 * returned in a static thread-unsafe buffer (used only to format an
 * error message on the failing path).  Parses sql via Lime.
 */
const char *
vx_unknown_table(struct xsql *db, const char *sql)
{
	static char namebuf[64];
	bt_t *bt = xstore_bt_of((struct xsql *)db);
	sql_arena_t *arena = NULL;
	sql_stmt_t *root = NULL;
	const char *perr = NULL;
	const sql_src_t *fr = NULL;
	const char *found = NULL;

	if (bt == NULL || sql == NULL) return NULL;
	if (sql_parse_ast(sql, strlen(sql), &arena, &root, &perr) != 0 || root == NULL)
		return NULL;
	if (root->kind == SQL_KIND_SELECT && root->u.select != NULL)
		fr = root->u.select->from;
	else if (root->kind == SQL_KIND_UPDATE && root->u.update != NULL)
		/* UPDATE has a single target table (not a sql_src chain). */
		fr = NULL;
	for (; fr != NULL; fr = fr->next) {
		char t[64], vtmp[64]; uint32_t id;
		if (fr->subquery || fr->table.len == 0 || fr->table.len >= sizeof t)
			continue;
		memcpy(t, fr->table.p, fr->table.len); t[fr->table.len] = '\0';
		if (xstore_table_id(bt, t, &id)) continue;          /* table exists */
		if (xstore_view_sql(bt, t, vtmp, (int)sizeof vtmp)) continue; /* view */
		snprintf(namebuf, sizeof namebuf, "%s", t);
		found = namebuf;
		break;
	}
	sql_arena_destroy(arena);
	return found;
}

void
vx_result_free(vx_result_t *r)
{
	if (r == NULL) return;
	arena_free(r->arena);
	free(r->cells);
	free(r);
}
