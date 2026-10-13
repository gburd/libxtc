# kaka: a Kafka-shaped log broker on libxtc

`kaka` is the seventh worked example.  It is a single-broker,
partitioned, append-only log service with producer and consumer
clients, modelled on Apache Kafka's data path.  Its purpose is to
exercise the parts of libxtc that the earlier examples do not:
sustained backpressure, per-partition ordering, segmented on-disk
logs, and consumer-group offset tracking.

This document is the design.  It was written as a phased build plan;
the Status section at the end says what was built and how it is
tested.

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
  * Bounded memory under a producer flood -- records in closed (rolled)
    segments are evicted from RAM and cold reads go to the segment
    files through an offset index, so resident memory stays bounded
    (~2 segments) no matter how many messages are appended.  Combined with credit-based
    backpressure on in-flight requests, this is the property that is the
    real test of an actor runtime.

Out of scope (documented, not built):

  * Replication, ISR, leader election.
  * Exactly-once semantics / transactions.
  * Log compaction by key, and time-based retention (only size-based
    retention, `--retention-bytes`, is implemented; off by default).
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
| Offset / high-water mark | partition proc state                 | The partition proc is the sole writer and reader; FETCH asks it |
| Consumer group state     | (not built)                          | Partition assignment / rebalancing is not implemented; consumers pick their partitions |
| Offset commit store      | coordinator proc + segmented log     | COMMIT / OFFSETS requests; each commit is fdatasync'ed before its ack and replayed on restart |
| Per-topic metrics        | `xtc_stats` counters + histograms    | Produce/consume rates, append latency p50/p99 |
| Resource budget          | `xtc_res`                            | `--max-memory` caps record bytes held in RAM; `--max-clients` caps connections |

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
  3. As the partition proc durably appends records (fdatasync before
     the ack), it returns credit to the producer (the ack), which
     resumes reading.

The in-flight credit window is expressed with libxtc's
`xtc_credit` regulator (`broker_credit_selftest` in broker.c): the
producer takes a credit before each PRODUCE and returns it on each
reply, so at most N requests are ever outstanding, and
`xtc_credit_peak` reports the observed high-water for tuning.  This is
the Erlang `:jobs` / RabbitMQ `credit_flow` / GenStage
`max_demand` sliding-window pattern packaged as a primitive.

The result is end-to-end backpressure: a slow disk slows the producer
through TCP flow control, bounding the in-flight (un-acked) request
budget at every hop.  This flow-control property is the one to
benchmark, and the one Kafka itself spends the most engineering on.

**Memory footprint (measured):** the partition log keeps only a bounded
WINDOW of recent records in RAM (partition.c: the in-memory slot vector).
When a segment rolls closed its records are durable on disk, so they are
EVICTED from RAM; a read of an evicted offset is served from the segment
file through the segment's sparse offset index (`plog_fetch`).  Resident memory therefore stays bounded (~2
segments) regardless of how many messages have been appended -- verified
by test_partition's `bounded_memory` case (20,000 appends, RAM peak ~1300
records, evicted reads still correct).  So the broker is bounded in BOTH
in-flight work and cumulative resident memory.  Recovery does not load
records into RAM: it scans the segments to rebuild the index (CRC-checking
the newest one), and the RAM window starts empty.

## On-disk layout

    <dir>/<topic>-<partition>/
        00000000000000000000.log     segment, CRC-framed records
        00000000000000093124.log     next segment after roll
        ...
    <dir>/__offsets/
        00000000000000000000.log     committed offsets (same format)

Each record on disk is `crc32 | offset u64 | key_len u32 | value_len
u32 | key | value`; the bytes after the CRC are exactly the FETCH wire
encoding.  The offset index (one entry per ~4 KiB) is kept in memory
and rebuilt by scanning the segments at startup, as the rexis Bitcask
rebuilds its key index.  A segment is fdatasync'ed before the next one
is created, so only the newest segment can have a torn tail; it is
truncated at the first bad record.

## Protocol

A small length-framed binary protocol (PROTOCOL.md, written in
phase 1).  Request types: PRODUCE, FETCH, COMMIT, OFFSETS (METADATA is
reserved).
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

Built and tested (`make test`, in-process, no daemon):

  * `frame.c` -- wire codec; `test_frame` round-trips every frame type.
  * `partition.c` -- the segmented log; `test_partition` covers offsets,
    growth, recovery across rolls and restarts, bounded RAM, ack-needs-
    sync and fail-stop on a failed sync, torn-tail truncation, cold
    reads through the index, and size-based retention.
  * `broker.c` -- partition and connection procs.  `test_broker` drives
    them through a real xtc loop: PRODUCE/FETCH, the credit window
    (peak in-flight <= 4), metrics and the memory budget (over-cap
    PRODUCE answered with a MEMORY error, nothing stored), group commit
    (64 queued produces, one fdatasync), fail-stop after an fsync error,
    the budget tracking live RAM rather than lifetime bytes, and a
    request that outlives its connection's timeout (it owns its payload).
  * `group.c` -- the offset coordinator; `test_group` covers commit /
    fetch, isolation, and replay after a restart.

The networked paths (accept back-off under fd exhaustion, SIGTERM drain
of a loaded broker, COMMIT/OFFSETS across a kill -9, kill -9 crash
recovery of acked records, cold-FETCH latency) are exercised by an
external harness against a running broker, not by `make test`.

Durability modes (main.c):

  * default -- a PRODUCE is acked after fdatasync (group commit).
  * `--fsync-interval-ms=N` -- acked after write(2); fdatasync at most
    every N ms, on segment roll, and at shutdown.  A broker crash loses
    nothing (the page cache survives it); a kernel crash or power loss
    can lose up to N ms of acked records.

Not built: replication, consumer-group membership and rebalancing,
METADATA, time-based retention, compaction.
