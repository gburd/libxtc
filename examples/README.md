# xtc examples

Self-contained programs demonstrating each major xtc feature.  Build
each via:

```
cd build_unix
make examples
./01_hello       # or ./02_pingpong, etc.
```

| File | Demonstrates |
|---|---|
| `01_hello_async.c` | Minimal coroutine: `xtc_async`, `xtc_yield`, `xtc_await`. ~30 LOC. |
| `02_proc_pingpong.c` | Erlang-style processes: `xtc_proc_spawn`, `xtc_send`, `xtc_recv`. Bounce a counter 100 rounds. |
| `03_supervised_app.c` | OTP application: `xtc_app` with a root supervisor (`one_for_all`), two workers, restart-on-crash. |
| `04_lockmgr_demo.c` | Heavyweight lock manager: deadlock between two transactions; detector aborts the youngest. |
| `05_rexis/` | **Redis-compatible server** with hard resource budgets.  Drop-in for redis-cli. |
| `06_sqlxtc/` | **Networked SQLite** with the Quack JSON protocol.  Multi-client; uses xtc throughout. |
| `07_kaka/` | **Kafka-shaped log broker** (Phase 0 scaffold): partitioned append-only logs with credit-based backpressure.  See its README for the design. |

## What each example proves

**01** -- the core async/await contract works end-to-end with a real
loop, single fiber, single yield.

**02** -- message passing across processes; the messaging API is
sufficient to build request/reply RPC patterns; sender pid is encoded
in the payload by user-space (no implicit reply-to).

**03** -- the M10/M10.5 supervisor stack composes: app owns loop +
registry + root sup; sup owns children with restart policy; an
external watcher proc can request orderly shutdown via `xtc_app_stop`.

**04** -- the M13c lock manager detects real deadlocks
(circular wait), aborts a victim per policy, and surfaces stats.

**05** -- the flagship demonstration: a Redis-compatible server
implementing RESP2/RESP3 protocol with hard resource budgets
enforced via `xtc_res`. Demonstrates the full xtc API surface:
`xtc_loop`, `xtc_proc`, `xtc_lrlock`, `xtc_slab`, `xtc_res`,
`xtc_log`, `xtc_cfg`, `xtc_app`, `xtc_supervisor`, and `xtc_inject`.
Supports ~35 Redis commands including strings, lists, and hashes.

## Building

The Makefile target `examples` is built by:

```
make 01_hello 02_pingpong 03_supervised_app 04_lockmgr_demo
cd 05_rexis && make
```

Each binary links statically against `libxtc.a` and pthread.
