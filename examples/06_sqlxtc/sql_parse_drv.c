/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/sql_parse_drv.c
 *	Tokenizer + driver for the Lime-generated SQL parser.
 *	Compiled in only when sql_parse_gen.c is present (Phase 2).
 *	Exposes:
 *
 *	  int sql_parse_lime(const char *sql, size_t len, sql_info_t *info);
 *	  int sql_parse_ast(const char *sql, size_t len,
 *	                    sql_arena_t **arena_out, sql_stmt_t **root_out,
 *	                    const char **err_out);
 *
 *	sql_parse_lime keeps the legacy classifier contract (sets
 *	info->kind / info->readonly).  sql_parse_ast returns the full
 *	AST: on success *arena_out owns the tree (caller frees it with
 *	sql_arena_destroy) and *root_out is the statement list; on a
 *	parse error it returns -1 with *err_out set and frees the arena.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sql_parse.h"
#include "sql_ast.h"
#include "sql_parse_lime.h"

/* From Lime-generated sql_parse_gen.[ch]. */
#include "sql_parse_gen.h"

extern void *SqlParseAlloc(void *(*mallocProc)(size_t));
extern void  SqlParseFree(void *p, void (*freeProc)(void *));
extern void  SqlParse(void *yyp, int yymajor, sql_token_t yyminor,
                      sql_parse_state_t *pstate);

/* ===== keyword table ===== */

typedef struct kw {
	const char *name;
	int         tok;
} kw_t;

/* Sorted by name for clarity.  Linear scan is fine for ~70 entries. */
static const kw_t k_kws[] = {
	{ "ALL",         TK_ALL },
	{ "AND",         TK_AND },
	{ "AS",          TK_AS },
	{ "ASC",         TK_ASC },
	{ "ATTACH",      TK_ATTACH },
	{ "AUTOINCREMENT", TK_AUTOINCR },
	{ "BEGIN",       TK_BEGIN },
	{ "BETWEEN",     TK_BETWEEN },
	{ "BY",          TK_BY },
	{ "CASE",        TK_CASE },
	{ "CHECK",       TK_CHECK },
	{ "COMMIT",      TK_COMMIT },
	{ "CREATE",      TK_CREATE },
	{ "CROSS",       TK_CROSS },
	{ "DATABASE",    TK_DATABASE },
	{ "DEFAULT",     TK_DEFAULT },
	{ "DELETE",      TK_DELETE },
	{ "DESC",        TK_DESC },
	{ "DETACH",      TK_DETACH },
	{ "DISTINCT",    TK_DISTINCT },
	{ "DROP",        TK_DROP },
	{ "ELSE",        TK_ELSE },
	{ "END",         TK_END },
	{ "EXCEPT",      TK_EXCEPT },
	{ "EXISTS",      TK_EXISTS },
	{ "EXPLAIN",     TK_EXPLAIN },
	{ "FALSE",       TK_FALSE },
	{ "FOREIGN",     TK_FOREIGN },
	{ "FROM",        TK_FROM },
	{ "FULL",        TK_FULL },
	{ "GROUP",       TK_GROUP },
	{ "HAVING",      TK_HAVING },
	{ "IF",          TK_IF },
	{ "IN",          TK_IN },
	{ "INDEX",       TK_INDEX },
	{ "INNER",       TK_INNER },
	{ "INSERT",      TK_INSERT },
	{ "INTERSECT",   TK_INTERSECT },
	{ "INTO",        TK_INTO },
	{ "IS",          TK_IS },
	{ "JOIN",        TK_JOIN },
	{ "KEY",         TK_KEY },
	{ "LEFT",        TK_LEFT },
	{ "LIKE",        TK_LIKE },
	{ "LIMIT",       TK_LIMIT },
	{ "NATURAL",     TK_NATURAL },
	{ "NOT",         TK_NOT },
	{ "NULL",        TK_NULL },
	{ "OFFSET",      TK_OFFSET },
	{ "ON",          TK_ON },
	{ "OR",          TK_OR },
	{ "ORDER",       TK_ORDER },
	{ "OUTER",       TK_OUTER },
	{ "PLAN",        TK_PLAN },
	{ "PRAGMA",      TK_PRAGMA },
	{ "PRIMARY",     TK_PRIMARY },
	{ "QUERY",       TK_QUERY },
	{ "RECURSIVE",   TK_RECURSIVE },
	{ "REFERENCES",  TK_REFERENCES },
	{ "REPLACE",     TK_REPLACE },
	{ "RIGHT",       TK_RIGHT },
	{ "ROLLBACK",    TK_ROLLBACK },
	{ "SELECT",      TK_SELECT },
	{ "SET",         TK_SET },
	{ "TABLE",       TK_TABLE },
	{ "TEMP",        TK_TEMP },
	{ "TEMPORARY",   TK_TEMPORARY },
	{ "THEN",        TK_THEN },
	{ "TO",          TK_TO },
	{ "TRANSACTION", TK_TRANSACTION },
	{ "TRIGGER",     TK_TRIGGER },
	{ "TRUE",        TK_TRUE },
	{ "UNION",       TK_UNION },
	{ "UNIQUE",      TK_UNIQUE },
	{ "UPDATE",      TK_UPDATE },
	{ "USING",       TK_USING },
	{ "VALUES",      TK_VALUES },
	{ "VIEW",        TK_VIEW },
	{ "WHEN",        TK_WHEN },
	{ "WHERE",       TK_WHERE },
	{ "WITH",        TK_WITH }
};

