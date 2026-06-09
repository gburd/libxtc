# SQL on the libxtc-native engine, and the MVCC model it will carry

This document records how sqlxtc runs SQL on the libxtc-native storage
engine instead of SQLite's built-in B-tree, and the snapshot-isolation
MVCC model that layers on that storage -- with citations, because the
model is the one most production databases (PostgreSQL chief among
them) actually use, not merely a pair of papers.

## How SQL reaches our storage: the virtual-table seam

SQLite's storage cannot be swapped by "reimplementing btree.h": in the
single-file amalgamation `btree.c` is fused into `sqlite3.c`, there is
no `SQLITE_OMIT_BTREE`, and the VDBE is intimately coupled to the
record/rowid/overflow/schema semantics of that B-tree.  Reimplementing
it is a multi-year fork.

The supported, real mechanism -- and the one sqlxtc uses (`xstore.c`)
-- is a **SQLite virtual-table module** (`sqlite3_module`).  SQLite
still tokenizes, parses, plans, and runs the VDBE; only table I/O is
redirected, through the module's `xFilter`/`xNext`/`xColumn`/`xUpdate`
methods, into our engine: `btree.c` over the cooling buffer pool
(`bufmgr.c`).  `CREATE VIRTUAL TABLE t USING xstore` makes `t`'s rows
live in our on-disk, larger-than-RAM-capable B-tree.

This is also the hardest libxtc path, which is the point of building
it: when SQL runs on a connection proc, a cursor scan or an insert can
park the fiber on offloaded page I/O *in the middle of the VDBE*.
`test_xstore` runs a 4000-row, 800 KB working set through a 64 KB pool
both off a loop and ON a loop with the page provider live -- the VDBE
parks mid-statement and the coroutine stack save/restore carries the
deep SQLite-plus-B-tree C call chain correctly.  That validation (no
fiber-stack overflow, correct results under heavy eviction) is the
hardening this exercise exists to produce.

Status: implemented.  `xstore` over the B-tree is a multi-version
store: each row version is keyed (tableid, rowid, inverted commit_ts),
so a snapshot read returns the newest version at or before its
timestamp.  Snapshot-isolation visibility and Cahill SSI
serializability run on the read and commit paths (see xstore.c and the
SSI / write-skew cases in test_xstore.c).  The full isolation-level
set -- READ UNCOMMITTED, READ COMMITTED, REPEATABLE READ, SNAPSHOT,
SERIALIZABLE -- is selectable via xstore_isolation() (test_isolation.c),
and savepoints / nested transactions are implemented via the vtab's
xSavepoint/xRelease/xRollbackTo hooks (test_savepoint.c).  Commit
timestamps come from a Hybrid Logical Clock.  The separate mvcc.c
demonstrator (per-shard HLC, deferred-reply 2PC coordinator) remains a
KV-level scale-out study; it is NOT the path under the SQL engine,
which uses the single-process commit clock and SSI described here.

## The MVCC model: PostgreSQL heap visibility + Cahill SSI

The target is not a toy.  It is the model PostgreSQL ships, refined by
two decades of practice and the literature.

### Snapshot isolation: per-tuple xmin/xmax visibility (PostgreSQL)

Each row version (tuple) is tagged with `xmin` (the transaction that
created it) and `xmax` (the transaction that deleted/superseded it).
A version is visible to a snapshot S iff `xmin` committed before S and
`xmax` did not commit before S.  Readers never block writers and
writers never block readers; a read walks the version chain and takes
the version visible at its snapshot.  This is PostgreSQL's
`HeapTupleSatisfiesMVCC` over the heap, with indexes pointing at heap
tuples and `VACUUM` reclaiming versions no live snapshot can see.

Our engine maps onto this directly: a hybrid logical clock supplies
the commit timestamps that order `xmin`/`xmax` against a snapshot (the
per-shard HLC, `M_CAUSALITY.md`), the B-tree keyed by
`(user_key, commit_ts)` stores the versions, and the GC horizon (the
oldest live snapshot, already in `mvcc.c`) is `VACUUM`'s job.  Sources
to read: PostgreSQL `src/backend/access/heap/heapam_visibility.c`
(`HeapTupleSatisfiesMVCC`), `src/backend/utils/time/snapmgr.c`, and
the tuple header in `src/include/access/htup_details.h`.

### Serializability: Cahill's Serializable Snapshot Isolation

Snapshot isolation permits write-skew.  The fix sqlxtc will adopt is
**Serializable Snapshot Isolation (SSI)**, Cahill, Rohm and Fekete,
"Serializable Isolation for Snapshot Databases" (SIGMOD 2008; ACM TODS
2009) -- the same algorithm Dan Ports and Kevin Grittner brought into
**PostgreSQL 9.1** (2011) as its `SERIALIZABLE` level.  SSI keeps SI's
non-blocking reads and additionally tracks read-write
*anti-dependencies* (an `rw` edge from a transaction whose read is
later overwritten).  When a transaction sits at the apex of two
incoming/outgoing `rw` edges -- a "dangerous structure," the pivot of
a potential cycle -- one transaction is aborted.  This gives true
serializability at close to SI cost.  Sources to read: PostgreSQL
`src/backend/storage/lmgr/predicate.c` (the SSI implementation; its
header comment is an excellent precis) and the SIREAD-lock /
conflict-flag bookkeeping therein.

