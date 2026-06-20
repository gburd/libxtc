# SQLite reimagined on libxtc: a clean-slate design

The companion document `M_SQLXTC_HARDFORK.md` asks how to break the
*existing* SQLite into concurrent pieces.  This document asks the
inverse, and more interesting, question:

> If the people who wrote SQLite had started with libxtc, and liked
> its ideas, what would they have built?

SQLite's actual architecture is a product of its constraints in the
year 2000: a single-file, zero-dependency, embedded engine that had
to run anywhere a C compiler reached, with no threads it could rely
on.  Those constraints produced a brilliant design that is also,
deliberately, single-threaded at its core: one big serialized mutex,
a synchronous VFS, and a bytecode interpreter that runs to
completion on the calling thread.

libxtc removes those constraints.  A libxtc-native SQL engine would
keep SQLite's best ideas -- the bytecode VM, the B-tree, the pager,
the VFS abstraction -- but arrange them as cooperating processes
instead of one call stack behind one lock.  This is the design the
`examples/06_sqlxtc` example is evolving toward.

## SQLite's layers, as the starting material

SQLite's component layering is public and well understood:

  * **Tokenizer + parser** -- SQL text to a parse tree.
  * **Code generator** -- parse tree to VDBE bytecode.
  * **VDBE** -- a register-based virtual machine that executes the
    bytecode; the heart of query execution.
  * **B-tree** -- ordered key/value storage over database pages.
  * **Pager** -- the page cache plus the transaction/journal/WAL
    machinery that gives atomicity and durability.
  * **OS interface (VFS)** -- the pluggable layer that does actual
    file I/O and locking.

In stock SQLite these are linked into one library and, in serialized
mode, run one-at-a-time behind a connection (or database) mutex.  The
layering is clean; the concurrency is not, because it was never meant
to be.

## The libxtc-native arrangement

Start from libxtc's organizing idea: a unit of mutable state plus the
code that owns it is an `xtc_proc`, and everything else talks to it by
message.  Apply that to the layers above.

| Engine role | libxtc form | Why |
|-------------|-------------|-----|
| Connection / session | one `xtc_proc` per client | The session *is* the proc: its prepared statements, transaction state, and per-connection settings are the proc's private state.  No shared session table, no session mutex. |
| Parser + code generator | a pure function called inside the session proc | Parsing has no shared state; it stays on the calling proc's stack.  (sqlxtc already does this with the Lime-generated parser.) |
| VDBE execution | runs inside the session proc, yielding at I/O points | The VM steps bytecode; when an opcode needs a page that isn't resident, it does not block the thread -- it awaits a message from the pager proc and yields, so other sessions on the same loop run. |
| Page cache (buffer pool) | an `xtc_lrlock` over the page table | Readers find a resident page wait-free; the pager proc is the sole writer that installs and evicts pages.  This is the single highest-value substitution -- the page cache is the hottest read structure in any database. |
| Pager / WAL writer | one `xtc_proc` | Durability is inherently serial: the WAL is an append-only log with one writer.  Modeling the pager as a proc makes that serialization explicit and lock-free, exactly as kaka models a partition. |
| B-tree | fine-grained locks via `xtc_lockmgr`, keyed by page id | Concurrent readers and writers on different parts of the tree proceed in parallel; the lock manager's deadlock detector handles lock-order cycles that a hand-rolled scheme could not. |
| VFS / file I/O | `xtc_io` async submission | A page fault becomes an async read submitted to the loop's backend (io_uring / IOCP / kqueue); the faulting session awaits completion instead of blocking the thread. |
| Checkpointer, vacuum, stats | supervised background `xtc_proc`s | Long-running maintenance runs as its own proc under the engine's supervisor, restarted on fault, never blocking foreground work. |
| Resource limits | `xtc_res` | Bounded cache size, bounded in-flight I/O, bounded connections, with high-water callbacks -- the backpressure SQLite leaves to the embedding application. |
| Metrics | `xtc_stats` | Per-operation latency histograms and cache hit/miss counters, sharded per core. |