#define N_KW (int)(sizeof k_kws / sizeof k_kws[0])

static int
keyword_lookup(const char *p, int n)
{
	int i;
	for (i = 0; i < N_KW; i++) {
		if ((int)strlen(k_kws[i].name) != n) continue;
		int j;
		int ok = 1;
		for (j = 0; j < n; j++) {
			char c = p[j];
			if (c >= 'a' && c <= 'z') c -= 32;
			if (c != k_kws[i].name[j]) { ok = 0; break; }
		}
		if (ok) return k_kws[i].tok;
	}
	return -1;
}

/* ===== tokenizer ===== */

typedef struct {
	const char *p;
	const char *end;
} lex_t;

static int
lex_next(lex_t *l, sql_token_t *tok)
{
	while (l->p < l->end) {
		char c = *l->p;
		if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
			l->p++; continue;
		}
		/* line comment */
		if (l->p + 1 < l->end && c == '-' && l->p[1] == '-') {
			while (l->p < l->end && *l->p != '\n') l->p++;
			continue;
		}
		/* block comment */
		if (l->p + 1 < l->end && c == '/' && l->p[1] == '*') {
			l->p += 2;
			while (l->p + 1 < l->end &&
			       !(l->p[0] == '*' && l->p[1] == '/'))
				l->p++;
			if (l->p + 1 < l->end) l->p += 2;
			continue;
		}
		break;
	}
	if (l->p >= l->end) return 0;

	tok->p = l->p;

	{
		char c = *l->p;
		switch (c) {
		case '(':  l->p++; tok->len = 1; return TK_LP;
		case ')':  l->p++; tok->len = 1; return TK_RP;
		case ',':  l->p++; tok->len = 1; return TK_COMMA;
		case ';':  l->p++; tok->len = 1; return TK_SEMI;
		case '.':
			if (l->p + 1 < l->end &&
			    isdigit((unsigned char)l->p[1])) break;
			l->p++; tok->len = 1; return TK_DOT;
		case '*':  l->p++; tok->len = 1; return TK_STAR;
		case '+':  l->p++; tok->len = 1; return TK_PLUS;
		case '-':  l->p++; tok->len = 1; return TK_MINUS;
		case '/':  l->p++; tok->len = 1; return TK_SLASH;
		case '%':  l->p++; tok->len = 1; return TK_PERCENT;
		case '?':  l->p++; tok->len = 1; return TK_QMARK;
		case '~':  l->p++; tok->len = 1; return TK_TILDE;
		case '&':  l->p++; tok->len = 1; return TK_AMP;
		case '=':
			l->p++;
			if (l->p < l->end && *l->p == '=') l->p++;
			tok->len = (int)(l->p - tok->p); return TK_EQ;
		case '<':
			l->p++;
			if (l->p < l->end && (*l->p == '=' || *l->p == '>')) {
				int t = (*l->p == '=') ? TK_LE : TK_NE;
				l->p++;
				tok->len = (int)(l->p - tok->p);
				return t;
			}
			tok->len = 1;
			return TK_LT;
		case '>':
			l->p++;
			if (l->p < l->end && *l->p == '=') {
				l->p++;
				tok->len = 2;
				return TK_GE;
			}
			tok->len = 1;
			return TK_GT;
		case '!':
			if (l->p + 1 < l->end && l->p[1] == '=') {
				l->p += 2;
				tok->len = 2;
				return TK_NE;
			}
			break;
		case '|':
			l->p++;
			if (l->p < l->end && *l->p == '|') {
				l->p++;
				tok->len = 2;
				return TK_CONCAT;
			}
			tok->len = 1;
			return TK_PIPE;
		case '\'': {
			l->p++;     /* opening quote */
			while (l->p < l->end) {
				if (*l->p == '\'') {
					if (l->p + 1 < l->end && l->p[1] == '\'') {
						l->p += 2; continue;
					}
					l->p++;     /* closing quote */
					tok->len = (int)(l->p - tok->p);
					return TK_STRING;
				}
				l->p++;
			}
			return -1;     /* unterminated */
		}
		case '"': {
			l->p++;
			while (l->p < l->end && *l->p != '"') l->p++;
			if (l->p < l->end) l->p++;
			tok->len = (int)(l->p - tok->p);
			return TK_ID;     /* quoted identifier */
		}
		case '`': case '[': {
			char close = (c == '`') ? '`' : ']';
			l->p++;
			while (l->p < l->end && *l->p != close) l->p++;
			if (l->p < l->end) l->p++;
			tok->len = (int)(l->p - tok->p);
			return TK_ID;
		}
		default: break;
		}
	}

	/* number */
	if (isdigit((unsigned char)*l->p) ||
	    (*l->p == '.' && l->p + 1 < l->end &&
	     isdigit((unsigned char)l->p[1]))) {
		while (l->p < l->end &&
		       (isdigit((unsigned char)*l->p) ||
		        *l->p == '.')) l->p++;
		if (l->p < l->end && (*l->p == 'e' || *l->p == 'E')) {
			l->p++;
			if (l->p < l->end && (*l->p == '+' || *l->p == '-'))
				l->p++;
			while (l->p < l->end && isdigit((unsigned char)*l->p))
				l->p++;
		}
		tok->len = (int)(l->p - tok->p);
		return TK_NUMBER;
	}

	/* identifier / keyword */
	if (isalpha((unsigned char)*l->p) || *l->p == '_') {
		while (l->p < l->end &&
		       (isalnum((unsigned char)*l->p) || *l->p == '_'))
			l->p++;
		tok->len = (int)(l->p - tok->p);
		{
			int kw = keyword_lookup(tok->p, tok->len);
			if (kw > 0) return kw;
		}
		/* X'..' blob literal */
		if ((tok->len == 1 && (tok->p[0] == 'x' || tok->p[0] == 'X')) &&
		    l->p < l->end && *l->p == '\'') {
			l->p++;
			while (l->p < l->end && *l->p != '\'') l->p++;
			if (l->p < l->end) l->p++;
			tok->len = (int)(l->p - tok->p);
			return TK_BLOB;
		}
		return TK_ID;
	}

	/* unknown char: skip and report error */
	l->p++;
	return -1;
}

