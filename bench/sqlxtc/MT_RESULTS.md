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

## Group-commit WAL writer on loops (engine_loop) -- fast-NVMe finding

Driving the SQL workload as libxtc procs on a multi-loop executor with
the group-commit WAL writer (engine_loop) vs off-loop OS threads each
doing a synchronous commit (engine_mt).  Write-heavy (50% read),
in-cache, on arnold (20 cores, NVMe).  Median kops/s:

| cores | off-loop sync (engine_mt) | on-loop group-commit (engine_loop P=4) |
| ----: | ------------------------: | -------------------------------------: |
|   1 |  676 | 201 |
|   4 |  612 | 201 |
|   8 |  338 | 162 |
|  16 |  177 | 131 |
|  20 |  142 | 123 |

Finding: on fast local NVMe the group-commit path is NOT faster -- it
loses at low concurrency and only converges at high.  fsync is cheap
here, so batching saves little, while the group-commit machinery (a
message round-trip to the writer proc + park + ack per commit) is pure
overhead versus a direct fdatasync.  Group commit pays off when fsync is
EXPENSIVE (slow/secure-erase disks, F_FULLFSYNC, replicated/remote logs)
or at very high committer counts; it is a scale-out / durable-remote
optimization, not a local-NVMe one.  The real local write ceiling is
leaf-latch contention -- both paths decline with concurrency as writers
serialize on hot pages -- which is the next thing to attack, not the
commit path.

Two improvements did land from this work:
  * wal.c: pipelined group commit (drain non-blocking + flush; no fixed
    gather window), which removes the window latency that made the
    writer lose to synchronous commit even at low load.
  * src/ptc/sync.c: a real use-after-scope crash in xtc_arwlock's
    fiber-waiter wake path, found by the high-fiber-concurrency write
    workload and fixed (wake under the lock).

## Parallel-writer B-tree: optimistic latch coupling for bt_insert

The write decline (writers serializing on the root's exclusive latch)
is fixed by descending shared and taking only the leaf exclusive.
Write-heavy (50% read), in-cache, arnold (20c, NVMe), median kops/s:

| cores | bt_insert BEFORE (excl. from root) | AFTER (optimistic) | gain |
| ----: | ---------------------------------: | -----------------: | :--- |
|   1 |  676 |  522 | 0.77x (re-latch overhead) |
|   2 |   -- |  957 | -- |
|   4 |  612 | 1169 | 1.91x (peak) |
|   8 |  338 |  764 | 2.26x |
|  16 |  177 |  290 | 1.64x |
|  20 |  142 |  227 | 1.60x |

Writes now SCALE to a ~1169 kops peak at 4 threads instead of declining
monotonically, and are ~1.6-2.3x higher at 8-20 threads.  Trade-off:
single-thread is ~23% lower (the optimistic path re-latches the leaf
shared->exclusive, and a split-bound insert pays a second, pessimistic
descent).  The residual decline past 4 threads is the SPLIT path -- still
pessimistic exclusive-coupling from the root -- plus the single global
commit-clock atomic (g_xclock); making splits optimistic (B-link sibling
chasing) and sharding the clock are the next steps, with diminishing
returns and rising complexity.

## B-link splits + HLC commit clock (7dc290b / c5ae116)

The split path was rewritten as a B-link (Lehman-Yao) tree: all four
descents go latch-free with move-right (follow the right-link past a
concurrent split), and splits serialize on one parking-safe per-tree
SMO lock instead of holding an exclusive latch from the root.  A split
no longer blocks reads or non-split writes.

Write-heavy (50% read), in-cache, arnold (20c, NVMe), best of 3 kops/s:

| cores | optimistic insert (f744bd6) | B-link (7dc290b) | gain |
| ----: | --------------------------: | ---------------: | :--- |
|   1 |  522 |  630 | 1.21x (single-thread regression recovered) |
|   2 |  957 |  997 | 1.04x |
|   4 | 1169 | 1346 | 1.15x (peak, 2.1x over 1c) |
|   8 |  764 | 1182 | 1.55x |
|  16 |  290 |  565 | 1.95x |
|  20 |  227 |  381 | 1.68x |

B-link both recovers the single-thread regression the optimistic path
introduced (630 vs 522) and lifts high-thread throughput 55-95% at
8-20 threads; the post-peak decline is far gentler.  Reads are
unharmed (100% read, in-cache: 1145 / 2387 / 2226 / 1895 kops at
1 / 4 / 8 / 20 cores).

The commit clock is now a Hybrid Logical Clock (wall-clock ms in the
high 48 bits, logical counter in the low 16) instead of a bare atomic
counter.  In a single process this does not change throughput -- it is
still one CAS per commit -- but it anchors stamps to wall-clock time
and adds the merge-on-observe rule that makes the same stamp a valid
causal timestamp once the engine is distributed (the chosen causality
model).  Interval Tree Clocks were rejected: ITC tracks a partial
causal order, not a single totally-ordered stamp, so it cannot serve
as an MVCC commit timestamp.

Remaining write-scaling levers (diminishing returns): full split-vs-split
parallelism (drop the SMO lock for fine-grained per-level latching) and
batching/sharding the commit-clock CAS under very high commit rates.