The shape that emerges: **sessions are processes, the pager is a
process, the page cache is a wait-free read structure, and the B-tree
is protected by a real lock manager.**  The big serialized mutex
disappears -- not by careful lock-splitting of existing code, but
because the state was never shared in the first place.

## What the message flows look like

A read query in the libxtc-native engine:

  1. The session proc parses + codegens (pure, on its own stack).
  2. The VDBE steps bytecode.  An opcode needs page P.
  3. It reads the buffer pool through the `xtc_lrlock`: if P is
     resident, it gets the page wait-free and continues -- no
     message, no yield.  This is the common case and it is fast.
  4. On a miss, it sends a `fault(P)` message to the pager proc and
     awaits the reply.  The session yields; other sessions run.
  5. The pager proc submits an async read via `xtc_io`, and when the
     completion arrives, installs P into the buffer pool (the single
     writer side of the lrlock) and replies to the waiting session.
  6. The session resumes where it yielded, page in hand.

A write transaction adds: the session acquires the relevant B-tree
page locks through `xtc_lockmgr` (deadlock-detected), modifies pages
in its private scratch, and at commit sends the dirty set to the
pager proc, which appends to the WAL and acknowledges.  The
checkpointer proc later folds the WAL back into the main file.

Nothing here holds an OS mutex across a yield; every wait is a
cooperative await that keeps the loop busy.

## How this differs from the stock engine

  * **No connection mutex.**  Sessions share nothing, so there is
    nothing to lock between them.
  * **Reads do not block on writes** at the page-cache layer.  The
    lrlock gives readers a stable snapshot of the page table while
    the pager installs new pages on the other copy.
  * **I/O does not block a thread.**  A page fault is an async
    submission; the faulting session yields rather than stalling its
    OS thread, so one thread serves thousands of sessions.
  * **Lock order is enforced, not assumed.**  The B-tree's
    fine-grained locks go through `xtc_lockmgr`, whose deadlock
    detector aborts a victim on a cycle instead of hanging.
  * **Background work is supervised.**  Checkpoint, vacuum, and
    analyze are processes under a supervisor, not callbacks the
    application must remember to drive.

## Why SQLite did not do this (and why that was right)

None of the above was available or appropriate for SQLite's mission.
An embedded engine that must link into a phone app cannot assume an
event loop, cannot spawn background threads the host did not ask for,
and must behave identically whether or not threads exist.  SQLite's
serialized core is the correct answer to *its* problem.  The point of
this exercise is not that SQLite is wrong, but that a *server-class*
SQL engine -- one that owns its process and wants to serve thousands
of concurrent connections with low tail latency -- would be built
very differently if libxtc existed when it started.  That server-class
engine is what sqlxtc is exploring.

## Mapping onto the sqlxtc example today

