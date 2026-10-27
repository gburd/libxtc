/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/fuzz/fuzz_sql.c -- libFuzzer harness for the sqlxtc SQL parser:
 * sql_parse (the classifier conn.c runs on every new statement) and
 * sql_parse_ast (the Lime parse every statement the engine executes goes
 * through), over arbitrary text.
 *
 *	The input is copied to an exact-size, NUL-terminated heap buffer
 *	(the engine passes strlen(sql), conn.c passes the request's length
 *	of a buffer that is not NUL-terminated: both are exercised, the
 *	latter by passing n without a terminator).  A successful AST parse
 *	is walked so every node the grammar built is touched (ASan sees a
 *	dangling or uninitialised child), and sql_expr_span of every
 *	select-list expression must lie inside the input.
 *
 *	Semantic oracle: a statement whose first keyword is one of SELECT,
 *	INSERT, REPLACE, UPDATE, DELETE, CREATE, DROP or PRAGMA can only
 *	parse as that kind.  This is what caught the parser carrying on
 *	after a stack overflow ("SELECT" + 98 '(' + "DELETE FROM t" parsed
 *	as a DELETE; corpus/sql/crash-stack-overflow-continues).
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sql_parse.h"
#include "sql_ast.h"

static const char *g_lo, *g_hi;

static void
fz_check(int ok)
{
	if (!ok)
		abort();
}

static void fz_expr(const sql_expr_t *e, int depth);
static void fz_select(const sql_select_t *s, int depth);

static void
fz_list(const sql_exprlist_t *l, int depth)
{
	const sql_exprlist_item_t *it;
	const char *p;
	int len;

	if (l == NULL)
		return;
	for (it = l->head; it != NULL; it = it->next) {
		fz_expr(it->expr, depth + 1);
		if (it->expr != NULL && sql_expr_span(it->expr, &p, &len)) {
			fz_check(len > 0 && p >= g_lo && p + len <= g_hi);
		}
	}
}

static void
fz_expr(const sql_expr_t *e, int depth)
{
	const sql_case_arm_t *a;

	fz_check(depth < 100000);
	if (e == NULL)
		return;
	fz_expr(e->a, depth + 1);
	fz_expr(e->b, depth + 1);
	fz_expr(e->c, depth + 1);
	fz_list(e->list, depth + 1);
	fz_select(e->sel, depth + 1);
	for (a = e->arms; a != NULL; a = a->next) {
		fz_expr(a->when, depth + 1);
		fz_expr(a->then, depth + 1);
	}
	fz_expr(e->els, depth + 1);
}

static void
fz_select(const sql_select_t *s, int depth)
{
	for (; s != NULL; s = s->rhs, depth++) {
		fz_list(s->cols, depth);
		fz_expr(s->where, depth);
		fz_list(s->group, depth);
		fz_expr(s->having, depth);
		fz_list(s->order, depth);
		fz_expr(s->limit, depth);
		fz_expr(s->offset, depth);
	}
}

/* The kind the first keyword of s[0..n) forces, or -1 (none, or a
 * keyword that may start several kinds: WITH, EXPLAIN, ...). */
static int
fz_lead_kind(const char *s, size_t n)
{
	static const struct {
		const char *kw;
		int kind;
	} k[] = {
		{ "SELECT", SQL_KIND_SELECT },
		{ "INSERT", SQL_KIND_INSERT },
		{ "REPLACE", SQL_KIND_INSERT },
		{ "UPDATE", SQL_KIND_UPDATE },
		{ "DELETE", SQL_KIND_DELETE },
		{ "CREATE", SQL_KIND_CREATE },
		{ "DROP", SQL_KIND_DROP },
		{ "PRAGMA", SQL_KIND_PRAGMA },
	};
	size_t i = 0, j, w;

	for (;;) { /* the lexer's whitespace + comments */
		while (i < n &&
		    (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' ||
		        s[i] == '\n'))
			i++;
		if (i + 1 < n && s[i] == '-' && s[i + 1] == '-') {
			while (i < n && s[i] != '\n')
				i++;
		} else if (i + 1 < n && s[i] == '/' && s[i + 1] == '*') {
			for (i += 2;
			     i + 1 < n && !(s[i] == '*' && s[i + 1] == '/');)
				i++;
			i = i + 1 < n ? i + 2 : n;
		} else {
			break;
		}
	}
	for (w = i; w < n &&
	     ((s[w] >= 'A' && s[w] <= 'Z') || (s[w] >= 'a' && s[w] <= 'z') ||
	         (s[w] >= '0' && s[w] <= '9') || s[w] == '_');
	     w++)
		;
	for (j = 0; j < sizeof k / sizeof k[0]; j++) {
		size_t m, len = strlen(k[j].kw);
		if (w - i != len)
			continue;
		for (m = 0; m < len; m++)
			if ((s[i + m] & ~0x20) != k[j].kw[m])
				break;
		if (m == len)
			return k[j].kind;
	}
	return -1;
}

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n);

int
LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
	sql_info_t info;
	sql_arena_t *arena = NULL;
	sql_stmt_t *root = NULL, *st;
	const char *err = NULL;
	char *s;
	int rc, lead;

	if (n > 16384)
		return 0;
	/* No terminator: the parsers are given a length and must keep to it. */
	s = malloc(n ? n : 1);
	fz_check(s != NULL);
	memcpy(s, d, n);
	g_lo = s;
	g_hi = s + n;
	(void)sql_parse(s, n, &info);
	rc = sql_parse_ast(s, n, &arena, &root, &err);
	if (rc == 0) {
		fz_check(arena != NULL);
		lead = fz_lead_kind(s, n);
		if (lead >= 0 && root != NULL)
			fz_check((int)root->kind == lead);
		for (st = root; st != NULL; st = st->next) {
			fz_check(st->kind >= SQL_KIND_UNKNOWN &&
			    st->kind <= SQL_KIND_EXPLAIN);
			if (st->kind == SQL_KIND_SELECT)
				fz_select(st->u.select, 0);
		}
		sql_arena_destroy(arena);
	} else {
		fz_check(arena == NULL && root == NULL && err != NULL);
	}
	free(s);
	return 0;
}
