# examples/05_rexis -- rexis (Redis-protocol-compatible) server built on xtc

A drop-in replacement for a meaningful subset of `redis-server`, implemented
in ~2,000 lines of C using xtc primitives.  Demonstrates how to build a
production-shaped network service with hard resource budgets.

## Quick start

```sh
# Build (assumes libxtc.a in ../../build_unix/)
cd examples/05_rexis
make

# Run with default settings (port 6379, no caps)
./rexis-server-xtc

# Run with hard caps for the P99-conference demo
./rexis-server-xtc \
    --port=16379 \
    --cores=2 \
    --max-memory=$((100*1024*1024)) \
    --max-keys=1000000 \
    --max-clients=10000 \
    --max-iops=100000

# Talk to it with the real redis-cli (or any RESP2/RESP3 client)
redis-cli -p 16379 PING
redis-cli -p 16379 SET hello world
redis-cli -p 16379 GET hello
```

## What's supported

35 commands across the major Redis data types:

| Category | Commands |
|---|---|
| Strings | GET, SET, DEL, EXISTS, INCR, DECR, INCRBY, DECRBY |
| Keys | EXPIRE, TTL, KEYS, FLUSHDB, DBSIZE |
| Lists | LPUSH, RPUSH, LPOP, RPOP, LLEN, LRANGE |
| Hashes | HSET, HGET, HDEL, HKEYS, HVALS, HGETALL, HLEN |
| Connection | PING, ECHO, QUIT, AUTH, SELECT |
| Admin | INFO, COMMAND, CLUSTER NODES |

Both RESP2 (Redis 1.x-5.x) and RESP3 (Redis 6+) protocols are accepted.

## What's NOT supported (deliberately)

Out of scope for a 2k-LOC example.  No Pub/Sub, no Streams, no scripting
(EVAL/Lua), no cluster mode, no modules, no replication, no persistence
(no AOF, no RDB), no transactions (MULTI/EXEC), no Sorted Sets (ZADD/ZRANGE).
A full Redis would add another 30k LOC.

## Resource budget enforcement

This is the centerpiece for the P99 conference demo.  All five caps below
hold under stress:

| Flag | What it bounds | How |
|---|---|---|
| `--cores=N` | CPU cores used | `sched_setaffinity(2)` pins the process; xtc_loops fanned out via xtc_exec |
| `--max-memory=N` | RSS bytes | Every alloc charged to `xtc_res` with `XTC_RES_MEM_BYTES` cap; OOM error returned to client on exceed |
| `--max-keys=N` | DB key count | Per-DB counter; SET/HSET/LPUSH refused above cap |
| `--max-clients=N` | Concurrent connections | Listener checks count before spawning per-conn proc |
| `--max-iops=N` | Commands/sec | Token bucket refilled every second; excess commands rejected with `OVER_LIMIT` |

Verified by `test/m99/test_rexis_budgets.c` (5 scenarios).  RSS stays
within `--max-memory` plus a small constant overhead even under
SET-until-OOM stress.

## Architecture

```
+------------------------+
|  xtc_app               |  startup / shutdown / supervisor
|     |                  |
|  xtc_supervisor (one_for_one)
|     |
|     +-- listener_proc          accepts; spawns per-conn procs
|     |
|     +-- expire_proc            timer-driven TTL expiry
|     |
|     +-- metrics_proc           periodic xtc_log of res stats
|     |
|     +-- N x conn_proc          one xtc_proc per accepted client;
|         (BEAM-style)           reads RESP cmds, dispatches, writes resp
|
+------------------------+
        |
        v
+------------------------+
| xtc_lrlock-protected   |  Hash table (FNV-1a) keyed by string;
|       database         |  values are tagged unions (string/list/hash);
|                        |  reads are wait-free, writes coalesce via apply_op
+------------------------+
```

xtc primitives used:

* **xtc_loop / xtc_io_poll** -- event loop and TCP accept/read/write
* **xtc_net_listen** -- listening socket with the standard knobs
* **xtc_proc** -- one BEAM-style process per accepted connection
* **xtc_lrlock** -- wait-free reads on the database
* **xtc_slab** -- per-DB-table memory allocator (read/write copies of bucket arrays)
* **xtc_res** -- the resource accountant; the centerpiece
* **xtc_log** -- structured operational logging
* **xtc_cfg** -- runtime configuration knobs
* **xtc_app + xtc_supervisor** -- clean startup/shutdown
* **xtc_inject** -- fault-injection hooks (unused in steady state, present for tests)

## Files

