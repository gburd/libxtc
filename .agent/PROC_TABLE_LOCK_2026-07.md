# 19.5c -- Per-loop proc-table lock (t->lock) contention: design + decision

## The bottleneck (confirmed, PLAN.md 19.5c)
Every xtc_send -- including a same-carrier fiber-to-fiber hand-off --
calls __resolve -> __table_lookup (proc.c), which takes the per-LOOP
mutex t->lock to read a slot and pin the target proc with a refcount.
One t->lock per carrier, shared by every fiber on it, so per-command
sends serialize on it.  ~367k __lll_lock_wait_private/10s under the PG
fiber-per-session workload; flat in carrier count.

## Why the lookup cannot trivially go lock-free
Two coupled hazards, both currently covered by holding t->lock across
the whole load+refcount:

1. ARRAY REALLOC.  __table_alloc_slot grows via __os_realloc(t->slots),
   which MOVES the array.  A lock-free reader indexing t->slots[id]
   could read a freed/moved array during a concurrent grow.

2. PROC FREE.  __proc_free does an immediate __os_free(p) (NOT
   RCU-retired).  A lock-free reader that loads slot.proc then does
   refs++ races the free -> use-after-free.  The lock is what makes
   "see-live-and-pin OR see-NULL, never freed" atomic vs teardown.

Also: proc.c currently uses NO RCU, and RCU is not initialized in a
bare proc/loop program.

## Options

### A. Full RCU rework (lock-free reads)
- Make t->slots an _Atomic array; grow = alloc-new + copy + publish
  (release) + xtc_rcu_retire(old_array).
- Route __proc_free through xtc_rcu_retire so a reader inside a read
  side can load+refcount+recheck safely.
- Every __resolve caller (xtc_send, xtc_proc_wake, links/monitors)
  brackets with xtc_rcu_read_lock/_unlock.
- REQUIRES xtc_rcu_init() in every proc-using program's startup (new
  hard dependency for the whole messaging core) and a DST test that
  plants a resume-vs-exit UAF and proves the simulator catches it.
- Biggest win (reads take zero locks) but the biggest blast radius:
  it touches the busiest struct in the library and adds an init-order
  dependency + a read-side bracket to every send/wake.  High risk of a
  lost-wakeup or init-ordering regression.  Its own careful session.

### B. Stripe t->lock (CHOSEN for the near-term)
Replace the single per-table mutex with a small fixed array of stripe
mutexes (like xtc_chash's 64 stripes), keyed by local_id.
- Lookups and slot-alloc for DIFFERENT procs take DIFFERENT stripes ->
  the per-carrier serialization becomes N-way parallel.  For the PG
  workload (many distinct target procs per carrier) this removes almost
  all the contention.
- Array realloc stays safe the same way chash's grow does: a grow
  claims ALL stripes in ascending order (rare -- only when a loop's
  proc count doubles past its cap: 16, 32, 64...; ~never in steady
  state after warmup), so no reader can be mid-index during the
  realloc.  Better: switch grow to alloc-new + copy + publish + free
  under all-stripes (never realloc-in-place a pointer a stripe reader
  holds), matching chash exactly.
- Proc free is unchanged (still under a stripe lock, so the
  load+refcount is still atomic vs teardown -- teardown takes the SAME
  stripe as the lookup for that local_id).  NO RCU, NO new init
  dependency, NO read-side brackets.  Reads are still locked, but on N
  locks instead of 1.
- ~40-line change, bounded, testable with a concurrent stress + TSan.
- Does NOT make reads fully lock-free (option A does), but recovers the
  dominant serial section, which is what the PG measurement asks for.

## Decision
Do B now (bounded, correct, low-risk, directly attacks the measured
serial section).  Keep A as a future option ONLY if a measurement
shows the striped lock itself is still the ceiling (unlikely for the
fiber-per-session pattern, where targets are well-distributed across
local_ids).  Ship B with a concurrent stress test (many fibers, disjoint
+ overlapping targets) + TSan-clean; hand the PG team a build to
re-measure.

## Correctness of B (the one subtle rule)
The stripe for a given proc MUST be derived from its local_id ONLY
(stripe = local_id & (NSTRIPES-1)), NOT from anything array-relative,
so that a lookup and the matching teardown/detach for the same proc
always take the SAME stripe -- otherwise the "see-live-and-pin OR
see-NULL" atomicity is lost.  Grow takes all stripes ascending (the
sole multi-stripe hold -> the one deadlock-avoidance ordering rule),
exactly as chash does.
