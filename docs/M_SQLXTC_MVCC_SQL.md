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

Status: the storage swap is single-version today (`xstore` over the
B-tree).  Snapshot-isolation visibility and serializability layer on
this storage as described below; the engine already has the pieces
(`mvcc.c`: per-shard HLC, 2PC coordinator, version chains), and the
remaining work is to merge them with the B-tree-backed storage and put
the visibility check on the read path.

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
3. Merge `mvcc.c`'s 2PC coordinator + HLC so multi-row SQL transactions
   commit atomically at a single commit timestamp (today writes
   autocommit per statement at a fresh timestamp).
4. Add SSI (Cahill): rw-antidependency tracking + dangerous-structure
   abort for `SERIALIZABLE`; cite `predicate.c` lineage in the code.
5. The larger-than-RAM + (later) HammerDB/Quack benchmark comparison
   against SQLite under equal core/memory constraints.

The citations above are carried in the source where each piece lands,
so the provenance of the model is explicit in the code, not just here.
