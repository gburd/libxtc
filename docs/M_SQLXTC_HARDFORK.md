# sqlxtc -- breaking SQLite into concurrent xtc_procs

This document is the architectural plan for hard-forking SQLite to run
its internal subsystems as separate concurrent xtc_procs, removing the
giant serialized mutex on reads, removing the writer mutex by
restructuring the write path, and routing all I/O through xtc's async
primitives.  The goal: a SQLite-compatible database server that
genuinely scales across multiple cores -- something stock SQLite
explicitly does not do.

This is a large project.  It will be done incrementally over many
rounds.  Each round delivers a working server; the scaling improvement
arrives in stages.

## Why we're doing this

* The current sqlxtc embeds vanilla sqlite3.c in serialized mode.  All
  database access funnels through one mutex.  Concurrent reads
  serialize; concurrent writes block each other; CPU usage caps at one
  core regardless of how many connections we accept.

* This is fine as a demonstration of the Quack protocol, the xtc_proc
  per-connection pattern, and the xtc_lwlock-as-SQLite-mutex
  integration.  It is not fine as a production database server.

* Real concurrent databases (PostgreSQL, MySQL, DuckDB) decompose the
  work: storage layer, B-tree pages, query planner, query executor,
  statistics, WAL writer, autovacuum -- each runs concurrently against
  shared structures protected by fine-grained locks (or lock-free
  data structures, or per-connection caches).

* sqlxtc is a chance to exercise xtc's primitives -- xtc_lwlock,
  xtc_lrlock, xtc_lockmgr, xtc_proc, xtc_chan, xtc_slab, xtc_io --
  against the workload pattern that PostgreSQL would impose.  Whatever
  works in sqlxtc generalizes to the eventual postgres-on-xtc port.

## SQLite's internal architecture (the source we're forking from)

SQLite is conceptually layered:

```
+-----------------------------------------------+
|  SQL parser, planner, optimizer (parse.c)     |
+-----------------------------------------------+
|  VDBE (virtual database engine; bytecode)     |
+-----------------------------------------------+
|  B-tree (btree.c) -- table & index storage    |
+-----------------------------------------------+
|  Pager (pager.c) -- transactions, WAL, locks  |
+-----------------------------------------------+
|  OS interface / VFS (os_unix.c, os_win.c)     |
+-----------------------------------------------+
```

In serialized mode every layer-internal operation is wrapped in one
of a handful of recursive mutexes (the connection mutex, the
"unconfigurable" mutex for the global allocator, etc.).  Concurrency
boils down to: each backend (thread holding a sqlite3*) waits in line.

## The hard-fork plan -- five stages

### Stage 1: extract the layers as named subsystems

Today, every sqlite3.c source file is compiled into one .o.  All
internal symbols are visible across the layer boundary.  The pager
calls into the OS layer directly; the B-tree calls into the pager
directly; etc.

Step 1 is a refactor without functional change: build sqlite3 as
**five separate static libraries** -- libparser.a, libvdbe.a,
libbtree.a, libpager.a, libsqlite_os.a -- with explicitly defined
inter-layer call surfaces.  Cross-layer calls go through a vtable;
within-layer calls stay direct.

This is mostly mechanical.  Sets us up for stage 2.

### Stage 2: each subsystem becomes a server xtc_proc

Each layer's API becomes a request/response interface served by a
dedicated `xtc_svr` (gen_server-style xtc proc).

```
                   client conn proc
                        |
                        v  Quack request
                  +------------+
                  | dispatcher |  decides which subsystem(s) to talk to
                  +------------+
                   |    |    |
                   v    v    v
              +-----+ +-----+ +-----+
              |parse| | plan| |exec |       (multiple instances of each)
              +-----+ +-----+ +-----+
                                  |
                              vdbe-bytecode
                                  |
                                  v
                              +-------+
                              | btree |     (multiple instances; sharded by table)
                              +-------+
                                  |
                                  v
                              +-------+
                              | pager |     (one per database file)
                              +-------+
                                  |
                                  v
                              +-------+
                              | vfs   |     (one per database file)
                              +-------+
```

Cross-layer calls become message sends.  This adds latency per call
but enables genuine concurrency: while pager A is waiting on disk,
btree B can serve a different lookup.

