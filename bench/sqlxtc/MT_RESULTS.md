# Multi-threaded A/B: sqlxtc (xstore) vs SQLite

`engine_mt.c` -- same random point-op SQL workload, T OS threads, one
shared store, equal total cache.  Run on `meh` (24 cores, 125 GB, NixOS
x86_64).  Median kops/s over reps; p99 in microseconds.  Working set
~95 MB (500k rows x 200 B).

IMPORTANT: this is the OFF-LOOP path.  sqlxtc's concurrency is designed
for libxtc loops/fibers with a group-commit WAL writer; raw OS threads
hit the un-optimized fallback -- the buffer-manager latch serves
off-loop waiters on a condvar, and commits go through wal_commit_sync
(one fsync each, serialized by a mutex, NO group commit).  So writes
here understate the intended (loop) deployment.  mvcc_bench and the
networked server measure the loop path.

## In-cache (256 MB cache, fits the set)

| read% | thr | xstore kops | sqlite kops | x/sqlite |
| ---:  | --: | ----------: | ----------: | :------- |
| 100 |  1 |  351 |   92 | 3.8x |
| 100 |  4 |  649 |  626 | 1.0x |
| 100 | 16 |  584 | 1245 | 0.47x |
|  95 |  1 |  164 |   72 | 2.3x |
|  95 |  8 |  408 |  240 | 1.7x |
|  95 | 16 |  120 |  281 | 0.43x |
|  50 |  2 |  173 |   63 | 2.8x |
|  50 | 24 |   41 |   46 | 0.88x |

## Larger-than-RAM (4 MB cache, ~24x over)

| read% | thr | xstore kops | sqlite kops | x/sqlite |
| ---:  | --: | ----------: | ----------: | :------- |
| 100 |  8 |  132 | 1151 | 0.11x |
|  95 |  1 |  101 |   95 | 1.06x |
|  50 |  4 |  118 |   53 | 2.2x  |
|  50 | 24 |   18 |   45 | 0.40x |

## Findings

  1. Single-threaded, in-cache: xstore is 1.8-3.8x faster than SQLite
     (efficient MVCC read path + cooling pool).
  2. xstore read scaling plateaus ~4-8 threads then loses to SQLite,
     because every page fix takes a single global mutex
     (bufmgr.c ht_lookup_pin / ht_mu).  SQLite's per-connection caches
     have no shared lock and scale further.
  3. Mixed read/write collapses xstore at high thread counts: off-loop
     commits serialize on one fsync each (no group commit), stalling
     readers behind exclusive leaf latches.  This is the fallback path,
     not the loop/group-commit path.
  4. Larger-than-RAM reads favor SQLite (xstore's paging read path adds
     ht_mu + cooling-pool overhead per fix); moderate-write larger-than
     -RAM at 2-8 threads favors xstore (MVCC append + double-write beats
     WAL + checkpoint under paging).

## Top fixes implied

  * Shard ht_mu (per-bucket locks) or make the page-fix lookup
    lock-free / RCU -- the #1 read-scaling bottleneck.
  * Drive the SQL path on libxtc loops/procs so the group-commit writer
    is used (the designed write-scaling path), instead of off-loop
    per-commit fsync.

## After the fix: striped page-table lock (commit "stripe the buffer-manager page-table lock")

Re-run on meh, in-cache (256 MB), 100% read, median kops/s.  The single
global `ht_mu` is replaced by 256 striped bucket locks.

| thr | xstore BEFORE | xstore AFTER | gain | sqlite |
| --: | ------------: | -----------: | :--- | -----: |
|   4 |  649 |  868 | 1.34x |  400 |
|   8 |  647 | 1184 | 1.83x | 1087 |
|  16 |  584 | 1515 | 2.59x | 1113 |
|  24 |  569 | 1553 | 2.73x | 1292 |

xstore now scales on reads and OVERTAKES SQLite at 16-24 threads (1553
vs 1292 kops at 24 threads, where before it lost 569 vs 1245).  A stripe
-count sweep (32/64/128/256) gives ~1520-1565 kops at 24 threads
regardless, confirming the page-table contention is gone.  Single-thread
is stripe-count-independent (~220 kops), i.e. unaffected by the change
(run-to-run session variance dominates).

The 95%-read mixed case improves only modestly (e.g. 16 threads
120 -> 132): there the bottleneck is the SECOND issue -- off-loop
synchronous commits with no group commit -- which a bufmgr lock change
cannot help.  That requires driving the SQL path on libxtc loops/procs
so the group-commit WAL writer is used; it is the next optimization.
