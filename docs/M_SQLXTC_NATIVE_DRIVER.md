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