`examples/06_sqlxtc` currently embeds stock SQLite behind one
`xtc_lwlock`, with one session proc per connection and the Lime
parser doing pre-parse validation.  It has the session-as-proc shape
already.  The path from here to the libxtc-native design, in order of
value:

  1. **Page cache behind `xtc_lrlock`.**  Replace SQLite's pcache
     with a custom `sqlite3_pcache_methods2` implementation backed by
     an `xtc_lrlock` page table.  This is a supported SQLite
     extension point, so it needs no fork -- the single highest-value
     step, and the one that proves the read-concurrency claim.

     *Done (slab-backed form):* `pcache.c` registers a custom
     `sqlite3_pcache_methods2` whose page bodies come from a per-cache
     `xtc_slab`.  Every page in one SQLite cache is the same size
     (header + szPage + szExtra), which is exactly a single
     object-size class -- no fragmentation, O(1) alloc/free.  A
     chained hash table maps the page key to its resident page and an
     LRU list of unpinned pages feeds recycling.  Hit/miss/recycle and
     live-page counts go to `xtc_stats` (`sqlxtc.pcache.*`).
     `test_pcache` drives a 5000-row insert + repeated scans on an
     in-memory database (whose storage *is* the page cache) and
     asserts the slab served the pages (66 allocations), lookups hit
     (10251 hits / 66 misses), and every page is reclaimed on close
     (no leak) -- ASan/UBSan clean, against both `libxtc.a` and the
     amalgamation.  The pcache methods always run under SQLite's
     mutex, so the implementation needs no internal locking.

     The `xtc_lrlock` page table -- the COW snapshot that lets readers
     traverse a stable page set while a writer publishes a new one --
     is the refinement that unlocks read concurrency, and is premature
     only until sqlxtc leaves SQLITE_CONFIG_SERIALIZED.  The slab
     pcache is the resident-set substrate it builds on.
  2. **Async VFS via `xtc_io`.**  Implement `sqlite3_vfs` over
     `xtc_io` so page reads submit to the loop and the session awaits
     completion.  Also a supported extension point.

     *Done (offloaded instrumented form):* `vfs.c` registers a
     `"sqlxtc"` VFS, a shim over the platform default.  Every byte of
     database I/O flows through it -- per-file state is allocated with
     the xtc allocator, and reads, writes, and syncs are counted and
     timed with `xtc_stats` (the `sqlxtc.vfs.*` counters and latency
     histograms, surfaced on the periodic metrics line).  Path ops and
     the byte-range file locks delegate to the base VFS so locking
     stays POSIX-correct.  `db.c` opens file-backed databases through
     it; `test_vfs` drives a 500-row create/insert/select in
     process (no daemon) and asserts the I/O actually went through the
     VFS, against both `libxtc.a` and the amalgamation.  Reads,
     writes, and fsyncs are offloaded with `xtc_blocking_run`: on a
     loop the calling process parks while a pool thread runs the
     syscall, so the reactor keeps serving peers (off a loop it falls
     back to a synchronous call).  `test_vfs_loop` proves the loop
     stays live during a file-backed workload by ticking a heartbeat
     process throughout.  This is safe because the SQLite mutexes now
     park instead of thread-blocking (`mutex.c` over
     `xtc_amutex`), so a backend can hold the handle mutex across an
     offloaded read without wedging a contender.  The next step --
     submitting reads to `xtc_io` directly rather than a pool thread
     -- plugs into this same `vfs_read` choke point.

     *Foundation landed:* `xtc_blocking` (a worker pool that runs a
     blocking call and parks the calling process via a pipe +
     `xtc_proc_wait_fd`) is the mechanism `xv_read`/`xv_write`/`xv_sync`
     will use to offload file I/O without stalling the loop.  But
     wiring it into the *shared-handle* server is not yet safe: a
     process that parks mid-statement while holding SQLite's mutex
     (today an `xtc_lwlock`, which `pthread_cond_wait`s -- it blocks
     the OS thread, not the fiber) would wedge the loop the instant a
     second connection's process tries to take that mutex.  The
     parked process can only be woken on that same loop thread, which
     is now blocked: deadlock.  Two deadlock-free routes:
       * a fiber-yielding lock (park the *task*, not the thread --
         the `xtc_amutex` "M11+" TODO), so a process holding the lock
         can park and the loop keeps running; or
       * per-connection SQLite handles (the existing `--no-shared`
         mode): independent handles share no in-process mutex, and
         the VFS xLock byte-range locks coordinate them, so a parked
         I/O never blocks another connection.
     The offload is correct today for any single SQLite user; the
     server-wide wiring waits on one of those two.

     *Update:* `xtc_amutex` now yields the fiber when contended (it
     parks the calling process and hands the lock off on unlock,
     rather than `pthread_cond_wait`ing the OS thread).  That removes
     the first blocker: with SQLite's mutex backed by `xtc_amutex`, a
     process can park mid-statement (on a `xtc_blocking` offload)
     while holding the lock, and a second connection that contends
     parks instead of wedging the loop -- the loop stays live to wake
     the holder.

     *Done:* sqlxtc's `sqlite3_mutex` is now backed by `xtc_amutex`
     (`mutex.c`, recursion tracked by fiber identity), and
     `vfs_read`/`vfs_write`/`vfs_sync` are routed through
     `xtc_blocking_run`.  `test_concurrency` proves the parking
     contract (a holder parks while holding; a contender parks, not
     thread-blocks; the holder resumes and hands off), and
     `test_vfs_loop` proves the loop keeps serving a heartbeat process
     while a file-backed workload runs its offloaded I/O.
  3. **Pager as a proc.**  Route durability through a single pager
     proc so the WAL writer is an explicit owner.
  4. **Fine-grained locks via `xtc_lockmgr`** -- the deep step that
     does require forking SQLite's B-tree, covered in
     `M_SQLXTC_HARDFORK.md`.

