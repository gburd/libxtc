# Deterministic Simulation Testing (DST) -- design and roadmap

This document is the design plan for TigerBeetle/FoundationDB-style
deterministic simulation testing in libxtc: (seed + config) yields a
byte-identical, replayable execution that exercises the REAL multi-loop,
multi-OS-thread, work-stealing executor and the shared-everything,
fiber-yielding-latch data plane -- NOT a scoped-down single-loop model.

## The model: logical concurrency on one physical thread

You cannot have byte-identical replay AND literal hardware parallelism;
two cores committing stores in a nondeterministic order is the
definition of nondeterminism.  TigerBeetle and FoundationDB resolve this
by keeping LOGICAL concurrency (many flows interleaving at decision
points) and dropping PHYSICAL parallelism in the simulator.  libxtc can
do exactly this because its concurrency is already expressed as
cooperatively-scheduled fibers returning to a scheduler at well-defined
yield points.

In sim mode the N loops become N scheduler-visible runnable entities on
ONE OS thread.  A single seeded scheduler repeatedly: selects the next
loop to advance (seeded), runs exactly one __xtc_loop_step_once for it,
and repeats until quiescence or a step budget.  The identical
work-stealing (exec.c __xtc_exec_try_steal), cross-loop inbox
(loop.c __xtc_inbox_drain), and shared-latch (sync.c) code runs; only
WHICH runnable entity advances next becomes a seeded choice instead of
an OS race.

What is preserved: the interleaving granularity and the set of reachable
interleavings of the concurrency logic.  No concurrency code is bypassed
or simplified, and the seeded scheduler can explore MORE interleavings
than chance (it can preempt immediately after a steal CAS, or starve a
loop for thousands of steps).

What is changed: physical simultaneity becomes seeded logical
interleaving -- a reduction to the sequentially-consistent interleavings.
The interleavings it cannot produce are those requiring weakly-ordered
memory; those remain the domain of TSan / herd7.

## DOES / DOESN'T catch

CAUGHT: ordering/logic races, lock-discipline races (lost wakeup,
double-grant, FIFO violation), message races, use-after-free on
scheduling boundaries (run under ASan), liveness/deadlock, timer/timeout
ordering, IO-fairness starvation.

NOT CAUGHT: pure hardware memory-ordering races (missing/weak
memory_order, store-buffer reorder) -- the domain of TSan and targeted
herd7/litmus tests.  Real kernel-edge bugs only as faithfully as the SIM
backend models them.

This is the contract: sim DST owns the LOGICAL concurrency-correctness
space; TSan owns the PHYSICAL memory-ordering space.  Complementary;
neither subsumes the other.

## Seams (all already single-point in the tree)

- Clock: __os_clock_mono (os_time.c) -> a virtual clock g_vclock_ns,
  advanced by the scheduler to the next pending deadline.
- IO: the xtc_io_* vtable (xtc_io.h over struct xtc_io in io_int.h) ->
  a new XTC_IO_BACKEND_SIM backend (io_sim.c) whose xtc_io_poll does not
  block but returns scripted completions vs the virtual clock.
- PRNG: one seeded tree (splitmix64/xorshift64, the rng_next idiom in
  dio_sched.c) feeding per-loop and per-decision streams; route the
  steal-victim start (exec.c), round-robin placement, and lock victim
  (lock_mgr.c) through it.
- Scheduler decision points: which loop runs next, steal victim,
  run-queue pop order, latch grant order, mailbox/inbox delivery.

## Architecture

- src/evt/sim.c: xtc_sim_run(exec, seed, config, max_steps) replaces the
  per-loop worker threads (exec.c spawns one __os_thread_create per loop)
  with the single-thread seeded scheduler driving __xtc_loop_step_once
  (loop.c) -- the identical production code path.