| File | Lines | Purpose |
|---|---|---|
| `main.c` | 486 | Entry point, arg parsing, app bringup |
| `proto.c` | 387 | RESP2/RESP3 parser + response builder |
| `proto.h` | 149 | Protocol type definitions |
| `db.c` | 1,048 | Database (FNV-1a hash table + lrlock + slab) |
| `db.h` | 209 | Database API |
| `cmd.c` | 741 | Command dispatch + 35 handlers |
| `cmd.h` | 64 | Command type definitions |
| `conn.c` | 296 | Per-connection xtc_proc |
| `conn.h` | 48 | Connection API |
| `expire.c` | 64 | TTL expiration timer |
| `metrics.c` | 77 | Periodic res-stats logging |
| `Makefile` | 60 | Build glue |

Total: ~3,600 LOC of source.

## Tests

In `test/m99/`:

| Test | Cases | Coverage |
|---|---|---|
| `test_resp_parser.c` | 33 | RESP2/RESP3 framing, malformed input |
| `test_rexis_loopback.c` | 12 | End-to-end command round-trips |
| `test_rexis_budgets.c` | 5 | All five `--max-*` caps under stress |
| `test_rexis_pbt.c` | 5 | hegel-c properties (atomicity, idempotence, FIFO) |

Run via `cd test/m99 && make check`.  Loopback and budget tests
spawn a real `rexis-server-xtc` binary.

## Bench

In `bench/rexis_compat/`:

```sh
cd bench/rexis_compat
make
./bench --host=127.0.0.1 --port=16379 --pipeline=1 --requests=100000
```

Reports ops/sec and p50/p99/p999 in the M17 conformance format.  When
real `redis-server` is on `$PATH`, a side-by-side comparison can be run.

## Gaps in xtc surfaced by this example

Things this example needed that xtc doesn't provide cleanly today.
Each is an opportunity for a new xtc primitive:

1. **No built-in hash map / dict primitive.**  We rolled our own
   FNV-1a bucket hash in `db.c` (~150 LOC).  Many xtc-based apps will
   want this; consider an `xtc_dict` in M19.

2. **No token-bucket rate limiter.**  The IOPS cap is implemented
   inline with atomics; ~30 LOC.  Not an immediate priority but
   common enough to deserve `xtc_ratelimit`.

3. **No glob pattern matcher.**  The `KEYS pattern` command needed
   one; ~40 LOC inline.  Consider an `xtc_glob` helper.

4. **No `xtc_proc` enumeration.**  We track `conn_count` with a
   manual atomic.  An `xtc_proc_iterate` callback would let
   `INFO clients` walk the live connection set.

5. **No core-pinning API in `xtc_exec`.**  We call `sched_setaffinity(2)`
   directly.  An `xtc_exec_pin(exec, cpu_mask)` would be portable
   (cpuset on FreeBSD, processor_bind on illumos).

6. **No non-blocking `xtc_recv` variant.**  We use `xtc_recv` with a
   short timeout to poll, which is wasteful.  An `xtc_try_recv`
   that returns immediately would be cleaner for opportunistic
   draining.

7. **No timer inside `xtc_proc`.**  We use `xtc_recv` with a timeout
   to implement periodic work.  An `xtc_proc_after(proc, ns, fn)`
   would be more idiomatic.

8. **No async-fd readiness in `xtc_proc`.**  This is the biggest
   gap.  The listener and per-conn procs busy-poll: they accept
   non-blocking, then sleep briefly, then try again.  A primitive
   like `xtc_proc_wait_fd(fd, XTC_IO_READABLE, timeout)` that
   integrates the loop's `xtc_io_poll` readiness with `xtc_recv`
   semantics would let connections wake exactly when there's
   data to read.  Without it, a 10000-client server with mostly-idle
   connections still wakes each connection every 50ms, which costs
   real CPU at scale.  This is the highest-leverage missing piece.

## Known limitations

* Plaintext RESP only.  No TLS yet (TLS-Redis is a separate exercise;
  see `docs/M_TLS.md` and the existing `xtc_tls` API for the building
  blocks).
* No persistence.  All data is in RAM; restart loses everything.
  AOF + RDB would add another ~500 LOC.
* `KEYS pattern` is O(n) like real Redis -- documented, not a bug.
* `INFO` output is hand-rolled and approximate; real Redis has
  hundreds of metrics; we expose ~30.
* `CLUSTER NODES` returns a single-node response unconditionally.

## Comparison vs real `redis-server`

Honest microbenchmarks on a single client, no pipelining, on this
host (Linux x86_64, AMD EPYC):

| Metric | redis-server (7.x) | rexis-server-xtc |
|---|---|---|
| GET ops/sec | ~115,000 | ~85,000 |
| SET ops/sec | ~110,000 | ~78,000 |
| GET p99 latency | 12 us | 18 us |

xtc is ~25% slower in steady-state, primarily due to the busy-poll
in the per-connection proc (gap #8 above).  Once `xtc_proc_wait_fd`
lands and the conn loop sleeps on real fd readiness, the gap should
close.  We are NOT claiming faster than real Redis here; the value
is in the **bounded behaviour under stress** (all 5 caps hold; real
Redis lets you OOM the host on `maxmemory` overrun via fragmentation).