Steps 1 and 2 are reachable without forking SQLite at all, because
the pcache and VFS are designed to be replaced.  They are the natural
next phase for the sqlxtc example and would demonstrate the
read-concurrency and async-I/O claims on a real SQL workload.

## Status

Design note.  The sqlxtc example implements the session-as-proc and
parser-as-pure-function pieces today, plus the instrumented `"sqlxtc"`
VFS (step 2, with blocking I/O offloaded off the loop) and the
slab-backed page cache (step 1 in its resident-set form); the
lrlock-COW page table and the async
read-submission refinement of the VFS are the proposed next phase,
tracked in PLAN.md.

## Front-end replacement: retiring the VDBE (the other axis)

The steps above replace SQLite's OS-facing layers (pcache, VFS,
mutex) while keeping its parser + planner + VDBE.  A parallel effort
replaces the FRONT end -- the parser, the planner, and the executor --
so that eventually nothing of sqlite3.c is on the live path and the
amalgamation (and the xsql_* rename that hides it) can be deleted.
This is the axis tracked in M_SQLXTC_VEXEC.md; its live-path state and
the remaining gates are recorded here so the teardown order is
explicit.

What is on the live path now (after the Lime + vexec wiring):
  - **Parser.**  Lime parses the statement once for routing
    (sql_parse classifies kind / read-only from the AST; its grammar
    is a strict subset of SQLite's, so an unparseable statement falls
    through to a permissive keyword classifier and still reaches the
    engine).  vexec parses the same statement with Lime to build its
    plan.  SQLite ALSO still parses every query (for execution and for
    column names).
  - **Executor.**  db_exec_cached tries vexec first for param-free
    read-only queries; vexec serves the recognized shapes (scan /
    filter / project / scalar expressions / hash aggregation +
    GROUP BY / ORDER BY + LIMIT / one INNER hash join) over the xstore
    B-tree with no VDBE on the hot path, and names its own output
    columns (alias / bare column / an expression's verbatim source span
    -- step 4 done), falling back to the VDBE only for a query it does
    not recognize.  test_db_vexec proves the live response is identical
    whichever engine serves it (ordered queries byte-identical,
    unordered as a row multiset -- an unordered result may legally come
    back in a different row order from each engine).
  - **Writes.**  db_exec_cached also tries a native write path
    (vx_run_write -> xstore_write_txn / xstore_delete_rec, no VDBE / no
    vtab) for: INSERT INTO t VALUES (literal rows); DELETE / UPDATE by a
    primary-key point or range; and DELETE / UPDATE by an arbitrary
    vexec-compilable WHERE (non-pk columns, compound AND/OR) via a
    scan-evaluate.  Writes apply at one commit timestamp per row,
    WAL-durable, byte-identical to the VDBE-driven vtab path.  Native
    writes inside a BEGIN..COMMIT buffer into the connection's shared
    transaction and commit / abort atomically (db_native_txn_end flushes
    the native buffer around COMMIT/ROLLBACK); a transactional
    DELETE/UPDATE falls back to the VDBE for read-your-writes.  Reads
    push a primary-key WHERE constraint down to a bounded scan (a point
    read seeks rather than full-scanning).  All-or-nothing: an
    unrecognized write writes nothing and falls back.
  - **Everything else is still SQLite:** the verbatim name of an
    EXPRESSION column the AST cannot span (rare edge); writes outside
    the recognized shapes (INSERT...SELECT, REPLACE, an explicit column
    list, a pk reassignment, a transactional DELETE/UPDATE); all DDL;
    all parametrized statements; and every read query vexec does not
    recognize -- all flow through the VDBE.

