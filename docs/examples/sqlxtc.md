---
title: sqlxtc (SQLite)
parent: Examples
nav_order: 2
permalink: /examples/sqlxtc/
lede: >-
  A SQL engine built from scratch on libxtc -- parser, vectorized executor, and B-link/buffer-pool/WAL storage -- rather than embedding SQLite.
---

1. TOC
{:toc}

---

Source:
[`examples/06_sqlxtc/`](https://codeberg.org/gregburd/libxtc/src/branch/main/examples/06_sqlxtc)
(~45,000 lines of C, including a large test suite). This is the most
demanding use of the library and the one that drove much of its
hardening.

## The software that inspired it

[SQLite](https://www.sqlite.org/) is the most-deployed database engine
in the world: a compact, embeddable, single-file SQL engine. Its
concurrency model is deliberately conservative -- by default a database
connection serializes through a single big mutex
(`SQLITE_CONFIG_SERIALIZED`), and even in WAL mode writers are
serialized. SQLite's own I/O is synchronous through a VFS shim. This is
exactly right for an embedded engine on one device; it is a poor fit for
a server that wants to use many cores and keep latency flat under
concurrency.

`sqlxtc` asks: *if you started a server-class SQL engine today, with a
fiber runtime in hand, what would each layer look like?* Rather than
embed SQLite and fight its threading model, sqlxtc builds the whole
stack fresh so every layer is fiber-aware and deterministically
testable.

## Similar, and different

```mermaid
flowchart TD
    subgraph SQLite
        Q1["SQL text"] --> P1["lemon parser"]
        P1 --> V1["bytecode VDBE<br/>(row-at-a-time)"]
        V1 --> B1["B-tree + pager"]
        B1 --> VFS["synchronous VFS"]
        L1["big connection mutex"] -.->|serializes| V1
    end
    subgraph sqlxtc
        Q2["SQL text"] --> P2["Lime parser"]
        P2 --> AST["AST"]
        AST --> VX["vectorized executor<br/>(batch-at-a-time)"]
        VX --> BM["buffer pool + B-link tree"]
        BM --> WAL["WAL + double-write"]
        WAL --> AIO["xtc_aio<br/>(parks a fiber, not a thread)"]
        LT["page latches (xtc_arwlock)<br/>+ commit lock (xtc_amutex)"] -.->|concurrency| BM
    end
```

**Similar:** it is a SQL engine -- parse, plan, execute against a
transactional, crash-safe, page-based store with a write-ahead log. The
storage concepts (B-tree pages, a buffer pool, WAL, recovery) are the
textbook ones SQLite also uses.

**Different in three big ways:**

1. **Parser.** SQLite uses the lemon parser generator; sqlxtc uses
   [**Lime**](https://codeberg.org/gregburd/lime), a parser generator,
   to produce its grammar-driven parser (`sql_parse.c` from
   `sql_grammar` via Lime).
2. **Executor.** SQLite's VDBE interprets bytecode one row at a time;
   sqlxtc's `vexec.c` is **vectorized** -- it processes batches of rows,
   which is the modern analytic-engine shape and far friendlier to the
   CPU.
3. **Concurrency and I/O.** SQLite serializes on a mutex and does
   synchronous I/O; sqlxtc uses libxtc's parking latches
   (`xtc_arwlock` per buffer-pool frame, `xtc_amutex` for the catalog
   and the commit critical section) and MVCC versions, so readers never
   block writers, and routes every disk operation through
   [`xtc_aio(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_aio.3)
   so a slow read **parks a fiber, not a thread**.

## How it works

- **`conn.c` / `main.c`** -- a connection process per client, speaking a
  small JSON protocol ("Quack") over TCP; many concurrent clients.
- **`sql_parse.c`** -- the
  [Lime](https://codeberg.org/gregburd/lime)-generated parser, driven by
  `sql_parse_drv.c`, producing an AST (`sql_ast.c`).
- **`vexec.c`** -- the vectorized executor and its planner: one
  access-path chooser (rowid range, longest-equality-prefix index seek,
  index order instead of a sort, MIN/MAX from the first or last key,
  index nested-loop joins) shared by SELECT, aggregates, ORDER BY,
  UPDATE and DELETE, and a per-statement plan cache. `vxfunc.c` holds
  the date/time, printf and math functions.
- **`bufmgr.c`** -- the buffer pool: pinning, eviction (CLOCK with a
  double-write buffer), and the pin-accounting that DST and ASan hardened
  (see [Known issues]({{ '/reference/known-issues/' | relative_url }}) for
  the pin-race history).
- **`btree.c` / `btnode.c` / `btovf.h`** -- a B-link tree with
  latch-free optimistic reads, leaf-only write latching, page-bounded
  entries and out-of-line large values (see below).
- **`wal.c` / `xlog.c`** -- write-ahead logging, group commit, and
  crash recovery (redo/undo), validated under the deterministic
  simulator and by kill -9 torture of the server.
- **`xstore.c`** -- the catalog and MVCC record store: per-connection
  transactions, snapshot reads, row write intents with deadlock
  detection, composite primary keys and UNIQUE constraints as
  order-preserving tuple indexes, and SSI for SERIALIZABLE.

## The B-tree, and what it borrows from RethinkDB

`btree.c` is a B-link tree (Lehman-Yao): every node carries fence keys
and a right-sibling link, so a descent that loses a race with a split
walks right instead of restarting. Readers take no latch at all on the
common path -- an optimistic descent validated by each frame's version
seqlock (OLC) -- and inserts latch only the leaf; splits and merges
serialize on a per-tree structure-modification lock and post separators
bottom-up. Nodes are slotted pages with prefix compression derived from
the fences and a 4-byte cached key head per slot.

1.57 compared this design against RethinkDB's B-tree
(`src/btree/`, `src/buffer_cache/`) and borrowed six things.

**Every entry is bounded by the page.** RethinkDB caps a leaf entry at
1/8 of a page (`leaf_epsilon`), and every split and merge guarantee
follows from that bound. sqlxtc had no bound: a 2 KB row could make a
4 KiB leaf unsplittable, and an indexed TEXT value over 1 KB overflowed
the split's separator buffer. Now `bt_max_key()` and `bt_max_inline()`
derive the limits from the page size (4 KiB pages: 498-byte keys, 506
bytes of key plus value inline), every call checks them before touching
anything, and leaves split **by bytes**, choosing the cut nearest half
that leaves room for the row being inserted (`btnode_split_ex`).

**Large values live out of line.** RethinkDB's blob stores a small
value inline and a large one in dedicated blocks referenced from the
leaf. sqlxtc does the same in one level: a value above the inline bound
is written to data pages first, and the leaf keeps a bounded reference
(flagged by the high bit of the slot's length field, so inline pages are
unchanged). Lookups and cursors reassemble it, checksum-verified; a
replace or delete frees the old pages only after the reference is gone,
while splits and merges only move the reference. B-tree values go up to
65535 bytes (the API's 16-bit length); the SQL engine above it stores at
most 4096 bytes per row and refuses a longer one with "row too large"
(see KNOWN_ISSUES).

**Writers do not hold the root.** RethinkDB's write descent keeps the
root write-locked across a child read, which serializes every later
operation (it compensates with page snapshots and eight shards per
table). sqlxtc's delete did the same; it now descends with shared
latches and takes only the leaf exclusive, which multiplied concurrent
delete throughput across four loops by eight.

**Questions about the whole tree are answered from the tree.**
RethinkDB keeps a population block and samples separator keys for
split points. sqlxtc's INSERT used to scan the whole table for the
largest rowid; `bt_seek_lt` answers it in one descent, so a single
INSERT takes the same 0.2 ms at 100k rows as at 1k (it was 9.6 ms).

**Split points come from the tree's own separators.** RethinkDB's
`get_distribution` samples the separator keys of the top levels to
estimate a key distribution in a handful of page reads. sqlxtc's
`bt_sample_keys` does the same, and the morsel-parallel scan cuts its
rowid range at those points instead of uniformly, so a table with
skewed rowids still splits into even morsels (1M rows, 90% of them in
the top 1% of the rowid space: the largest morsel went from 14.5x the
mean to 1.09x, and the 8-worker scan from 197 ms to 59 ms).

**Every node is checked after every change, in test builds.** RethinkDB
validates nodes after each mutation in debug builds. `btnode_check`
verifies a node's layout, space accounting, key order, fence
containment and out-of-line references; `-DBT_CHECK`, which is on
automatically under the deterministic simulator, runs it at every
mutation point, so a structural bug is reported at the operation that
caused it and pinned by a replayable seed.

What was deliberately *not* borrowed: per-entry replication timestamps
and in-page tombstones (backfill for replicas; the WAL is sqlxtc's
change stream), top-down proactive split/merge with root write locks
(strictly less concurrent than B-link), page-level copy-on-write
snapshots (sqlxtc has row-level MVCC), the log-structured serializer
(sqlxtc has a WAL with page LSNs), and the 250-byte key cap (sqlxtc's
16-bit keys with prefix compression are better; the page-derived bound
replaces it).

## Transactions and isolation

Every client connection has its own transaction (whatever `--threads`
is): its uncommitted writes are buffered privately and are invisible to
other connections until COMMIT, and ROLLBACK discards only its own
writes.

| Level | Reads | Write-write conflicts | Selected by |
| --- | --- | --- | --- |
| SNAPSHOT (default), REPEATABLE READ | one snapshot per transaction | the second writer of a row waits for the first; if the first commits, the second's statement fails | default / `BEGIN ISOLATION LEVEL REPEATABLE READ` |
| READ COMMITTED (READ UNCOMMITTED is the same) | a fresh snapshot per statement | the second writer waits, then re-runs its statement on a fresh snapshot (up to 8 times) | `SET TRANSACTION ISOLATION LEVEL READ COMMITTED` |
| SERIALIZABLE | one snapshot per transaction | as SNAPSHOT, plus SSI: a dangerous read-write pivot (write skew) fails the COMMIT | `BEGIN ISOLATION LEVEL SERIALIZABLE` |

The statements accepted over the wire are
`BEGIN [TRANSACTION] ISOLATION LEVEL <level>`,
`SET TRANSACTION ISOLATION LEVEL <level>` (inside a transaction, before
its first query) and
`SET SESSION CHARACTERISTICS AS TRANSACTION ISOLATION LEVEL <level>`
(the connection default).

Since 1.58 every write takes a *row write intent* before it touches the
row, much as PostgreSQL takes a tuple lock. A second transaction that
writes the same row parks its fiber until the holder commits or rolls
back. Then it either proceeds (the row did not change, or READ COMMITTED
re-runs the statement), or fails the statement with `could not serialize
access due to concurrent update`. A wait-for cycle (A holds r1 and wants
r2, B the reverse) fails exactly one of them, the youngest, within
microseconds. After such a failure the transaction is aborted, as in
PostgreSQL: later statements fail with `current transaction is aborted`
until ROLLBACK, and a COMMIT reports the failure rather than committing
the rest. The commit-time first-committer-wins check stays as a backstop
and is the only conflict check SSI adds to. Before 1.58 conflicts were
found only at COMMIT, so a TPROC-C client lost its whole transaction
after doing all of its work; finding them at the statement made 8 and
32 connections faster than 1, not slower.

The intent waits are fiber naps counted in nap time, with no clock read
and no random backoff, so they replay deterministically under the
simulator.

Snapshots are taken from a *read horizon* that trails commits still
writing their log record or applying their versions, so a snapshot
never sees part of a transaction, and a row never appears inside a
running snapshot. COMMIT returns once the commit is durable and visible.

## Durability and I/O errors

A commit is acknowledged only after its log record is written and
`fdatasync`'d (group commit amortizes the fsync across concurrent
committers). If a log write or fsync fails -- ENOSPC, EIO, a file-size
limit -- every commit in that batch gets an `I/O error` reply and the
log **fail-stops**: the server refuses every later write with the same
error (reads still work) until it is restarted. After a failed fsync the
kernel may have dropped the dirty pages it could not write, so retrying
cannot make the earlier writes durable; restarting replays the log,
which holds exactly the acknowledged commits. A double-write slot that
cannot be made durable blocks the page's home write, and a clean
shutdown whose flush fails does not mark the store clean, so the next
start replays the log.

The log is compacted while the server runs: a background checkpoint
replaces it with a checkpoint record plus the live rows whenever it
passes `max(--checkpoint-mb, 2 x its size after the last checkpoint)`
(default 64 MiB). Commits pause for the duration of the dump; reads do
not. Crash recovery replays the compacted log onto a fresh page file,
so restart time is proportional to the live data plus the writes since
the last checkpoint. (The in-place recovery path that would trust the
page file and its double-write buffer exists and is tested, but the
server does not use it yet.)

## Buffer replacement: HOT/COOL and CLOCK

The default replacement policy combines a CLOCK hand, HOT/COOL retention
states, and a separate one-bit reference flag. These are complementary:
CLOCK determines which frame to inspect; HOT/COOL expresses retention
preference; the reference flag grants another chance to a recently used
page. With scan resistance enabled, a demand-loaded page enters COOL and
a second resident access promotes it to HOT. A scan need not promote
every page it reads into the hot working set.

`bm_opts_t.claim_threshold` controls an optional protection-work governor.
After that many HOT passes/demotions or reference-bit second chances in
one eviction call, the sweep stops honoring those preferences for its
next eligible victim. Pins, in-flight I/O, and reservation checks still
apply; dirty candidates require writeback. Zero preserves legacy
selection. The default is zero; 128 is a measured opt-in value, not a
universal recommendation. This option applies to CLOCK, not the optional
sampled-eviction implementation.

The governor limits time spent protecting candidates, **not total
allocation latency**. Pinned pages, writeback, competing allocators, and
retries can exceed the nominal budget. The cost is evicting useful pages
sooner. A near-full sequential hot-set fixture demonstrates sustained
churn with a small budget; a shorter individual sweep does not imply
less total work.

### Measuring the trade-off

The standalone `bm_sweep_bench` Makefile target compares thresholds in
one binary. Its positional arguments are threshold, duration in seconds,
disposable store path, frame count, page count, loop count, fiber count,
Zipf exponent multiplied by 100, direct-I/O flag, and seed. It validates
read-back page ids, requires sustained eviction during warm-up, and
reports one CSV row. It does not start the provider or trickler.

A FreeBSD 15.1 run on a 32-vCPU host on local NVMe used 4 KiB pages, 16 loops,
128 fibers, Zipf exponent 1.10, and a dataset 1.125 times the pool.
Thresholds 0 and 128 alternated over three repetitions each; measured
windows lasted 20 seconds after eviction warm-up. These measurements
precede the subsequent writeback-pinning and stale-load publication fixes;
they motivate the governor but are not qualification numbers for the
final release tree. Medians:

| Pool frames | Ops/s, 0 | Ops/s, 128 | Hit %, 0 | Hit %, 128 | p99.9 ms, 0 | p99.9 ms, 128 |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 16,384 | 2,954,644 | 3,869,069 | 99.0534 | 98.9779 | 5.888 | 4.864 |
| 65,536 | 2,268,189 | 5,103,385 | 99.2586 | 99.1795 | 8.704 | 4.096 |
| 262,144 | 1,669,002 | 6,330,307 | 99.4441 | 99.3374 | 11.776 | 3.584 |

At 16K frames, p99 **worsened from 2 us to 608 us** as misses crossed
one percent, despite higher throughput and lower p99.9. These are
read-only buffer-manager measurements, not SQL transaction results.
They do not establish write-heavy performance or device-level read
amplification. Quantiles are histogram bucket lower bounds, sampled in
microseconds. Independent fixed-duration arms execute different request
counts, not a lockstep request trace.

The added `bm_stats_t` counters describe successful CLOCK reclamations:
`allocs` counts successful eviction calls, `advances` their frame visits,
`adv_max` the largest such visit count, and `forced_claims` reclaims after
the budget expired. Failed calls and allocation retries are excluded;
`advances / allocs` is scan work per successful reclaim, not total I/O
amplification. The benchmark's sweep maximum includes warm-up.

### Checkpoint LSN continuity

Compacting the WAL must not restart its log sequence numbers. Data-page
LSNs survive compaction, and in-place recovery compares each image's
record LSN against the page LSN before applying it. Restarting at one
can incorrectly suppress newer images. The compaction emitter continues
above the old log's highest issued LSN; rebinding resumes from the
compacted file's maximum. The LSN monotonicity regression and the fuzzy
checkpoint test exercise this rule, including forced eviction.

## Concurrent page publication and writeback

A miss loads a page into a private frame before publishing it in the
page table. Checking only whether that pid is resident at publication
is insufficient: another loader can publish it, a writer can change it,
and eviction can flush and remove it before the first read completes.
Publishing that delayed read would restore an obsolete image, including
keys that a successful B-tree delete removed.

The buffer manager validates page-table generation across the read. A
mapping change invalidates a private image when no resident winner is
available; the reader releases its frame and retries. No page-table
mutex spans the I/O. A regression pauses the original loader while a
second loader completes a delete, checkpoint, and eviction, then verifies
that resuming the delayed read cannot resurrect the key.

Eviction also rechecks dirty and I/O state after reserving an unpinned
frame: a writer can dirty and unpin between the initial checks and the
reservation. Writeback itself holds a pin through completion so that
snapshotting and disk I/O cannot race frame recycling. Releasing that
internal pin does not mark the page recently accessed. Page-ID quarantine
also waits for old frame owners, including writeback: a live old snapshot
must not overwrite a newly allocated page using the same id. Draining
quarantine makes an id reusable, not readable: retired-id membership
continues to reject disk loads until allocation publishes an initialized
replacement. Otherwise a delayed descent can delete from an obsolete
pre-merge image while the live key remains in the merged sibling.
Publication establishes the loader's pin before exposing the frame and never resets
the count after readers can increment it. Private page-in frames stay
unpinnable until their data is ready. These are storage correctness
requirements independent of the optional sweep governor.

The server sets the runtime fiber stack size to 512 KiB before creating
fibers. SQL parsing and execution, especially with sanitizer instrumentation,
need more headroom than the runtime's 64 KiB default. Budget this larger
per-fiber virtual reservation when choosing the connection limit; it is
not a claim that each connection immediately consumes 512 KiB of RSS.

## How libxtc concepts are applied

The SQLite-vs-sqlxtc diagram above is drawn as boxes, but every box is
really libxtc machinery. Here is the runtime shape:

```mermaid
flowchart TD
    APP(["xtc_app + root supervisor"]):::sup --> SVR["listener proc<br/>(supervised, xtc_net_accept)"]
    SVR -->|xtc_proc_spawn per client| C1["conn proc (fiber)"]
    SVR -->|xtc_proc_spawn per client| C2["conn proc (fiber)"]
    EX["xtc_exec: one loop per core,<br/>work-stealing"]:::run -.->|runs| C1 & C2
    C1 -->|parse + plan| VX["vectorized executor<br/>(runs on the conn fiber)"]
    VX -->|descend, fix pages| BT["B-link tree<br/>(latch-coupled on the fiber)"]
    BT --> BM["buffer pool"]
    BM -->|per-frame latches| LT["xtc_arwlock<br/>(parks the fiber)"]:::lock
    VX -->|COMMIT| CL["commit lock + in-flight set<br/>(xtc_amutex)"]:::lock
    BM -->|page I/O| AIO["xtc_aio: a miss parks<br/>the fiber, not the thread"]:::run
    C1 -->|memory budget| RES["xtc_res caps"]
    classDef sup fill:#e8f0ff,stroke:#36b;
    classDef run fill:#e6f6ec,stroke:#2e9e57;
    classDef lock fill:#fff3e0,stroke:#e08a00;
```

- **Supervision.** The server is an `xtc_app` with a root supervisor; the
  connection front door is a supervised listener proc (one_for_one,
  permanent, at most 10 restarts a minute) that accepts with
  `xtc_net_accept` (backing off on EMFILE). Connection procs and the
  storage procs are not supervised, and faults are not contained:
  sqlxtc installs no `xtc_fault_guard`, so a fault in any fiber ends the
  process. That is deliberate for a storage engine -- a fault taken while
  holding a page latch or the commit lock leaves shared state that cannot
  be unwound safely -- and recovery is a restart that replays the log.
  (Contrast [let it crash]({{ '/philosophy/let-it-crash/' | relative_url }}),
  which fits stateless connection handlers, not a shared page cache.) A
  connection proc releases its socket, buffers and engine handle only
  when it returns normally; one killed with `xtc_exit_pid` leaks them
  until the server restarts.
- **The executor and the B-tree run on fibers, not threads.** Each
  connection is a fiber (`xtc_proc_spawn`) on the multi-loop `xtc_exec`
  executor; the vectorized executor and the B-link-tree descent run
  *on that fiber*. A page fix that misses does not block a thread -- it
  parks the fiber via `xtc_aio` and the loop serves other connections
  until the read completes. This is the whole reason to build on libxtc:
  storage-engine code reads like straight-line synchronous C yet never
  stalls a core.
- **Data sharing.** The buffer pool and B-link tree are the *shared*
  structures (the deliberate
  [compromise]({{ '/philosophy/message-passing/' | relative_url }}) away
  from pure message passing, because copying pages through mailboxes
  would be absurd). Readers do not lock rows: MVCC gives each one a
  snapshot. Connection *state* -- the open transaction, its write buffer
  and snapshot -- is private to each connection, shared-nothing where it
  can be, shared-with-discipline where it must be. (The catalog uses an
  `lrlock` for its read-mostly table count; sqlxtc does not use RCU or
  the deadlock-detecting lock manager -- `04_lockmgr_demo` in
  [the examples index]({{ '/examples/' | relative_url }}) shows those.)
- **Locking.** Latch-coupling on the B-link tree uses short per-frame
  `xtc_arwlock` latches that park the fiber instead of blocking the
  thread, taken in a fixed top-down order so they cannot deadlock. One
  `xtc_amutex` commit lock makes the write-write check, SSI validation,
  timestamp assignment and in-flight registration a single atomic step;
  the log write and the apply run outside it, so group commit still
  batches concurrent committers.
- **Resource limits.** `xtc_res` caps bound memory and in-flight work so
  a query storm degrades instead of OOMing.

## Advantages of building it on libxtc

- **Every layer is fiber-aware.** A page miss parks the requesting fiber
  and the loop keeps serving others; there is no thread blocked on a
  read, and no callback soup.
- **Deterministic testing of the whole engine.** Because all I/O and
  scheduling flow through libxtc, the simulator can replay a full
  transaction workload -- with injected torn writes, crashes, and
  fsync-loss -- from a seed. Crash safety is tested at three levels: the
  simulator (seeded crashes, torn writes, lost fsyncs), the server under
  repeated kill -9 with every acknowledged commit checked after restart,
  and injected log-write failures (a file-size limit makes every write
  past it fail) where every acknowledged commit must survive and every
  failed one must have returned an error. See
  [Testing]({{ '/testing/' | relative_url }}).
- **Parking primitives.** `xtc_arwlock` / `xtc_amutex` let a latch holder
  park on I/O without wedging the loop, which is what lets the engine
  hold a page latch across a page read.

## Challenges (warts and all)

- **It is a lot of code.** Rebuilding a storage engine is ~45k lines;
  embedding SQLite would have been a fraction of that. The payoff is
  fiber-awareness and testability an off-the-shelf engine with its own
  threading cannot give -- but the cost is real and worth stating
  plainly.
- **Buffer-manager pin accounting is genuinely hard.** The concurrent
  demand-load / eviction path had real races (a stale swip clobbering a
  fresh pin, a load publishing a frame before pinning it) that only the
  deterministic simulator plus ASan pinned down. Those fixes -- and the
  one residual epoll lost-wakeup shape -- are documented honestly in
  [Known issues]({{ '/reference/known-issues/' | relative_url }}).
- **Cross-thread wakeups under a multi-loop executor** were where sqlxtc
  found real library bugs (a proc parking its wait fd on the wrong
  loop's ring). The engine's pressure is precisely what made those
  bugs reproducible -- an argument for building the hard example.

## Benchmark: TPROC-C against SQLite and Turso

Measured for 1.58 on a bare-metal host: two Xeon 8375C sockets (128
threads), 1 TiB, and eight local NVMe drives in mdadm RAID-0 under XFS
(`su=64k,sw=8`, `noatime,largeio`).  The OS was tuned for measurement:
the performance governor, C-states held shallow through
`/dev/cpu_dma_latency`, transparent hugepages and NUMA balancing off,
swap off, irqbalance and timers stopped.  On that array a 4 KiB write
plus `fdatasync` takes 95 us.

All three engines ran the same driver: Turso's own `perf/tpc-c` (a
tpcc-mysql derivative, 35 prepared statements, the standard 45/43/4/4/4
mix with no think time) through the sqlite3 C API.  For sqlxtc that API
is a small client shim speaking Quack over loopback TCP, so every
sqlxtc statement is a network round trip; SQLite and Turso run
in-process.  The setup was 10 warehouses (about 7 GB in SQLite), 30 s of
ramp, 120 s measured, 3 passes per cell.  The engine was pinned to NUMA
node 0 and, for sqlxtc, the client to node 1.  The driver was patched in
three places so that retries are real transactions: COMMIT runs inside
the retry loop, the transaction is re-opened before a retry, and
Payment no longer calls `exit()` on a conflict.

New-Order transactions per minute (tpmC), median of 3:

| Engine, mode | 1 conn | 8 conns | 32 conns | 64 conns |
| --- | ---: | ---: | ---: | ---: |
| SQLite 3.53.4, WAL, `synchronous=NORMAL` (no fsync per commit) | 95,400 | 99,400 | 95,300 | -- |
| SQLite 3.53.4, WAL, `synchronous=FULL` (fsync per commit) | 81,000 | 84,500 | 84,400 | -- |
| **sqlxtc 1.58** (fdatasync before every ack, 32 loops) | 13,900 | **54,600** | 47,200 | 39,700 |
| Turso 0.8.3, WAL, `synchronous=NORMAL` | 4,830 | 4,970 | -- | -- |
| Turso 0.8.3, MVCC, `BEGIN CONCURRENT` | 1,320 | 5,790 | 10,500 | 9,200 |

What the numbers say:

- **SQLite wins, by a wide margin, at every connection count.**  It does
  about 95k tpmC on one core: an in-process B-tree with no network hop,
  under one database-wide write lock.  Connections add nothing (CPU stays
  near 1% of the box) because every write serializes, but each one is
  very cheap.
- **sqlxtc scales where SQLite cannot, but from a much lower base.**  At
  one connection a New-Order costs about 2.9 ms.  That is one TCP round
  trip per statement (about 38 per committed transaction across the mix,
  from the server's counters) plus one `fdatasync`; the server spends a
  median 9.7 us per statement.  At 8 connections group commit and row-level concurrency
  give 3.9x.  Past that the hot rows of TPC-C bound it: Payment holds its
  warehouse row until COMMIT, and the run includes periodic log
  checkpoints that pause commits.  sqlxtc is durable per commit in every
  configuration; the comparable SQLite line is FULL.
- **Turso 0.8.3 is the slowest here.**  In WAL mode it is CPU-bound in
  B-tree seeks and plans some TPC-C lookups on a prefix or secondary index
  where SQLite uses the full primary key (`EXPLAIN QUERY PLAN` on the same
  file).  Its MVCC mode scales to 10.5k at 32 connections, but conflict
  retries dominate: about 13 million write-write conflicts in 120 s.

Tail latency, New-Order p99, per-10 s-interval median: SQLite 5.5-7.7 ms;
sqlxtc 3.8 ms at 1 connection and 15 ms at 8; Turso MVCC 120 ms at 32.
At 32 and 64 connections a sqlxtc run that catches a checkpoint shows a
1.4 s p99 for that interval (see [Known
issues]({{ '/reference/known-issues/' | relative_url }})).

Configuration mattered as much as code.  sqlxtc's buffer pool defaults
to 1 MiB, and these runs pass `--storage-frames=65536` (256 MiB).  The
1.58 server also bounds the CLOCK sweep by default (`--storage-claim=128`;
without it the 8-connection run spent 78% of the server in `evict_one`
and did 8.4k), and the page provider walks a bounded slice per tick
(the full-pool walk halved one-connection throughput at a 1 GiB pool).

### In-process, after the 1.59 fixes

The numbers above measure the network binding as much as the engine:
at one connection about 70% of a transaction was Quack round trips.
For 1.59 the same driver was linked against the engine directly (a
sqlite3-API binding over `sx_*`, the way SQLite and Turso run), and
five bottlenecks that the in-process profile exposed were fixed:

1. **The catalog was re-read on every statement.**  The in-process table
   cache was filled only by `CREATE` in the same process, so on any
   reopened store each statement scanned the catalog B-tree and re-parsed
   the column list: 19% of a connection's CPU.
2. **The periodic log checkpoint stopped commits for the whole dump**
   (1-3.5 s at 1 warehouse, about 16 s at 10).  It now runs online: commits
   wait only while it takes its start point and swaps the files.
3. **Off-loop commits did not group.**  A committer on a plain thread
   wrote and fsynced its record alone under a mutex.  It now joins a
   leader/follower group commit, the deferred-durable commit of
   PostgreSQL's async WAL series.
4. **Row intents did not wait on plain threads**, and a transaction's
   first UPDATE checked for a newer version before waiting for the row's
   holder, so a hot row (TPC-C's warehouse) failed after eight re-runs.
   At 32 connections that was 2.8 million Payment retries a minute.
5. **A recursive libxtc mutex won on its contended path by a plain
   thread could not be re-entered** (a core libxtc bug; the server never
   hit it because its callers are fibers).

Same host, same tuning, 10 warehouses, both engines in-process
through the same driver binary, both `fdatasync` before every commit
returns.  sqlxtc with a 4 GiB pool, SQLite with a 16 GB page cache;
60 s ramp, 120 s measured, 3 passes alternating the engines; tpmC,
median of 3; no commit failures in any run:

| Engine | 1 conn | 8 conns | 32 conns | 64 conns |
| --- | ---: | ---: | ---: | ---: |
| SQLite 3.53.4, WAL, `synchronous=FULL` | 84,500 | 85,400 | 86,400 | 84,600 |
| **sqlxtc 1.59, in-process** | 43,900 | **98,600** | 71,700 | 64,200 |
| sqlxtc 1.58, Quack over TCP (above) | 13,900 | 54,600 | 47,200 | 39,700 |

sqlxtc passes SQLite at 8 connections and
stays at 0.76-0.83x of it at 32 and 64, where SQLite's single writer
is flat.  At one connection it reaches half of SQLite.  Each commit
costs one WAL `fdatasync` (58 us on this array) in both engines, but
sqlxtc also writes evicted pages back through its double-write ring:
one ring `fdatasync` per page, about 1,000 a second, on the same disks.
SQLite's only per-commit sync is the WAL frame; its checkpoint writes
the database file in batches.  The double-write ring was then removed
(below): in this recovery mode it protected nothing.  The remaining
retries at 32 and 64 connections are New-Order and Payment updating
the district row after reading it under snapshot isolation, where
first-committer-wins rolls one of them back.  SQLite has no such
conflict because it serializes writers.

### The rest of the 1.59 work: the log, the B-tree, the version GC

A second round compared sqlxtc's write path with Berkeley DB, BDB JE,
WiredTiger, LMDB, LeanStore, InnoDB and PostgreSQL's async WAL series,
and measured it on in-process workloads of many shapes, against SQLite
on the same disk.

* **The log buffer is a ring with parallel insert.**  A committer
  reserves its LSN and a byte range under a small lock, copies and
  checksums its record outside it, and publishes completion in the LSN's
  slot.  The first committer to find no flush running leads: it writes
  and syncs the completed prefix and wakes exactly the waiters it
  covered, plus one as the next leader (Berkeley DB's commit queue,
  JE's copy outside the latch, InnoDB's completion links).  At 16
  threads the group factor went from 2.7 to 8.8 commits per fsync.
* **The log is preallocated.**  Each commit's `fdatasync` had also been
  making the file's new size durable.  On the test host's XFS NVMe that
  cost 104 us against 34 us for the same write into blocks already
  written, so the log now keeps zeros 4 MiB ahead of its end (WiredTiger
  does the same; InnoDB's redo files are fixed-size).  This one change
  doubled single-connection commit throughput.
* **The double-write ring is gone.**  The page file is trusted only
  after a clean close, and after a crash it is rebuilt from the log, so
  a torn page is never read.  The ring cost an `fdatasync` per page and
  protected nothing.
* **Pages fill under sequential inserts.**  An insert at the end of a
  leaf splits at the end (LMDB's `MDB_APPEND`, InnoDB's
  `PAGE_LAST_INSERT`), so ascending keys fill pages to 94% instead of
  47%.  Interior levels are read optimistically (version-checked, no
  latch), as in LeanStore.
* **Old row versions are pruned on write by default**, and every
  statement pins its snapshot so pruning can never take a version a
  reader is about to use.  A ledger store of 100k accounts after 2M
  transfers went from 1.17 GB to 685 MB.

Three bugs surfaced on the way, each now with a test that failed
before the fix: a transaction larger than 16 MiB, and every commit after
it, was lost in crash recovery (test_wal_bigtxn); a transaction's
spilled rows were never deleted (test_steal_reclaim); and a direct-I/O
store never reopened trusted (test_dio_restart).
[Known issues]({{ '/reference/known-issues/' | relative_url }}) has the details.

**A ledger benchmark.**  The driver ports the schema and workload shapes
of a PostgreSQL double-entry ledger benchmark (TigerBeetle-style
accounts and transfers) to the sqlite3 API and runs them in one
process: 100k accounts, 2M seeded transfers, ten shapes.  Each shape
runs for 10 s.  An audit after every run checks that posted debits
equal posted credits and the sum of posted transfers, and that pending
balances match the open pending transfers.  SQLite 3.53.4 runs with WAL, `synchronous=FULL` and
`BEGIN IMMEDIATE` above one connection; both engines `fdatasync` every
commit.  The table shows transactions per second, median of two
interleaved runs, on a 32-vCPU host with local NVMe; every audit passed.

| Shape | What a transaction does | 1 conn SQLite / sqlxtc | 8 conns | 32 conns |
| --- | --- | ---: | ---: | ---: |
| transfer | debit + credit 2 accounts, insert a transfer | 3,119 / **13,477** | 4,926 / **46,031** | 4,929 / **36,801** |
| lifecycle | a pending transfer, then post (80%) or void | 2,140 / **9,861** | 3,709 / **28,282** | 3,729 / **21,280** |
| batch | 10 transfers in one transaction | 490 / **2,709** | 790 / **8,055** | 764 / **6,406** |
| insert | append one event row, autocommit | 19,037 / **24,474** | 18,357 / **64,485** | 18,034 / **46,817** |
| mixed | 60% transfer, 20% lifecycle, 20% reads | 2,780 / **13,850** | 5,584 / **48,037** | 5,687 / **37,884** |
| blend | 40% transfer, 25% balance, the rest spread over five shapes | 2,982 / **6,959** | 6,753 / **39,098** | 7,101 / **47,339** |
| balance | read one account | **376,010** / 340,653 | 391,075 / **2,182,646** | 294,696 / **3,800,567** |
| history | an account's last 20 transfers (two index ranges) | **17,559** / 11,448 | **133,390** / 59,098 | 43,915 / **74,241** |
| statement | count and sum an account's debits | **35,682** / 26,451 | **248,187** / 152,574 | 82,574 / **174,880** |
| analytic | group the last 5,000 transfers | **1,160** / 603 | **9,259** / 3,336 | **17,986** / 5,392 |

Writes are where sqlxtc leads: 4-10x on every write shape, because
commits group across connections where SQLite has one writer.  Point
reads match SQLite at one connection and scale with connections.  Range
reads still lose at low concurrency.  `analytic` visits exactly the
5,001 rows it needs, but each row costs about twice SQLite's:
decoding 9-byte integers, advancing the cursor, and evaluating the
expression tree.  That per-row cost is the next target.

**TPROC-C in process, warm.**  The same harness on the same host, 10
warehouses, 120 s ramp so the 4 GiB pool holds the store, 60 s
measured, two passes alternating the engines:

| Engine | 1 conn | 8 conns |
| --- | ---: | ---: |
| SQLite 3.53.4, WAL, `synchronous=FULL` | **93,900** | 86,700 |
| sqlxtc 1.59 | 75,700 | **150,000** |

With a 30 s ramp the pool is still filling from the cold page file, and
sqlxtc reads 54,000 at one connection.  Count the warm-up when comparing
with the metal table above, which used a 60 s ramp.

### Feature by feature

The same data set (10 warehouses), the same sqlite3-API binary per
engine, one prepared statement per row with random keys, 10 s each.
Operations per second, 1 thread / 16 threads:

| Statement shape | SQLite (WAL, NORMAL) | sqlxtc | Turso WAL | Turso MVCC |
| --- | ---: | ---: | ---: | ---: |
| point read, integer PK | 439k / 1.53M | 34k / 507k | 524k / 1.47M | 181k / 451k |
| point read, 3-column PK | 255k / 1.56M | 27k / 421k | 573 / 8.1k | 110 / 974 |
| secondary index equality + ORDER BY | 236k / 1.58M | 26k / 338k | 168k / 1.44M | 50k / 365k |
| count(*) with an index predicate | 233k / 1.60M | 25k / 332k | 882 / 12k | 210 / 1.5k |
| MIN over an index prefix | 420k / 1.56M | 35k / 509k | 363k / 1.46M | 142k / 441k |
| `= (SELECT max(...))` subquery | 337k / 1.52M | 25k / 50k | 2.8k / 45k | 668 / 3.5k |
| SUM over a PK range | 184k / 1.59M | 33k / 503k | 114k / 1.39M | 35k / 234k |
| join on PKs (customer x warehouse) | 284k / 1.56M | 24k / 323k | 580 / 8.4k | 111 / 968 |
| DISTINCT over a range | 14k / 223k | 35k / 521k | 7.1k / 96k | 2.6k / 21k |
| ORDER BY ... DESC LIMIT 1 | 434k / 1.50M | 34k / 534k | 369k / 1.46M | 144k / 449k |
| GROUP BY over 10 rows | 199k / 1.54M | 18k / 137k | 122k / 1.47M | 42k / 259k |
| UPDATE by PK in a transaction | 91k / 61k | 3.8k / 21k | 22k / 12k | 2.1k / 45k |
| INSERT in a transaction | 113k / 108k | 4.6k / 38k | 69k / 61k | 8.6k / 59k |

At one thread, sqlxtc's floor is about 25-40 us per statement: one
loopback TCP round trip and JSON encoding, which the in-process engines
do not pay.  It is the only engine whose writes get faster with more
threads (5.6x for UPDATE and 8.4x for INSERT at 16), because SQLite
and Turso WAL take a database-wide write lock.  DISTINCT is faster than
SQLite at every thread count (a hash set against SQLite's temporary
B-tree).  Turso's very low rows come from its planner: for
`c_w_id = ? AND c_d_id = ? AND c_id = ?` it seeks only the two-column
prefix of an index, a 3000-row range, where SQLite seeks the full
primary key (`EXPLAIN QUERY PLAN` on the same file).

## Run it

```sh
cd examples/06_sqlxtc && make XTC_BUILD=../../build sqlxtc-server
./sqlxtc-server -d /var/tmp/demo.db &    # durable store /var/tmp/demo.db.xstore
# then talk the Quack JSON protocol, or run the in-process test suite:
make XTC_BUILD=../../build test-wal-fail test-ww-conflict test-wal-recover test-btree
```

---

&larr; [rexis (Redis)]({{ '/examples/rexis/' | relative_url }}) &middot;
Next: [kaka (Kafka)]({{ '/examples/kaka/' | relative_url }}) &rarr;
