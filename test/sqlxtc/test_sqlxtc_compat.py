#!/usr/bin/env python3
# Copyright (c) 2026, The XTC Project
# Use of this source code is governed by the ISC License.
#
# test/sqlxtc/test_sqlxtc_compat.py
#
# Wired-in (make check + CI) SQL-compatibility differential test for
# the sqlxtc engine.  Each case runs the SAME SQL through BOTH sqlxtc
# (via the Quack JSON-over-TCP protocol) and Python's stdlib sqlite3,
# and asserts identical result sets (order-insensitive, type-normalised).
#
# This is a SQL-DIALECT compatibility measurement, NOT a run of
# SQLite's testfixture TCL suite -- sqlxtc is a from-scratch engine
# that speaks a wire protocol, not SQLite's C API, so that suite does
# not apply to it (see harvest_sqlite_tests.py for the corpus-scale
# opt-in measurement against ~/src/sqlite).
#
# This corpus is self-contained (no ~/src/sqlite needed) and covers a
# broad slice of the feature matrix that sqlxtc IS expected to handle:
# DDL, DML, SELECT shapes, joins, subqueries, aggregates, expressions,
# NULL / affinity semantics, and transactions.  Cases exercising
# features sqlxtc does not yet implement live in XFAIL_CORPUS below and
# are reported but not counted as failures (they document the gap).

import os
import sqlite3
import sys

sys.path.insert(0, os.path.abspath(os.path.dirname(__file__)))
from compat_common import Quack, normalise_rows, Server  # noqa: E402

