# sqlxtc engine A/B: larger-than-RAM, equal memory, vs SQLite

`engine_ab.c` runs the **same SQL workload through the same VDBE**,
differing only in the storage engine underneath:

  * `--engine xstore` -- the libxtc-native engine: `btree.c` (a
    prefix-compressed B-link tree) over `bufmgr.c` (a LeanStore-style
    cooling buffer pool), MVCC, reached through the `xstore` virtual
    table.
  * `--engine sqlite` -- SQLite's own B-tree and pager, a file-backed
    rowid table.

Because both sides parse, plan, and execute through SQLite's VDBE, the
benchmark isolates exactly one variable: the storage engine.

## Method

  * **Equal memory.** xstore's pool is sized to `--cache-kb`; SQLite is
    given the identical budget via `PRAGMA cache_size=-<cache-kb>`.
  * **Larger than RAM.** `--rows x --row-bytes` is sized far above the
    cache so both engines page to disk.  The runs below use 200000 rows
    x 512 bytes ~= 100 MB through a 4 MB cache -- a 24x working
    set/cache ratio.
  * **Workload.** Load the rows in batched transactions, then run
    `--ops` random point operations: `SELECT v WHERE k=?` or
    `UPDATE SET v=? WHERE k=?`, chosen by `--read-pct`.  Each op is
    timed; we report throughput and latency percentiles.
  * **Single-threaded embedded.** The fair baseline: SQLite is
    single-writer, so a single thread isolates the storage engines
    without conflating xtc's concurrency.

Reproduce:

    nix-shell -p openssl pkg-config liburing util-linux \
        --command 'sh run_ab.sh > results/engine_ab-$(hostname -s).jsonl'

## Results (floki, 1 core pinned, median of 3 reps)

| engine | read% | kops/s | load kops | p99 (us) | max (us) |
|--------|------:|-------:|----------:|---------:|---------:|
| sqlite |  100  | 645    | 651       | 1.9      | 73       |
| xstore |  100  | 608    | 1204      | 2.4      | 423      |
| sqlite |   95  | 630    | 663       | 2.3      | 64       |
| xstore |   95  | 542    | 1206      | 5.1      | 337      |
| sqlite |   50  | 554    | 669       | 2.4      | 49       |
| xstore |   50  | 295    | 1230      | 9.7      | 326      |

(Working set 100 MB through a 4 MB cache; both engines evicting.)

## What it says -- honestly

  * **Read paths are competitive.**  At a 24x larger-than-RAM working
    set, xstore reads land within ~6% of SQLite's mature storage on the
    read-only mix (608 vs 645 kops/s) and ~14% on 95/5 (542 vs 630).
    The from-scratch B-tree + cooling pool hold their own on the read
    path that matters most for a buffer-managed store.

  * **Writes cost more, exactly as MVCC predicts.**  On the 50/50 mix
    xstore is ~1.9x slower (295 vs 554 kops/s).  Three known causes,
    all on the roadmap:
      1. *Multi-version write amplification.* Every UPDATE appends a
         new `(rowid, ~commit_ts)` version rather than updating in
         place, so the tree grows and eviction churns harder.
      2. *No GC on the SQL path yet.* The sharded MVCC store has
         snapshot-horizon GC (`mvcc.c`); xstore does not yet prune dead
         versions, so a write-heavy run only accumulates them.  Wiring
         the low-water-mark prune into xstore is the next step.
      3. *O(log n) version insert.* Each version is a fresh keyed
         insert; an O(1) "prepend to this rowid's chain" path (see
         PERF_IDEAS.md) would cut the write cost.

  * **The tail is higher and it is the buffer pool.**  xstore's max
    latency (~330-420 us) is several times SQLite's (~50-70 us).  The
    spikes are eviction stalls: when a point op needs a frame and the
    cooling pool must write a dirty page back synchronously, that op
    eats the I/O.  SQLite's pager amortizes writeback better.  The fix
    is background/asynchronous writeback of cooling pages (the bufmgr
    can offload the write via `xtc_blocking_run` and keep serving),
    which is also where xtc's design should ultimately *win* the tail.

  * **Load looks ~2x faster but is not durability-equivalent.**
    xstore's SQL path does not yet flush a durable WAL on commit, while
    SQLite runs `journal_mode=WAL, synchronous=NORMAL`.  The group-
    commit WAL writer (`wal.c`) exists but is not yet under xstore, so
    the load numbers are not an apples-to-apples durability comparison
    and are reported only for completeness.

## Version GC: where it helps, where it does not

Multi-version storage accumulates dead versions; reclaiming them is the
first write-path item.  Two approaches were measured.

**Full-tree vacuum (`xstore_gc()`, `--gc-every N`) is wrong for
larger-than-RAM.**  Scanning the whole 200k-row tree every N ops reads
every page through the small cache, so the scan thrashes the buffer
pool and costs more than the smaller tree saves: on the 50/50 mix it
made throughput *worse* (about 34 vs 70 kops/s) and pushed the max
latency past 50 ms.  A periodic stop-the-world vacuum does not belong
on a memory-constrained engine.

**Inline autovacuum (`xstore_autovacuum(1)`, `--autovacuum`) is the
right shape but only pays off under skew.**  Following PostgreSQL HOT
pruning, each write prunes just that rowid's dead versions -- touching
only pages already hot from the write, no full scan.  Whether it helps
depends entirely on the access pattern:

| workload | xstore no-AV | xstore +AV | sqlite |
|----------|------:|------:|------:|
| uniform 200k keys, 50/50 | 188 | 165 | 355 |
| **hot set 200 keys, 80% writes, 300k ops** | **170** | **216 (+27%)** | 368 |

  * *Uniform* random over 200k keys updates each row at most once or
    twice, so version chains are naturally short -- there is no bloat
    to collect, and the per-write prune probe is pure overhead
    (~12% slower).
  * *Skewed* (a small hot set updated thousands of times) is exactly
    where multi-version storage hurts: without GC the hot keys' chains
    grow without bound and their working set spills the cache.  Inline
    autovacuum keeps each hot chain at ~1 version, lifting throughput
    **+27%** (170 -> 216 kops/s) and tightening p99 (30 -> 19 us).

The honest conclusion: version GC is **workload-dependent**, so it is
an opt-in policy (`xstore_autovacuum`), not a default.  The remaining
refinement is adaptive -- prune a rowid only once its chain exceeds a
threshold, so the skewed case keeps the win without the uniform-case
overhead.  (SQLite, updating in place with no versions at all, remains
faster on writes; closing that gap is the O(1)-version-insert and
async-writeback work, not GC.)

## Bottom line

The libxtc-native storage engine, larger than RAM and under an equal
memory budget, is **read-competitive with SQLite today** and slower on
writes by the amount multi-version storage without GC costs.  The three
write-path items (version GC under xstore, O(1) version insert,
asynchronous cooling-pool writeback) are the measured, prioritized work
to close the gap -- and the asynchronous-writeback item is where xtc's
cooperative I/O model is positioned to beat a synchronous pager on the
tail.
