# How sqlxtc uses libxtc, and what to shift to libxtc-style next

sqlxtc is the flagship example that the whole libxtc runtime exists to
serve: a concurrent SQL engine built entirely on libxtc's Erlang/Tokio/
Seastar-shaped primitives.  This report maps which libxtc feature each
sqlxtc subsystem uses today, then lists the places that still use raw
OS primitives and would benefit from moving to the libxtc model.

## What sqlxtc uses today, by subsystem

### The server (main.c, conn.c, admin.c, metrics.c)

- **`xtc_app`** -- the supervised, multi-loop application.  The server
  is an `xtc_app_t` with one `xtc_loop_t` per CPU; loop 0 also owns the
  listener, the metrics proc, and the storage engine.
- **`xtc_svr`** (the gen_server behaviour) -- request/reply plumbing
  (`xtc_svr_call`, `xtc_svr_reply`, `xtc_svr_cast`, `xtc_svr_call_save`)
  used by the MVCC version-store server and the shard coordinator.
- **`xtc_proc_spawn` / `xtc_recv` / `xtc_send` / `xtc_self`** -- every
  client connection is a process; the listener spawns a connection proc
  per accept.  The Quack wire protocol is read/written inside the proc.
- **`xtc_res`** -- a resource budget (memory / connection caps) the
  server enforces per the `-n` / `--max-memory` flags.
- **`xtc_net` / `xtc_tcp`** -- the non-blocking listener + per-conn
  socket (`xtc_net_setnonblock`, `xtc_tcp_opts_t`).
- **`xtc_stats` / `xtc_hist` / `xtc_counter` / `xtc_gauge`** -- query
  latency histograms (`xtc_stat_query_latency`), totals/errors,
  connection gauges, resident-memory gauge, surfaced over the admin
  protocol and `xtc_inspect_procs`.
- **`xtc_log` / `xtc_log_drain`** -- structured server logging.

### The buffer pool (bufmgr.c) -- the LeanStore-style cooling pool

- **`xtc_aio`** (`xtc_aio_pread` / `xtc_aio_pwrite` / `xtc_aio_fdatasync`)
  -- ALL page I/O is async/offloaded; a page fault parks the fiber, not
  the loop.  This is what makes the pool larger-than-RAM without
  blocking the event loop.
- **`xtc_arwlock`** -- the async reader/writer lock on the page frame /
  hash partitions: readers run concurrently with the cooling writer;
  acquisition parks rather than spins.
- **`xtc_dio_sched`** -- the direct-I/O scheduler
  (`xtc_dio_sched_report_multi`) batches and orders page writebacks for
  the O_DIRECT page store.
- **`xtc_proc_spawn` / `xtc_proc_sleep`** -- the background cooling /
  writeback proc runs as its own process on a timer.

### The B-link tree (btree.c, btnode.c)

- **`xtc_arwlock`** -- latch crabbing for parallel writers: a descent
  hand-over-hands rwlatches down the tree; the B-link right-link lets a
  concurrent split be tolerated without restart.  This is the
  concurrent-writer story and the reason there is no global tree lock.

### The WAL + ARIES recovery (wal.c, xlog.c)

- **`xtc_svr`** -- the WAL is a gen_server; a writer `xtc_svr_call`s it
  to append a record and parks on the ack (group commit batches the
  fdatasync).
- **`xtc_aio_pwrite` / `xtc_aio_fdatasync`** -- durable log writes, async.
- **`xtc_proc_spawn` / `xtc_recv_correlate`** -- the flush proc; callers
  correlate their ack by request id.

### The vectorized executor (vexec.c)

- **`xtc_exec`** -- morsel-parallel scan/aggregation: `xtc_exec_init(N)`
  fans a scan across N loops with `xtc_exec_spawn_on`, then
  `xtc_exec_run` blocks until every worker is DONE.  Each worker owns a
  rowid range and aggregates locally; results merge at the end.  This is
  the multi-core read path.

### The connection handle, mutexes, page-cache shim
  (engine.c, mutex.c, pcache.c, vfs.c)

- **`xtc_amutex`** -- the async mutex behind the (now legacy) SQLite
  mutex methods, so even the vendored engine's locks park instead of
  blocking the loop.
- **`xtc_slab`** -- the page-cache (pcache) slab allocator: fixed-size
  page frames recycled without malloc churn.
- **`xtc_vfs`** -- the SQLite VFS shim routes the vendored pager's I/O
  through the xtc async I/O layer (legacy path; goes away with sqlite3.c).
- **`xtc_blocking`** -- the offload pool for the rare genuinely-blocking
  syscall the VFS must make.

### MVCC + shards (mvcc.c, test_shard.c)

