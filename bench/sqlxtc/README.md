# sqlxtc benchmarks

This directory measures the **libxtc concurrency model** under an
OLTP-shaped workload, on the sqlxtc MVCC engine
(`examples/06_sqlxtc/mvcc.c`).  It is reproducible, statistically
aggregated, and -- importantly -- honest about what it does and does
not yet measure.

## What this measures, and what it does not (read this first)

**Measured today:** the MVCC engine + the libxtc runtime end to end on
a key/value OLTP workload -- throughput and latency percentiles
(p50/p95/p99/p99.9/max) as a function of core count and read/write mix.
This is a direct, honest proxy for the project's actual goal:
*predictable, fast, low-p99-variability* concurrency on libxtc.

**NOT measured yet, and why:**

  * **A full SQL TPC-C vs SQLite comparison.**  TPC-C end to end is
    still future work, but the storage integration it depended on now
    EXISTS: the from-scratch MVCC + B-tree engine runs SQL underneath
    SQLite's VDBE through the `xstore` virtual table
    (`examples/06_sqlxtc/xstore.c`).  `engine_ab.c` / `ENGINE_AB.md`
    therefore DO measure SQL-on-our-engine vs SQLite directly, on a
    larger-than-RAM point workload under an equal memory budget (see
    below).  A full TPC-C transaction profile on top of that is the
    remaining step.
  * **HammerDB TPROC-C/TPROC-H.**  HammerDB's drivers are Oracle, SQL
    Server, Db2, MySQL/MariaDB, and PostgreSQL.  It has **no SQLite
    driver and no custom-protocol (Quack) driver**, so it cannot drive
    either side of the intended comparison.  A fair comparison needs a
    load generator that speaks both embedded-SQLite and the engine; the
    `mvcc_bench` here is the engine-side load generator.  A Quack
    HammerDB Tcl driver (to drive the networked server) is future work.
  * **Larger-than-RAM working sets.**  Done, for the SQL path: the
    `xstore` virtual table carries versioned rows in the on-disk
    B-tree + cooling buffer pool (`bufmgr.c`), so `engine_ab.c` runs a
    working set far larger than the cache (24x in the recorded runs)
    and both engines page to disk.  The standalone `mvcc_bench`
    demonstrator remains in-memory.

These are stated plainly so results are not over-claimed; the project's
discipline (see `../../the design notes`) is honest, tested,
CI-verified work.

## The headline result so far: a real bug, found and fixed

The first run of `mvcc_bench` immediately exposed a serious libxtc bug.
Read-heavy throughput **collapsed** with more cores (402 kops/s at 1
core to 15.7 at 4) and max op latency hit multiple seconds.  The cause
was a **lost-wakeup race in the cross-loop receive path**
(`__do_recv`): the receiver confirmed its mailbox empty under a lock,
released it, and only then re-locked to arm its waker -- a sender
delivering in that gap fired no waker, so the receiver stalled to its
timeout.  Benign on one loop (cooperative; no concurrent sender in the
gap), fatal across loops.  The correctness tests never caught it -- they
completed, just slowly within their timeouts.  Fixed in commit
`691726f` (arm the waker under the same lock that confirms empty).
After the fix: 4-core read-heavy went **15.7 -> 402 kops/s (25x)** and
max latency **4.29 s -> ~3 ms**.  This is exactly why the benchmark
exists.

## Running it

```sh
# local sweep: 5 reps, 32 clients, 15000 ops/client, 1M-key space
CORES="1 2 4 8" READ_PCTS="100 80 50 0" ./run.sh 5 32 15000 1000000
python3 stats.py results/*.jsonl
```

`run.sh` builds libxtc (if needed) and the load generator, sweeps the
matrix (pinning to physical cores with `taskset`), runs each cell REPS
times, and appends one JSON object per run to
`results/<host>-<UTC>.jsonl`.  `stats.py` groups by (host, read%, cores)
and reports the **median across reps with the inter-quartile range** --
median is robust to scheduler outliers, and the IQR is the run-to-run
variability that "low p99 variance" is about.

It runs on any host with a C toolchain; results in `results/` here were
collected on **floki** (dev box) and **meh** (24-core Linux server).
The harness is portable to nuc (FreeBSD) and arnold; remote use is just
`rsync` the tree, then `bash -l run.sh` (note: some hosts default to
fish, so invoke bash explicitly).

## Resource constraining

  * **Cores:** the executor uses N loops = N cores, and `run.sh` pins
    the process to cores `0..N-1` with `taskset`.  A SQLite comparison
    (once the SQL engine is on our storage) constrains SQLite to the
    same core set with the same `taskset`.
  * **Memory / buffer pool:** the engine is currently in-memory; the
    larger-than-RAM matrix (a capped buffer pool with a working set
    several times its size) lands with the storage integration, at
    which point SQLite is constrained to an equal `PRAGMA cache_size`.

## What the data shows (see results/ + `python3 stats.py`)

  * **No more collapse or multi-second stalls** at any core count --
    the fix holds; max latency is bounded in the millisecond range.
  * **Scaling depends on work-per-message.**  Every operation is a
    cross-loop RPC, so for trivial ops (a hash lookup) the messaging
    cost dominates: on the fast dev box throughput is roughly flat
    across cores; on the 24-core server (higher per-op cost relative to
    messaging) it scales positively (about 1.8x at 8 cores for
    read-heavy) and p50 latency drops as load spreads.  The takeaway is
    architectural, not a bug: tiny ops over per-op RPC are
    messaging-bound.  The `PERF_IDEAS.md` ideas (co-locate clients with
    their shard, batch, per-loop coordinators, bigger ops) target
    exactly this, and real SQL statements are far heavier than a hash
    lookup, so they amortize the messaging far better.
  * **Writes cost more than reads** and scale less, because all writes
    funnel through the single 2PC coordinator (a known bottleneck;
    per-loop coordinators are in `PERF_IDEAS.md`).

## Files

```
mvcc_bench.c   the YCSB-shaped load generator (links mvcc.c + libxtc)
run.sh         matrix sweep -> results/<host>-<UTC>.jsonl
stats.py       aggregate JSONL -> median + IQR table, scaling factors
engine_ab.c    SQL-on-our-engine vs SQLite, larger-than-RAM, equal cache
run_ab.sh      build + run the A/B matrix -> results/engine_ab-<host>.jsonl
ENGINE_AB.md   the A/B method, results, and honest analysis
results/        raw JSONL (floki, meh) -- committed for reproducibility
PERF_IDEAS.md   collected performance-improvement ideas
```

## The path to the full comparison

1. [DONE] Wire the MVCC + B-tree engine under SQL so sqlxtc runs SQL on
   the libxtc-native engine -- the `xstore` virtual table
   (`examples/06_sqlxtc/xstore.c`) does this.
2. [DONE] Larger-than-RAM workloads with SQLite constrained to an equal
   cache -- `engine_ab.c` / `ENGINE_AB.md` (24x working set/cache; reads
   competitive, writes slower by the MVCC-without-GC margin).
3. Add a Quack HammerDB Tcl driver (or a TPC-C load generator that
   speaks both Quack and embedded SQLite) so the networked path is
   measured alongside the embedded one.
4. Run the full core x memory matrix on meh / nuc / arnold and report
   with the same statistics.

Each step is real, testable, and CI-gated; none of it is faked here.