### The in-memory lineage: Neumann/Muehlbauer/Kemper

For the in-memory regime, Neumann, Muehlbauer and Kemper, "Fast
Serializable Multi-Version Concurrency Control for Main-Memory
Database Systems" (SIGMOD 2015, HyPer) refines validation: precise,
cheap serializability validation against the read predicate/set of
concurrently committed transactions, with a version organization
tuned for main memory.  It informs the validation-on-commit path when
the working set is resident; SSI informs the disk-resident, predicate-
locked path.  Both reduce to: order by a logical clock, detect the
dangerous read-write structure, abort the pivot.

## Plan (each step tested, ASan/UBSan, CI-gated)

1. **(done)** SQL on the B-tree storage via `xstore` -- the storage
   swap, larger-than-RAM, on-loop validated.
2. **(done)** MVCC snapshot reads on the `xstore` path: versions keyed
   by `(rowid, commit_ts)`, a read returns the newest non-tombstone
   version with `commit_ts <= snapshot` (PostgreSQL
   HeapTupleSatisfiesMVCC; newer versions stand in for `xmax`).  A
   global logical commit clock supplies timestamps; `xstore_now()` and
   `xstore_as_of(ts)` expose snapshot/AS-OF reads from SQL.
   `test_xstore` proves a read at an old snapshot sees the pre-update
   value while latest sees the new one, and a delete is invisible to
   the old snapshot.  Surfaced and fixed the vtable-boundary latch
   self-deadlock (see `M_SQLXTC_XTC_GAPS.md`).
3. **(done)** Atomic multi-row SQL transactions: the `xstore` vtable
   buffers a transaction's writes (via SQLite's xBegin/xUpdate/xCommit
   hooks) and flushes them all at ONE commit timestamp, so the rows
   share an xmin and a partial commit is never visible; xRollback
   discards the buffer; reads inside the txn see their own buffered
   writes (read-your-writes) and use the snapshot captured at xBegin
   (repeatable read).  `test_xstore` proves a two-row txn commits at a
   single timestamp, no snapshot sees a partial commit, and rollback
   discards.  (This is mvcc.c's stage-then-commit applied to the
   B-tree store; merging the cross-shard 2PC coordinator for
   distributed transactions remains where multiple shards are
   involved.)
4. **(done)** Serializable isolation by Cahill SSI pivot detection.
   A serializable `xstore` transaction validates at xSync (2PC phase
   1, so a failure can still roll back) and aborts only if it is a
   PIVOT -- a transaction with BOTH an outgoing and an incoming
   rw-antidependency.  Fekete et al. (2005) proved every
   serialization-graph cycle under snapshot isolation contains such a
   pivot, so aborting all pivots breaks all cycles while letting
   read-mostly transactions (outgoing edge only) commit -- strictly
   more concurrency than the precision validation it replaces.  The
   outgoing edge (a key we read was overwritten by a transaction that
   committed after our snapshot) is read from the shared B-tree's
   version timestamps, so it catches every committer; the incoming
   edge (a concurrent transaction read a rowid we are about to write)
   cannot be -- reads leave no version -- so serializable transactions
   publish their read sets to a small in-memory SSI registry that a
   committing writer scans against its write set.  Following PostgreSQL
   `predicate.c`, the outgoing edge counts toward an abort only when
   its target has already committed (the pivot's out-neighbor commits
   first), so in a write-skew the first committer commits and the
   second aborts.  `xstore_serializable(on)` selects the level per
   connection.  `test_xstore` shows (a) write-skew committing under SI
   (the anomaly) and one of two transactions aborting under
   serializable, and (b) a read-mostly transaction with only an
   outgoing edge committing under SSI where precision validation would
   have aborted it -- with the result still serializable.  The
   explicit transaction is detected via `sqlite3_get_autocommit` so
   reads before the first write are captured (SQLite fires xBegin only
   at the first write).  This is the model PostgreSQL 9.1+ uses
   (Cahill, Rohm, Fekete, SIGMOD 2008); the remaining refinement is
   finer-grained predicate locking (a full scan currently reads as
   "the whole table" rather than a key range).
5. **(done)** The larger-than-RAM benchmark against SQLite under an
   equal memory budget: `bench/sqlxtc/engine_ab.c` runs the same SQL
   through the same VDBE, differing only in the storage engine
   (`xstore` over our B-tree + cooling buffer pool, vs SQLite's own
   B-tree + pager), with equal cache budgets and a working set 24x
   larger than the cache so both page to disk.  Result (floki, median
   of 3, see `bench/sqlxtc/ENGINE_AB.md`): xstore reads are competitive
   -- within ~6% of SQLite read-only (608 vs 645 kops/s) and ~14% on
   95/5 -- while the 50/50 mix is ~1.9x slower, the measured cost of
   multi-version storage without GC on the SQL path.  The three write-
   path items this quantifies (version GC under xstore, O(1) version
   insert, asynchronous cooling-pool writeback to cut the eviction
   tail) are the prioritized next work; the async-writeback item is
   where xtc's cooperative I/O is positioned to beat a synchronous
   pager on the tail.  (A full TPC-C profile and the HammerDB/Quack
   networked comparison remain later work.)

The citations above are carried in the source where each piece lands,
so the provenance of the model is explicit in the code, not just here.
