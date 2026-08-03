# Microbenchmark regression suite (`bench/micro`)

Guards the most critical hot paths **release-over-release**.  If a
future change slows a guarded primitive past its tolerance, the gate
exits nonzero and prints which benchmark regressed and by how much --
so CI or the developer is told before the release ships.

This is **not** in `make check` (too noisy/slow for per-commit).  It is
an on-demand + release-time gate:

```sh
make bench-micro          # build the driver, run the checker, gate on regressions
```

## What is guarded

One driver (`bench_micro.c`) measures each path as a tight, warmed,
N-iteration loop and prints CSV (`name,ns_per_op,ops_per_sec`).  The
cases:

| benchmark          | primitive                                                     |
|--------------------|---------------------------------------------------------------|
| `proc_send_recv`   | `xtc_send` + `xtc_recv` round-trip, two procs on one loop     |
| `fiber_switch`     | `xtc_yield` throughput (full cooperative yield, not raw swap)  |
| `lwlock_excl`      | `xtc_lwlock` acquire+release, exclusive, uncontended          |
| `lwlock_shared`    | `xtc_lwlock` acquire+release, shared, uncontended             |
| `lrlock_read`      | `xtc_lrlock` wait-free read begin/end                         |
| `amutex_lock`      | `xtc_amutex` lock+unlock, uncontended                         |
| `deque_push_pop`   | `xtc_deque` owner push+pop (LIFO fast path)                   |
| `deque_steal`      | `xtc_deque` push+steal (thief FIFO CAS path)                  |
| `slab_alloc_free`  | `xtc_slab` alloc+free (magazine fast path)                    |
| `malloc_free`      | `malloc`+`free` baseline (for the slab comparison)            |
| `chan_mpsc`        | `xtc_chan_mpsc` try_send + try_recv                           |
| `chan_mpmc`        | `xtc_chan_mpmc` try_send + try_recv                           |
| `chash_get`        | `xtc_chash` get (wait-free reader)                            |
| `chash_insert`     | `xtc_chash` insert (overwrite, non-allocating)                |
| `cskip_get`        | `xtc_cskip` get (lock-free reader)                            |
| `cskip_insert`     | `xtc_cskip` insert (overwrite)                                |
| `timer_set_fire`   | `xtc_timer_set` + fire on a loop                              |

`fiber_switch` is deliberately the full `xtc_yield` cost (stack-tail
reclaim + per-fiber TLS save/restore + the run-queue turn), not the
bare `~7.6 ns/swap` `fcontext` register swap.  It is the number a fiber
that yields actually pays, so a regression in **any** part of the yield
path -- not only the asm swap -- is caught.

`proc_send_recv` and `timer_set_fire` are round-trips that include an
allocation (envelope / timer node) plus loop scheduling; they are
naturally the slowest cases and the most sensitive to host noise.

## How the estimate is taken

Each benchmark runs `N_RUNS` times; the driver reports the **minimum**,
not the mean or median.  Microbench noise is one-sided: outside
interference (other processes, cache eviction, core migration) only
ever makes a run slower, never faster, so the fastest run is the
least-perturbed estimate of the primitive's true cost.  On a loaded
host the median still absorbs interference; the minimum is the robust
floor a regression gate needs.

## How the gate works

`baseline.json` holds the accepted `ns_per_op` per benchmark plus a
`tolerance` (the allowed fractional slowdown).  `check.sh` runs the
suite and compares:

* a benchmark measured slower than `ns_per_op * (1 + tolerance)`
  **regresses** -> exit 1;
* a baselined benchmark missing from the run (dropped/renamed) -> exit 1;
* a run/setup error -> exit 2;
* everything within tolerance -> exit 0.

The default tolerance is **+100% (2x)**.  The gate's job is to catch a
hot path that materially **doubled** -- the stated goal -- not to police
micro-noise.  Microbench variance on a shared/loaded host is large and
one-sided; a tight tolerance false-positives every run.  A real 2x
regression still trips this; ordinary noise does not.  Override per-run
with `BENCH_TOLERANCE`, or edit a per-benchmark `tolerance` in the JSON.

## Baselines are MACHINE-RELATIVE

The `ns/op` numbers depend on the **CPU, compiler, and build flags**.
A baseline generated on one host is meaningless on another.  The
regression check is meant to run on a **consistent, quiescent host**:
the value is catching a big regression (a hot path going ~2x slower),
not comparing machines.

On a heavily loaded host (e.g. load average several times the core
count) even the min-of-N floor for the cheapest sub-30ns primitives can
occasionally exceed a 2x tolerance from pure scheduler interference.
That is the environment, not the gate.  **Run on an idle host** for a
trustworthy result, and regenerate the baseline on the host the gate
will run on.

## Regenerating the baseline

On the target host, after building `bench_micro`:

```sh
cd build_unix
BENCH_MICRO_BIN=./bench_micro BENCH_UPDATE=1 \
    ../bench/micro/check.sh ../bench/micro/baseline.json
```

Commit the updated `baseline.json`.  Regenerate when the intended
performance of a path legitimately changes (and the change is
reviewed), or when moving the gate to a new reference host.

## Files

* `bench_micro.c` -- the driver (emits CSV).
* `run.sh` -- runs the driver, passes through an optional iteration scale.
* `check.sh` -- runs `run.sh`, invokes `check.py` to compare/gate.
* `check.py` -- CSV-vs-baseline comparison, and (`BENCH_UPDATE=1`) baseline writer.
* `baseline.json` -- the committed, machine-relative accepted numbers.
