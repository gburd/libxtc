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
| Pub/Sub | SUBSCRIBE, UNSUBSCRIBE, PUBLISH |
| Connection | PING, ECHO, QUIT, AUTH, SELECT |
| Admin | INFO, COMMAND, CLUSTER NODES |

Both RESP2 (Redis 1.x-5.x) and RESP3 (Redis 6+) protocols are accepted,
as are inline commands (`PING\r\n`, `SET k v\r\n` -- what telnet and
`nc` send; words are split on spaces/tabs and not unquoted).

Pub/Sub is built on libxtc process groups (`xtc_pg`, over the
duplicate-key registry `xtc_reg_register_dup`): SUBSCRIBE joins the
connection's pid to a channel group, PUBLISH fans a RESP push frame out
to every subscriber's mailbox (which the connection proc appends to its
socket), and a closing connection calls `xtc_reg_drop_pid` to leave every
channel it joined.  See the pubsub walkthrough in the docs example page.

## What's NOT supported (deliberately)

Out of scope for a 2k-LOC example.  No Streams, no scripting
(EVAL/Lua), no cluster mode, no modules, no replication, no
Redis-compatible persistence formats (no AOF, no RDB -- rexis has its
own append-only bitcask store instead, see `--persist=DIR` below), no
transactions (MULTI/EXEC), no Sorted Sets (ZADD/ZRANGE).
A full Redis would add another 30k LOC.  Pub/Sub keyspace notifications
and pattern subscriptions (PSUBSCRIBE) are also out of scope.

## Resource budget enforcement

This is the centerpiece for the P99 conference demo.  The caps below
hold under stress:

