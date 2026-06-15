/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_sql_ast.c
 *	Differential parse oracle for the Lime SQL parser.
 *
 *	For each statement in a corpus, parse it two ways: with the Lime
 *	parser (sql_parse_ast) and with the reference engine
 *	(xsql_prepare_v2 against an in-memory database).  Assert the two
 *	AGREE on accept/reject.  This validates that the Lime grammar
 *	covers the SQL the engine accepts -- the prerequisite for ever
 *	making Lime the sole parser -- without yet trusting Lime for
 *	execution.
 *
 *	Acceptable deltas are listed explicitly (the Lime grammar is a
 *	deliberate subset: e.g. it does not implement every PRAGMA name,
 *	CTEs, or window functions).  Each such statement is tagged so the
 *	test asserts the KNOWN relationship rather than strict equality.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sql_parse.h"
#include "sql_ast.h"
#include "sqlite3.h"   /* reference engine; xsql.h (force-included) renames the API */

static int g_fail;
#define CK(c, msg) do { if (!(c)) { \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, (msg)); \
	g_fail = 1; } } while (0)

/* Expected relationship between the two parsers for a corpus entry. */
enum expect {
	BOTH_OK = 0,   /* both accept */
	BOTH_ERR,      /* both reject */
	REF_ONLY       /* reference accepts, Lime rejects (known subset gap) */
};

struct corpus {
	const char  *sql;
	enum expect  exp;
};

/* Reference parse: does xsql_prepare_v2 accept it?  We prepare, not
 * run, so DDL/DML with no data is fine.  A table the test references
 * must exist first (see setup below). */
static int
ref_accepts(xsql *db, const char *sql)
{
	xsql_stmt *st = NULL;
	int rc = xsql_prepare_v2(db, sql, -1, &st, 0);
	if (st) xsql_finalize(st);
	return rc == SQLITE_OK;
}

static int
lime_accepts(const char *sql)
{
	sql_arena_t *arena = NULL;
	sql_stmt_t  *root = NULL;
	const char  *err = NULL;
	int rc = sql_parse_ast(sql, strlen(sql), &arena, &root, &err);
	if (arena) sql_arena_destroy(arena);
	return rc == 0;
}

