---
title: "M_LIBXTC_PG_BOUNDARY: where libxtc ends and Postgres begins"
---

# M_LIBXTC_PG_BOUNDARY: where libxtc ends and Postgres begins

## Thesis

libxtc is the C runtime equivalent of what you'd get if Seastar, Tokio,
and BEAM had a baby and the baby grew up speaking POSIX.  It is
generic, language-runtime-shaped, and database-agnostic.  It is **not**
database-shaped.

A future threaded PostgreSQL built on libxtc draws a hard line: libxtc
provides the runtime; Postgres provides the database.  Anything
specific to "what a database does" -- catalogs, transactions, MVCC,
WAL, query planning, the wire protocol -- belongs to PG core.  Anything
generic to "running concurrent stuff in C with caps and back-pressure"
belongs in libxtc.

## The bright line

libxtc knows about: runtime, memory, sync, IO.

PG knows about: catalogs, transactions, MVCC, WAL, query planning,
the wire protocol, replication, role-based access, statistics policy.

Cross the line and bad things happen.  Let runtime concerns leak into
PG core and you can't share a primitive with sqlxtc, with a future
Redis-on-libxtc, or with a Kafka-on-libxtc.  Let database concerns
leak into libxtc and the runtime grows a 100k-line dependency on a
storage engine.

## Concrete dispositions

| Subsystem                        | libxtc | PG core | Notes                                                                                      |
|----------------------------------|:-----:|:-------:|--------------------------------------------------------------------------------------------|
| Async runtime / event loop       |   X   |         | xtc_loop, xtc_proc, xtc_app, xtc_orc supervisor.  PG's `latch` API maps onto xtc_recv.     |
| Memory contexts / allocator      |   X   |         | xtc_mctx + xtc_slab.  Postgres MemoryContext can be a thin shim on top.                    |
| Mutex / RWLock / LWLock          |   X   |         | xtc_mutex (pthread-shaped), xtc_lwlock (PG-shaped multi-reader), xtc_amutex (async).       |
| Lock manager (deadlock detector) |   X   |         | xtc_lockmgr ports PG's lock.c verbatim with a deadlock-graph walker.  Already there.       |
| Buffer manager                   |       |    X    | Knows about pages, relations, indexes, WAL, checkpoints.  PG-specific.                     |
| Background workers               |   X   |    X    | libxtc gives the substrate (xtc_proc, xtc_orc).  PG gives the policy (autovacuum, walwriter). |
| Snapshot manager                 |       |    X    | Mvcc-specific.  Lives in PG core.                                                          |
| Catalog cache                    |       |    X    | PG-specific data, but the lock primitive is libxtc's xtc_lrlock.                           |
| WAL                              |       |    X    | Format, replay, replication is PG.  The async write path can ride xtc_io's submit/complete. |
| Wire protocol                    |       |    X    | PG v3 is PG-specific.  Parser, error code mapping, COPY framing, replication slots: PG.    |
| Authentication                   |       |    X    | pg_hba, SCRAM, GSS, etc. is PG.  TLS termination is libxtc-adjacent (we don't ship it yet). |
| Statistics collection            |   X   |    X    | xtc_log emits structured events; pg_stat_* organises them.  Counter primitives in libxtc.  |
| Replication                      |       |    X    | Logical + physical, slots, conflict resolution: all PG.  Network plumbing: xtc_net.        |
| Resource governance              |   X   |         | xtc_res caps memory, fds, channels, in-flight messages.  Per-tenant scopes are PG.         |
| Time / clocks                    |   X   |         | __os_clock_mono, __os_clock_real.  PG's `GetCurrentTimestamp` becomes a thin shim.         |
| Crypto / hashing                 |       |    X    | PG's pgcrypto stays in PG.                                                                 |
| File I/O (low-level)             |   X   |         | xtc_io with epoll/kqueue/io_uring.  PG `mdread`/`mdwrite` rides on top.                    |

## Rule of thumb

If a primitive would be useful for, say, a Redis or a Kafka or a
SQLite-server (this very example), it belongs in libxtc.  If it's
only useful inside PG, it stays in PG.

Apply the rule to a borderline case:

* A **buffer cache** with replacement policy, dirty tracking, and
  WAL hooks -- Postgres-only.  PG core.
* A **bounded LRU cache primitive** with concurrent get/put and a
  pluggable eviction policy -- Redis would use it for keyspace
  metadata, sqlxtc for prepared-statement caches, PG for the catalog
  cache.  libxtc.

The bounded LRU is the kind of primitive libxtc can absorb without
crossing the line.  The buffer cache is not.

## What sqlxtc demonstrates about the boundary

`examples/06_sqlxtc` is a working stress test of the boundary:

* Every xtc primitive sqlxtc uses (xtc_proc, xtc_lwlock, xtc_lrlock,
  xtc_res, xtc_app, xtc_log, xtc_net, xtc_slab) is generic.  None of
  them know what SQLite is or what SQL is.
* sqlxtc's database-specific code (the SQLite amalgamation, the Lime
  SQL grammar, the Quack wire protocol) lives **entirely** in
  `examples/06_sqlxtc/`.  The libxtc tree is unchanged.