# Each entry: (desc, [setup_sql...], test_sql[, opts]).  Both engines get
# the setup then the test; result sets must match (or both must error).
# opts (optional dict): "ordered": True compares rows in ORDER (default is
# an order-insensitive multiset compare, which cannot see a wrong ORDER BY
# or LIMIT/OFFSET window); "params": [...] binds the test statement's
# ? / ?NNN / :name markers (sent as Quack "params", positional).
CORPUS = [
    # ---- DDL ----
    ("ddl_create", [], "CREATE TABLE t(a INT, b TEXT)"),
    ("ddl_create_pk", [], "CREATE TABLE t(id INTEGER PRIMARY KEY, n TEXT)"),
    ("ddl_create_check", [], "CREATE TABLE t(x INT CHECK(x > 0))"),
    ("ddl_create_notnull", [], "CREATE TABLE t(a INT NOT NULL, b TEXT)"),
    ("ddl_create_default", [], "CREATE TABLE t(a INT DEFAULT 7, b TEXT)"),
    ("ddl_drop", ["CREATE TABLE d(x INT)"], "DROP TABLE d"),
    ("ddl_drop_ifexists", [], "DROP TABLE IF EXISTS nope"),
    ("ddl_create_index",
     ["CREATE TABLE t(k INT, v TEXT)",
      "INSERT INTO t VALUES(1,'a'),(2,'b'),(3,'c')",
      "CREATE INDEX ti ON t(k)"],
     "SELECT v FROM t WHERE k=2"),
    ("ddl_create_unique_index",
     ["CREATE TABLE t(k INT, v TEXT)",
      "INSERT INTO t VALUES(1,'a'),(2,'b')",
      "CREATE UNIQUE INDEX tiu ON t(k)"],
     "SELECT v FROM t WHERE k=1"),
    ("ddl_view",
     ["CREATE TABLE t(x INT)",
      "INSERT INTO t VALUES(10),(20),(30)",
      "CREATE VIEW v AS SELECT x*2 AS y FROM t"],
     "SELECT y FROM v ORDER BY y"),

    # ---- DML ----
    ("dml_insert_multirow",
     ["CREATE TABLE t(a INT, b TEXT)",
      "INSERT INTO t VALUES (1,'one'),(2,'two'),(3,'three')"],
     "SELECT * FROM t ORDER BY a"),
    ("dml_insert_cols",
     ["CREATE TABLE t(a INT, b TEXT, c INT)",
      "INSERT INTO t(a,c) VALUES(1,3),(2,6)"],
     "SELECT a, b, c FROM t ORDER BY a"),
    ("dml_update_all",
     ["CREATE TABLE t(id INT, v INT)",
      "INSERT INTO t VALUES(1,10),(2,20),(3,30)",
      "UPDATE t SET v = v + 1"],
     "SELECT * FROM t ORDER BY id"),
    ("dml_update_where",
     ["CREATE TABLE t(id INT, v INT)",
      "INSERT INTO t VALUES(1,10),(2,20),(3,30)",
      "UPDATE t SET v = v * 2 WHERE id > 1"],
     "SELECT * FROM t ORDER BY id"),
    ("dml_delete_where",
     ["CREATE TABLE t(a INT)",
      "INSERT INTO t VALUES(1),(2),(3),(4)",
      "DELETE FROM t WHERE a % 2 = 0"],
     "SELECT * FROM t ORDER BY a"),
    ("dml_delete_all",
     ["CREATE TABLE t(a INT)",
      "INSERT INTO t VALUES(1),(2),(3)",
      "DELETE FROM t"],
     "SELECT COUNT(*) FROM t"),

    # ---- WHERE predicates ----
    ("where_and_or",
     ["CREATE TABLE t(a INT, b INT)",
      "INSERT INTO t VALUES(1,1),(2,4),(3,9),(4,16),(5,25)"],
     "SELECT a FROM t WHERE (a > 1 AND a < 5) OR b = 25 ORDER BY a"),
    ("where_not",
     ["CREATE TABLE t(a INT)",
      "INSERT INTO t VALUES(1),(2),(3),(4),(5)"],
     "SELECT a FROM t WHERE NOT (a > 3) ORDER BY a"),
    ("where_between",
     ["CREATE TABLE t(a INT)",
      "INSERT INTO t VALUES(1),(2),(3),(4),(5),(6)"],
     "SELECT a FROM t WHERE a BETWEEN 2 AND 5 ORDER BY a"),
    ("where_in_list",
     ["CREATE TABLE t(a INT)",
      "INSERT INTO t VALUES(1),(2),(3),(4),(5)"],
     "SELECT a FROM t WHERE a IN (2,4) ORDER BY a"),
    ("where_not_in",
     ["CREATE TABLE t(a INT)",
      "INSERT INTO t VALUES(1),(2),(3),(4)"],
     "SELECT a FROM t WHERE a NOT IN (2,3) ORDER BY a"),
    ("where_like",
     ["CREATE TABLE t(s TEXT)",
      "INSERT INTO t VALUES('apple'),('apricot'),('banana'),('cherry')"],
     "SELECT s FROM t WHERE s LIKE 'ap%' ORDER BY s"),
    ("where_like_underscore",
     ["CREATE TABLE t(s TEXT)",
      "INSERT INTO t VALUES('cat'),('cot'),('cut'),('cart')"],
     "SELECT s FROM t WHERE s LIKE 'c_t' ORDER BY s"),
    ("where_is_null",
     ["CREATE TABLE t(a INT, b INT)",
      "INSERT INTO t VALUES(1,NULL),(2,5),(NULL,6)"],
     "SELECT a FROM t WHERE b IS NULL"),
    ("where_is_not_null",
     ["CREATE TABLE t(a INT, b INT)",
      "INSERT INTO t VALUES(1,NULL),(2,5),(NULL,6)"],
     "SELECT a FROM t WHERE a IS NOT NULL ORDER BY a"),
    ("where_cmp_ops",
     ["CREATE TABLE t(a INT)",
      "INSERT INTO t VALUES(1),(2),(3),(4),(5)"],
     "SELECT a FROM t WHERE a >= 2 AND a <= 4 AND a <> 3 ORDER BY a"),

    # ---- ORDER BY / LIMIT / DISTINCT ----
    ("order_asc", ["CREATE TABLE t(x INT)",
                   "INSERT INTO t VALUES(3),(1),(2)"],
     "SELECT x FROM t ORDER BY x ASC"),
    ("order_desc", ["CREATE TABLE t(x INT)",
                    "INSERT INTO t VALUES(3),(1),(2)"],
     "SELECT x FROM t ORDER BY x DESC"),
    ("order_multi",
     ["CREATE TABLE t(a INT, b INT)",
      "INSERT INTO t VALUES(1,2),(1,1),(2,1),(2,2)"],
     "SELECT a, b FROM t ORDER BY a ASC, b DESC"),
    ("limit", ["CREATE TABLE t(x INT)",
               "INSERT INTO t VALUES(1),(2),(3),(4),(5)"],
     "SELECT x FROM t ORDER BY x LIMIT 3"),
    ("limit_offset",
     ["CREATE TABLE t(x INT)",
      "INSERT INTO t VALUES(1),(2),(3),(4),(5)"],
     "SELECT x FROM t ORDER BY x LIMIT 2 OFFSET 2"),
    ("distinct",
     ["CREATE TABLE t(v INT)",
      "INSERT INTO t VALUES(1),(2),(2),(3),(3),(3)"],
     "SELECT DISTINCT v FROM t ORDER BY v"),

    # ---- Joins ----
    ("join_inner",
     ["CREATE TABLE a(id INT, n TEXT)", "CREATE TABLE b(id INT, v INT)",
      "INSERT INTO a VALUES(1,'x'),(2,'y'),(3,'z')",
      "INSERT INTO b VALUES(1,10),(2,20),(4,40)"],
     "SELECT a.n, b.v FROM a JOIN b ON a.id=b.id ORDER BY a.id"),
    ("join_left",
     ["CREATE TABLE a(id INT)", "CREATE TABLE b(id INT, v INT)",
      "INSERT INTO a VALUES(1),(2),(3)",
      "INSERT INTO b VALUES(1,10),(3,30)"],
     "SELECT a.id, b.v FROM a LEFT JOIN b ON a.id=b.id ORDER BY a.id"),
    # ---- Subqueries ----
    ("subq_in",
     ["CREATE TABLE a(x INT)", "CREATE TABLE b(y INT)",
      "INSERT INTO a VALUES(1),(2),(3),(4)",
      "INSERT INTO b VALUES(2),(4)"],
     "SELECT x FROM a WHERE x IN (SELECT y FROM b) ORDER BY x"),
    ("subq_scalar",
     ["CREATE TABLE t(v INT)", "INSERT INTO t VALUES(1),(2),(3)"],
     "SELECT v, (SELECT MAX(v) FROM t) FROM t ORDER BY v"),
    ("subq_not_in",
     ["CREATE TABLE a(x INT)", "CREATE TABLE b(y INT)",
      "INSERT INTO a VALUES(1),(2),(3),(4)",
      "INSERT INTO b VALUES(2),(4)"],
     "SELECT x FROM a WHERE x NOT IN (SELECT y FROM b) ORDER BY x"),

    # ---- Aggregates ----
    ("agg_count_star",
     ["CREATE TABLE t(x INT)", "INSERT INTO t VALUES(1),(2),(3)"],
     "SELECT COUNT(*) FROM t"),
    ("agg_all",
     ["CREATE TABLE t(x INT)",
      "INSERT INTO t VALUES(10),(20),(30),(40)"],
     "SELECT COUNT(*), SUM(x), AVG(x), MIN(x), MAX(x) FROM t"),
    ("agg_group_by",
     ["CREATE TABLE t(c TEXT, v INT)",
      "INSERT INTO t VALUES('a',1),('a',2),('b',3),('b',4),('c',5)"],
     "SELECT c, SUM(v) FROM t GROUP BY c ORDER BY c"),
    ("agg_having",
     ["CREATE TABLE t(g INT, n INT)",
      "INSERT INTO t VALUES(1,1),(1,2),(2,3),(2,4),(3,5)"],
     "SELECT g, COUNT(*) FROM t GROUP BY g HAVING COUNT(*) > 1 ORDER BY g"),
    ("agg_count_null",
     ["CREATE TABLE t(x INT)",
      "INSERT INTO t VALUES(1),(NULL),(3),(NULL)"],
     "SELECT COUNT(*), COUNT(x) FROM t"),

    # ---- Expressions / functions ----
    ("expr_arith", [], "SELECT 2 + 3 * 4 - 1"),
    ("expr_paren", [], "SELECT (2 + 3) * 4"),
    ("expr_mod", [], "SELECT 17 % 5"),
    ("expr_concat", [], "SELECT 'foo' || 'bar'"),
    ("expr_coalesce",
     ["CREATE TABLE t(x INT)",
      "INSERT INTO t VALUES(1),(NULL),(3)"],
     "SELECT COALESCE(x,-1) FROM t ORDER BY COALESCE(x,-1)"),
    ("expr_ifnull",
     ["CREATE TABLE t(x INT)", "INSERT INTO t VALUES(1),(NULL)"],
     "SELECT IFNULL(x,0) FROM t ORDER BY IFNULL(x,0)"),
    ("expr_abs", [], "SELECT ABS(-7), ABS(7), ABS(0)"),
    ("expr_length", [], "SELECT LENGTH('hello')"),
    ("expr_upper_lower", [], "SELECT UPPER('aBc'), LOWER('aBc')"),
    ("expr_typeof", [], "SELECT typeof(1), typeof(1.5), typeof('x'), typeof(NULL)"),

    # ---- NULL semantics ----
    ("null_compare",
     ["CREATE TABLE t(a INT, b INT)",
      "INSERT INTO t VALUES(1,1),(2,NULL),(NULL,3)"],
     "SELECT a, b FROM t WHERE a IS NULL OR b IS NULL "
     "ORDER BY COALESCE(a,0), COALESCE(b,0)"),

    # ---- Type affinity edges ----
    ("affinity_text_col",
     ["CREATE TABLE t(a TEXT)",
      "INSERT INTO t VALUES(42),('43'),('cat')"],
     "SELECT a, typeof(a) FROM t ORDER BY a"),
    ("affinity_numeric_leading_space",
     ["CREATE TABLE t(x, y NUMERIC)",
      "INSERT INTO t VALUES(1,'  5')"],
     "SELECT x, typeof(y), y FROM t"),
    ("affinity_numeric_trailing_space",   # regression: the bug found here
     ["CREATE TABLE t(x, y NUMERIC)",
      "INSERT INTO t VALUES(1,'5  '),(2,'2.5  ')"],
     "SELECT x, typeof(y) FROM t ORDER BY x"),

    # ---- Compound queries ----
    ("union_all",
     ["CREATE TABLE a(x INT)", "CREATE TABLE b(x INT)",
      "INSERT INTO a VALUES(1),(2)", "INSERT INTO b VALUES(2),(3)"],
     "SELECT x FROM a UNION ALL SELECT x FROM b ORDER BY x"),
    ("union_distinct",
     ["CREATE TABLE a(x INT)", "CREATE TABLE b(x INT)",
      "INSERT INTO a VALUES(1),(2),(3)", "INSERT INTO b VALUES(2),(3),(4)"],
     "SELECT x FROM a UNION SELECT x FROM b ORDER BY x"),
    ("intersect",
     ["CREATE TABLE a(x INT)", "CREATE TABLE b(x INT)",
      "INSERT INTO a VALUES(1),(2),(3),(4)", "INSERT INTO b VALUES(3),(4),(5)"],
     "SELECT x FROM a INTERSECT SELECT x FROM b ORDER BY x"),
    ("except",
     ["CREATE TABLE a(x INT)", "CREATE TABLE b(x INT)",
      "INSERT INTO a VALUES(1),(2),(3),(4)", "INSERT INTO b VALUES(3),(4)"],
     "SELECT x FROM a EXCEPT SELECT x FROM b ORDER BY x"),

    # ---- Transactions ----
    ("txn_commit",
     ["CREATE TABLE t(x INT)", "BEGIN",
      "INSERT INTO t VALUES(1),(2),(3)", "COMMIT"],
     "SELECT COUNT(*) FROM t"),

    # ---- PRAGMA (read-only introspection sqlxtc supports) ----
    ("pragma_table_info",
     ["CREATE TABLE t(a INT, b TEXT, c REAL)"],
     "PRAGMA table_info(t)"),

    ("order_by_expr",
     ["CREATE TABLE t(x INT)", "INSERT INTO t VALUES(1),(2),(3)"],
     "SELECT x FROM t ORDER BY -x"),
    ("insert_select",
     ["CREATE TABLE a(x INT)", "CREATE TABLE b(x INT)",
      "INSERT INTO a VALUES(1),(2),(3)",
      "INSERT INTO b SELECT x FROM a"],
     "SELECT x FROM b ORDER BY x"),
    ("case_expr",
     ["CREATE TABLE t(x INT)", "INSERT INTO t VALUES(1),(2),(3)"],
     "SELECT CASE WHEN x > 1 THEN 'hi' ELSE 'lo' END FROM t ORDER BY x"),

    # ---- Errors: both must reject ----
    ("err_no_table", [], "SELECT * FROM does_not_exist"),
    ("err_syntax", [], "SELECT FROM"),

    # ---- Volume ----
    ("volume_100",
     ["CREATE TABLE t(n INT)"]
     + ["INSERT INTO t VALUES(%d)" % i for i in range(100)],
     "SELECT COUNT(*), SUM(n), MIN(n), MAX(n) FROM t"),

    # ---- Features that landed in the native executor (formerly XFAIL) ----
    ("null_arith_no_from", [], "SELECT 1 + NULL, NULL * 5"),
    ("three_table_join",
     ["CREATE TABLE a(id INT)", "CREATE TABLE b(id INT, c INT)",
      "CREATE TABLE c(id INT, v INT)",
      "INSERT INTO a VALUES(1),(2)",
      "INSERT INTO b VALUES(1,1),(2,2)",
      "INSERT INTO c VALUES(1,100),(2,200)"],
     "SELECT a.id, cc.v FROM a JOIN b ON a.id=b.id "
     "JOIN c AS cc ON b.c=cc.id ORDER BY a.id"),
    ("three_table_join_same_alias",
     ["CREATE TABLE a(id INT)", "CREATE TABLE b(id INT, c INT)",
      "CREATE TABLE c(id INT, v INT)",
      "INSERT INTO a VALUES(1),(2)",
      "INSERT INTO b VALUES(1,1),(2,2)",
      "INSERT INTO c VALUES(1,100),(2,200)"],
     "SELECT a.id, c.v FROM a JOIN b ON a.id=b.id "
     "JOIN c ON b.c=c.id ORDER BY a.id"),
    ("comma_join",
     ["CREATE TABLE a(x INT)", "CREATE TABLE b(y INT)",
      "INSERT INTO a VALUES(1),(2)", "INSERT INTO b VALUES(3),(4)"],
     "SELECT a.x, b.y FROM a, b ORDER BY a.x, b.y"),
    ("select_star_star",
     ["CREATE TABLE t(a INT, b INT)", "INSERT INTO t VALUES(1,2)"],
     "SELECT *, * FROM t"),
    ("agg_plus_const",
     ["CREATE TABLE t(x INT)", "INSERT INTO t VALUES(1),(2),(3)"],
     "SELECT SUM(x) + 1 FROM t"),
    ("min_two_args",
     ["CREATE TABLE t(a INT, b INT)", "INSERT INTO t VALUES(3,5)"],
     "SELECT MIN(a,b), MAX(a,b) FROM t"),
    ("order_by_alias",
     ["CREATE TABLE t(x INT)", "INSERT INTO t VALUES(3),(1),(2)"],
     "SELECT x AS q FROM t ORDER BY q"),
    ("cast_expr",
     [], "SELECT CAST('42' AS INTEGER), CAST(3.9 AS INTEGER)"),
    ("substr",
     [], "SELECT substr('hello', 2, 3)"),
    ("nullif",
     [], "SELECT nullif(5, 5), nullif(5, 6)"),
    ("group_concat",
     ["CREATE TABLE t(x TEXT)", "INSERT INTO t VALUES('a'),('b'),('c')"],
     "SELECT group_concat(x) FROM t"),
    ("exists",
     ["CREATE TABLE a(x INT)", "CREATE TABLE b(y INT)",
      "INSERT INTO a VALUES(1),(2)", "INSERT INTO b VALUES(2)"],
     "SELECT x FROM a WHERE EXISTS(SELECT 1 FROM b WHERE y=x) ORDER BY x"),
    ("cte",
     [], "WITH c(n) AS (SELECT 1 UNION ALL SELECT 2) SELECT n FROM c ORDER BY n"),
    # ---- sq158 lexfix: wrong-result + parse gaps (audit gaps 9/10/13/14) ----
    # LIMIT a, b means OFFSET a LIMIT b (SQLite parse.y:972); ordered so a
    # swapped window is visible.
    ("lex_limit_comma",
     ["CREATE TABLE t(x INT)", "INSERT INTO t VALUES(1),(2),(3),(4),(5)"],
     "SELECT x FROM t ORDER BY x LIMIT 1, 2", {"ordered": True}),
    ("lex_limit_offset_ordered",
     ["CREATE TABLE t(x INT)", "INSERT INTO t VALUES(1),(2),(3),(4),(5)"],
     "SELECT x FROM t ORDER BY x DESC LIMIT 2 OFFSET 1", {"ordered": True}),
    ("lex_offset_without_limit", [], "SELECT 1 OFFSET 1"),   # both reject
    # '' inside a string literal is one quote -- in results, stored data,
    # and comparisons.
    ("lex_quote_collapse", [], "SELECT 'it''s', '''', 'a''''b'"),
    ("lex_quote_stored",
     ["CREATE TABLE t(s TEXT)", "INSERT INTO t VALUES('O''Brien')"],
     "SELECT s, length(s) FROM t"),
    ("lex_quote_where",
     ["CREATE TABLE t(s TEXT)",
      "INSERT INTO t VALUES('O''Brien'),('OBrien')"],
     "SELECT count(*) FROM t WHERE s = 'O''Brien'"),
    ("lex_quote_update",
     ["CREATE TABLE t(id INTEGER PRIMARY KEY, s TEXT)",
      "INSERT INTO t VALUES(1,'x')", "UPDATE t SET s = 'don''t' WHERE id = 1"],
     "SELECT s FROM t"),
    # Hex integer literals (0x / 0X, 64-bit two's complement).
    ("lex_hex", [], "SELECT 0x10, 0XFF, 0x7fffffffffffffff, 0xffffffffffffffff"),
    ("lex_hex_arith", [], "SELECT 0x10 + 1, -0x1F, 0xa * 2"),
    ("lex_hex_too_big", [], "SELECT 0x1ffffffffffffffff"),   # both reject
    ("lex_hex_insert",
     ["CREATE TABLE t(x INT)", "INSERT INTO t VALUES(0x20)"],
     "SELECT x FROM t WHERE x = 0x20"),
    # NOT LIKE / LIKE ... ESCAPE / NOT BETWEEN.
    ("expr_not_like",
     ["CREATE TABLE t(s TEXT)",
      "INSERT INTO t VALUES('apple'),('apricot'),('banana'),('cherry')"],
     "SELECT s FROM t WHERE s NOT LIKE 'ap%'"),
    ("expr_like_escape",
     ["CREATE TABLE t(s TEXT)",
      "INSERT INTO t VALUES('10%'),('100'),('10_x'),('10%x')"],
     "SELECT s FROM t WHERE s LIKE '10!%%' ESCAPE '!'"),
    ("expr_like_escape_underscore",
     ["CREATE TABLE t(s TEXT)",
      "INSERT INTO t VALUES('a_c'),('abc'),('a!c')"],
     "SELECT s, s LIKE 'a!_c' ESCAPE '!', s NOT LIKE 'a!!c' ESCAPE '!' FROM t"),
    ("expr_not_between",
     ["CREATE TABLE t(a INT)",
      "INSERT INTO t VALUES(1),(2),(3),(4),(5),(NULL)"],
     "SELECT a FROM t WHERE a NOT BETWEEN 2 AND 4"),
    # IS / IS NOT / IS [NOT] DISTINCT FROM (never NULL).
    ("expr_is",
     ["CREATE TABLE t(a INT, b INT)",
      "INSERT INTO t VALUES(1,1),(1,2),(NULL,1),(NULL,NULL),(2,NULL)"],
     "SELECT a, b, a IS b, a IS NOT b FROM t"),
    ("expr_is_where",
     ["CREATE TABLE t(a INT, b INT)",
      "INSERT INTO t VALUES(1,1),(1,2),(NULL,1),(NULL,NULL)"],
     "SELECT count(*) FROM t WHERE a IS b"),
    ("expr_is_distinct",
     ["CREATE TABLE t(a INT, b INT)",
      "INSERT INTO t VALUES(1,1),(1,2),(NULL,1),(NULL,NULL)"],
     "SELECT a IS DISTINCT FROM b, a IS NOT DISTINCT FROM b FROM t"),
    ("expr_is_null_forms",
     ["CREATE TABLE t(a INT)", "INSERT INTO t VALUES(1),(NULL)"],
     "SELECT a IS NULL, a IS NOT NULL, NULL IS NULL FROM t"),
    # ORDER BY ... NULLS FIRST / NULLS LAST (ordered; NULL rows are equal).
    ("order_nulls_last",
     ["CREATE TABLE t(a INT)", "INSERT INTO t VALUES(3),(NULL),(1),(NULL),(2)"],
     "SELECT a FROM t ORDER BY a NULLS LAST", {"ordered": True}),
    ("order_desc_nulls_first",
     ["CREATE TABLE t(a INT)", "INSERT INTO t VALUES(3),(NULL),(1),(NULL),(2)"],
     "SELECT a FROM t ORDER BY a DESC NULLS FIRST", {"ordered": True}),
    ("order_nulls_last_limit",
     ["CREATE TABLE t(a INT)", "INSERT INTO t VALUES(3),(NULL),(1),(NULL),(2)"],
     "SELECT a FROM t ORDER BY a ASC NULLS LAST LIMIT 4", {"ordered": True}),
]

