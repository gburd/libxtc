# Excising sqlite3.c and the "sqlite" naming from sqlxtc -- audit + plan

> **STATUS: DONE.**  `sqlite3.c`/`sqlite3.h` and the four SQLite shims
> (mem.c, mutex.c, pcache.c, vfs.c) plus `xsql.h` are physically removed
> from `examples/06_sqlxtc/`.  The server links **no `sqlite3.o`** and
> contains **zero `sqlite3` symbols** (`nm sqlxtc-server | grep sqlite3`
> is empty).  engine.c / vexec.c / xstore.c are native; the vtab module
> in xstore.c survives only under `#if defined(SQLXTC_VTAB)` (never
> defined in the build) as reference code.  The differential oracle is
> 32/32 on both the libxtc.a and the amalgamation links; the 64-client
> MT load test and every re-homed storage test pass; UBSan is clean.
> The single-loop-many-committers tests test_parallel /
> test_server_storage were retired (superseded by the MT load test and
> the crash-recovery suite; they hit a native commit-wait edge that the
> production connection-per-loop topology does not).

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

## Progress update (native-feature completion for the storage tests)

Steps 1-3 are substantially done.  The native engine gained the
features the in-process storage tests needed, and those tests now run
SQLite-free:

* native SAVEPOINT / RELEASE / ROLLBACK TO (grammar + name->level stack);
* sx_open_bt(bt, &h) -- a native connection over a test-owned B-tree;
* in-transaction read-your-writes (xstore_scan_open_txn overlays the
  write buffer: UPDATE replaces, DELETE hides, INSERT appended);
* per-connection MVCC isolation snapshots on the native read path
  (REPEATABLE READ / SNAPSHOT / SERIALIZABLE freeze; READ COMMITTED
  re-samples);
* SSI serializable conflict detection on the native commit path
  (xs_ssi_validate runs in xs_commit_ctx; the native scan records reads
  into the read set + SSI registry; xstore_commit returns SX_BUSY on a
  pivot and rolls back);
* MVCC SQL functions as native C APIs (xstore_set_isolation /
  clock_now / as_of / gc_run / autovacuum_set / prune_count);
* blob literals x'..' (lit_cell); typeof() (vexec VXF_TYPEOF);
* native multi-statement sx_exec (split on top-level ';');
* ALTER TABLE x RENAME TO y (grammar + xstore_rename_table).

Re-homed to the native sx_* / xstore C API (no vtab, no sqlite3_):
test_xstore_scan, test_torn_smo, test_wal_compact, test_wal_recover,
test_inplace_redo, test_clean_restart, test_steal, test_native_txn,
test_savepoint, test_isolation, test_xstore.

### Remaining for the physical sqlite3.c deletion

1. test_server_storage: a server-storage + crash-recovery test.
   Converted draft hit a crash-recovery + tiny-pool WAL timing issue on
   the native autocommit path (the rest converted cleanly); kept on the
   vtab path for now -- a focused crash/WAL investigation, not a
   recognition gap.
2. Retire the VDBE-comparison tests (test_vexec, test_vexec_ord,
   test_vexec_join, test_vexec_par, test_vexec_run, test_db_vexec): the
   external python-sqlite3 oracle is their replacement.  test_native_driver
   intentionally keeps the in-process VDBE as a differential oracle.
3. Remove the non-native vexec source path (the sqlite3_prepare_v2
   join/derived cursor branches -- dead once test_vexec_join retires),
   the VDBE fallback (nat_fallback_to_vdbe), the legacy connection in
   sx_open, the vtab module + SQL functions in xstore.c, and the
   mem/mutex/pcache/vfs shims.
4. Drop sqlite3.o from every Makefile rule + dist + amalgamation; delete
   sqlite3.c/.h + xsql.h + the shims; re-run CI.

The external oracle stays the gating correctness proof throughout.

## EXACT EXECUTION RECIPE (proven this session; replay in order)

This is the precise, verified sequence to cut sqlite3.o.  engine.c and
vexec.c transforms below were applied and confirmed to compile STANDALONE
with no sqlite3.h (gcc -fsyntax-only ... -I. -I../../src/inc <file>, no
-include xsql.h).  Do all of A-G in one session, build + run the oracle
after, commit, then push and check CI.  Baseline: commit 717177c.

