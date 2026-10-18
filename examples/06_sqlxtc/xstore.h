/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/xstore.h
 *	SQLite virtual-table module backed by the libxtc-native B-tree
 *	storage engine.  See xstore.c.
 */

#ifndef SQLXTC_XSTORE_H
#define SQLXTC_XSTORE_H

#include <stdint.h>

struct xsql;
typedef struct bt bt_t;
typedef struct bm bm_t;
struct wal;

/* Register the "xstore" virtual-table module on `db`, backed by the
 * shared engine B-tree `bt`.  Thereafter:
 *	CREATE VIRTUAL TABLE t USING xstore;
 * creates a t(k INTEGER PRIMARY KEY, v) table whose rows live in `bt`
 * (the cooling buffer pool, larger-than-RAM capable) rather than in
 * SQLite's built-in B-tree. */
int xstore_register(struct xsql *db, bt_t *bt);
/* Native registration: the only connection model.  Wires the ctx +
 * g_dbmap mapping over the shared engine B-tree `bt` (no vtab module /
 * SQL functions / hooks).  Runs in native_mode from the start.
 * (The vtab-based xstore_register is compiled only under SQLXTC_VTAB,
 * kept for reference; the shipping engine never uses it.) */
int xstore_register_native(struct xsql *db, bt_t *bt);
/* Drop a connection's xstore association on close (releases its g_dbmap
 * slot so the fixed-size map does not overflow under many connections).
 * Safe for an unregistered handle. */
void xstore_unregister(struct xsql *db);
/* Like xstore_unregister, but also frees the connection ctx (the native
 * connection has no module ctx_free hook to run it on close). */
void xstore_unregister_native(struct xsql *db);

/* Attach a write-ahead log to the engine (process-global, like the
 * shared B-tree and commit clock).  When set, each commit logs its
 * versions durably BEFORE applying them to the B-tree, so a crash
 * after commit can redo it and a crash before leaves no trace.  NULL
 * (the default) means no durability. */
void xstore_set_wal(struct wal *w);
/* 1 once the attached log has fail-stopped after a write/fsync error:
 * every later write fails until the store is reopened. */
int  xstore_wal_failed(void);
/* Hold back every commit (and STEAL spill) and wait for in-flight ones
 * to finish, so the tree and log are stable for a checkpoint; readers
 * keep running.  Pair with xstore_quiesce_end.  Call from a proc or off
 * a loop, holding no engine lock. */
void xstore_quiesce_begin(void);
void xstore_quiesce_end(void);

/* Recover the B-tree from the log at `wal_path`: replay every committed
 * transaction's versions (idempotent -- version keys are immutable),
 * and advance the commit clock past the highest recovered timestamp.
 * Call once at open, before serving, after bt_open and before
 * xstore_set_wal's writer is needed.  Returns XTC_OK. */
int xstore_recover(bt_t *bt, const char *wal_path);

/*
 * In-place crash recovery (ARIES physiological redo).  Like
 * xstore_recover, but trusts a possibly-torn base IN PLACE (bm opened
 * reopen=1, not truncated) and applies the XL_PAGE full-page
 * after-images logged on the structure-modification path, page-LSN
 * gated, to repair a torn split/merge rather than discarding the base.
 * Ordinary row writes still redo logically (idempotent).  `out_pages`
 * (optional) receives the count of page images applied.  See xstore.c
 * for the precise scope (mechanism + test; not the live default).
 */
int xstore_recover_inplace(bt_t *bt, bm_t *bm, const char *wal_path,
    uint64_t *out_pages);

/* Enable (or disable) physiological SMO logging on the B-tree: each
 * split/merge logs its modified pages as XL_PAGE after-images inside a
 * nested-top-action bracket (a dummy CLR).  Enable before driving
 * writes whose torn structure should be repairable in place by
 * xstore_recover_inplace. */
void xstore_register_smo(int enable);

/* In-WAL checkpoint: compact the log to a CHECKPOINT record plus a dump
 * of the live tree, bounding it.  Call only when commits are quiesced. */
