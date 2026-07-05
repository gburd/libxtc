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
  Run under ASan/UBSan so scheduling-boundary UAFs surface.  As of
  2026-07 the implemented per-step checker (src/evt/exec.c) also asserts:
  n_timers within [0, cap_timers] (a heap push never overran its backing
  array), the timer min-heap ROOT invariant (timers[0] <= its two heap
  children, so the earliest deadline is really at index 0 -- what the
  scheduler reads to advance the virtual clock), slow-path FIFO
  run-queue coherence (q_head NULL iff q_tail NULL -- a half-cleared
  queue drops or duplicates a ready task), and a non-negative recycled-
  task free-list count.  All cheap (a handful of loads per loop) so they
  run every step.
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
delivery, all replayed), test_sim_sync (the blocking M9 sync
primitives sync.c -- semaphore / barrier / gate -- driven under the
new fiber-park path: no over-admission past a counting semaphore's
count, all barrier parties released together, gate open/close/drain
correctness, all replayed; a sync.sem.spurious_timeout buggify holds
the invariant), test_sim_reg (the process registry reg.c:
register/whereis/unregister races across loops resolve
deterministically -- at-most-one holder per name, no lost/duplicate
registration, exact unregister, replayed; a reg.whereis.transient_miss
buggify exercises the caller retry), test_sim_lwlock (the M13b
lightweight lock lock_lw.c: contending fibers park in the acquire
slow path and are re-granted on release -- EXCLUSIVE mutual exclusion
+ no lost update, SHARED no torn read + writer-exclusive with
concurrent readers, replayed; needs a fiber-park shim, see below),
test_sim_lrlock (the wait-free-read Left-Right lock lock_lr.c:
concurrent readers never observe a torn value under a mutating writer,
publish is atomic, replayed -- NON-blocking, no shim), test_sim_mctx
(hierarchical memory contexts mctx.c: byte/chunk accounting is exact,
reset clears + keeps a context alive, destroy cascades children +
fires every before-destroy cleanup once with no leak, replayed --
NON-blocking, no shim), test_sim_slab (the slab + magazine allocator
slab.c: fibers alloc/stamp/verify/free a shared cache asserting NO
double-alloc + NO leak (n_inuse 0) + balanced alloc/free, replayed --
NON-blocking, no shim), test_sim_pdict (the per-process dictionary
pdict.c: put/get/erase/clear across yields stay correct AND ISOLATED
between procs -- a get resolves only to the calling proc's own value
-- replayed, NON-blocking, no shim), test_sim_stats (the
per-CPU-sharded counters/gauges/histograms stats.c: a shared counter
read == the exact sum of all fibers' increments, gauge net exact, hist
count exact + quantile in range, replayed; totals schedule-independent
-- NON-blocking, no shim), test_sim_buggify3 (two Buggify
sites -- chan.mpmc.spurious_full + svr.recv.delay_dispatch -- progress +
activation + replay + disabled=>zero), test_sim_buggify4 (the four newest
scheduler/AIO-path Buggify sites -- timer.fire.late,
sched.inbox.drain_one_fewer, sched.runq.defer_ready, io.aio.slow_completion
-- progress + activation + replay + disabled=>zero over a ping/pong +
timer-sleeper and a file-AIO workload), test_sim_torn (the TORN/CORRUPT-
write fault class -- checksummed pages under torn-write + corrupt-read
injection: every corruption caught by the checksum with zero silent bad
data, every page recovered by rewrite, replayed), test_sim_crash_recover (the WAL
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
test_sim_chan), the blocking M9 sync primitives (sync.c semaphore /
barrier / gate / notify: a fiber-park path lets a fiber blocking on one
PARK instead of freezing the sim thread -- no over-admission past a
counting semaphore's count, all barrier parties released together,
gate open/close/drain correctness, all replayed -- test_sim_sync), the
process registry (reg.c: register/whereis/unregister races across loops
resolve deterministically -- at-most-one holder per name, no
lost/duplicate registration, exact unregister, replayed --
test_sim_reg), the remaining ptc primitives -- the M13b lightweight
lock (lock_lw.c: a fiber-park shim lets a fiber blocking on a
contended acquire PARK instead of freezing the sim thread; EXCLUSIVE
mutual exclusion + no lost update, SHARED no torn read + concurrent
readers -- test_sim_lwlock), the wait-free-read Left-Right lock
(lock_lr.c: concurrent readers never torn under a writer, atomic
publish -- test_sim_lrlock), RCU epoch reclamation (rcu.c: a per-FIBER
reader slot -- keyed on __xtc_current_task() -- keeps overlapping
read-sides from different fibers isolated, and a cooperative-yield
synchronize() lets reader fibers run and drain instead of spinning
sched_yield(); no reader ever observes a freed node, every retired
node is reclaimed, replayed -- test_sim_rcu), hierarchical memory contexts (mctx.c:
exact byte/chunk accounting, reset-keeps-alive, destroy cascades +
fires every cleanup once, no leak -- test_sim_mctx), the slab +
magazine allocator (slab.c: no double-alloc, no leak, balanced
alloc/free -- test_sim_slab), the per-process dictionary (pdict.c:
per-proc isolation + exact erase/clear/count -- test_sim_pdict), and
the per-CPU-sharded counters/gauges/histograms (stats.c: counter read
== exact increment sum, gauge net exact, hist count exact --
test_sim_stats) -- all NON-blocking except lwlock, all replayed, and
-- a first slice of storage-engine DST -- the
sqlxtc buffer manager (the storage concurrency layer: fixes, striped
page-table locks, cooling-stage eviction, and background cool/flush all
run under the deterministic scheduler with seeded page-I/O completion
ordering, and replay).  WAL + crash-recovery under DST is DONE (the
capstone, test_sim_crash_recover; see below).

