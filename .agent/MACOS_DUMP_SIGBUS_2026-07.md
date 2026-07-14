# test_dump /dump/dump/basic -- intermittent SIGBUS on macOS (2026-07)

## Observed
CI run 29356719929, macos job (macos-latest, Apple Silicon):
  running C test: test_dump
  /dump/dump/basic   [ ERROR ]   "child killed by signal 10" (SIGBUS)
  2 of 3 tests successful
The SAME code (with the 19.5c proc-table striping) was macos-GREEN on
run 29356485238.  So it is INTERMITTENT / seed-dependent, not a
deterministic regression from the striping.

## Why it is suspicious but probably NOT the striping
test_dump/basic runs a loop with a parked sleeper proc + a driver proc
that calls xtc_dump(fd) -> xtc_inspect_procs/_loops, which 19.5c
changed (single tbl->lock -> per-id stripes; the whole-table inspect
scan now takes __pt_lock_all).  BUT:
  - macOS was green on a run that already had the striping.
  - The lock discipline is preserved: __table_release takes the per-id
    stripe, __pt_lock_all takes all stripes, so they still exclude;
    __fill_proc_info takes p->mbox_lock INSIDE the all-stripes hold
    (all-stripes -> mbox_lock ordering; no inversion found -- nothing
    takes mbox_lock then a stripe).
  - SIGBUS is a bad-memory-access signature (misalignment / stale
    pointer / freed read), not a deadlock (which would hang the alarm).
  - Only macOS/arm64 shows it; Linux (incl. ASan+UBSan+TSan on the
    proc/inspect paths) is clean, and TSan on test_proc/svr/reg/sup is
    clean.  Apple-Silicon SIGBUS on an access Linux tolerates usually
    means an alignment assumption.

## Leading hypotheses (for a macOS-host investigation)
1. A pre-existing macOS flake in test_dump unrelated to 19.5c (the
   test exercises the crash/panic path -- KNOWN_ISSUES.md 153 -- and
   xtc_dump is signal-safety-sensitive; a race between the driver's
   dump and loop teardown could read a half-freed proc/loop only on
   the ucontext-substrate macOS scheduling order).
2. An over-aligned member (_Alignas(XTC_CACHE_LINE)) in a proc/loop/
   table struct read by the inspect path, mis-handled on arm64 under
   the specific alloc path -- the exact class AGENTS.md warns about
   (must use __os_aligned_alloc, not __os_calloc, for over-aligned
   structs).  Worth auditing struct xtc_proc_table / xtc_proc for a
   cache-line-aligned member now that the table struct grew a
   16-mutex array.

## Action
- NOT a release blocker on its own IF it re-runs green (intermittent),
  but it MUST be root-caused on a real macOS host before it can be
  trusted -- an intermittent SIGBUS in the state-dump path is exactly
  the kind of thing that hides a real memory bug.
- Needs a macOS/arm64 host (or the macos CI runner with a repro loop:
  run test_dump under lldb / with a fixed seed sweep) to catch it.
- Check first: did struct xtc_proc_table gaining pthread_mutex_t
  stripes[16] change its alignment/size such that the __os_calloc in
  __table_for now returns storage that trips an over-aligned member?
  (pthread_mutex_t is not over-aligned, so probably not, but verify.)
