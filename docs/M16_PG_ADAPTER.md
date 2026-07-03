# M16 -- PostgreSQL adapter (design sketch)

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

### M16.1 -- backend-as-proc bringup
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

### M16.2 -- async I/O integration
PG 16+ ships `src/backend/storage/aio` with Andres's reactor design.
Replace its internals with `xtc_io` (backend-pluggable: epoll on
Linux, io_uring opt-in, kqueue on BSDs).  The PG-facing API stays:
`pgaio_io_*`, `pgaio_wait_one`, etc.

**Risk:** PG's aio queue model assumes thread-affinity that we'll
break by dispatching across a multi-loop executor.  Mitigation:
each PG backend is pinned to one `xtc_loop` for the life of the
backend (matches the historical fork-per-backend model).

### M16.3 -- memory + locks
PG's `MemoryContext` API maps cleanly onto `xtc_mctx`:

- `MemoryContextCreate` -> `xtc_mctx_create_child`
- `palloc(sz)` -> `xtc_mctx_alloc(curr, sz)`
- `pfree(p)` -> `xtc_mctx_free(p)`
- `MemoryContextDelete(ctx)` -> `xtc_mctx_destroy(ctx)`

The lock primitives map directly (the M13 ports were always aimed at
this slot).

**Risk:** PG's "the current memory context" thread-locality.  Today
each backend has one current context; under threads we have to make
it `__thread` and push/pop on `xtc_proc` switch.  This is the
migration's biggest invasive change -- touches every `palloc()` call
site implicitly via the `CurrentMemoryContext` macro.

