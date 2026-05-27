# M16 — PostgreSQL adapter (design sketch)

**Status:** research; not yet implemented.  This document defines the
contract between xtc and a future threaded PostgreSQL backend, the
phased path to land it, and the integration risks.

## Goal

Replace key parts of PostgreSQL's process-model substrate with xtc
primitives so that one PG instance can run **multiple backends in a
single process**, while preserving the single-backend-per-thread
mental model that PG code is written against.

This unlocks:

- Cheaper backends (no fork; ~1k overhead vs ~10MB for a fork).
- Faster context switching (fiber yield vs kernel thread).
- Better cache locality (shared-memory becomes cheaper).
- A path toward Tokio/Seastar-style sharding within Postgres.

## What gets replaced

| PG subsystem | xtc primitive | Migration |
|---|---|---|
| `Backend = fork()` | `xtc_proc` (BEAM-style proc) | M16.1 |
| `MyLatch` (signal-based) | `xtc_notify` | M16.1 |
| `WaitEventSet` | `xtc_io_poll` events | M16.2 |
| `MemoryContext` (palloc/pfree) | `xtc_mctx` | M16.3 |
| `LWLock` | `xtc_lwlock` (already ported) | M16.3 |
| `LRLock` (lrlck branch) | `xtc_lrlock` (already ported) | M16.3 |
| `LockManager` (heavyweight) | `xtc_lockmgr` (already ported) | M16.3 |
| `proc_exit/CFI signal handling` | `xtc_exit_*` + supervisors | M16.4 |
| `aio` (async I/O) | `xtc_io` + io_uring backend | M16.4 |
| `GUC` system | `xtc_cfg` | M16.5 |
| Tracing / wait events | `xtc_log` + `xtc_inject` | M16.5 |

## Phasing

### M16.1 — backend-as-proc bringup
Smallest unit of progress: convert `BackendStartup` to spawn an
`xtc_proc` instead of `fork()`.  PG's main loop runs as the proc's
entry function.  `MyLatch` becomes a `xtc_notify`.

**Risk:** PG's `MyLatch` is tightly intertwined with the signal
infrastructure (SIGUSR1, SIGURG); we need to translate the existing
"set the latch from a signal handler" idiom into "send a notify
from anywhere".  Deferred-signal handling already exists in PG's
`miscadmin.h`; we wrap it.

**Deliverable:** an example `pg_minimal_backend.c` in `examples/`
that talks to a PostgreSQL master and runs one query.

### M16.2 — async I/O integration
PG 16+ ships `src/backend/storage/aio` with Andres's reactor design.
Replace its internals with `xtc_io` (backend-pluggable: epoll on
Linux, io_uring opt-in, kqueue on BSDs).  The PG-facing API stays:
`pgaio_io_*`, `pgaio_wait_one`, etc.

**Risk:** PG's aio queue model assumes thread-affinity that we'll
break by dispatching across a multi-loop executor.  Mitigation:
each PG backend is pinned to one `xtc_loop` for the life of the
backend (matches the historical fork-per-backend model).

### M16.3 — memory + locks
PG's `MemoryContext` API maps cleanly onto `xtc_mctx`:

- `MemoryContextCreate` → `xtc_mctx_create_child`
- `palloc(sz)` → `xtc_mctx_alloc(curr, sz)`
- `pfree(p)` → `xtc_mctx_free(p)`
- `MemoryContextDelete(ctx)` → `xtc_mctx_destroy(ctx)`

The lock primitives map directly (the M13 ports were always aimed at
this slot).

**Risk:** PG's "the current memory context" thread-locality.  Today
each backend has one current context; under threads we have to make
it `__thread` and push/pop on `xtc_proc` switch.  This is the
migration's biggest invasive change — touches every `palloc()` call
site implicitly via the `CurrentMemoryContext` macro.

### M16.4 — process exit + supervision
PG's `proc_exit` chains atexit-registered callbacks.  Map to
`xtc_proc`'s exit-handler chain.  `xtc_supervisor` takes the role of
the postmaster: it owns child specs (each PG backend), restart
strategy is `XTC_RESTART_TRANSIENT` (PG keeps a backend alive until
it normally exits or crashes; on crash we don't restart).

### M16.5 — observability + config
- `GUC_*` macros wrap `xtc_cfg_register_*` calls.
- `pg_stat_*` views read from `xtc_cfg`/`xtc_log`.
- Wait events → `xtc_log` events tagged with the wait class.

## What we don't replace (deliberately)

- **WAL writer / checkpointer / autovacuum** — these are still
  separate processes, not threads.  They communicate with backends
  via the shared-memory queues (which xtc doesn't own).  Keeping
  them on the PG side avoids an enormous patch surface.

- **Shared memory** — PG owns DSM (`dynamic_shared_memory`); xtc's
  `xtc_slab` shared-memory mode uses the same `mmap` primitives but
  without the segment-tracking hash.  M16 just teaches PG how to
  call `xtc_slab_create_ex` on top of an already-allocated DSM
  region.

- **Plan execution / parser** — these are pure CPU code; they don't
  touch xtc.

## Compatibility surface

xtc must commit to a stable C ABI for the public symbols listed in
PLAN.md §18.  M16 freezes the M13 lock API (already done) and adds
no new public symbols beyond a small `pg_xtc_glue.h` header that
lives in the PG tree, not in xtc's.

## Testing

A new `test/m16/` directory will hold integration tests:

- `test_backend_smoke.c` — spawn one `xtc_proc` "backend" that talks
  to a real PG instance.
- `test_lwlock_pg_parity.c` — exercise xtc_lwlock with PG's
  test_lwlock workload (replay the lrlck test_lwlock test cases via
  xtc).
- `bench_threads_vs_forks.c` — compare backend-startup latency.

## Open questions

1. **Signal handling under threads**: PG sends SIGUSR1 to a backend;
   under threads, how do we route it?  Per-thread signal masks +
   sigwait in a coordinator thread is the classic answer.  Need a
   spike to validate.

2. **`exit(2)` semantics**: a thread crashing kills the process.
   Mitigation: catch SIGSEGV in a per-thread handler and convert to
   `xtc_exit_self("crashed", ...)` which a supervisor can observe.
   Some CFI patterns may not survive this.

3. **`fork()` for `pg_dump` etc.**: external utilities still fork
   the postmaster.  No change needed.

## Effort estimate

Rough order of work:

| Phase | Lines | Weeks |
|---|---|---|
| M16.1 backend-as-proc | ~600 | 2 |
| M16.2 aio integration | ~1200 | 4 |
| M16.3 mctx + locks | ~800 | 3 |
| M16.4 exit + supervision | ~400 | 2 |
| M16.5 cfg + observability | ~600 | 2 |

Total: ~13 person-weeks for a working "1 process, N backends"
prototype.  This is research-level; production hardening is at
least another quarter beyond.

## Next concrete step (when you give the green light)

Spike M16.1 in `~/ws/postgres/` on a fresh branch:

1. Add `pg_xtc_glue.h/.c` that translates `MyLatch` to `xtc_notify`.
2. Replace one (1) call site in `BackendStartup` with `xtc_proc_spawn`.
3. Verify `psql -c "select 1"` round-trips.

Estimated: 2 days for the smoke test, 2 weeks for the full M16.1
landing including tests.