/* ===== driver ===== */

/*
 * Core: run the push parser over `sql`, filling `st` (which the caller
 * has initialized with an arena).  Assigns 1-based ordinals to ? params
 * in lexed order.  Returns 0 on accept, -1 on lex/parse error.
 */
static int
sql_parse_run(const char *sql, size_t len, sql_parse_state_t *st)
{
	lex_t lex = { sql, sql + len };
	void *parser;
	sql_token_t tok;

	parser = SqlParseAlloc(malloc);
	if (!parser) { st->error = 1; st->err_msg = "oom"; return -1; }

	for (;;) {
		int t = lex_next(&lex, &tok);
		if (t == 0) break;
		if (t < 0) {
			st->error = 1;
			st->err_msg = "lex error";
			break;
		}
		SqlParse(parser, t, tok, st);
		if (st->error) break;
	}
	if (!st->error) {
		sql_token_t z = { NULL, 0 };
		SqlParse(parser, 0, z, st);   /* end-of-input */
	}
	SqlParseFree(parser, free);
	return st->error ? -1 : 0;
}

/* Walk the AST and assign 1-based ordinals to ? parameters in the
 * left-to-right order they appear.  (The grammar leaves PARAM.ival 0;
 * a single post-order walk numbers them.) */
static void number_params_expr(sql_expr_t *e, int *next);
static void number_params_list(sql_exprlist_t *l, int *next) {
	sql_exprlist_item_t *it;
	if (!l) return;
	for (it = l->head; it; it = it->next) number_params_expr(it->expr, next);
}
static void number_params_expr(sql_expr_t *e, int *next) {
	sql_case_arm_t *arm;
	if (!e) return;
	if (e->op == SX_E_PARAM) { e->ival = (*next)++; return; }
	number_params_expr(e->a, next);
	number_params_expr(e->b, next);
	number_params_expr(e->c, next);
	number_params_list(e->list, next);
	for (arm = e->arms; arm; arm = arm->next) {
		number_params_expr(arm->when, next);
		number_params_expr(arm->then, next);
	}
	number_params_expr(e->els, next);
}

