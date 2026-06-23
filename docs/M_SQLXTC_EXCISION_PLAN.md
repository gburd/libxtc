# Excising sqlite3.c and the "sqlite" naming from sqlxtc -- audit + plan

This document is the complete, actionable plan for the final step of the
sqlxtc excision: physically removing the vendored `sqlite3.c` and
retiring every "sqlite" term from the example's own sources.  It is the
result of a full audit of where SQLite is still referenced and why.

## Current state (committed, green)

- The server runs **SQLite-free by default** (`sx_open` returns a native
  `sx_db` handle; an unrecognized statement is a hard error).
- The entire external differential oracle (32/32) runs natively with no
  VDBE and no `sqlite3_prepare`; the oracle is a gating CI check.
- `sqlite3.c` is still **linked** into `sqlxtc-server` and the in-process
  tests because the engine objects still REFERENCE `xsql_*` symbols on
  dead-at-runtime paths, and ~22 in-process tests still drive the engine
  through the SQLite vtab / C API as a reference.

## The "sqlite" term audit -- four categories

### A. `sqlite3_*` public-API tokens (442 distinct) -- VANISH with the shim

Every `sqlite3_<name>` the example uses is already remapped to `xsql_*`
by the force-included `xsql.h` shim (see `docs/M_SQLXTC_NAMING.md`).
These are NOT separate renames: when `sqlite3.c` + `xsql.h` are deleted,
the engine must stop CALLING them at all (the removal below), and the
symbols cease to exist.  No `sqlite3_` identifier survives.

### B. `SQLITE_*` constants in the example's own code

The non-vendored sources use a handful of result/type constants
(`SQLITE_OK`, `SQLITE_ROW`, `SQLITE_DONE`, `SQLITE_NULL`, `SQLITE_ERROR`,
`SQLITE_NOMEM`, `SQLITE_INTEGER`, ...).  The native API already defines
`SX_OK` / `SX_ROW` / `SX_DONE` / `SX_NULL` / `SX_INTEGER` ... in
`engine.h`.  Convert the example sources to the `SX_*` names; the
`SQLITE_*` spellings then exist only inside vendored `sqlite3.c`, which
is deleted.

### C. The string/identifier literals that are FILE-FORMAT or SQL-VISIBLE

`sqlite_master`, `sqlite_stat*`, `"SQLite format 3"`: these were
deliberately left untouched (per M_SQLXTC_NAMING.md) for on-disk and
SQL compatibility with the vendored engine.  Since the native engine
uses its OWN catalog (the xstore catalog, table-id 0) and its OWN record
codec -- not `sqlite_master` and not the SQLite file format -- these
literals are ONLY referenced by `sqlite3.c` and the dead vtab path.
They leave with `sqlite3.c`.  The native engine has no `sqlite_*`
identifier.

### D. The word "SQLite" in comments

