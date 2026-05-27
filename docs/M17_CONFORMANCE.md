# M17 — Conformance benchmarks vs Tokio + Erlang

**Status:** research; not yet implemented.  This document outlines
the conformance + benchmark plan so we can publish credible
"xtc vs Tokio vs Erlang" numbers before declaring the runtime
production-ready.

## Goal

Demonstrate that xtc's primitives match — within a constant factor
— the behaviour and performance of equivalent Tokio (async Rust)
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
| W5 | reader/writer ratio sweep (1:1 → 100:1) | rwlock vs lrlock |
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

1. **Correctness** — output values match a reference oracle.
2. **Throughput** — within 50% of the best of the three runtimes.
3. **Tail latency** — p99 within 2× the best.
4. **Memory** — peak RSS within 2× the best.

A failure on any of these flags an architectural concern and
becomes a tracking issue.  We do **not** hide failing benchmarks;
we publish them as "xtc currently 4× slower than Tokio on W3".

## Tooling

```sh
# benches/conformance/run.sh runs all 21 (= 7 workloads × 3 runtimes)
# binaries and produces a CSV.

./benches/conformance/run.sh > results.csv
./benches/conformance/plot.py < results.csv > results.html
```

The plot output is committed to docs/M17/results-YYYY-MM.md as a
rolling artifact so we can track progress over time.

## Open questions

1. **Erlang ping-pong overhead** — Erlang processes are ~300 bytes;
   xtc procs are ~512 bytes (with mailbox + monitor lists).  Is the
   gap acceptable?  Need a memory-shrink pass on `struct xtc_proc`
   if not.

2. **Tokio's work-stealing scheduler** is single-process,
   N-threads.  xtc's executor matches this layout.  Should W1
   benchmark task creation sequentially or via a contention
   harness?  (Both, if budget allows.)

3. **Fair benchmarking** — Tokio uses an MPSC channel for
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
| First-run analysis | — | 5 |

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