- **`xtc_svr`** -- the version-store and shard coordinator are
  gen_servers; HLC timestamps and snapshot registration go through
  `xtc_svr_call`.

## What still uses raw OS primitives -- and what was done

The items below were the shift candidates; the status of each follows.
The guiding rule that emerged: convert a lock to `xtc_amutex` only when
it is (or could be) held ACROSS an I/O park on a loop.  A lock that is
always released before I/O, or that only ever runs off a loop, is
faster and just as correct as a raw `pthread_mutex`, so converting it is
a regression for no benefit.

### 1. The catalog lock (xstore.c) -- DONE

`g_cat_mu` (the table-id catalog cache + view registry), the SSI
registry lock, and the active-snapshot lock are now `xtc_amutex`
(`xtc_amutex_static` slots).  `xs_cat_find_or_create` persists the
catalog row (`xs_put`, which parks on the WAL ack) INSIDE the lock now
that the park releases the loop, so the durable row exists before any
other connection can take the lock and look the table up -- the
cross-connection CREATE-then-SELECT race is closed by construction.
This was the single highest-value libxtc-ification.

### 2. The buffer-pool partition/frame locks (bufmgr.c) -- AUDITED, KEPT RAW

The pool already uses `xtc_arwlock` for the frame/page CONTENT latch --
the one latch held across a page-in.  The hash-partition, free-list,
page-id, prefetch-ring, and double-write locks are raw `pthread_mutex`,
and the code is audited (and now commented) to release every one of
them BEFORE any `xtc_aio` I/O: a miss loads into a free frame and only
then re-takes the stripe to publish; the double-write lock only hands
out a ring slot.  Since none of these ever spans a park, converting
them to a parking mutex only adds ownership-tracking overhead to the
hottest path in the engine.  Measured on the resident fix/unfix loop:
~18 ns/op with `pthread_mutex` vs ~22 ns with `xtc_amutex`, a 24%
regression.  Kept raw, by design.

### 3. The WAL append lock (wal.c) -- AUDITED, KEPT RAW

The WAL's on-loop path is the group-commit writer: committers park on
the writer's mailbox (`xtc_recv`) and the writer does its pwrite +
fdatasync through `xtc_aio` with no mutex around the I/O -- already
fully libxtc-native.  The `sync_mu`-guarded `wal_commit_sync` that holds
the lock across a synchronous `fdatasync` is the OFF-LOOP fallback only:
`xs_wal_emit` dispatches to the group-commit `wal_commit` whenever it
runs on a loop (`xtc_self()` is a real pid), and to `wal_commit_sync`
only off a loop (recovery before the loop is up, and the off-loop
tests).  An `xtc_amutex` there would fall back to a condvar off-loop
anyway, so there is nothing to gain.  Kept raw.

### 4. IN-list / IN (SELECT) membership (vexec.c) -- DONE

The membership test was a linear scan over the list per row.  When
every element is a constant literal (always true for an uncorrelated
IN (SELECT) whose values are materialized as literals), the values are
now sorted once at compile time and eval does a binary search:
O(rows * log list) instead of O(rows * list).  Measured 1.40x on a
~2000-element list over 20k rows; the win grows with list size.  (The
benchmark's earlier 0.07x was a harness that re-prepared per call; the
server caches the prepared statement, so the materialization is
amortized.)

### 5. ORDER BY ... LIMIT top-N (vexec.c) -- DONE

vexec materialized the full result then sorted then sliced LIMIT.  It
now keeps a bounded top-N buffer of K = OFFSET + LIMIT rows during the
scan, with a lazily-recomputed worst slot, then sorts only those K.
Measured 2.8x-3.8x on 50k-row ORDER BY ... LIMIT queries; this turns
the old 0.60x regression vs the VDBE into a clear win.

### 6. The SQLite mutex/mem/pcache/vfs shims -- RETIRED

sqlite3.c and the four extension-point shims (and the `xtc_blocking`
offload they needed) are removed.  The native engine makes no blocking
syscall on the hot path: page I/O is `xtc_aio`, WAL commit is the
group-commit writer, and the catalog/SSI/snapshot locks are
`xtc_amutex`.

## Summary

sqlxtc runs its server, connections, buffer pool, B-tree latching, WAL,
recovery, MVCC, and parallel reads on libxtc's async / proc /
gen_server / aio / exec primitives -- a real exercise of the runtime.
The catalog locks moved to `xtc_amutex` (closing a real race); the
buffer-pool and WAL locks were audited and intentionally kept raw
because they never span a park on a loop, and a parking mutex there
measurably regresses the hot path for no benefit.  The vexec top-N and
IN-membership wins are pure performance, orthogonal to the runtime.
