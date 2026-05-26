# Known issues — pending investigation

## Windows: `test_proc::selective_receive` regression

**Status:** failing on Windows MinGW after this round; was passing 36/36 last round.

**Symptom:** the test sends 5 messages to a proc before calling `xtc_loop_run`; the proc uses `xtc_recv_match` to selectively pick value 42 first, then drains 1, 2, 3, 4 in order. On Windows specifically, this fails — likely a timing/ordering issue with the IOCP wakeup path.

**What changed this round:**
- 4 new modules linked into `libxtc.a` (log, cfg, inject, pdict) — none of which touch proc/recv
- `xtc_res` gained alert callbacks — no path through proc/recv exercises them in the test
- `__mbox_deliver` operator-precedence fix — semantically equivalent for the tested case (alive=1, cap=0)

**Hypothesis:** the new modules' static initialization or symbol-table churn may have shifted memory layout, surfacing a latent ordering bug in the Windows IOCP path. On Linux/FreeBSD/illumos the test passes consistently.

**Workaround:** none yet. The cooperative test (`test/otp/test_otp_proc_lib.c::test_selective_receive`) covers the same scenario and passes on all platforms; the bug is specific to test_proc's exact configuration.

**Next steps:**
1. Re-run on Windows with `--no-fork` to see deterministic output.
2. Add `xtc_log` calls inside `__do_recv` to trace the receive path on Windows.
3. Compare the IOCP wakeup integration after the fixes (round 3 IOCP poll now drains all signaled events; possibly some interaction).
4. If unfixable in current shape, mark `test_proc::selective_receive` as Windows-skip until M16-era cleanup.

## test_alloc M7 skipped on Windows

**Status:** intentional — `_aligned_malloc` returns memory that requires `_aligned_free`, not plain `free`. The hook surface uses a single free path. Keeping the M7 case Windows-skipped is correct.

## svr.c branch coverage 50.78%

**Status:** improved this round with `test_otp_gen_server_phase2.c` (+10 cases). Audit re-run needed to measure.

**Targets remaining:** call-after-stop edge, reply-when-server-already-stopped, OOM during reply path.

## io_common.c at 65.71% line / 42.86% branch

**Status:** error-cleanup paths and ENOMEM/EAGAIN edges in `xtc_io_init`/`xtc_io_fini` not exercised. Adding fault-injection tests (use `xtc_inject_attach_wait` + a hook that returns ENOMEM) is the cleanest fix.

## AIX runtime untested

**Status:** code-complete, awaiting host. No way to verify without one.

## macOS untested

**Status:** awaiting host setup.

## `pbt_proc::send_recv_roundtrip` and `pbt_proc::fifo_order` flake under `make check`

**Status:** RESOLVED in M11.5b.  The proc registry's `__lt[]` table was
leaking entries on `xtc_loop_fini`; consecutive PBT loops were aliasing
stale entries.  Fix: added `__xtc_proc_loop_unregister(loop)` called from
`xtc_loop_fini`.  Both properties are re-enabled.

