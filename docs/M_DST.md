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
park/wake replay), test_sim_fault (seeded fault-schedule replay).

Cross-cutting risk: undeclared nondeterminism (any rand(), un-seeded
_Thread_local, hash-of-pointer iteration order leaking into a scheduling
decision).  The trace-diff harness is the detector; a Phase-0 grep is
the prophylactic.  A CI sim job that hangs/times out catches a future
blocking primitive added without a sim shim.
