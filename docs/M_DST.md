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
- Phase 7: machine-death + swarm fleet (test_sim_machine_death: a
  seeded xtc_exit_pid kill mid-run, exit propagation to linked/monitored
  peers + deterministic supervisor restart, quiescence + replay;
  test_sim_swarm: a shardable seed sweep over partition + latency +
  buggify + machine-death, bounded default and a manual 100k+ sweep).
  DONE (surfaced + fixed a net-latency deferred-delivery use-after-free;
  see the swarm entry under FoundationDB parity).

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
slice of storage-engine DST), test_sim_machine_death (a seeded
xtc_exit_pid kill mid-run: exit propagation to a linked ('E') +
monitored ('D') peer, deterministic one_for_one supervisor restart of
the killed child, quiescence + replay), test_sim_svr (the L4 gen_server
svr.c: seeded call/cast sequences to an accumulator server, replies are
running partial sums with no lost/double update, quiescence + replay --
surfaced + fixed a real xtc_svr_join use-after-free, see below),
test_sim_chan (the L3 channels chan.c beyond mpsc: mpmc exactly-once
delivery, watch no-regress latest-value, broadcast full-ordered
delivery, all replayed), test_sim_buggify3 (the two newest Buggify
sites -- chan.mpmc.spurious_full + svr.recv.delay_dispatch -- progress +
activation + replay + disabled=>zero), test_sim_crash_recover (the WAL
crash-recovery capstone, see below), and test_sim_swarm (a shardable
seed sweep combining partition + latency + buggify + machine-death,
bounded 300-seed default and a manual 100k+ sweep).

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
real runtime code), the L4 gen_server (svr.c: seeded call/cast
sequences dispatched by a server proc, deterministic reply values +
quiescence + replay -- test_sim_svr), the L3 channels beyond the mpsc
buggify site (chan.c mpmc / watch / broadcast: exactly-once delivery,
no-regress latest-value, full ordered broadcast, all replayed --
test_sim_chan), and -- a first slice of storage-engine DST -- the
sqlxtc buffer manager (the storage concurrency layer: fixes, striped
page-table locks, cooling-stage eviction, and background cool/flush all
run under the deterministic scheduler with seeded page-I/O completion
ordering, and replay).  WAL + crash-recovery under DST is DONE (the
capstone, test_sim_crash_recover; see below).

Still OUTSIDE sim's reach BY DESIGN (not a coverage gap to close):

