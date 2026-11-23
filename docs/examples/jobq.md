---
title: jobq (simulation testing your own program)
parent: Examples
nav_order: 15
permalink: /examples/jobq/
lede: >-
  A job queue with leases and fencing tokens, and the deterministic simulation test that hunts its bugs: the template for testing your own program the way libxtc tests itself.
---

1. TOC
{:toc}

---

Source:
[`examples/20_jobq/`](https://codeberg.org/gregburd/libxtc-examples/src/branch/main/20_jobq)
(about 1,400 lines of C: the service, a native `main`, and the harness).

## What it is

`20_jobq` is two programs built from one service.  `jobq.c` is a job
queue with leases, retries and fencing tokens, spread over four loops.
`main.c` runs it on real threads.  `dst.c` runs the same code under
libxtc's deterministic simulator for hundreds of seeds, with injected
latency, partitions, crashes and the service's own pessimal paths, and
checks the service's invariants after every run.  The harness uses only
the installed `xtc_sim.h` (see `xtc_sim(3)`), so it is the template for a
deterministic simulation test (DST) of your own program.

[Testing]({{ '/testing/' | relative_url }}) explains why libxtc treats DST
as its main defence.  This page shows what it takes to give a program of
your own the same defence.

## The service

A producer submits jobs.  Workers lease a job, run it, write its result
to an object store, and report it complete.  A lease runs out, so a
worker that crashes or stalls loses its job and the queue grants the job
again.  That makes the queue survive worker failure, and it is also how
a job comes to run twice: a worker that stalled past its lease can wake
up and write after the job's next holder already wrote.

The defence is a fencing token.  Every lease carries a token larger than
any granted before it, and the store refuses a write whose token is below
one it has accepted for that job.  A restarted queue keeps the tokens
growing by making a new epoch durable before it grants anything, and
building tokens as `epoch << 32 | n`.

| loop | procs |
|---|---|
| 0 | the queue (journal, leases), under a supervisor |
| 1 | the object store, the producer |
| 2, 3 | two workers each, under a supervisor |

All communication is `xtc_send` between loops.  Every request is
idempotent and carries a request id, and every client retries on
silence.  The harness can therefore drop, delay and reorder any message,
and kill any process, without changing the outcome.

## The harness, in miniature

The cut-down program below has the same structure as `dst.c`, reduced to
one lease: a server on loop 0, two workers on loops 1 and 2, and a store.
The store is the code under test:

{% include snippet.html file="22_dst_lease.c" region="store" %}

The worker takes the lease, works for a seeded time, checks that its
lease is still valid, and writes.  The buggify point is where the race
lives: a pause between the check and the write that outlives the lease.
`XTC_SIM_BUGGIFY` decides it once per run from the seed, so some seeds
pause every time and some never do.  `xtc_sim_buggify_fault` adds a
per-reach coin on top.

{% include snippet.html file="22_dst_lease.c" region="worker" %}

The harness runs a seed, checks the invariant, and runs the seed again.
The trace hash, built by the program from its own events and their
virtual times, must come out the same:

{% include snippet.html file="22_dst_lease.c" region="harness" %}

Built normally, 200 seeds pass, each replayed.  Built with
`-DXTC_DST_INJECT_BUG=1`, the store skips the fence and seed 2 fails.
That second build matters as much as the first: a harness that has not
been shown to fail on a known bug has not been shown to test anything.

## The full harness

`dst.c` adds what a real service needs.

**A fault mix per seed.**  Each fault class is on or off per seed (swarm
testing), so some runs see one class alone and others see all of them:
cross-loop latency (`xtc_sim_net_latency`), delivery reordering
(`xtc_sim_swizzle_enable`), loop starvation (`xtc_sim_sched_pessimal`),
clock jitter (`xtc_sim_clock_skew`), and buggify.

**A nemesis.**  A proc that runs beside the service and, at seeded
times, kills the queue (`xtc_exit_pid`; its supervisor restarts it from
the journal), kills a worker, or cuts a loop off the network
(`xtc_sim_partition_isolate`).  Then it heals everything and leaves, so
the service can finish.

**The invariants, checked at quiescence** (`xtc_sim_set_consistency_check`):
no acknowledged job lost; no write under an older lease after one under
a newer lease; no two workers under one lease; the stored result is the
completing lease's; every job done.

**The service's own decision points**, marked `XTC_SIM_BUGGIFY`: a worker
pausing past its lease, a worker dropping a job, leases shorter than the
work, a slow journal, duplicate submits and completions.

**The replay check**: the service's trace hash, `xtc_sim_state_hash`, and
the counters must agree between two runs of each seed.

**Coverage, reported**: the kills and cuts the nemesis made, the seeds in
which the queue restarted, a lease was regranted, and the fence refused
a stale write, and how often each buggify site was reached and on.  A
green sweep that never reached the dangerous states proves little.

```sh
make check-jobq                               # in a libxtc build tree
make check-jobq-sweep JOBQ_SEEDS=100000       # a longer sweep
```

`make check` runs `check-jobq`: the service once on threads, then 300
seeds, each twice, in about a second.  A native build makes its own
simulation library in `jobq-sim/` the first time.  A sweep of 20,000
seeds passes, with 92,688 kills and 32,194 network cuts, and the store
fenced a stale write in 10,705 of those seeds (2026-11-22, libxtc 2.3
development; the counts move when libxtc's own buggify sites change).

## What it catches

Three bugs are planted in `jobq.c`, each behind `XTC_DST_BUG(n)`
(`make PLANT=n`).  `make check-plant` in the example requires each to
fail the 300-seed run:

| plant | the bug | first failing seed | reported as |
|---|---|---|---|
| 1 | the store accepts a write without the fence | 2 | FENCE |
| 2 | the queue acknowledges a submit before its journal write | 7 | ACKED-LOST |
| 3 | a restarted queue reuses its epoch, so tokens repeat | 2 | DOUBLE-EXEC |

A failure prints the seed and the command that replays it.  `-v` prints
the run's trace, so the failure reads as a story with timestamps:

```
   111.286 ms  queue.grant        9  10000000c
   115.483 ms  nemesis.kill_q     0  0
   116.483 ms  queue.epoch        0  2
   118.886 ms  queue.grant        9  200000001
   125.240 ms  store.write        9  200000001
   127.159 ms  store.write        9  10000000c
```

The queue leased job 9 in epoch 1, was killed, restarted as epoch 2 and
leased the job again.  The new holder wrote, then the old holder's write
landed after it.

While the example was being written, the harness also found a real bug
in it: the first shutdown asked the store to stop with a message, sent
to a pid looked up by name.  A buggify site inside libxtc
(`reg.whereis.transient_miss`) made both lookups miss, the store was never
told, and seed 17 ended in `XTC_E_DEADLK` with the store parked.  The
fix kills the store by pid.

## Adopting DST for your own program

1. Put the service behind a function that spawns its procs on an
   `xtc_exec`.  Production calls it and `xtc_exec_run`; the harness calls
   it and `xtc_sim_exec_run`.
2. Take time from `xtc_clock_mono` and `xtc_proc_sleep`, and random
   choices from `xtc_sim_rand`.  The determinism guard aborts a run that
   reads the real clock, the environment or a thread id.
3. Bound every wait.  An unbounded wait on a dropped message is a hang,
   and the simulator reports it as `XTC_E_DEADLK`.
4. Write the invariants as code over the service's state.
5. Mark the decisions that make your service subtle with
   `XTC_SIM_BUGGIFY`.
6. Run each seed twice and compare a trace hash of your own events.
7. Plant a bug for each invariant, behind `XTC_DST_BUG(n)`, and prove
   the sweep catches it.
8. Keep every seed that ever failed, and rerun it in `make check`.

Link the harness against a library configured
`--with-io-backend=sim`: in a build tree, that build's `libxtc.a`;
installed, `libxtc-sim.a` (`pkg-config --static --libs xtc-sim`).

## Limits worth knowing

**Sockets are not simulated.**  The simulator models messages between
loops: latency, reordering, partitions.  A real socket never becomes
ready under a simulated run, because the simulator has no kernel poller.
A service that talks over `xtc_net` needs a seam to be simulated, such
as messages in the test build and sockets in production.  `20_jobq`
uses the loops as its network.

**A partition drops DOWN signals too.**  A cut edge drops every message
on it, including the DOWN a monitor or supervisor waits for, and nothing
redelivers it after the heal.  In a real process a DOWN is never dropped;
in distributed Erlang a lost connection surfaces as a DOWN with reason
`noconnection`.  Keep each supervisor on its children's loop, as
`20_jobq` does, until the simulator models this.

**`xtc_sim_partition_clear` also clears the latency window**, and the
partition matrix outlives a run.  Clear it before each run, and set the
latency again after a heal.

## Alternatives not taken

* **Stress testing on real threads.**  It runs the real kernel and the
  real scheduler, which DST does not, and it belongs next to DST
  (`main.c` runs the service that way).  It cannot replay a failure,
  and it does not steer toward the bad interleavings.
* **A model checker.**  Exhaustive over a model of the protocol, not
  over the code.  DST runs the code itself, so the bug it finds is in the
  program you ship.
* **Exactly-once execution.**  It needs the worker's effect and the
  completion to commit together, which a store outside the queue cannot
  do.  At-least-once with fenced effects is the standard answer.
* **A per-step invariant check.**  It would catch a violation earlier
  in the trace, but it has to run inside the service.  The check at
  quiescence sees the store's whole write log and the journal, which is
  where the safety properties live.