- A virtual clock (CLOCK_REAL | CLOCK_VIRTUAL mode on __os_clock_mono).
- The parking primitives yield to the scheduler instead of blocking a
  pthread primitive: on a loop, contended waiters ALREADY park the fiber
  via xtc_yield (sync.c amutex/arwlock fiber paths, aio.c); in sim all
  logic runs inside fibers so that existing path is the live one.  Only
  primitives with no fiber path today (barrier/sem/notify thread-waits,
  inject.c pthread_cond_wait, lwlock slow-path cond_wait) need a one-line
  "yield-and-recheck if on a fiber" shim.  Plain uncontended mutexes
  (mbox_lock, inbox.lock) need no change on one thread.
- Config: compile-time --with-io-backend=sim builds io_sim.c +
  XTC_IO_BACKEND_SIM; runtime xtc_sim_run activates the virtual clock +
  seeds the PRNG.  Sim builds are test-only (the backend is single-per-
  binary by design).

## The one behavioral substitution

xtc_io_poll no longer blocks in sim (the scheduler owns blocking/clock
advance); plus a handful of "yield instead of cond_wait" shims.
Everything else -- deque, inbox, latch state machines, timer heap,
mailbox -- is reused unmodified.

## Checkers / tests

- xtc_sim_check(exec) after every step: run-queue/alive coherence, deque
  integrity (0 <= bottom-top <= CAP), task-state legality, latch
  invariants (no writer&&readers; no holder underflow), mailbox
  accounting, lost-wakeup detection (quiescent with alive>0 == deadlock).
  Run under ASan/UBSan so scheduling-boundary UAFs surface.
- Fault injection: reuse inject.c with a seeded "fault" PRNG stream;
  add io.sim.* points and a deque-CAS-window interposition point to
  exercise branches single-threaded execution never hits by chance.
- A DST test: (seed, cfg) -> run N steps asserting invariants ->
  replay same seed -> assert byte-identical trace + state hash.
- Trace record per step (loop_id, step_kind, vclock, prng_draws,
  alive_snapshot); two runs must produce identical traces.  First
  divergence names the leaking nondeterminism source.

## Roadmap

- Phase 0: PRNG tree + route the random draw sites through it. DONE.
- Phase 1: virtual clock seam. DONE.
- Phase 2: io_sim.c backend. DONE.
- Phase 3: sim scheduler (xtc_sim_exec_run) -- the real N-loop
  work-stealing executor run deterministically on one thread; cross-
  loop parking replays. DONE (validated under ASan).
- Phase 4: invariant checker (xtc_sim_check, run every step) +
  state hash (xtc_sim_state_hash) + replay-equality harness. DONE.
- Phase 5: seeded fault injection (xtc_sim_fault, dedicated FAULT
  stream so faults do not perturb the schedule). DONE.
- Phase 6: scale + soak (test_sim_soak: a configurable seed sweep over
  a mixed cross-loop ping/pong + timer-sleeper workload; asserts every
  seed reaches quiescence, replays identically, holds invariants, and
  that the sweep explores many distinct schedules).  DONE.

## A real defect Phase 6 surfaced

The soak's timer-sleeper workload immediately exposed a scheduler bug
that the yield/recv-only Phase 3-5 tests missed: __sim_loop_runnable
treated any loop with n_alive > 0 as runnable.  But an alive proc may be
PARKED (awaiting a timer / fd / cross-loop waker), so a loop whose only
proc was sleeping on a timer was reported runnable forever -- the
scheduler kept picking it, it made no progress, and the virtual clock
never advanced to fire the timer.  Result: xtc_proc_sleep (and any
timer park) never completed under sim (budget exhaustion / hang).

Fix: runnability is now the REAL ready-work signal -- a ready task on
the owner FIFO (q_head) or the stealable deque, a timer already due at
the current virtual time, or a pending cross-loop inbox -- never merely
n_alive.  When no loop is runnable the scheduler advances the clock to
the earliest pending deadline (making that timer due), and distinguishes
clean quiescence (no proc alive) from a DEADLOCK (procs alive but all
parked with no timer/inbox: no waker can arrive) -> XTC_E_DEADLK.  This
is exactly the class of liveness bug DST exists to catch.

