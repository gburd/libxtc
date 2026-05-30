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

**Status:** Planned for M16.

The following xtc_cfg features are not yet implemented:
- Per-session/per-database scoping (PostgreSQL-specific)
- Configuration-file parsing (postgresql.conf reader)
- SIGHUP-driven reload (signal integration)

These are tracked here rather than inline to avoid "v1" language in the code.

## xtc_slab_pressure_stop API incomplete

**Status:** Deferred.

The PSI pressure listener (`xtc_slab_pressure_listen`) cannot be cleanly
stopped because it doesn't return a handle.  A future API change will add
`xtc_slab_pressure_listen_ex()` returning an opaque handle and
`xtc_slab_pressure_stop(handle)`.  For now, the listener runs until
process exit.

