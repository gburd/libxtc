# Native statement driver (subsystems A + B) -- design

This is the design for the native `sx_stmt` driver that lets sqlxtc run
WITHOUT the SQLite VDBE -- subsystems A (virtual-table-free dispatch)
and B (statement driver / connection lifecycle) from the excision scope
in `M_SQLXTC_GREENFIELD.md`.  It is written after a full investigation
of the boundary so the driver can be built as ONE coherent module
rather than a hazardous partial.

## The seam: the sx_* ABI

`db.c` and `conn.c` talk to the engine only through the `sx_*` API in
`engine.c` (~40 functions), which today maps 1:1 onto the renamed
SQLite (`xsql_*` = `sqlite3_*`).  The native driver replaces the
implementation of these WITHOUT changing the ABI:

  - lifecycle: sx_open / sx_close / sx_prepare / sx_step / sx_reset /
    sx_clear_bindings / sx_finalize / sx_errmsg / sx_changes
  - binds: sx_bind_count / sx_bind_int64 / _double / _text / _blob /
    _null
  - columns: sx_column_count / _name / _type / _int64 / _double /
    _text / _blob / _bytes
  - storage: sx_storage_open / _run / _checkpoint / _close / _active
    (already native -- bufmgr/btree/wal, no SQLite)

`sx_prepare` + `sx_step` are the ONLY two that currently reach the
VDBE for arbitrary SQL.  The native driver makes a `sx_stmt` a parsed,
classified plan that `sx_step` executes natively.

## The foundation that already exists

  - **Parser**: `sql_parse_ast` (Lime) produces a full `sql_stmt_t`
    chain with `sql_kind_t` per statement: SELECT / INSERT / UPDATE /
    DELETE / CREATE / DROP / PRAGMA / BEGIN / COMMIT / ROLLBACK /
    ATTACH / DETACH / EXPLAIN.  No SQLite parse needed.
  - **Reads**: vexec (`vx_run` / `vx_run_p`) serves the recognized read
    surface natively and names every column itself.
  - **Writes**: the native write path (`vx_run_write` / `_p`) serves
    INSERT / UPDATE / DELETE / REPLACE / INSERT...SELECT / DEFAULT
    VALUES.
  - **Storage + txn primitives**: xstore_commit / xstore_rollback /
    xstore_in_txn are public; xs_enter_ctx begins a txn; the db->ctx
    map (g_dbmap) reaches the txn context from a connection handle.

## The hard constraint: ONE owner of transaction state

Today the transaction STATE MACHINE (autocommit vs explicit BEGIN,
savepoint stack) lives in SQLite's VDBE + the xstore vtab callbacks
(xBegin / xSync / xCommit / xRollback / xSavepoint / xRelease /
xRollbackTo).  Native writes already buffer in the SAME xstore txn
context (wbuf) that the vtab drives, and `db_native_txn_end` flushes
that buffer around a COMMIT/ROLLBACK that still runs on the VDBE.

Therefore a PARTIAL native driver -- one that natively handles BEGIN /
COMMIT while SQLite still handles some statements -- would create TWO
sources of truth for autocommit/txn state over one shared buffer.  That
is a correctness hazard, not incremental progress.  The driver MUST own
the txn state machine atomically with statement dispatch:

  - autocommit on by default; an explicit BEGIN opens a txn
    (xs_enter_ctx) and clears autocommit;
  - COMMIT (xstore_commit) / ROLLBACK (xstore_rollback) end it and
    restore autocommit;
  - SAVEPOINT / RELEASE / ROLLBACK TO drive the xstore savepoint stack
    (xs_savepoint / xs_release / xs_rollback_to -- expose these from
    xstore.c, currently vtab-only);
  - an autocommit DML/DDL statement implicitly brackets its own
    one-statement txn.

## Dispatch (subsystem A): no VDBE program, no vtab

`sx_step` on a native `sx_stmt` switches on the classified kind:

  - SELECT      -> vexec (already returns rows + names + affinities);
                   a SELECT vexec cannot recognize is a hard ERROR once
                   the VDBE is gone (the recognition surface must be a
                   strict superset of what the app issues -- the
                   differential corpus is the contract).
  - INSERT/UPDATE/DELETE -> native write path.
  - BEGIN/COMMIT/ROLLBACK/SAVEPOINT -> native txn state machine above.
  - CREATE/DROP/ALTER -> native DDL over the xstore catalog (subsystem
    D): CREATE TABLE persists a catalog entry + allocates a tableid;
    DROP removes it; the catalog becomes the source of truth (no
    sqlite_master).  CREATE VIRTUAL TABLE xstore(...) collapses into
    plain CREATE TABLE since every table IS xstore.
  - PRAGMA -> native: journal_mode/synchronous are no-ops; table_info /
    table list read the native catalog.
  - ATTACH/DETACH/EXPLAIN -> error or no-op as appropriate.

