/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * test/sqlxtc/test_sql_parse.c
 *	Tests for sql_parse() which combines a keyword classifier with
 *	the Lime-generated grammar parser.
 *
 *	30+ cases.  Builds against the sqlxtc objects in
 *	examples/06_sqlxtc/.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../examples/06_sqlxtc/sql_parse.h"
#include "../../examples/06_sqlxtc/sql_ast.h"

static int n_pass = 0;
static int n_fail = 0;

#define CHECK(cond, name) do {                                          \
	if (cond) { n_pass++; printf("  OK   %s\n", name); }            \
	else      { n_fail++; printf("  FAIL %s\n", name); }            \
} while (0)

static int
parse(const char *sql, sql_info_t *info)
{
	return sql_parse(sql, strlen(sql), info);
}

/* "SELECT " + depth '(' + tail, parsed to an AST: the return code, and
 * the parsed root's kind in *kind (-1 when there is none). */
static int
parse_nested(int depth, const char *tail, int *kind)
{
	char buf[512];
	size_t n = (size_t)snprintf(buf, sizeof buf, "SELECT ");
	struct sql_arena *a = NULL;
	struct sql_stmt *root = NULL;
	const char *err = NULL;
	int rc, i;

	for (i = 0; i < depth && n + 1 < sizeof buf; i++)
		buf[n++] = '(';
	n += (size_t)snprintf(buf + n, sizeof buf - n, "%s", tail);
	rc = sql_parse_ast(buf, n, &a, &root, &err);
	*kind = (rc == 0 && root != NULL) ? (int)root->kind : -1;
	if (a != NULL)
		sql_arena_destroy(a);
	return rc;
}

/* 1 if the first column of CREATE TABLE sql has the declared type want. */
static int
coltype_is(const char *sql, const char *want)
{
	struct sql_arena *a = NULL;
	struct sql_stmt *root = NULL;
	const char *err = NULL;
	const sql_coldef_t *c;
	int ok;

	if (sql_parse_ast(sql, strlen(sql), &a, &root, &err) != 0 ||
	    root == NULL)
		return 0;
	c = root->kind == SQL_KIND_CREATE ? root->u.create->cols : NULL;
	ok = c != NULL && c->type.len == strlen(want) &&
	    memcmp(c->type.p, want, c->type.len) == 0 &&
	    c->type.p >= sql && c->type.p + c->type.len <= sql + strlen(sql);
	sql_arena_destroy(a);
	return ok;
}

/* 1 if the first select item of sql has a source span, and it is
 * exactly `want` (a slice of sql, not of a dequoted copy). */
static int
span_is(const char *sql, const char *want)
{
	struct sql_arena *a = NULL;
	struct sql_stmt *root = NULL;
	const char *err = NULL, *p = NULL;
	size_t n = strlen(sql);
	int len = 0, ok;

	if (sql_parse_ast(sql, n, &a, &root, &err) != 0 || root == NULL)
		return 0;
	ok = root->kind == SQL_KIND_SELECT &&
	    sql_expr_span(root->u.select->cols->head->expr, &p, &len) &&
	    p >= sql && p + len <= sql + n && (size_t)len == strlen(want) &&
	    memcmp(p, want, (size_t)len) == 0;
	sql_arena_destroy(a);
	return ok;
}

