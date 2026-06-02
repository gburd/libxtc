# sqlxtc: the WAL-logged storage engine (ARIES/AETHER), knitted together

This document ties the pieces -- the write-ahead log, the buffer
manager (eviction, the trickler, prefetch), and recovery -- into one
coherent transactional storage engine, maps each piece to the
classical ARIES design as implemented by Stasis (Sears,
`~/src/stasis-aries-wal`), and states precisely what is built and
tested versus what remains.

## 1. The shape of the engine

```
   SQL (SQLite VDBE)
        |  xstore virtual table (xstore.c)
        v
   MVCC versions: key (rowid, ~commit_ts), value = [flags][bytes]
        |                                   ^
   commit: log-before-apply                | read at snapshot
        v                                   |
   WAL (wal.c) ----group commit---> durable log file
        |                                   ^
        | apply (bt_insert)                 | recover: replay (wal_scan)
        v                                   |
   B-link B-tree (btree.c) over the cooling buffer pool (bufmgr.c)
        |  trickler writes dirty pages ahead of eviction (recLSN order)
        |  prefetch warms the next leaf; scan-resistant cooling stage
        v
   page file
```

## 2. What is implemented and tested

### 2.1 Logical-redo WAL, log before apply (the write-ahead rule)

A committing transaction serializes its versions into one record
`{commit_ts, [(rowid, flags, value)...]}` and makes it **durable before
any of them touch the B-tree** (`xs_wal_log` / `xs_wal_put` ->
`wal_commit` on a loop, `wal_commit_sync` off a loop).  This single
ordering choice gives the engine its recovery model for free:

  * **Redo is available.**  A crash after the log is durable can replay
    the record and re-apply the inserts.
  * **Undo is unnecessary.**  Because MVCC inserts versions only at
    commit, and only after the log is durable, the B-tree never holds
    uncommitted version data -- a crash before the commit record is
    durable has not touched the tree, so there is nothing to roll back.
    This is why there is no UNDO pass (contrast Stasis
    `recovery2.c:stasis_recovery_undo`, which walks `prevLSN` chains and
    writes CLRs): the MVCC commit protocol stands in for no-steal of
    uncommitted data.
  * **Redo is idempotent** without page-LSN gating because version keys
    `(rowid, ~commit_ts)` are immutable: re-inserting one yields the
    identical entry.  (Stasis gets redo idempotence from the per-page
    `pageLSN` comparison inside `stasis_operation_redo`; our immutable
    keys give it structurally.)

The ARIES write-ahead invariant -- *a page is not written to disk until
the log records describing it are durable* (Stasis
`pageHandle.c:phWrite` forces the log to `page->LSN` before the write)
-- is therefore satisfied **trivially** here: a page is dirtied only by
applying an already-durable commit, so the log is always durable past
whatever a flush could write.  The trickler and eviction may write any
dirty page without a log force.

### 2.2 Group commit

`wal.c` is a dedicated group-commit writer process (an `xtc_proc` that
owns the log fd): committers send their record and park on the ack; the
writer batches every commit arriving within a window into one
`write(2)` + one `fdatasync(2)`, offloaded via `xtc_blocking_run` so the
loop keeps serving.  This is Stasis `groupForce.c:stasis_log_group_force`
-- one fsync amortized across many commits.  Off a loop (no writer
process) `wal_commit_sync` appends + fsyncs synchronously under a mutex;
both paths share the on-disk format, so `wal_scan` replays either.

### 2.3 Recovery (redo)

`xstore_recover` -> `wal_scan` reads the log in LSN order and re-applies
every complete record's versions (`bt_insert`), advancing the commit
clock past the highest recovered timestamp.  A torn trailing record (a
partial write at the moment of the crash) is detected by a short read
and ends the scan -- everything before it is intact.  This is ARIES
analysis+redo collapsed into one pass because there is no undo and no
checkpoint to start from yet (see 3).

`test_wal_recover` proves it end to end: 40 explicit two-row
transactions + 20 autocommit inserts commit through the WAL on a loop;
the entire buffer pool is then destroyed WITHOUT flushing (the pool was
large enough that nothing was ever evicted, so the data file received
no page -- total loss of the materialized data); a fresh, empty B-tree
then recovers all 100 rows from the log alone.  This is the NO-FORCE
property: commits are durable via the log without forcing the data
pages.

