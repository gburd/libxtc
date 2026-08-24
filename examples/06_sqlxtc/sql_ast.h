/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/sql_ast.h
 *	Abstract syntax tree produced by the Lime SQL parser.
 *
 *	The grammar actions build this tree; the driver returns its root
 *	to the caller.  Nodes are allocated from a per-parse arena
 *	(sql_arena), so a parse that fails partway leaks nothing -- the
 *	caller frees the whole arena in one call regardless of outcome.
 *	A node never owns a heap pointer of its own; strings are slices
 *	(pointer + length) into the original SQL text, which the caller
 *	keeps alive for the tree's lifetime.
 *
 *	This is a structural tree, not yet a bound/typed plan: identifiers
 *	are not resolved to columns and expressions are not type-checked.
 *	It is enough to drive a planner and to differential-test the parse
 *	against the reference engine.
 */

#ifndef SQLXTC_SQL_AST_H
#define SQLXTC_SQL_AST_H

#include <stddef.h>
#include <stdint.h>

#include "sql_parse.h"   /* sql_kind_t */

/* ---- arena -------------------------------------------------------- *
 *
 * A bump allocator backed by a singly linked list of blocks.  All AST
 * nodes, child-pointer arrays, and the statement list live here.  One
 * sql_arena_destroy frees everything; there are no per-node frees, so
 * the grammar actions cannot leak on an early syntax error. */
typedef struct sql_arena sql_arena_t;

sql_arena_t *sql_arena_create(void);
void         sql_arena_destroy(sql_arena_t *a);
void        *sql_arena_alloc(sql_arena_t *a, size_t n);

/* ---- a text slice into the source SQL ----------------------------- */
typedef struct sql_str {
	const char *p;    /* into the caller's SQL buffer (not owned) */
	uint32_t    len;
} sql_str_t;

/* A possibly schema-qualified name (schema.table); schema.len == 0 if
 * unqualified. */
typedef struct sql_qname {
	sql_str_t schema;
	sql_str_t table;
} sql_qname_t;

/* ---- expressions -------------------------------------------------- */
typedef enum sql_expr_op {
	SX_E_NULL = 0,    /* literal NULL */
	SX_E_NUMBER,      /* numeric literal (text in .lit) */
	SX_E_STRING,      /* '...' string literal (text WITHOUT quotes in .lit) */
	SX_E_BLOB,        /* x'..' blob literal */
	SX_E_BOOL,        /* TRUE / FALSE (.ival 1/0) */
	SX_E_PARAM,       /* ? placeholder (.ival = ordinal, 1-based) */
	SX_E_COLUMN,      /* a.b.c column reference (1..3 names in .name[]) */
	SX_E_STAR,        /* * or t.* (table in .name[0] if qualified) */
	SX_E_UNARY,       /* .op2 is the token; one child a */
	SX_E_BINARY,      /* .op2 is the token; children a, b */
	SX_E_BETWEEN,     /* a BETWEEN b AND c (three children) */
	SX_E_IN_LIST,     /* a IN (list)  -- a in child a, list in .list */
	SX_E_IN_SELECT,   /* a IN (select) -- a in child a, sel in .sel */
	SX_E_IS_NULL,     /* a IS NULL    (.ival 0) / a IS NOT NULL (.ival 1) */
	SX_E_CASE,        /* CASE [base] WHEN..THEN.. [ELSE..] END */
	SX_E_FUNC,        /* name(args) -- name in .name[0], args in .list,
	                   * .ival bit0 = DISTINCT, bit1 = star-arg */
	SX_E_SUBQUERY     /* (select) used as a scalar */
} sql_expr_op_t;

typedef struct sql_expr  sql_expr_t;
typedef struct sql_exprlist sql_exprlist_t;
typedef struct sql_select sql_select_t;

/* A CASE arm: WHEN cond THEN result. */
typedef struct sql_case_arm {
	sql_expr_t          *when;
	sql_expr_t          *then;
	struct sql_case_arm *next;
} sql_case_arm_t;