Stage 2's payoff is *only* in workloads that are I/O bound.  CPU-bound
workloads (everything-in-RAM) get slower because we've added
inter-process hops.  This is fine; CPU-bound SQLite was already not
the differentiator.

### Stage 3: read concurrency via xtc_lrlock at the buffer-pool layer

The big win.  Replace the pager's single buffer-pool mutex with
xtc_lrlock semantics:

* Reads lock the buffer pool's read copy and execute wait-free.
* Writes mutate the write copy, append to the oplog, and publish.
* The oplog is replayed onto the now-stale copy after the swap drains.

For SQLite specifically: each page in the buffer pool has its own
xtc_lrlock (page-level; 4096-byte pages, so the overhead is modest).
Multiple connections reading the same page concurrently see consistent
snapshots; one writer can mutate a page while readers continue against
the prior version.

Constraints:

* The lrlock pattern requires deterministic apply_op functions.  Page
  writes are deterministic (write-byte-N-to-value-V), so this fits.
* Memory cost doubles per page (two copies).  COW mode (already in
  xtc_lrlock) brings the steady state back to one copy.
* Need to handle page eviction carefully -- can't evict a page with
  active readers.

### Stage 4: writer concurrency via fine-grained btree locks + xtc_lockmgr

The harder win.  SQLite's writer mutex exists because it serializes
B-tree restructuring (page splits, rebalances).  Replace with:

* Per-table xtc_lockmgr lock objects.  Each transaction acquires a
  WRITE lock on the tables it touches.  The lockmgr's deadlock
  detector handles cycles automatically.
* Per-page xtc_lwlock for the actual page-level mutation.  Held only
  while the page is being modified.
* WAL writes serialize at the WAL level only (one WAL writer); but
  since WAL append is fast, this is rarely the bottleneck.

The btree's split/rebalance code needs auditing for non-deterministic
choices that could deadlock under concurrent updates.  The xtc_lockmgr
handles cycles; we need to ensure the choices are correct (acquire
locks in a stable order: by page-id ascending).

### Stage 5: async I/O via xtc_io VFS

Replace SQLite's default `unix.c` VFS with one whose `xRead` and
`xWrite` route through xtc_io:

* `xRead` becomes `xtc_io_read_async(fd, buf, len, offset)` and the
  calling proc yields until the read completes.  When the I/O backend
  is io_uring, this is genuinely zero-copy with completion delivered
  via xtc_io_poll.
* `xWrite` similarly.
* `xSync` (fdatasync) becomes a barrier; the calling proc yields
  until the I/O backend's sync completion arrives.

This is the primary place where xtc_io's async path pays off: the
worst SQLite workload is write-heavy with sync-after-each-commit; with
async I/O the calling proc yields while waiting for fsync, freeing the
CPU for other connections.

The "critical writes" the user asked about (writes that must
synchronize before completion) get the async-wait pattern: register
the write, yield until completion.  Non-critical writes (e.g., stats
updates that can be lost on crash) skip the sync.

## Subsystem-by-subsystem disposition

| Subsystem | Refactor | Lock primitive | xtc_proc shape |
|---|---|---|---|
| Parser (parse.c) | Stateless; one parse per request | None | Pool of N parser procs |
| VDBE (vdbe.c) | Per-statement state on stack | None | Pool of N executor procs |
| Btree | Per-table state | xtc_lockmgr (table) + xtc_lwlock (page) | One btree proc per table; shared across connections |
| Pager / buffer pool | Big shared cache | xtc_lrlock per page | One pager proc per DB file |
| WAL | Append-only log | xtc_lwlock (single writer) | One WAL writer proc per DB file |
| VFS | Stateless | None | xtc_io-driven; no proc, just async wrappers |
| Statistics | Append-mostly | xtc_stats counters | Same proc as caller |
| Vacuum | Background work | None (or table-level lockmgr) | One vacuum proc per DB file |
| Schema cache | Read-mostly catalog | xtc_lrlock | One schema proc per DB; clients consult it |

## Work that's redundant after the rewrite

SQLite carries a lot of code to compensate for missing runtime
primitives.  When xtc supplies the primitive, the SQLite code becomes
redundant:

