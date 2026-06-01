# sqlxtc scale-out stage 4: MVCC + cross-shard transactions

Stages 1-3 of `M_SQLXTC_SCALEOUT.md` landed: a group-commit WAL writer,
a share-nothing sharded store with a control-plane proc, live process
introspection, and a causal tracer with a hybrid logical clock.  Stage
4 is the capstone of the data path: snapshot-isolation MVCC and atomic
transactions that span shards.  This document is the executable plan.

It is deliberately scoped to the **sharded KV** (`test_shard.c`-shaped)
demonstrator, not the SQLite-backed engine: the goal is to exercise the
hardest coordination on libxtc and prove the primitives, exactly as the
earlier stages did.

## Status: slices 1-5 SHIPPED (mvcc.c, test_mvcc.c)

The versioned shards, the per-shard HLC, the deferred-reply 2PC
coordinator, write-write conflict detection, and snapshot-aware version
GC are implemented and tested.  `test_mvcc` proves: (1) snapshot
isolation -- a read at an old snapshot does not see a write committed
after it; (2) cross-shard atomicity -- a two-key transaction spanning
shards commits all-or-nothing through the coordinator; (3) concurrent
conflict -- N clients committing one key at one snapshot resolve to
exactly one commit, the rest aborting, on a single loop and on a
4-loop executor; (4) version GC -- a hot key's chain stays short as
the low-water mark advances, while a LIVE old snapshot pins the
version it can see (retained across a dozen newer commits, then
reclaimable once released).  All deterministic, ASan + UBSan clean,
no daemon.

The GC (slice 5) needs no epoch/RCU machinery: each shard is the sole
accessor of its own version chains (share-nothing), so reclamation is
a single-threaded prune driven by one value -- the coordinator's
low-water mark (the minimum live snapshot, shipped to shards in
PREPARE).  A snapshot is pinned by mvcc_begin and released by
mvcc_snapshot_release.  The remaining refinement is a hard bound on
versions a single very-long-lived reader can pin (real engines abort
such readers); the demonstrator keeps a generous per-key cap.
Wiring it under the SQL engine is out of scope (see "Honest scope").

## What stage 4 reuses (already built)

  * **The deferred-reply gen_server** (`xtc_svr_call_save` +
    `XTC_SVR_NOREPLY`).  The 2PC coordinator IS the canonical user:
    receive a transaction call, defer the reply, fan prepares out to
    the shards, and reply only when every vote is in.  This is why we
    fixed deferred reply -- stage 4 is its payoff.
  * **The hybrid logical clock** (`xtc_trace.h`'s HLC, currently a
    global tracer clock).  Stage 4 promotes the same idea to a
    **per-shard** clock as the MVCC timestamp source -- decentralized,
    causal, no central allocator (the chokepoint the stage-2/3 sharded
    test exposed).  See `M_CAUSALITY.md`.
  * **The share-nothing shards** (one `xtc_svr` per shard, one loop per
    core).  Each shard already serializes its own ops; stage 4 adds
    versioning and a prepare/commit protocol to that serial owner.
  * **Causal tracing** to debug the protocol: the prepare/commit
    message fan-out shows up in `xtc-trace` with cause edges, so a
    stuck or mis-ordered transaction is legible.

## The model

Snapshot isolation with a hybrid logical clock, single-machine,
multi-shard.  Borrowed shape: CockroachDB/Yugabyte (HLC + per-key
versions + a transaction coordinator), minus the multi-node parts.

### Per-key versioning (in each shard)

A shard's table maps key -> a short version chain, newest first:

    version { uint64 commit_hlc; value; uint64 txn_id; int committed; }

  * A read at snapshot timestamp `ts` returns the newest version whose
    `commit_hlc <= ts` and `committed`.  Wait-free against other
    readers; it only walks the chain.
  * A write installs an UNCOMMITTED version tagged with the writing
    txn's id; it becomes visible only when the coordinator commits it
    with a `commit_hlc`.
  * Garbage collection (later): drop versions older than the oldest
    live snapshot.  Epoch-reclaimed (`xtc_rcu`) -- a known later need.

### The transaction coordinator (one xtc_svr, deferred reply)

A client opens a transaction, does reads/writes (buffered in the
coordinator's per-txn state), and commits.  Commit is 2PC:

  1. Coordinator picks `commit_ts = max(HLC of all shards it touched)`
     advanced once (its own HLC tick).
  2. `handle_call(COMMIT)` saves the call (`xtc_svr_call_save`),
     returns `XTC_SVR_NOREPLY`, and casts PREPARE(txn, commit_ts) to
     every participant shard.
  3. Each shard validates (no write-write conflict newer than the
     txn's snapshot; its own HLC advanced past commit_ts) and casts
     back a vote.
  4. When the coordinator has all votes: if unanimous yes, cast
     COMMIT(txn, commit_ts) to the shards (they flip the staged
     versions to committed) and `xtc_svr_reply` the saved client call
     with success; otherwise cast ABORT and reply failure.

This is precisely deferred reply + message fan-out + the per-shard HLC,
which is why the earlier stages were prerequisites.

### Read path

A read-only transaction takes a snapshot `ts` (the coordinator's HLC
at open) and reads each shard at `ts` with no locks -- the wait-free
version-chain walk.  A consistent multi-shard snapshot falls out of
the single HLC domain.

## Implementation slices (each a working, tested artifact)

  1. **Per-shard HLC.**  Give each shard `xtc_svr` its own HLC field;
     advance it on every request and carry an HLC in the shard
     request/reply messages (reuse the tick/update logic now in
     proc.c, refactored into a tiny shared helper or duplicated in the
     example).  Test: causality across shard hops holds (the
     reorderings the stage-3 test counted now carry a correct HLC).
  2. **Versioned single-shard MVCC.**  Replace each shard's flat slot
     with a version chain; add read-at-ts and staged-write.  Test:
     a reader at an old snapshot does not see a newer committed write;
     a write is invisible until committed.  Single shard, no
     coordinator yet.
  3. **The 2PC coordinator (deferred reply).**  Add the coordinator
     `xtc_svr`; implement begin / read / write / commit for
     single-shard transactions first (degenerate 2PC), proving the
     deferred-reply path end to end.  Test: a committed txn is visible
     at its commit_ts; an aborted txn leaves no trace.
  4. **Cross-shard transactions.**  Two-key transactions spanning
     shards; prepare/vote/commit fan-out; conflict detection
     (write-write).  Test: concurrent conflicting cross-shard txns --
     exactly one commits; a reader sees all-or-nothing (atomicity);
     run on the multi-loop executor with the causal tracer on so a
     failure is debuggable.
  5. **GC + bounds.**  Reclaim versions below the oldest snapshot
     (epoch / `xtc_rcu`); bound the version chain.  Test: long run,
     bounded memory.

Slices 1-2 are low-risk and self-contained.  Slice 3 exercises the
deferred-reply fix in anger.  Slice 4 is the hard, valuable core
(distributed atomicity + conflict detection) and is where new dogfood
findings are most likely -- distributed deadlock, the coordinator as a
bottleneck, snapshot-GC interaction.  Record them in
`M_SQLXTC_XTC_GAPS.md`.

## Honest scope

This proves snapshot-isolation MVCC + 2PC on libxtc's primitives in the
KV demonstrator.  Wiring it under the SQL engine (replacing SQLite's
storage) and full serializable isolation remain beyond stage 4; this
stage is the concurrency core they would build on.  As with every
earlier stage, each slice stands alone as a tested artifact, no daemon,
ASan + UBSan clean, CI-gated.
