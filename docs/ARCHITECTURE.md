---
title: "Architecture"
---

# Architecture

This document is the short reference for the xtc layering.  The
authoritative long-form design is in [`../PLAN.md`](../PLAN.md);
read that for rationale, alternatives considered, and open
questions.  This document is what you read when you want a
five-minute orientation.

## The six layers

```
+---------------------------------------------------------------------+
| L5  pg/   PostgreSQL adapter (subsumes src/backend/storage/aio,     |
|           latch, signal/CFI, MemoryContext, GUC bridge)             |
+---------------------------------------------------------------------+
| L4  orc/  Orchestration: supervisors, xtc_svr (gen_server),         |
|           xtc_fsm (gen_statem), xtc_app, xtc_reg                    |
+---------------------------------------------------------------------+
| L3  ptc/  Processes / Threads / Channels: PIDs, mailboxes (with     |
|           selective receive), links, monitors, channels, futures,   |
|           sync primitives (incl. RCU, LRLock, LWLock, lock mgr),    |
|           dispatch()/reply(), async()/await(), xtc_yield(),         |
|           xtc_log, xtc_cfg, observability, blocking-call contract,  |
|           hooks framework                                           |
+---------------------------------------------------------------------+
| L2  evt/  Event loop: per-thread reactor, run queues, work-         |
|           stealing deque, task lifecycle, wakers, timer wheel,      |
|           coroutine substrate (fiber + Duff's-device)               |
+---------------------------------------------------------------------+
| L1  io/   Pollable I/O abstraction: epoll/kqueue/IOCP/io_uring/     |
|           poll wrapper, async file/socket/timer registration        |
+---------------------------------------------------------------------+
| L0  os/   __os_*: threads, processes, shm, mmap, mutex, atomics,    |
|           time, file ops, signals, errno -> xtc_err mapping,        |
|           allocator hook, TLS, CPU/NUMA topology, dynamic loading,  |
|           rng, locale, env, dlerror, getopt_long                    |
+---------------------------------------------------------------------+
```

Lower layers know nothing of upper layers.  Each layer has its own
internal header, its own subdirectory under `src/`, and its own test
binary under `test/`.

## Reading order for new contributors

1. [`../PLAN.md`](../PLAN.md) (S)0 -- guiding principles.
2. [`../PLAN.md`](../PLAN.md) (S)2 -- the six layers in detail.
3. [`../PLAN.md`](../PLAN.md) (S)14 -- the worked SQL-query example.
4. [`abi-stability.md`](abi-stability.md) -- the longevity contract.
5. The current milestone's `M*_CLAIMS.md`.

## Key design choices (with links into the plan)

- **Shared-nothing reactors by default; cross-loop only by explicit
  channel/mailbox/shared-buffer handle.**  See PLAN.md (S)0.2, (S)2.3.
- **Configure-time backend selection, never runtime.**  No vtable
  on the hot path.  See PLAN.md (S)0.7, (S)2.2.
- **C11 dialect.**  PG18 raised the floor; PG19 inherits.  See
  PLAN.md (S)0.9.
- **Graceful degradation to single-thread + `poll(2)` + Duff's-
  device protothreads** when the platform is too constrained for
  fibers and async I/O.  See PLAN.md (S)3.6.
- **BDB/DBSQL conventions throughout** -- `__os_*`, `__xtc_*`,
  `xtc_*`, `XTC_E_*`, `PUBLIC:` markers, `dist/s_*` generators,
  out-of-source build enforced.  See PLAN.md (S)8.
- **Test-first, claim-driven.**  Every claim in code or
  documentation has a test.  See `M*_CLAIMS.md` per milestone.
- **Mechanical change as doctrine.**  Structural change goes
  through `dist/s_*` generators.  See PLAN.md (S)18.3.

## Module status

| Layer | Status |
|---|---|
| L0 `os/` | **M1 complete** for the core six modules (alloc, atomic, time, thread, tls, mutex/rwlock/cond/sem) + M5.5 NUMA topology probe + Windows-aware `_aligned_*` and `GetSystemInfo`. Remaining modules (signals, dynamic loading, network, files, dirs, shm, proc, cpu-extras, rng, env, locale, errno, getopt) land in M1.5+. |
| L1 `io/` | **M2 + M6 complete** -- `xtc_io` with poll/epoll/io_uring/kqueue tested green on Linux+FreeBSD; **illumos `port_*` event ports complete (132/132 tests pass)**; **Windows IOCP round-1 with `PostQueuedCompletionStatus` wakeup + `WSAEventSelect` readiness emulation (30/35 tests pass)**; AIX `pollset_*` stub awaiting host. |
| L2 `evt/` | **M3 single-thread + M4 coroutines + M4.5 fcontext asm + M5 multi-loop + M5.5 NUMA-aware steal complete + Windows fiber substrate (`coro_winfiber.c`)** -- `xtc_loop` driving the run queue, min-heap timer, task / waker / park-on-{timer,fd}; stackful fibers (ucontext on POSIX, Win32 fibers on Windows); `async()`/`await()`/`xtc_yield()`, `XTC_COOP_REGION` marker, header-only protothreads; `xtc_exec` multi-loop executor with Chase-Lev work-stealing deque, MPSC inbox, cross-thread wakers; per-arch fcontext asm at ~7 ns/swap. |
| L3 `ptc/` | **M7 + M7.5 channels (oneshot/mpsc/mpmc/watch/broadcast) + xtc_res governance + M8 processes/mailboxes/links/monitors/`xtc_exit_pid` + M9 + M9.5 sync + M11 xtc_mctx + M13a xtc_rcu + M13b xtc_lrlock + M13c xtc_lockmgr (full 9-mode BDB-parity: configurable matrix, lock-vec, upgrade/downgrade, 8 victim policies, per-locker timeouts, statistics, failchk, slab pool)**. |
| L4 `orc/` | **M10 supervisor (4 strategies + restart intensity) + M10.5 xtc_reg (process registry) + xtc_svr (gen_server) + xtc_app (root sup + lifecycle)** complete. |
| L5 `pg/` | not started (M16). |

M1 lands the building blocks that everything else depends on:
8 cycle-counted hot-path atomic primitives (load/store/CAS/fetch_add
for four widths plus pointer), a hookable allocator with the BDB
out-parameter convention, monotonic + wall clock, threads / TLS /
mutex / rwlock / cond / sem.  The full surface is documented in
`man3/__os_*.3` and asserted by the M1 test suite
.Pq see Pa M1_CLAIMS.md .