struct sql_expr {
	sql_expr_op_t op;
	int           op2;        /* operator token (TK_*) for UNARY/BINARY */
	int64_t       ival;       /* PARAM ordinal, BOOL value, IS-NOT flag, FUNC flags */
	sql_str_t     lit;        /* literal text (NUMBER/STRING/BLOB) */
	sql_str_t     name[3];    /* COLUMN/STAR/FUNC name parts; n in .nname */
	int           nname;
	sql_expr_t   *a;          /* first child (unary operand, lhs, IN/CASE base) */
	sql_expr_t   *b;          /* second child (rhs, BETWEEN lo) */
	sql_expr_t   *c;          /* third child (BETWEEN hi) */
	sql_exprlist_t *list;     /* FUNC args / IN list */
	sql_select_t   *sel;      /* subquery (SUBQUERY / IN_SELECT) */
	sql_case_arm_t *arms;     /* CASE arms */
	sql_expr_t     *els;      /* CASE ELSE */
	/* Verbatim source span of this node's own token(s), into the SQL
	 * text the parser was given (NULL/0 if not a span-bearing leaf).
	 * The whole-expression span is the union over the tree -- see
	 * sql_expr_span() -- and gives SQLite-style column names for
	 * expression select items. */
	const char     *src;
	uint32_t        srclen;
};

/* A list of expressions (select list items, VALUES row, IN list, args). */
typedef struct sql_exprlist_item {
	sql_expr_t                *expr;
	sql_str_t                  alias;   /* AS alias for a select item, else len 0 */
	int                        sort;    /* 0 none, 1 ASC, 2 DESC (ORDER BY) */
	struct sql_exprlist_item  *next;
} sql_exprlist_item_t;

struct sql_exprlist {
	sql_exprlist_item_t *head;
	sql_exprlist_item_t *tail;
	int                  n;
};

/* ---- FROM / table references -------------------------------------- */
typedef enum sql_join_type {
	SX_J_NONE = 0,    /* comma / cross */
	SX_J_INNER,
	SX_J_LEFT,
	SX_J_RIGHT,
	SX_J_FULL,
	SX_J_CROSS,
	SX_J_NATURAL
} sql_join_type_t;

typedef struct sql_src {
	/* A base table (schema.name) or a subquery; aliased optionally. */
	sql_str_t      schema;    /* len 0 if unqualified */
	sql_str_t      table;     /* table name; len 0 if subquery */
	sql_select_t  *subquery;  /* non-NULL for ( select ) AS alias */
	sql_str_t      alias;     /* len 0 if none */
	const char    *subsrc;    /* verbatim ( select ) span (subquery only) */
	uint32_t       subsrclen;
	/* Join to the PREVIOUS src in the list. */
	sql_join_type_t join;
	sql_expr_t    *on;        /* ON expr (NULL if USING or none) */
	sql_exprlist_t *using_cols; /* USING (a,b) as a name list (.expr = COLUMN) */
	struct sql_src *next;
} sql_src_t;

/* ---- SELECT ------------------------------------------------------- */
typedef enum sql_setop {
	SX_SET_NONE = 0, SX_SET_UNION, SX_SET_UNION_ALL,
	SX_SET_INTERSECT, SX_SET_EXCEPT
} sql_setop_t;

/* A common table expression: name [ (cols) ] AS ( select ). */
typedef struct sql_cte {
	sql_str_t        name;
	sql_exprlist_t  *cols;     /* optional column-name list (COLUMN exprs), or NULL */
	sql_select_t    *select;
	const char      *src;      /* verbatim source span of the ( select ), or NULL */
	uint32_t         srclen;
	struct sql_cte  *next;
} sql_cte_t;

struct sql_select {
	sql_cte_t      *with;       /* WITH cte-list prefix (NULL if none) */
	int             with_recursive; /* 1 if WITH RECURSIVE */
	int             distinct;   /* 1 = DISTINCT */
	sql_exprlist_t *cols;       /* select list (items may be STAR) */
	sql_src_t      *from;       /* FROM chain (NULL if none) */
	sql_expr_t     *where;      /* WHERE (NULL if none) */
	sql_exprlist_t *group;      /* GROUP BY (NULL if none) */
	sql_expr_t     *having;     /* HAVING (NULL if none) */
	sql_exprlist_t *order;      /* ORDER BY (items carry .sort) */
	sql_expr_t     *limit;      /* LIMIT (NULL if none) */
	sql_expr_t     *offset;     /* OFFSET (NULL if none) */
	sql_setop_t     setop;      /* compound operator to .rhs */
	sql_select_t   *rhs;        /* right side of a set operation */
	const char     *src;        /* verbatim start of this SELECT in the
	                             * source SQL (the SELECT keyword), or
	                             * NULL; length runs to end-of-statement
	                             * for a trailing SELECT (INSERT..SELECT) */
};