| Stock SQLite | Replaced by xtc | What we delete |
|---|---|---|
| `sqlite3_mutex_methods` | `xtc_lwlock` | The platform mutex shims (~500 LOC) |
| `sqlite3_mem_methods` | `xtc_slab` + `xtc_res` | The platform malloc shims (~300 LOC) |
| `sqlite3_vfs` (unix.c, win.c, etc.) | xtc_io | Per-platform VFS code (~3000 LOC each) |
| `sqlite3_threadsafe()` config | Always true via xtc | Config branches around thread-safety (~200 LOC) |
| `pthread_t` / `HANDLE` plumbing | xtc_proc | Threading code (~400 LOC) |
| Internal "shared cache" mode | xtc_lrlock + xtc_proc | The shared-cache code (~2000 LOC) |
| `sqlite3_progress_handler` | xtc_log + xtc_inject | The progress hook (~150 LOC) |
| Write-ahead log timer | xtc_timer | Timer plumbing (~100 LOC) |

Rough total: ~8000 LOC of stock SQLite that we can remove because xtc
supplies the equivalent more cleanly.  The fork still vendors
sqlite3.c for the layers we're not yet rewriting; over time the
amalgamation shrinks as we replace more.

## Phasing within the project

Stage 1 (mechanical layer split): can ship in one round.  Mechanical;
risk-low.

Stage 2 (subsystems as servers): three rounds, one per layer
(parser-proc, btree-proc, pager-proc).  Each adds latency but no
correctness change.  Risk-medium.

Stage 3 (xtc_lrlock buffer pool): one round.  This is where we'll
discover whether xtc_lrlock scales -- the property tests around it
will need to be much more comprehensive.  Risk-medium.

Stage 4 (fine-grained btree locking): two rounds.  Audit + retrofit.
Risk-high; touches the part of SQLite where bugs are subtle.  Plan to
spend half the budget on test infrastructure (a stress harness that
generates random concurrent workloads and checks consistency
invariants between transactions).

Stage 5 (async VFS): one round.  Mostly straightforward given xtc_io's
existing async-read pattern, but the interaction with SQLite's
expectation that reads are synchronous needs careful handling.

Total: roughly eight rounds.  Each stage delivers a working server;
the speedup arrives gradually.

## What we'll learn

The stages will surface gaps in xtc.  Specifically:

* Stage 2 will exercise `xtc_svr` performance under message-heavy
  workload.  If the gen_server pattern adds too much latency for
  fine-grained internal calls, we'll need a faster intra-proc
  request-reply that avoids the full mailbox round-trip.
* Stage 3 will stress-test xtc_lrlock with non-trivial data
  structures (4KB pages, frequent updates).  The wait-for-readers
  drain time will become a real number we can measure.
* Stage 4 will exercise xtc_lockmgr under a workload it was
  designed for (transactional locking with deadlock detection).
  Expect to discover at least one bug.
* Stage 5 will exercise xtc_io's async semantics through a
  real workload.  The io_uring backend should shine; we'll see.

Each gap surfaced becomes a tracked issue and either an enhancement
or a documentation note about when each primitive is appropriate.

## What this enables for postgres-on-xtc

PostgreSQL has a similar architecture: parser, planner, executor,
buffer manager, WAL.  The differences from SQLite:

* PG already runs multi-process with shared memory.  Threading is
  the next step; xtc is a candidate runtime.
* PG's lock manager is deadlock-detecting today; xtc_lockmgr is
  a port of essentially the same algorithm.
* PG's buffer manager is a hand-rolled shared-memory hash table
  with per-buffer LWLocks.  Conceptually the same as xtc_lwlock.

When sqlxtc Stage 4 lands and proves out fine-grained btree locking
under xtc_lockmgr, we'll have evidence for the PG port that the same
approach works on a larger codebase.  That's the throughline: sqlxtc
is a learning lab for the eventual postgres-on-xtc effort.

## Status

Stage 0 (current): vanilla SQLite under serialized mutex; xtc owns
the network frontend, the connection lifecycle, the resource budget,
and the SQLite mutex layer.  Working today; saturation bench shows
single-core ceiling.

Stages 1-5: planned per the above.  Each has its own follow-up doc
when work begins.
