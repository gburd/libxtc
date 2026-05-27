# M17 Results Format

This document is the authoritative specification for the stdout format
emitted by every M17 conformance benchmark binary and for the CSV schema
produced by `bench/conformance/run.sh`.

---

## 1. Binary stdout format

Each benchmark binary must write **exactly one line** to stdout, consisting
of whitespace-separated `key=value` tokens in the order shown below.

```
workload=<ID> runtime=<RT> params=<PARAMS> elapsed_ns=<N> cpu_us=<N> rss_kb=<N> p50_ns=<N> p95_ns=<N> p99_ns=<N> p999_ns=<N>
```

### Field definitions

| Field        | Type    | Description                                              |
|--------------|---------|----------------------------------------------------------|
| `workload`   | string  | Workload identifier: `W1`–`W7` (uppercase).              |
| `runtime`    | string  | Runtime name: `xtc`, `tokio`, or `erlang`.               |
| `params`     | string  | Workload parameters in `KEY=VALUE` form, joined by `:`.  |
| `elapsed_ns` | uint64  | Wall-clock time for the entire workload, nanoseconds.    |
| `cpu_us`     | uint64  | CPU time (user + sys) consumed, microseconds.            |
| `rss_kb`     | uint64  | Peak resident set size, kibibytes.                       |
| `p50_ns`     | uint64  | 50th-percentile operation latency, nanoseconds.          |
| `p95_ns`     | uint64  | 95th-percentile operation latency, nanoseconds.          |
| `p99_ns`     | uint64  | 99th-percentile operation latency, nanoseconds.          |
| `p999_ns`    | uint64  | 99.9th-percentile operation latency, nanoseconds.        |
| `rejected`   | uint64  | Count of requests rejected due to backpressure (W6 only).  Zero for workloads that do not model resource caps. |

### Rules

- All integer fields must be non-negative decimal integers with no
  separators (`12345`, not `12,345`).
- `params` must not contain whitespace.  Use `:` to separate multiple
  parameters, e.g. `N=10000:threads=4`.
- If a metric is not applicable, emit `0`.
- Additional key=value tokens after `p999_ns` are allowed and ignored
  by `run.sh`.  Workload W6 appends `rejected=<N>` immediately after
  `p999_ns` to report the count of requests that were turned away by
  the backpressure mechanism.  Other workloads may omit this field or
  emit `rejected=0`.
- Lines starting with `#` are treated as comments and ignored.

### Example

```
workload=W1 runtime=xtc params=N=10000 elapsed_ns=123456789 cpu_us=98765 rss_kb=4096 p50_ns=11234 p95_ns=18900 p99_ns=23400 p999_ns=45000
```

---

## 2. CSV schema (run.sh output)

`bench/conformance/run.sh` aggregates all binary outputs into a CSV with
the following header line followed by one data row per successful run:

```
workload,runtime,params,elapsed_ns,cpu_us,rss_kb,p50_ns,p95_ns,p99_ns,p999_ns
```

Column order and types match the binary stdout format exactly.  Absent
values default to `0`.

Lines beginning with `#` in the CSV are comments emitted by `run.sh`
(e.g. the final summary line to stderr) and are not present in stdout.

---

## 3. Invoking benchmarks

### Parameter convention

Workload parameters are passed on the command line as `--params=KEY=VALUE`
(or `--KEY=VALUE` for convenience), e.g.:

```sh
./w1_spawn/xtc/bench --params=N=10000
./w1_spawn/xtc/bench --N=10000        # shorthand accepted by convention
```

The `params` field in the output must reflect the actual values used,
regardless of how they were supplied.

### Default parameters

Each workload defines a set of defaults; running the binary with no
arguments must produce valid output using those defaults.  `run.sh` invokes
binaries without arguments.

---

## 4. Per-runtime file layout

### xtc (C)

```
w<N>_<name>/xtc/
    main.c       — benchmark source
    Makefile     — build rule; `make` produces ./bench
    bench        — compiled binary (gitignored)
```

`main.c` links against `libxtc.a` (the static library built in
`build_unix/` by the top-level build system).  Use
`include/hist.h` (with `HIST_IMPLEMENTATION`) for latency histograms.

### Tokio (Rust)

```
w<N>_<name>/tokio/
    Cargo.toml   — package manifest; name = "bench"
    src/
        main.rs  — benchmark source
    bench        — symlink or wrapper script (produced by `cargo build --release`)
```

The binary is `target/release/bench`; the wrapper script `bench` invokes it
so that `run.sh` can find a stable path.

### Erlang (escript)

```
w<N>_<name>/erlang/
    main.erl     — escript source
    bench        — executable wrapper: `#!/bin/sh\nescript "$(dirname $0)/main.erl" "$@"`
```

The `bench` wrapper calls `escript main.erl`, passing arguments through.
`main.erl` must be self-contained (no compiled `.beam` dependencies outside
the stdlib).

---

## 5. Measurement guidelines

- **Wall time**: record with `clock_gettime(CLOCK_MONOTONIC)` (C),
  `std::time::Instant` (Rust), or `erlang:monotonic_time(nanosecond)`.
- **CPU time**: `getrusage(RUSAGE_SELF)` for `ru_utime + ru_stime` (C/Erlang),
  or read `/proc/self/stat` (Linux).
- **RSS**: `getrusage(RUSAGE_SELF).ru_maxrss` (kibibytes on Linux,
  bytes on macOS — normalise to KiB).
- **Latency percentiles**: use `hist_percentile()` from `include/hist.h`
  for xtc benchmarks; any equivalent HDR-histogram library for Tokio and
  Erlang.

---

## 6. Conformance criteria

| Metric       | Pass threshold                          |
|--------------|-----------------------------------------|
| Throughput   | Within 50% of the best of the three runtimes |
| p99 latency  | Within 2× the best                     |
| Peak RSS     | Within 2× the best                     |

A failure on any criterion is published as a known gap and becomes a
tracking issue; results are never suppressed.
