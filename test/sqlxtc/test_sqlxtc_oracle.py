#!/usr/bin/env python3
# Copyright (c) 2026, The XTC Project
# Use of this source code is governed by the ISC License.
#
# test/sqlxtc/test_sqlxtc_oracle.py
#
# Differential test: run a corpus of SQL statements through both
# sqlxtc-via-Quack and python's stdlib sqlite3 (which embeds the
# same SQLite library underneath), and assert the result sets
# match.  Validates that the Quack frontend doesn't lose or mangle
# data on the wire.
#
# The corpus is a representative slice of SQL features: DDL, DML,
# SELECT shapes, aggregates, constraints, transactions, pragmas,
# joins, subqueries, NULL handling, type affinity edge cases.

import json
import os
import socket
import sqlite3
import subprocess
import sys
import time

# --- helpers ----------------------------------------------------

class Quack:
    def __init__(self, port, host="127.0.0.1"):
        self.s = socket.create_connection((host, port), timeout=10)
        self.f = self.s.makefile("rwb", buffering=0)
        # Skip hello banner
        self.f.readline()

    def query(self, sql):
        self.f.write((json.dumps({"q": sql}) + "\n").encode())
        cols = None
        rows = []
        err = None
        while True:
            line = self.f.readline()
            if not line:
                break
            o = json.loads(line.decode().strip())
            if "cols" in o:
                cols = o["cols"]
            elif "row" in o:
                rows.append(tuple(o["row"]))
            elif "done" in o:
                return cols, rows, None
            elif "err" in o:
                return cols, rows, o["err"]
        return cols, rows, "(no done)"

    def close(self):
        try:
            self.f.write(b'{"quit":1}\n')
        except Exception:
            pass
        self.s.close()


def normalise_rows(rows):
    """Make Quack vs sqlite3 row sets comparable.  SQLite returns
    bytes for BLOB; Quack returns base64-shaped strings.  Floats may
    differ in formatting."""
    out = []
    for r in rows:
        new = []
        for v in r:
            if isinstance(v, float):
                new.append(("f", round(v, 6)))
            elif isinstance(v, bytes):
                new.append(("s", v.decode("ascii", errors="replace")))
            elif v is None:
                new.append(("n", 0))
            elif isinstance(v, str):
                new.append(("s", v))
            elif isinstance(v, int):
                new.append(("i", v))
            else:
                new.append(("o", repr(v)))
        out.append(tuple(new))
    return sorted(out)


# --- corpus -----------------------------------------------------
#
# Each entry: (description, [setup_sql], test_sql).  All tests
# expected to produce the same result set when run through both
# sqlxtc and reference sqlite3.

