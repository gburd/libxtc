# M_EC2_SCALE -- libxtc scale-out test on a 32-core EC2 instance

A one-shot scale-out run of libxtc v0.8.0 on real large hardware, to
measure how the work-stealing executor, the stackful-fiber memory
model, the cooperative-preemption watchdog, and the DST soak behave at
scale.  The instance was ephemeral and was torn down after the run.

## Host

- AWS EC2 i4i.8xlarge, us-east-2c
- Intel Xeon Platinum 8375C @ 2.90 GHz, 32 vCPU
- 247 GB RAM
- 2x 3.4 TB NVMe instance storage (build + scratch on one NVMe)
- Ubuntu 24.04, kernel 6.17-aws, gcc 13.3, io_uring backend
- libxtc v0.8.0 (commit 996f65e); `make check` = All tests passed.

## Results

### 1. Executor scaling -- distributed production (the real pattern)

Each loop runs a generator that spawns its share of 20,000,000 tiny
(200-iter) leaf tasks on its OWN loop, so production is distributed
across cores as it is in a real server (connections/requests arrive on
many loops).  Work stealing balances the leaves.

    loops   tasks/sec     speedup vs 1 loop
        1     1.7 M/s      1.0x
        2     3.3 M/s      2.0x
        4     6.5 M/s      3.8x
        8     9.6 M/s      5.6x
       16    10.9 M/s      6.4x
       32    13.0 M/s      7.7x

Near-linear to 8 cores (5.6x), then diminishing returns (memory
bandwidth / cross-core cache effects on a leaf-heavy workload).  13.0 M
tiny-tasks/sec on 32 cores is competitive work-stealing scaling.

### 2. Executor scaling -- single producer (a bottleneck to know about)

When ONE thread spawns all 20 M tasks round-robin across the loops
(a single-producer pattern), throughput plateaus:

    loops   tasks/sec
        1     1.2 M/s
        2     1.7 M/s
        4     2.1 M/s
        8     2.3 M/s
       16     2.4 M/s
       32     2.4 M/s

The single spawning thread + cross-loop enqueue contention is the cap,
not the executor.  HONEST FINDING: feed work distributed (spawn from
within loops), not from one producer, to scale past ~8 loops.  This is
the normal server pattern; the single-producer case is a fan-out
admission point that should itself be sharded.

### 3. Memory per task (stackful fibers)

100,000 simultaneously-parked fibers, RSS delta per live fiber:

    stack size   bytes/live-fiber
        16 KiB       7919   (guard/VMA fragmentation dominates)
        32 KiB       4096   (one committed page)
        64 KiB       4096
       128 KiB       4096

A parked fiber commits ~1 page (4 KiB) regardless of its (larger)
reserved stack -- demand paging.  At 5,000,000 fibers the process used
~12 GB RSS (~2.5 KB/fiber amortized) and hit the VMA cap gracefully.
Confirms the S1 madvise-on-park opportunity (docs/M_PREEMPTION.md): a
parked fiber's committed page is reclaimable, which would push the
idle-fan-in floor well below 4 KiB.  For extreme fan-in the stackless
xtc_tnt Isolate layer (hundreds of bytes, no stack) is the alternative.

### 4. Fairness under a runaway (cooperative preemption)

One non-yielding CPU-burn runaway + cooperative workers on one loop:

    budget OFF (runaway never yields):  8 cooperative iterations
    budget ON  (xtc_yield_if_due, 1ms): 776 cooperative iterations

The Phase-1 cooperative-assisted watchdog turns near-total starvation
into fair interleaving (97x more cooperative progress) on this hardware.

### 5. DST soak at scale

The deterministic-simulation soak (mixed cross-loop ping/pong + timer
sleepers under the seeded scheduler) over 50,000 seeds:

    swept 50000 seeds: 0 failures, 256 distinct schedules; every seed
    reached quiescence, replayed identically, invariants held (56 s).

The largest soak run to date -- strong evidence the scheduler core is
deterministic and correct across a wide interleaving space.

## Takeaways

- The work-stealing executor scales ~8x across 32 cores under the
  realistic distributed-production pattern; a single producer is the
  bottleneck to shard around.

## FOLLOW-UP (2026-07): the 7.7x was mostly benchmark artifact

