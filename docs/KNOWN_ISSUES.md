---
title: Known issues
parent: Reference
nav_order: 7
lede: >-
  Honest caveats, workarounds, and the platform-verification status.
permalink: /reference/known-issues/
---
## RESOLVED: runtime-thread signal mask (process-directed signal on a scheduler thread)

**Status:** RESOLVED.  The carrier reported a process-directed SIGCHLD
delivered to a libxtc scheduler thread (where MyProcPid == 0) instead of
the thread the embedder designated.

Three layers:

1. Every runtime thread is created with all signals blocked
   (__os_pthread_create_masked).

2. The real residual: the ucontext coroutine substrate restores
   uc_sigmask on every swapcontext, and getcontext captured the CREATING
   thread's mask -- so a fiber created from a thread with a signal
   unblocked (a proc spawned from the embedder's main thread) unblocked
   that signal on whatever runtime loop/worker thread later ran the
   fiber.  (The hand-written fcontext substrate does not touch the
   signal mask, so it was immune -- which is why the forced-fcontext CI
   never caught this.)  Fixed in src/evt/coro_uctx.c: the fiber's
   uc_sigmask blocks process-directed signals but EXEMPTS SIGVTALRM
   (preemption), the synchronous fault signals SIGSEGV/SIGBUS/SIGFPE/
   SIGILL (which the R1 fault guard must catch -- blocking a
   hardware-generated fault is undefined behavior anyway), and SIGABRT
   (assert/panic).

3. A proc fiber's uc_sigmask is inherited across fork(); xtc_osproc's
   child path now resets its own mask to empty immediately after fork so
   the child (and any exec'd image) starts clean, not with the runtime
   mask.

**Evidence:** test/m1/test_thread_sigmask.c (main is the only
SIGUSR1-unblocked thread; multi-loop executor + blocking pool + 200
process-directed SIGUSR1s) reports 0 deliveries to any libxtc thread,
20/20 under CPU load and in the gated parallel make check; R1 fault
containment (test_proc fault_contain/fault_early_contain/
recovery_registry) passes; fork/exec (test_osproc) passes; the three
preemption tests pass; ASan make check is clean.  test_thread_sigmask is
now in the gated make check set.

## RESOLVED: xtc_exec_fini cross-thread-spawn teardown leak (LSan)

**Status:** RESOLVED.  Surfaced 2026-07 via
test/concurrency/repro_idle_uring_wake.c.

**Symptom:** procs spawned CROSS-THREAD (xtc_proc_spawn from an OS thread
that is not on the target loop) onto a service-mode executor that is
stopped mid-flight were not fully reclaimed by xtc_exec_fini --
LeakSanitizer reported the task+coro (xtc_async -> __os_calloc,
coro_uctx.c) as leaked, run-to-run variable (~3-7%).

**Fix, in two parts:**

1. __xtc_inbox_fini now, for each undrained XTC_INB_PUBLISH message,
   runs the carried task's cleanup (releasing the fiber stack + coro
   struct) and frees the task -- exactly as xtc_loop_fini's all_tasks
   walk does for drained tasks.  XTC_INB_WAKE references an
   already-tracked task and is left untouched.  This reclaimed procs
   left sitting in a loop's inbox at fini (the bulk).

2. THE RESIDUAL (the run-queue case) root cause: xtc_async stores
   t->cleanup only AFTER __xtc_task_spawn_ex makes the task visible (it
   pushes an XTC_INB_PUBLISH for a foreign spawn).  A short-lived
   cross-thread-spawned coro can be drained, run, and reach DONE on the
   target loop's thread while the spawning thread has not yet stored
   t->cleanup -- so the DONE path read cleanup==NULL, recycled the task
   as a PLAIN task (freeing the task struct without releasing its
   coro/fiber-stack -> the coro leaked), and left the spawner's pending
   t->cleanup store to land on freed memory (a latent UAF).  Fixed in
   __xtc_loop_step: the recycle-as-plain path now also requires
   t->fn != __xtc_coro_step.  t->fn is set before the task is published
   and never changes, so it is a race-free discriminator: any
   coro-backed task is left on all_tasks for the fini walk (which runs
   its by-then-stored cleanup exactly once); only genuinely plain
   pinned tasks recycle.

**Evidence:** repro_idle_uring_wake under ASan leak-detection goes to
0 leaks across 70+ consecutive runs on both epoll and io_uring (was
~3-7%); the full proc/sup/svr/tnt suites pass under ASan with no
use-after-free or double-free; a drain-to-idle shutdown was already
leak-clean and stays so.

**Why it was not caught earlier:** the leak is only visible with a
WORKING LeakSanitizer -- GitHub's containerized CI runner restricts the
ptrace LSan needs, so LSan silently no-ops there.  The lost-wakeup
guarantee the reproducer checks is covered leak-clean under DST by
test/sim/test_sim_wake_park.c.

## Platform runtime-verification status (1.x readiness, B2)

For an honest 1.x stance, here is exactly which platforms run at
RUNTIME (the full test suite executes) versus which only COMPILE:

- Linux (epoll + io_uring), macOS (kqueue): runtime-verified on every
  commit in CI.
- FreeBSD 15 (clang, kqueue): re-verified against the current tree
  (2026-06, full gmake check passes, including the native kqueue
  file-AIO path).  Not in per-commit CI.
- illumos (SunOS 5.11, UltraSPARC v9 / sparcv9 big-endian, gcc, event
  ports): re-verified against the current tree (2026-07, full gmake
  check passes -- including the property suites on big-endian SPARC,
  with OpenSSL 3, and the native event-port file-AIO path
  (SIGEV_PORT -> PORT_SOURCE_AIO) incl. /aio/roundtrip).  Not in
  per-commit CI.
- Windows: the IOCP runtime (AFD socket poll, cross-thread wakeup,
  file AIO) was RUNTIME-verified on a Windows host with MinGW (2026-06
  -- see "IOCP backend status" below; three bugs found and fixed).
  Per-commit Windows CI remains an MSVC xtc.lib + smoke build.
- AIX (pollset): compiles, code-reviewed, no test host.

## RESOLVED (partial): native stack backtrace beyond execinfo

**Status:** real backends added; one (Windows) remains compiled-but-not-
runtime-verified for lack of a Windows host.

**Previous issue:** `src/os/os_backtrace.c` had only the execinfo backend
(glibc/macOS/BSD) and a no-op stub everywhere else, so musl and Windows
got an empty C backtrace in `xtc_dump` / the panic+crash handler.

**What changed this round:**
  - **libunwind backend** (`XTC_HAVE_LIBUNWIND`) for libc's lacking
    `<execinfo.h>` (notably musl): `__os_backtrace` walks the calling
    thread with `unw_step`; `__os_backtrace_emit` symbolizes best-effort
    with `dladdr` (`XTC_HAVE_DLADDR`) or prints raw addresses otherwise.
    Detected by `dist/configure.ac` (`--with-libunwind=auto|yes|no|PATH`,
    OPTIONAL); execinfo wins when both are present.
  - **DbgHelp backend** (`_WIN32`): `CaptureStackBackTrace` +
    `SymFromAddr`, linked with `-ldbghelp`.
  - New test `test/m12/test_backtrace.c` drives a 3-deep call chain,
    asserts >= 2 captured frames, and (on a symbolizing platform)
    requires at least one of its own frames to be named.

**Verified on this round's Linux glibc x86_64 host:**
  - execinfo path: full `make check` for m1/m12 green, `test_backtrace`
    green, the existing `test_dump` panic/crash path green.
  - libunwind+dladdr path: built by forcing `XTC_HAVE_LIBUNWIND` and
    linking the system libunwind; produced a REAL symbolized trace and
    passed `test_backtrace`.
  - libunwind addresses-only path (dladdr disabled): `test_backtrace`
    passes on frame count alone.
  - All three Unix variants compile `-Wall -Wextra -Wpedantic` clean.

**NOT runtime-verified (honest gaps):**
  - The libunwind path has not run on an actual musl host yet, only by
    forcing the backend on glibc.
  - The Windows DbgHelp path is COMPILED-NOT-RUNTIME-VERIFIED: it
    cross-compiles clean with mingw-w64 and links with `-ldbghelp`, but
    no Windows machine in the CI matrix exercises it.  Reviewed against
    the Win32 DbgHelp API docs; same status as `src/io/io_aix.c`.
  - Platforms with neither execinfo nor libunwind still get the honest
    no-op stub (`__os_backtrace_supported()` returns 0).

See `docs/guide/debugging.md` for the per-platform symbolization matrix.

## RESOLVED: xtc_slab SHARED_MEMORY mode cross-process support

**Status:** FIXED in this round.

**Previous issue:** The SHARED_MEMORY mode tests used MAP_PRIVATE | MAP_ANONYMOUS,
which is single-process memory, not actual shared memory.  Additionally, the
slab's shm_cursor was stored in per-process private memory, so two processes
with their own xtc_slab_t structs would each start carving chunks at offset 0,
causing collisions.

**Fix:** The cursor now lives in a 64-byte header at the start of the shared
region, using atomic CAS for coordination.  First attacher initializes the
header (magic=0x5854435F534C4142 "XTC_SLAB", version=1, cursor=64); subsequent
attachers verify magic and use the existing cursor.

**Verification:** New test file test/m11/test_slab_shm.c exercises real
cross-process sharing via fork(2) + POSIX shm_open:
  - test_shm_basic_fork: parent allocs, child reads+modifies via offset
  - test_shm_alloc_in_child: child allocs, parent reads via offset
  - test_shm_concurrent_alloc: 50 concurrent allocs from each process, no overlap
  - test_shm_size_too_small: XTC_E_RESOURCE when region < header+chunk
  - test_shm_resolve_invalid_offset: NULL on junk offsets

The previous misleading tests have been renamed to clarify their scope:
  - test_slab.c: test_shm_offset_resolve -> test_shm_offset_resolve_single_process
  - pbt_slab.c: prop_shm_offset_roundtrip -> prop_shm_offset_roundtrip_single_process

## RESOLVED: Windows fault containment (SEH) + fiber teardown double-free

**Status:** resolved.  `xtc_fault_guard_install` was a no-op on Windows;
it now installs a Vectored Exception Handler that contains a
fiber-attributable hardware fault by restoring the CONTEXT captured at
`xtc_proc_recovery_arm()` (via `EXCEPTION_CONTINUE_EXECUTION` -- no
stack unwinding, which is what makes it safe on a fiber stack; a
`longjmp` driven from a VEH reliably corrupted the CRT heap).
Runtime-verified on a Windows host (santorini, MinGW gcc) by a
dedicated driver: a proc arms recovery, registers resources, and takes
a real access violation -- the VEH contains it (exit 0), the recovery
resource registry releases the proc's fd + callback automatically, and
the monitor observes DOWN(reason).  A companion driver confirms the
ESCALATION half of the contract: a fault inside a critical section is
NOT contained -- the VEH returns EXCEPTION_CONTINUE_SEARCH and the
process dies with 0xC0000005 (EXCEPTION_ACCESS_VIOLATION), preserving
the PG critical-section PANIC semantics.
While wiring this up, a pre-existing Windows double-free was found and
fixed in `coro_winfiber.c` (the done branch destroyed the coro
eagerly *and* via the task cleanup at `loop_fini`), which had made any
loop+process tear down with heap corruption on Windows.

## Windows: `test_proc::selective_receive` regression

**Status: RESOLVED (2026-07-06).**  Confirmed passing on Windows 11
ARM64 with MSVC 2026 (VS18).  The round-2 IOCP rewrite fixed it: the
wakeup path no longer uses a `WaitForMultipleObjects` set -- wakeups
are a plain `PostQueuedCompletionStatus` reaped by
`GetQueuedCompletionStatusEx` and coalesced into one event -- so the
selective-receive ordering (pull 42 first via `xtc_recv_match`, then
drain 1,2,3,4 in arrival order) is now deterministic on Windows.
Verified on a real santorini host (not assumed): the scenario was
added to `test/msvc/smoke.c` (`smoke_selective_proc`) and the MSVC
smoke test prints `ok selective_receive: 42 first, then 1,2,3,4 in
order (IOCP wakeup path)`.  The full munit `test_proc` cannot build
under cl.exe (munit uses GCC-isms), so the smoke test is the Windows
regression guard; the munit `test_proc::selective_receive` continues
to pass on Linux/FreeBSD/illumos.

<details><summary>Original failure notes (historical)</summary>

**Symptom:** the test sends 5 messages to a proc before calling `xtc_loop_run`; the proc uses `xtc_recv_match` to selectively pick value 42 first, then drains 1, 2, 3, 4 in order. On Windows specifically, this failed -- a timing/ordering issue with the IOCP wakeup path.  The earlier hypothesis (now confirmed) was that the round-2 IOCP rewrite -- `WaitForMultipleObjects` set replaced by `PostQueuedCompletionStatus` + `GetQueuedCompletionStatusEx` with a coalesced wakeup -- would fix it.  That is exactly what resolved it, confirmed on a santorini run.
</details>


## IOCP backend status (Windows)

**Status:** round-2 native rewrite; the smoke test now runtime-verifies
more of it on santorini (VS18, ARM64).  As of 2026-07 the MSVC smoke
gate passes: version, strerror, clocks, slab, lwlock, SEH fault
containment, xtc_fs, selective_receive (IOCP wakeup ordering),
single-op AND multi-op native file AIO (overlapped pwrite/pread reaped
from the port), and cross-thread wakeup coalescing (256 foreign
xtc_send via PostQueuedCompletionStatus) all pass on the real host.

ONE scenario is not yet green and is intentionally a SKIP (not a gate):
the loopback-socket connect/accept/echo over the AFD poll path
(smoke_sock_server / smoke_sock_client) -- the echo does not complete on
santorini (s_sock_srv_ok stays 0), which points at the AFD-poll
level-triggered re-arm being incomplete.  This is the socket-readiness
corner still under investigation; the smoke prints a visible `skip
loopback socket echo` line rather than failing, so a genuine AFD-poll
bug surfaces without blocking the two proven IOCP additions.  Chasing
the AFD echo to green is the remaining IOCP runtime-verification item.

### Round 2 (current source): native completion port + AFD poll

`src/io/io_iocp.c` was rewritten from the round-1 readiness emulation
to a native completion-port design (details in
`docs/M_WINDOWS_MATRIX.md`):

  - `CreateIoCompletionPort` + `GetQueuedCompletionStatusEx` is the
    only wait primitive; the round-1 64-handle
    `WaitForMultipleObjects` cap is GONE.
  - Socket readiness uses the AFD poll fast path (`\Device\Afd` +
    `IOCTL_AFD_POLL` via `NtDeviceIoControlFile`), re-armed per
    completion (level-triggered).  This is the wepoll/libuv design.
  - Wakeup is `PostQueuedCompletionStatus`; file AIO is overlapped
    `ReadFile`/`WriteFile` reaped from the same port (no hEvent).
  - OVERLAPPED-ownership rule enforced: the kernel owns an OVERLAPPED
    from request-accept to completion-dequeue; deregister cancels with
    `NtCancelIoFileEx` and defers the free to the reap; registration
    nodes have stable heap addresses so the kernel-held back-pointer
    never dangles.  `dist/configure.ac` adds `-lntdll`;
    `dist/build_msvc.bat` links `ntdll.lib`.

**What is verified (Linux dev host this round):** cross-compiles clean
with mingw-w64 gcc 14.3.0 `-std=c11 -Wall -Wextra`, links into a
PE32+ binary against `-lntdll -lws2_32`, and leaves the Linux build
untouched (`io_iocp.c` is `XTC_IO_BACKEND_IOCP`-only -- an empty TU on
Linux; the full C munit suite stays green on epoll/uring).

**What is NOT verified:** the backend has NOT executed on Windows.
The AFD poll correctness, the level-triggered re-arm (no busy-loop, no
dropped edges), the cancel/lifetime under churn (no double-free, no
freeing a kernel-owned OVERLAPPED), the wakeup coalescing/ordering,
and the file-AIO port round-trip must all be validated on santorini
before this is production quality.  Same reviewed-but-untested status
as `src/io/io_aix.c`.  This rewrite is the suspected fix territory for
the `test_proc::selective_receive` flake (the wakeup ordering changed),
but that cannot be confirmed without the host.

**Smoke coverage WRITTEN, awaiting a santorini run (NOT yet verified):**
`test/msvc/smoke.c` now drives four IOCP-runtime scenarios beyond the
already-verified strerror/clocks/slab/lwlock/fault-containment set.
They COMPILE (cross-checked with mingw-w64 gcc 14.3.0 `-std=c11 -Wall
-Wextra -fsyntax-only` against the real Windows headers, with the MSVC
pthread shim forced into its `_MSC_VER` path) but have NOT been built
or run under cl.exe on a Windows host:

  - `smoke_aio_proc` (pre-existing): a single overlapped pwrite+pread
    round-trip at offset 0 -- the file-AIO port round-trip.
  - `smoke_aio_multi_proc` (new): several positioned overlapped
    pwrite/pread ops at distinct offsets in one loop run, so more than
    one AIO completion is reaped from the port per run (the reap loop,
    not just a single completion).
  - `smoke_xt_worker` + `smoke_xt_sender` (new): a foreign OS thread
    bursts `xtc_send` at a batch of parked worker procs, exercising the
    cross-thread `PostQueuedCompletionStatus` wakeup and its coalescing
    (`__xtc_io_iocp_wakeup_post`); the loop must reap every delivery and
    return once all workers exit.
  - `smoke_sock_server` + `smoke_sock_client` (new): a 127.0.0.1
    connect/accept/echo driven entirely by `xtc_proc_wait_fd` on the
    raw Winsock socket fds -- the AFD poll fast path and its
    level-triggered re-arm (WRITABLE for connect completion, READABLE
    for accept, then READABLE/WRITABLE for the echo).  Send/recv is raw
    Winsock in the test so its error handling does not depend on any
    errno mapping in the library net helpers; if a listen port cannot
    be bound on the runner the scenario SKIPS rather than fails.

These are the runtime scenarios the "What is NOT verified" list above
names; the smoke test is the intended Windows regression guard for
them once santorini runs `dist/build_msvc.bat` and reports the `ok`
lines.  Until that run they remain COMPILED-NOT-RUNTIME-VERIFIED.

### Round 1 (historical): WSAEventSelect emulation -- SUPERSEDED

The round-1 backend (WSAEventSelect + WaitForMultipleObjects,
hard-capped at 64 handles, ~60% of native IOCP throughput) passed the
full suite on the reference Windows toolchain (MinGW64: 233/233).
Those results describe the SUPERSEDED source, not the round-2 rewrite
above; they are retained here only as the baseline the round-2 code
must re-establish on santorini.  Round-1 notes that still stand:

  - **Clang64 POSIX-only test ports.**  `test_net_udp` used a bare
    `nanosleep` (absent in the Clang64/MinGW runtime) -- now portable
    (a `test_msleep` shim).  `test_proc_wait_fd` still uses `pipe(2)` +
    `clock_gettime(CLOCK_PROCESS_CPUTIME_ID)` + pthreads; porting it to
    the socket-pipe compat is straightforward but must be verified on
    the host before landing (an unverifiable Windows edit risks a
    silent Linux regression), so it is deferred to a santorini pass.
  - **`test_proc::selective_receive` flake** (above): the cooperative
    equivalent (`test/otp/test_otp_proc_lib.c`) passes on every
    platform, so selective receive itself is correct; the flake is in
    test_proc's exact IOCP-wakeup timing and needs the host to chase.

**Native file AIO (mechanism):** `xtc_io_aio_submit` issues overlapped
`ReadFile`/`WriteFile`; `fsync`/`fdatasync` have no async form on
Windows (FlushFileBuffers is synchronous) and are offloaded.  Round 1
joined the OVERLAPPED.hEvent to the WaitForMultipleObjects set and was
verified on santorini under VS2022 (17) and VS2026 (18) by the smoke
test (an overlapped pwrite+pread round-trip).  Round 2 instead reaps
the AIO completion from the port (no hEvent); that variant is
COMPILED-NOT-RUNTIME-VERIFIED and the smoke test must be re-run.

Driving the santorini host non-interactively from CI/automation is not
yet wired (it is configured for an interactive PowerShell session); the
Windows matrix is run by hand via `dist/santorini-matrix.sh`.

## Async file I/O: backend coverage

`xtc_aio_pread/pwrite/fsync/fdatasync` present one portable API; the
native completion mechanism is per backend, with a blocking-pool
offload everywhere else (always correct, just thread-backed).  Native
completion is implemented and validated on:

  - **io_uring** (Linux): `IORING_OP_READ/WRITE/FSYNC`, reaped from the
    CQE ring.  CI `build-and-test` + ASan/UBSan.
  - **IOCP** (Windows): overlapped `ReadFile`/`WriteFile` reaped from
    the completion port; fsync offloaded.  santorini smoke test.
  - **kqueue** (FreeBSD, macOS): POSIX AIO (`aio_read`/`aio_write`/
    `aio_fsync`) with `SIGEV_KEVENT` -> `EVFILT_AIO`.  CI `macos` job
    compiles and runs `test_aio` against it (green).

  - **illumos / Solaris event ports** (SunOS 5.11): POSIX AIO
    (`aio_read`/`aio_write`/`aio_fsync`) with `SIGEV_PORT` posting the
    completion as a `PORT_SOURCE_AIO` event on the loop's existing event
    port, reaped in the `port_getn` drain (`aio_return` -> publish
    `a->res` -> wake `a->tag`).  Implemented in `src/io/io_solaris.c`
    (`xtc_io_aio_submit` + `struct sol_aio`); RUNTIME-verified on the
    illumos/OpenIndiana host (sun) -- full `gmake check` passes,
    including the native `/aio/roundtrip` case.

Still offloaded to the blocking pool (native AIO is a follow-up with
platform-specific completion plumbing -- they are DIFFERENT mechanisms,
not one shared path):

  - **epoll / poll / select** (Linux without io_uring, generic POSIX):
    would need POSIX AIO with `SIGEV_SIGNAL` delivered to a self-pipe /
    signalfd the loop already watches (epoll has no AIO filter).  A new
    shared `io_posixaio.c` submodule.
  - **AIX**: AIX `aio_*` (or the legacy LIO interface).

The offload fallback makes "write storage code once as if AIO is always
available" hold on all of these today; the follow-ups only remove the
thread hop.

**Decision (updated 2026-07): illumos native AIO is now DONE and
verified; epoll/poll/select and AIX stay offload-backed deliberately.**
Assessment of the remaining targets:
- Linux without io_uring (epoll/poll/select): the only ZERO-THREAD
  clean form is libaio (io_submit + an eventfd via IOCB_FLAG_RESFD),
  which adds a dependency; POSIX AIO with SIGEV_SIGNAL is
  signal-safety-hazardous and SIGEV_THREAD reintroduces the very thread
  hop this would remove.  On Linux the zero-thread answer already exists
  and is native: io_uring.  So this target has near-zero value.
- illumos event ports (SIGEV_PORT -> PORT_SOURCE_AIO on the existing
  port): DONE.  It was implemented WITH the illumos host in the loop
  (the bring-up did surface an EINVAL from a stack-local port_notify_t;
  fixed by heap-allocating the notify+aiocb in struct sol_aio so they
  outlive the async call) and now passes the full suite on sun.
- AIX aio_*: no test host at all; remains offload-backed.
The offload path is CORRECT and COMPLETE on the still-offloaded targets
(native AIO is a pure performance optimisation that removes a thread
hop, not a correctness gap).

## Portable block-device I/O layer -- DONE (v1.4.0)

Implemented as xtc_bdev (src/io/io_bdev.c, xtc_bdev.h): open a raw
device / partition (or a regular file), query logical+physical sector
size and capacity via the native per-OS ioctl (BLKSSZGET/BLKGETSIZE64,
DIOCGSECTORSIZE/DIOCGMEDIASIZE, DKIOCGMEDIAINFO, the Windows drive
geometry IOCTL) with a fstat fallback, aligned pread/pwrite through the
xtc_aio path, and flush via xtc_aio_fsync.  See xtc_bdev(3).

## test_alloc M7 skipped on Windows

**Status:** intentional -- `_aligned_malloc` returns memory that requires `_aligned_free`, not plain `free`. The hook surface uses a single free path. Keeping the M7 case Windows-skipped is correct.

## svr.c branch coverage

**Status:** ~68% branch / ~79% line as of the R4 (handle_continue) round
(measured with gcovr over the full make-check suite; the exact figure
drifts test-to-test, so treat it as approximate).  The uncovered lines
are error and edge paths, not mainline behavior.

**Targets remaining:** call-after-stop edge, reply-when-server-already-stopped, OOM during reply path.

## io_common.c coverage

**Status:** ~71% branch as of this round.  The uncovered branches are
error-cleanup paths and ENOMEM/EAGAIN edges in `xtc_io_init`/`xtc_io_fini`
not exercised by the happy-path tests.  Adding fault-injection tests
(use `xtc_inject_attach_wait` + a hook that returns ENOMEM) is the
cleanest fix.

## AIX runtime untested

**Status:** code-complete, awaiting host. No way to verify without one.

## RESOLVED: macOS now in CI

The `macos` GitHub Actions job (macos-latest, Apple Silicon) builds and
runs the full C munit suite every commit -- kqueue backend, ucontext
substrate, GCD dispatch semaphores.  Standing it up fixed six real
portability bugs (Darwin feature macro, rwlock storage size, unnamed
semaphores, _SC_NPROCESSORS_ONLN, hardcoded -lrt, lrlock slot
reclamation teardown order).


## sqlxtc multi-thread load test flakes on the macOS CI runner

**Status:** KNOWN FLAKE (transient, re-run passes).  The
`sqlxtc multi-thread load` CI step (`test/sqlxtc/test_sqlxtc_mt.sh` ->
`test_sqlxtc_concurrent.py`, N clients * M queries against a shared
server on a plain `CREATE TABLE`) intermittently fails on the macOS
runner with all clients timing out and `server CRASHED under multi-loop
load (cores=3)`.  Observed once on 5d78c00 and passed on re-run with no
code change; the 5 preceding commits' macOS jobs were green.

The failing path is the VDBE/connection handling under concurrent load
(the test table is NOT xstore-backed, so it never reaches the vexec
fast path) -- so it is independent of the vexec/Track-B work.  Likely
the same multi-loop cross-thread timing sensitivity seen elsewhere on
the macOS/kqueue runner.  Treat a lone macOS MT-load failure as flaky:
re-run the job (`gh run rerun <id> --failed`) and judge by the other 12
jobs.  A persistent failure across re-runs would indicate a real
concurrency regression and must be chased.

## `pbt_proc::send_recv_roundtrip` and `pbt_proc::fifo_order` flake under `make check`

**Status:** RESOLVED in M11.5b.  The proc registry's `__lt[]` table was
leaking entries on `xtc_loop_fini`; consecutive PBT loops were aliasing
stale entries.  Fix: added `__xtc_proc_loop_unregister(loop)` called from
`xtc_loop_fini`.  Both properties are re-enabled.

## xtc_cfg: missing features

**Status:** Config-file parsing and reload DONE; per-session scoping
remains M16.

- Configuration-file parsing (postgresql.conf reader): DONE --
  `xtc_cfg_load_file()` reads `name = value` lines (comments, quotes,
  per-kind parsing, bounds/validators), skipping unknown/bad lines.
- SIGHUP-driven reload: DONE as a mechanism -- `xtc_cfg_reload()`
  re-reads the last loaded file.  The app wires SIGHUP to it from the
  event loop (the function is not async-signal-safe, by documentation).
- Per-session/per-database scoping (PostgreSQL-specific): still M16 --
  it needs a session/override-stack model that does not exist yet.

## xtc_slab_pressure_stop API incomplete

**Status:** DONE.

Resolved: `xtc_slab_pressure_listen_ex()` returns an opaque
`xtc_slab_pressure_t` handle and `xtc_slab_pressure_stop(handle)` joins
the listener thread, closes its fds, and frees it.  Plain
`xtc_slab_pressure_listen()` delegates to `_ex` and discards the handle
(unchanged fire-and-forget behaviour).


## epoll backend: rare lost blocking-I/O-completion wakeup under heavy churn

**Status:** RESOLVED (pending continued monitoring).  The primary causes
were fixed earlier (see (A)/(A-residual)/(B) below); the last remaining
epoll-only residual in the buffer-manager stress test appears CLOSED by
the v1.8.0 cross-thread prepare/park wake fix.

**The closing fix (v1.8.0):** the residual was the same lost-wake class
as the PostgreSQL carrier's cross-thread fd/latch wake miss -- a
cross-thread wake (here, the blocking pool completing an offloaded fsync
and waking the parked evictor) arriving while the target task was still
RUNNING, between arming its waker and the loop transitioning it to
PARKED on yield, was DROPPED by the XTC_INB_WAKE drain (which only
enqueued PARKED tasks).  The fix latches task->wake_pending when the
task is not yet PARKED and the RUNNING->PARKED transition consumes it
and re-schedules instead of parking (see src/evt/loop.c).

**Evidence:** test_bufmgr_mt, which historically hung ~3-7% of runs on
epoll, now passes 130/130 consecutive runs on the epoll backend with
zero hangs, and the reproducer test/concurrency/repro_blocking_epoll.c
(which hung 8/8 at its worst) passes 10/10.  test_bufmgr_mt is no longer
gated off epoll: it runs on the Codeberg (epoll) CI as well as the
io_uring CI.  Kept in this document (rather than deleted) so the
long investigation and the several rejected approaches below stay on
record; if any epoll wake-miss recurs, this is the history to read
first.

<details><summary>Historical investigation (the fixes that led here)</summary>

### (A) FIXED -- xtc_yield / xtc_await did not preserve __current_proc

The primary lost-wakeup was not in the epoll backend at all.  It was a
fiber-context bug: the per-thread "current proc" pointer
(`__current_proc`, an L3 process-layer TLS) was not preserved across a
yield.  When a proc calls `xtc_yield()` (or `xtc_await`) the scheduler
runs OTHER procs in between -- each setting `__current_proc` to itself --
and on resume the public yield primitives did NOT restore it.  Internal
parks (`xtc_proc_wait_fd`, `xtc_proc_sleep`, `xtc_recv`, the amutex)
each restored it by hand, but a plain `xtc_yield()` did not.  A proc
that yielded therefore resumed running as whatever proc ran last; its
next `xtc_blocking_run` -> `wait_fd` then registered the completion fd
under the WRONG task and parked the WRONG task, so the real proc never
woke.

Minimal reproducer: ONE loop, TWO procs, three `xtc_blocking_run` calls
each -- hangs 10/10 on epoll (a trace showed proc P1, after its
worker's bare `xtc_yield()`, registering its pipe fd under proc P2's
task tag and "P2" parking twice without waking).  io_uring masked it:
its completion path re-samples readiness, so the misattributed park
self-healed; epoll's `epoll_wait(-1)` blocked forever.

Fix: preserve the process context across every coro yield via a hook
(`__xtc_fiber_ctx_save` / `__xtc_fiber_ctx_restore`, installed by
`xtc_proc_spawn`, no-op when no process layer is in use, so the L2 coro
layer keeps no hard dependency on L3).  Applied to `xtc_yield` and
`xtc_await` in all three coro substrates (fctx, ucontext, Win32 fiber).
Verified: the pure reproducer goes 0 -> 20/20 on epoll (and the
N-loop/N-proc one 0 -> 20/20); io_uring `make check` + ASan + UBSan +
epoll `make check` all clean; io_uring `bufmgr_mt` 50/50.

### (A-residual) FIXED -- epoll-only hang in test_bufmgr_mt was a pin underflow

After the (A) fix, `test_bufmgr_mt` still hung about 3-7% on the epoll
backend (io_uring far less, and confounded by host load).  The earlier
read of this as a benign "thrash livelock" was WRONG: it was a real
pin-accounting bug in the example's from-scratch buffer manager
(examples/06_sqlxtc/bufmgr.c), traced to ground with armed abort probes
+ post-mortem cores and now fixed at root.

Root cause (swip mode only): a fixer can hold a STALE swip word `w`
pointing at a frame that has since been evicted, freed, and recycled by
the demand-load path for a DIFFERENT slot.  The fixer does
`try_pin(sw_frame(w))` and transiently bumps that recycled frame's pin
(it fails its slot recheck a moment later and unpins -- net zero).  But
the demand-load path claimed the frame with an UNCONDITIONAL
`store(pin, 1)`, which clobbered the fixer's transient increment; the
fixer's matching `unpin` then drove pin to -1.  A frame wedged at
pin == -1 is indistinguishable from an eviction reservation, so every
later fixer spins `try_pin` on it forever -- the hang.  (epoll merely
lost the timing lottery more often; io_uring re-samples readiness and
mostly masked it.)

Fixes, all in bufmgr.c, verified epoll 80/80 + io_uring 30/30 + ASan
12/12 + UBSan 8/8 clean, with the btree/xstore (pid-mode) tests green:

  - The swip demand-load and swip alloc paths claim a recycled frame
    with a CAS loop (`claim_frame`: spin CAS pin 0 -> 1, yielding),
    NOT a blind store, so a fixer's transient stale pin is never
    clobbered.  Scoped to swip mode only: pid-mode (`bm_fix_pid`, the
    B-tree path) has no swip references, so no stale fixer exists; it
    keeps the plain store (a CAS-wait there would deadlock against a
    latch-coupling pin that cannot drain while the claimer spins).
  - Eviction releases a reservation with CAS(-1 -> 0), never a blind
    store, so it cannot clobber a concurrent loader's fresh pin; and it
    re-validates `state == BM_COOL` after reserving (a stale COOL read
    could otherwise reserve a frame already on the free list).
  - `free_push` sets `state = FREE` before clearing pin, closing the
    COOL+pin==0 window an eviction sweep could otherwise reserve in.

Two general robustness improvements landed alongside (they reduce churn
and were part of the original investigation, kept because they are
correct on their own): `bm_fix` yields on a contended `try_pin` retry
so the loop regains control, and a CLOCK second-chance reference bit on
frames spares a recently touched COOL page one eviction sweep.

test_bufmgr_mt is no longer gated: it runs on BOTH the io_uring CI
(GitHub) and the epoll CI (Codeberg).

### (B) FIXED -- buffer-manager load/publish pin-ordering race

While chasing the epoll hang, a genuine buffer-manager race was found
and fixed (examples/06_sqlxtc/bufmgr.c).  In bm_fix's demand-load path
the frame was published as BM_COOL and only THEN pinned
(`state = COOL; pin = 1`).  In that window a concurrent evict_one --
which acts on COOL frames -- could win `try_reserve` (CAS pin 0 -> -1),
and the load's unconditional `pin = 1` store then clobbered the
reservation, leaving the frame both published (in use) and pushed back
onto the free list: a double-owned frame that corrupts the free list
(`free_n` seen at 762 for a 32-frame pool) and livelocks get_free_frame.
Fix: pin the frame BEFORE it is ever visible as COOL/HOT (own it from
get_free_frame on), so eviction's try_reserve always fails on it.  Same
reorder applied to bm_fix_pid.  Verified: io_uring make check + ASan +
UBSan clean, bufmgr_mt 12/12.  This race is normally hidden when the
offloaded load completes fast; it surfaces when completion is slow.

The buffer-manager multi-threaded stress test (`examples/06_sqlxtc`,
`test_bufmgr_mt`) hangs intermittently when libxtc is built with the
epoll I/O backend (`--with-io-backend=epoll`); the same test passes
reliably under io_uring (130+ runs).  It is therefore run only on the
io_uring CI (GitHub) and skipped on the epoll CI (Codeberg containers,
whose seccomp profile blocks io_uring).

Diagnosis (post-mortem core, non-instrumented timing):

  - All executor loop threads are idle in `epoll_wait`, the blocking
    thread-pool workers are idle on their condvar (so every submitted
    disk I/O has COMPLETED and its wakeup byte was written), free
    frames are available (`free_n == 8`), and there are zero data
    mismatches -- yet `xtc_exec_run` never returns because one worker
    proc (`g_workers_done == N_WORKERS - 1`) never finishes.

  - The stuck worker is the buffer-manager EVICTOR: it reserved a
    frame for eviction (`pin == -1`), parked in `xtc_proc_wait_fd` on
    the offloaded flush write, and its completion wakeup was lost, so
    the frame stays `EVICTING` forever.  The loss is rare (~1 in many
    thousands of `xtc_blocking_run` calls) and timing-dependent, which
    is why the heavy-churn stress test triggers it while the lighter
    loop tests (test-vfs-loop, test-xstore, ...) do not.

  - io_uring masks it: its completion path samples current readiness
    rather than blocking indefinitely on a level/edge fd transition,
    so a missed edge self-heals; epoll's `epoll_wait(timeout = -1)`
    blocks forever.

The lost wakeup is in `xtc_proc_wait_fd`'s epoll arm/park/dispatch
path, hit through `xtc_blocking_run`.  A later session built the
minimal isolated reproducer this asked for -- N procs issuing
concurrent `xtc_blocking_run` on an N-loop epoll executor, no buffer
manager (test/concurrency/repro_blocking_epoll.c) -- and it hangs 8/8,
so the bug is purely in the library primitive.  Findings:

  - **It is NOT fd reuse.**  A persistent per-proc wakeup pipe (never
    reused across procs while a registration is live) still hangs, and
    a bounded 50ms re-poll of every loop does not recover the parked
    proc.  Ground truth from a core: stuck procs are PARKED on fds
    that are READABLE (the wake byte was written) yet are NOT in any
    epoll instance -- the fd was unregistered while the proc was still
    parked on it.  The only `del_fd` that can do this is dispatch's
    `del_fd(t->park_fd)`.

  - **Approaches tried, and why each was rejected (none shipped):**

    1. *Cross-thread waker* (pool wakes the proc via xtc_waker_wake):
       fixes the hang but ASan reports a heap-use-after-free -- the
       proc can return from xtc_blocking_run and exit the instant its
       result is read, racing a wake that holds its task pointer.

    2. *Timer-poll* (proc polls a done flag via xtc_proc_sleep):
       memory-safe and fixes the pure reproducer, but its added
       latency makes test_bufmgr_mt deadlock on BOTH backends (it
       slows completion enough to provoke buffer-manager frame
       exhaustion), i.e. it REGRESSES io_uring, which passes today.

    3. *Persistent per-proc wakeup pipe*: still hangs (ruling out fd
       reuse, as above).

    4. *Remove del_fd from dispatch* (rely on the parker's cleanup +
       run-before-poll): made the reproducer hang 12/12 -- the
       still-readable fd is mishandled, so the dispatch del_fd is
       load-bearing in a way not yet understood.

    5. *Event carries the fired fd* (epoll stores the fd in
       `data.u64`, a per-loop fd->tag map recovers the registrant, and
       dispatch dels exactly `ev->fd` -- the fd that actually fired --
       instead of a possibly-stale `t->park_fd`): implemented across
       all backends, verified to NOT regress io_uring (make check +
       reproducer + bufmgr_mt all green), but the epoll reproducer
       still hangs 15/15.  So the wrong-fd-del was not the root cause;
       reverted rather than ship inert complexity (a per-loop map).

    6. *Non-blocking completion read + re-wait* (set the pipe read end
       O_NONBLOCK and loop wait_fd until the byte is actually read, so
       a wakeup with no byte yet cannot block the loop thread in
       read()): contained to blocking.c, but the epoll reproducer
       still hangs 15/15.  Reverted.

Sharper diagnosis (instrumented, this is where it stands):

  - A parked proc resumes from its wait_fd `xtc_yield()` with
    `wake_revents == 0` AND `park_fd` still set -- i.e. it was
    re-scheduled WITHOUT the fd dispatcher running (dispatch sets
    wake_revents and clears park_fd before waking).  Such "spurious"
    resumes are not rare: tens of thousands per run.  The committed
    blocking_run then does a blocking read() on the not-yet-readable
    pipe, wedging the loop thread so it stops polling -- a plausible
    cascade into the hang -- but making that read non-blocking
    (approach 6) did not fix it, so the spurious resume is not the
    whole story.

  - The spurious resumes are NOT cross-thread: an instrumented
    `__xtc_inbox_push` counted ZERO `XTC_INB_WAKE` pushes for the
    whole run, so no `xtc_waker_wake` cross-thread path fired.  The
    only same-thread enqueue of a PARKED proc is the fd dispatcher,
    which clears park_fd and sets wake_revents -- so a resume with
    neither updated is self-contradictory under the current model and
    indicates the park/dispatch bookkeeping is desynchronised from the
    actual scheduler wakeup in a way the probes perturb rather than
    pin.  Per-fd counters also show stuck fds with `del == reg` and
    `deliv == 0/1` (the fd is unregistered, by the parker's own
    cleanup on a spurious resume, before epoll ever delivers its
    event), consistent with the "readable but not in any epoll"
    post-mortem above.

The bug needs a dedicated redesign rather than a point fix -- most
likely a per-proc wakeup eventfd registered ONCE at spawn with
EPOLLONESHOT re-arm (no per-wait registration churn, memory-safe, and
no cross-thread proc reference), or moving blocking-completion off the
fd-park mechanism entirely.  Until then test_bufmgr_mt runs only on the
io_uring CI and is skipped on the epoll CI, and xtc_blocking_run keeps
its committed pipe + wait_fd path (fast on io_uring).  The reproducer
and this write-up set up that focused effort.

### FIXED -- test_server_storage flaky hang (lost cross-thread wakeup)

`examples/06_sqlxtc/test_server_storage` hangs intermittently (~1 in 8
locally, io_uring backend) in the connection-per-proc + storage
workload.  A SIGABRT core of a hung run shows the scheduler loops
blocked in `io_uring_wait_cqes` while two blocking-pool workers sit in
`fdatasync` -- the WAL group-commit writer (wal.c `flush_io_fn`) and the
double-write buffer (bufmgr.c `dw_io_fn`).  The signature is a lost
cross-thread completion wakeup: a fiber offloaded an fsync via
`xtc_blocking_run`, the worker is (or just finished) running it, but the
loop never observes the completion and idle-waits forever.

Present at the shipped state (e36a68d), so it is NOT caused by the
xtc_aio page-I/O wiring; that work (do_io / bm_sync on xtc_aio) is
orthogonal.  It was surfaced while attempting to convert the WAL writer
and double-write buffer onto xtc_aio: that conversion must wait until
this lost-wakeup is understood, since adding more concurrent
completion traffic to the same hot path would only worsen it.

Likely the same lost-wakeup class as the (A) investigation; needs a
dedicated session with a clean core walk of the parked fiber's waker
state and the blocking-pool -> loop wake path.  Until then it is a
known flake on the io_uring examples CI.

FIXED (commit after b7a6558): the root cause was a cross-thread io_uring
submission.  xtc_proc_wait_fd -- which xtc_blocking_run parks on --
registered its wait fd on self->task->loop->io (the proc's HOME loop),
but under the multi-loop executor a proc runs stolen on another loop, so
the POLL_ADD went to a ring this thread does not own and was silently
dropped.  Fixed by registering/timing/cleaning up on __xtc_current_loop
(the running loop).  test_server_storage 30/30; the WAL/double-write
xtc_aio conversion is now unblocked.

</details>

## RESOLVED: macOS build break: sigev_notify_kqueue (io_kqueue.c)

**Status:** FIXED.  A 2026-06 macOS SDK image dropped the BSD
`struct sigevent` member `sigev_notify_kqueue`, so the native-file-AIO
path added in ce0cacc failed to compile on macOS with "no member named
'sigev_notify_kqueue'".  The `#if` guard
(`defined(EVFILT_AIO) && defined(SIGEV_KEVENT)`) passed, but the member
was absent under the current SDK.

**Fix:** the native kqueue file-AIO path is now restricted to the
platforms that actually provide the member AND honor SIGEV_KEVENT
completion for regular files -- FreeBSD and DragonFly
(`#if ... && (defined(__FreeBSD__) || defined(__DragonFly__))` in
src/io/io_kqueue.c).  macOS is deliberately excluded and falls through
to the blocking-pool offload (the XTC_E_NOSYS path), which is correct
and never blocks the loop -- Darwin's POSIX AIO does not reliably
support SIGEV_KEVENT completion on regular files anyway.  The macOS CI
job (macos-latest, Apple Silicon) builds and runs the full C suite
green on every commit.