int xstore_checkpoint_wal(bt_t *bt, struct wal *w, const char *wal_path);

/*
 * Fuzzy (ARIES-style) checkpoint -- O(dirty), not O(live-data).  Instead
 * of dumping the whole live tree (xstore_checkpoint_wal), this flushes
 * the dirty page set to the base (making it durable) and truncates the
 * log below the redo horizon.  The recLSN horizon (bm_min_rec_lsn) is
 * the oldest change not yet on the base -- the floor a pure no-flush
 * fuzzy checkpoint would use; because this checkpoint flushes the whole
 * dirty set, the safe horizon advances to the post-flush durable LSN
 * (at or above bm_min_rec_lsn).  It marks the base clean/trusted, writes
 * a CHECKPOINT record carrying that horizon as its start-LSN, and
 * retains only records at or after it.  Recovery of such a checkpoint
 * trusts the base in place (xstore_recover_inplace) and replays only
 * the retained tail, so restart cost is O(dirty), not O(database).
 *
 * Requires bm to be the buffer manager bt runs on, opened with
 * lsn_off >= 0, and physiological logging installed (xstore_register_smo
 * / non-split-leaf images) so the trusted base is torn-safe.  Call only
 * when commits are quiesced.  `out_horizon` (optional) receives the
 * captured horizon.  Returns XTC_OK or an error (log unchanged on error).
 */
int xstore_fuzzy_checkpoint(bt_t *bt, bm_t *bm, struct wal *w,
    const char *wal_path, uint64_t *out_horizon);

/* The MVCC commit clock: read it to persist at a clean shutdown,
 * restore it when trusting the base on a clean restart. */
uint64_t xstore_clock(void);
void xstore_set_clock(uint64_t v);
/* HLC merge-on-observe: fold a peer's commit timestamp into the local
 * clock so the next minted stamp is causally after it (for the future
 * distributed engine; an alias over xstore_set_clock). */
void xstore_clock_observe(uint64_t peer_ts);

/* Number of CLRs written during recovery undo passes (test/metric). */
uint64_t xstore_undo_clrs(void);

/* ---- storage-native scan (for a from-scratch executor) ----------- *
 *
 * A read-only MVCC snapshot scan over one table's rows, driven WITHOUT
 * a SQLite connection or the VDBE: the vectorized executor uses this to
 * read rows directly from the B-tree, applying the same snapshot
 * visibility (newest non-tombstone version per rowid with commit_ts <=
 * snap) the vtab cursor applies.  The row payload is returned as the
 * engine's record bytes (see xstore's record codec); column 0 is the
 * INTEGER PRIMARY KEY (the rowid), columns 1.. are the payload.
 *
 * The rowid range [lo, hi] (inclusive) bounds the scan -- pass
 * has_lo / has_hi = 0 for an unbounded end -- so a morsel-parallel
 * executor can hand each worker a disjoint slice and they scan
 * independently (each opens its own xstore_scan).  snap = 0 selects the
 * latest committed snapshot (xstore_clock()). */
typedef struct xstore_scan xstore_scan_t;

/* The B-tree a connection's xstore tables are backed by (from
 * xstore_register), so an executor with only the connection handle can
 * open a storage-native scan over it.  NULL if db was never registered. */
bt_t *xstore_bt_of(struct xsql *db);

xstore_scan_t *xstore_scan_open(bt_t *bt, const char *table,
                                uint64_t snap,
                                int64_t lo, int has_lo,
                                int64_t hi, int has_hi);
/* Like xstore_scan_open, but honours the connection's uncommitted
 * writes (read-your-writes): inside a transaction the scan overlays the
 * write buffer (UPDATE replaces, DELETE hides, INSERT appended).  When
 * the connection is not in a txn (or the buffer is empty) it is exactly
 * xstore_scan_open over the connection's bt. */
xstore_scan_t *xstore_scan_open_txn(struct xsql *db, const char *table,
                                    uint64_t snap,
                                    int64_t lo, int has_lo,
                                    int64_t hi, int has_hi);