Tests live in test/sim/ (run_sim_tests.sh on a --with-io-backend=sim
build; a sim-dst CI job runs them every push): test_sim_rng (PRNG +
virtual clock, in the ordinary make check), test_sim_sched (multi-loop
deterministic scheduler + replay), test_sim_pingpong (cross-loop
park/wake replay), test_sim_fault (seeded fault-schedule replay),
test_sim_soak (seed sweep + invariants + schedule exploration),
test_sim_critsec (seeded critical-section fault points), test_sim_latch
(the fiber-yielding latches xtc_amutex/xtc_arwlock: mutual exclusion +
rwlock exclusivity + no-torn-read under contention, replayed),
test_sim_partition (a seeded cross-loop network partition: cut groups
drop messages, connected groups deliver, the run quiesces without a
partitioned-peer deadlock, and replays identically), test_sim_lockmgr
(the heavyweight lock manager lock_mgr.c: contending fibers park in the
wait loop and are re-granted on release -- all acquire/release pairs
complete with no hang, and a deliberately constructed cycle is detected
with exactly one deadlock victim; both replay from seed),
test_sim_bufmgr (the sqlxtc STORAGE-ENGINE concurrency layer -- the
LeanStore-style buffer manager, examples/06_sqlxtc/bufmgr.c -- driven
by xtc_sim_exec_run: N worker fibers across N loops pin/read/verify a
shared page set against a small pool that evicts, the page provider
cools + flushes on the virtual clock, and page I/O completes via the
sim backend's deferred, seeded-latency AIO; the run reaches quiescence
(the last worker stops the periodic provider so its timer stops spinning
the clock), every pinned read matches its canonical content, and the run
replays byte-identically -- same content + engine state hash -- a first
slice of storage-engine DST).

## Feature coverage progress (toward modelling all of libxtc)

Goal: the DST sim should drive every libxtc concurrency feature so a
green sim sweep is evidence of production readiness.  Covered so far:
the scheduler core (loop/exec/proc/task placement + work stealing),
cross-loop messaging (send/recv mailbox park/wake), the virtual clock
(xtc_proc_sleep / timers), seeded fault injection + critical-section
fault points, the fiber-yielding latches (xtc_amutex, xtc_arwlock:
mutual exclusion + rwlock exclusivity + no-torn-read under contention),
simulated I/O faults (deferred seeded AIO completions + short/EIO
faults), Buggify (FoundationDB-style pessimal-path injection in
real runtime code), and -- a first slice of storage-engine DST -- the
sqlxtc buffer manager (the storage concurrency layer: fixes, striped
page-table locks, cooling-stage eviction, and background cool/flush all
run under the deterministic scheduler with seeded page-I/O completion
ordering, and replay).  WAL + crash-recovery under DST remains the
capstone (see below).

## FoundationDB parity

Toward FDB-class DST, in addition to the seeded scheduler / virtual
clock / replay / invariant checks already in place:

- Simulated I/O fault injection (DONE, test_sim_iofault): deferred
  seeded AIO completion ordering + short-transfer / EIO faults.