| Flag | What it bounds | How |
|---|---|---|
| `--cores=N` | Which CPUs the process may run on | `sched_setaffinity(2)` restricts it to CPUs 0..N-1.  It pins; it does not add threads (that is `--loops`) |
| `--loops=N` | Event loops (OS threads) | Connections go to the least-loaded of N `xtc_exec` loops.  One loop holds at most 65,520 clients: a pid has a 16-bit per-loop slot, so a loop runs at most 65,536 procs, and rexis keeps 16 for itself.  100k clients need `--loops=2`.  With N > 1 the keyspace is shared by N threads and every command takes a parking reader/writer latch (`xtc_arwlock`); with 1 it takes none |
| `--max-memory=N` | Keyspace + client buffers | One `xtc_res` `XTC_RES_MEM_BYTES` budget charged by every keyspace allocation (including the bucket array) AND by every client query/output buffer's growth above a fixed 16 KiB each.  A write that would pass it gets `-OOM`; a client whose buffer would pass it gets `-OOM` and is closed.  `INFO` `used_memory` is the budget's total (`used_memory_dataset` + `used_memory_clients`).  Not covered: the fixed 32 KiB of buffers per connection (bounded by `--max-clients`), proc stacks, allocator overhead.  0 = no cap |
| `--max-keys=N` | DB key count | Per-DB counter; SET/HSET/LPUSH refused above cap |
| `--max-clients=N` | Concurrent connections (default 10000; 0 = all the loops hold) | Listener checks the live count before spawning a per-conn proc.  An over-limit client gets `-ERR max number of clients reached` (Redis's reply) and a close, as does one that finds every loop full (`--loop-clients=N` lowers the per-loop cap; INFO shows `connected_clients`, `rejected_connections`, `loopN_clients`).  A cap above `--loops` x 65,520 is refused at startup with the `--loops` value that fits |
| `--max-fds=N` | Sockets, listener included (default: `--max-clients` + 64) | The `xtc_res` `XTC_RES_FDS` budget, attached with `xtc_res_attach_net`, so `xtc_net_accept` refuses BEFORE accepting: a client past it waits in the kernel backlog and is served when a socket closes.  The soft `RLIMIT_NOFILE` is raised to fit |
| `--max-iops=N` | Commands/sec | Token bucket refilled every second; excess commands are DELAYED (held in the connection's buffer until tokens return), not rejected |
| `--max-request=N` | Largest command (default 64 MiB) | Redis `proto-max-bulk-len`: a bulk whose header announces more is refused at once (`-ERR Protocol error: ...`) and the connection closed; the query buffer grows only to hold one command |
| `--max-output=N` | Unsent reply bytes per client (default 128 MiB) | Redis `client-output-buffer-limit`: a reply that would pass it is replaced WHOLE by `-ERR reply exceeds the client output limit` (never truncated); a pub/sub subscriber that falls that far behind is disconnected |
| `--timeout=SECS` | Idle connections (default 0 = never) | Redis `timeout`: no byte in either direction for SECS closes the connection; subscribers are exempt |

Backpressure: once more than 1 MiB of a client's replies are unsent, its
connection stops reading and running commands until the client drains
them, so a client that pipelines without reading is slowed by TCP flow
control rather than growing the server.  Malformed input is answered
with an error and the connection is closed (the rest of the stream
cannot be trusted).  Under fd exhaustion (EMFILE) the listener backs off
10 ms per failed accept instead of spinning, and recovers when fds free.

Verified by MEASUREMENT in `test/m99/test_rexis_budgets.c`: server RSS
growth while offered 4x the memory cap and while 100 clients each hold a
990 KB partial command, the server's CPU affinity mask, connections
actually served, the reply rate of a pipelined burst (<= 1.5x
`--max-iops`), server CPU while a client holds an oversized partial
bulk and under an fd flood, and the idle timeout.  Run with
`make check-rexis` in a build dir.

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
| xtc_lrlock-protected   |  Linear-hash table (FNV-1a, lhash.h) keyed by
|       database         |  string; grows one bucket per insert; values
|                        |  are tagged unions (string/list/hash)
+------------------------+
```

xtc primitives used:

* **xtc_loop / xtc_io_poll** -- event loop and TCP accept/read/write
* **xtc_net_listen** -- listening socket with the standard knobs
* **xtc_proc** -- one BEAM-style process per accepted connection
* **xtc_lrlock** -- wait-free reads on the database
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
| `db.c` | 1,183 | Database (linear-hash table + lrlock) |
| `lhash.h` | 166 | Linear hashing: the growing table under db.c and the bitcask index |
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
| `test_resp_parser.c` | 39 | RESP2/RESP3 framing, inline commands, malformed input, integer overflow |
| `test_rexis_loopback.c` | 17 | End-to-end round-trips, replies larger than any buffer, slow readers, uncapped LRANGE/KEYS/HGETALL |
| `test_rexis_budgets.c` | 11 | Every `--max-*` cap, measured; request/output limits, client-buffer memory, idle timeout, EMFILE; clean SIGTERM |
| `test_rexis_pbt.c` | 5 | properties (atomicity, idempotence, FIFO) |

Run via `make check-rexis` in a libxtc build dir (CI runs it in the
examples job).  Loopback, budget and PBT tests spawn a real
`rexis-server-xtc` on a kernel-chosen free port (see
`test/m99/rexis_harness.h`); every test also requires the server to exit
0 on SIGTERM.

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
Each was an opportunity for a new xtc primitive.  **Four of the eight
originally listed here have since shipped** -- the entries are kept, and
marked, because the list doubles as a record of how consumer pressure
shaped the library:

1. ~~**No built-in hash map / dict primitive.**~~  **SHIPPED.**  Both
   `xtc_chash` (concurrent, RCU-protected, `<xtc_chash.h>`) and
   `xtc_pdict` (per-proc dictionary, `<xtc_pdict.h>`) exist now.  rexis
   still uses its own table (`lhash.h`, linear hashing over intrusive
   chains): one loop owns the keyspace, so it needs no locking or RCU,
   and it grows one bucket at a time with no whole-table rehash.  Until
   2026-10 it was a FIXED 65,536-bucket array that never resized -- at 1M
   keys that cost 40% of its pipelined throughput, at 5M a 4.4x slower
   load.  A consumer that shares a table across loops should reach for
   `xtc_chash`.

2. **No token-bucket rate limiter.**  Still true.  The IOPS cap is
   implemented inline with atomics (~30 LOC).  `xtc_credit`
   (`<xtc_credit.h>`) covers the related *windowed* flow-control case --
   see `examples/07_kaka/broker.c` -- but not a time-based token bucket.

3. **No glob pattern matcher.**  Still true.  `KEYS pattern` needed one;
   ~40 LOC inline.

4. **No `xtc_proc` enumeration.**  Partly addressed: `xtc_inspect`
   (`<xtc_inspect.h>`) can enumerate live procs for observability.  rexis
   still tracks `conn_count` with a manual atomic.

5. **No core-pinning API in `xtc_exec`.**  Still true for pinning
   itself, but the *topology* half now ships: `xtc_ncpus()`,
   `xtc_numa_nnodes()`, `xtc_numa_node_of_cpu()`,
   `xtc_numa_current_node()`.

6. ~~**No non-blocking `xtc_recv` variant.**~~  **SHIPPED (it always
   existed -- this entry was wrong).**  `xtc_recv(&m, &sz, 0)` returns
   immediately; a `0` timeout is documented as non-blocking in
   `<xtc_proc.h>`.  rexis itself uses it in `conn.c`.

7. ~~**No timer inside `xtc_proc`.**~~  **SHIPPED.**  `xtc_proc_sleep()`
   parks the fiber (without blocking the OS thread) for a bounded time;
   a recv timeout also remains a fine way to do periodic work.

8. ~~**No async-fd readiness in `xtc_proc`** -- once "the biggest gap".~~
   **SHIPPED, and rexis now uses it.**  `xtc_proc_wait_fd(fd, interest,
   timeout_ns, &revents)` is exactly the primitive this entry asked for
   by name, and the listener + per-connection procs park on it
   (`conn.c`, `main.c`) instead of busy-polling.  The benchmark numbers
   below predate that change and should be re-measured.

## Known limitations

* Plaintext RESP only.  No TLS yet (TLS-Redis is a separate exercise;
  see `docs/M_TLS.md` and the existing `xtc_tls` API for the building
  blocks).
* Persistence is a log-structured **bitcask** store (`bitcask.c`), not
  Redis's AOF/RDB formats: run with `--persist=DIR` and the keyspace is
  durable across restart (`db.c` replays the log at startup; covered by
  `test/m99/test_rexis_persist.sh`).  Without `--persist` rexis is
  RAM-only.  Note the store's file I/O is currently synchronous -- see
  the note in `bitcask.h` about why, and `examples/06_sqlxtc/wal.c` for
  the `xtc_aio_*` alternative.
* `KEYS pattern` is O(n) like real Redis -- documented, not a bug.
* `--loops=N` spreads CONNECTIONS, not the keyspace.  The lrlock guards
  only the table header; both copies share the buckets and entries, and
  writers free entries in place, so with N > 1 every command also takes
  one `xtc_arwlock` (readers share it, writers serialize).  That buys
  connection capacity (65,520 clients per loop) and parallel parsing and
  I/O, not parallel writes.  The scalable shape is DragonFly's: one
  keyspace shard per loop, each command run on (or sent to) the loop that
  owns its key -- see `docs/examples/rexis-vs-dragonfly.md`.
* An idle connection costs ~15.7 KB of RSS (its fiber stack and two
  16 KiB base buffers; Valkey: 0.9 KB) and wakes once a second to check
  shutdown and the idle clock.  Measured with 100,000 idle connections on
  `--loops=2`: 0.53 of a core with no traffic, and 21% less memtier
  throughput (Valkey: 6%).
* A client that pipelines an unbounded stream and never reads stalls
  once the socket buffers fill (backpressure, above); Redis instead
  buffers replies without limit for normal clients by default.
* `INFO` output is hand-rolled and approximate; real Redis has
  hundreds of metrics; we expose ~30.
* `CLUSTER NODES` returns a single-node response unconditionally.

## Comparison vs Valkey and DragonFly

Measured 2026-10 on a 64-vCPU x86-64 host (32 cores x 2 SMT, one NUMA
node), loopback, memtier_benchmark GET:SET 10:1, 32 B values, 1M keys
prefilled, one server thread each, client on separate cores, medians
of 3 runs (method and the open-loop tails in
`docs/examples/rexis-vs-dragonfly.md`):

| | Valkey 9.0 | DragonFly (1 thread) | rexis-server-xtc |
|---|---:|---:|---:|
| ops/s, no pipelining | 219,000 | 195,000 | 153,000 |
| ops/s, pipeline 16 | 1,262,000 | 707,000 | 1,130,000 |
| open-loop p99 at 100k ops/s | 34 us | 34 us | 34 us |
| load 5M keys (pipelined SET) | 6.0 s | 7.0 s | 5.0 s |

Without pipelining rexis pays for ~7 syscalls per request (a one-shot
poll re-armed per park, and two `rt_sigprocmask` per fiber switch from
glibc's `swapcontext`) against Valkey's 2.  DragonFly's strength is the
other axis: it shards the keyspace over threads and scales to 2.7M
ops/s (8.4M pipelined) at 32 threads, where Valkey peaks near 1.1M and
rexis cannot use a second core.

We are NOT claiming faster than real Redis here; the value is in the
**bounded behaviour under stress** (all 5 caps hold; real Redis lets you
OOM the host on `maxmemory` overrun via fragmentation).