CORPUS = [
    # DDL
    ("create_simple",
        [], "CREATE TABLE t1(a INT, b TEXT)"),
    ("create_with_pk",
        [], "CREATE TABLE t2(id INTEGER PRIMARY KEY, name TEXT NOT NULL)"),
    ("create_with_check",
        [], "CREATE TABLE t3(x INT CHECK(x > 0))"),
    ("drop_table",
        ["CREATE TABLE drop_me(x INT)"],
        "DROP TABLE drop_me"),

    # DML basic
    ("insert_select_basic",
        ["CREATE TABLE q1(a INT, b TEXT)",
         "INSERT INTO q1 VALUES (1, 'one'), (2, 'two'), (3, 'three')"],
        "SELECT * FROM q1 ORDER BY a"),
    ("insert_select_filter",
        ["CREATE TABLE q2(a INT)",
         "INSERT INTO q2 VALUES (1),(2),(3),(4),(5),(6)"],
        "SELECT * FROM q2 WHERE a > 2 AND a < 6 ORDER BY a"),
    ("update_simple",
        ["CREATE TABLE q3(id INT, val INT)",
         "INSERT INTO q3 VALUES (1,10),(2,20),(3,30)",
         "UPDATE q3 SET val = val * 2 WHERE id > 1"],
        "SELECT * FROM q3 ORDER BY id"),
    ("delete_simple",
        ["CREATE TABLE q4(a INT)",
         "INSERT INTO q4 VALUES (1),(2),(3)",
         "DELETE FROM q4 WHERE a = 2"],
        "SELECT * FROM q4 ORDER BY a"),

    # SELECT shapes
    ("aggregate_count_sum",
        ["CREATE TABLE agg(x INT)",
         "INSERT INTO agg VALUES (10),(20),(30),(40)"],
        "SELECT COUNT(*), SUM(x), MIN(x), MAX(x) FROM agg"),
    ("group_by",
        ["CREATE TABLE gb(c TEXT, v INT)",
         "INSERT INTO gb VALUES ('a',1),('a',2),('b',3),('b',4),('c',5)"],
        "SELECT c, SUM(v) FROM gb GROUP BY c ORDER BY c"),
    ("having",
        ["CREATE TABLE hv(g INT, n INT)",
         "INSERT INTO hv VALUES (1,1),(1,2),(2,3),(2,4),(3,5)"],
        "SELECT g, COUNT(*) FROM hv GROUP BY g HAVING COUNT(*) > 1 ORDER BY g"),
    ("order_limit",
        ["CREATE TABLE ol(x INT)",
         "INSERT INTO ol VALUES (3),(1),(4),(1),(5),(9),(2),(6),(5),(3)"],
        "SELECT x FROM ol ORDER BY x DESC LIMIT 3"),
    ("distinct",
        ["CREATE TABLE ds(v INT)",
         "INSERT INTO ds VALUES (1),(2),(2),(3),(3),(3)"],
        "SELECT DISTINCT v FROM ds ORDER BY v"),

    # Joins
    ("inner_join",
        ["CREATE TABLE ja(id INT, name TEXT)",
         "CREATE TABLE jb(id INT, val INT)",
         "INSERT INTO ja VALUES (1,'one'),(2,'two'),(3,'three')",
         "INSERT INTO jb VALUES (1,10),(2,20),(4,40)"],
        "SELECT ja.name, jb.val FROM ja JOIN jb ON ja.id = jb.id ORDER BY ja.id"),
    ("left_join",
        ["CREATE TABLE la(id INT)",
         "CREATE TABLE lb(id INT, x INT)",
         "INSERT INTO la VALUES (1),(2),(3)",
         "INSERT INTO lb VALUES (1,10),(3,30)"],
        "SELECT la.id, lb.x FROM la LEFT JOIN lb ON la.id = lb.id ORDER BY la.id"),

    # Subqueries
    ("subquery_in",
        ["CREATE TABLE sa(x INT)",
         "CREATE TABLE sb(y INT)",
         "INSERT INTO sa VALUES (1),(2),(3),(4)",
         "INSERT INTO sb VALUES (2),(4)"],
        "SELECT x FROM sa WHERE x IN (SELECT y FROM sb) ORDER BY x"),
    ("scalar_subquery",
        ["CREATE TABLE ss(v INT)",
         "INSERT INTO ss VALUES (1),(2),(3)"],
        "SELECT v, (SELECT MAX(v) FROM ss) FROM ss ORDER BY v"),

    # NULL
    ("null_compare",
        ["CREATE TABLE nl(a INT, b INT)",
         "INSERT INTO nl VALUES (1,1),(2,NULL),(NULL,3)"],
        "SELECT a, b FROM nl WHERE a IS NULL OR b IS NULL ORDER BY COALESCE(a,0), COALESCE(b,0)"),
    ("coalesce",
        ["CREATE TABLE cl(x INT)",
         "INSERT INTO cl VALUES (1),(NULL),(3),(NULL),(5)"],
        "SELECT COALESCE(x, -1) FROM cl ORDER BY ROWID"),

    # Pragma (read-only)
    ("pragma_table_info",
        ["CREATE TABLE pt(a INT, b TEXT, c REAL)"],
        "PRAGMA table_info(pt)"),

    # Index
    ("create_use_index",
        ["CREATE TABLE ix(k INT, v TEXT)",
         "INSERT INTO ix VALUES (1,'a'),(2,'b'),(3,'c'),(4,'d')",
         "CREATE INDEX ix_k ON ix(k)"],
        "SELECT * FROM ix WHERE k = 3"),

    # View
    ("create_select_view",
        ["CREATE TABLE vt(x INT)",
         "INSERT INTO vt VALUES (10),(20),(30)",
         "CREATE VIEW vv AS SELECT x*2 AS y FROM vt"],
        "SELECT y FROM vv ORDER BY y"),

    # Functions
    ("string_funcs",
        ["CREATE TABLE sf(s TEXT)",
         "INSERT INTO sf VALUES ('hello'),('WORLD'),('mixed Case')"],
        "SELECT UPPER(s), LOWER(s), LENGTH(s) FROM sf ORDER BY ROWID"),
    ("math_funcs",
        ["CREATE TABLE mf(n REAL)",
         "INSERT INTO mf VALUES (4.0),(9.0),(16.0)"],
        "SELECT n, ABS(n - 8.0) FROM mf ORDER BY n"),

    # Type affinity edges
    ("affinity_int_to_text",
        ["CREATE TABLE af(a TEXT)",
         "INSERT INTO af VALUES (42), ('43'), ('cat')"],
        "SELECT a FROM af ORDER BY ROWID"),

    # Edge: empty result
    ("empty_result",
        ["CREATE TABLE er(x INT)"],
        "SELECT * FROM er"),

    # Compound queries
    ("union_all",
        ["CREATE TABLE u1(x INT)",
         "CREATE TABLE u2(x INT)",
         "INSERT INTO u1 VALUES (1),(2)",
         "INSERT INTO u2 VALUES (2),(3)"],
        "SELECT x FROM u1 UNION ALL SELECT x FROM u2 ORDER BY x"),
    ("union_distinct",
        ["CREATE TABLE ud1(x INT)",
         "CREATE TABLE ud2(x INT)",
         "INSERT INTO ud1 VALUES (1),(2),(3)",
         "INSERT INTO ud2 VALUES (2),(3),(4)"],
        "SELECT x FROM ud1 UNION SELECT x FROM ud2 ORDER BY x"),
    ("intersect",
        ["CREATE TABLE i1(x INT)",
         "CREATE TABLE i2(x INT)",
         "INSERT INTO i1 VALUES (1),(2),(3),(4)",
         "INSERT INTO i2 VALUES (3),(4),(5)"],
        "SELECT x FROM i1 INTERSECT SELECT x FROM i2 ORDER BY x"),

    # Boundaries
    ("large_insert",
        ["CREATE TABLE lr(n INT)"]
        + ["INSERT INTO lr VALUES (%d)" % i for i in range(100)],
        "SELECT COUNT(*), SUM(n), MAX(n) FROM lr"),

    # Errors (should fail in both)
    ("err_no_table",
        [], "SELECT * FROM no_such_table"),
    ("err_bad_syntax",
        [], "SELECT FROM"),
]