/* Advance to the next visible row.  Returns 1 with *rowid set and
 * *rec / *reclen pointing at the row's payload record (valid until the
 * next xstore_scan_next or close), 0 at end of scan, or <0 on error.
 * The table's resolved id is 0 (not found) -> open returns NULL. */
int xstore_scan_next(xstore_scan_t *s, int64_t *rowid,
                     const uint8_t **rec, int *reclen);

void xstore_scan_close(xstore_scan_t *s);

/* Base-table version keys examined by all storage scans so far (metric:
 * tests assert a point statement visits O(1) rows, not O(table)). */
uint64_t xstore_rows_visited(void);

/* Storage value classes returned by xstore_rec_col. */
enum {
	XSTORE_C_NULL = 0, XSTORE_C_INT, XSTORE_C_REAL, XSTORE_C_TEXT, XSTORE_C_BLOB
};

/* Decode payload column `idx` (0-based over the non-key columns) of a
 * scan record into a typed value.  Returns the XSTORE_C_* class; for
 * INT/REAL the value is in iout / rout, for TEXT/BLOB pout / nout point
 * into rec (valid for rec's lifetime).  Pass NULL for unwanted outs. */
int xstore_rec_col(const uint8_t *rec, int reclen, int idx,
                   int64_t *iout, double *rout,
                   const uint8_t **pout, int *nout);

/*
 * Native autocommit write path -- the storage side of bypassing the
 * VDBE for a recognized INSERT.  These run a write WITHOUT a SQLite
 * connection / VDBE / vtab, reusing the engine's own commit machinery
 * (one hlc_tick commit timestamp per row, WAL-durable before apply,
 * the same versioned record the vtab xUpdate path writes), so a native
 * insert and a VDBE insert are byte-identical on disk.
 */

/* Resolve a table name to its catalog id.  Returns 1 with *tableid set,
 * or 0 if the table is unknown (read-only catalog lookup). */
int xstore_table_id(bt_t *bt, const char *name, uint32_t *tableid);

/* The smallest rowid strictly greater than every existing rowid in the
 * table (max existing + 1, or 1 for an empty table) -- the value SQLite
 * would assign to a NULL INTEGER PRIMARY KEY.  Snapshot value; callers
 * needing concurrency-safe allocation must serialize. */
int64_t xstore_max_rowid(bt_t *bt, uint32_t tableid);
/* Approximately equi-depth rowid cut points of `tableid` inside the
 * rowid range [lo, hi), from the B-tree's upper separators
 * (bt_sample_keys, O(fanout) page reads -- never a scan): up to n
 * strictly increasing rowids in (lo, hi) written to out[], so that the
 * rowid ranges [lo, out[0]), [out[0], out[1]), ... hold about the same
 * number of row versions whatever the rowid distribution.  Returns the
 * count (0 when the tree is too small to have separators in range:
 * callers fall back to a uniform split). */
int xstore_rowid_split_points(bt_t *bt, uint32_t tableid, int64_t lo,
                              int64_t hi, int n, int64_t *out);
/* Largest rowid in `tableid` including this connection's uncommitted
 * buffered writes -- the auto-rowid base for an in-txn INSERT (a
 * db-handle variant of xstore_max_rowid that also scans the wbuf). */
int64_t xstore_max_rowid_txn(struct xsql *db, uint32_t tableid);
/* Does `rowid` exist as a live row of `tableid`, including this
 * connection's uncommitted buffered writes (a buffered tombstone hides
 * a committed row, a buffered upsert reveals one)?  Lets the native
 * INSERT enforce PK uniqueness inside a transaction.  Returns 1 / 0. */
int xstore_row_exists_txn(struct xsql *db, uint32_t tableid, int64_t rowid);
/* INSERT one new row (the key must not already exist).  Autocommit
 * checks absence atomically with the write, waiting out an in-flight
 * writer of the key; in a transaction the commit's write-write check
 * refuses a row committed since the snapshot.  Returns 0, 1 on a unique
 * violation, -1 on any other error. */
int xstore_insert_txn(struct xsql *db, uint32_t tableid, int64_t rowid,
                      const uint8_t *rec, int reclen);