The L4 tnt (Tina) Isolate layer's deterministic actor core is also
covered (tnt.c -- test_sim_tnt).  tnt was designed for DST: a handler
returns a TRANSITION rather than issuing a syscall, so the shard
scheduler is a pure function of message arrival order.  Under sim the
shard parks via a sim-clock sleep instead of its real wake pipe
(ADDITIVE, gated on __xtc_sim_active(); the production shard loop is
byte-identical and test/tnt/test_tnt passes unchanged, ASan-clean).  A
purely message-driven driver (self-KICK messages sequence the scenario;
each phase advances only after observing the prior phase's effect)
exercises spawn into typed arenas, exactly-once mailbox delivery,
drop-on-full with MAILBOX_FULL feedback (flooding N into a cap-C mailbox
in one turn drops exactly N-C, nothing silently lost), generational
stale-handle rejection (a send to a torn-down slot's OLD handle returns
STALE_HANDLE, never mis-delivers to a reused slot), and same-shard
cross-type delivery -- across a seed sweep with a bit-identical result
fingerprint on replay.  A sim-visible shard-wake seam (the shard parks
on its own proc mailbox; shard_wake wakes it via xtc_send) brings
cross-SHARD messaging and wall-clock TIMERS under sim too, so the
test drives two shards with a cross-shard send and a timer redelivered
around it.  Only the socket courier I/O (raw recv/send) stays outside
sim by design (needs a real kernel).

Still OUTSIDE sim's reach BY DESIGN (not a coverage gap to close):

- RCU (rcu.c) is now DONE under DST -- test_sim_rcu.  The two blockers
  that used to sit here are closed PURELY ADDITIVELY, gated on
  __xtc_current_task() != NULL, with the production OS-thread path
  byte-identical (test/m13/test_rcu passes unchanged):

  1. The reader slot was a single XTC_THREAD_LOCAL __rcu_self.  Under
     the single-thread sim all fibers share one OS thread, so fiber A's
     read_lock (which publishes the global epoch into active_epoch)
     would collide with fiber B's -- corrupting the per-reader epoch
     synchronize() scans, and freeing a node a concurrent reader still
     holds.  FIX: on a fiber, the reader slot is keyed on the CURRENT
     TASK -- each fiber gets its own struct rcu_tls, held in a small
     xtc_task_t-keyed table inside rcu.c and still registered into the
     SAME global registry synchronize scans (only WHERE the "my slot"
     pointer lives changes).  Slots are reused by task pointer (the
     loop recycles task structs, so the table stays bounded by peak
     concurrent fibers, not total spawned) and freed at xtc_rcu_fini.

  2. synchronize() spun sched_yield() waiting for readers to drain.
     Under sim sched_yield does not reach the DST scheduler, so the
     writer fiber would spin forever and the readers never run to
     drain -- a hang.  FIX: on a fiber, synchronize() yields to the
     loop (xtc_yield, with __current_proc saved/restored across the
     yield) so reader fibers get scheduled and clear active_epoch.

  test_sim_rcu asserts NO reader observes a freed/torn node (a sentinel
  in each node, checked across a yield inside the read-side; a negative
  control that disables the re-keying trips a heap-use-after-free under
  ASan at the re-read), every retired node is reclaimed (freed count ==
  allocated), quiescence (rc == XTC_OK, no synchronize hang), and
  byte-identical replay from the seed with a different seed reordering
  but staying consistent.  (The sem/gate/barrier deferral that used to
  sit here is now CLOSED -- see test_sim_sync above.)  Every
  concurrency primitive in the tree is now under DST.

- tnt (Tina) Isolate layer -- deterministic actor core DONE
  (test_sim_tnt), and the two paths that used to sit OUTSIDE sim here
  are now CLOSED by a sim-visible shard-wake seam.  Under sim a shard
  parks on its own proc mailbox and shard_wake wakes it via xtc_send
  (the fully-modeled cross-loop park/wake path -- test_sim_pingpong),
  so a cross-shard deliver or a timer/courier completion makes the
  target shard runnable deterministically on the very next step rather
  than poking a self-pipe the sim cannot observe.  ADDITIVE, gated on
  __xtc_sim_active(); the production shard loop uses the pipe and is
  byte-identical (test/tnt/test_tnt passes unchanged, ASan-clean).

  Closing the seam surfaced a real latent bug, fixed in the same change:
  the ambient tnt API (xtc_tnt_send / xtc_tnt_spawn /
  xtc_tnt_register_timer / xtc_tnt_scratch_arena) read the current
  shard from a thread-local (tl_shard).  Under the single-thread sim all
  shard fibers share one OS thread, and tl_shard -- set once per shard
  at shard_main entry, not restored on yield -- can read STALE (a peer
  shard that ran and yielded left it pointing at itself).  So a
  cross-shard xtc_tnt_send that ran a peer fiber could leave a FOLLOWING
  xtc_tnt_register_timer reading the wrong shard and misrouting its
  courier onto the peer's loop + completion ring (the timer then fired
  on the wrong shard and its completion was dropped as a stale target).
  Fix: the ambient calls now derive the shard from the STACK-LOCAL
  per-turn frame (tl_frame->shard), which is always correct inside a
  handler and restored on return.  In production (one thread per shard)
  frame->shard == tl_shard, so it is a no-op there.  This is the same
  bug class as the RCU per-fiber-slot issue below.

  test_sim_tnt now drives TWO shards: a driver on shard 0 mixes
  self-KICK messages, a CROSS-SHARD send to a peer on shard 1, and a
  one-shot TIMER redelivered around that cross-shard send -- proving,
  across a seed sweep with bit-identical replay: spawn into typed
  arenas, exactly-once mailbox delivery, drop-on-full feedback (flood N
  into a cap-C mailbox drops exactly N-C), generational stale-handle
  rejection, cross-shard delivery, and one-shot timer firing on the sim
  virtual clock.  Only the socket courier I/O (raw recv/send) stays
  outside sim -- not-coverable-by-design (needs a real kernel).

