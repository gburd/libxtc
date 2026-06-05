# Known issues -- pending investigation

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
Validated on the MSVC build host: the smoke test triggers a real
access violation, the VEH contains it, and the process recovers.
While wiring this up, a pre-existing Windows double-free was found and
fixed in `coro_winfiber.c` (the done branch destroyed the coro
eagerly *and* via the task cleanup at `loop_fini`), which had made any
loop+process tear down with heap corruption on Windows.

## Windows: `test_proc::selective_receive` regression

**Status:** failing on Windows MinGW after this round; was passing 36/36 last round.

**Symptom:** the test sends 5 messages to a proc before calling `xtc_loop_run`; the proc uses `xtc_recv_match` to selectively pick value 42 first, then drains 1, 2, 3, 4 in order. On Windows specifically, this fails -- likely a timing/ordering issue with the IOCP wakeup path.

**What changed this round:**
- 4 new modules linked into `libxtc.a` (log, cfg, inject, pdict) -- none of which touch proc/recv
- `xtc_res` gained alert callbacks -- no path through proc/recv exercises them in the test
- `__mbox_deliver` operator-precedence fix -- semantically equivalent for the tested case (alive=1, cap=0)

**Hypothesis:** the new modules' static initialization or symbol-table churn may have shifted memory layout, surfacing a latent ordering bug in the Windows IOCP path. On Linux/FreeBSD/illumos the test passes consistently.

**Workaround:** none yet. The cooperative test (`test/otp/test_otp_proc_lib.c::test_selective_receive`) covers the same scenario and passes on all platforms; the bug is specific to test_proc's exact configuration.

**Next steps:**
1. Re-run on Windows with `--no-fork` to see deterministic output.
2. Add `xtc_log` calls inside `__do_recv` to trace the receive path on Windows.
3. Compare the IOCP wakeup integration after the fixes (round 3 IOCP poll now drains all signaled events; possibly some interaction).
4. If unfixable in current shape, mark `test_proc::selective_receive` as Windows-skip until M16-era cleanup.

## IOCP backend status (Windows)

**Status:** backend functionally complete; remaining items are
Windows-runtime-dependent and need an interactive santorini session.

The IOCP backend (`src/io/io_iocp.c`) passes the full suite on the
reference Windows toolchain (MinGW64: 233/233, see
`docs/M_WINDOWS_MATRIX.md`).  Registration uses loopback sockets in the
tests (`test/include/io_pipe_compat.h`), which compose with
WSAEventSelect; anonymous CRT pipes do not, by design.  The open items
are not library defects:

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
  - **Round-2 AFD/NtDeviceIoControlFile fast path** is a performance
    upgrade, not a correctness gap (round 1 is correct).

Driving the santorini host non-interactively from CI/automation is not
yet wired (it is configured for an interactive PowerShell session); the
Windows matrix is run by hand via `dist/santorini-matrix.sh`.

## test_alloc M7 skipped on Windows

**Status:** intentional -- `_aligned_malloc` returns memory that requires `_aligned_free`, not plain `free`. The hook surface uses a single free path. Keeping the M7 case Windows-skipped is correct.

## svr.c branch coverage 50.78%

**Status:** improved this round with `test_otp_gen_server_phase2.c` (+10 cases). Audit re-run needed to measure.

**Targets remaining:** call-after-stop edge, reply-when-server-already-stopped, OOM during reply path.

## io_common.c at 65.71% line / 42.86% branch

**Status:** error-cleanup paths and ENOMEM/EAGAIN edges in `xtc_io_init`/`xtc_io_fini` not exercised. Adding fault-injection tests (use `xtc_inject_attach_wait` + a hook that returns ENOMEM) is the cleanest fix.

## AIX runtime untested

**Status:** code-complete, awaiting host. No way to verify without one.

## RESOLVED: macOS now in CI

The `macos` GitHub Actions job (macos-latest, Apple Silicon) builds and
runs the full C munit suite every commit -- kqueue backend, ucontext
substrate, GCD dispatch semaphores.  Standing it up fixed six real
portability bugs (Darwin feature macro, rwlock storage size, unnamed
semaphores, _SC_NPROCESSORS_ONLN, hardcoded -lrt, lrlock slot
reclamation teardown order).


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

**Status:** Primary root cause FOUND and FIXED (see (A) below); a rarer,
separate, epoll-only residual remains under investigation in the
buffer-manager stress test.

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