- Simulated network partition + message latency (DONE,
  test_sim_partition): a seeded, deterministic model of a partitioned /
  lossy / delayed network at the cross-LOOP message granularity the sim
  models.  xtc_send between procs on different loops routes through
  __mbox_deliver (proc.c) -- the single cross-loop delivery seam -- and
  three knobs hook it, all OFF by default (no behaviour change in
  production or in a normal sim run):
    * xtc_sim_partition_set(src_loop_id, dst_loop_id, blocked): set/clear
      one directed edge in a partition matrix indexed by pid.loop_id
      (== exec_id + 1; 0 == standalone).  A blocked edge makes
      __mbox_deliver DROP the message via the sender's EXISTING soft-full
      path (XTC_E_AGAIN) -- the sender already handles that return, so a
      partitioned peer never deadlocks the sim (the DST test asserts
      clean quiescence, not XTC_E_DEADLK).
    * xtc_sim_partition_isolate(loop_id): cut a loop off from every other
      loop in both directions (a fully-partitioned / dead-machine peer);
      same-loop delivery is left intact.
    * xtc_sim_partition_clear(): heal the network (clear the matrix,
      disable partitioning, clear the latency window).
    * xtc_sim_net_latency(min_ns, max_ns): defer each surviving cross-loop
      delivery to now + a seeded latency in [min,max] drawn from the IO
      stream (not a new PRNG stream, so enabling latency does not perturb
      the schedule).  The delivery is enqueued on the TARGET loop's sim
      event store (via __xtc_io_sim_defer_cb) at a virtual-time deadline;
      __xtc_io_sim_next_due already reports it, so the scheduler advances
      the clock to it and an in-flight delayed message is never mistaken
      for a deadlock.  Delivery ORDER across concurrent sends thus becomes
      part of the replayable schedule.
  test_sim_partition splits 4 loops into two groups A={0,1}, B={2,3},
  cuts every cross-group edge, and asserts: (a) within-group cross-loop
  messages still deliver (4 of 12 sends land), (b) the run reaches
  quiescence (no hang -- a partitioned peer cannot receive its cross-
  group messages yet does not deadlock), (c) the seeded run REPLAYS
  identically (arrival count + order hash + sim state hash), and (d) with
  the partition cleared all 12 messages deliver.

  HONEST LIMITATION: this covers the IN-PROCESS cross-loop message path
  only.  xtc's real cross-MACHINE transport is raw sockets (io_net.c),
  which cannot run under the single-thread sim (a blocking recv/send on a
  real fd has no fiber-yield seam and the sim I/O backend does not model
  a socket wire).  Simulating raw sockets / TLS deterministically is a
  separate, much larger effort and is NOT modelled here; the partition /
  latency knobs act at loop granularity, which is exactly what xtc's sim
  faithfully represents.
- Buggify (DONE, test_sim_buggify): xtc_sim_buggify(name) is a named
  point in REAL runtime code that, once-per-run-per-site (a seeded coin
  cached on first reach), lets the code take a legal-but-pessimal path;
  combined with a per-call xtc_sim_fault coin it fires on a fraction of
  occurrences.  Now planted at THREE sites: proc.mbox.spurious_full
  (xtc_send reports a soft-cap full early), chan.mpsc.spurious_full
  (xtc_chan_mpsc_try_send reports full with room to spare), and
  sched.steal.skip_near (the work-stealing scheduler skips a NUMA-near
  victim that has stealable work).  test_sim_buggify covers the mailbox
  site; test_sim_buggify2 covers the channel + steal sites over a
  work-stealing task workload.  Deterministic + replayable; off in
  production and when disabled.  Expand by planting more sites in the
  WAL / buffer-pool / recovery paths.

- Storage-engine concurrency under DST (FIRST SLICE DONE,
  test_sim_bufmgr): the sqlxtc buffer manager -- the storage engine's
  concurrency layer -- runs under xtc_sim_exec_run.  A scaled-down
  test_bufmgr_mt workload (8 worker fibers x 4 loops over a 32-frame pool
  holding 256 pages, so eviction churns) pins/reads/verifies pages while
  the page provider cools + flushes; page reads/writes complete through
  the sim I/O backend's deferred, seeded-latency AIO (faults enabled for
  latency only -- an injected EIO would be a spurious corruption on a
  read-back-what-you-wrote workload).  It asserts (a) QUIESCENCE
  (xtc_sim_exec_run == XTC_OK -- no hang; the last worker calls
  bm_provider_stop so the provider's periodic timer stops advancing the
  virtual clock forever, which would otherwise be XTC_E_AGAIN), (b) data
  CONSISTENCY (no torn pages / verification mismatch, plus a final
  single-threaded sweep -- the same invariant test_bufmgr_mt checks),
  (c) REPLAY (same seed twice -> identical content hash AND engine state
  hash), and (d) a DIFFERENT seed -> a different schedule (state hash
  differs) with still-consistent data.  The bufmgr already parked fibers
  cleanly under sim -- its pool locks are audited-released before any
  xtc_aio, its provider stops deterministically, and eviction has no
  unseeded external nondeterminism -- so it reached quiescence and
  replayed without a bufmgr change (the test compiles bufmgr.c into the
  sim harness; src/ is untouched).  This is a FIRST SLICE only: it
  exercises the concurrency layer (fixes, striped page-table locks,
  cooling-stage eviction, background writeback), NOT durability.

