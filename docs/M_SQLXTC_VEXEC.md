# M_SQLXTC_VEXEC -- a vectorized execution engine to replace the VDBE

Status: V0-V2 LANDED (expression evaluator, morsel parallelism); columnar vectors + V3-V6 planned.  This document scopes a
from-scratch execution engine for sqlxtc -- a DuckDB-style vectorized,
push-based, morsel-parallel executor built on the libxtc concurrency
model -- that replaces SQLite's VDBE (the bytecode interpreter,
sqlite3VdbeExec) while keeping SQLite's tokenizer, parser, and query
planner intact.

The replacement must produce results IDENTICAL to the VDBE for every
query, except that when the query does not specify an order
(no ORDER BY at the top level) the rows may be returned in any order.
That order-independence is the functional-equivalence test oracle:
run a query through both engines, sort both result sets, compare.

## Why, and why only the VDBE

The VDBE is a scalar, tuple-at-a-time bytecode interpreter: one row
flows through the opcode program at a time, every operator is a branch
in a giant switch, and a single statement runs on a single thread.
That is the opposite of what libxtc is for.  DuckDB showed the modern
shape: columnar, vectorized (a "chunk" of ~2048 values per operator
call so the per-tuple interpreter overhead amortizes and the inner
loops auto-vectorize), push-based pipelines, and morsel-driven
parallelism (the scan is sliced into morsels handed to a pool of
workers, operators are re-entrant per-worker, and a final combine
phase merges per-worker state).  Mapping morsels onto libxtc procs --
one pipeline-worker proc per executor loop, work-stealing already
provided by the multi-loop executor -- is the natural fit.

Scope is the EXECUTOR only.  SQLite's tokenizer + parser
(sqlite3RunParser) and planner (sqlite3WhereBegin and the Select
rewrite) stay: they already produce a correct logical plan and the
canonical VDBE opcode program.  We intercept that program, recognize
the patterns the planner emits, lower them to a vectorized physical
plan, and run it.  Building a parser/planner too is a separate,
larger project and is explicitly out of scope.

## The interception seam

After sqlite3_prepare_v2, a prepared statement holds a Vdbe with an
opcode array (Vdbe.aOp[], VdbeOp records: opcode, p1..p5, p4 typed
operand).  This bytecode is a stable, fully-documented IR and is the
interception point:

  1. Prepare as normal -- the planner runs, producing aOp[].
  2. A RECOGNIZER walks aOp[] and matches the canonical templates the
     planner emits for the supported query shapes (below).  On a match
     it builds a vectorized physical plan (a tree of push operators)
     and marks the statement "vexec".  On any unrecognized opcode or
     shape it leaves the statement as a normal VDBE program -- so the
     engine is always a strict superset of the VDBE: anything we do
     not yet vectorize still runs correctly on the interpreter.
  3. sx_step on a vexec statement drives the vectorized pipeline and
     emits result chunks; sx_step on a normal statement calls the VDBE
     as today.

Recognizing opcodes rather than the AST is deliberate: the opcode
program is a smaller, stabler, and already-optimized surface than the
Select/Expr trees, and matching it guarantees we honor exactly the
plan the planner chose (same join order, same index choices).

(V0 deviation: for P1 -- a single base table with no join and no index
choice -- there is no plan decision to preserve, so V0 recognizes from
the Lime AST instead of the opcode program; it is simpler and the
result is identical for that shape.  From P5 (joins) on, where join
order and index selection matter, recognition moves to the opcode
program as specified above.)

Supported query shapes, in build order (each falls back to the VDBE
until implemented):
  P1. single-table scan + projection + WHERE filter
      (OP_OpenRead / OP_Rewind / OP_Column / OP_ResultRow / OP_Next)
  P2. + scalar expressions and the common SQL functions in projection
      and filter (a vectorized expression evaluator)
  P3. + aggregation (OP_AggStep / OP_AggFinal), hash GROUP BY
  P4. + ORDER BY / LIMIT (a vectorized sort operator)
  P5. + joins (the planner's nested-loop and the hash-join the
      vectorized engine adds), starting with INNER, then LEFT
  P6. + subqueries / set operations as the recognizer learns them

