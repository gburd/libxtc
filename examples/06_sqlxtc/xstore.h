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

/* Attach a write-ahead log to the engine (process-global, like the
 * shared B-tree and commit clock).  When set, each commit logs its
 * versions durably BEFORE applying them to the B-tree, so a crash
 * after commit can redo it and a crash before leaves no trace.  NULL
 * (the default) means no durability. */
void xstore_set_wal(struct wal *w);

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

/* Advance to the next visible row.  Returns 1 with *rowid set and
 * *rec / *reclen pointing at the row's payload record (valid until the
 * next xstore_scan_next or close), 0 at end of scan, or <0 on error.
 * The table's resolved id is 0 (not found) -> open returns NULL. */
int xstore_scan_next(xstore_scan_t *s, int64_t *rowid,
                     const uint8_t **rec, int *reclen);

void xstore_scan_close(xstore_scan_t *s);

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

/* Autocommit-insert one already-encoded payload record at `rowid` (a new
 * committed version).  `rec` is the xstore payload record (the same
 * format xstore_rec_col decodes).  Returns 0 on success or <0 on error. */
int xstore_put_rec(bt_t *bt, uint32_t tableid, int64_t rowid,
                   const uint8_t *rec, int reclen);

/* Autocommit-delete `rowid` (write a tombstone version).  Returns 0 on
 * success or <0 on error.  Deleting an absent rowid still writes a
 * tombstone (idempotent), matching the vtab DELETE path. */
int xstore_delete_rec(bt_t *bt, uint32_t tableid, int64_t rowid);

#endif /* SQLXTC_XSTORE_H */