int
main(void)
{
	sql_info_t i;
	int k, d, bad = 0;

	/* SELECT */
	CHECK(parse("SELECT 1", &i) == 0 && i.kind == SQL_KIND_SELECT &&
	      i.readonly, "select_const");
	CHECK(parse("SELECT 1+1", &i) == 0 && i.kind == SQL_KIND_SELECT,
	      "select_expr");
	CHECK(parse("SELECT * FROM t", &i) == 0 && i.kind == SQL_KIND_SELECT,
	      "select_star");
	CHECK(parse("SELECT a, b FROM t WHERE a=1", &i) == 0 &&
	      i.kind == SQL_KIND_SELECT, "select_where");
	CHECK(parse("SELECT a FROM t ORDER BY a DESC LIMIT 10", &i) == 0,
	      "select_order_limit");
	CHECK(parse("SELECT DISTINCT a FROM t", &i) == 0, "select_distinct");
	CHECK(parse("SELECT a FROM t GROUP BY a HAVING COUNT(*)>1", &i) == 0,
	      "select_group");
	CHECK(parse("SELECT a FROM t1 JOIN t2 ON t1.id=t2.id", &i) == 0,
	      "select_join");
	CHECK(parse("SELECT a FROM t1 LEFT OUTER JOIN t2 USING(id)", &i) == 0,
	      "select_left_join");
	CHECK(parse("WITH x AS (SELECT 1) SELECT * FROM x", &i) == 0 &&
	      i.kind == SQL_KIND_SELECT, "with_cte");
	CHECK(parse("SELECT a FROM t UNION SELECT b FROM u", &i) == 0,
	      "select_union");

	/* INSERT */
	CHECK(parse("INSERT INTO t VALUES(1,2,3)", &i) == 0 &&
	      i.kind == SQL_KIND_INSERT && !i.readonly, "insert_simple");
	CHECK(parse("INSERT INTO t(a,b) VALUES(1,2)", &i) == 0 &&
	      i.kind == SQL_KIND_INSERT, "insert_cols");
	CHECK(parse("INSERT INTO t VALUES(1),(2),(3)", &i) == 0 &&
	      i.kind == SQL_KIND_INSERT, "insert_bulk");
	CHECK(parse("INSERT INTO t SELECT * FROM u", &i) == 0 &&
	      i.kind == SQL_KIND_INSERT, "insert_select");
	CHECK(parse("REPLACE INTO t VALUES(1,2)", &i) == 0 &&
	      i.kind == SQL_KIND_INSERT, "replace_into");

	/* UPDATE */
	CHECK(parse("UPDATE t SET a=1 WHERE id=2", &i) == 0 &&
	      i.kind == SQL_KIND_UPDATE && !i.readonly, "update_basic");
	CHECK(parse("UPDATE t SET a=1, b=2", &i) == 0 &&
	      i.kind == SQL_KIND_UPDATE, "update_multi");

	/* DELETE */
	CHECK(parse("DELETE FROM t", &i) == 0 &&
	      i.kind == SQL_KIND_DELETE, "delete_all");
	CHECK(parse("DELETE FROM t WHERE id<100", &i) == 0 &&
	      i.kind == SQL_KIND_DELETE, "delete_where");

	/* CREATE / DROP */
	CHECK(parse("CREATE TABLE t(a INTEGER, b TEXT)", &i) == 0 &&
	      i.kind == SQL_KIND_CREATE, "create_table");
	CHECK(parse("CREATE TABLE IF NOT EXISTS t(id INT PRIMARY KEY)", &i) == 0,
	      "create_table_if_ne");
	CHECK(parse("CREATE INDEX i ON t(a)", &i) == 0 &&
	      i.kind == SQL_KIND_CREATE, "create_index");
	CHECK(parse("CREATE VIEW v AS SELECT * FROM t", &i) == 0 &&
	      i.kind == SQL_KIND_CREATE, "create_view");
	CHECK(parse("DROP TABLE t", &i) == 0 && i.kind == SQL_KIND_DROP,
	      "drop_table");
	CHECK(parse("DROP TABLE IF EXISTS t", &i) == 0 &&
	      i.kind == SQL_KIND_DROP, "drop_if_exists");

	/* PRAGMA */
	CHECK(parse("PRAGMA journal_mode", &i) == 0 &&
	      i.kind == SQL_KIND_PRAGMA && i.readonly, "pragma_query");
	CHECK(parse("PRAGMA journal_mode = WAL", &i) == 0 &&
	      i.kind == SQL_KIND_PRAGMA, "pragma_set");

	/* TX */
	CHECK(parse("BEGIN", &i) == 0 && i.kind == SQL_KIND_BEGIN,
	      "begin");
	CHECK(parse("BEGIN TRANSACTION", &i) == 0 &&
	      i.kind == SQL_KIND_BEGIN, "begin_tx");
	CHECK(parse("COMMIT", &i) == 0 && i.kind == SQL_KIND_COMMIT,
	      "commit");
	CHECK(parse("END", &i) == 0 && i.kind == SQL_KIND_COMMIT,
	      "end_as_commit");
	CHECK(parse("ROLLBACK", &i) == 0 && i.kind == SQL_KIND_ROLLBACK,
	      "rollback");

	/* ATTACH/DETACH */
	CHECK(parse("ATTACH DATABASE 'x.db' AS x", &i) == 0 &&
	      i.kind == SQL_KIND_ATTACH, "attach");
	CHECK(parse("DETACH DATABASE x", &i) == 0 &&
	      i.kind == SQL_KIND_DETACH, "detach");

	/* EXPLAIN */
	CHECK(parse("EXPLAIN SELECT 1", &i) == 0 &&
	      i.kind == SQL_KIND_EXPLAIN && i.readonly, "explain");
	CHECK(parse("EXPLAIN QUERY PLAN SELECT * FROM t", &i) == 0 &&
	      i.kind == SQL_KIND_EXPLAIN, "explain_qp");

	/* Comments */
	CHECK(parse("-- a comment\nSELECT 1", &i) == 0 &&
	      i.kind == SQL_KIND_SELECT, "line_comment");
	CHECK(parse("/* block */ SELECT 1", &i) == 0 &&
	      i.kind == SQL_KIND_SELECT, "block_comment");

	/* Errors */
	CHECK(parse("", &i) < 0, "empty");
	CHECK(parse("XYZZY foo bar", &i) < 0, "unknown_keyword");

	/* Parser stack overflow is an error.  It used to empty the stack and
	 * carry on: 98 '(' then "DELETE FROM t" parsed as a DELETE. */
	for (d = 90; d < 140; d++)
		if (parse_nested(d, "DELETE FROM t", &k) == 0)
			bad++;
	CHECK(bad == 0, "stack_overflow_is_error");
	CHECK(parse_nested(40, "1" "))))))))))))))))))))))))))))))))))))))))",
	    &k) == 0 && k == SQL_KIND_SELECT, "nested_40_ok");

	/* A span is a slice of the SQL text even when a quoted identifier
	 * has a doubled quote (its value is then an arena copy): the span
	 * of "a""b" + 1 used to start in the arena and end in the text. */
	CHECK(span_is("SELECT \"a\"\"b\" + 1", "\"a\"\"b\" + 1"),
	      "span_doubled_quote");
	CHECK(span_is("SELECT 1 + \"a\"\"b\"(2)", "1 + \"a\"\"b\"(2)"),
	      "span_doubled_quote_func");
	CHECK(span_is("SELECT \"a\" + 1", "\"a\" + 1"), "span_quoted_id");
	CHECK(coltype_is("CREATE TABLE t(a \"x\"\"y\" INT)",
	      "\"x\"\"y\" INT"), "type_name_doubled_quote");
	CHECK(coltype_is("CREATE TABLE t(a UNSIGNED BIG INT)",
	      "UNSIGNED BIG INT"), "type_name_words");

	/* Trailing semicolon tolerated */
	CHECK(parse("SELECT 1;", &i) == 0 && i.kind == SQL_KIND_SELECT,
	      "trailing_semi");

	printf("\nSQL parser tests: %d passed, %d failed\n", n_pass, n_fail);
	return n_fail ? 1 : 0;
}