- L4 orchestration + cross-primitive composition (DONE): the L4
  application/supervision layer and a multi-primitive composition are
  now under DST.

  * SUPERVISOR strategy matrix (test_sim_sup_strategy): beyond the
    single-child one_for_one restart that test_sim_machine_death
    already covers, this drives ONE_FOR_ALL (crashing one child
    restarts every child), REST_FOR_ONE (crashing child i restarts i
    and every child started after it, none before), the restart
    POLICIES (permanent restarts on any exit, transient only on
    abnormal exit, temporary never), and restart INTENSITY (more than
    max_restarts crashes in period_ns makes the supervisor give up and
    exit).  Crash timing is seeded, so the spawn counts and
    supervisor-alive outcome are a pure function of the seed and
    replay.

  * APPLICATION lifecycle (test_sim_app): a multi-loop xtc_app is
    brought up (xtc_app_create + xtc_app_start) and driven with
    xtc_sim_exec_run instead of xtc_app_run, so the whole lifecycle is
    seeded.  A server child registers a well-known name in the app
    registry; client children on other loops resolve it by name and do
    a request/reply; the app stops cleanly once they finish.  Proves
    the registry + cross-loop request/reply + ordered startup/shutdown
    interoperate across the app lifecycle, replayed.

  * BLOCKING offload (test_sim_blocking): the production offload path
    hands work to a real pthread pool (a pool worker runs on a real OS
    thread outside the sim -- not-coverable-by-design, like raw
    sockets).  What IS coverable is the caller-side contract; under sim
    xtc_blocking_run runs the work synchronously on the calling fiber
    (ADDITIVE, gated on __xtc_sim_active(); the same result the
    off-a-loop synchronous fallback already produces in production), so
    many fibers offloading concurrently each get their own fn(arg)
    result with no cross-talk or lost completion, replayed.

  * COMPOSITION (test_sim_compose): the highest-leverage test -- the
    lock manager, the sqlxtc storage engine (xstore + WAL), an mpsc
    channel, and cross-loop scheduling run TOGETHER in one seeded run,
    where cross-primitive bugs hide.  Supervised workers take an
    EXCLUSIVE lockmgr lock on a shared key, commit a uniquely-keyed row
    durably under the lock, release, then report the row on the
    channel; a collector drains the channel.  The global invariant --
    lockmgr mutual exclusion held (never two holders), every commit
    durable, every channel report collected, count == workers*quota --
    holds and replays across the whole primitive stack at once.