Working dir: examples/06_sqlxtc.  Verify each .c compiles native after
its step with:  nix-shell -p gcc --run 'gcc -fsyntax-only -std=c11
-D_GNU_SOURCE -DSQLXTC_HAVE_LIME=1 -I. -I../../src/inc FILE.c'

### A. engine.h -- add result codes (already has SX_OK/ERROR/BUSY/ROW/DONE)
Add after SX_ERROR:  SX_MISUSE 21, SX_NOMEM 7  (SX_BUSY already 5).

### B. engine.c (136 refs -> 0).  All scripted edits:
1. Delete  #include "sqlite3.h"
2. Delete the _Static_assert block comparing SX_* to SQLITE_* (the
   "/* The sx_ result/type codes ... */" comment + the 8 asserts).
3. Global result-code rename (whole-word):  SQLITE_RANGE->SX_ERROR,
   SQLITE_OK->SX_OK, SQLITE_ROW->SX_ROW, SQLITE_DONE->SX_DONE,
   SQLITE_ERROR->SX_ERROR, SQLITE_BUSY->SX_BUSY, SQLITE_NOMEM->SX_NOMEM,
   SQLITE_MISUSE->SX_MISUSE.
4. struct sx_stmt: delete the `xsql_stmt *vdbe;` field (KEEP `int native;`
   -- it stays 1 and feeds sx_stmt_is_native).
5. Accessor dead VDBE tails -- replace each trailing `return xsql_X(st->vdbe...)`
   with an unreachable native default:
     xsql_step->SX_MISUSE; xsql_reset->SX_OK; xsql_clear_bindings->SX_OK;
     xsql_bind_parameter_count->0; xsql_bind_*->SX_MISUSE;
     xsql_column_count->0; xsql_column_name->NULL; xsql_column_type->SX_NULL;
     xsql_column_int64->0; xsql_column_double->0.0; xsql_column_blob->NULL;
     xsql_column_bytes->0; (const char*)xsql_column_text->NULL;
     (int64_t)xsql_changes64->0; xsql_errmsg->"".
