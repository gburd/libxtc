# Cross-thread wake-path contention (proc.c __lt_lock) -- 2026-07

## UPDATE 2026-07-14: the __lt_lock fix was NOT the PG bottleneck

The PG team's follow-up (/tmp/libxtc-proc-mutex-contention-report.md)
MEASUREMENT-FALSIFIED the wake-path hypothesis. Uprobe under load:
  xtc_proc_wake    = 0-1 calls / 10s   (essentially never)
  xtc_proc_wait_fd = 39 calls  / 10s
The fiber carrier cycles resumable backends in USERSPACE -- no
syscall-park, no cross-thread wake per command -- so __resolve Strategy 2
/ __lt_lock / xtc_proc_wake are all OFF their hot path. The __lt_lock
fix below (commit 76e7844) is still correct and stays (it removes a real
global serial section for cross-exec wake fan-out), but it does NOT move
the fiber-per-session PG plateau.

REAL bottleneck, root-caused from their 5 verified observations
(heap-allocated pthread_mutex, ~367k __lll_lock_wait_private/10s, on the
common cooperative cycle with ZERO wake/park syscalls, flat in carrier
count, absent at -c1): `t->lock`, the per-LOOP xtc_proc_table mutex
(proc.c ~570), shared by every fiber/proc on a carrier. Every xtc_send
-- including a same-loop fiber-to-fiber hand-off -- calls __resolve ->
__table_lookup (proc.c ~649), which takes t->lock to read the slot +
pin the target with a refcount. Per-command reader->backend->reply
hand-off => >=1 such send => every fiber on a carrier serializes on that
one per-loop lock => flat in carrier count. Reply written to the team:
/tmp/libxtc-proc-mutex-contention-reply.md. Tracked as PLAN.md 19.5c.
The real fix (lock-free __table_lookup via RCU against the teardown
protocol + a resume-vs-exit UAF DST test) is its OWN session, NOT this
release -- an RCU change to a per-command hot path with a live UAF
protocol will not be rushed onto a release eve.

---

## Original report (superseded above for the PG case)

## Source
/tmp/libxtc-wake-path-contention-report.md (PG fiber-carrier team,
libxtc v1.20.1, EC2 m6id.8xlarge, pgbench -M prepared -S -c16 -j16).

## Symptom
Fiber-per-session PostgreSQL throughput plateaus ~2.3x below the
process-per-backend baseline; machine ~83% idle; carriers blocked in
futex_wait (1.19M off-CPU samples, all __lll_lock_wait_private). Gap
invisible at -c1, does NOT respond to carrier count -> a per-wake serial
section, not per-command cost.

## Root cause (confirmed in source)
Every cross-thread xtc_proc_wake()/xtc_send() to a proc on a loop
OUTSIDE the caller's own exec falls to `__resolve` Strategy 2, which
scanned the process-global loop registry `__lt[]` under a single global
`__lt_lock` (proc.c). With N carriers every command-boundary wake
funnels through that one mutex -- a classic Amdahl serial section.

`__resolve` Strategy 1 (lock-free) already handles: target on the
caller's own loop, or a sibling loop in the caller's SAME xtc_exec. So
same-exec wakes were never the problem; cross-exec (or main-thread)
wakes were.

## Fix landed (option 1 in the report -- the primary ask)
Made the `__lt[]` slots atomic:

    struct lt_entry {
        _Atomic(xtc_loop_t *)             loop;
        _Atomic(struct xtc_proc_table *)  tbl;
    };

- `__resolve` Strategy-2 scan: LOCK-FREE, acquire loads, no __lt_lock.
- `__table_for` existing-table lookup: LOCK-FREE acquire fast path;
  only the create path (first proc on a loop) takes __lt_lock, and
  re-scans under it.
- register (`__table_for` create): publish tbl BEFORE loop with release
  stores, so a reader that sees a non-NULL loop is guaranteed the fully
  initialized table (never NULL).
- unregister (`__xtc_proc_loop_unregister`): clear loop FIRST (release),
  then tbl; the actual table free happens after the unlock, and the
  contract is a finalizing loop has no live procs / in-flight wakes.
- The three inspect/dump helpers that also scan __lt[] keep the lock
  (cold paths) but now read the slots via atomic_load_explicit (relaxed,
  under the lock) since the fields are _Atomic.

The proc lifetime race is unchanged: the refcount (__proc_release) +
generation check on the resolved proc still handle a proc dying under a
wake, exactly as when the scan held the lock. The atomic slots only
remove the global serialization of the *lookup*.

## Verification
- Full DST suite (test/sim/run_sim_tests.sh): 52/52 green, incl. the
  new test_sim_chash and every proc/reg/svr/xproc/teardown sim test;
  byte-identical replay preserved.
- test_proc_wake_crossthread: 0/100 lost wakes, 20 consecutive runs.
- TSan (clang + XTC_TSAN_FIBERS, the CI configuration): test_proc,
  test_svr, test_chan, test_reg all clean -- no data race from the
  lock-free scan. (gcc-TSan cannot run the fiber suite: "unexpected
  memory mapping" on the coroutine stacks -- documented, not this
  change. The one exec.c:134 race gcc-TSan flags is a pre-existing
  each-worker-writes-own-slot loop_node[] NUMA write, unrelated.)
- Gates: api-discipline, s_async, s_layer, s_noalloc, s_perm all OK.

## Deferred (option 2 -- NOT a release blocker)
Make xtc_proc_wake fire the armed waker WITHOUT taking the target's
per-proc mbox_lock (lock-free "set pending + wake"). Reasons to defer:
- mbox_lock is sharded per-target-proc (N targets => N locks), so it is
  far less contended than the single global __lt_lock was; option 1
  removes the dominant serial section.
- A lock-free wake races the park path's arm/disarm of `waker_armed`
  (plain int, set/cleared under mbox_lock). A lost wakeup here is a
  HANG -- the exact bug class (idle-loop wake-miss, cross-fd wake-miss)
  this codebase has already been bitten by.
- It must land with a DST test that PLANTS a lost-wakeup and proves the
  simulator catches it fast (per the project's bug-injection discipline).

## Dropped (option 3): loop-affinity guidance
PG team confirmed not needed. Would have been docs-only. Fast-path
condition for the record: co-locate a session's fiber and its typical
waker in the SAME xtc_exec and Strategy 1 skips the registry entirely.