int
main(void)
{
	xsql *db = NULL;
	char *errmsg = NULL;
	size_t i, n;
	int agree = 0, refonly = 0;

	static const struct corpus corpus[] = {
		/* ---- SELECT ---- */
		{ "SELECT 1", BOTH_OK },
		{ "SELECT * FROM t", BOTH_OK },
		{ "SELECT k, v FROM t WHERE k = 1", BOTH_OK },
		{ "SELECT k, v FROM t WHERE k = ? AND v < ?", BOTH_OK },
		{ "SELECT count(*), sum(k) FROM t", BOTH_OK },
		{ "SELECT DISTINCT k FROM t ORDER BY k DESC LIMIT 10", BOTH_OK },
		{ "SELECT k FROM t GROUP BY k HAVING count(*) > 1", BOTH_OK },
		{ "SELECT a.k FROM t AS a JOIN t AS b ON a.k = b.k", BOTH_OK },
		{ "SELECT k FROM t WHERE k BETWEEN 1 AND 10", BOTH_OK },
		{ "SELECT k FROM t WHERE k IN (1, 2, 3)", BOTH_OK },
		{ "SELECT k FROM t WHERE v IS NULL", BOTH_OK },
		{ "SELECT k FROM t WHERE v IS NOT NULL", BOTH_OK },
		{ "SELECT k FROM t LIMIT 5 OFFSET 2", BOTH_OK },
		{ "SELECT -k, +v, ~k FROM t", BOTH_OK },
		{ "SELECT k*2 + v/3 FROM t", BOTH_OK },
		{ "SELECT CASE WHEN k>0 THEN 'p' ELSE 'n' END FROM t", BOTH_OK },
		{ "SELECT k FROM t UNION SELECT v FROM t", BOTH_OK },
		{ "SELECT k FROM t WHERE k IN (SELECT k FROM t)", BOTH_OK },
		{ "WITH c AS (SELECT 1) SELECT * FROM c", BOTH_OK },
		{ "WITH a AS (SELECT k FROM t), b AS (SELECT v FROM t) SELECT * FROM a", BOTH_OK },
		{ "WITH RECURSIVE r AS (SELECT 1) SELECT * FROM r", BOTH_OK },

		/* ---- INSERT / UPDATE / DELETE ---- */
		{ "INSERT INTO t(k,v) VALUES(1,'a')", BOTH_OK },
		{ "INSERT INTO t(k,v) VALUES(1,'a'),(2,'b')", BOTH_OK },
		{ "INSERT INTO t(k,v) SELECT k,v FROM t", BOTH_OK },
		{ "UPDATE t SET v = 'x' WHERE k = 1", BOTH_OK },
		{ "DELETE FROM t WHERE k = 1", BOTH_OK },

		/* ---- DDL / PRAGMA / TX ---- */
		{ "CREATE TABLE u(a INTEGER PRIMARY KEY, b TEXT NOT NULL)", BOTH_OK },
		{ "CREATE INDEX ix ON t(k)", BOTH_OK },
		{ "DROP TABLE IF EXISTS u", BOTH_OK },
		{ "BEGIN", BOTH_OK },
		{ "COMMIT", BOTH_OK },
		{ "ROLLBACK", BOTH_OK },

		/* ---- both reject (malformed) ---- */
		{ "SELECT FROM", BOTH_ERR },
		{ "SELECT * FROM", BOTH_ERR },
		{ "INSERT INTO t VALUES", BOTH_ERR },
		{ "UPDATE t SET", BOTH_ERR },
		{ "DELETE t WHERE k=1", BOTH_ERR },        /* missing FROM */
		{ "CREATE TABLE u(", BOTH_ERR },
		{ "SELECT k FROM t WHERE", BOTH_ERR },
		{ "SELECT k FROM t WHERE k =", BOTH_ERR },
		{ "GIBBERISH zzz qqq", BOTH_ERR },
	};

	n = sizeof corpus / sizeof corpus[0];

	if (xsql_open(":memory:", &db) != SQLITE_OK) {
		fprintf(stderr, "FAIL: cannot open reference db\n");
		return 1;
	}
	/* Create the tables the corpus references, so the reference parser
	 * resolves names (prepare resolves schema). */
	if (xsql_exec(db, "CREATE TABLE t(k INTEGER, v TEXT)", 0, 0, &errmsg)
	    != SQLITE_OK) {
		fprintf(stderr, "FAIL: setup: %s\n", errmsg ? errmsg : "?");
		xsql_free(errmsg);
		xsql_close(db);
		return 1;
	}

	for (i = 0; i < n; i++) {
		const char *sql = corpus[i].sql;
		int rok = ref_accepts(db, sql);
		int lok = lime_accepts(sql);

		switch (corpus[i].exp) {
		case BOTH_OK:
			CK(rok, "reference should accept");
			CK(lok, sql);   /* Lime should accept too */
			if (rok && lok) agree++;
			break;
		case BOTH_ERR:
			CK(!rok, "reference should reject");
			CK(!lok, sql);   /* Lime should reject too */
			if (!rok && !lok) agree++;
			break;
		case REF_ONLY:
			/* Documented gap: reference accepts, Lime rejects.
			 * Assert the gap still holds (so we notice if Lime
			 * gains coverage or the reference changes). */
			CK(rok, "reference should accept (ref-only case)");
			CK(!lok, "Lime expected to reject (documented gap)");
			if (rok && !lok) refonly++;
			break;
		}
	}

	xsql_close(db);

	if (g_fail) {
		fprintf(stderr, "  DIFFERENTIAL PARSE: divergence detected\n");
		return 1;
	}
	printf("  ok   Lime vs reference parser agree on %d/%zu statements "
	       "(%d documented subset gaps)\n", agree, n, refonly);
	printf("All sqlxtc Lime parser differential tests passed.\n");
	return 0;
}
