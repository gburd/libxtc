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

**rexis** runs one libxtc process (`xtc_proc`) per connection on ONE
`xtc_loop`, with the keyspace in a single table and hard memory and
client budgets from `xtc_res`. Its shape is Valkey's, with Erlang-style
connection processes. It does not yet shard.

## Measurements

One 64-vCPU x86-64 host (32 cores, 2 SMT threads each, one NUMA node),
loopback, server pinned to its own cores and the load generator to
others. Valkey 9.0, a release build of DragonFly, and rexis on libxtc's
io_uring backend. GET:SET 10:1, 32-byte values, 1M keys prefilled.
Medians of 3 interleaved runs.

### Throughput and scaling (closed loop, memtier_benchmark)

| server threads | Valkey | DragonFly | rexis |
|---:|---:|---:|---:|
| 1 | 219k | 195k | 153k |
| 1, pipeline 16 | 1.26M | 0.71M | 1.13M |
| 8 | 1.13M | 1.20M | n/a |
| 32 | 0.60M | 2.66M | n/a |
| 32, pipeline 16 | 0.91M | 8.41M | n/a |

Valkey peaks around 8 io-threads and then falls back as the main thread
saturates. DragonFly keeps scaling because nothing is shared. rexis
cannot use a second core.

### Tail latency (open loop)

A closed-loop benchmark issues the next request only after the previous
reply, so it stops sending while the server stalls and never measures
the stall. That error is called *coordinated omission*. The numbers
below come from a client that sends each request at a scheduled time
and measures latency from that time. Check any tool you use the same
way: plant a known stall and see whether the tool reports it. In this
study, memtier's `--rate-limiting` reported a 375 us p99 for a run that
was stalled 20% of the time; the scheduled-start client reported 194 ms.

| one server thread, 100k ops/s offered | Valkey | DragonFly | rexis |
|---|---:|---:|---:|
| p50 | 21 us | 20 us | 22 us |
| p99 | 34 us | 34 us | 34 us |
| p99.99 | 140-440 us | 140-980 us | 100-200 us |

Inside capacity, all three have the same body: the loopback round trip.
They differ in the far tail. DragonFly runs a busy-polling loop and idle
tasks on the shard's own thread, and its p99.99 is the widest. rexis,
like Valkey, sleeps in the kernel when idle.

### Persistence and memory (1M keys, 100-byte values)

| | Valkey (RDB, fork) | DragonFly (forkless) | rexis (bitcask log) |
|---|---:|---:|---:|
| extra memory during a snapshot | +67% | +1% | none (no snapshot) |
| p99 during the snapshot, 50k ops/s | 138 us | 36 us | 39 us (logging every SET) |
| restart, cold cache | 0.7 s | 0.9 s | 2.1-2.9 s |
| on-disk size | 29 MB | 10 MB | 241 MB (log, never merged) |
| bytes per key, 32 B values, 5M keys | 98 | 85 | 152 |

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
   from a faster lock. The libxtc shape is below.
3. *Snapshot by version, not by fork or by logging every write.*

**In libxtc.** The study found three runtime costs that a sharded
server would pay on every hop. As of this writing they are open, and
they are what stands between libxtc and DragonFly-class hop rates:

- the slab allocator's statistics counters share one cache line across
  cores, which caps cross-loop message throughput (removing them tripled
  it at 16 loops);
- on glibc, the default `ucontext` fiber switch makes two signal-mask
  system calls (the `fcontext` switch makes none and is 3.7x faster);
- `xtc_proc_wait_fd` re-arms its poll registration every time a fiber
  parks. DragonFly keeps one multishot poll per socket for the socket's
  whole life.

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
  This is simpler to write, and reads scale. But every write still
  serializes on its bucket, and the cache lines bounce between cores.
  DragonFly's measurements, and Valkey's flat curve, are the argument
  against it for a write-heavy cache.
- **Valkey-style I/O threads around one executor.** This scales parsing
  and socket I/O, not commands, and it peaks around 8 threads on the
  host above.
- **Fork-based snapshots.** They are simple and correct, but memory can
  rise by up to 2x under write load, and the fork itself stalls the
  server.