# Features sqlxtc does not yet implement.  Reported, not failed -- these
# document the SQL surface still to build (measured at scale by the
# harvester).  Each still runs both engines: if sqlxtc SILENTLY produced
# a wrong result instead of rejecting, that would show up as XPASS-wrong.
# (All previously-listed gaps now land in the native executor and have
# been promoted into CORPUS above; this set is intentionally empty.)
XFAIL_CORPUS = [
]


def compare(qk, ref, setup, test, opts=None):
    """Run setup+test on both engines; return (ok, qrows, qerr, rrows, rerr)."""
    opts = opts or {}
    ordered = bool(opts.get("ordered"))
    params = opts.get("params")
    for s in setup:
        qk.query(s)
        try:
            ref.execute(s)
        except sqlite3.Error:
            pass
    qcols, qrows, qerr = qk.query(test, params)
    rerr = None
    rrows = []
    try:
        if params is not None:
            rrows = ref.execute(test, tuple(params)).fetchall()
        else:
            rrows = ref.execute(test).fetchall()
    except sqlite3.Error as e:
        rerr = str(e)
    if (qerr is None) != (rerr is None):
        return False, qrows, qerr, rrows, rerr
    if qerr is None and (normalise_rows(qrows, ordered) !=
                         normalise_rows(rrows, ordered)):
        return False, qrows, qerr, rrows, rerr
    return True, qrows, qerr, rrows, rerr


