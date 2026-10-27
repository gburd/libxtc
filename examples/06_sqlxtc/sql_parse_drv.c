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
#include <stdatomic.h>
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
	{ "ALTER",       TK_ALTER },
	{ "AND",         TK_AND },
	{ "AS",          TK_AS },
	{ "ASC",         TK_ASC },
	{ "ATTACH",      TK_ATTACH },
	{ "AUTOINCREMENT", TK_AUTOINCR },
	{ "BEGIN",       TK_BEGIN },
	{ "BETWEEN",     TK_BETWEEN },
	{ "BY",          TK_BY },
	{ "CASE",        TK_CASE },
	{ "CAST",        TK_CAST },
	{ "CHECK",       TK_CHECK },
	{ "COMMIT",      TK_COMMIT },
	{ "CREATE",      TK_CREATE },
	{ "CROSS",       TK_CROSS },
	{ "CURRENT_DATE", TK_CTIME },
	{ "CURRENT_TIME", TK_CTIME },
	{ "CURRENT_TIMESTAMP", TK_CTIME },
	{ "DATABASE",    TK_DATABASE },
	{ "DEFAULT",     TK_DEFAULT },
	{ "DELETE",      TK_DELETE },
	{ "DESC",        TK_DESC },
	{ "DETACH",      TK_DETACH },
	{ "DISTINCT",    TK_DISTINCT },
	{ "DROP",        TK_DROP },
	{ "ELSE",        TK_ELSE },
	{ "END",         TK_END },
	{ "ESCAPE",      TK_ESCAPE },
	{ "EXCEPT",      TK_EXCEPT },
	{ "EXISTS",      TK_EXISTS },
	{ "EXPLAIN",     TK_EXPLAIN },
	{ "FALSE",       TK_FALSE },
	{ "FIRST",       TK_FIRST },
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
	{ "LAST",        TK_LAST },
	{ "LEFT",        TK_LEFT },
	{ "LIKE",        TK_LIKE },
	{ "LIMIT",       TK_LIMIT },
	{ "NATURAL",     TK_NATURAL },
	{ "NOT",         TK_NOT },
	{ "NULL",        TK_NULL },
	{ "NULLS",       TK_NULLS },
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
	{ "RELEASE",     TK_RELEASE },
	{ "RENAME",      TK_RENAME },
	{ "REPLACE",     TK_REPLACE },
	{ "RIGHT",       TK_RIGHT },
	{ "ROLLBACK",    TK_ROLLBACK },
	{ "SAVEPOINT",   TK_SAVEPOINT },
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
		case '?':   /* ? or ?NNN */
			l->p++;
			while (l->p < l->end && isdigit((unsigned char)*l->p)) l->p++;
			tok->len = (int)(l->p - tok->p);
			return TK_QMARK;
		case ':': case '@': case '$':   /* :name @name $name */
			l->p++;
			while (l->p < l->end &&
			    (isalnum((unsigned char)*l->p) || *l->p == '_')) l->p++;
			tok->len = (int)(l->p - tok->p);
			if (tok->len < 2) return -1;
			return TK_QMARK;
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
		case '"': case '`': case '[': {
			/* Quoted identifier; "" / `` inside is one quote ([x] has no
			 * escape).  The driver dequotes it before the parser. */
			char close = (c == '[') ? ']' : c;
			l->p++;
			for (;;) {
				if (l->p >= l->end) return -1;     /* unterminated */
				if (*l->p == close) {
					if (close != ']' && l->p + 1 < l->end &&
					    l->p[1] == close) { l->p += 2; continue; }
					l->p++;
					break;
				}
				l->p++;
			}
			tok->len = (int)(l->p - tok->p);
			return TK_ID;
		}
		default: break;
		}
	}

	/* hex integer 0x1F: at least one hex digit (the grammar converts it) */
	if (l->p + 2 < l->end && l->p[0] == '0' &&
	    (l->p[1] == 'x' || l->p[1] == 'X') &&
	    isxdigit((unsigned char)l->p[2])) {
		l->p += 2;
		while (l->p < l->end && isxdigit((unsigned char)*l->p)) l->p++;
		if (l->p < l->end && (isalnum((unsigned char)*l->p) || *l->p == '_'))
			return -1;     /* 0x1g: unrecognized token, as SQLite */
		tok->len = (int)(l->p - tok->p);
		return TK_NUMBER;
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
		/* 12abc / 0x: SQLite's "unrecognized token", not 12 AS abc. */
		if (l->p < l->end && (isalpha((unsigned char)*l->p) || *l->p == '_'))
			return -1;
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

/* A quoted identifier token "x" / [x] / `x`: point it at the bare name,
 * collapsing a doubled quote (""/``) into the arena when present, so the
 * grammar and every AST consumer see the identifier itself. */
static void
dequote_id(sql_token_t *tok, sql_parse_state_t *st)
{
	char q = tok->p[0], close = (q == '[') ? ']' : q;
	const char *p = tok->p + 1;
	int n = tok->len - 2, i, o;
	char *buf;
	if (n < 0) return;
	if (close == ']' || memchr(p, close, (size_t)n) == NULL) {
		tok->p = p; tok->len = n; return;
	}
	buf = (char *)sql_arena_alloc(st->arena, (size_t)n + 1);
	if (buf == NULL) { st->error = 1; st->err_msg = "oom"; return; }
	for (i = 0, o = 0; i < n; i++) {
		buf[o++] = p[i];
		if (p[i] == close && i + 1 < n && p[i + 1] == close) i++;
	}
	tok->p = buf; tok->len = o;
}

/*
 * Core: run the push parser over `sql`, filling `st` (which the caller
 * has initialized with an arena).  Returns 0 on accept, -1 on lex/parse
 * error.
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
		tok.src = tok.p;
		tok.srclen = tok.len;
		if (t == TK_ID && tok.len >= 2 &&
		    (tok.p[0] == '"' || tok.p[0] == '[' || tok.p[0] == '`')) {
			dequote_id(&tok, st);
			if (st->error) break;
		}
		SqlParse(parser, t, tok, st);
		if (st->error) break;
	}
	if (!st->error) {
		sql_token_t z = { NULL, 0, NULL, 0 };
		SqlParse(parser, 0, z, st);   /* end-of-input */
	}
	SqlParseFree(parser, free);
	return st->error ? -1 : 0;
}

static _Atomic unsigned long long g_parse_count;   /* parses run (metric) */

unsigned long long
sql_parse_count(void)
{
	return atomic_load_explicit(&g_parse_count, memory_order_relaxed);
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

	atomic_fetch_add_explicit(&g_parse_count, 1, memory_order_relaxed);
	if (sql_parse_run(sql, len, &st) < 0) {
		if (err_out) *err_out = st.err_msg ? st.err_msg : "parse error";
		sql_arena_destroy(arena);
		return -1;
	}
	/* Parameter ordinals were assigned in lexical order by the grammar's
	 * TK_QMARK action (param_ordinal). */
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

	atomic_fetch_add_explicit(&g_parse_count, 1, memory_order_relaxed);
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