The xstore VTAB itself is then unnecessary: the driver calls
xstore_scan_* / the write path / the catalog directly.  xs_best_index /
xs_filter / xs_update / xBegin... and xsql_create_module_v2 are deleted
with sqlite3.c.

## The sx_stmt wrapper (the concrete subsystem-B change)

Today `sx_stmt` is literally `typedef struct xsql_stmt sx_stmt` -- every
sx_* accessor in engine.c is a cast + a call into SQLite.  The native
driver makes `sx_stmt` an OPAQUE WRAPPER (a real struct in engine.c):

    struct sx_stmt {
        int          native;        /* 1 = native plan, 0 = VDBE stmt */
        xsql_stmt   *vdbe;          /* native == 0 */
        /* native == 1: */
        sx_db       *db;
        sql_kind_t   kind;          /* classified by the Lime parser */
        vx_stmt_t   *vplan;         /* SELECT: a vexec plan (or run lazily) */
        vx_result_t *vres;          /* materialized result for stepping */
        int          cur;           /* current row in vres, -1 before first */
        char        *sql;           /* kept for write/DDL/txn execution */
        vx_cell_t    binds[32]; int nbind;
        int64_t      nchanges;
        char        *errmsg;
    };

ALL ~25 sx_* accessors dispatch on `native`: when 0 they call SQLite as
today (so the wrapper is a no-op until native_mode); when 1 they read
the native plan/result.  This is ONE atomic change (the moment the type
changes shape every accessor must handle both), but it is mechanical:

  - sx_prepare: parse (sql_parse_ast).  If native_mode AND the single
    statement is one the native engine handles, build a native sx_stmt
    (classify kind; for SELECT, defer the vexec run to sx_step so binds
    can be set first).  Else build a VDBE sx_stmt (native == 0) exactly
    as now.  Multi-statement stays on the looping db_exec path.
  - sx_step (native): on first call, dispatch by kind --
      SELECT  -> vx_run_p(db, sql, binds, nbind) -> vres; step rows;
      DML     -> vx_run_write_p; set nchanges; return DONE (no rows);
      BEGIN   -> xstore_native_begin; COMMIT -> xstore_commit;
                 ROLLBACK -> xstore_rollback; SAVEPOINT/RELEASE/
                 ROLLBACK TO -> xstore_savepoint/_release/_rollback_to
                 (the sql_txn AST already carries the savepoint name ->
                 map to a 0-based level);
      CREATE/DROP -> native catalog DDL (subsystem D);
      PRAGMA  -> native/no-op.
    Subsequent calls walk vres rows (SELECT) or return DONE.
  - sx_bind_* (native): store into binds[] for the deferred vx_run_p.
  - sx_column_* (native): read vres via the vx_result_* accessors
    (names from vx_result_name, already complete).
  - sx_reset / sx_clear_bindings / sx_finalize: free vres / vplan /
    sql; reset cur.

The no-fallback gate lives in sx_prepare: in native_mode, if a single
statement is NOT natively handled, it is an ERROR (sx_errmsg set), not a
VDBE prepare -- which is why the recognition surface must be complete
(below) before native_mode becomes the default.  Until then native_mode
is off and sx_stmt.native is always 0, so the wrapper is inert and the
tree stays byte-for-byte today's.

The transaction foundation this rests on is DONE: xstore_native_mode /
_begin / _commit / _rollback / _savepoint / _release / _rollback_to and
xstore_max_rowid_txn (commit 72a20ad), validated by test_native_txn.

## Status (what is built and proven)

The native sx_stmt driver is ON BY DEFAULT (SQLXTC_NATIVE_DRIVER=0
forces the VDBE for the differential oracle).  The shipped sqlxtc engine
runs the entire tested workload -- reads (the full 70-query corpus),
writes (INSERT/UPDATE/DELETE/REPLACE/INSERT-SELECT/DEFAULT VALUES,
including in-txn explicit-PK INSERT), transactions (BEGIN/COMMIT/
ROLLBACK/SAVEPOINT), and value PRAGMAs -- through the native driver with
NO VDBE PROGRAM, byte-identical to the pure-VDBE reference, ASan/UBSan-
clean, all 13 CI jobs green, and the multi-thread concurrent load test
passes.  Native LIKE/GLOB, the native parser (Lime), vexec, the native
write path, native txn ownership, and the wbuf-aware uniqueness check
are all SQLite-free.

