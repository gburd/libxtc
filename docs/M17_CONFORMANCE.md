# M17 -- Conformance benchmarks vs Tokio + Erlang

**Status:** in progress.  Workloads W1-W7 have xtc + Tokio (+
Erlang, where meaningful) implementations under
`bench/conformance/`; W5 (reader/writer ratio sweep) completed and
fairness-audited in A10.  This document outlines the conformance +
benchmark plan and records the per-workload fairness findings so we
can publish credible "xtc vs Tokio vs Erlang" numbers before
declaring the runtime production-ready.

## Goal

Demonstrate that xtc's primitives match -- within a constant factor
-- the behaviour and performance of equivalent Tokio (async Rust)
and Erlang (BEAM) idioms, on a small set of canonical workloads.

If we cannot show parity, we surface where the gap is and decide
whether to close it or document the trade-off.

## Workload matrix

| ID | Workload | Why |
|---|---|---|
| W1 | spawn-N-await-all | task creation cost |
| W2 | echo server, 1k clients, 10k req/sec | network throughput |
| W3 | mailbox ping-pong (N=1M messages) | actor latency |
| W4 | mutex contention (N writers) | sync primitive cost |
| W5 | reader/writer ratio sweep (1:1 -> 100:1) | rwlock vs lrlock (done, A10; BEAM-exempt) |
| W6 | tail latency under backpressure (M19.4 res) | p99/p999 |
| W7 | timer wheel (N=100k pending timers) | scheduler accuracy |

## Reference implementations

For each workload we keep equivalent code in three runtimes:

```
benches/
  conformance/
    w1_spawn/
      xtc.c          (this repo)
      tokio.rs       (cargo-runnable, lives in benches/conformance/tokio/)
      erlang.erl     (escript)
    w2_echo/
      ...
    w3_pingpong/
      ...
    ...
```

Each implementation:

1. Uses the runtime's idiomatic primitives (no fair-vs-unfair
   tweaks).
2. Reports the same metrics format on stdout:
   ```
   workload=W1 runtime=xtc params=N=10000 elapsed_ns=12345 cpu_us=2345 rss_kb=45678
   ```
3. Is single-binary, no external deps beyond the runtime itself.

## Metrics

Per workload, we capture:

- Wall-time elapsed
- CPU time (user + sys)
- Peak RSS
- p50 / p95 / p99 / p999 of operation latency (from in-process
  histogram)

For tail latency, use HDRHistogram (xtc has no built-in equivalent
yet; M19.4 candidate).

## Hardware

- Reference machine: 32-core AMD EPYC, 64 GB RAM, no other load
  during runs.  (CI's Linux box where possible; `bench/` results in
  PLAN.md already use this.)
- Pin: `taskset 0-15` to reduce NUMA noise.
- Disable: turbo boost, swap, irq-balance.

## Conformance criteria

For each (workload, primitive) pair we check:

1. **Correctness** -- output values match a reference oracle.
2. **Throughput** -- within 50% of the best of the three runtimes.
3. **Tail latency** -- p99 within 2x the best.
4. **Memory** -- peak RSS within 2x the best.

A failure on any of these flags an architectural concern and
becomes a tracking issue.  We do **not** hide failing benchmarks;
we publish them as "xtc currently 4x slower than Tokio on W3".

## W5 -- reader/writer ratio sweep, and BEAM applicability

W5 sweeps the reader:writer ratio (1:1, 10:1, 100:1) over N worker
threads sharing one guarded counter, and compares two xtc
reader/writer primitives against two Tokio ones:

- xtc `xtc_arwlock` -- the fiber-yielding shared/exclusive latch
  (`xtc_sync.h`); readers share, one writer excludes.
- xtc `xtc_lrlock` -- the Left-Right lock (`xtc_lrlock.h`);
  wait-free reads against two side-by-side copies, single writer
  with a pointer-swap publish.
- Tokio `tokio::sync::RwLock` (async latch, `src/main.rs`).
- Tokio `parking_lot::RwLock` (raw-OS latch, `src/main_pl.rs`) --
  the fair analogue of the wait-free / raw read side, mirroring the
  W4 `parking_lot::Mutex` variant.

The xtc binary runs both primitives (`--prim=arwlock|lrlock`) and
sweeps ratios (`--ratio=R`, default sweep 1/10/100); the CLI matches
W4 (`--threads`, `--ops`, `--params=threads=N:ops=M:ratio=R`).