Still to reach full FDB parity: machine-death simulation (kill a loop's
procs mid-run) and a swarm/soak fleet (millions of seeds).

WAL crash-recovery under DST: DONE (test_sim_crash_recover).  N worker
fibers across several loops commit transactions through xstore + the
group-commit WAL under xtc_sim_exec_run with seeded I/O latency; a
seeded crash point (drawn from XTC_SIM_RNG_FAULT, tripped by a shared
commit-attempt counter) calls xtc_exec_stop to halt the run mid-
workload with the buffer pool unflushed (data file empty, WAL-only
durability).  The WAL is then cut at the durable frontier
(durable_lsn -- only fsync-confirmed records survive, the conservative
deterministic crash boundary), a fresh B-tree is recovered via
xstore_recover, and the test verifies DURABILITY (every acked txn fully
present), ATOMICITY (both rows of a 2-row txn or neither -- no torn
txn), and no fabricated/leaked row.  Each run executes in a fresh child
process (FDB fork-per-run discipline) so the seed alone determines the
outcome.  A 40-seed sweep crashes at many points (before any commit,
mid-workload, at clean drain; acked 0..60 of 64) and EVERY seed
recovers to exactly its own durable-commit set and replays
byte-identically.  No WAL/recovery bug was found -- recovery restores
exactly the durable-commit set.  (Fork-per-run also revealed that
process-global engine state, chiefly the monotonic MVCC commit clock,
accumulates across in-process runs and perturbs the schedule; fork
isolation fixes replay with zero engine changes -- a note for any
future single-process crash-sweep.)
(Network-partition simulation is now DONE at loop granularity -- see the
simulated network partition entry above.)

Known gaps and why:

- Work-stealing completion ORDER is now bit-identical under replay
  (FIXED 2026-07).  Discovered by test_sim_buggify2: when 400 plain
  tasks are spawned on one loop and stolen by three idle peers, an
  ORDER-SENSITIVE hash of the completion sequence diverged across two
  runs of the SAME seed IN THE SAME PROCESS (the completion SET and the
  buggify activation count always replayed; a commutative hash was
  stable; and two SEPARATE processes each produced the same value).
  Root cause: the leak was NOT in the steal path (the two-pass victim
  walk, the seeded STEAL/SCHED streams, or a deque top/bottom snapshot
  were all deterministic).  It was leaked per-thread state ACROSS runs.
  __xtc_loop_step binds __xtc_current_loop to the loop it steps -- the
  DST scheduler multiplexes N loops on one thread, so each step must
  rebind it -- but xtc_sim_exec_run did NOT restore it on return (unlike
  xtc_loop_run, which saves/restores).  On return the calling thread's
  binding dangled at the last-stepped loop.  A SECOND sim run in the
  same process then saw a non-NULL binding on entry; when malloc reused
  a freed loop's address for the new run's loop-0 (observed: identical
  address across runs), a spawn-from-caller took the
  __xtc_current_loop == loop DIRECT-enqueue path (filling the deque to
  256 immediately) instead of the cross-loop inbox PUBLISH path the
  first run took (deque empty until drained).  That different initial
  deque distribution made a different set of loops runnable at step 0
  (peer-stealable vs not), so the seeded scheduler produced a different
  -- still valid -- steal interleaving.  Fix: xtc_sim_exec_run now saves
  __xtc_current_loop on entry and restores it on every exit path, so
  each run starts from the identical binding.  Sim-only (the function
  only runs under sim); production current-loop handling is unchanged.
  test_sim_buggify2 now hashes the completion sequence ORDER-sensitively
  and asserts it replays; the full sim suite (11 tests) replays, incl.
  under ASan+UBSan.

