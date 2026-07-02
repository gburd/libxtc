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