### M16.4 -- process exit + supervision
PG's `proc_exit` chains atexit-registered callbacks.  Map to
`xtc_proc`'s exit-handler chain.  `xtc_supervisor` takes the role of
the postmaster: it owns child specs (each PG backend), restart
strategy is `XTC_RESTART_TRANSIENT` (PG keeps a backend alive until
it normally exits or crashes; on crash we don't restart).

### M16.5 -- observability + config
- `GUC_*` macros wrap `xtc_cfg_register_*` calls.
- `pg_stat_*` views read from `xtc_cfg`/`xtc_log`.
- Wait events -> `xtc_log` events tagged with the wait class.

## What we don't replace (deliberately)

- **WAL writer / checkpointer / autovacuum** -- these are still
  separate processes, not threads.  They communicate with backends
  via the shared-memory queues (which xtc doesn't own).  Keeping
  them on the PG side avoids an enormous patch surface.

- **Shared memory** -- PG owns DSM (`dynamic_shared_memory`); xtc's
  `xtc_slab` shared-memory mode uses the same `mmap` primitives but
  without the segment-tracking hash.  M16 just teaches PG how to
  call `xtc_slab_create_ex` on top of an already-allocated DSM
  region.

- **Plan execution / parser** -- these are pure CPU code; they don't
  touch xtc.

## Compatibility surface

xtc must commit to a stable C ABI for the public symbols listed in
PLAN.md (S)18.  M16 freezes the M13 lock API (already done) and adds
no new public symbols beyond a small `pg_xtc_glue.h` header that
lives in the PG tree, not in xtc's.

## Testing

A new `test/m16/` directory will hold integration tests:

- `test_backend_smoke.c` -- spawn one `xtc_proc` "backend" that talks
  to a real PG instance.
- `test_lwlock_pg_parity.c` -- exercise xtc_lwlock with PG's
  test_lwlock workload (replay the lrlck test_lwlock test cases via
  xtc).
- `bench_threads_vs_forks.c` -- compare backend-startup latency.

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

1. Add `pg_xtc_glue.h/.c` that translates `MyLatch` to the proc
   mailbox + `xtc_proc_wait_fd` (NOT `xtc_notify`, which does not
   exist -- see the M16.1 implementation plan below).
2. Replace one (1) call site in `BackendStartup` with `xtc_proc_spawn`.
3. Verify `psql -c "select 1"` round-trips.

Estimated: 2 days for the smoke test, 2 weeks for the full M16.1
landing including tests.

## M16.1 implementation plan (concrete, 2026-07)

The "Spike" above front-loads the single biggest risk (PG global state
under a shared address space, plus real PG-source integration) into
step one, and it maps the latch onto APIs that have since drifted.
Before any code is written, correct the API drift, then split M16.1
into an in-tree concept proof and an out-of-tree PG spike.

### API-drift corrections (verified against the current headers)

| Design says | Reality | Fix |
| --- | --- | --- |
| MyLatch -> xtc_notify | xtc_notify does NOT exist | Latch = the proc mailbox (xtc_send / xtc_recv) plus xtc_proc_wait_fd(fd, interest, timeout, &revents), which waits on fd-readiness OR mailbox OR timeout in one call -- the exact shape of PG's WaitLatchOrSocket.  SetLatch(pid) -> xtc_send(pid, &wake, 1). |
| WaitEventSet -> xtc_io_poll events | xtc_io_poll is the low-level loop poller, not a per-backend wait | Backend waits use xtc_proc_wait_fd (returns XTC_IO_* bits plus XTC_WAIT_MAILBOX / XTC_WAIT_TIMEOUT). |
| xtc_supervisor | No xtc_supervisor.h; it is xtc_orc.h | Use xtc_sup_start, xtc_child_spec_t, XTC_RESTART_TRANSIENT. |
| xtc_slab_create_ex on a DSM region | Actual symbol is xtc_slab_create; shm via xtc_slab_opts_t.shm_base/shm_size | Rename; the DSM-region story holds via opts. |
| xtc_mctx_create_child | Actual is xtc_mctx_create(parent, name, flags, &out); child = non-NULL parent | Rename in the M16.3 map. |
| xtc_exit_* chain | Actual: xtc_proc_at_exit, xtc_exit_self, xtc_proc_recovery_* (R1, DONE) | M16.4 rides existing R1 machinery, not a new API. |

Everything else in the map (xtc_lwlock, xtc_lrlock, xtc_lockmgr,
xtc_cfg_*, xtc_aio_*, xtc_log, xtc_mctx_*) is present and correct.

### The split

- M16.1a -- mock backend (in-tree, ZERO PG source).  A standalone
  examples/09_pgmock/ that proves only the RUNTIME seam: a postmaster
  proc accepts on a socket, spawns one backend per connection as an
  xtc_proc, and the backend runs a hand-rolled minimal PG v3 wire
  handshake plus a SELECT 1 echo, driven entirely by the xtc
  scheduler.  No PG source, no globals problem.  Structurally cloned
  from examples/06_sqlxtc (listener proc -> xtc_proc_spawn per conn ->
  xtc_proc_wait_fd + framed I/O in the conn proc).  This is the
  concept proof and the CI-gated deliverable.
- M16.1b -- real PG bringup (out-of-tree spike, in a PG branch, NOT in
  this repo).  Only after 09_pgmock is green: replace ONE call site in
  BackendStartup with xtc_proc_spawn, back MyLatch with the
  mailbox + xtc_proc_wait_fd glue proven in 09_pgmock, and round-trip
  psql -c "select 1" with ONE backend at a time (N-concurrent, which
  exposes PG's globals, is explicitly deferred to M16.3b).

### M16.1a files

- examples/09_pgmock/main.c -- xtc_app + xtc_net_listen + a listener
  proc that xtc_proc_spawns a backend proc per connection.
- examples/09_pgmock/backend.c -- the backend xtc_proc_fn: PG v3
  StartupMessage -> AuthenticationOk + ReadyForQuery; Query("select
  1") -> RowDescription + DataRow("1") + CommandComplete +
  ReadyForQuery; Terminate -> exit.  ~200 LOC over raw recv/send +
  xtc_proc_wait_fd (PG v3 framing is 1 type byte + 4-byte length,
  NOT xtc_net_recv_frame's pure 4-byte frame, so hand-roll it).
- examples/09_pgmock/pg_latch.c/.h -- the reusable Latch-shaped glue
  (SetLatch -> xtc_send; WaitLatchOrSocket -> xtc_proc_wait_fd) that
  transfers verbatim to M16.1b.
- examples/09_pgmock/Makefile + .gitignore (build artifacts never
  committed) + README, cloned from examples/06_sqlxtc.
- test/m16/test_pgmock_smoke.c -- raw-socket client (no libpq dep, so
  CI needs no PG) that drives the StartupMessage + Query and asserts
  the DataRow bytes; asserts two concurrent connections get two
  distinct pids (no-fork multiplexing); asserts clean exit + no
  leaked fds/mctx (ASan clean).

### Hard risks (each sidestepped or confined in M16.1)

1. Shared address space (PG assumes one address space per backend):
   16.1a has no PG globals; 16.1b runs ONE backend at a time.  Full
   __thread-ization is M16.3b.
2. PG global/static state (MyProc, MyLatch, CurrentMemoryContext, ...):
   16.1 confronts only MyLatch (reachable from the proc user-data,
   not a bare global); the rest stays single-backend-at-a-time.
3. Signals under threads: the mock uses zero signals (latch wake is
   in-address-space xtc_send).  16.1b converts a signal-set-flag to
   xtc_send via a per-loop coordinator.
4. longjmp error handling (PG_TRY / elog(ERROR)): the mock has no
   elog; the runtime already provides containment via
   xtc_proc_recovery_* (R1, DONE), wired to PG's PG_exception_stack in
   M16.4.
5. PG v3 wire framing != xtc's 4-byte length frame: hand-rolled in
   backend.c (PG-specific, correctly outside libxtc per the boundary
   doc).

### M16.1 acceptance

test/m16/test_pgmock_smoke.c passes under make check and the ASan/UBSan
CI jobs: PG v3 handshake completes; SELECT 1 returns a DataRow of "1";
two concurrent connections are served by distinct procs on one loop;
Terminate exits cleanly with no leaks.  16.1b exit criterion (spike):
real psql -c "select 1" round-trips against a PG built with
USE_XTC_BACKENDS, backend ran as an xtc_proc (no fork in strace),
one backend at a time.

### Effort re-scope

M16.1a ~450 LOC / ~1 wk (in-tree, CI-gated).  M16.1b ~250 LOC glue +
1 PG call site / ~1.5 wk (out-of-tree).  M16.2 split 16.2a (wrap
pgaio over xtc_aio, no io_uring swap) / 16.2b (io_uring method).
M16.3 split 16.3a (MemoryContext shim over xtc_mctx) / 16.3b
(CurrentMemoryContext -> __thread + N-concurrent backends -- the real
shared-address-space reckoning, the largest chunk).  M16.4 rides R1 +
xtc_sup_start.  M16.5 xtc_cfg + xtc_log, both present.
