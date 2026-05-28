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
 *	examples/sqlxtc/.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../examples/sqlxtc/sql_parse.h"

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

int
main(void)
{
	sql_info_t i;

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

	/* Trailing semicolon tolerated */
	CHECK(parse("SELECT 1;", &i) == 0 && i.kind == SQL_KIND_SELECT,
	      "trailing_semi");

	printf("\nSQL parser tests: %d passed, %d failed\n", n_pass, n_fail);
	return n_fail ? 1 : 0;
}
