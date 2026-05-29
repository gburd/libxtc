# kaka: a Kafka-shaped log broker on libxtc

`kaka` is the seventh worked example.  It is a single-broker,
partitioned, append-only log service with producer and consumer
clients, modelled on Apache Kafka's data path.  Its purpose is to
exercise the parts of libxtc that the earlier examples do not:
sustained backpressure, per-partition ordering, segmented on-disk
logs, and consumer-group offset tracking.

This document is the design and the build plan.  The code lands in
phases; each phase is independently testable and leaves the tree
green.

## What kaka is and is not

kaka implements the Kafka *concepts* on libxtc primitives.  It does
not implement the Kafka wire protocol, ZooKeeper/KRaft consensus, or
multi-broker replication.  The intent is to demonstrate libxtc on a
realistic streaming workload, not to be a drop-in Kafka.

In scope:

  * Topics partitioned into ordered, append-only logs.
  * Producers that publish records to a (topic, partition) with
    acknowledgement.
  * Consumers that read from an offset and commit progress.
  * Consumer groups with at-least-once delivery and per-partition
    assignment.
  * Segmented log storage with offset indexing, reusing the Bitcask
    record/CRC discipline from the rexis example.
  * Bounded memory under producer flood -- the property the review
    (docs/M_CRITICAL_REVIEW.md, section 4) identified as the real
    test of an actor runtime.

Out of scope (documented, not built):

  * Replication, ISR, leader election.
  * Exactly-once semantics / transactions.
  * Log compaction by key (only time/size retention).
  * The Kafka binary protocol; kaka uses a small framed protocol of
    its own, the way sqlxtc uses Quack rather than the PostgreSQL
    wire protocol.

## Concept-to-primitive map

This is the heart of the example: every Kafka concept is one libxtc
primitive, chosen deliberately.

| Kafka concept            | libxtc primitive                     | Why |
|--------------------------|--------------------------------------|-----|
| Broker process           | `xtc_app` + `xtc_supervisor`         | The broker is a supervised tree; a crashed partition log restarts without taking the broker down |
| Partition log            | one `xtc_proc` per partition         | Per-partition serialization falls out of the single-consumer mailbox: all writes to a partition are messages to its proc, so ordering is automatic and lock-free |
| Producer connection      | one `xtc_proc` per TCP connection    | Same per-connection model as rexis/sqlxtc; uses `xtc_proc_wait_fd` for idle-CPU-free I/O |
| Consumer connection      | one `xtc_proc` per TCP connection    | As above |
| Record append            | `xtc_send` to the partition proc     | The mailbox cap is the partition's in-flight write bound |
| Backpressure             | credit-based flow control (see below)| The mailbox cap alone drops on overflow; kaka adds credits so producers throttle instead |
| Segmented log + index    | Bitcask-style records (from rexis)   | Reuses the CRC-framed append log; segments roll at a size threshold |
| Offset / high-water mark | `xtc_lrlock` over the partition meta | Consumers read the high-water mark wait-free; the partition proc is the sole writer |
| Consumer group state     | `xtc_lockmgr` keyed by (group, part) | Partition assignment within a group is a lock: one consumer holds the (group, partition) lock at a time |
| Offset commit store      | Bitcask KV (group,partition -> off)  | Durable, survives broker restart |
| Per-topic metrics        | `xtc_stats` counters + histograms    | Produce/consume rates, append latency p50/p99 |
| Resource budget          | `xtc_res`                            | Bounded RSS, connection count, in-flight bytes |

The single most important line in that table is the partition log:
because a partition is an `xtc_proc` and all appends are messages to
it, **partition ordering and single-writer correctness are free** --
there is no per-partition lock on the write path, only the mailbox.
This is the BEAM insight applied to a Kafka partition.

## Backpressure: the part the mailbox cap does not solve

The review noted that libxtc's bounded mailbox drops messages with
`XTC_E_AGAIN` when full; it does not throttle the sender.  For a log
broker, dropping a produced record is data loss.  kaka therefore
layers credit-based flow control on top of the mailbox:

  1. A partition proc grants each connected producer a credit budget
     (records or bytes it may have in flight).
  2. A producer that exhausts its credit stops reading from its
     socket -- it removes the socket from the event loop via
     `xtc_proc_wait_fd` so it parks instead of spinning.
  3. As the partition proc durably appends records, it returns credit
     to the producer (a small control message), which resumes
     reading.

The result is end-to-end backpressure: a slow disk slows the producer
through TCP flow control, with bounded broker memory at every hop.
This is the property to benchmark -- "broker RSS stays flat under a
producer flood" -- and the one Kafka itself spends the most
engineering on.

