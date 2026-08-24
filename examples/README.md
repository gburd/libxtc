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
| `06_sqlxtc/` | **A from-scratch SQL engine on libxtc** (Lime parser + vectorized executor + B-link/buffer-pool/WAL storage), served over the Quack JSON protocol.  Multi-client; uses xtc throughout. |
| `07_kaka/` | **Kafka-shaped log broker** (Phase 0 scaffold): partitioned append-only logs with credit-based backpressure.  See its README for the design. |
| `08_tnt/` | **The canonical echo demo for libxtc's Tina-faithful Isolate layer** (now a SUPPORTED library API: `src/orc/tnt.c`, `<xtc_tnt.h>`, `man xtc_tnt`).  Thread-per-core, shared-nothing, stackless state machines that return transitions; generational handles, drop-on-full mailboxes, stage-then-commit I/O.  TCP echo server.  See man xtc_tnt(3). |
| `09_pgmock/` | **Mock PostgreSQL backend on the xtc scheduler** (M16.1a): a postmaster proc accepts and spawns one backend xtc_proc per connection, each speaking a hand-rolled minimal PG v3 wire handshake + `SELECT 1` -- ZERO PostgreSQL source.  Proves the runtime seam (no-fork multiplexing, `WaitLatchOrSocket` -> `xtc_proc_wait_fd`) for the future PG adapter.  See its README and the docs Examples page. |
| `10_circuit_breaker.c` | **The circuit-breaker pattern as an `xtc_fsm` (gen_statem)**: CLOSED/OPEN/HALF_OPEN, tripping after N consecutive failures, failing fast while open, and half-opening after a cooldown *state timeout*.  Shows `state_enter`, state timeouts, and synchronous `xtc_fsm_call`/`xtc_fsm_reply` -- the three things hand-rolled breakers get wrong. |
| `11_lorb/` | **A limit order book / matching engine** ported from C++ to C on libxtc: price-time (FIFO) priority, partial fills, and Market/Limit/Stop/Stop-Limit orders (add/cancel/modify).  AVL price-level trees + intrusive FIFO order lists, with `xtc_chash` for the id/price maps and `xtc_slab` for the order/level pools.  Ships a generate-and-process benchmark (throughput + p50/p99/p999) and a ported unit-test suite.  ~2.6M orders/s locally.  See its README. |

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

**10** -- the L4 gen_statem (`xtc_fsm`) is enough to express a real
protective control machine.  The circuit breaker leans on all three
gen_statem features: the OPEN cooldown is a *state timeout* (armed in
one place), the CLOSED/OPEN/HALF_OPEN transitions each run `enter()`,
and "may I proceed?" is a synchronous `xtc_fsm_call` answered by
`xtc_fsm_reply`.  Note the subtlety the example calls out: a real event
cancels a pending state timeout, so a fail-fast query while OPEN must
re-arm the cooldown -- exactly the gen_statem trap.

**05** -- the flagship demonstration: a Redis-compatible server
implementing RESP2/RESP3 protocol with hard resource budgets
enforced via `xtc_res`. Demonstrates the full xtc API surface:
`xtc_loop`, `xtc_proc`, `xtc_lrlock`, `xtc_slab`, `xtc_res`,
`xtc_log`, `xtc_cfg`, `xtc_app`, `xtc_supervisor`, and `xtc_inject`.
Supports ~35 Redis commands including strings, lists, and hashes.

**11** -- a matching engine is a realistic non-actor workload for the
map/pool primitives: `xtc_chash` as the order/price index (with the
required `xtc_rcu` read-side bracket on every lookup) and `xtc_slab`
as the order/level pools.  It also shows the discipline of porting a
reference C++ data structure faithfully while fixing its latent
AVL-delete and book-edge bugs, proven by an invariant fuzzer (30 seeds
x 2M ops) and clean ASan/UBSan runs where the stock C++ original
crashes.

## Building

The Makefile target `examples` is built by:

```
make 01_hello 02_pingpong 03_supervised_app 04_lockmgr_demo 10_circuit_breaker
cd 05_rexis && make
```

Each binary links statically against `libxtc.a` and pthread.