static void number_params_select(sql_select_t *s, int *next);
static void
number_params_src(sql_src_t *src, int *next)
{
	for (; src; src = src->next) {
		if (src->subquery) number_params_select(src->subquery, next);
		number_params_expr(src->on, next);
	}
}
static void
number_params_select(sql_select_t *s, int *next)
{
	if (!s) return;
	number_params_list(s->cols, next);
	number_params_src(s->from, next);
	number_params_expr(s->where, next);
	number_params_list(s->group, next);
	number_params_expr(s->having, next);
	number_params_list(s->order, next);
	number_params_expr(s->limit, next);
	number_params_expr(s->offset, next);
	number_params_select(s->rhs, next);
}
static void
number_params_stmt(sql_stmt_t *s, int *next)
{
	if (!s) return;
	switch (s->kind) {
	case SQL_KIND_SELECT:
		number_params_select(s->u.select, next);
		break;
	case SQL_KIND_INSERT:
		if (s->u.insert) {
			int i;
			for (i = 0; i < s->u.insert->n_rows; i++)
				number_params_list(s->u.insert->rows[i], next);
			number_params_select(s->u.insert->select, next);
		}
		break;
	case SQL_KIND_UPDATE:
		if (s->u.update) {
			sql_assign_t *a;
			for (a = s->u.update->sets; a; a = a->next)
				number_params_expr(a->val, next);
			number_params_expr(s->u.update->where, next);
		}
		break;
	case SQL_KIND_DELETE:
		if (s->u.del) number_params_expr(s->u.del->where, next);
		break;
	case SQL_KIND_CREATE:
		if (s->u.create) number_params_select(s->u.create->select, next);
		break;
	case SQL_KIND_PRAGMA:
		if (s->u.pragma) number_params_expr(s->u.pragma->value, next);
		break;
	default: break;
	}
}

int
sql_parse_ast(const char *sql, size_t len, sql_arena_t **arena_out,
              sql_stmt_t **root_out, const char **err_out)
{
	sql_parse_state_t st;
	sql_arena_t *arena;

	if (arena_out) *arena_out = NULL;
	if (root_out) *root_out = NULL;
	if (err_out) *err_out = NULL;

	arena = sql_arena_create();
	if (!arena) { if (err_out) *err_out = "oom"; return -1; }

	memset(&st, 0, sizeof st);
	st.kind = SQL_KIND_UNKNOWN;
	st.arena = arena;

	if (sql_parse_run(sql, len, &st) < 0) {
		if (err_out) *err_out = st.err_msg ? st.err_msg : "parse error";
		sql_arena_destroy(arena);
		return -1;
	}
	/* ? parameter ordinals are assigned by a post-order walk of each
	 * statement's expression trees (left-to-right appearance order). */
	{
		int next = 1;
		sql_stmt_t *s;
		for (s = st.stmts; s; s = s->next)
			number_params_stmt(s, &next);
	}
	if (arena_out) *arena_out = arena; else sql_arena_destroy(arena);
	if (root_out) *root_out = st.stmts;
	return 0;
}

int
sql_parse_lime(const char *sql, size_t len, sql_info_t *info)
{
	sql_parse_state_t st;
	sql_arena_t *arena;

	arena = sql_arena_create();
	if (!arena) return -1;

	memset(&st, 0, sizeof st);
	st.kind = SQL_KIND_UNKNOWN;
	st.arena = arena;

	if (sql_parse_run(sql, len, &st) < 0) {
		info->err = st.err_msg ? st.err_msg : "lime: parse failure";
		sql_arena_destroy(arena);
		return -1;
	}
	if (st.kind != SQL_KIND_UNKNOWN) {
		info->kind = st.kind;
		info->readonly = st.readonly;
	}
	sql_arena_destroy(arena);   /* classifier does not keep the tree */
	return 0;
}
