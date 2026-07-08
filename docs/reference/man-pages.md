---
title: Manual pages
parent: Reference
nav_order: 1
permalink: /reference/man-pages/
lede: >-
  Every public function has a manual page -- coverage is a release gate.
---
Every public function ships a manual page (`man xtc_loop`, etc.).
Coverage is enforced by a release gate: a public `xtc_*` symbol without
a man page fails the build. The pages below link to their source in the
repository; after `make install` they are on your system as
`man 3 xtc_loop` and friends.

## Section 7 -- overview

- [`xtc(7)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man7/xtc.7) -- the library, top to bottom.
- [`xtc-build(7)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man7/xtc-build.7) -- build and configure options.
- [`xtc-abi-stability(7)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man7/xtc-abi-stability.7) -- what stays fixed across releases.

## Section 3 -- the public API

Grouped roughly by layer (event runtime, processes, synchronization,
orchestration, I/O, storage, observability):
- [`xtc_aio(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_aio.3) -- async file I/O for fibers
- [`xtc_alloc_audit(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_alloc_audit.3) -- debug allocation auditor with per-process leak detection
- [`xtc_app(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_app.3) -- OTP-style application container
- [`xtc_async(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_async.3) -- L2 stackful coroutines and protothreads
- [`xtc_bdev(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_bdev.3) -- portable block-device I/O over async fibers
- [`xtc_blocking(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_blocking.3) -- offload blocking work to a thread pool without stalling the loop
- [`xtc_cfg(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_cfg.3) -- typed runtime configuration registry
- [`xtc_chan(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_chan.3) -- typed channels for cross-task message passing
- [`xtc_dio_sched(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_dio_sched.3) -- adaptive genetic-algorithm tuner for runtime self-tuning
- [`xtc_dump(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_dump.3) -- crash diagnostics: runtime-state dump, panic, fatal-signal handler
- [`xtc_exec(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_exec.3) -- L2 multi-loop work-stealing executor
- [`xtc_free(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_free.3) -- public allocation, clock, sleep, and atomic helpers
- [`xtc_fs(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_fs.3) -- portable synchronous filesystem helpers
- [`xtc_inject(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_inject.3) -- fault-injection points
- [`xtc_inspect(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_inspect.3) -- live process and loop introspection
- [`xtc_io(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_io.3) -- L1 event-notification engine
- [`xtc_iosched(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_iosched.3) -- adaptive write-batching scheduler for the async file path
- [`xtc_launch(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_launch.3) -- run a function on a fiber with a precise one-shot deadline
- [`xtc_lockmgr(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_lockmgr.3) -- transactional lock manager with deadlock detection
- [`xtc_log(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_log.3) -- async ring-buffer logger
- [`xtc_loop(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_loop.3) -- L2 single-thread event loop, tasks, wakers, timers
- [`xtc_lrlock(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_lrlock.3) -- left-right lock with wait-free reads
- [`xtc_lwlock(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_lwlock.3) -- lightweight lock with shared / exclusive modes
- [`xtc_mctx(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_mctx.3) -- hierarchical memory contexts with cleanup callbacks
- [`xtc_net(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_net.3) -- networking helpers
- [`xtc_osproc(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_osproc.3) -- OS-process spawn, control socket, and lifecycle
- [`xtc_pdict(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_pdict.3) -- Erlang-style per-process dictionary
- [`xtc_pkey(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_pkey.3) -- memory-protection-key (PKU) tier for in-process isolation
- [`xtc_preempt(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_preempt.3) -- per-worker preemption timer seam
- [`xtc_proc(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_proc.3) -- BEAM-style lightweight processes with mailboxes
- [`xtc_rcu(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_rcu.3) -- epoch-based read-copy-update
- [`xtc_reg(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_reg.3) -- named process registry
- [`xtc_res(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_res.3) -- resource accountant with hard caps and high-water alerts
- [`xtc_slab(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_slab.3) -- slab + magazine allocator
- [`xtc_stack_reclaim(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_stack_reclaim.3) -- reclaim a parked fiber's unused stack memory
- [`xtc_stats(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_stats.3) -- runtime statistics: counters, gauges, histograms
- [`xtc_strerror(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_strerror.3) -- return a stable English description of an xtc error code
- [`xtc_supervisor(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_supervisor.3) -- OTP-style supervisor trees
- [`xtc_svr(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_svr.3) -- gen_server-style request/reply server abstraction
- [`xtc_sync(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_sync.3) -- async synchronisation primitives
- [`xtc_tls(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_tls.3) -- TLS over async sockets
- [`xtc_tnt(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_tnt.3) -- Tina-faithful stackless Isolate layer
- [`xtc_trace(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_trace.3) -- causal message tracing (libxtc's seq_trace)
- [`xtc_version_components(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_version_components.3) -- decompose the xtc library version into integer components
- [`xtc_version_string(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_version_string.3) -- return the xtc library version as a SemVer string