### 2.4 The buffer manager: eviction, trickler, prefetch

  * **Cooling-stage eviction with scan resistance** (LeanStore +
    2Q): demand-loaded pages enter the COOL probationary stage and are
    promoted to HOT only on a second access, so a scan does not evict
    the hot set.  Eviction reclaims clean pages and prefers COOL
    victims.  (See docs/M_SQLXTC_STORAGE.md sec 2.5.)

  * **Trickler** (`bm_trickler_spawn`): writes dirty pages out ahead of
    eviction so reclaiming a frame is a state flip, never a synchronous
    write on the fault path.  It orders writeback the way ARIES does --
    COOL (imminent-victim) pages first, then oldest-dirtied first via a
    per-frame `dirty_seq` (our recLSN proxy) -- and paces a bounded
    batch per pass so writeback is smoothed, not bursty.  This is the
    role of Stasis `truncation.c` + `bufferHash.c:writeBackWorker`
    driving `dirtyPageTable.c:stasis_dirty_page_table_flush_with_target`
    (LSN-ordered when the goal is truncation).

  * **Torn-free flush.**  `flush_frame` snapshots the page under a
    NON-BLOCKING try-shared latch (so it never writes a half-modified
    image and never deadlocks with the B-tree's latch coupling), clears
    dirty under the latch, releases, then writes the snapshot -- no
    latch held across I/O.

  * **Prefetch** (`bm_prefetch_pid`): a non-blocking read-ahead request;
    the page-provider warms the page in the background (resident COOL).
    The B-link cursor prefetches the next leaf's `right_sibling`, so a
    forward scan stays a cache hit.  This is Stasis
    `bufferHash.c:bhPrefetchPagesImpl` + `prefetch_worker`.

## 3. What remains for full ARIES (honestly scoped)

The engine recovers committed data correctly today by replaying the
whole log into the B-tree (treating the page file as a rebuildable
cache).  Three pieces remain to make it a fully checkpointed,
cold-restartable ARIES system:

  1. **Persistent superblock + non-truncating reopen.**  `bm_create`
     currently opens the page file `O_TRUNC` and `bt_open` always
     creates a fresh root, so the page file is never read back on
     restart.  A superblock page (root pid, `next_pid`, page size) plus
     a reopen mode that preserves the file is the prerequisite for the
     page file to serve as a durable checkpoint base.

  2. **Checkpoint + log truncation.**  Once the page file is a durable
     base: a checkpoint flushes all dirty pages (`flush_frame` over the
     dirty set, in `dirty_seq` order -- the trickler already does this
     incrementally) + `fdatasync`, records the checkpoint LSN, and
     truncates the WAL prefix whose effects are all materialized.  The
     truncation horizon is the minimum `dirty_seq` over still-dirty
     pages -- exactly Stasis `dirtyPageTable.c:minRecLSN` driving
     `truncation.c:stasis_truncation_truncate`.  This bounds the log
     and recovery time (recovery then replays only from the checkpoint
     LSN forward, ARIES `redoLSN`).  `wal_truncate` is the missing
     primitive; it is only SAFE after the page file durably reflects
     the truncated prefix, which needs (1).

  3. **Physiological SMO logging for torn B-tree pages.**  Logical redo
     reconstructs committed data onto a consistent base, but a crash
     mid structure-modification (a leaf split that updated the leaf but
     not the parent) can leave the on-disk tree structurally torn.
     ARIES handles this with nested top actions + physiological page
     redo and page LSNs (Stasis logs an SMO and redoes it idempotently
     via `pageLSN`).  Adding a per-page `pageLSN` and physiological
     redo records for node mutations is the path to STEAL-safe restart
     from a torn page file; until then, recovery from the log onto a
     freshly built (or cleanly checkpointed) tree is the supported
     model.

## 4. Why this order

Durability (the log) and recovery come first because they are the
property the rest depends on; the trickler and prefetch make the data
path fast and smooth without changing correctness; checkpointing is an
optimization that bounds the log and is only meaningful once the page
file is a durable, reopenable base.  The MVCC commit protocol's
"versions appear only at commit" is the lever that let the whole thing
be redo-only -- the single most simplifying decision, and the reason
this is tractable as an example rather than a multi-year ARIES port.

## References

  * Mohan et al., "ARIES: A Transaction Recovery Method...", ACM TODS
    1992 (analysis/redo/undo, CLRs, pageLSN, fuzzy checkpoints).
  * Sears, Stasis (`~/src/stasis-aries-wal`): `recovery2.c`,
    `truncation.c`, `dirtyPageTable.c`, `logger/groupForce.c`,
    `pageHandle.c`, `bufferManager/bufferHash.c`.
  * Leis et al., "LeanStore", ICDE 2018 (swizzling, cooling stage).
  * Johnson and Shasha, "2Q", VLDB 1994 (probationary admission).