## On-disk layout

    <dir>/<topic>-<partition>/
        00000000000000000000.log     segment, Bitcask-framed records
        00000000000000000000.index   offset -> file position
        00000000000000093124.log     next segment after roll
        ...
    <dir>/__offsets/
        bitcask.data                 group,partition -> committed offset

Each record on disk carries the Bitcask header (CRC32, timestamp,
key-len, value-len) plus the 64-bit logical offset.  Recovery on
startup scans the active segment of each partition to rebuild the
in-memory offset index and high-water mark, exactly as the rexis
Bitcask recovers its key index.

## Protocol

A small length-framed binary protocol (PROTOCOL.md, written in
phase 1).  Request types: PRODUCE, FETCH, COMMIT, OFFSETS, METADATA.
Each frame is a 4-byte length, a 1-byte type, and a type-specific
body.  The wire format is deliberately minimal; the example is about
the broker internals, not protocol design.

## Phases

Each phase is a separable change set that builds, tests, and leaves
the tree green.

  Phase 0 -- scaffold.  Directory, Makefile (links the in-tree
    libxtc.a like the other examples), README, empty PROTOCOL.md, a
    main.c that starts an `xtc_app`, binds a port, and accepts
    connections that it immediately closes.  Smoke test: connects.

  Phase 1 -- protocol + single in-memory partition.  Framing codec
    with round-trip unit tests.  One hard-coded topic/partition held
    by one partition proc with an in-memory record vector.  PRODUCE
    appends, FETCH reads by offset.  No persistence, no groups.

  Phase 2 -- segmented persistence.  Partition proc writes records to
    a Bitcask-framed segment with an offset index; segments roll at a
    size threshold; recovery rebuilds the index on restart.  A
    differential test (produce N, restart, fetch N) modelled on
    test_db_persist.

  Phase 3 -- credit-based backpressure.  Producer credit budget,
    park-on-exhaustion via xtc_proc_wait_fd, credit return on durable
    append.  Bench harness: a flood producer against a throttled-disk
    partition; assert broker RSS stays within a budget.

  Phase 4 -- consumer groups.  Group state via xtc_lockmgr keyed by
    (group, partition); offset commits to a Bitcask KV store; rebalance
    on consumer join/leave.  Test: two consumers in a group split the
    partitions, committed offsets survive a broker restart.

  Phase 5 -- observability + budgets + docs.  xtc_stats for
    produce/consume rates and append latency; xtc_res caps on
    connections and in-flight bytes; a self-contained metrics test
    like rexis test_metrics; a closing design note on what would be
    needed for replication.

## Why this example matters

rexis exercises the read-mostly KV path (xtc_lrlock, slab, res).
sqlxtc exercises the request/reply path with a shared mutable engine
(xtc_lwlock around SQLite).  kaka exercises the third shape -- a
high-throughput streaming pipeline with ordering, persistence, and
backpressure -- which is where actor runtimes either prove or
disprove their bounded-resource claims.  It is also the example most
directly comparable to a system (Kafka) that operators already have
strong intuitions about, which makes its resource behaviour easy to
judge.

## Status

Phases 1-2 complete: the framing codec, the in-memory partition log, and
the broker's partition/connection procs are implemented and tested.

  * `frame.c` -- wire codec; `test_frame` (6 checks) round-trips
    every frame and request/response type.
  * `partition.c` -- in-memory ordered log; `test_partition` (3
    checks) covers append/offset assignment, read-by-offset, and
    growth past the initial capacity.
  * `broker.c` -- partition_proc (one xtc_proc per topic/partition,
    single-owner ordering) and conn_proc (socket parked via
    xtc_proc_wait_fd).  `test_broker` drives a full PRODUCE -> FETCH
    round-trip in-process through a real xtc loop (no socket), so it
    runs under `make test` without a daemon.

The networked end-to-end path (a TCP client against the running
broker) is built but exercised manually; the in-process self-test is
the automated correctness gate for the proc-messaging layer.

Phase 2 (segmented persistence) is done: partition.c gains
plog_create_ex, which writes each append to a CRC-framed segment
file that rolls at a size threshold and is scanned + replayed on
restart.  The broker persists each partition under
<log_dir>/<topic>-<partition>/ when started with --dir.  A
standalone test produces 3000 records across several segment rolls
and verifies they survive two restarts.

Phases 3-5 (credit-based backpressure, consumer groups, full
observability) are tracked in PLAN.md.
