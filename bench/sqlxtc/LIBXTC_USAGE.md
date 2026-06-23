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

## What still uses raw OS primitives -- candidates to shift to libxtc

These are the places sqlxtc still reaches past libxtc to the OS.  Each
is a candidate for the libxtc model, in rough priority order.

### 1. The catalog lock (xstore.c: ~52 raw `pthread_mutex` sites) -- HIGH

`g_cat_mu` (the table-id catalog cache + view registry + connection
map) is a raw `pthread_mutex`.  The code already documents the hazard
this creates: `xs_cat_find_or_create` must persist the catalog row
(`xs_put`, which PARKS on the WAL ack) OUTSIDE the lock, because
"parking while holding a pthread mutex would wedge the loop if another
fiber contended g_cat_mu."  That out-of-lock publish is exactly the
cross-connection CREATE race we had to chase down twice.

**Shift:** make `g_cat_mu` an `xtc_amutex`.  Then the catalog row can be
persisted INSIDE the critical section (the park releases the loop), the
reserve-then-publish window closes, and the visibility race disappears
by construction.  This is the single highest-value libxtc-ification.

### 2. The buffer-pool partition/frame locks (bufmgr.c: ~56 raw
       `pthread_mutex`, mixed with 15 `xtc_arwlock`) -- HIGH

The pool already uses `xtc_arwlock` for the frame/page latches, but the
hash-partition and free-list bookkeeping still use raw `pthread_mutex`.
Under a fiber that parks for a page-in while holding a partition mutex,
the loop stalls.

**Shift:** convert the remaining partition/free-list `pthread_mutex` to
`xtc_amutex` (or fold them into the existing `xtc_arwlock` discipline)
so no pool lock is ever held across an `xtc_aio` park.

### 3. The WAL append lock (wal.c: ~8 raw `pthread_mutex`) -- MEDIUM

The WAL is a gen_server, but a few internal counters/buffers are guarded
by raw `pthread_mutex`.  Since the append path parks on fdatasync,
these should be `xtc_amutex` for the same reason.

### 4. The native subquery / IN-list materialization (vexec.c) -- MEDIUM
       (perf, not correctness)

The benchmark's worst case is uncorrelated `IN (SELECT ...)`, which
re-runs and re-materializes the subquery on every outer execution.
This is not a libxtc primitive gap; it is a vexec caching gap.  But the
fix -- materialize once and share across executions of a prepared
statement -- pairs naturally with caching the morsel-parallel
`xtc_exec` plan, so the parallel scan does not re-`xtc_exec_init` per
call either.

### 5. ORDER BY ... LIMIT top-N (vexec.c) -- LOW (perf)

vexec sorts the full result then applies LIMIT.  A bounded top-N heap
(the VDBE's approach) would close the 0.6x gap.  No libxtc involvement;
listed for completeness.

### 6. The blocking-mutex shim for the vendored engine
       (mutex.c via xtc_amutex) -- RETIRES WITH sqlite3.c

Once sqlite3.c is removed, the SQLite mutex/mem/pcache/vfs shims and the
`xtc_blocking` offload they need go away entirely; the native engine
never makes a blocking syscall on the hot path.

## Summary

sqlxtc already runs its server, connections, buffer pool, B-tree
latching, WAL, recovery, MVCC, and parallel reads on libxtc's
async/proc/gen_server/aio/exec primitives -- it is a real exercise of
the runtime, not a toy.  The remaining raw-`pthread_mutex` sites in the
catalog, buffer pool, and WAL are the clear next shifts: moving them to
`xtc_amutex` lets every critical section safely span an I/O park, which
both removes a class of cross-fiber visibility races (the catalog) and
keeps the event loop from ever stalling under load.  The vexec
subquery/top-N items are pure performance follow-ups orthogonal to the
runtime.