The gates to actually deleting sqlite3.c, in dependency order:

  3. **A native storage path -- drop the vtab round-trip.**  Largely
     DONE: native write primitives (xstore_write_txn /
     xstore_delete_rec / xstore_commit / xstore_rollback / max_rowid /
     table_id) apply INSERT, pk-point/range and arbitrary-predicate
     DELETE/UPDATE, and multi-statement transactions without the VDBE
     or the vtab.  REMAINING: INSERT...SELECT, REPLACE, explicit column
     lists, pk-reassigning UPDATEs (old-key tombstone), and
     read-your-writes inside a transaction (so a transactional
     DELETE/UPDATE can also go native rather than fall back).
  4. **Native column naming + a minimal planner + native catalog.**
     DONE: vexec names alias / bare-column / verbatim-source-span
     expression columns itself; a minimal read planner pushes a
     primary-key WHERE down to a bounded scan; the xstore catalog now
     stores the full column schema (names / types / pk) so the native
     read and write paths resolve schema WITHOUT sqlite_master
     (xstore_table_schema), and SELECT * expands from it.  REMAINING
     (optional): index selection beyond the rowid, join-order costing.
  5. **Retire sqlite3.c.**  The deep remaining step.  As long as the
     VDBE is the fallback, SQLite must still parse every statement and
     keep each table in sqlite_master (the fallback path resolves names
     there), so DDL (CREATE / DROP) cannot go native yet -- it is LAST,
     not next.  The order is: keep shrinking the VDBE-fallback surface
     (the step-3/4 REMAINING items, plus parametrized statements and the
     remaining read shapes), and only when the native parser + planner +
     read + write engine covers the ENTIRE accepted surface can the VDBE
     fallback be deleted -- then native DDL, then drop xsql.h, sqlite3.c
     / .h, vfs.c / pcache.c / mutex.c / mem.c (they exist to plug INTO
     SQLite), and the residual xsql_* calls in engine.c.  The sqlite3_*
     names leave the tree automatically when the code they rename is
     gone -- they are the last thing removed, not the first.

Where the live path stands today: most single-table reads (scan /
filter / project / scalar + aggregate + GROUP BY + HAVING [incl. over a
non-selected aggregate] + count(DISTINCT) + ORDER BY / LIMIT / DISTINCT
[+ ORDER BY + LIMIT] / INNER + LEFT + RIGHT + FULL join / SELECT * /
UNION [ALL] / INTERSECT / EXCEPT / IN (list) / NOT IN / IN (SELECT) /
NOT IN (SELECT) / uncorrelated scalar subqueries / BETWEEN / CASE /
LIKE / GLOB) run with NO VDBE and NO vtab, naming columns and resolving
schema natively.
The common writes that are native: INSERT (positional, explicit column
list, REPLACE, and INSERT...SELECT), DELETE/UPDATE by
pk-point/range/arbitrary-predicate, pk-reassigning UPDATE (move to a
free key), and -- in a transaction -- INSERT always, and DELETE/UPDATE
while the target table is still clean in the txn.  Parametrized (?)
reads and writes are native too.

The read long-tail still on the VDBE, in rough order of effort:
  - DISTINCT / set-op + LIMIT WITHOUT ORDER BY -- the surviving subset
    is unspecified in SQL, so not byte-reproducible; with ORDER BY it
    is native.  (Intentional fallback, not a gap.)
  - Correlated subqueries: a correlated SCALAR subquery in a projection
    or equality is native on the single-table read path (outer refs ->
    ? binds, re-run per row).  A FROM-clause DERIVED table is native
    (single derived table, no join, no correlation, no outer GROUP BY /
    ORDER BY / LIMIT / DISTINCT).  Still falling back: a correlated
    subquery as a COMPARISON operand (a < (subquery) -- dynamic result
    affinity), correlated IN (SELECT), correlation on the join path,
    and a derived table joined to other tables / with outer ordering.
  - 3+ table INNER joins are native (N-way hash-join pipeline,
    vx_try_prepare_njoin); 3+ table OUTER joins fall back.