- Byte-STREAM connection abstraction (DONE, test_sim_stream): the piece
  the message-level partition sim does not model.  It does NOT
  reimplement kernel TCP (raw sockets stay outside sim); it models the
  ordered bidirectional byte-stream a connection presents ABOVE the
  socket, with a deterministic in-process wire (a pair of ordered
  byte-chunk channels) that fragments and paces each write by a seeded
  schedule.  A length-prefixed request/response protocol runs over it
  and the reader must reassemble frames across arbitrary chunk
  boundaries -- the exact frame-reassembly a real xtc_net_recv_frame
  consumer does -- proving ordered, lossless byte delivery under a
  fragmenting wire, replayed.

- OS SUBPROCESS lifecycle (DONE, test_sim_osproc): using FoundationDB's
  "process = in-process actor with a simulated lifecycle" pattern.  FDB
  never fork/exec's under simulation; its Sim2 models a process
  (ISimulator::ProcessInfo) as a cooperatively-scheduled actor with a
  simulated lifecycle (spawn / kill / reboot keyed by KillType) and
  pushes REAL subprocess spawning out to fdbmonitor, which the
  simulated code path never crosses.  xtc_osproc does the same: under
  sim the fn-callback child runs as an xtc_proc FIBER and its lifecycle
  -- running, exit-with-status, signalled termination, wait / try_wait
  / reap -- is modelled on the sim clock (ADDITIVE, gated on
  __xtc_sim_active(); the production fork path is byte-identical,
  test_osproc unchanged).  The exec (argv) path and the live control
  SOCKET have no in-process equivalent and decline with XTC_E_NOSYS
  under sim -- a consumer needing them is on the not-coverable
  real-kernel tier, exactly as FDB's real exec lives in fdbmonitor
  outside the simulator.  The common "run isolated work, collect its
  exit status" contract IS modelled and replays.

