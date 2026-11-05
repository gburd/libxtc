---
title: rexis vs DragonFly and Valkey
parent: Examples
nav_order: 13
permalink: /examples/rexis-vs-dragonfly/
lede: >-
  What a multi-core Redis-compatible server needs, measured against DragonFly and Valkey, and what libxtc gives you toward it.
---

1. TOC
{:toc}

---

This page compares [rexis]({{ '/examples/rexis/' | relative_url }}), the
libxtc RESP server in `examples/05_rexis`, with two production servers
speaking the same protocol: [Valkey](https://valkey.io/) (the Redis
fork) and [DragonFly](https://www.dragonflydb.io/), a server built from
the start for many cores. It explains how DragonFly gets its speed,
shows the measurements, and says which of DragonFly's ideas belong in an
application built on libxtc and which belong in libxtc itself.

## Three designs

**Valkey** executes every command on one main thread. Its io-threads
read, parse and write sockets, but the command still runs on the main
thread, so the server scales I/O, not execution. Snapshots `fork()` and
let the kernel's copy-on-write keep the parent's pages stable while the
child writes them out.

**DragonFly** splits the keyspace into one shard per thread. A key's
shard is a hash of the key modulo the shard count, and only the thread
that owns a shard ever touches it, so the data needs no locks. A client
connection lives on one thread for its whole life as a fiber. When it
runs a command for a key owned by another thread, it sends a small
callback to that thread's task queue and waits for the answer. That
round trip is called a *hop*. If the connection already runs on the
owning thread, the command runs inline with no hop. Commands that touch
several shards (`MSET`, `MULTI`/`EXEC`, Lua) are made atomic with VLL
("very lightweight locking"): intent-lock counters per key and a global
transaction id, taken only when more than one shard is involved.
Underneath are three data-structure choices:

- the **Dash table**, an extendible hash: fixed-size segments behind a
  small directory, and a full segment splits on its own, so no operation
  rehashes the whole table;
- **forkless snapshots**: every bucket carries a version, a background
  fiber serializes buckets older than the snapshot's version, and a
  write to a not-yet-serialized bucket serializes its old contents
  first;
- **per-thread allocator heaps**, so allocation never takes a shared
  lock and each shard knows its own memory use exactly.

**rexis** runs one libxtc process (`xtc_proc`) per connection, with
hard memory and client budgets from `xtc_res`. With `--loops=N` the
connections are spread over N `xtc_exec` loops, and the keyspace is cut
into 64 shards by key hash, each with its own reader/writer latch
(`xtc_arwlock`). Any loop runs any command on any shard, taking the
latches of the keys it names. Its shape is Valkey's keyspace with
Erlang-style connection processes on several threads. It is not
DragonFly's shard-per-thread design: nothing owns a shard.

## Measurements

One two-socket 64-core x86-64 bare-metal host (Ice Lake, 128
threads, 1 TiB), Debian 13, kernel 6.12, loopback. The OS was tuned for
measurement: the performance governor, the C1E and C6 idle states
disabled, transparent hugepages and NUMA balancing off, irqbalance
stopped. The host has two NUMA nodes. Each server ran on node 0, first
on its physical cores and then on their SMT siblings, with its memory
bound to node 0. The load generator ran on node 1. The servers were
Valkey 9.0.6 (built from source, jemalloc), a release build of
DragonFly (commit `b113c641`, helio `40b7b204`), and rexis from libxtc
2.1.1-37 (`a2134a08`, examples `7748ab9`) on the io_uring backend with
the default `fcontext` fiber switch. rexis runs `--loops=N`: N
`xtc_exec` loops over a keyspace cut into 64 shards, each guarded by
an `xtc_arwlock`. The workload was GET:SET 10:1, 32-byte values, 1M
keys prefilled. Each cell is the median of 3 interleaved runs. The
raw results and the scripts are kept with the benchmark kit, not in
this tree.

### Throughput and scaling (closed loop, memtier_benchmark)

memtier_benchmark runs with 2N threads (4 to 32) and 16 connections per
thread for 15 s. At 16 and 32 threads without pipelining the table
uses 64 client threads with 8 connections each (512 connections, as
before). With 32 threads, memtier's own threads were saturated at
32 server threads (it warned on all 64 of them), and the extra
client raised rexis from 2.21M to 2.39M ops/s. DragonFly stayed the
same (2.32M and 2.30M).

| server threads | Valkey | DragonFly | rexis `--loops=N` |
|---:|---:|---:|---:|
| 1 | 101k | 92k | 103k |
| 2 | 102k | 172k | 200k |
| 4 | 300k | 333k | 395k |
| 8 | 668k | 655k | 771k |
| 16 | 755k | 1.23M | 1.42M |
| 32 | 0.65M | 2.30M | 2.39M |
| 1, pipeline 16 | 0.75M | 0.44M | 0.85M |
| 2, pipeline 16 | 1.11M | 0.86M | 1.49M |
| 4, pipeline 16 | 1.32M | 1.58M | 2.75M |
| 8, pipeline 16 | 1.26M | 2.80M | 4.83M |
| 16, pipeline 16 | 1.18M | 5.08M | 7.73M |
| 32, pipeline 16 | 1.06M | 9.09M | 11.27M |

Every run of a cell is within 3% of the cell's median, except three
cells: Valkey at 32 threads (0.60-0.64M), and rexis at 16 threads
(1.35-1.42M) and 32 threads (2.38-2.57M) without pipelining.

All three servers scale differently:

- **Valkey** peaks at 16 io-threads without pipelining and at 4 with
  it, then falls back, because every command still runs on its one
  main thread.
- **DragonFly** keeps scaling to 32 threads because nothing is shared.
- **rexis now scales too.** With `--loops=N` it is ahead of DragonFly
  at every thread count in this workload: by 4-19% without pipelining,
  and by 24-92% with it.

The result says less than it seems to:

- **No hot keys.** rexis's shards are not DragonFly's. Any loop runs
  any command on any shard, so a hot key still serializes its writers,
  and its cache lines move between cores.
- **Few writes.** This workload is 91% GET, over 1M uniformly random
  keys: the case a reader/writer latch handles best.
- **SET-only scales less.** With a SET-only load (valkey-benchmark,
  8 threads, 200 connections, pipeline 16), rexis leads to 4 threads
  (1.75M against DragonFly's 1.36M), the two are level at 8 (2.53M and
  2.57M), and DragonFly leads at 16 and 32 (4.33M and 4.30M against
  3.20M and 3.50M). Valkey stays below 0.9M. The
  [rexis page]({{ '/examples/rexis/' | relative_url }}) has the curve.

With one server thread every server is about 100k ops/s here, against
140-219k on the one-node host of the earlier study. The cause is the
socket crossing. With the load generator on node 0 instead (the
server's own node), the same one-thread runs give:

- **Valkey:** 169k, and 0.92M with pipeline 16;
- **DragonFly:** 147k and 0.50M;
- **rexis:** 174k and 1.08M.

So a loopback round trip that crosses the socket costs a one-thread
server 37-41% of its rate without pipelining, and 12-21% with it. The
multi-thread rows keep the cross-socket placement so that the client
never takes the server's cores.

### Tail latency (open loop)

A closed-loop benchmark issues the next request only after the previous
reply, so it stops sending while the server stalls and never measures
the stall. That error is called *coordinated omission*. The numbers
below come from a client that sends each request at a scheduled time
and measures latency from that time. Check any tool you use the same
way: plant a known stall and see whether the tool reports it. In the
earlier study, memtier's `--rate-limiting` reported a 375 us p99 for a
run that was stalled 20% of the time; the scheduled-start client
reported 194 ms.

The client was 8 threads with 64 connections at one server thread, 16
threads with 256 connections at 8, and 32 threads with 512 connections
at 32. Each run was 20 s after a 3 s warm-up, 3 runs per cell, and no
run had an error or an unanswered request. Latencies are in us. The
p99.99 cell shows its median, with the range of the 3 runs.

| server threads, offered ops/s | Valkey p50 / p99 / p99.99 | DragonFly p50 / p99 / p99.99 | rexis p50 / p99 / p99.99 |
|---|---:|---:|---:|
| 1, 50k | 28 / 32 / 412 (404-428) | 27 / 32 / 416 (404-436) | 26 / 30 / 330 (326-346) |
| 1, 75k | 26 / 33 / 498 (498-514) | 27 / 40 / 506 (498-518) | 25 / 30 / 354 (354-362) |
| 1, 100k | 561 / 995 / 1,237 (1,172-1,253) | 782 / 1,352 / 1,712 (1,663-1,728) | 272 / 506 / 872 (832-905) |
| 1, 100k, client on the server's node | 17 / 20 / 494 (494-537) | 18 / 22 / 518 (514-522) | 16 / 18 / 342 (334-350) |
| 8, 200k | 61 / 107 / 461 (461-514) | 33 / 46 / 121 (106-253) | 26 / 34 / 54 (52-55) |
| 8, 400k | 59 / 110 / 502 (502-510) | 41 / 67 / 301 (301-444) | 26 / 428 / 676 (660-700) |
| 32, 500k | 62 / 569 / 872 (856-889) | 34 / 47 / 68 (60-76) | 26 / 35 / 53 (52-57) |
| 32, 1.5M | over capacity: p50 2.9 s | 45 / 74 / 161 (107-420) | 28 / 134 / 334 (317-338) |

- **One thread.** Inside capacity all three have the same body (p50
  25-28 us, p99 30-40 us): the cross-socket loopback round trip. At
  100k/s with the client on the far node all three are over capacity,
  and that queue is the latency. With the client on the server's node
  the same 100k/s is inside capacity for all three. rexis has the
  lowest p99.99 at every one-thread load, 0.7-0.8x Valkey's and
  DragonFly's.
- **Eight threads.** At 200k/s rexis's p99.99 is 54 us, against 121 us
  for DragonFly and 461 us for Valkey. At 400k/s rexis's p99 jumps to
  428 us while DragonFly's stays at 67 us. Eight rexis loops carry
  about half of their closed-loop rate there (771k), and each loop
  thread is busy 58-60% of the time, so the p99 is queueing inside the
  server, not a saturated core. Where it queues was not measured.
- **Thirty-two threads.** At 500k/s rexis and DragonFly have similar
  tails (p99.99 53 and 68 us). At 1.5M/s Valkey has fallen behind its
  schedule, and rexis's p99 (134 us) is 1.8x DragonFly's.
- **Maxima.** Maxima are not shown. Inside capacity, every run whose
  maximum passed 2 ms also had the client itself 2-11 ms late on one of
  its scheduled sends, so the stall was on the client side, not in the
  server.

### Persistence and memory (1M keys, 100-byte values)

Each server loaded 1M keys of 100 bytes (pipelined SETs). Then the
client offered 50k ops/s open loop (SET:GET 1:1) for 30 s, and a
snapshot was triggered 7 s in. rexis has no snapshot: its bitcask log
appends every SET as it happens, so its row shows the same load with
that logging on. The server was then restarted with a cold page cache,
and the restart time runs until every key is loaded. 3 runs each;
every range was within 2% unless shown.

| | Valkey (RDB, fork) | DragonFly (forkless) | rexis (bitcask log) |
|---|---:|---:|---:|
| load 1M keys (memtier, which ends on a whole second) | 2 s | 3 s | 4 s |
| extra memory during a snapshot | +65% | +0.7% | none (no snapshot) |
| p99 during the snapshot, 50k ops/s | 3.9 ms | 55 us | 33 us (logging every SET) |
| p99.99 during the snapshot | 8.2-8.8 ms | 0.9-2.5 ms | 358-363 us |
| restart, cold cache | 0.95 s | 1.42 s | 3.80 s |
| on-disk size | 29 MB | 9.8 MB | 241 MB (log, never merged) |
| RSS bytes per key, 32 B values, 5M keys | 97 | 86 | 152 |

- **Valkey's fork.** In this test the server and its forked child
  share one pinned core, so the child's 1.44 s snapshot competes with
  the main thread. That shows as a 3.9 ms p99, where the one-node
  study measured 138 us. Given a core of its own
  (`--bgsave-cpulist`), the child takes 0.79 s, the p99 during the
  snapshot is 33 us, and the extra memory is +45%; the p99.99 is still
  4.2-5.5 ms (2 runs).
- **rexis's memory.** rexis holds 152 bytes per key of RSS against its
  own accounting of 120 (INFO `used_memory`). The difference is malloc
  overhead that `xtc_res` cannot see.
- **rexis's log.** rexis loads about 3x faster than in the earlier
  study (13 s), and restarts 4.3x faster (16.2 s), because its tables
  now grow. Its log is
  still 241 MB, because every SET is appended and the log is never
  merged.

## What to take from DragonFly, and where it goes

**In the application (rexis).**

1. *Tables must grow.* rexis used to have a fixed 65,536-bucket
   keyspace and a fixed 4,096-bucket persistence index. At 1M keys that
   cost 40% of its pipelined throughput, and at 5M keys a load slowed to
   a crawl. Both are now linear hashes (`examples/05_rexis/lhash.h`):
   one bucket splits per insert, so, like a Dash segment split, growth
   never rehashes the whole table. That one change took rexis from 665k
   to 1.13M pipelined ops/s at 1M keys.
2. *Own the data per loop.* DragonFly's scaling comes from shards, not
   from a faster lock. rexis's `--loops` shards its keyspace behind 64
   latches instead, and on the read-heavy load above that already
   scales as far as DragonFly does. Its limits are the ones a latch has
   and an owner does not: a hot key, and a write-heavy load. At 8
   loops and 400k ops/s its p99 is 428 us where DragonFly's is 67 us. The
   shard-per-loop shape, where nothing is shared, is below.
3. *Snapshot by version, not by fork or by logging every write.*

**In libxtc.** The earlier study found three runtime costs that a
sharded server pays on every hop. Two are fixed and the third is
smaller:

- *Slab statistics.* The slab allocator's statistics counters shared
  one cache line across cores, and every cross-loop message bounced
  it. They are now striped per thread (2.0): cross-loop round trips
  at 16 loops went from 3.0M to 9.5M a second.
- *The fiber switch.* On glibc the `ucontext` fiber switch made two
  signal-mask system calls. `fcontext`, which makes none, is now the
  default on Linux x86-64 and aarch64 (1.59), and every number above
  uses it.
- *Re-arming polls.* `xtc_proc_wait_fd` re-armed its poll registration
  every time a fiber parked, in an `io_uring_enter` of its own. The
  re-arm now goes into the same `io_uring_enter` that waits (1.60), and
  a timed-out poll stays armed for the next park (1.59). Polls are
  still one-shot, so a poll that fired is re-armed at the next park;
  DragonFly keeps one multishot poll per socket for the socket's whole
  life.

## The shard-per-loop shape on libxtc

`xtc_exec` gives you N loops on N threads, `xtc_shard_id()` tells a
fiber which loop it runs on, and `xtc_send` / `xtc_recv` carry a request
to another loop and the reply back. A key's owner is a hash; the owner
proc is the only code that touches its slice:

{% include snippet.html file="16_shard_per_loop.c" region="shard" %}

A client runs the command inline when it is already on the owner's loop
(DragonFly's `CanRunInlined`), and otherwise does one round trip:

{% include snippet.html file="16_shard_per_loop.c" region="hop" %}

The complete program is
[`docs/_includes/snippets/16_shard_per_loop.c`](https://codeberg.org/gregburd/libxtc/src/branch/main/docs/_includes/snippets/16_shard_per_loop.c).
It is built and run by the documentation gate. It prints how many
commands ran inline and how many hopped, then checks that no increment
was lost.

What this sketch does not include, and a server would need: batching
several commands into one hop per shard (DragonFly's "squashing"),
atomicity for commands that span shards (VLL or a simpler two-phase
scheme), and per-loop memory budgets (one `xtc_res` per loop with a
global cap reconciled periodically, rather than one shared counter).

## Alternatives considered

- **One table shared by every loop under a lock or RCU** (`xtc_chash`).
  This is simpler to write, and reads scale; rexis's `--loops` is this
  design with 64 latched shards. But every write still serializes on
  its shard, and the cache lines bounce between cores. rexis's tail at
  eight loops and 400k ops/s (above), and its SET-only curve on the
  [rexis page]({{ '/examples/rexis/' | relative_url }}), are the
  argument against it for a write-heavy cache.
- **Valkey-style I/O threads around one executor.** This scales parsing
  and socket I/O, not commands. On the host above it peaks at 16
  io-threads without pipelining, and at 4 with pipelining.
- **Fork-based snapshots.** They are simple and correct, but memory can
  rise by up to 2x under write load. On the host above, the fork and
  the child's writes cost Valkey a 3.9 ms p99 during the snapshot.