## The vectorized data model

  - A DataChunk is a small set of columns, each a Vector of up to
    VEXEC_VECTOR_SIZE (2048) values, plus a selection vector (the
    subset of rows still live after filters, so filters never compact
    until necessary).  This is the DuckDB chunk model.
  - Column types map from SQLite's storage classes (INTEGER, REAL,
    TEXT, BLOB, NULL); TEXT/BLOB vectors hold (ptr, len) with the
    bytes owned by an arena per chunk.  NULLs are a validity bitmask
    per vector (no sentinel values).
  - Vectors support the flat, constant, and dictionary encodings
    DuckDB uses, so a constant or a low-cardinality column costs O(1)
    per chunk.

## Push-based pipelines, morsel-parallel on libxtc

A physical plan is a set of PIPELINES.  A pipeline is a chain of
operators from a source (a table scan or a build-side hash table) to a
sink (the result, an aggregate's hash table, or a sort's run buffer).
Operators implement the push interface:
  - source: GetMorsel() -> a DataChunk (or done); the scan slices the
    table's key range into morsels.
  - operator: Execute(in_chunk) -> out_chunk (filter, project, probe).
  - sink: Sink(chunk) per worker into per-worker local state, then
    Combine() to merge worker states, then Finalize().

Parallelism is morsel-driven, mapped onto the libxtc executor:
  - xtc_exec gives N loops (one OS thread each); we spawn one
    pipeline-worker proc per loop.
  - Each worker loops: pull a morsel from the source (an atomic cursor
    over the table's key space -- the B-tree's O(1) parked-cursor
    resume makes a morsel a [start_key, count] slice), run it through
    the pipeline's operator chain, Sink into THIS worker's local state.
  - Workers never share mutable operator state on the hot path: each
    has its own hash table / sort run / aggregate accumulator (the
    single-owner, no-locks-on-the-hot-path libxtc discipline).  Only
    Combine (rare, once per pipeline) merges them, under a brief lock.
  - A blocking sink (build a hash table, sort) is a pipeline boundary:
    the next pipeline starts only after the previous one's Combine.
    The scheduler is the executor's run loop; back-pressure is natural
    (a worker that gets no morsel parks).
  - The storage reads are already async: a scan morsel that misses the
    buffer pool parks the worker fiber on xtc_aio page I/O and the loop
    runs another worker -- so I/O latency overlaps compute for free,
    the Seastar/Tokio property the VDBE cannot exploit.

MVCC integration: a vexec pipeline reads at the transaction's snapshot
exactly as the xstore cursor does (the same commit_ts visibility
filter on the version keys), so isolation is unchanged.  A vexec
statement that writes falls back to the VDBE for the write for now;
read pipelines are the parallel win.

## Correctness and the differential test oracle

The functional gate is differential testing against the VDBE:
  - A corpus of queries (start from the existing sqlxtc + SQLite test
    SQL, then a generated grammar of SELECTs over seeded tables).
  - For each: run through the VDBE (force fallback) and through vexec;
    canonicalize both result sets (sort rows; the query's own ORDER BY
    is preserved and checked positionally, otherwise compare as
    multisets) and assert equality of values AND types AND NULLs.
  - Property tests (hegel-c): for random tables and random supported
    queries, vexec result multiset == VDBE result multiset.
  - Concurrency: many vexec statements + concurrent writers under each
    isolation level; results must match a VDBE run at the same
    snapshot.
  - ASan/UBSan/TSan on the parallel pipeline; the morsel cursor and
    per-worker state are the race surface to clear.

A query the recognizer does not support MUST fall back to the VDBE and
still pass, so the oracle also guards that the fallback is never wrong.