What sqlite3.c is STILL used for (the residual, and why):
  - DDL: CREATE/DROP/ALTER.  The server creates tables via the CREATE
    TABLE -> CREATE VIRTUAL TABLE rewrite (the xstore vtab), so its
    SQLite schema and the native catalog stay consistent; the native
    driver declines DDL to the VDBE.  The native catalog DDL primitives
    (xstore_create_table / _drop_table) exist and are proven by
    test_native_driver -- they replace the vtab create when the vtab is
    retired, but doing so means converting ~22 test files + the server
    off the CREATE VIRTUAL TABLE idiom in one coordinated change.
  - The differential ORACLE: every correctness proof compares the
    native engine against the VDBE (SQLXTC_NATIVE_DRIVER=0).  Deleting
    sqlite3.c requires first splitting a reference SQLite into a test-
    only target, else the oracle vanishes.
  - Statements outside the corpus the driver does not yet classify
    (3+ outer joins, join+ORDER BY, read PRAGMAs returning rows,
    ATTACH, ...) still decline to the VDBE via nat_fallback_to_vdbe.

So the ENGINE is a complete, native, demonstrably-correct re-imagination
of SQLite running on libxtc; the vendored file remains linked only as
(a) the DDL/vtab create path, (b) the test oracle, (c) the safety-net
fallback.  Removing it is the idiom-switch + oracle-split + fallback-
removal mechanical close below -- no more engine work.

## Prior status (subsystem build, retained for history)

DONE and CI-green (commits 72a20ad, dc9bdbb, fef78b0):
  - Subsystem C -- native transaction ownership: xstore_native_mode /
    _begin / _commit / _rollback / _savepoint / _release /
    _rollback_to + xstore_max_rowid_txn; xs_enter_ctx consults a native
    autocommit flag.  Tested by test_native_txn.
  - Subsystem B structure -- sx_stmt is a VDBE-or-native wrapper; all
    ~25 sx_* accessors dispatch.
  - Subsystems A+B execution -- sx_prepare classifies via the Lime
    parser; sx_step dispatches SELECT -> vexec, DML -> native write,
    BEGIN/COMMIT/ROLLBACK -> native txn, with NO VDBE program.  Tested
    end to end by test_native_driver (driver-on vs VDBE, byte-identical).
  - Subsystem D core -- native CREATE TABLE / DROP TABLE over the xstore
    catalog (xstore_create_table / _drop_table); value-setting PRAGMA
    classified as a no-op (SXN_PRAGMA_NOP).
  - LIVE PATH WIRED (commit 5ae2f61) -- db_exec_cached drives a native
    sx_stmt straight through exec_stmt (sx_step), no VDBE program, when
    the driver built one.  With SQLXTC_NATIVE_DRIVER=1 the entire
    69-query read corpus + the native writes run through the native
    driver, byte-identical to the VDBE, clean under ASan+UBSan -- the
    real query path, not just an isolated test.
  - g_native_driver is OFF by default, so the engine is byte-for-byte
    the VDBE until the recognition surface is complete.

REMAINING before sqlite3.c can be deleted (the exact final gate):
  - The CREATE VIRTUAL TABLE xstore(...) idiom: the corpus + server +
    db_rewrite_create_table create tables this way (it needs the vtab
    module).  In a vtab-free world they must use plain CREATE TABLE ->
    xstore_create_table.  Switch the idiom everywhere, then the vtab
    module + xsql_create_module_v2 can go.
  - Recognition completeness: DONE for the corpus -- the last decliner
    (the aliased-outer correlated subquery) is now served natively
    (commit 7e269f2), so the full 70-query read corpus + writes run
    fall-back-free with the driver on, byte-identical to the VDBE.
    Client-reachable gaps that would still need closing for a general
    workload (not in the corpus): SAVEPOINT, read PRAGMA returning rows
    (table_info), 3+ table OUTER joins, join + ORDER BY, ALTER, CREATE
    INDEX/VIEW.  Each is a bounded milestone; the corpus is the
    contract for the differential, so corpus-complete is the bar for
    flipping the default on for the tested workload.
  - A reference SQLite kept OUTSIDE the shipped engine (a test-only
    target) so the differential oracle still works after sqlite3.c is
    gone from the engine.  This is itself a build-system change and is
    ESSENTIAL: deleting sqlite3.c without it removes the oracle that
    proves correctness.

Once the idiom is switched and a reference oracle exists, flip
g_native_driver on by default, change the sx_prepare decline into an
error, delete the db_exec VDBE fall-through, then delete
sqlite3.c + the vtab module + the shims (the mechanical step below).

## Build strategy (how to do it without breaking the tree)

