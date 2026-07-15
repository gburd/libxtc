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

## REFINED root-cause hypothesis (2026-07, after reading the dump path)

STRONGEST candidate: __os_backtrace() -> system backtrace() (execinfo)
walking a UCONTEXT FIBER STACK on arm64.  test_dump/basic calls
xtc_dump(fd) from INSIDE a live proc (dump_driver, a fiber), so the
backtrace runs on a small swapped-in ucontext stack.  macOS's
backtrace() follows the frame-pointer chain; on a fiber stack that
chain can run past the (small, guard-paged) stack top into unmapped
memory -> SIGBUS.  Evidence FOR:
  - test_dump/basic (dump from a live fiber) SIGBUSes; test_dump/panic/
    aborts (also calls xtc_dump, but from the panic/abort context, not
    a live fiber body) passed in the same run.
  - SIGBUS = bad access (walking off the stack), matches.
  - macOS/arm64 only; Linux backtrace (EH-ABI unwinder / execinfo)
    terminates cleanly on the same fiber stacks; ruled out alignment
    (struct xtc_proc_table has no over-aligned member).
  - Intermittent because it depends on what garbage sits just past the
    fiber stack top and whether it dereferences to a mapped page.

## Confirming experiment (needs a macOS/arm64 host)
1. On macOS, run test_dump under lldb; on the SIGBUS, `bt` -- expect
   the crash inside backtrace()/__os_backtrace, walking frames beyond
   the fiber stack.
2. Quick confirmation without lldb: temporarily make xtc_dump skip the
   backtrace section (n = 0 branch) and see if /dump/dump/basic goes
   reliably green on macOS across a seed sweep.  If yes -> confirmed.

## Candidate fix (implement ONLY after the experiment confirms it)
A fiber-stack-aware frame walker for the dump backtrace: the runtime
knows the current coro's stack bounds (__xtc_current_coro->stack +
guard/size, see coro_common.h).  Walk the FP chain manually and STOP
when the next frame pointer leaves [stack_lo, stack_hi) -- never
dereference off the fiber stack.  Fall back to system backtrace() only
when not on a fiber (__xtc_current_coro == NULL).  This is macOS-arm64-
specific and MUST be validated on a real host; NOT blind-patched (the
system backtrace works fine on Linux and on the main thread, and a
wrong guard could silently degrade the diagnostic everywhere).

## Status: NOT a release blocker (intermittent, diagnostic-only path,
## green on most runs); tracked for a macOS-host session.  The dump's
## backtrace is a debugging nicety, not a correctness mechanism.

---
(original note below)


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
