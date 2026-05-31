# sqlxtc -- design

This is the in-depth design document for `examples/06_sqlxtc`, a
networked, concurrent SQL database server built on libxtc.  It is the
authoritative overview; three companion documents in `../../docs/`
carry the deep-dives:

  * `M_SQLXTC_HARDFORK.md`  -- breaking SQLite into concurrent xtc_procs.
  * `M_SQLXTC_GREENFIELD.md`-- a clean-slate SQL engine on libxtc.
  * `M_SQLXTC_STORAGE.md`   -- the from-scratch storage engine.

`README.md` is the operator's guide (build, run, flags, tests); this
document explains *why the pieces are shaped the way they are*.


## 1. What sqlxtc is, and what it is becoming

sqlxtc started as a worked example: take the libxtc runtime and stand
up a multi-client TCP SQL server with SQLite as the storage and
execution engine.  Its first job is to exercise xtc's primitives --
processes, supervisors, mailboxes, locks, async I/O, resource caps --
against the workload shape a real database imposes, because whatever
works here generalizes to the eventual PostgreSQL-on-xtc port.

It is now becoming something more specific: **SQLite redesigned for
xtc**.  Stock SQLite is, by design, single-threaded at its core -- one
big serialized mutex, a synchronous VFS, a bytecode interpreter that
runs to completion on the calling thread.  Those were the right
choices for a zero-dependency embedded engine in the year 2000.  They
are the wrong choices for a server that should scale across cores.
sqlxtc keeps SQLite's best ideas -- the bytecode VM, the B-tree, the
pager, the VFS abstraction -- and re-seats them on libxtc: a unit of
mutable state plus the code that owns it becomes an `xtc_proc`, and
everything else talks to it by message; every blocking primitive
becomes a fiber-parking one so a single OS thread serves thousands of
connections without stalling.

Two strategies run in parallel toward that end (section 6):

  * **Hard-fork** -- progressively route the *existing* SQLite through
    xtc primitives at its documented seams, then decompose its layers
    into concurrent procs.  This is live today: the mutex, memory,
    page-cache, and file-I/O substrates already run on xtc.
  * **Greenfield** -- build a from-scratch, xtc-native storage engine
    (buffer manager + B-link tree) that will eventually replace
    SQLite's btree/pager/buffer-pool behind the same engine facade.
    This is live today as a tested foundation (section 5).


## 2. Process and concurrency model

The organizing idea of libxtc: **the session is the process.**

```
              accept()                          spawn
listen_fd ----------> listener_proc ----xtc_proc-----> conn_proc(fd)
                         (xtc_proc)                        |
                                                           v
                                            read frame -> quack_parse
                                            -> sql_parse pre-validate
                                            -> sx_ engine exec
                                            -> quack_emit row stream
                                                           |
                                                           v
                                                  send() to client
```

  * `main.c` brings up an `xtc_app` and a supervisor, applies the
    resource caps (`xtc_res`), and spawns the listener.
  * The listener `xtc_proc` owns the listening socket (`xtc_net`,
    reuseport) and spawns one `conn_proc` per accepted connection.
  * Each `conn_proc` is the client's session: its prepared statements,
    its transaction state, its per-connection database handle are the
    proc's private state.  No shared session table, no session mutex.
  * Many `conn_proc`s run as fibers on one (or, with the executor,
    a few) OS threads.  The whole point of the parking primitives
    below is that a session blocked on a lock or on disk yields the
    thread to its peers instead of stalling them.

The bound on concurrency is set by `xtc_res` caps: max clients
(connections refused at the cap, never queued unboundedly), max
memory, max in-flight queries (a token bucket), max attached
databases.  Predictable p99 and bounded resource use come from
refusing work, not from buffering it.


## 3. The Quack wire protocol

Quack (PROTOCOL.md) is a deliberately tiny line-delimited JSON
protocol over TCP, named in homage to DuckDB but not interoperable
with it.  A request is one JSON object per line:

```json
{"q": "SELECT id, v FROM t WHERE id > ?1", "params": [10]}
```