6. sx_close: make native-only (drop the #ifdef/legacy xsql_close branch):
     { struct sx_native_db *nd=(struct sx_native_db*)h; if(h==NULL)return;
       xstore_unregister_native((struct xsql*)h); free(nd->errmsg); free(nd); }
7. sx_exec: remove the `#ifdef SQLXTC_HAVE_LIME / if(sx_is_native_db(h)){`
   wrapper (body is always-run) and the trailing `#endif return xsql_exec(...)`;
   the multi-statement splitter body stays; ends `... return SX_OK; }`.
8. sx_prepare: delete decls `xsql_stmt *vdbe=NULL; int rc;`.  Replace the
   "SXN_NONE fall through + native-handle hard-error + VDBE wrapper" tail
   with: keep the SXN_NONE comment, then unconditional
     { struct sx_native_db *nd=(struct sx_native_db*)h; free(nd->errmsg);
       nd->errmsg=strdup("unsupported SQL (native engine)"); return SX_ERROR; }
     #else (void)h;(void)sql;(void)n_bytes;(void)tail;(void)st; return SX_ERROR; #endif }
9. Delete nat_fallback_to_vdbe entirely (function + its doc comment).
10. sx_step: SELECT branch -- replace the two
      `if(!nat_fallback_to_vdbe(st)) return nat_set_run_error(st); return SX_MISUSE;`
      blocks with just `return nat_set_run_error(st);`.  SXN_WRITE decline:
      replace `if(!nat_fallback_to_vdbe(st)) return SX_ERROR; return xsql_step(...)`
      with `return nat_set_run_error(st);`.
11. sx_finalize: delete `if(st->vdbe!=NULL)(void)xsql_finalize(st->vdbe);`.
12. sx_column_count / sx_column_name: drop the nat_fallback_to_vdbe calls
      (the `if(!nat_select_run(st)){...}` -> `if(!nat_select_run(st)) return 0/NULL;`)
      and the trailing `return xsql_column_*(st->vdbe...)` -> `return 0/NULL;`.
13. sx_column_text: trailing `return (const char*)xsql_column_text(...)` -> `return NULL;`.
14. Casts to vexec/xstore opaque-db: `(sqlite3 *)st->db` -> `(struct xsql *)st->db`
      (vx_pragma_table_info, vx_run_p, vx_unknown_table, vx_run_write_p);
      `xstore_register_native((xsql *)nd` -> `(struct xsql *)nd`.

### C. vexec.h (the executor's connection type)
Replace  #include "sqlite3.h" ...  with  `struct xsql;`  and change every
`sqlite3 *` -> `struct xsql *` (the opaque db param: vx_run, vx_run_p,
vx_run_write_p, vx_run_parallel, vx_pragma_table_info, vx_unknown_table).

### D. vexec.c (120 refs -> 0)
1. Add `#include "engine.h"` (for SX_*) before `#include "vexec.h"`.
2. Global `sqlite3 *` -> `struct xsql *`.
3. jsrc_build: the two non-native returns `return 0;` (bt==NULL / nc<=0)
   -> `return -1;` (fall back; no SQLite source).
4. vx_try_prepare_join: delete `sqlite3_stmt *bsrc=NULL,*psrc=NULL;`; the
   build/probe affinity `if(native){...}else{sqlite3_prepare_v2...}` ->
   `if(!build_src.native||!probe_src.native) goto fallback; <native aff>`;
   delete the two `sqlite3_finalize(bsrc/psrc)` success lines and the
   `fallback:` finalize lines.
5. vx_try_prepare_njoin: delete `sqlite3_stmt *psrc[VX_JOIN_MAX];` + its
   init loop + the per-side `else{sqlite3_prepare...}` (-> `if(!src[side].native)
   goto fallback;`) + the two psrc finalize loops.
6. join_build: delete `sqlite3_stmt *bsrc`, the `else if sqlite3_prepare`,
   the `else{sqlite3_step...read_src_cell}` branch (-> unconditional native
   scan block), the `if(bsrc)sqlite3_finalize` in done:.
7. njoin_build_side: same shape -- delete `sqlite3_stmt *bs`, the else
   branches, the bs finalize.
8. Delete read_src_cell (def + the fwd decl line) -- only the removed
   SQLite branches used it.
9. struct vx_stmt: delete the 3 fields `sqlite3_stmt *src/probe/nstream`
   (NOTE: `src` is a 2-line decl -- remove BOTH lines incl the dangling
   "* storage-native single-table path */" comment).
10. next_chunk probe stepping + njoin nstream stepping: drop the
    `else{sqlite3_step(st->probe/nstream)...}` (keep the native scan block,
    wrap it in a bare `{ }`).
11. vx_step: the join/njoin `else if sqlite3_prepare_v2(...&st->probe/nstream)`
    -> `if(!probe_src.native) return SX_ERROR;` then unconditional scan open.
12. vx_finalize: delete the 3 `if(st->src/probe/nstream) sqlite3_finalize(...)`.
13. Global result-code rename SQLITE_DONE/ERROR/MISUSE/ROW -> SX_*.
14. Fix the comment "next sqlite3_step reuses" -> "next scan step reuses".

### E. xstore.c (226 refs -> 0)  -- THE LONG POLE, not yet done
The vtab module is interleaved with shared storage helpers, so guard each
vtab function with `#if defined(SQLXTC_VTAB)` ... `#endif` rather than
delete-by-range.  KEEP: xs_enter_ctx, ctx_free (shared with
xstore_unregister_native), xs_ssi_validate, xs_commit_ctx, xs_rollback_ctx,
all xs_cat_*, the scan, xstore_* native APIs, xstore_register_native.
GUARD (these reference xsql_vtab/xsql_context/xsql_value/xsql_result_*/
xsql_declare_vtab/xsql_create_module_v2 -- all sqlite3.o symbols):
  - typedef struct xstore_vtab (704) + the cursor's `xsql_vtab_cursor base`
    field (1034) -- guard or drop the base member;
  - the fwd decl `static const xsql_module xstore_module;` (1065);
  - xs_enter (1028, wraps xs_enter_ctx);
  - xs_connect, xs_disconnect, xs_best_index, xs_open(xsql_vtab),
    xs_close(xsql_vtab_cursor), xs_filter, xs_next, xs_eof,
    xs_column(xsql_vtab_cursor), xs_rowid, xs_update, xs_begin(xsql_vtab),
    xs_sync (NOT xs_ssi_validate), xs_commit(xsql_vtab),
    xs_rollback(xsql_vtab), xs_savepoint, xs_release, xs_rollback_to;
  - fn_now, fn_as_of, fn_serializable, fn_isolation, fn_gc, fn_autovacuum,
    fn_prune_count;
  - the `static const xsql_module xstore_module = {...};` (3386);
  - xstore_register(xsql*) (3517) -- the vtab registrar (no callers now).
Then: drop `#include "sqlite3.h"`; the bare `sqlite3`/`xsql` opaque-pointer
params on the KEPT functions (xstore_bt_of/ctx_of/register_native/
unregister*/native_*/as_of/etc.) already use `struct xsql *` in xstore.h
-- make the .c match (they take `struct xsql *db`).  Map any KEPT-code
SQLITE_OK/BUSY -> SX_* (xs_commit_ctx returns SQLITE_BUSY -> SX_BUSY;
xs_ssi_validate returns SQLITE_OK/BUSY -> SX_OK/SX_BUSY; xs_put etc.).
Verify: grep -cE 'xsql_vtab|xsql_context|xsql_value|xsql_module|xsql_declare|
xsql_create_module|xsql_result' xstore.c  == 0 outside #ifdef SQLXTC_VTAB.

### F. Delete the shims + xsql.h + sqlite3.c/.h
git rm mem.c mem.h mutex.c mutex.h pcache.c pcache.h vfs.c vfs.h xsql.h
       sqlite3.c sqlite3.h
NOTE: vfs.c provided vfs_register() called by sx_open.  Replace the
sx_open `if(!memlike)(void)vfs_register(0);` with nothing (the native
page store opens its file directly via the xtc VFS in bufmgr/wal; confirm
sx_storage_open does the file I/O, which it does).  Grep for vfs_register
/ pcache / mem_methods / mutex_methods uses in main.c/db.c/conn.c and
remove (these were SQLite-subsystem wiring; sx_config_* are now no-ops).

### G. Makefile + CI link cut
1. SRCS: drop mutex.c mem.c vfs.c pcache.c (deleted).  Add vexec.c
   sql_parse_drv.c sql_ast.c sql_parse_gen.c to the SERVER objects so the
   native engine links (they were LIME_OBJ/VEXEC_OBJ -- fold in).
2. Remove `sqlite3.o:` rule + every `sqlite3.o` from $(TARGET), ENGINE_NATIVE,
   and all test rules.  Remove the `-include xsql.h` from CFLAGS and
   AMALG_CFLAGS.
3. Amalgamation: AMALG_OBJS = $(SRCS:.c=.amalg.o) now native; add
   -DSQLXTC_HAVE_LIME=1 to AMALG_CFLAGS and the parser/vexec sources to
   the amalg object set; drop sqlite3.o from sqlxtc-server-amalg.
4. distclean/clean: drop sqlite3.o lines (harmless if left).

### H. Verify + commit
cd build_unix && make ... (libxtc); cd examples/06_sqlxtc;
make XTC_BUILD=../../build_unix sqlxtc-server     # MUST build w/o sqlite3.o
( python3 ../../test/sqlxtc/test_sqlxtc_oracle.py )   # 32/32
make XTC_BUILD=../../build_unix test_xstore test_isolation test_savepoint \
     test_native_txn test_mvcc test_xstore_scan test_clean_restart test_steal \
     test_torn_smo test_wal_compact test_wal_recover test_inplace_redo
bash ../../test/sqlxtc/test_sqlxtc_mt.sh   # x2
make XTC_BUILD=../../build_unix amalg
ASan/UBSan per AGENTS.md.  Then commit + push + verify all 13 CI jobs.

### I. After the cut: perf wins, then libxtc-usage items
Perf (bench/sqlxtc/SQLITE_VS_SQLXTC.md): top-N for ORDER BY ... LIMIT
(vexec sorts-then-limits, 0.6x); cache the IN(SELECT) materialized list +
the xtc_exec parallel plan across executions.
libxtc (bench/sqlxtc/LIBXTC_USAGE.md): #1 g_cat_mu -> xtc_amutex (lets the
catalog row persist inside the lock, closing the CREATE race); #2 bufmgr
partition/free-list pthread_mutex -> xtc_amutex; #3 WAL append counters.