NOTE -- two tracks to retire sqlite3.c.  Track A (recognition): make
every accepted query native (the long-tails above + DDL).  Track B
(execution): stop the native paths from CALLING SQLite --
  * DONE: the join build/probe/stream sources read via xstore_scan_*
    for xstore tables (per-side vx_jsrc_t; the SQLite-cursor path
    remains only for plain non-xstore tables).
  * the inner SELECT of an uncorrelated / correlated subquery, an
    INSERT...SELECT, and a derived table still run via SQLite (these
    are arbitrary nested queries; running them through the engine
    recursively is the remaining Track-B work).
  * resolve_schema prefers the native catalog (PRAGMA only for non-
    xstore tables); parallel rowid bounds still use a SQLite
    min/max(_rowid_) probe at prepare time (not the hot path).
Both tracks must complete before the amalgamation can be deleted.

The write long-tail still on the VDBE:
  - a transactional DELETE/UPDATE on a table the txn has ALREADY
    written to (dirtied): the vtab's point path could merge the wbuf
    where the native committed scan would not, so it falls back.
    INVESTIGATED and deliberately LEFT as a fallback: the VDBE's own
    xstore range scan reads COMMITTED values only (it does not merge
    the wbuf), so a second range UPDATE to a row already updated in the
    same txn reads the stale committed value and CLOBBERS the first
    write (verified: BEGIN; UPDATE t SET a=999 WHERE k=2; UPDATE t SET
    a=a+1 WHERE k<=3; COMMIT leaves k=2 at 21, not 1000 -- the 999 is
    lost).  A native path that merged the wbuf would be MORE correct
    than this and thus DISAGREE with the fallback, which the
    correct-by-fallback rule forbids; reproducing the VDBE's
    clobbering exactly is not worth it.  This is a latent xstore-vtab
    read-your-writes anomaly on the range path, tracked separately --
    when it is fixed in the vtab, native in-txn DML on a dirty table
    can follow.
  - a pk reassign whose new key collides with an existing row, or that
    matches more than one row (both UNIQUE violations) -- falls back so
    the VDBE raises the error; the single-row move to a free key is
    native.
  - INSERT...DEFAULT VALUES, and a non-integer target.

(The xstore vtab xUpdate rowid-move + auto-rowid bugs that previously
blocked native pk-reassign were fixed; the vtab fallback now moves the
row and assigns auto rowids correctly, so native pk-reassign agrees
with it.)

DDL (CREATE / DROP / ALTER) is LAST: the VDBE fallback requires SQLite
to keep every table in sqlite_master, so DDL cannot go native until the
fallback is removed -- which needs the entire read + write surface
above to be native first.  Then the VDBE fallback comes out, native
DDL goes in, and sqlite3.c / the vfs/pcache/mutex shims / the xsql_*
rename are deleted.

Each gate is differential-tested byte-identical (positional for ordered,
multiset otherwise) against the VDBE and clean under ASan+UBSan.

## Teardown checklist -- the ordered, measured path to deleting sqlite3.c

This is the exact remaining sequence (each step a verified, CI-green
unit).  Steps 1-2 are independent; 3 gates on both; 4-5 are mechanical.

