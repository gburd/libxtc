# bench/conformance -- Cross-Runtime Conformance Benchmarks

This directory contains the benchmark infrastructure and workload
implementations for the **M17 conformance suite**: xtc vs Tokio vs Erlang.

See [docs/M17_CONFORMANCE.md](../../docs/M17_CONFORMANCE.md) for the full
design rationale, conformance criteria, and effort estimates.

See [docs/M17_RESULTS_FORMAT.md](../../docs/M17_RESULTS_FORMAT.md) for the
exact stdout format every benchmark binary must emit.

---

## Quick start

```sh
# Run all benchmarks and collect CSV results:
./bench/conformance/run.sh > results.csv

# Visualise (requires Python 3; matplotlib optional):
python3 bench/conformance/plot.py < results.csv
```

---

## Directory layout

```
bench/conformance/
    README.md          -- this file
    run.sh             -- benchmark runner / CSV aggregator
    plot.py            -- visualisation script (matplotlib or text fallback)
    include/
        hist.h         -- HDR-style histogram, single-header C library
        hist.c         -- implementation translation unit for hist.h
    w1_spawn/          -- W1: spawn-N-await-all
    w2_echo/           -- W2: echo server, 1k clients
    w3_pingpong/       -- W3: mailbox ping-pong
    w4_mutex/          -- W4: mutex contention
    w5_rwratio/        -- W5: reader/writer ratio sweep
    w6_tail/           -- W6: tail latency under backpressure
    w7_timer/          -- W7: timer wheel accuracy
```

Each workload directory contains one subdirectory per runtime:

```
w<N>_<name>/
    xtc/               -- xtc implementation (C; main.c + Makefile)
    tokio/             -- Tokio implementation (Rust; Cargo.toml + src/main.rs)
    erlang/            -- Erlang implementation (escript; main.erl)
```

The compiled (or scripted) benchmark binary must be named **`bench`** inside
its runtime subdirectory.  `run.sh` discovers executables at
`w<N>_<name>/<runtime>/bench` and skips absent ones silently.

---

## Adding a workload implementation

1. Create `w<N>_<name>/<runtime>/` and add source files.
2. Build to produce an executable named `bench` in that directory.
3. The binary must write exactly one key=value line to stdout; see
   [docs/M17_RESULTS_FORMAT.md](../../docs/M17_RESULTS_FORMAT.md).
4. Run `./bench/conformance/run.sh` -- the new result appears automatically.

---

## hist.h -- HDR histogram

`include/hist.h` provides a log-linear histogram for in-process latency
capture.  Sub-bucket count and total bucket storage scale with precision:

| sub_bits | sub-buckets | relative error | buckets (60 s range) |
|----------|-------------|----------------|----------------------|
| 4        | 16          | <= 6%           | 512                  |
| 7        | 128         | <= 0.8%         | 3840                 |
| 10       | 1024        | <= 0.1%         | 30 720               |

### Usage

```c
// In exactly one .c file:
#define HIST_IMPLEMENTATION
#include "bench/conformance/include/hist.h"

hist_t h;
hist_init(&h, 7);            // sub_bits=7, ~2 decimal sig-figs

hist_record(&h, elapsed_ns); // record observations

uint64_t p50  = hist_percentile(&h, 50.0);
uint64_t p99  = hist_percentile(&h, 99.0);
uint64_t p999 = hist_percentile(&h, 99.9);

hist_dump_csv(&h, stderr);   // optional: dump full distribution
hist_fini(&h);
```

Or link `include/hist.c` as a separate compilation unit (no macro needed).

---

## Workload status

| ID | Name                     | Status     |
|----|--------------------------|------------|
| W1 | spawn-N-await-all        | placeholder |
| W2 | echo server              | placeholder |
| W3 | mailbox ping-pong        | placeholder |
| W4 | mutex contention         | placeholder |
| W5 | reader/writer ratio      | placeholder |
| W6 | tail latency             | placeholder |
| W7 | timer wheel              | placeholder |

Implementations arrive in M17-2 through M17-8.

---

## Reference hardware

- 32-core AMD EPYC, 64 GB RAM, no competing load.
- Pin: `taskset 0-15` to reduce NUMA noise.
- Disable: turbo boost, swap, irq-balance.