The server replies with a banner on connect, then for each request a
stream of row objects terminated by a status object.  `quack.c` is a
hand-rolled encoder/decoder -- no JSON library dependency -- because
the protocol surface is small and the parser must be auditable for a
network-facing server.  Bound parameters (int / text / null) are
supported; float and blob params are future work (they need a number
parser in the hand-rolled reader).

The protocol is intentionally minimal: it demonstrates the
per-connection proc pattern and the engine facade without the
complexity of a real wire protocol.  A PostgreSQL-wire front end is a
separate, larger exercise.


## 4. The engine facade and the four xtc substrate seams

### 4.1 The `sx_` facade

`engine.h` / `engine.c` define the `sx_` surface: `sx_open`,
`sx_close`, `sx_exec`, `sx_prepare` / `sx_step` / `sx_reset` /
`sx_finalize`, the `sx_bind_*` and `sx_column_*` families,
`sx_errmsg`, `sx_changes`, plus the global lifecycle (`sx_init`,
`sx_shutdown`) and the pre-init configuration calls
(`sx_config_mutex`, `sx_config_mem`, `sx_config_serialized`).

**The entire application speaks `sx_`.**  Only `engine.c` and the four
seam files name the vendored `sqlite3_*` symbols; `main.c`, `conn.c`,
`db.c`, `quack.c`, `sql_parse.c`, `metrics.c` are 100% SQLite-free.
This is the single boundary that matters for the greenfield path:
swapping the storage/execution backend for the from-scratch engine is
a rewrite of `engine.c` alone, against an unchanged application.

### 4.2 Concurrency policy

File-backed databases get a private handle per connection (concurrent
executions and concurrent WAL readers); in-memory databases share one
handle (private handles would each be a separate empty database).
WAL journal mode lets readers run concurrently with a writer; a
fiber-parking busy handler (section 4.3) makes contending writers
queue and retry rather than fail with BUSY.

### 4.3 Why every blocking point parks the fiber

The four seams exist to make one invariant hold: **no operation on a
shared engine handle may block the OS thread, because the thread is
shared by many sessions and the lock holder may itself be a parked
fiber on that same thread.**

  * `mutex.c` -- `sqlite3_mutex_methods` backed by `xtc_amutex`.  A
    contender for the connection mutex *parks* (yields the loop)
    rather than blocking; recursion is tracked by FIBER identity (the
    proc id from `xtc_self`), not thread id, because many procs share
    one thread.  A pthread mutex here would deadlock the loop: the
    holder might be a fiber parked mid-fsync on the same thread, which
    can only resume once the loop runs.

  * `mem.c` -- `sqlite3_mem_methods` backed by xtc's allocator
    (`__os_malloc` / `__os_realloc` / `__os_free`).  Every allocation
    the engine makes flows through xtc.  Two payoffs: xtc's allocator
    is a hookable vtable, so a host that supplies its own primitives
    (the PG port substituting an arena/slab) captures SQLite's
    allocations transparently, and the `xtc_alloc_audit` machinery can
    attribute them to the owning proc.  Each block carries a 16-byte
    header recording its size, because SQLite's `xSize` must report
    the usable size and the xtc allocator does not track it; the
    header width also keeps the returned pointer 16-byte aligned.

  * `pcache.c` -- `sqlite3_pcache_methods2` backed by an `xtc_slab`.
    Every page in one SQLite cache is the same size, so a per-cache
    slab supplies page bodies with no fragmentation and O(1)
    alloc/free; a chained hash indexes resident pages and an LRU of
    unpinned pages feeds recycling, so the resident set stays bounded
    by `cache_size` even when the working set is far larger.

  * `vfs.c` -- a `"sqlxtc"` `sqlite3_vfs`.  Reads, writes, and fsyncs
    are offloaded via `xtc_blocking_run`: the calling proc parks while
    a pool thread does the syscall and the loop keeps serving peers
    (off a loop it falls back to a synchronous call).  Per-file state
    uses the xtc allocator; I/O is counted and timed with `xtc_stats`
    (the `sqlxtc.vfs.*` and `sqlxtc.pcache.*` counters on the metrics
    line).  Path operations and byte-range file locks delegate to the
    base platform VFS so locking stays POSIX-correct -- which is why
    the current VFS is a *shim*, not a standalone OS layer (it borrows
    `xRandomness` / `xSleep` / `xCurrentTime` from the base VFS, so a
    full `SQLITE_OS_OTHER` substitution is a later step that must
    first re-home those onto `xtc`).

  * `sx_busy_handler` -- when a connection finds the database locked,
    it does NOT thread-sleep; it parks briefly (`xtc_proc_sleep`),
    which drains the run queue so the loop polls I/O and the lock
    holder -- possibly a parked fiber mid-offloaded-fsync on this same
    thread -- can resume, finish, and release.  A bare `xtc_yield`
    would keep the run queue hot and starve the I/O completion.