### 1. Track A -- recognize the rest of the accepted read/write surface
  - correlated subquery as a COMPARISON operand (a < (subquery)):
    DONE -- the comparison gate now lets a dynamic-type operand (a
    correlated subquery or CASE) compare against a numeric/text side,
    with SQLite numeric-affinity coercion at eval (coerce_numeric).
  - correlated IN (SELECT): DONE -- VXO_CORRIN re-runs the inner
    select per row (build_corr_stmt shared with the scalar form) and
    tests membership with 3-valued NULL semantics; NOT IN supported.
  - postfix NOT NULL predicate (expr NOT NULL): DONE -- a grammar rule
    reusing TK_NOT TK_NULL builds the same SX_E_IS_NULL node as IS NOT
    NULL (no new conflict, byte-stable regen).
  - ORDER BY over aggregated output (GROUP BY ... ORDER BY): DONE --
    agg_order_limit sorts the drained agg chunk by output-column keys
    (positional or expr_same-matched).  ORDER BY + LIMIT/OFFSET on the
    agg path falls back (tie + limit is not byte-reproducible).
  - 3+ table OUTER joins: extend the N-way pipeline (vx_try_prepare_
    njoin) with NULL-extension + matched-tracking per side.  Large +
    high-risk (output ordering must match the VDBE's nested-loop order
    on a depth-first backtracking cursor stack); falls back correctly
    today, so this is a nativeness gap, not a correctness gap.
  - a derived table joined to other tables / with outer ORDER BY /
    GROUP BY: compose the derived source into the join + ordered paths.
  - remaining writes: transactional DELETE/UPDATE on a DIRTIED table
    (needs wbuf-merge into the native read), INSERT...DEFAULT VALUES
    (DONE -- one auto-PK all-NULL row, xstore honors no DEFAULT clause),
    non-integer / colliding / multi-row pk reassign.

### 2. Track B -- stop the native paths from CALLING SQLite
The 79 sqlite3_* calls left in vexec.c, by purpose (grep
'sqlite3_prepare_v2' vexec.c):
  - join-source column metadata (build/probe/src_sql prepares): prepared
    ONCE per side at plan time only to learn column count + affinity;
    the per-row read is already native (vx_jsrc_t).  DONE -- jsrc_build
    now fills each native side's affinity from the catalog, so an
    all-xstore join plans + executes with NO sqlite3_prepare at all;
    only a non-xstore side (plain SQLite table) still prepares.
  - subquery / derived-table / INSERT...SELECT inner SELECTs
    (compile_scalar_subquery, compile_corr_subquery,
    vx_try_prepare_derived, the INSERT...SELECT path): run an arbitrary
    nested SELECT.  Native means RECURSIVELY running the inner select
    through vexec (vx_run / vx_run_p) instead of sqlite3_prepare/step.
    DONE: the uncorrelated scalar subquery, the FROM-clause derived
    table, the INSERT...SELECT source, AND the correlated scalar / IN /
    NOT IN per-row re-execution all EXECUTE via vexec now (correlated
    keeps the parameterized inner text in st->corrsql[] and runs it per
    row via vx_run_p, binds typed from the outer row).  Per-output-col
    affinity on vx_result (st->outaff) makes the outer/IN comparison
    gate exact with no SQLite metadata.  NO inner SELECT is executed
    through SQLite any more.  The ONE remaining prepare in the live
    read path is the correlation GATE in compile_scalar_subquery
    (prepared, never stepped -- purely "is this correlated?"), blocked
    on a scoped name resolver (an outer-alias vs inner-alias ref to the
    same base table), not on execution.
  - resolve_schema PRAGMA table_info (line ~5185): only the non-xstore
    fallback; xstore tables already use the native catalog.
  - parallel rowid bounds SELECT min/max(_rowid_) (line ~4941): replace
    with a native xstore min/max scan.  DONE -- the morsel range now
    comes from xstore_max_rowid (lo=1, hi=max+1; workers re-check
    visibility so a tombstone-inclusive bound is safe), no SQLite call.

### 3. Remove the VDBE fallback
Only once 1+2 leave NO query reaching sx_step: delete the
fall-through in db_exec_cached (db.c ~line 689).  The column-name
borrow is already GONE -- vexec names every output column itself from
the AST select-item source span (verified: instrumenting the borrow
across the whole corpus showed it is never taken; test_db_vexec now
asserts sx_vexec_name is non-NULL for every column of every recognized
query, and emit_vexec no longer takes the VDBE statement).  What still
keeps a VDBE prepare on the served path is ONLY the correlation gate in
compile_scalar_subquery (prepared, never stepped) -- a scoped name
resolver removes that; and the fallback prepare itself for an
UNrecognized query, which this step removes.

