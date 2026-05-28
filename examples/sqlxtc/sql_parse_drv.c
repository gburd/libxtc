/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/sqlxtc/sql_parse_drv.c
 *	Tokenizer + driver for the Lime-generated SQL parser.
 *	Compiled in only when sql_parse_gen.c is present (Phase 2).
 *	Exposes:
 *
 *	  int sql_parse_lime(const char *sql, size_t len, sql_info_t *info);
 *
 *	Returns 0 on accept, -1 on parse failure.  On accept, fills
 *	info->kind / info->readonly.  On failure, leaves them as
 *	the caller had them so the keyword-classifier fallback wins.
 */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sql_parse.h"
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

int
sql_parse_lime(const char *sql, size_t len, sql_info_t *info)
{
	lex_t lex = { sql, sql + len };
	void *parser;
	sql_token_t tok;
	sql_parse_state_t st;

	memset(&st, 0, sizeof st);
	st.kind = SQL_KIND_UNKNOWN;
	st.readonly = 0;
	st.error = 0;
	st.err_msg = NULL;

	parser = SqlParseAlloc(malloc);
	if (!parser) return -1;

	for (;;) {
		int t = lex_next(&lex, &tok);
		if (t == 0) break;
		if (t < 0) {
			st.error = 1;
			st.err_msg = "lex error";
			break;
		}
		SqlParse(parser, t, tok, &st);
		if (st.error) break;
	}
	if (!st.error) {
		/* End-of-input. */
		sql_token_t z = { NULL, 0 };
		SqlParse(parser, 0, z, &st);
	}
	SqlParseFree(parser, free);

	if (st.error) {
		info->err = st.err_msg ? st.err_msg : "lime: parse failure";
		return -1;
	}
	if (st.kind != SQL_KIND_UNKNOWN) {
		info->kind = st.kind;
		info->readonly = st.readonly;
	}
	return 0;
}
