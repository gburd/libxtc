#!/usr/bin/env python3
# Copyright (c) 2026, The XTC Project
# Use of this source code is governed by the ISC License.
#
# test/sqlxtc/harvest_sqlite_tests.py
#
# Harvest executable SQL statements out of SQLite's own TCL test
# corpus (~/src/sqlite/test/*.test) and run each through BOTH the
# sqlxtc engine (via the Quack JSON-over-TCP protocol) and Python's
# stdlib sqlite3, comparing result sets.
#
# This measures sqlxtc's SQL-DIALECT compatibility with SQLite -- the
# fraction of real SQLite-corpus statements for which sqlxtc produces
# an identical result set.  It is NOT (and cannot be) a run of
# SQLite's testfixture suite: sqlxtc is a from-scratch engine that
# speaks a wire protocol, not SQLite's C API, so the TCL suite (which
# drives the sqlite3 C library and asserts SQLite-internal behaviour)
# does not apply.  See the FINAL REPORT in the task write-up.
#
# Usage:
#   python3 harvest_sqlite_tests.py [--sqlite-dir DIR] [--files GLOB...]
#                                   [--verbose] [--max-files N]
#
# Depends on ~/src/sqlite/test being present; it is therefore an
# opt-in script, NOT part of `make check`.  The wired-in CI test is
# test_sqlxtc_compat.py, which carries a self-contained corpus.

import argparse
import glob
import os
import re
import sqlite3
import sys

# Reuse the Quack client + normalisation + server harness.
sys.path.insert(0, os.path.abspath(os.path.dirname(__file__)))
from compat_common import Quack, normalise_rows, Server, split_statements

# Portable-dialect test files worth harvesting from.  We deliberately
# avoid SQLite-internal ones (pragma, vacuum, fts, rtree, vtab, wal,
# corrupt, fuzz, tclsqlite, savepoint internals, EXPLAIN, malloc, ioerr,
# analyze, auth, attach, trigger internals) -- those assert engine
# internals no independent engine reproduces.
DEFAULT_FILES = [
    "select1", "select2", "select3", "select4", "select5",
    "where", "where2", "where3",
    "join", "join2", "join3",
    "insert", "insert2",
    "update", "delete",
    "orderby1", "orderby2", "orderby4",
    "groupby",
    "limit",
    "subquery", "subquery2",
    "distinct",
    "between", "in", "in2",
    "like", "like2",
    "cast",
    "coalesce",
    "aggnested",
    "minmax", "minmax2", "minmax3", "minmax4",
    "null",
    "tkt-*",  # regression kernels, mostly portable
    "boundary1",
    "func", "func2",
    "expr",
    "int",
    "tempdb",
    "table",
    "view",
    "count",
    "collate1",
    "conflict",
]

# --- TCL block extraction ---------------------------------------
#
# We do not parse TCL.  We scan for the tokens `execsql` and `db eval`
# followed by a brace-delimited block, extract the block body, and
# treat it as SQL.  Blocks that reference TCL variables ($x) or
# command substitution ([...]) are skipped -- we cannot resolve them.

BLOCK_RE = re.compile(r'\b(?:execsql|db(?:2)?\s+eval)\s*\{', re.IGNORECASE)


def extract_sql_blocks(text):
    """Yield the raw SQL text of each execsql/db-eval brace block."""
    blocks = []
    for m in BLOCK_RE.finditer(text):
        i = m.end() - 1          # points at the opening '{'
        depth = 0
        j = i
        n = len(text)
        while j < n:
            c = text[j]
            if c == '{':
                depth += 1
            elif c == '}':
                depth -= 1
                if depth == 0:
                    break
            j += 1
        if depth != 0:
            continue
        body = text[i + 1:j]
        blocks.append(body)
    return blocks


def usable_sql(stmt):
    """Skip statements we cannot faithfully replay differentially."""
    s = stmt.strip()
    if not s:
        return False
    # TCL variable / command substitution we can't resolve.
    if '$' in s or re.search(r'\[[a-z]', s):
        return False
    # SQLite-internal surface we deliberately exclude.
    low = s.lower()
    for bad in ("pragma", "explain", "vacuum", "attach", "detach",
                "reindex", "analyze", "sqlite_", "fts", "rtree",
                "create virtual", "create trigger", "using fts",
                "randomblob", "hex(", "zeroblob", "sqlite_version",
                "load_extension", "?", "printf(", "glob(",
                "collate ", "raise(", "sqlite_master", "sqlite_schema"):
        if bad in low:
            return False
    return True


def harvest_file(path):
    """Return the ordered list of SQL statements from one .test file."""
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            text = fh.read()
    except OSError:
        return []
    stmts = []
    for body in extract_sql_blocks(text):
        for stmt in split_statements(body):
            if usable_sql(stmt):
                stmts.append(stmt.strip())
    return stmts