- CRASH RECOVERY under a MULTI-PRIMITIVE composition (DONE,
  test_sim_compose_crash): the FoundationDB signature test ("kill a
  process mid-transaction, verify the durability invariant survives"),
  now spanning the full stack.  The composition workload (lockmgr +
  xstore/WAL + channel + supervision) runs, and at a SEEDED step
  mid-workload the run crashes (xtc_exec_stop, pool unflushed); the WAL
  is cut at the durable frontier and replayed into a fresh tree.
  Invariant: every row whose COMMIT returned SX_OK before the crash
  (acked, observed while holding the lock) is present after recovery,
  and lockmgr mutual exclusion held up to the crash -- across the whole
  primitive stack, forked-per-run for exact replay of the process-global
  MVCC clock.

- RESOURCE governance (DONE, test_sim_res): many fibers across loops
  concurrently acquire/release a small-capped resource; the lock-free
  CAS accountant must hold used <= cap at every observation, conserve
  exactly (final used == 0), and account every attempt (successes +
  rejects == attempts) -- a lost-update or torn cap-check would show as
  a used>cap observation or nonzero final used.  Replayed.

  Still not-coverable-by-design in this area: the exec (argv)
  subprocess path (a real program image xtc_osproc would execvp -- no
  in-process equivalent), the live control-SOCKET request/reply over a
  real kernel socketpair, the raw-socket/TLS wire, and the blocking
  POOL thread -- a real child image, kernel socket, or OS thread runs
  outside the single-thread sim's control, so there is nothing
  deterministic to simulate (this is exactly why FDB keeps real exec in
  fdbmonitor).  The library-side completion-delivery logic around them
  (a fiber parking on an fd/AIO completion) is already exercised by
  test_sim_iofault.

## FoundationDB parity

Toward FDB-class DST, in addition to the seeded scheduler / virtual
clock / replay / invariant checks already in place:

- Simulated I/O fault injection (DONE, test_sim_iofault): deferred
  seeded AIO completion ordering + short-transfer / EIO faults.  The
  TORN/CORRUPT-write fault class (torn write persists a prefix but
  reports full success; corrupt read bit-flips a byte) is also modelled
  now -- see the torn-write entry below (test_sim_torn).
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
  combined with a per-call fault coin it fires on a fraction of
  occurrences.  Now planted at ELEVEN sites.  The FOUR ORIGINAL
  primitive-level sites: proc.mbox.spurious_full
  (xtc_send reports a soft-cap full early), chan.mpsc.spurious_full
  (xtc_chan_mpsc_try_send reports full with room to spare),
  sched.steal.skip_near (the work-stealing scheduler skips a NUMA-near
  victim that has stealable work), chan.mpmc.spurious_full
  (xtc_chan_mpmc_try_send reports full with room to spare -- the mpmc
  twin of the mpsc site), svr.recv.delay_dispatch (the gen_server
  yields AFTER receiving a message but BEFORE dispatching it -- the
  message is in hand, not re-queued, so it cannot be lost; a legal
  pessimal delay that lets other procs interleave in the recv/dispatch
  window, and MAY push a slow call past its caller's timeout, a legal
  XTC_E_AGAIN outcome), sync.sem.spurious_timeout (a fiber acquiring a
  semaphore with a finite timeout occasionally reports a spurious
  XTC_E_AGAIN even though the count was satisfiable -- a documented
  timeout outcome the caller must retry; a fresh per-call fault draw so
  a retrying caller succeeds), and reg.whereis.transient_miss
  (xtc_reg_whereis reports a registered name as transiently NOT FOUND --
  a lookup is a hint that may race a concurrent unregister, so callers
  already retry).

  The FOUR NEW SCHEDULER/AIO-PATH sites (planted 2026-07 to match FDB
  DENSITY -- pessimal paths in the REAL runtime the code already
  tolerates, each exercised for progress + activation + replay +
  disabled=>zero, and each VERIFIED not to perturb any other test's
  replay):
    * timer.fire.late (src/evt/loop.c, the due-timer drain) -- a due
      timer is fired one scheduler turn LATE (re-armed a bounded +1us
      later, at most ONCE per timer via a zero-initialised sim_late
      guard on struct xtc_timer, so a late fire cannot spin).  A timer
      firing late is always tolerated; the scheduler advances the
      virtual clock to the re-armed deadline and fires it then.
    * sched.inbox.drain_one_fewer (src/evt/loop.c, __xtc_inbox_drain) --
      the cross-loop inbox drain processes one FEWER message this turn,
      holding the tail message back (re-queued at the inbox front) for
      the next drain.  A held WAKE/PUBLISH is NOT lost (the loop stays
      runnable because inbox.head != NULL, so the scheduler steps it
      again and drains it): a legal one-turn delay.  Only fires when
      there is more than one message (so the loop still makes progress
      this turn).
    * sched.runq.defer_ready (src/evt/loop.c, step-1 run-queue) -- a
      popped ready task is DEFERRED one turn (re-enqueued; a DIFFERENT
      ready task runs instead) -- the local-run-queue twin of
      sched.steal.skip_near.  Exactly XTC_TASK_RESCHED, which the loop
      already tolerates.  Only fires when another ready task exists (so
      the loop still makes progress); if the re-pop returns the same
      task (it was the only one) it runs it -- cannot spin.
    * io.aio.slow_completion (src/io/io_sim.c, xtc_io_aio_submit) -- a
      deferred file-AIO completion is pushed an EXTRA +5us later.  The
      fiber is parked awaiting it and simply wakes later: a legal
      slow-disk delay.

  ISOLATION DISCIPLINE (why the four new sites do not desync any other
  test's replay -- the hazard that got a prototype slab.alloc site
  DROPPED): the buggify once-per-run coin AND each new site's per-call
  coin (xtc_sim_buggify_fault) now draw from a DEDICATED PRNG stream
  (XTC_SIM_RNG_BUGGIFY), NOT the FAULT stream the critical-section /
  fault-point tests replay against; and each per-call coin is reached
  ONLY behind XTC_SIM_BUGGIFY(name) && ... so it draws NOTHING when
  buggify is disabled (the && short-circuits).  A site on a hot shared
  path therefore adds draws only inside tests that ENABLE buggify, and
  those tests compare run-to-run (self-consistent replay), so replay is
  preserved by construction.  Confirmed empirically: the full sim suite
  (incl. test_sim_buggify / buggify2 / buggify3 / swarm, which enable
  buggify) replays byte-identically with the four new sites present.

  test_sim_buggify covers the mailbox site;
  test_sim_buggify2 covers the channel(mpsc) + steal sites over a
  work-stealing task workload; test_sim_buggify3 covers the
  chan.mpmc + svr.recv sites (chan.mpmc.spurious_full over an mpmc
  producer/consumer workload asserting every item is still delivered
  exactly once, and svr.recv.delay_dispatch over a gen_server call
  workload asserting
  replies+timeouts == total with no lost/duplicated reply);
  test_sim_buggify4 covers the FOUR new sites -- PART A drives a
  cross-loop ping/pong + timer-sleeper workload (reaching timer.fire.late
  / sched.inbox.drain_one_fewer / sched.runq.defer_ready) asserting every
  ping replies and every sleeper wakes despite the pessimal delays, and
  PART B drives a file-AIO write/read workload under seeded latency
  (reaching io.aio.slow_completion) asserting every op completes; both
  parts assert activation + byte-identical replay + disabled=>zero.  The
  sync.sem.spurious_timeout site is exercised by test_sim_sync (a
  semaphore-contention workload asserting no over-admission past the
  count despite the spurious timeouts, retried to completion), and
  reg.whereis.transient_miss by test_sim_reg (a register/whereis/
  unregister churn asserting every lookup eventually resolves to its
  own registration).  Deterministic + replayable; off in production and
  when disabled.

- TORN / CORRUPT-write fault class (DONE, test_sim_torn): the torn-page
  hazard FoundationDB models, added to the sim I/O backend alongside the
  short-transfer / EIO faults.  A short transfer reports AND moves fewer
  bytes (clean; the caller re-issues the remainder).  A TORN WRITE
  instead PERSISTS only a seeded PREFIX of the buffer (a strict prefix,
  at least the last byte lost) while REPORTING full success, and a
  CORRUPT READ bit-flips one byte in the returned buffer -- both leave
  latent bad bytes that only a CHECKSUM can catch.  Enabled with
  xtc_sim_io_corrupt_enable(pct); off by default; seeded on the IO
  stream so enabling does not perturb the schedule.  The submit path
  (io_sim.c) draws __xtc_sim_io_torn_prefix (how many bytes a torn write
  persists) and __xtc_sim_io_flip_byte (which byte a corrupt read
  flips).  test_sim_torn drives N fibers across loops, each owning a
  private page whose last 8 bytes hold an FNV-1a checksum of the rest:
  each fiber writes + fsyncs + reads-back + verifies, and on a checksum
  mismatch (a detected torn/corrupt page) REWRITES from the in-memory
  copy (the storage-engine recovery discipline).  It asserts (a) THE
  DURABILITY INVARIANT -- no corruption is EVER accepted silently (a
  page that passes the checksum but differs from what was written is
  counted as silent bad data and MUST stay 0), (b) PROGRESS -- every
  page eventually verifies (a rewrite recovers a torn page; the run
  quiesces), (c) LIVE INJECTION -- at least one torn/corrupt event is
  detected, (d) REPLAY -- the same seed corrupts the identical set of
  pages (byte-identical detected count + order-hash + sim state hash),
  and (e) a DIFFERENT seed corrupts a different set (a different state
  hash) while still never accepting silent bad data.  Exercised at the
  io_sim / self-contained-consumer level, NOT by editing
  examples/06_sqlxtc (a parallel agent owns that).  No bug found -- the
  checksum catches every torn write, and the rewrite recovers it.
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

- Blocking sync primitives under DST (DONE, test_sim_sync): the M9
  semaphore / barrier / gate / notify (sync.c) blocked a waiter in raw
  pthread_cond_wait with NO fiber-yield path (unlike xtc_amutex /
  xtc_arwlock).  A fiber-park path was added -- PURELY ADDITIVE and
  gated on __xtc_current_task() != NULL, exactly mirroring the
  xtc_amutex / lock-manager discipline: a caller running inside a fiber
  arms a waker, enqueues on a FIFO fiber wait queue, drops the internal
  lock, xtc_yield()s to the loop (parking on a timer when the timeout is
  finite, cancelled on exit so no orphan timer advances the sim clock),
  and re-checks its predicate on wake (a wake without the predicate met
  simply re-parks -- wake-and-recheck, not direct hand-off, since a
  count / generation is not a single ownable token).  A post / leave /
  close / signal (or the barrier's last arrival) wakes BOTH kinds of
  waiter (broadcast the condvar for threads, wake every queued fiber
  waker).  A caller NOT on a loop (cur == NULL: OS threads, the blocking
  pool, tooling) takes the ORIGINAL pthread_cond_wait /
  pthread_cond_timedwait path BYTE FOR BYTE -- test/m9/test_sync (the
  thread-path suite) still passes unchanged, incl. under ASan+UBSan.
  test_sim_sync drives three workloads across N loops: a counting
  semaphore contended by more fibers than its capacity (INVARIANT: peak
  concurrent holders <= CAP -- NO over-admission past the count; every
  worker acquires+releases exactly once), a barrier rendezvous of P
  fiber parties for R rounds (INVARIANT: no party PASSES round k until
  all P have entered it -- parties released TOGETHER), and a gate that a
  closer closes + drains while workers enter/leave (INVARIANT: no worker
  admitted AFTER close, drain returns with count 0).  Each asserts
  quiescence (rc == XTC_OK, no hang) + its invariant + byte-identical
  replay (app hash + sim state hash) + a different seed reorders while
  holding the invariant.  A sync.sem.spurious_timeout buggify (a fiber
  acquire with a finite timeout occasionally declines a satisfiable
  count and reports XTC_E_AGAIN -- a documented outcome the caller
  retries) is exercised for progress + activation + replay; the sem
  invariant holds under it.  No sync bug was found -- the clean
  primitives (sem/gate/barrier/notify) all park and re-grant correctly.

- Process registry under DST (DONE, test_sim_reg): the registry
  (reg.c, name -> xtc_pid_t under a single mutex) never blocks (no
  cond_wait), so it is sim-safe as-is -- no shim needed; the seeded
  scheduler simply owns the register-vs-register / register-vs-
  unregister / lookup-vs-both interleaving.  N worker fibers across N
  loops RACE to register their own pid under a shared name set, look it
  up, and unregister it.  INVARIANTS: for each name, register succeeds
  AT MOST once at a time (a duplicate returns XTC_E_INVAL -- no
  lost/duplicate registration); a whereis by the winner resolves to ITS
  OWN pid (DETERMINISTIC resolution -- never a stale/other pid); after
  the run the registry count is 0 (every registration was
  unregistered).  A second churn workload over private names asserts
  register/unregister pair up exactly.  Each asserts quiescence + the
  invariant + byte-identical replay (an order-sensitive resolution hash
  + sim state hash) + a different seed reorders while staying
  consistent.  A reg.whereis.transient_miss buggify (a registered name
  reported transiently NOT FOUND -- a lookup is a hint that may race a
  concurrent unregister) is exercised for progress + activation +
  replay; the caller retries and every lookup eventually resolves.  No
  registry bug was found.

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

CONCURRENCY-PRIMITIVE COVERAGE IS NOW COMPLETE: every
concurrency primitive in the tree is under DST -- the scheduler core,
cross-loop messaging, the virtual clock, the fiber-yielding latches
(amutex / arwlock), the blocking sync primitives (sem / gate /
barrier / notify), the lock manager, the lightweight lock (lwlock),
the wait-free-read lock (lrlock), RCU epoch reclamation (rcu),
memory contexts (mctx), the slab
allocator, the per-process dictionary (pdict), the stats
counters/gauges/histograms, the L3 channels, the L4 gen_server, the
process registry, plus the storage-engine slices (buffer manager, WAL
crash-recovery).  RCU -- the last primitive to land -- came under DST
via a per-FIBER reader slot (keyed on __xtc_current_task()) plus a
cooperative-yield synchronize() (see the RCU note above); no primitive
remains outside sim's reach.
The primitives needing a fiber-park shim (a fiber blocking on a
contended acquire PARKS instead of freezing the sim thread, gated on
__xtc_current_task() != NULL, production thread path byte-identical)
are: sem/gate/barrier/notify (sync.c), the lock manager (lock_mgr.c),
and the lwlock (lock_lw.c); everything else is NON-blocking and needs
no shim.

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
  latency, Buggify (which now activates the four new scheduler/AIO-path
  sites too), a machine-death kill, and TORN/CORRUPT-write injection
  (some seeds run a couple of extra AIO page verifiers under
  xtc_sim_io_corrupt_enable, asserting no torn page is ever accepted
  silently) -- chosen from the seed
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
  and asserts it replays; the full sim suite (38 tests) replays, incl.
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

- Lightweight lock (lock_lw.c): DONE for the fiber path
  (test_sim_lwlock), thread path unchanged.  The M13b lwlock is a
  multi-reader / single-writer lock over an atomic state word whose
  contended slow path is a pthread_cond_wait.  A fiber-park path was
  added -- PURELY ADDITIVE and gated on __xtc_current_task() != NULL,
  mirroring the sem / lock-manager discipline: a caller blocking on a
  contended xtc_lwlock_acquire from INSIDE a fiber arms a waker,
  enqueues on the lock's FIFO fiber wait queue, drops wait_mu, and
  xtc_yield()s to the loop; on wake it re-CASes the state word
  (WAKE-AND-RECHECK -- the lock is a shared count / exclusive bit, not
  a single ownable token, so a wake without the lock free simply
  re-parks).  A release wakes BOTH kinds of waiter (broadcast the
  condvar for threads, wake every queued fiber).  Two subtleties the
  DST test surfaced during development:
    * A DETACH-ALL wake (splice the whole fiber queue out on release,
      wake off-lock) LOST a wakeup once 3+ fibers contended: the
      winner drains, a loser re-parks WITHOUT re-enqueuing (it never
      left the acquire loop), so the next release found an empty queue
      and the loser hung (XTC_E_DEADLK).  Fix: the release wakes the
      queued wakers IN PLACE under wait_mu WITHOUT detaching -- each
      waiter removes itself only when it actually acquires -- so a
      re-parking loser stays queued for the next release.  Waking in
      place under the lock is safe because a woken fiber cannot
      re-acquire wait_mu (and thus cannot mutate the list) until the
      releaser drops it, and xtc_waker_wake only marks the task
      runnable (it does not run it inline).
    * NO spurious-contention buggify was planted in the acquire park
      loop: an uncontended acquire has no timeout, so a
      decline-and-re-park with no pending wake would hang (unlike the
      semaphore, whose caller retries on a finite timeout).  Per the
      "do not ship a racy park" discipline the buggify was dropped.
  A caller NOT on a loop (cur == NULL: OS threads, the blocking pool,
  tooling) takes the ORIGINAL pthread_cond_wait path byte for byte --
  test/m13/test_lwlock (the thread-path suite) still passes unchanged,
  incl. under ASan+UBSan.  The struct gained two opaque void* fields
  (wq_head / wq_tail) for the fiber queue, zero-initialised by
  xtc_lwlock_init and never touched on the thread path.  No lwlock bug
  was found -- the primitive parks and re-grants correctly.

  (A slab.alloc.magazine_miss buggify was PROTOTYPED then REMOVED: the
  slab allocator backs channels / gen_server, so the new site fired
  inside test_sim_buggify / test_sim_buggify3 and its per-call
  xtc_sim_fault draw perturbed those tests' replay -- a documented
  hazard of adding a draw site on a hot shared path.  test_sim_slab's
  small chunk_size + magazine already forces the cache-lock slow path
  naturally, so the buggify added risk for no coverage gain and was
  dropped rather than shipped.)

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