- RCU (rcu.c) uses XTC_THREAD_LOCAL per-thread reader state and
  sched_yield() in synchronize().  Under the single-thread sim every
  fiber shares ONE __rcu_self slot (so overlapping read-sides from
  different fibers would corrupt each other's nesting count) and
  sched_yield() does not yield to the DST scheduler (so a writer
  spinning in synchronize() would never let a reader advance the epoch).
  Bringing RCU under DST would require a per-FIBER reader slot and a
  fiber-yield shim in synchronize() -- a real rcu.c change, deferred.
  RCU's thread-path correctness stays the domain of TSan / stress tests.
- The semaphore / gate / barrier primitives (sync.c) block a waiter in
  raw pthread_cond_wait with NO fiber-yield path (unlike xtc_amutex /
  xtc_arwlock, which park the fiber -- covered by test_sim_latch).  A
  fiber that must block on a sem/gate/barrier under the single-thread
  sim would freeze the whole scheduler (no other OS thread can signal
  it).  Only the NON-blocking variants (xtc_sem_try_acquire, etc.) are
  sim-safe today; the blocking paths need the same one-line "yield-and-
  recheck if on a fiber" shim the roadmap notes for barrier/sem, which
  is deferred.

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
  occurrences.  Now planted at FIVE sites: proc.mbox.spurious_full
  (xtc_send reports a soft-cap full early), chan.mpsc.spurious_full
  (xtc_chan_mpsc_try_send reports full with room to spare),
  sched.steal.skip_near (the work-stealing scheduler skips a NUMA-near
  victim that has stealable work), chan.mpmc.spurious_full
  (xtc_chan_mpmc_try_send reports full with room to spare -- the mpmc
  twin of the mpsc site), and svr.recv.delay_dispatch (the gen_server
  yields AFTER receiving a message but BEFORE dispatching it -- the
  message is in hand, not re-queued, so it cannot be lost; a legal
  pessimal delay that lets other procs interleave in the recv/dispatch
  window, and MAY push a slow call past its caller's timeout, a legal
  XTC_E_AGAIN outcome).  test_sim_buggify covers the mailbox site;
  test_sim_buggify2 covers the channel(mpsc) + steal sites over a
  work-stealing task workload; test_sim_buggify3 covers the two new
  sites (chan.mpmc.spurious_full over an mpmc producer/consumer workload
  asserting every item is still delivered exactly once, and
  svr.recv.delay_dispatch over a gen_server call workload asserting
  replies+timeouts == total with no lost/duplicated reply).
  Deterministic + replayable; off in production and when disabled.
  Expand by planting more sites in the WAL / buffer-pool / recovery
  paths.

- gen_server under DST (DONE, test_sim_svr): the L4 gen_server (svr.c)
  is an xtc_proc that dispatches each envelope to handle_call /
  handle_cast; an in-proc call routes its reply back through the
  caller's mailbox by tag (xtc_recv_match), so BOTH the server's recv
  loop and the caller's reply wait PARK the fiber and re-run under the
  seeded scheduler -- exactly the call/reply interleavings a race hides
  in.  N client procs across loops issue seeded call/cast sequences to
  an accumulator server; the test asserts (a) QUIESCENCE, (b) the
  gen_server invariant (each delivered message updates the accumulator
  exactly once, so every call reply is a running partial sum <= the
  grand total -- no lost/double update, exactly one reply per call),
  (c) byte-identical REPLAY (reply multiset + order hash + sim state
  hash), and (d) a different seed reorders the replies while holding the
  invariant.  The server's async xtc_svr_stop is drained the FDB way:
  a winder proc issues the stop INSIDE the sim run (the server exits its
  recv loop and signals its 'stopped' notify, the run drains and
  quiesces), then the handle is reclaimed with a NON-blocking
  xtc_svr_join(svr, 0) AFTER the run returns -- a blocking join would
  pthread_cond_wait and freeze the single sim thread (sem/gate/notify
  have no fiber-yield shim), mirroring test_sim_machine_death PART B's
  supervisor teardown.

  A REAL DEFECT this surfaced (and fixed) in svr.c: xtc_svr_join with a
  finite timeout freed the server struct UNCONDITIONALLY -- it ignored
  the xtc_notify_wait result.  If the deadline expired while the server
  was still running its recv loop, the free happened out from under the
  live server and its next read of s->stop_requested was a heap-use-
  after-free.  Found by test_sim_svr + test_sim_buggify3 under ASan (the
  deterministic scheduler + a slow-server buggify made the timeout race
  reliably reproducible; confirmed with a minimal ASan reproducer that
  joins with a short timeout while never stopping the server -- no
  buggify involved).  This is a real production API hazard, not sim-
  specific.  Fix: xtc_svr_join now reclaims ONLY when the wait confirms
  the server stopped (XTC_OK); on timeout it returns XTC_E_AGAIN and
  leaves the struct intact so the caller can join again.  A blocking
  join (timeout < 0) waits until the server signals and always reclaims.
  make check's OTP/gen_server suite (which joins with a 1 s timeout
  after a prompt stop -> XTC_OK -> free) is unaffected.

- Channels under DST (DONE, test_sim_chan): the L3 channels (chan.c)
  beyond the mpsc buggify site.  Producers/consumers POLL the
  non-blocking try_send/try_recv/recv and xtc_yield (or, when a producer
  parks on a timer, xtc_proc_sleep so the virtual clock advances) back
  to the seeded scheduler, which owns the interleaving.  Three variants:
    * mpmc  -- P producers each enqueue a disjoint block into a small
      bounded channel; C consumers drain until closed+empty.  INVARIANT:
      every item is consumed EXACTLY ONCE (no drop, no duplicate); the
      delivery order is part of the replayable schedule.
    * watch -- a single-slot latest-value channel; a writer publishes a
      monotonic sequence, readers sample.  INVARIANT: a reader never
      sees an unpublished value or one that regresses below the last it
      saw.
    * broadcast -- one sender, R subscribers on a lossy ring (sized >
      the message count so a keeping-up subscriber never lags).
      INVARIANT: each subscriber sees the FULL published sequence in
      strictly increasing order (never an unpublished / out-of-order
      value).
  Each variant asserts quiescence + its delivery invariant + byte-
  identical replay (set/order hash + sim state hash) + a different seed
  reorders while holding the invariant.  Footprint is small (few
  producers/consumers, tiny counts, per-run free) so the suite stays
  memory-bounded.

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

