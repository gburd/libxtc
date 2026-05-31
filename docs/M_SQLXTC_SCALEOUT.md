# sqlxtc -- scale-out design (the combined architecture)

This document combines the three transformation campaigns sketched in
`M_SQLXTC_HARDFORK.md` (decompose into procs), `M_SQLXTC_STORAGE.md`
(durable, recoverable, MVCC storage), and the share-nothing multi-core
idea into one architecture, and lays out the staged plan to build it.

Its purpose is twofold: produce a SQL server that scales across cores,
and -- equally -- *dogfood libxtc hard enough to find its bugs and
missing primitives*.  The companion `M_SQLXTC_XTC_GAPS.md` is the
running ledger of what that dogfooding has surfaced.

## The architecture is a 2-D decomposition

The three campaigns are not alternatives; they are three cuts of one
design:

  * Horizontal (sharding): partition the key space across cores.  One
    loop (one core, via `xtc_exec`) owns each shard.
  * Vertical (proc pipeline): the inherently serial / shared
    subsystems become singleton `xtc_proc`s -- the WAL writer, the
    checkpointer, the GC/vacuum, the timestamp allocator, the
    cross-shard coordinator, the read-mostly catalog.
  * Leaf (durable MVCC storage): at the bottom of each shard sits the
    from-scratch buffer manager + B-link tree, made durable by a WAL
    and recoverable by replay.

```
            listener (xtc_net, reuseport)
                  | accept
            conn / session proc  -- routes each statement by shard key
        +---------+-------------+---------------+
        v         v             v               v
     shard 0   shard 1       shard 2   ...   shard N      (xtc_exec: 1 loop = 1 core)
        |  within a shard: parse -> plan -> execute as cooperative fiber calls
        v
     btree / buffer pool (shard-local)  -->  WAL-writer proc (group commit) --> fsync (xtc_io)
                                                     ^
        cross-shard txns -->  timestamp allocator + 2PC coordinator (MVCC snapshots)
```

## Variant C: hybrid share-nothing + global control procs

Each shard's storage is touched by exactly one loop, so WITHIN a shard
fibers are cooperative and most in-shard locking disappears (the
Seastar insight).  Locking survives only where a shard's own helper
threads touch shard-local pages -- the page provider and the offloaded
fsync/eviction -- which is the narrow, correct role for the per-frame
`xtc_arwlock` content latch.  Across shards, state is never shared;
coordination is by message.

A small set of inherently-global, serial subsystems stay as singleton
procs, each supervised:

  * timestamp / txn-id allocator  (MVCC snapshot points)
  * cross-shard 2PC coordinator   (atomic multi-shard commit)
  * read-mostly catalog           (`xtc_lrlock` COW)
  * GC / vacuum                    (epoch reclamation, `xtc_rcu`)

Per-shard WAL writers give durability that scales across cores; the
coordinator drives prepare/commit across them for cross-shard
transactions.

## What this dogfoods, by primitive

| Subsystem | xtc primitive(s) exercised | likely gap to surface |
|---|---|---|
| Shard routing | `xtc_exec`, `xtc_shard_id`, cross-loop send | cross-loop wakeup cost; proc migration? |
| In-shard engine | cooperative fibers, yield watchdog | a non-yielding fiber starves its core |
| WAL writer + group commit | `xtc_proc` mailbox drain, timed `xtc_recv`, `xtc_blocking` fsync, `xtc_timer` | "wait for N-or-T" coordination; gen_server deferred reply |
| Global control procs | `xtc_svr`, `xtc_chan`, `xtc_reg`, `xtc_supervisor` | deferred / multi-reply; restart state recovery |
| MVCC + GC | atomics, `xtc_rcu` | epoch reclamation API at scale |
| Cross-shard txn | 2PC coordinator proc, `xtc_lockmgr` | distributed deadlock; coordinator as bottleneck |
| Crash testing | `xtc_inject`, R3 `xtc_osproc_spawn` | recovery correctness; OS-process tier |

## Staged plan

Each stage is a working artifact and a focused dogfood.

  1. **WAL-writer proc + group commit** on the single greenfield
     shard.  The soonest place we are forced to admit a missing
     primitive.  (This stage: `wal.{c,h}`, `test_wal`.)
  2. **Shard-router seam** while there is still only one shard (route
     through a router that always picks shard 0).  Cheap now, brutal
     to retrofit.
  3. **N shards on N loops, single-shard transactions only.**  The
     multi-core scaling proof for the easy case (share-nothing,
     no cross-shard coordination yet).
  4. **Cross-shard transactions** via MVCC snapshots + a 2PC
     coordinator + a timestamp allocator.  The richest coordination
     dogfood.
  5. **Supervise + crash-inject** the shards and the global procs.

Stages 1-3 are "what's possible from A and C today" on the existing
greenfield engine; stages 4-5 are the capstone.

## Honest scope

The from-scratch engine is not yet wired into `engine.c` (SQLite is
still the live backend), and SQL execution still runs on stock SQLite
in serialized mode.  This plan builds the scale-out substrate
underneath that facade, stage by stage, until it can take over.  Each
stage stands alone as a tested artifact even before the cut-over.
