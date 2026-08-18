# L1 proportional-share scheduler -- the measurement

Feature: opt-in weighted-fair (proportional-share) scheduling, INSPIRED
BY Glommio (Glauber Costa / ScyllaDB).  `bench/bench_sched_shares.c` is
the measure-first gate: it demonstrates the gap the feature fills and
the after-number once it exists.

## Setup

One event loop.  Two groups of well-behaved worker fibers -- class A and
class B, `N_PER_CLASS = 4` each -- doing equal-size compute chunks
(`CHUNK_ITERS = 2000`) with a cooperative `xtc_yield` between chunks.
CPU split is measured as the ratio of completed chunks, A:B, over a
fixed wall-clock window (default 300 ms).

This is a different axis from `bench_fairness.c`, which measures a
single non-yielding runaway and the cooperative yield-budget remedy.
This bench is about proportional CPU SHARE among well-behaved workers,
which the FIFO run queue cannot express at all.

## Result (300 ms window, representative run)

```
BEFORE (plain FIFO run queue):         A=69272 B=69272  ratio=1.00:1
AFTER  (class A shares 3, B shares 1): A=8048  B=2685    ratio=3.00:1
```

- BEFORE: the FIFO run queue splits CPU 1:1 among the equally busy
  classes.  There is no knob -- no way to say "A should get 3x B".  This
  is the gap.
- AFTER: tagging A with 3 shares and B with 1 (via
  `xtc_exec_class_create` + `xtc_proc_opts_t.sched_class`) makes the
  min-vruntime pick give A ~3x the CPU: 3.00:1.

The absolute chunk counts drop in the AFTER run because the two-class
accounting adds a small per-run clock read; the ratio is the point, and
it moves from 1:1 to 3:1 exactly as the shares dictate.

## How it works

Each loop carries a small set of run classes `{shares, vruntime,
reciprocal = (1<<22)/shares, ready FIFO}`.  `__queue_pop` picks the
minimum-vruntime class and pops its FIFO; the run-end boundary accrues
`vruntime += (cost_ns * reciprocal) >> 12` -- Glommio's exact formula
(executor/mod.rs `account_vruntime` + shares.rs `reciprocal_shares`).  A
higher-shares class accrues virtual time more slowly, so it is picked
more often, in proportion to its shares.

Off by default with zero overhead: a loop with no class created runs the
exact plain-FIFO + work-stealing path, byte-for-byte; the vruntime
accounting activates only once a class exists (`loop->n_classes > 0`).

## Proof tiers

- DST: `test/sim/test_sim_sched_shares.c` -- 3:1 shares => 3.00:1 CPU
  (run count) over a seeded run, byte-identical replay, a latency class
  scheduled promptly (step 8 vs a 1600-run backlog), and the default-off
  path unchanged.
- PBT: `test/pbt/pbt_sched_shares.c` -- for drawn shares hi >= lo, the
  higher-shares class is never picked fewer times (monotonic CPU).
- Unit: `test/m14/test_stall.c` -- the L3 over-budget stall watchdog
  (hog trips it and is named; a polite run never trips; off by default
  is a single branch).

## Running

```sh
cd build_unix && make bench_sched_shares && ./bench_sched_shares 300
```