Still to reach full FDB parity: (nothing structural remains from the
original gap list -- machine-death and the swarm/soak fleet below are
now DONE; the honest scope limits noted per-feature -- raw-socket
cross-machine transport, physical memory-ordering -- remain out of
sim's reach by design).

- Machine-death / proc-kill simulation (DONE, test_sim_machine_death):
  FoundationDB's "kill a machine mid-run" at the granularity xtc's sim
  models.  A seeded reaper proc, after a seeded virtual-time delay,
  kills a victim via xtc_exit_pid (proc.c) -- the BEAM-style async kill:
  the victim raises its exit at the next yield/recv and runs the real
  __notify_links_and_monitors path, so nothing is sim-specific except
  the SEEDED choice of victim + timing (drawn from the APP stream, which
  never perturbs the SCHED/STEAL streams, so a kill replays regardless).
  The test verifies the system reacts DETERMINISTICALLY on two fronts:
    * PART A -- exit PROPAGATION.  Workers across loops; an observer
      links AND monitors the victim.  When the reaper kills it the exit
      propagates cross-loop through __mbox_deliver: the observer receives
      the 'E' link-exit signal and the 'D' monitor DOWN.  The run reaches
      QUIESCENCE (no hang -- a killed proc must not stall the sim) and
      REPLAYS (signal count + order-sensitive hash + sim state hash).
    * PART B -- SUPERVISOR RESTART.  A one_for_one supervisor owns N
      permanent children across loops.  The reaper kills a seeded child
      mid-run; the supervisor observes the DOWN and RESTARTS it (the
      deterministic restart -- restart count replays).  A watcher-reaper
      then stops the supervisor after a settle and the run winds down;
      the supervisor handle is joined + freed so the struct is reclaimed
      (no leak under ASan -- see the wind-down note in Known gaps: the
      settle + stop + join drains the async shutdown before quiescence).
  A DIFFERENT seed kills a DIFFERENT victim/child at a DIFFERENT time and
  stays consistent.  Runs clean under ASan.

- Swarm / soak fleet (DONE default-bounded, large-sweep MANUAL,
  test_sim_swarm): a large, SHARDABLE seed sweep over the rich fault set
  the sim now models, extending test_sim_soak.  Each seed runs a mixed
  workload (cross-loop ping/pong + timer sleepers) under a SEEDED
  combination of the scenarios -- network partition, seeded delivery
  latency, Buggify, and a machine-death kill -- chosen from the seed
  itself, so the seed fully determines both scenario and schedule.  Per
  seed it asserts QUIESCENCE (rc == XTC_OK -- no hang / livelock /
  deadlock), the per-step structural invariants (xtc_sim_exec_run
  returns XTC_E_INTERNAL on violation), and REPLAY (two runs -> identical
  sim state hash + app result); across the sweep the seeds must explore
  many distinct schedules.  Memory discipline: every run builds a fresh
  exec, spawns a BOUNDED proc set, and frees all per-run state
  (exec_fini + partition/buggify cleared) so a large sweep stays
  memory-bounded (RSS flat across seeds -- measured ~2.4 MB at 3000
  seeds).  Invocation:
    * test_sim_swarm                 -- bounded default (300 seeds),
      the committed suite / CI job (~0.35 s, a few MB RSS).
    * test_sim_swarm <count>         -- sweep <count> seeds from base 0.
    * test_sim_swarm <count> <base>  -- shard: <count> seeds from <base>,
      disjoint per shard, for a nightly/manual 100k+ sweep (~90 s per
      100k shard).  This is the FDB "millions of seeds" fleet, run
      manually/nightly rather than in the fast committed suite.
  A REAL DEFECT the swarm surfaced (and fixed): combining machine-death
  with net-latency exposed a use-after-free in the sim net-latency
  DEFERRED-DELIVERY path (proc.c __mbox_deferred_run).  A cross-loop
  send under a latency window defers the delivery to now + a seeded
  latency, capturing the target's proc pointer; if the target EXITS
  (normally or killed) inside that deferral window it is reaped/freed,
  and the deferred callback then dereferenced the freed proc.  Fix: the
  deferred delivery now captures the target's PID, not a raw pointer,
  and re-resolves it via __resolve at fire time (the generation check
  also rejects a reused slot), DROPPING the message if the target is
  gone.  Sim-only (the whole net-latency block is gated on
  __xtc_sim_active()); production is unchanged.  Confirmed with a
  minimal ASan reproducer and the full swarm now runs clean under ASan.

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
  stepping until truly idle).  RESOLVED IN PRACTICE by
  test_sim_machine_death PART B: rather than change the scheduler, the
  test's reaper stops the supervisor after a bounded settle and the
  workload continues stepping until the supervisor's async shutdown
  drains, then joins + frees the handle -- so a supervisor-under-kill
  DST test reaches quiescence and runs clean under ASan.  (A general
  "drain async-service shutdown before declaring quiescence" scheduler
  option is still tracked as a nicety, but is no longer needed for a
  faithful supervisor DST test.)

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