All four seams are *supported SQLite extension points*: they route
SQLite's internals through xtc without editing the 250k-line
amalgamation body.  The hard-fork plan lists them as the platform
shims that become redundant once xtc supplies the primitive.


## 5. The native storage engine (greenfield foundation)

A from-scratch, xtc-native storage engine is being built to replace
SQLite's btree/pager/buffer-pool behind the same `sx_` facade.  It is
studied from LeanStore (pointer swizzling, cooling-stage eviction) and
Karl Malbrain's threadskv B-link trees (slotted pages, prefix
compression, range cursors).  `M_SQLXTC_STORAGE.md` is the full
design.  What is built and tested today:

  * `bufmgr.c` -- a LeanStore-style buffer manager.  A *Swip* in the
    parent encodes HOT / COOL / EVICTED in its two top bits, so
    resolving a resident page is a pointer load with no hash lookup;
    a page-table path (pid -> frame hash) coexists for callers (the
    B-tree) whose child pointers are stable page ids.  A page-provider
    `xtc_proc` proactively unswizzles cold frames to COOL and writes
    dirty ones out ahead of demand, so reclaiming a frame is a cheap
    state flip; page I/O is offloaded so the loop never stalls.  The
    per-frame content latch is an `xtc_arwlock` (next paragraph).
    Tested standalone and under a 4-thread executor stress.

  * `btnode.c` -- a slotted node with prefix compression: the common
    prefix of a page's fence keys is stored once and each slot keeps
    only the key suffix plus a 4-byte head for fast comparison.

  * `btree.c` -- a B-tree KV store over the buffer manager and the
    slotted node, with **parallel writers** via fine-grained latch
    coupling (crabbing).  There is no global writer lock.  Writers
    exclusive-latch-couple from the root keeping a stack of held
    frames, release every ancestor above a "safe" node (one a child
    split cannot cascade into), and propagate a split UP through the
    already-held stack -- never acquiring a latch upward, so the
    scheme is deadlock-free.  Readers shared-latch-couple (latch the
    child before releasing the parent), so a reader is never split
    out from under and a miss is conclusive.  All latches are taken
    top-down in one order by every writer and reader.

The decisive enabler here is **`xtc_arwlock`**, a fiber-yielding
shared/exclusive latch added to the library for this work: its
contended waiters park rather than block the OS thread, so a latch may
be held across a child fix or a page-allocation park without wedging a
cooperative loop -- the property latch coupling requires.  `xtc_lwlock`
(thread-blocking) and `xtc_amutex` (exclusive-only) could not back it.

The from-scratch engine is **not yet wired into `engine.c`**; SQLite
is still the live backend.  Wiring `engine.c` onto the B-tree cursors
is the largest remaining piece and the moment SQLite's storage layer
is actually replaced.  Remaining storage stages: cursor prefetch +
skip-scan, WAL + recovery, then the SQL/VDBE integration.


## 6. The two evolution paths, and where we are

| | Hard-fork | Greenfield |
|---|---|---|
| Question | Make *existing* SQLite concurrent on xtc | What if SQLite were written on xtc from scratch? |
| Method | Route seams through xtc, then split layers into procs | Build an xtc-native buffer manager + B-tree + VDBE |
| Status | Mutex, memory, page-cache, file-I/O seams live | Buffer manager + B-link tree (parallel writers) live |
| Doc | `M_SQLXTC_HARDFORK.md` | `M_SQLXTC_GREENFIELD.md`, `M_SQLXTC_STORAGE.md` |