def main():
    server = Server(port=16456)
    if not server.available():
        print("FAIL: sqlxtc-server not built at %s" % server.bin)
        return 1
    server.start()

    rc = 0
    try:
        passed = failed = 0
        for case in CORPUS:
            desc, setup, test = case[:3]
            opts = case[3] if len(case) > 3 else None
            ref = sqlite3.connect(":memory:")
            ref.isolation_level = None
            qk = server.fresh()
            ok, qr, qe, rr, re_ = compare(qk, ref, setup, test, opts)
            ref.close()
            if ok:
                passed += 1
            else:
                failed += 1
                print("  FAIL %s" % desc)
                print("       sql   : %s" % test)
                print("       sqlxtc: rows=%r err=%r" % (qr[:6], qe))
                print("       sqlite: rows=%r err=%r" % (rr[:6], re_))

        # XFAIL: expected-unsupported.  A case "xpasses" if sqlxtc now
        # matches SQLite (a feature landed -- promote it to CORPUS).  A
        # case is a real BUG only if sqlxtc ran and gave a WRONG result
        # (neither an error nor a match) -- that we must flag.
        xfail = xpass = xbug = 0
        for case in XFAIL_CORPUS:
            desc, setup, test = case[:3]
            opts = case[3] if len(case) > 3 else None
            ref = sqlite3.connect(":memory:")
            ref.isolation_level = None
            qk = server.fresh()
            ok, qr, qe, rr, re_ = compare(qk, ref, setup, test, opts)
            ref.close()
            if ok:
                xpass += 1
                print("  xpass %s  (feature now works -- promote to CORPUS)"
                      % desc)
            elif qe is not None:
                xfail += 1        # expected: sqlxtc rejected it
            else:
                xbug += 1         # ran but wrong -- a real correctness bug!
                rc = 1
                print("  XBUG %s  (ran but WRONG -- correctness bug!)" % desc)
                print("       sql   : %s" % test)
                print("       sqlxtc: %r" % (normalise_rows(qr)[:6]))
                print("       sqlite: %r" % (normalise_rows(rr)[:6]))

        print("\nCORPUS   : %d passed, %d failed (of %d)" %
              (passed, failed, len(CORPUS)))
        print("XFAIL set: %d still-unsupported, %d newly-working, %d bugs" %
              (xfail, xpass, xbug))
        if len(CORPUS):
            print("Supported-feature compat: %d/%d = %.1f%%" %
                  (passed, len(CORPUS), 100.0 * passed / len(CORPUS)))
        if failed:
            rc = 1
    finally:
        server.stop()
    return rc


if __name__ == "__main__":
    sys.exit(main())