/* Reserve an auto-assigned rowid for `tableid`: greater than `floor`
 * (the visible max, including this txn's buffered rows) and than any
 * rowid already handed out in this process, so concurrent INSERTs that
 * omit the key never pick the same one. */
int64_t xstore_next_rowid(struct xsql *db, uint32_t tableid, int64_t floor);
/* Native table schema (replaces PRAGMA table_info / sqlite_master).  One
 * column descriptor per declared column, in declared order; column 0 is
 * the INTEGER PRIMARY KEY (the rowid).  `affinity` is the SQLite type
 * affinity letter derived from the declared type ('i' INTEGER, 'r'
 * REAL/numeric, 't' TEXT, 'b' BLOB/none). */
typedef struct xstore_col {
	char name[64];
	char decltype[32];
	char affinity;
	int  is_pk;
	int  hidden;   /* synthetic rowid column for an implicit-rowid table;
	                * stored as storage-column 0 but NOT a user column
	                * (excluded from SELECT * and positional INSERT). */
} xstore_col_t;

/* Fill `cols` (capacity `cap`) with the schema of `name` from the
 * persisted catalog.  Returns the column count (>=1) on success, 0 if
 * the table is unknown or has no recorded schema (caller falls back to
 * sqlite_master), or <0 on error / overflow. */
int xstore_table_schema(bt_t *bt, const char *name,
                        xstore_col_t *cols, int cap);

/* Autocommit-insert one already-encoded payload record at `rowid` (a new
 * committed version).  `rec` is the xstore payload record (the same
 * format xstore_rec_col decodes).  Returns 0 on success or <0 on error. */
int xstore_put_rec(bt_t *bt, uint32_t tableid, int64_t rowid,
                   const uint8_t *rec, int reclen);

/* Autocommit-delete `rowid` (write a tombstone version).  Returns 0 on
 * success or <0 on error.  Deleting an absent rowid still writes a
 * tombstone (idempotent), matching the vtab DELETE path. */
int xstore_delete_rec(bt_t *bt, uint32_t tableid, int64_t rowid);

/* Transaction-aware native write: buffers into the connection's write
 * set when a BEGIN..COMMIT is open (committing atomically at one
 * timestamp with any other writes in the transaction, VDBE or native),
 * or autocommits immediately in autocommit mode.  `deleted` writes a
 * tombstone (rec/reclen ignored).  Returns 0 on success, <0 on error. */
int xstore_write_txn(struct xsql *db, uint32_t tableid, int64_t rowid,
                     const uint8_t *rec, int reclen, int deleted);

/* True if the connection has an open (non-autocommit) transaction. */
int xstore_in_txn(struct xsql *db);

/*
 * 1 if the current transaction holds any buffered (uncommitted) write
 * for `tableid`, else 0 (also 0 outside a transaction).  A native read
 * path may run in-transaction only when this is 0: then a committed
 * scan sees exactly what a vtab cursor would.
 */
int xstore_txn_table_dirty(struct xsql *db, uint32_t tableid);
/* Native transaction control: flush (commit) or discard (rollback) the
 * connection's buffered write set at one timestamp.  The live path runs
 * these just before the SQLite COMMIT/ROLLBACK statement so native
 * (VDBE-free) buffered writes are durable / discarded atomically.
 * No-ops when not in a transaction.  Return 0 / <0. */
int xstore_commit(struct xsql *db);
int xstore_rollback(struct xsql *db);

/* Native-driver transaction ownership (subsystems A+B).  A native
 * sx_stmt driver calls xstore_native_mode(db, 1) to take autocommit
 * ownership from SQLite, then drives BEGIN via xstore_native_begin and
 * COMMIT/ROLLBACK via xstore_commit/xstore_rollback.  Savepoints (0-
 * based level) go through xstore_savepoint/_release/_rollback_to.  When
 * native_mode is off the engine behaves exactly as the SQLite-driven
 * path.  Return 0 / -1 (not xstore-backed). */
int xstore_native_mode(struct xsql *db, int on);
/* Set the connection isolation level from a name (the native C-API
 * equivalent of the xstore_isolation() SQL function). */