**BEAM applicability:** W5 is deliberately Erlang-exempt.  A
reader/writer lock is not a first-class BEAM primitive -- shared
mutable state is normally kept behind a single owning process (an
actor), which serialises *all* access and so has no reader-scaling
dimension for a ratio sweep to exercise.  ETS gives concurrent
readers but is a table/KV abstraction, not an rwlock, and its
semantics (per-key atomicity, no explicit shared/exclusive latch)
make it a different measurement.  W5 is therefore an rwlock-*primitive*
comparison and only meaningful for the two runtimes that expose one
(xtc, Tokio).  This is not a gap in BEAM; it is a design difference,
so we record it rather than force an unrepresentative Erlang entry.

## Fairness re-audit (A10.2)

All seven workloads were re-audited to confirm xtc, Tokio, and (where
present) Erlang measure the same thing under the same conditions.
Findings:

- **Defaults match across runtimes** for every workload: W1 N=10000,
  W2 clients=1000/msgs=10, W3 N=1000000, W4 threads=8/ops=100000,
  W5 threads=8/ops=100000/ratios=1,10,100, W6 gens=8/ops=1000000/cap=1000,
  W7 N=100000.  Per-op work, the 1-in-1000 sampling cadence, the
  per-thread sample-window stagger (`idx*97+1`), and the merged-
  histogram + percentile reporting are identical across runtimes.

- **Tokio runtime thread count is the one systematic asymmetry.**
  W4 and W5 Tokio pin `worker_threads(n_tasks)` to match the
  intended contention level; but W1, W3, and W7 Tokio use
  `#[tokio::main]`, which defaults to a multi-thread runtime sized
  to *all* available cores, while the xtc W1/W3/W7 sides run on a
  single cooperative loop.  For W1 (task-creation cost) and W3
  (an inherently serial 2-actor ping-pong) this does not hand Tokio
  a throughput advantage that changes the conclusion, but it should
  be normalised (pin Tokio's worker_threads, or run xtc on an
  N-loop executor) before publishing headline numbers.  Tracked as
  a follow-up for the W1/W3/W7 owners; W4/W5 are already correct.

- **W5 (owned here) is fair by construction.** Both xtc primitives
  and both Tokio primitives run the identical ratio-phased loop
  (`ratio` reads then 1 write, repeating), the same N threads, the
  same ops budget, the same sampling, and each verifies the final
  counter equals the total writes issued (mutual-exclusion oracle).
  The async-vs-raw distinction is surfaced as two explicit variants
  per runtime (arwlock/lrlock, tokio_rwlock/parking_lot) rather than
  hidden, so no side silently does less work.

- **No accidental advantages found** in per-op work, warmup, or
  measurement window: none of the workloads warm up (all measure
  cold, consistently), all use CLOCK_MONOTONIC / Instant /
  erlang:monotonic_time for the timed window, and RSS/CPU are read
  via getrusage or /proc uniformly.  Pinning (`taskset 0-15`) is
  applied by the harness, not the binaries, so it affects all
  runtimes equally.

## Tooling

```sh
# benches/conformance/run.sh runs all 21 (= 7 workloads x 3 runtimes)
# binaries and produces a CSV.

./benches/conformance/run.sh > results.csv
./benches/conformance/plot.py < results.csv > results.html
```

The plot output is committed to docs/M17/results-YYYY-MM.md as a
rolling artifact so we can track progress over time.

## Open questions

1. **Erlang ping-pong overhead** -- Erlang processes are ~300 bytes;
   xtc procs are ~512 bytes (with mailbox + monitor lists).  Is the
   gap acceptable?  Need a memory-shrink pass on `struct xtc_proc`
   if not.

2. **Tokio's work-stealing scheduler** is single-process,
   N-threads.  xtc's executor matches this layout.  Should W1
   benchmark task creation sequentially or via a contention
   harness?  (Both, if budget allows.)

3. **Fair benchmarking** -- Tokio uses an MPSC channel for
   backpressure; Erlang uses bounded mailboxes; xtc uses
   `xtc_chan_mpsc` + `xtc_res`.  Each runtime's idiom is
   different; we should compare "what each runtime would write"
   not artificially constrained code.

## Effort estimate

| Component | Lines | Days |
|---|---|---|
| 7 xtc benchmarks | ~1500 | 5 |
| 7 Tokio benchmarks | ~1500 | 5 |
| 7 Erlang benchmarks | ~1000 | 3 |
| run.sh + plot.py | ~300 | 2 |
| First-run analysis | -- | 5 |

Total: ~20 person-days (~1 month) for a credible first-pass
publication.

## Next concrete step

When greenlit:

1. Pick 1 workload (W1 spawn-N-await-all is simplest).
2. Implement xtc + Tokio + Erlang versions.
3. Run on the buildfarm Linux box, publish W1 results.
4. Iterate: each subsequent workload is a self-contained patch
   that adds ~3 files + 1 row of CSV.

This is mostly bench-coding work; design risks are minimal.
