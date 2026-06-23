# SQLite vs sqlxtc -- comparison report

A measured A/B between the **vendored SQLite VDBE** and the
**from-scratch sqlxtc engine** (Lime parser -> `sx_classify` -> the
vectorized executor `vexec` for reads / the native write path for DML),
both running in one process over the SAME on-disk xstore B-tree, so the
only variable is the execution engine.

Harness: `bench/sqlxtc/native_vs_vdbe.c`.  It creates one table as an
xstore vtab (so the rows are visible to BOTH engines), seeds it, warms
both, then times the same query set `--iters` times under the native
driver (`sx_native_driver(1)`) and under the VDBE
(`sx_native_driver(0)`).  Row counts are checked equal on every run, so
this is a differential as well as a benchmark.

Numbers below are from an 8-core x86-64 Linux dev box, `-O2`, best of a
warmed run.  "vdbe/native" > 1.0 means sqlxtc is faster.

## 20,000 rows, 300 iterations/query

| query                                              | native | vdbe   | vdbe/native |
|----------------------------------------------------|-------:|-------:|------------:|
| `SELECT count(*) FROM t`                           | 2208us | 2171us | 0.98x |
| `SELECT count(a),sum(a),min(a),max(a) FROM t`      | 3087us | 3751us | **1.21x** |
| `SELECT b,count(*),sum(a) FROM t GROUP BY b`       | 3884us | 7746us | **1.99x** |
| `SELECT k,a,b FROM t WHERE a > 5000`               | 5259us | 5203us | 0.99x |
| `... WHERE a>1000 AND a<9000 ORDER BY a LIMIT 50`  | 5195us | 3387us | 0.65x |
| `SELECT k,b FROM t WHERE b LIKE 'r5%'`             | 3585us | 3846us | **1.07x** |
| `SELECT a FROM t WHERE a IN (SELECT a FROM t ...)` |19398us | 5402us | 0.28x |

## 100,000 rows, 60 iterations/query

| query                                              | native  | vdbe   | vdbe/native |
|----------------------------------------------------|--------:|-------:|------------:|
| `SELECT count(*) FROM t`                           | 11249us |11352us | 1.01x |
| `SELECT count(a),sum(a),min(a),max(a) FROM t`      | 16431us |19588us | **1.19x** |
| `SELECT b,count(*),sum(a) FROM t GROUP BY b`       | 19164us |43226us | **2.26x** |
| `SELECT k,a,b FROM t WHERE a > 5000`               | 25766us |25835us | 1.00x |
| `... ORDER BY a LIMIT 50`                          | 28724us |17281us | 0.60x |
| `SELECT k,b FROM t WHERE b LIKE 'r5%'`             | 18232us |18406us | 1.01x |
| `SELECT a FROM t WHERE a IN (SELECT a FROM t ...)` |424974us |28068us | 0.07x |

## What the numbers say

**Where sqlxtc already wins**

- **Aggregation, especially GROUP BY: 2.0-2.3x faster, and the lead
  grows with row count.**  vexec aggregates a vector of rows per call
  through a tight typed loop with no per-row bytecode dispatch; the VDBE
  interprets an opcode stream per row.  This is the DuckDB-shaped win
  and it is the whole point of the vexec design.
- **Full multi-aggregate (count/sum/min/max in one pass): ~1.2x.**
- **Scans (count(\*), range filter, LIKE): parity (~1.00x).**  Both
  read the same B-tree; the executor overhead is in the noise against
  the scan + decode cost.

**Where sqlxtc is currently slower (known, with clear fixes)**

- **ORDER BY ... LIMIT: 0.60-0.65x.**  vexec materializes the whole
  result, sorts it, then applies LIMIT.  The VDBE keeps a bounded top-N
  heap.  Fix: a top-N operator in vexec when LIMIT is present and the
  sort key is cheap.
- **Uncorrelated `IN (SELECT ...)`: 0.07-0.28x, and it gets WORSE with
  scale.**  In this harness each `db_exec` is a fresh statement, so the
  subquery is re-run and re-materialized on every outer call.  Fix:
  cache the materialized IN-list across executions of a prepared
  statement, or hoist the subquery to a one-time CTE-style materialize.
  This is the single biggest outlier and the top optimization target.

**The structural caveat (and why this benchmark understates sqlxtc)**

Every `db_exec` here re-parses (Lime) and re-plans (vexec compile) the
query -- there is no prepared-statement plan cache on this path.  So the
per-call numbers fold parse+plan into execution.  The VDBE in this
harness also re-prepares, so the comparison is fair, but BOTH would
benefit from caching, and sqlxtc's parse+plan is the larger fixed cost
today.  The server's `db_exec_cached` path DOES cache the `sx_stmt`, so
the deployed latency is lower than these raw `db_exec` numbers.

**What this benchmark does NOT measure (sqlxtc's architectural reason
for existing)**

The VDBE is single-threaded per connection and SQLite serializes
writers with a database-level lock.  sqlxtc's reasons to exist are
orthogonal to single-query latency:

- **Morsel-parallel scans across libxtc loops** (`vx_run_parallel`): a
  big aggregate/scan fans out over N event loops on N cores.  At small
  scale the ~3ms loop-spin-up cost dominates (see
  `VEXEC_RESULTS.md`); at 500k+ rows the parallel path pulls ahead.
- **Concurrent writers** via the B-link tree (parallel-writer crabbing)
  and MVCC snapshots -- no global write lock.  SQLite's
  `BEGIN IMMEDIATE` serializes; sqlxtc's `test_sqlxtc_mt.sh` runs one
  writer loop per CPU concurrently.
- **Larger-than-RAM** via the LeanStore-style cooling buffer pool
  (`bufmgr.c`), where SQLite's pager is page-cache bound.

Those are measured in `engine_ab.c` (larger-than-RAM A/B),
`engine_mt.c` (concurrent throughput), and `MT_RESULTS.md`.

## Honest headline

For **single-query, in-memory, latency-bound** work sqlxtc is roughly at
parity on scans, **clearly faster on aggregation/GROUP BY (up to
2.3x)**, and **slower on ORDER-BY-LIMIT and uncorrelated IN-subqueries**
until the top-N and subquery-materialization-caching optimizations land.
The architecture's real wins -- parallelism, concurrent writers,
larger-than-RAM -- are by design and show up in the multi-core /
multi-writer / out-of-core benchmarks, not in this single-threaded
latency A/B.