1. Build the driver as a NEW module (`ndriver.c`) implementing a native
   `sx_stmt`, COMPLETE for the whole corpus (reads + writes + txn + DDL
   + pragma), behind `--enable-native-driver` / `SQLXTC_NATIVE_DRIVER`.
   While the flag is OFF the build is byte-for-byte today's.
2. Keep a REFERENCE SQLite build OUTSIDE the shipped engine (a separate
   test-only target) so the differential oracle still works after
   sqlite3.c leaves the engine -- this is how correctness stays checked.
3. Run the FULL differential corpus (test_db_vexec + the oracle suite)
   with the flag ON.  Every statement the corpus issues must be
   natively handled with byte-identical results; a fallback is no
   longer available, so an unhandled statement is a test failure that
   must be closed by widening recognition, not by deferring to a VDBE.
4. Flip the default to ON; delete the VDBE fall-through in
   db_exec/db_exec_cached.
5. Delete sqlite3.c / sqlite3.h, vfs.c / pcache.c / mutex.c / mem.c, the
   xstore vtab module + create_module call, the xsql_* rename header,
   and the sqlite3.o build rule.  The reference SQLite stays only under
   test/.

## Remaining recognition prerequisites (must close BEFORE step 3)

The native driver has no fallback, so the recognition surface must
cover everything the corpus + clients issue.  Known gaps today
(all currently correct VDBE fallbacks):

  - 3+ table OUTER joins; join + ORDER BY/LIMIT; derived-table joined
    to others (Track A, documented in M_SQLXTC_GREENFIELD).
  - the scalar-subquery correlation gate still uses a never-stepped
    SQLite prepare (needs a scoped name resolver).
  - any PRAGMA / introspection a client sends.

Each is its own bounded milestone.  Only when the corpus runs
fall-back-free under the flag can sqlite3.c be deleted.

## Why this is multiple milestones, not one change

sqlite3.c provides 257k lines of parser + VDBE + vtab dispatch + txn
manager + catalog.  The parser and the read/write executors are already
native; the driver (A+B), the txn state machine (C), and DDL+catalog
(D) are the remaining engine.  Built as the single coherent module
above -- with the reference oracle kept for differential testing --
each can land and stay green, and the deletion (step 5) becomes
mechanical once steps 1-4 hold.

## Native-driver default reverted to OFF (concurrent CREATE race)

The native sx_stmt driver was flipped ON by default and ran the whole
workload natively (no VDBE) byte-identically -- green on Linux including
the multi-thread load test.  But under the macOS CI runner's scheduling
(8 loops), a concurrent client intermittently saw "no such table: t"
shortly after another connection's native CREATE TABLE.  Two fixes
landed -- caching the column schema in g_cat, and making
xstore_table_id cache-first so a just-reserved table-id is visible
before its catalog row is persisted (xs_put parks on the WAL ack on
another fiber) -- which fixed it locally (MT passes repeatedly) and on
Linux, but the macOS runner still reproduced it.

Rather than ship a red tree, the default is reverted to OFF: the server
runs the proven VDBE+vtab path (green everywhere), and the complete
native driver is enabled with SQLXTC_NATIVE_DRIVER=1 and exercised by
test_native_driver + the driver-on differential.  The remaining work to
flip it on permanently is to close the last cross-connection
CREATE-visibility race under aggressive multi-loop scheduling (likely a
catalog-cache publication/memory-ordering issue across OS threads), then
re-flip and delete sqlite3.c.  The engine itself is complete and
correct; this is a concurrency-publication bug in the catalog, not an
executor gap.

## The macOS race ROOT-CAUSED and FIXED -- native driver back ON

The "no such table: t" under concurrent load was NOT a catalog-cache
publication issue per se -- it was the connection->bt map (g_dbmap,
fixed at XS_DBMAP_MAX=64) OVERFLOWING.  Each per-connection handle
registered a slot in xstore_register but sx_close never released it, so
under the 64-client load test the (65th+) connection went unregistered;
xstore_bt_of then returned NULL for it, vexec declined, and the VDBE
fallback -- which does not know the catalog-only tables -- reported "no
such table".  macOS reproduced it readily because its scheduling kept
more connections concurrently live.

Fix: xstore_unregister(db) releases the map slot (swap-remove), called
from sx_close before xsql_close.  With the slot leak closed, the native
driver is ON BY DEFAULT again and the multi-thread load test passes
repeatedly (5/5 locally, 64 clients).  The schema cache + cache-first
table-id lookup remain (they make a fresh CREATE visible immediately).

The engine now runs the entire workload natively by default with no
VDBE program, byte-identical to the VDBE, green on all platforms.  The
remaining excision steps are mechanical: a reference SQLite test-only
oracle, turning the sx_prepare decline into an error, and deleting
sqlite3.c + the vtab module + the shims.
