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

#endif /* SQLXTC_XSTORE_H */