The 7.7x-on-32-cores number was investigated ("why so low?").  It is
NOT a scheduler-design limit.  A local 8-core decomposition isolated
two contention sources, both outside the scheduler:

    variant                          8-core throughput   8-core scaling
    shared global atomic (this bench)    6.5 M/s          ~4x (plateaus)
    per-loop isolated counter            8.9 M/s          5.2x
    no-alloc + per-loop (pure sched)    21.9 M/s          ~10x

1. The benchmark folded every leaf's completion into ONE global
   atomic (g_done).  That single cache line, written by all cores at
   millions/sec, ping-pongs across the machine -- textbook false
   contention, worse with more cores (hence the 32-core collapse).
   Isolating the counter per loop recovered a large fraction.  Pure
   BENCHMARK artifact.
2. The remaining falloff is the per-task ALLOCATOR: xtc_task_spawn
   mallocs a task + coro struct per unit, and glibc malloc's cross-core
   arena contention caps spawn-heavy microbenchmarks at millions/sec.
   Partly libxtc (it allocs per task), partly glibc.

Removing BOTH (a rescheduling worker pool -- no per-unit alloc -- plus
per-loop counters) shows the executor SCHEDULER scaling ~10x on 8 cores
(2.2 -> 21.9 M ops/s).  So libxtc's work-stealing scheduler scales
nearly linearly; the 7.7x figure was dominated by a single shared
counter in the harness plus allocator pressure, not the runtime.

Implications: (a) real workloads should not funnel through one shared
atomic (they don't -- each connection/request keeps its own state);
(b) a per-task slab/freelist for the task+coro structs (instead of
malloc) would lift the spawn-heavy ceiling -- a concrete, bounded
optimization (candidate for a future release, akin to the S1 stack
work).  The corrected headline: the scheduler scales; feed it
distributed work and pool the per-task allocation.

### Follow-up (per-task free-list, implemented): allocation was NOT
### the throughput ceiling -- but DONE tasks leaked

The per-task free-list from (b) was implemented and measured
honestly.  Two findings, one expected and one not:

- THROUGHPUT: no measurable win on glibc.  A rigorous back-to-back
  A/B (free-list ON vs a XTC_TASK_FREELIST_MAX=0 build) on a
  steady-state churn benchmark showed 1.6-1.8 M/s BOTH ways -- the
  delta is inside the run-to-run noise.  glibc's per-thread tcache
  already IS a free-list for a ~120-byte size class in a tight
  recycle pattern, so a hand-rolled per-loop free-list saves only a
  tcache-hit malloc (~15 ns) against a ~600 ns/task total.  The EC2
  hint that "allocator pressure" capped scaling was measuring the
  shared-counter contention, not malloc.
- FOOTPRINT (the real bug): before this change, a completed plain
  task was never freed -- it lingered in loop->all_tasks until
  loop_fini.  A long-lived loop that churns tasks therefore grew
  WITHOUT BOUND: 878 MB RSS for 8M sequential tasks (steady-state
  width 256).  With the free-list (and, crucially, unlinking the
  DONE task from all_tasks and reclaiming it), the same workload
  peaks at ~4 MB -- a 222x reduction.  This is an unbounded-memory
  leak fix for exactly the long-lived-server use case libxtc
  targets, NOT a micro-optimization.

So the change ships for the footprint fix; the free-list recycling
rides along at zero throughput cost.  The regression guard is
test/m3/test_task.c Ts7_recycle_bounded (asserts RSS growth < 32 MB
across a 600k-task churn; fails at 64 MB against the old
linger-forever behavior).  The corrected microbenchmark is
bench/bench_exec_scale.c (spawn / reuse / churn modes, per-loop
cache-isolated counters, distributed generators).
- The stackful memory floor is ~1 committed page/parked fiber; S1
  (madvise-on-park) and the stackless tnt layer are the two levers for
  extreme fan-in.
- Cooperative preemption (Phase 1) is effective; true involuntary
  preemption (Phase 2b-arch) remains future work.
- DST holds at 50k seeds.

## Cost / cleanup

Ephemeral run.  The instance, key pair, and security group were all
deleted after the run (see the teardown in the session).  No AWS
resources left standing.