~100 comments reference "SQLite" -- mostly explaining how a native path
mirrors or replaces SQLite behaviour ("matches how SQLite reports an
INTEGER PRIMARY KEY", "no SQLite object to prepare against").  After the
deletion these split into:
  - **keep** (design-lineage / behavioural-equivalence notes that are
    still true and useful: "SQLite-equivalent implicit-rowid table",
    "no such table: NAME -- SQLite's wording");
  - **update** (transitional notes about the legacy path / VDBE
    fallback / shim, which no longer exist) -> delete or rephrase to the
    native reality.
A single comment sweep after the code removal handles category D.

## The removal plan (each step keeps the tree green until the last)

The external python-sqlite3 oracle is the correctness proof THROUGHOUT
(it links its own SQLite, outside the engine), so equivalence is checked
at every step.

### Step 1 -- close the native recognition gaps the in-process tests need

These features the VDBE currently provides and the storage tests
exercise must become native first:
  - **native SAVEPOINT / RELEASE / ROLLBACK TO**: Lime tokens
    (TK_SAVEPOINT, TK_RELEASE), grammar rules carrying the savepoint
    name, new SQL_KIND_SAVEPOINT/RELEASE, a name->level map on the
    connection, wired to the existing `xstore_savepoint` /
    `xstore_release` / `xstore_rollback_to`.  (Regenerate
    `sql_parse_gen.{c,h}` byte-identically; the %expect count likely
    needs no bump but verify the CI staleness check.)  Used by
    test_savepoint.
  - everything else the storage tests issue is already native (verified
    by the oracle being 32/32).

### Step 2 -- add `sx_open_bt(bt, &h)` for storage tests

A SQLite-free native connection over a caller-provided B-tree, so the
storage-engine tests (which create their own bm/bt) can drive it through
the `sx_*` SQL API with no SQLite object.  (Drafted this session.)

### Step 3 -- re-home the in-process tests (no sqlite3 API)

  - **Retire the 6 VDBE-comparison tests** (test_vexec, test_vexec_ord,
    test_vexec_join, test_vexec_par, test_vexec_run, test_db_vexec):
    their whole purpose is "match the VDBE", which the external oracle
    now does.  Remove from CI + Makefile; delete the files.  (test_vexec_par's
    UNIQUE coverage -- morsel-parallel scaling -- moves to a native-only
    scaling test that checks speedup, not VDBE equality.)
  - **Convert the ~12 storage tests** (test_xstore, test_xstore_scan,
    test_isolation, test_savepoint, test_native_txn, test_mvcc,
    test_steal, test_clean_restart, test_torn_smo, test_wal_compact,
    test_wal_recover, test_server_storage, test_inplace_redo) to
    `sx_open_bt` + the `sx_*` API + native `CREATE TABLE` (the vtab
    convention `USING xstore(k, a, b)` maps to `CREATE TABLE
    t(k INTEGER PRIMARY KEY, a, b)` -- first column is the rowid).
    test_xstore and test_isolation also use the vtab-only SQL functions
    `xstore_as_of()` / `xstore_gc()` / `xstore_serializable()`; those
    move to the xstore C API (snapshot reads, `xstore_native_begin`,
    the GC entry point) since they test the storage engine, not SQL.
    Update each test's Makefile rule to link the native engine objects
    (engine.c + vexec.c + sql_parse* + sql_ast.c + xstore.c + ...,
    NOT sqlite3.o).

### Step 4 -- remove the non-native source path from vexec.c

`jsrc_build` returns 0 for a non-catalog table; with every table native,
the non-native join/derived branches (the `sqlite3_prepare_v2` cursors
in `vx_try_prepare_join`, `vx_try_prepare_njoin`, `join_build`,
`next_chunk`) are dead.  Make a non-native side a hard fallback and
delete the `sqlite3_stmt` cursor branches + fields.  (NOTE: test_vexec_join
deliberately exercised the mixed native+SQLite-source join; that test is
retired in Step 3, unblocking this removal.)

### Step 5 -- remove the engine's legacy paths

  - delete the VDBE fallback (`nat_fallback_to_vdbe`) and make a runtime
    decline a hard error;
  - delete the legacy connection branch in `sx_open` (always native;
    require storage) + the `g_native_conn` flag;
  - delete the vtab module + SQL functions from xstore.c (`xs_connect` ..
    `xs_rollback_to`, `xstore_module`, `xstore_register` (keep
    `xstore_register_native`), `fn_now`/`fn_as_of`/...);
  - delete the SQLite mem/mutex/pcache/vfs shims (mem.c, mutex.c,
    pcache.c, vfs.c) and their `xtc_blocking`/`xtc_slab`/`xtc_amutex`
    wiring;
  - convert the example sources' `SQLITE_*` constants to `SX_*`
    (category B) and sweep the comments (category D).

### Step 6 -- drop the link + delete the files

  - remove `sqlite3.o` from the `sqlxtc-server` and every test rule in
    the example `Makefile`, and from `dist/Makefile.in` + the
    amalgamation tooling (`dist/mkamalgamation.py`);
  - delete `sqlite3.c`, `sqlite3.h`, `xsql.h`, mem.c/mem.h, mutex.c/
    mutex.h, pcache.c/pcache.h, vfs.c/vfs.h (the SQLite shims);
  - re-run all 13 CI jobs (the oracle gate proves equivalence).

## Why it is staged, not one commit

Steps 1-5 each keep `sqlite3.c` linked and the tree green; only Step 6
flips the link.  Doing it as one change would leave the tree
un-buildable for the duration.  The high-risk items are the parser
SAVEPOINT feature (Step 1, with the byte-identical regeneration
constraint) and the test re-homing (Step 3, ~18 files), which is why
they are sequenced first behind the still-present fallback.

## Scope summary

- ~270k lines deleted (`sqlite3.c` + `sqlite3.h`).
- 4 shim files deleted (mem/mutex/pcache/vfs) + `xsql.h`.
- 6 tests retired, ~12 tests re-homed to the native API.
- 1 parser feature (SAVEPOINT) + 1 test helper (`sx_open_bt`) added.
- engine.c / xstore.c / vexec.c lose every `xsql_*` / `sqlite3_*`
  reference; the Makefile / dist / amalgamation lose `sqlite3.o`.