They are not in conflict: the hard-fork keeps a working server running
and proves the xtc primitives against SQLite's real workload, while
the greenfield engine grows underneath the same `sx_` facade until it
can take over storage.  The shared facade is what lets the swap happen
incrementally rather than as a big-bang rewrite.

Honest current limitation: SQL execution still runs on stock SQLite in
serialized mode, so it does not yet scale across cores -- the seams
make the *substrate* concurrent and loop-safe, not the bytecode
interpreter.  Multi-core SQL execution arrives with the layer
decomposition (hard-fork stages 2-4) or the greenfield VDBE.


## 7. Build, amalgamation, and tests

The example builds two ways, both in CI:

  * Against `libxtc.a` (the normal static library).
  * Against the **single-file xtc amalgamation** (`make amalg`): the
    sqlxtc sources are compiled `-Iamalg/include`, where forwarding
    stub headers resolve every `#include "xtc_*.h"` to one `xtc.h`,
    and link `sqlxtc-server-amalg` against a single `xtc.o`.  This
    exercises the amalgamation as a real, broad consumer.

SQLite itself (`sqlite3.c`, compiled once to `sqlite3.o`) is reused
as-is in both link configurations.

Tests are claim-driven and run in-process where possible (no daemon,
which the agent harness cannot drive safely):

  * `test_mem`        -- proves the engine allocates through xtc
                         (counts `__os_alloc` calls during a SQL
                         workload; asserts the workload is correct).
  * `test_concurrency`-- the xtc_amutex mutex parks, does not block.
  * `test_pcache`     -- the slab page-cache bounds the resident set.
  * `test_vfs` /
    `test_vfs_loop`   -- the offloaded VFS, off-loop and on-loop.
  * `test_parallel`   -- per-connection WAL handles, concurrent exec.
  * `test_bufmgr` /
    `test_bufmgr_mt`  -- buffer manager, single- and multi-threaded.
  * `test_btnode`     -- slotted node + prefix compression.
  * `test_btree` /
    `test_btree_loop` /
    `test_btree_mt`   -- the B-tree serially, on a loop with offloaded
                         I/O, and under interleaved parallel writers +
                         readers on a multi-loop executor.
  * `test_quack` /
    `test_sql_parse`  -- the protocol codec and the pre-parser.

Plus the end-to-end smoke test (`../../test/sqlxtc/`) against a real
running server.


## 8. File map

```
main.c          app/supervisor bringup, listener, resource caps, flags
conn.c          per-connection xtc_proc; read frame, dispatch, reply
quack.{c,h}     Quack JSON codec (hand-rolled, no dependency)
db.{c,h}        connection/handle management; Quack result streaming
sql_parse.c     SQL pre-parser (kind + readonly classification)
metrics.c       periodic xtc_res / xtc_stats snapshot logger

engine.{c,h}    the sx_ facade -- the ONLY SQLite boundary in the app
mutex.{c,h}     seam: sqlite3_mutex_methods  on xtc_amutex
mem.{c,h}       seam: sqlite3_mem_methods    on the xtc allocator
pcache.{c,h}    seam: sqlite3_pcache_methods2 on xtc_slab
vfs.{c,h}       seam: sqlite3_vfs            on xtc_blocking + xtc_stats

bufmgr.{c,h}    greenfield: LeanStore-style buffer manager
btnode.{c,h}    greenfield: prefix-compressed slotted node
btree.{c,h}     greenfield: B-tree, parallel writers via latch coupling

sqlite3.{c,h}   the vendored SQLite amalgamation (our fork's base)
amalg/          generated single-file xtc (gitignored)
lime/           Lime parser generator (submodule)
```


## 9. License

ISC, like the rest of libxtc.  The bundled SQLite amalgamation is
public domain.  The bundled Lime parser generator is BSD-2-Clause.