int xstore_set_isolation(struct xsql *db, const char *level);
/* Level for the current transaction only (SET TRANSACTION ISOLATION
 * LEVEL); 0 ok, -1 bad level, -2 not in a txn, -3 txn already queried. */
int xstore_set_txn_isolation(struct xsql *db, const char *level);
/* Mark the start of a DML statement (write-write conflict base). */
void xstore_stmt_begin(struct xsql *db);
void xstore_stmt_end(struct xsql *db);
/* 1 if the current statement's last write was refused as a conflict. */
int  xstore_last_conflict(struct xsql *db);
/* Native C-API equivalents of the MVCC SQL functions. */
int64_t xstore_clock_now(void);              /* xstore_now() */
int xstore_as_of(struct xsql *db, int64_t ts);   /* xstore_as_of(ts) */
int xstore_gc_run(struct xsql *db);          /* xstore_gc() -> reclaimed */
int xstore_autovacuum_set(struct xsql *db, int on); /* xstore_autovacuum(on) */
int64_t xstore_prune_count(void);            /* xstore_prune_count() */
int xstore_native_begin(struct xsql *db);
int xstore_savepoint(struct xsql *db, int level);
int xstore_release(struct xsql *db, int level);
int xstore_rollback_to(struct xsql *db, int level);

/* Native DDL over the xstore catalog (subsystem D).  create_table
 * allocates a tableid + persists the column schema (coldefs = the
 * comma-joined column list, first column the INTEGER PRIMARY KEY), the
 * same catalog row the vtab xConnect writes -- vexec / the native write
 * path then operate on the table with no SQLite schema.  Returns the
 * tableid (>0) or 0.  drop_table tombstones the catalog row (lookup
 * skips it).  Returns 0 / -1.  These bypass SQLite's schema -- for the
 * native driver, where there is no VDBE to keep in sync. */
uint32_t xstore_create_table(struct xsql *db, const char *name, const char *coldefs);
int xstore_drop_table(struct xsql *db, const char *name);
/* ALTER TABLE oldname RENAME TO newname (re-keys the catalog entry). */
int xstore_rename_table(struct xsql *db, const char *oldname, const char *newname);
/* Views (CREATE VIEW / DROP VIEW): the executor expands a FROM
 * reference to a view as a derived table (its SELECT text). */
int xstore_create_view(struct xsql *db, const char *name, const char *select_sql);
int xstore_view_sql(bt_t *bt, const char *name, char *out, int cap);
int xstore_drop_view(struct xsql *db, const char *name);

/* ---- secondary indexes, UNIQUE, composite PRIMARY KEY ------------ *
 *
 * An index maps a tuple of column values (1..16 columns, each ASC or
 * DESC) to the rowids carrying it, in an order-preserving key encoding,
 * so an equality prefix plus an optional range on the next column is a
 * contiguous range and walks in index order.  Entries live in the SAME
 * B-tree as base rows under a per-index namespace; they are DERIVED
 * data (rebuilt from the base rows at open).  The index catalog IS
 * persisted (WAL-durable version rows), so the set of indexes survives
 * a crash.  A UNIQUE index is enforced (statement time + re-validated at
 * commit under the commit lock); a PRIMARY KEY that is not a rowid alias
 * and every UNIQUE constraint are such indexes (kind 'p' / 'u', named
 * sqlite_autoindex_<t>_<n>, as SQLite does).  See xstore.c.
 */

/* Create index `name` on `table`(`cols[0..ncol)`), DESC per bit of
 * descmask, unique 0/1, kind 'i' (CREATE INDEX) / 'p' (PRIMARY KEY) /
 * 'u' (UNIQUE constraint).  Persists the catalog row and builds entries
 * from the existing rows.  Returns 1 on success (also when an index of
 * that name exists), 0 on failure (unknown table / column), or -1 when
 * a UNIQUE index cannot be built because existing rows duplicate a key
 * (nothing is created). */