## Milestones

  V0  Recognizer + fallback plumbing: intercept the query, match P1
      (scan+filter+project), build a single-pipeline single-worker
      vectorized executor; everything else falls back.  Differential
      oracle stood up.  Gate: P1 queries match the VDBE; all else
      falls back and still passes.
      DONE (vexec.c / test_vexec.c).  V0 recognizes P1 from the Lime
      AST (single base table, projection of plain columns or *,
      optional WHERE of one column-OP-literal), runs it as a chunked
      pipeline sourced from the engine's own cursor (so MVCC and value
      decoding match the VDBE), and applies the filter/projection
      itself.  The recognizer is conservative: joins, aggregates,
      ORDER BY, LIMIT, DISTINCT, GROUP BY, compound/IN predicates, and
      expression projections all fall back, as does any WHERE where
      column affinity could coerce the literal (the affinity
      no-coercion gate, after the affinity = '5' vs INTEGER column bug
      was caught by the differential oracle).  16 P1 queries match the
      VDBE, 11 fall back; clean under ASan+UBSan.
  V1  Vectorized expression evaluator (P2) + the chunk/vector/selection
      model with NULL bitmasks and dictionary encoding.
      EXPRESSION EVALUATOR DONE (vexec.c / test_vexec.c).  Projection
      and WHERE are compiled from the Lime AST into a vexec expression
      tree and evaluated per row: column refs, INTEGER/REAL/TEXT/NULL
      literals, arithmetic (+ - * / % with SQLite int/real promotion,
      integer division, x/0 -> NULL), || concat, comparisons with
      3-valued NULL logic, AND/OR/NOT, IS [NOT] NULL, and the functions
      abs/length/lower/upper/coalesce/ifnull.  The affinity no-coercion
      gate is enforced per operator (a comparison or arithmetic that
      would coerce types falls the whole query back).  32 P1/P2 queries
      match the VDBE incl. arithmetic/NULL/div-by-zero edge cases;
      clean under ASan+UBSan.  REMAINING for V1: the columnar vector
      representation -- chunks are currently row-major cells (each cell
      self-describes its type, so NULLs are represented but not yet as
      a separate validity bitmask, and there is no dictionary encoding).
      That is a representation/perf change with no observable-result
      effect, deferred to a focused follow-up before V2's parallelism
      makes the per-vector layout matter.
  V2  Morsel parallelism on the executor: P1/P2 scans run on N worker
      procs with an atomic morsel cursor; I/O overlap via xtc_aio.
      Gate: results unchanged, throughput scales with loops.
      DONE (vx_run_parallel in vexec.c / test_vexec_par.c).  The
      table's rowid space is sliced into morsels handed out by an
      atomic cursor; one worker per executor loop (xtc_exec_spawn_on)
      opens its OWN connection, prepares a range-scoped source
      (... WHERE _rowid_ >= ?1 AND _rowid_ < ?2) that reuses the SAME
      compiled plan (the proj/filter expression trees are immutable and
      shared read-only), evaluates it over its morsels, and appends
      surviving rows to its own buffer; a final combine concatenates
      them (multiset).  The expression evaluator is already stateless,
      so it is parallel-safe unchanged.  Gate met: 6 queries match the
      VDBE running on 4 loops; clean under ASan+UBSan on the parallel
      path (the race surface is just the atomic morsel cursor and the
      immutable shared plan -- each worker owns its connection, row
      scratch, and result buffer).  NOTE: V2 uses a blocking step per
      worker (one worker per loop, so the loop is never shared); the
      xtc_aio fiber-overlap of page I/O within a worker is a
      refinement deferred with the columnar model.
  V3  Hash aggregation + GROUP BY (P3), per-worker partial aggregates +
      Combine.
  V4  Vectorized sort + LIMIT (P4); ORDER BY honored positionally.
  V5  Hash join (P5), build/probe as pipeline boundary, parallel build.
  V6  Widen recognizer coverage (subqueries, set ops); measure vs VDBE
      on an analytic workload (the scan/agg/join queries where
      vectorization + parallelism should show multiples).

## Risks and honest unknowns

  - The recognizer is the crux: SQLite's planner emits many opcode
    shapes and they shift across versions.  Matching must be
    conservative -- a mis-recognized plan that produces wrong results
    is far worse than a fallback.  Every recognizer rule ships with
    differential tests, and an unmatched opcode is always a fallback.
  - Vectorized writes and the full TEXT/collation/affinity semantics
    of SQLite are subtle; V0-V6 are READ pipelines, writes stay on the
    VDBE until the read engine is proven.
  - This is a large engine (DuckDB's executor is tens of thousands of
    lines).  The plan is incremental and always-correct-by-fallback,
    so partial delivery is useful and safe at every milestone.