- Lock manager + deadlock detector (lock_mgr.c): DONE for the fiber path
  (test_sim_lockmgr), thread path unchanged.  A contended xtc_lock_get
  now checks __xtc_current_task(): a caller running inside a fiber on a
  loop PARKS -- it arms a waker on its lock_entry, sets is_fiber, drops
  the partition lock, and xtc_yield()s to the loop (with a park-on-timer
  when the timeout is positive, cancelled on exit so no orphan timer
  advances the sim clock) -- and is re-granted when a release, downgrade,
  release_all, or deadlock-victim abort wakes its waker.  This mirrors
  the xtc_amutex fiber-park discipline in sync.c exactly.  A caller NOT
  on a loop (cur == NULL: OS threads, the blocking pool, tooling) takes
  the ORIGINAL pthread_cond_wait / pthread_cond_timedwait path
  byte-for-byte -- the fiber path is purely additive and gated on
  __xtc_current_task() != NULL, so the storage engine's blocking-pool
  use is unaffected.  The detector's seeded LOCKVIC victim stream makes
  the victim choice replay; a DST test drives DETECT_ON_BLOCK
  (synchronous, no background thread) since the periodic detector thread
  cannot run on the single sim thread -- the production PERIODIC path is
  retained unchanged for threaded use.

- Supervisor restart logic (sup.c): the restart behaviour itself runs
  on a loop and IS deterministic under the sim (verified locally: a
  child crashing N times is restarted N times, replayed identically).
  But a supervisor's wind-down is ASYNC (xtc_sup_stop kicks the proc;
  the self-free happens on a later mailbox-poll), and the sim scheduler
  declares quiescence before that async shutdown drains -- so the
  supervisor struct is not reclaimed within one sim run (a leak under
  ASan that does NOT occur under the real xtc_loop_run, which keeps
  stepping until truly idle).  A faithful supervisor DST test needs the
  sim scheduler to drain async-service shutdown (continue stepping a
  bounded number of steps after the last app proc exits, until the
  service procs reach their at-exit free) before declaring quiescence.
  Tracked; not shipped to avoid a leaky test.

- File AIO under the sim I/O backend with seeded completion ordering
  (the XTC_SIM_RNG_IO stream): DONE.  xtc_sim_io_faults_enable(lat_min,
  lat_max, fault_pct) makes the sim backend DEFER each file-AIO
  completion to now + a seeded latency (so the completion ORDER across
  concurrent ops is part of the replayable schedule and the fiber
  genuinely parks) and optionally inject a seeded fault (a short
  transfer or an EIO).  Off by default (inline completion).  Covered by
  test_sim_iofault: 12 workers x 3 deferred AIO ops, seeded faults
  injected, completion order + fault pattern replay identically.

  Wiring this exposed and fixed TWO real latent bugs: (1) __xtc_loop_step
  did not bind __xtc_current_loop, so a fiber doing xtc_aio_* under the
  sim scheduler saw a NULL loop and wrongly took the off-loop blocking
  path (a hang); the step now binds it, matching production wake routing.
  (2) reporting a bare sim wakeup flag as pending work kept a loop
  perpetually runnable after its work drained, so the scheduler never
  reached quiescence; the wakeup flag is now correctly treated as
  redundant with the XTC_INB_WAKE inbox message the sim scheduler
  already observes.

Cross-cutting risk: undeclared nondeterminism (any rand(), un-seeded
_Thread_local, hash-of-pointer iteration order leaking into a scheduling
decision).  The trace-diff harness is the detector; a Phase-0 grep is
the prophylactic.  A CI sim job that hangs/times out catches a future
blocking primitive added without a sim shim.