int xstore_create_index_ex(struct xsql *db, const char *name, const char *table,
                           const char *const *cols, int ncol, uint32_t descmask,
                           int unique, int kind);
/* Single-column ascending CREATE INDEX (compat wrapper).  1 / 0. */
int xstore_create_index(struct xsql *db, const char *name, const char *table,
                        const char *col, int unique);
/* Drop CREATE INDEX `name`.  1 on success, 0 if unknown, -1 if it is a
 * constraint index (PRIMARY KEY / UNIQUE: not droppable, as SQLite). */
int xstore_drop_index(struct xsql *db, const char *name);

/* The indexes of `table` (planner support): up to cap entries. */
typedef struct xstore_index_info {
	uint32_t idx_id;
	char     name[64];
	int      ncol;
	char     col[16][64];
	uint32_t descmask;
	int      unique;
	char     kind;
} xstore_index_info_t;
int xstore_index_list(bt_t *bt, const char *table, xstore_index_info_t *out,
                      int cap);

/* Is there an ascending index whose leading column is `table`.`col`?
 * Fills *idx_id and returns 1; else 0. */
int xstore_index_lookup(bt_t *bt, const char *table, const char *col,
                        uint32_t *idx_id);

/* Candidate rowids whose index entry's leading column equals the typed
 * value (XSTORE_C_*), into out (cap).  Returns the count, or -1 on
 * overflow (caller scans).  Candidates may be stale: the caller
 * re-fetches each through MVCC and re-applies the WHERE. */
int xstore_index_seek_eq(bt_t *bt, uint32_t idx_id, int vclass,
                         int64_t ival, double rval,
                         const uint8_t *bytes, int nbytes,
                         int64_t *out, int cap);

/* A typed value for index bounds (XSTORE_C_*; p/n for TEXT/BLOB). */
typedef struct xstore_val {
	int            cls;
	int64_t        i;
	double         r;
	const uint8_t *p;
	int            n;
} xstore_val_t;

/* Verified index range scan on the connection's snapshot: rows whose
 * first neq index columns equal eq[], and (optionally) whose next
 * column lies in [lo, hi] (inclusive per lo_incl / hi_incl; NULL bound =
 * open; NULLs never match a range), in index order (reverse = 1:
 * descending).  Each row returned is the version visible to the
 * connection (its own buffered writes included) and carries the key, so
 * the caller only needs to re-apply residual predicates.  NULL if the
 * index or bounds are unusable (caller scans). */
typedef struct xstore_iscan xstore_iscan_t;
xstore_iscan_t *xstore_iscan_open(struct xsql *db, const char *table,
                                  uint32_t idx_id, const xstore_val_t *eq,
                                  int neq, const xstore_val_t *lo, int lo_incl,
                                  const xstore_val_t *hi, int hi_incl,
                                  int reverse);
/* 1 with the row (rec valid until the next call), 0 at the end, <0 error. */
int  xstore_iscan_next(xstore_iscan_t *s, int64_t *rowid,
                       const uint8_t **rec, int *reclen);
void xstore_iscan_close(xstore_iscan_t *s);

/* The connection's last UNIQUE-index violation message ("UNIQUE
 * constraint failed: t.a, t.b"), "" if none (a rowid-key conflict). */
const char *xstore_uniq_err(struct xsql *db);

/* Statement atomicity in a transaction: mark the write buffer before a
 * statement; undo back to the mark if the statement fails (the
 * transaction stays open, as in SQLite).  mark is -1 outside a txn. */
int  xstore_stmt_mark(struct xsql *db);
void xstore_stmt_undo(struct xsql *db, int mark);

/* Count of index seeks served (metric / test evidence that the planner
 * took the index rather than a full table scan). */
uint64_t xstore_index_seek_count(void);

/* Rebuild every catalog-recorded index for `bt` from its base rows.
 * Called at storage open (after the base is recovered / trusted) so
 * derived index entries are always consistent with durable base data,
 * regardless of whether index pages were flushed before a crash.
 * Returns 0. */
int xstore_index_rebuild_all(bt_t *bt);

#endif /* SQLXTC_XSTORE_H */