# --- main -------------------------------------------------------

def main():
    here = os.path.abspath(os.path.dirname(__file__))
    server_bin = os.path.normpath(os.path.join(here, "..", "..",
                                                 "examples", "06_sqlxtc",
                                                 "sqlxtc-server"))
    if not os.access(server_bin, os.X_OK):
        print("FAIL: %s not built" % server_bin)
        return 1

    db = "/tmp/sqlxtc-oracle-%d.db" % os.getpid()
    port = 16434
    # Clean up any stale process from the same port.
    subprocess.call(["pkill", "-9", "-f", "sqlxtc-server.*-p %d" % port],
                    stderr=subprocess.DEVNULL)
    if os.path.exists(db):
        os.unlink(db)

    # Spawn server detached so even an unclean exit doesn't hang us.
    log = open("/tmp/sqlxtc-oracle.log", "wb")
    proc = subprocess.Popen(
        [server_bin, "-p", str(port), "-d", db],
        stdin=subprocess.DEVNULL,
        stdout=log, stderr=log,
        start_new_session=True,
    )
    time.sleep(0.5)

    rc = 0
    try:
        try:
            qk = Quack(port)
        except Exception as e:
            print("FAIL: cannot connect to sqlxtc: %s" % e)
            return 1

        ref = sqlite3.connect(":memory:")
        ref.isolation_level = None     # autocommit; matches sqlxtc

        passed = 0
        failed = 0
        for desc, setup, test in CORPUS:
            # Reset both databases.
            try:
                ref.close()
            except Exception:
                pass
            ref = sqlite3.connect(":memory:")
            ref.isolation_level = None

            qk.close()
            # rm + restart isolated db file for each test
            if os.path.exists(db):
                os.unlink(db)
            subprocess.call(["pkill", "-9", "-f",
                             "sqlxtc-server.*-p %d" % port],
                            stderr=subprocess.DEVNULL)
            time.sleep(0.05)
            proc = subprocess.Popen(
                [server_bin, "-p", str(port), "-d", db],
                stdin=subprocess.DEVNULL,
                stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                start_new_session=True,
            )
            time.sleep(0.3)
            qk = Quack(port)

            for sql in setup:
                qk.query(sql)
                try:
                    ref.execute(sql)
                except sqlite3.Error:
                    pass

            qcols, qrows, qerr = qk.query(test)

            ref_err = None
            ref_rows = []
            try:
                cur = ref.execute(test)
                ref_rows = cur.fetchall()
            except sqlite3.Error as e:
                ref_err = str(e)

            ok = True
            if (qerr is None) != (ref_err is None):
                ok = False
            elif qerr is None:
                if normalise_rows(qrows) != normalise_rows(ref_rows):
                    ok = False

            if ok:
                passed += 1
                print("  ok   %s" % desc)
            else:
                failed += 1
                print("  FAIL %s" % desc)
                print("       sqlxtc: cols=%r rows=%r err=%r" %
                      (qcols, qrows[:5], qerr))
                print("       sqlite: rows=%r err=%r" %
                      (ref_rows[:5], ref_err))

        print("\nResults: %d passed, %d failed (out of %d)" %
              (passed, failed, len(CORPUS)))
        if failed > 0:
            rc = 1
        try:
            qk.close()
        except Exception:
            pass
        ref.close()
    finally:
        try:
            os.killpg(os.getpgid(proc.pid), 9)
        except Exception:
            pass
        if os.path.exists(db):
            os.unlink(db)
        log.close()

    return rc

if __name__ == "__main__":
    sys.exit(main())