# --- differential run -------------------------------------------

_MUTATION_RE = re.compile(
    r'^\s*(insert|update|delete|create|drop|alter|replace|begin|commit|'
    r'rollback|savepoint|release)\b', re.IGNORECASE)


def _is_mutation(sql):
    """Does this statement change persistent DB state?  Used by the
    cascade guard: a mutation that one engine accepts and the other
    rejects (or that produces a mismatch) desynchronises the two
    databases, so everything after it must be tainted."""
    return bool(_MUTATION_RE.match(sql))


def run_file(server, path, verbose=False):
    """Run one harvested file's statements through both engines.

    Returns a per-statement list of dicts with the comparison outcome.
    Each file gets a fresh sqlxtc DB and a fresh sqlite3 :memory:.
    A statement counts only if sqlite3 accepts it (defines the oracle);
    statements sqlite3 itself rejects are 'skipped' (out of dialect).
    """
    stmts = harvest_file(path)
    if not stmts:
        return []

    ref = sqlite3.connect(":memory:")
    ref.isolation_level = None
    qk = server.fresh()

    # Cascade guard: once the two engines' persistent state diverges
    # (a statement one accepted and the other rejected, or a genuine
    # result mismatch), every LATER statement in the file runs against
    # different data and its comparison is meaningless.  We therefore
    # taint the remainder of the file.  A statement is scored as a real
    # correctness bug ONLY while both engines are still in lock-step.
    tainted = False
    out = []
    for sql in stmts:
        # Oracle first.
        ref_err = None
        ref_rows = []
        try:
            cur = ref.execute(sql)
            ref_rows = cur.fetchall()
        except sqlite3.Error as e:
            ref_err = str(e)
        except Exception as e:            # non-SQL TCL leakage
            ref_err = "nonsql:" + str(e)

        # sqlxtc.
        try:
            qcols, qrows, qerr = qk.query(sql)
        except Exception as e:
            qcols, qrows, qerr = None, [], "wire:" + str(e)

        rec = {"file": os.path.basename(path), "sql": sql,
               "ref_err": ref_err, "q_err": qerr,
               "ref_rows": ref_rows, "q_rows": qrows}

        if tainted:
            # State already diverged upstream; do not double-count.
            rec["verdict"] = "tainted"
        elif ref_err is not None:
            # sqlite3 rejected: out of dialect, not counted.  If sqlxtc
            # ACCEPTED what sqlite3 rejected, state may diverge -> taint.
            rec["verdict"] = "skip_oracle_reject"
            if qerr is None and _is_mutation(sql):
                tainted = True
        elif qerr is not None:
            rec["verdict"] = "q_reject"
            # A rejected mutation leaves sqlxtc's data behind sqlite3's.
            if _is_mutation(sql):
                tainted = True
        elif normalise_rows(qrows) == normalise_rows(ref_rows):
            rec["verdict"] = "match"
        else:
            rec["verdict"] = "mismatch"
            if _is_mutation(sql):
                tainted = True
        out.append(rec)
        if verbose and rec["verdict"] in ("q_reject", "mismatch"):
            print("  [%s] %s" % (rec["verdict"], sql[:100]))
    try:
        ref.close()
    except Exception:
        pass
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sqlite-dir",
                    default=os.path.expanduser("~/src/sqlite/test"))
    ap.add_argument("--files", nargs="*", default=None,
                    help="basenames (no .test) or globs; default: portable set")
    ap.add_argument("--max-files", type=int, default=0)
    ap.add_argument("--verbose", action="store_true")
    ap.add_argument("--report", default=None,
                    help="write categorized JSON report here")
    args = ap.parse_args()

    testdir = args.sqlite_dir
    if not os.path.isdir(testdir):
        print("SKIP: SQLite test dir not found: %s" % testdir)
        return 0

    pats = args.files if args.files else DEFAULT_FILES
    paths = []
    seen = set()
    for pat in pats:
        for p in sorted(glob.glob(os.path.join(testdir, pat + ".test"))):
            if p not in seen:
                seen.add(p)
                paths.append(p)
    if args.max_files:
        paths = paths[:args.max_files]
    if not paths:
        print("SKIP: no matching .test files")
        return 0

    print("Harvesting from %d files under %s" % (len(paths), testdir))

    server = Server()
    server.start()

    totals = {"match": 0, "mismatch": 0, "q_reject": 0,
              "skip_oracle_reject": 0, "tainted": 0}
    mismatches = []
    rejects = []
    try:
        for path in paths:
            recs = run_file(server, path, verbose=args.verbose)
            fm = {"match": 0, "mismatch": 0, "q_reject": 0,
                  "skip_oracle_reject": 0, "tainted": 0}
            for r in recs:
                totals[r["verdict"]] += 1
                fm[r["verdict"]] += 1
                if r["verdict"] == "mismatch":
                    mismatches.append(r)
                elif r["verdict"] == "q_reject":
                    rejects.append(r)
            n_eval = fm["match"] + fm["mismatch"] + fm["q_reject"]
            if n_eval:
                print("  %-16s  %3d/%3d match  (%d mismatch, %d reject, "
                      "%d oracle-skip)" %
                      (os.path.basename(path), fm["match"], n_eval,
                       fm["mismatch"], fm["q_reject"],
                       fm["skip_oracle_reject"]))
    finally:
        server.stop()

    evaluated = totals["match"] + totals["mismatch"] + totals["q_reject"]
    print("\n" + "=" * 62)
    print("HARVEST COMPATIBILITY SUMMARY")
    print("=" * 62)
    print("Statements evaluated (in-lock-step, sqlite3 accepted): %d" % evaluated)
    print("  match       : %d" % totals["match"])
    print("  q_reject    : %d  (sqlxtc rejected -- parse/unsupported)"
          % totals["q_reject"])
    print("  mismatch    : %d  (ran, wrong result -- CORRECTNESS BUGS)"
          % totals["mismatch"])
    print("  oracle-skip : %d  (sqlite3 itself rejected; not counted)"
          % totals["skip_oracle_reject"])
    print("  tainted     : %d  (skipped: state diverged upstream in file)"
          % totals["tainted"])
    if evaluated:
        print("\nCOMPAT RATE: %d / %d = %.1f%%  (identical to SQLite)" %
              (totals["match"], evaluated,
               100.0 * totals["match"] / evaluated))

    # Categorize rejects by a crude feature signature.
    def feature_of(sql):
        low = sql.lower()
        for feat, pat in [
            ("window_fn", r'\bover\s*\('),
            ("cte_with", r'\bwith\b.*\bas\s*\('),
            ("right_join", r'\bright\s+join\b'),
            ("full_join", r'\bfull\b.*\bjoin\b'),
            ("natural_join", r'\bnatural\b'),
            ("using_clause", r'\busing\s*\('),
            ("case_expr", r'\bcase\b'),
            ("cast_expr", r'\bcast\s*\('),
            ("glob", r'\bglob\b'),
            ("like_escape", r'\blike\b.*\bescape\b'),
            ("in_subq", r'\bin\s*\(\s*select'),
            ("exists", r'\bexists\b'),
            ("group_concat", r'group_concat'),
            ("string_agg", r'string_agg'),
            ("substr", r'\bsubstr\b'),
            ("replace_fn", r'\breplace\s*\('),
            ("round_fn", r'\bround\s*\('),
            ("trim_fn", r'\btrim\s*\('),
            ("instr_fn", r'\binstr\s*\('),
            ("nullif", r'\bnullif\b'),
            ("insert_select", r'insert\s+into[^;]*\bselect\b'),
            ("create_index", r'create\s+.*index'),
            ("create_view", r'create\s+.*view'),
            ("alter_table", r'\balter\s+table\b'),
            ("having", r'\bhaving\b'),
            ("distinct", r'\bdistinct\b'),
            ("compound", r'\b(union|except|intersect)\b'),
            ("collate", r'\bcollate\b'),
            ("order_by", r'\border\s+by\b'),
        ]:
            if re.search(pat, low):
                return feat
        return "other"

    from collections import Counter
    rej_features = Counter(feature_of(r["sql"]) for r in rejects)
    print("\nsqlxtc REJECTS by feature (missing/unsupported), ranked:")
    for feat, cnt in rej_features.most_common():
        print("  %-16s %d" % (feat, cnt))

    print("\nCORRECTNESS MISMATCHES (ran but wrong result): %d" %
          len(mismatches))
    for r in mismatches[:40]:
        print("  --- %s" % r["file"])
        print("      SQL   : %s" % r["sql"][:160])
        print("      sqlxtc: %r" % (normalise_rows(r["q_rows"])[:6]))
        print("      sqlite: %r" % (normalise_rows(r["ref_rows"])[:6]))

    if args.report:
        import json
        rep = {
            "totals": totals,
            "compat_rate": (100.0 * totals["match"] / evaluated
                            if evaluated else None),
            "reject_features": dict(rej_features),
            "mismatches": [
                {"file": r["file"], "sql": r["sql"],
                 "sqlxtc": normalise_rows(r["q_rows"])[:20],
                 "sqlite": normalise_rows(r["ref_rows"])[:20]}
                for r in mismatches],
            "sample_rejects": [
                {"file": r["file"], "sql": r["sql"], "err": r["q_err"]}
                for r in rejects[:60]],
        }
        with open(args.report, "w") as fh:
            json.dump(rep, fh, indent=2, default=str)
        print("\nWrote report: %s" % args.report)

    return 0


if __name__ == "__main__":
    sys.exit(main())
