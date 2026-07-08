---
title: PostgreSQL adapter (design)
parent: Reference
nav_order: 20
lede: >-
  How a threaded PostgreSQL would sit on the libxtc runtime.
permalink: /reference/pg-adapter/
---
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

## M16.1b spike outcome (2026-07): seam PROVEN, one fd-plumbing bug from a round-trip

An out-of-tree spike ran real PostgreSQL (a throwaway xtc-m16-1b-spike
branch of a PG master checkout; full recipe + M16_1B_FINDINGS.md live on
that branch, NOT in this repo).  Result: the runtime seam WORKS -- a
real backend runs as an xtc_proc fiber -- blocked on one concrete
fd-plumbing bug, not on the deferred shared-address-space wall.

What the spike proved:
  - libxtc.a links into the PG backend (wired into src/Makefile.global
    with -I.../src/inc, -DUSE_XTC_BACKENDS, and libxtc's -luring/-lssl/
    -lcrypto deps).
  - postmaster_child_launch routes B_BACKEND to xtc_pg_spawn_backend
    (an xtc scheduler hosted in the postmaster on a dedicated pthread)
    instead of fork_process().
  - The backend's REAL BackendMain -> PostgresMain -> InitPostgres ->
    StartupXLOG executes inside the fiber, reaches "connection received:
    host=[local]", and YIELDS cooperatively to the xtc loop (the loop
    idles in io_uring_wait, not spinning) -- the pg_latch glue
    (SetLatch->xtc_send, WaitLatchOrSocket->xtc_proc_wait_fd) fires.
  - Three real seam bugs found + fixed along the way: a NULL LatchWaitSet
    crash in StartupXLOG (run only the safe subset of InitPostmasterChild
    in the fiber); a wrong-event scan (match only WL_SOCKET_* events, not
    the latch self-pipe fd); a busy-spin on pure-latch waits (park on the
    latch signalfd instead of xtc_proc_wait_fd(-1,...) which returns
    E_INVAL).

The remaining blocker (fstat-proven, one bug): the fiber parks forever
in xtc_proc_wait_fd at the startup-packet read.  A dup() workaround
(added so the postmaster's closesocket(s.sock) in ServerLoop does not
kill the backend's socket) made the READ fd (the dup, fd 12) diverge
from the WAIT fd registered in the WaitEventSet (fd 3); fd 3 never
becomes readable -> hang -> psql timeout.  Fix (in the findings file):
drop the dup() and instead gate the postmaster's closesocket on
child_type != B_BACKEND under USE_XTC_BACKENDS, so read and wait share
the one true fd; then wire SetLatch -> fiber wakeup.  That should give
the real "select 1".

Assessment: M16.1b is ~one fd-fix from a first real round-trip -- the
concept (a PG backend as an xtc_proc, cooperatively scheduled) is
validated.  The DEEPER walls remain deferred and correctly so: single
backend at a time only (on_shmem_exit sharing, PG process-globals),
untested sigsetjmp/PG_exception_stack interaction with the fiber stack,
and signal delivery to the scheduler thread -- these are the M16.3b
(__thread-ize globals) and later work.  The spike de-risked exactly
what it should: the runtime seam is real; the hard part is PG's globals,
as the plan predicted.

## M16 MILESTONE (2026-07): threaded PostgreSQL runs on the xtc scheduler

SUCCESS.  `psql -c "select 1"` round-trips with a real PostgreSQL backend
running as an xtc_proc fiber whose client-socket waits are driven by
xtc_proc_wait_fd on the xtc scheduler loop.  This is threaded PostgreSQL
on xtc -- the M16 concept, working end to end.

STRATEGY that unlocked it: the earlier fork-replacement spike (M16.1b,
on /home/gburd/ws/postgres/master branch xtc-m16-1b-spike) proved the
runtime seam but hit PostgreSQL's process-per-backend shared-address-
space / global-state wall (deferred to M16.3b) plus one fd-divergence
bug.  Rather than __thread-ize PG's globals ourselves, we START from an
ALREADY-THREADED PostgreSQL -- Sam Willis's multithreaded-postgres
(github.com/samwillis/multithreaded-postgres, REL_19_BETA1 + thread-per-
session backends + a carrier-thread pool) -- which has already done that
work, and wire the xtc scheduler in as the CARRIER for its threaded
backends.  The globals wall is gone because the tree is already threaded;
we change only the carrier.

Location: /home/gburd/src/multithreaded-postgres, branch xtc-carrier
(out-of-tree, throwaway; M16_XTC_CARRIER_FINDINGS.md + the recipe live
there, NOT in this repo).  ~404 LOC over the multithreaded baseline,
gated on USE_XTC_CARRIER:
  - src/backend/postmaster/pg_xtc_carrier.c/.h (new) -- a single-loop
    xtc_app on a dedicated pthread (xtc_pg_carrier_start); launches a
    B_BACKEND as an xtc_proc fiber (backend_thread_entry) on the carrier
    loop; a __thread xtc_in_backend_fiber flag marks the carrier path.
  - launch_backend.c -- routes B_BACKEND to the xtc carrier.
  - waiteventset.c -- a backend fiber's WaitEventSetWait socket waits go
    through xtc_proc_wait_fd (park on the xtc loop) instead of blocking
    the carrier in epoll_wait.
  - the pg_xtc_glue Latch<->xtc mailbox/wait_fd mapping ported from the
    prior spike / libxtc examples/09_pgmock/pg_latch.*.

Proof (postmaster log): "xtc: carrier scheduler thread up", "B_BACKEND
launched as xtc fiber (child_slot=1)", repeated "fiber wait_fd fd=34
interest=0xd timeout_ms=-1 (via xtc_proc_wait_fd)"; psql returns the row,
rc=0.

Significance: a PostgreSQL backend is now cooperatively scheduled by
xtc's fiber runtime -- the foundation for the threaded-PG vision (many
backends multiplexed on a small carrier pool by the xtc scheduler,
sharing the data plane).  Next: multiple concurrent backends on the
carrier pool; wire SetLatch cross-backend wakeups fully; then the larger
integration (xtc_aio for backend I/O, xtc_proc supervision of backends).