* The mutex implementation in `examples/06_sqlxtc/xtc_mutex.c` is the
  inverse of what a future PG adapter would do: SQLite's mutex
  interface gets backed by xtc_lwlock.  In a PG adapter, PG's
  `LWLock` API gets backed by `xtc_lwlock`.  Same primitive, two
  consumers.
* The 200-client / 200,000-query saturation run completes with zero
  errors and the configured RSS cap held -- that's xtc_res, xtc_proc,
  and xtc_lwlock all working together with a database that is
  emphatically not Postgres.

The fact that we shipped sqlxtc *without* touching libxtc is the
boundary check.  If we had to add a `xtc_sqlite_helper()` call to
get this working, the line would be in the wrong place.

## Future-of-PG implications

A future PG-on-libxtc adapter (call it M16) would be a thin layer:

* PG's `MemoryContext` -> wrap `xtc_mctx`.  ~500 LOC.
* PG's `latch` -> `xtc_recv` + a bit of bookkeeping.  ~300 LOC.
* PG's `LWLock` -> `xtc_lwlock` directly.  Already API-compatible.
  ~200 LOC.
* PG's `LockMgr` -> `xtc_lockmgr` (port lives in libxtc already).
* PG's `aio` -> `xtc_io`.  Mostly a name change.  ~600 LOC.
* PG's process model (`fork`-based postmaster) -> `xtc_proc` +
  `xtc_orc` supervisor tree.  This is the structural change; rest
  is shim layers.  ~3000 LOC.

Five thousand lines, give or take, to put PG on a threaded runtime
without rewriting query planning, WAL, or MVCC.  That's the whole
bet: keep the database in PG, keep the runtime in libxtc, and meet
in the middle through a thin adapter.

## Anti-patterns (things we will NOT do)

* **Don't** put SQLite-specific code anywhere outside
  `examples/06_sqlxtc/`.
* **Don't** put PG-specific code anywhere outside the (future)
  `pg/` tree of the adapter.
* **Don't** add a `xtc_query()` primitive -- that's a database
  responsibility.
* **Don't** add a `xtc_wal_*()` primitive -- same.
* **Don't** add a wire-protocol abstraction in libxtc.  Each
  consumer (Quack, RESP, PG v3, Kafka) does its own framing on
  top of xtc_net.

## Closing observation

The single best signal that the boundary is in the right place is
that sqlxtc, the redis example, and a hypothetical Kafka example
share their entire runtime layer and have zero shared application
code.  We have two of those three working today; the test for the
third would be: can a Kafka-on-libxtc be built without changing
libxtc?  If yes, the boundary is right.  If no, libxtc has accreted
something domain-specific and we should rip it back out.