/* ---- INSERT / UPDATE / DELETE / CREATE / DROP --------------------- */
typedef struct sql_insert {
	sql_str_t       schema, table;
	sql_exprlist_t *cols;       /* explicit (a,b,c) column list or NULL */
	/* Exactly one of these is set: */
	sql_exprlist_t **rows;      /* VALUES rows; each is an exprlist */
	int             n_rows;
	sql_select_t   *select;     /* INSERT ... SELECT */
	int             def_values; /* INSERT ... DEFAULT VALUES */
	int             replace;    /* REPLACE INTO */
} sql_insert_t;

typedef struct sql_assign {
	sql_str_t          col;
	sql_expr_t        *val;
	struct sql_assign *next;
} sql_assign_t;

typedef struct sql_update {
	sql_str_t     schema, table;
	sql_assign_t *sets;
	sql_expr_t   *where;
} sql_update_t;

typedef struct sql_delete {
	sql_str_t   schema, table;
	sql_expr_t *where;
} sql_delete_t;

/* CREATE TABLE column + constraint shapes (kept structural). */
typedef struct sql_coldef {
	sql_str_t          name;
	sql_str_t          type;     /* declared type token text, len 0 if none */
	int                primary;  /* 1 if PRIMARY KEY on this column */
	int                notnull;
	int                unique;
	struct sql_coldef *next;
} sql_coldef_t;

typedef enum sql_create_kind {
	SX_CR_TABLE = 0, SX_CR_TABLE_AS, SX_CR_INDEX, SX_CR_VIEW
} sql_create_kind_t;

typedef struct sql_create {
	sql_create_kind_t kind;
	int               temp;
	int               unique;       /* CREATE UNIQUE INDEX */
	int               if_not_exists;
	sql_str_t         schema, name;
	sql_coldef_t     *cols;         /* TABLE: column defs */
	sql_select_t     *select;       /* TABLE_AS / VIEW */
	sql_str_t         on_table;     /* INDEX: target table */
	sql_exprlist_t   *index_cols;   /* INDEX: indexed columns (COLUMN exprs) */
} sql_create_t;

typedef enum sql_drop_kind {
	SX_DR_TABLE = 0, SX_DR_INDEX, SX_DR_VIEW, SX_DR_TRIGGER
} sql_drop_kind_t;

typedef struct sql_drop {
	sql_drop_kind_t kind;
	int             if_exists;
	sql_str_t       schema, name;
} sql_drop_t;

typedef struct sql_pragma {
	sql_str_t   schema, name;
	sql_expr_t *value;     /* = value or (value), NULL for a bare read */
} sql_pragma_t;

typedef struct sql_txn {
	/* BEGIN / COMMIT / ROLLBACK; .name carries an optional mode/name. */
	sql_str_t name;
} sql_txn_t;

typedef struct sql_attach {
	sql_expr_t *target;    /* the file expression */
	sql_str_t   alias;
} sql_attach_t;

/* ---- a statement -------------------------------------------------- */
typedef struct sql_stmt {
	sql_kind_t kind;
	int        readonly;
	int        explain;     /* 1 = EXPLAIN, 2 = EXPLAIN QUERY PLAN */
	union {
		sql_select_t *select;
		sql_insert_t *insert;
		sql_update_t *update;
		sql_delete_t *del;
		sql_create_t *create;
		sql_drop_t   *drop;
		sql_pragma_t *pragma;
		sql_txn_t    *txn;
		sql_attach_t *attach;
		sql_str_t     detach;   /* DETACH name */
	} u;
	struct sql_stmt *next;      /* statements are chained (a; b; c) */
} sql_stmt_t;

/* Compute the verbatim source span of an expression: the union of the
 * span-bearing leaves under it.  Returns 1 with *p / *len set to the
 * slice of the original SQL text covering the whole expression, or 0 if
 * no leaf carried a span.  Used to name an expression select item the
 * way SQLite does (its source text). */
int sql_expr_span(const sql_expr_t *e, const char **p, int *len);

#endif /* SQLXTC_SQL_AST_H */