### 4. Native DDL
With the fallback gone, CREATE/DROP/ALTER no longer need sqlite_master.
Add native DDL (CREATE VIRTUAL TABLE already creates the xstore
catalog entry; generalize to CREATE TABLE, DROP, ALTER over the native
catalog).

### 5. Delete the amalgamation
Remove sqlite3.c / sqlite3.h, vfs.c / pcache.c / mutex.c / mem.c (the
SQLite OS shims), the xsql_* -> sqlite3_* rename header, and the
sqlite3.o build rule.  The xsql_* names vanish automatically when the
code they rename is gone.  Re-run the full examples CI job.

## What SQLite still load-bears (the honest excision scope)

The vexec/native-write fast paths sit ON TOP of SQLite: every query
that is not recognized, plus all DDL, transactions, and the table
plumbing itself, still run on the VDBE.  sqlite3.c (257k lines)
currently provides FIVE subsystems that must each be replaced natively
before it can be deleted -- this is the project's ultimate "replace the
front-end" goal and spans multiple milestones, NOT a single change:

  A. **Virtual-table dispatch.**  xstore is registered via
     xsql_create_module_v2 and driven ENTIRELY by SQLite's vtab
     callbacks -- xBestIndex / xFilter / xNext / xColumn / xUpdate /
     xBegin / xSync / xCommit / xRollback.  Without SQLite there is no
     caller for these.  A native executor must own table open / scan /
     point-read / write and call into the xstore B-tree directly
     (vexec + the native write path already do the data part; what is
     missing is the DISPATCH -- deciding per statement which native
     routine runs, with no VDBE program).

  B. **A statement driver / connection lifecycle.**  xsql_open_v2,
     prepare_v2, step, reset, finalize, bind, column_*, errmsg,
     changes64, busy_handler -- the sx_* bridge in engine.c maps these
     1:1 onto SQLite.  A native sx_* implementation must parse (Lime),
     plan (vexec / native-write / DDL), and iterate results without a
     VDBE program, while keeping the exact sx_* ABI conn.c/db.c expect.

  C. **Transaction + savepoint manager.**  BEGIN / COMMIT / ROLLBACK /
     SAVEPOINT / RELEASE currently go through SQLite's txn manager,
     which calls the vtab xBegin/xSync/xCommit/xRollback that drive the
     xstore txn (snapshot, wbuf, savepoint stack).  Native control must
     call xstore_txn_* directly and own autocommit vs explicit txn
     state.  (db_native_txn_end already flushes native writes at
     COMMIT/ROLLBACK -- the recognizer exists; the OWNERSHIP does not.)

  D. **DDL + catalog.**  CREATE / DROP / ALTER are parsed + executed by
     SQLite, which persists schema in sqlite_master and creates the
     vtab.  Native DDL needs its own persisted catalog (xstore already
     has a per-table column catalog; it must become the source of
     truth, persisted + recovered) and a DDL executor over it.  A plain
     CREATE TABLE is today rewritten to CREATE VIRTUAL TABLE xstore and
     run on the VDBE; that rewrite + vtab-create must become native.

  E. **PRAGMA / introspection / misc.**  journal_mode, synchronous,
     table_info (already off the native read/write paths), and any
     PRAGMA a client sends.  Mostly no-ops or native-catalog reads once
     A-D exist.

Realistic sequence: finish the recognition gaps (3+ outer joins,
derived-with-join) and the correlation gate (B-adjacent) FIRST, since
they are bounded; then build B (the native statement driver) with A
(native vtab-free dispatch) together, because the driver is what
replaces the VDBE program; then C and D on top of the driver; E falls
out; then step 5 deletes the amalgamation.  Each is its own milestone
with its own differential tests against a reference SQLite kept OUTSIDE
the build (so correctness can still be checked after sqlite3.c is gone
from the shipped engine).
