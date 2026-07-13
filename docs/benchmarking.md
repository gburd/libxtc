---
title: Benchmarking
nav_order: 5
has_children: false
permalink: /benchmarking/
lede: >-
  How we measure performance at scale -- and an honest admission that most published benchmarks are marketing.
---

1. TOC
{:toc}

---

## A warning about benchmarks

Let us be blunt: **most published benchmarks are marketing.** They are
run on hardware chosen to flatter, with a workload chosen to win, tuned
by the authors of the thing being sold and not by the authors of the
thing it beats, and reported as a single big number with the caveats in
6-point type. A benchmark that exists to prove a conclusion is not
evidence; it is an advertisement.

libxtc tries to do the opposite: publish the *methodology* and the
*workloads* so you can run them yourself, compare against a credible
baseline we did not write, and see the losses as well as the wins. A
performance claim you cannot reproduce is worth nothing, so the goal here
is reproducibility and honesty -- warts and all -- not a leaderboard.

## What we measure against

The conformance suite (`bench/conformance/`) pits libxtc against
**Tokio** (Rust's async runtime) on the same machine, because Tokio is a
mature, widely respected, independently built runtime -- a baseline we
have no ability to tilt. Seven workloads probe different axes:

| Workload | Probes |
|---|---|
| W1 spawn | task/fiber creation and teardown throughput |
| W2 echo | socket round-trip latency and throughput |
| W3 pingpong | message-passing latency between two units |
| W4 mutex | contended-lock throughput |
| W5 rwratio | reader/writer mix on a shared structure |
| W6 tail | tail latency (p99/p99.9) under load |
| W7 timer | timer scheduling accuracy and throughput |

Each is run in **two framings**, because a single number hides the
trade: **single-core** (per-core efficiency -- the fair default,
especially for the serial workloads) and **full-parallelism** (all cores
-- what a deployment actually sees). The driver reads `BENCH_WORKERS`
rather than silently grabbing every core, so the comparison is
apples-to-apples.

## How we run at scale

- **Micro-benchmarks** (`bench/bench_*.c`) isolate one primitive:
  `bench_million_tasks` (can we hold a million live fibers, and at what
  memory?), `bench_mem_per_task` (bytes per fiber), `bench_exec_scale`
  (throughput vs. core count), `bench_fairness` (does one loop starve
  others?), `bench_disk` / `bench_uring_disk` / `bench_net` (I/O paths).
  The fiber context switch measures about **7.6 ns** on x86-64 -- a
  micro-number that is honest precisely because it measures one narrow
  thing and says so. On Apple Silicon (macOS arm64) there is no Mach-O
  fcontext substrate yet, so the portable `ucontext`-based fallback is
  used instead; it saves/restores the signal mask on every switch (a
  `sigprocmask` syscall), which measures in the tens of microseconds
  rather than nanoseconds (see [Known issues]({{ '/reference/known-issues/' | relative_url }})).
- **Application benchmarks** run the [example servers]({{ '/examples/' | relative_url }})
  under realistic load: the [rexis](  {{ '/examples/rexis/' | relative_url }})
  Redis work-alike against `redis-benchmark`, and the
  [sqlxtc](  {{ '/examples/sqlxtc/' | relative_url }}) engine under
  concurrent query load (`bench/sqlxtc/`). These matter more than the
  micro-numbers: they show libxtc under the messy, mixed workload a real
  system produces, not a synthetic best case.
- **At scale** means many cores and long runs, on real hardware, with the
  baseline built and run the same way -- not a cherry-picked burst.

## Reading the results honestly

A few principles we hold ourselves to:

- **Report the losses.** If Tokio wins a workload, that is in the results
  too. A runtime that wins everything is a runtime whose benchmark was
  designed to.
- **Per-core efficiency first.** Throwing more cores at a problem can
  mask a per-core regression; the single-core framing catches it.
- **Tail latency, not just mean.** W6 exists because a good average with
  a bad p99 is a bad system for anything user-facing; the mean is the
  easiest number to game.
- **The same machine, the same day.** Cross-run, cross-machine
  comparisons drift; a result is a *paired* measurement or it is noise.

## Running them yourself

```sh
cd bench/conformance
BENCH_WORKERS=1 ./run.sh        # single-core (per-core efficiency)
BENCH_WORKERS=$(nproc) ./run.sh # full parallelism
```

The harness and its output format are in
[`bench/conformance/`](https://codeberg.org/gregburd/libxtc/src/branch/main/bench/conformance);
the sqlxtc application benchmarks and their recorded runs are in
[`bench/sqlxtc/`](https://codeberg.org/gregburd/libxtc/src/branch/main/bench/sqlxtc).
Run them on your hardware, against your baseline. That is the only
benchmark result that should ever persuade you -- including ours.

---

&larr; [Testing]({{ '/testing/' | relative_url }})
