/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/xstore.c
 *	A SQLite virtual-table module that runs SQL on the libxtc-native
 *	storage engine (btree.c over the cooling buffer pool, bufmgr.c)
 *	with multi-version concurrency control (MVCC) snapshot reads.
 *
 *	SQLite tokenizes/parses/plans/runs the VDBE; table I/O is
 *	redirected here, into our on-disk, larger-than-RAM-capable
 *	B-tree.  Reimplementing the amalgamation's internal btree.h is
 *	impractical; the virtual-table seam is the supported mechanism.
 *
 *	MVCC model (the PostgreSQL heap lineage; see
 *	docs/M_SQLXTC_MVCC_SQL.md).  Each row VERSION is stored under the
 *	B-tree key (rowid, commit_ts), with versions of one rowid
 *	clustered and ordered newest-first (the timestamp half is stored
 *	bit-inverted so a forward scan visits the highest commit_ts
 *	first).  The version's commit_ts is its xmin (the transaction
 *	that created it); a delete writes a tombstone version.  A read at
 *	snapshot S returns, per rowid, the newest version with
 *	commit_ts <= S that is not a tombstone -- i.e. PostgreSQL's
 *	HeapTupleSatisfiesMVCC, with newer versions standing in for xmax.
 *	Readers never block writers and writers never block readers.
 *
 *	A single global logical clock supplies commit timestamps and
 *	snapshots for this one shared B-tree (the per-shard hybrid
 *	logical clock in mvcc.c is the sharded variant).  Two SQL
 *	functions expose it: xstore_now() returns the current clock, and
 *	xstore_as_of(ts) pins this connection's reads at snapshot ts (0 =
 *	latest) -- a time-travel / AS OF read that makes snapshot
 *	visibility observable from SQL.
 *
 *	STATUS: writes commit per-statement (autocommit) at a fresh
 *	timestamp.  Transaction-level snapshot isolation across multiple
 *	statements (write buffering + a single commit timestamp via
 *	xBegin/xCommit, merging mvcc.c's 2PC coordinator) and Cahill SSI
 *	serializability are the next steps in docs/M_SQLXTC_MVCC_SQL.md.
 */

#include "xstore.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sqlite3.h"
#include "btree.h"
#include "xtc.h"

/* Global logical commit clock for the shared engine B-tree. */
static _Atomic uint64_t g_xclock = 1;

#define XS_F_DELETED 0x01u     /* value[0] flag: this version is a tombstone */
#define XS_VKLEN     16        /* (rowid:8) + (inverted commit_ts:8) */
#define XS_VMAX      4096       /* max row payload bytes (one page-ish) */

/* Per-connection state, shared by the module and the SQL functions:
 * the engine storage and this connection's read snapshot. */
typedef struct xstore_ctx {
	bt_t            *bt;
	_Atomic uint64_t read_snap;   /* 0 == read latest committed */
} xstore_ctx_t;

/* Order-preserving big-endian; the timestamp half is inverted so that,
 * within a rowid, higher commit_ts sorts first (newest version first). */
static void
enc_vkey(int64_t rowid, uint64_t commit_ts, uint8_t out[XS_VKLEN])
{
	uint64_t r = (uint64_t)rowid ^ 0x8000000000000000ull;
	uint64_t t = ~commit_ts;
	int i;
	for (i = 7; i >= 0; i--) { out[i] = (uint8_t)(r & 0xFF); r >>= 8; }
	for (i = 7; i >= 0; i--) { out[8 + i] = (uint8_t)(t & 0xFF); t >>= 8; }
}
static int64_t
dec_rowid(const uint8_t *k)
{
	uint64_t r = 0;
	int i;
	for (i = 0; i < 8; i++) r = (r << 8) | k[i];
	return (int64_t)(r ^ 0x8000000000000000ull);
}
static uint64_t
dec_ts(const uint8_t *k)
{
	uint64_t t = 0;
	int i;
	for (i = 0; i < 8; i++) t = (t << 8) | k[8 + i];
	return ~t;
}

/* ---- vtab + cursor ---- */
typedef struct xstore_vtab {
	sqlite3_vtab base;
	xstore_ctx_t *ctx;
} xstore_vtab_t;

typedef struct xstore_cursor {
	sqlite3_vtab_cursor base;
	bt_t        *bt;
	uint64_t     snap;          /* the read snapshot for this scan */
	int          point;         /* 1 == point lookup (<= one row) */
	int          eof;
	int          have_last;      /* last_key/last_rowid are valid */
	uint8_t      last_key[XS_VKLEN]; /* resume point (key last consumed) */
	int64_t      last_rowid;     /* rowid already resolved (scan dedup) */
	int64_t      rowid;          /* current visible row */
	uint8_t      val[XS_VMAX];
	uint16_t     vlen;
} xstore_cursor_t;

/*
 * The cursor holds NO btree page latch between calls.  SQLite runs an
 * UPDATE/DELETE by keeping its read cursor open and calling xUpdate
 * between xNext calls; if the cursor held a shared content latch, the
 * xUpdate's bt_insert (exclusive latch on the same page) would
 * self-deadlock on the one thread.  So each xFilter/xNext opens a
 * short-lived btree cursor, copies the row out, and closes it --
 * releasing the latch before control returns to the VDBE.  Resuming by
 * key costs an O(log n) re-descent per row; a latch-releasing,
 * position-revalidating btree cursor would restore O(1) amortized
 * (see bench/sqlxtc/PERF_IDEAS.md).
 */

static const sqlite3_module xstore_module;   /* fwd */

static int
xs_connect(sqlite3 *db, void *pAux, int argc, const char *const *argv,
    sqlite3_vtab **ppv, char **pzErr)
{
	xstore_vtab_t *v;
	int rc;
	(void)argc; (void)argv; (void)pzErr;

	rc = sqlite3_declare_vtab(db, "CREATE TABLE x(k INTEGER PRIMARY KEY, v)");
	if (rc != SQLITE_OK)
		return rc;
	v = sqlite3_malloc(sizeof *v);
	if (v == NULL)
		return SQLITE_NOMEM;
	memset(v, 0, sizeof *v);
	v->ctx = (xstore_ctx_t *)pAux;
	*ppv = &v->base;
	return SQLITE_OK;
}

static int
xs_disconnect(sqlite3_vtab *pv)
{
	sqlite3_free(pv);
	return SQLITE_OK;
}

static int
xs_best_index(sqlite3_vtab *pv, sqlite3_index_info *info)
{
	int i;
	(void)pv;
	for (i = 0; i < info->nConstraint; i++) {
		if (info->aConstraint[i].usable &&
		    info->aConstraint[i].iColumn == 0 &&
		    info->aConstraint[i].op == SQLITE_INDEX_CONSTRAINT_EQ) {
			info->aConstraintUsage[i].argvIndex = 1;
			info->aConstraintUsage[i].omit = 1;
			info->idxNum = 1;
			info->estimatedCost = 1.0;
			info->estimatedRows = 1;
			return SQLITE_OK;
		}
	}
	info->idxNum = 0;
	info->estimatedCost = 1.0e6;
	return SQLITE_OK;
}

static int
xs_open(sqlite3_vtab *pv, sqlite3_vtab_cursor **ppc)
{
	xstore_vtab_t *v = (xstore_vtab_t *)pv;
	xstore_cursor_t *c = sqlite3_malloc(sizeof *c);
	if (c == NULL)
		return SQLITE_NOMEM;
	memset(c, 0, sizeof *c);
	c->bt = v->ctx->bt;
	c->eof = 1;
	*ppc = &c->base;
	return SQLITE_OK;
}

static int
xs_close(sqlite3_vtab_cursor *pc)
{
	sqlite3_free(pc);          /* no latch/cursor held between calls */
	return SQLITE_OK;
}

/* Copy the value payload (after the flag byte) into the cursor. */
static void
xs_stash(xstore_cursor_t *c, int64_t rowid, const uint8_t *val, uint16_t vl)
{
	c->rowid = rowid;
	c->vlen = (vl > 1) ? (uint16_t)(vl - 1) : 0;
	if (c->vlen > XS_VMAX) c->vlen = XS_VMAX;
	if (c->vlen) memcpy(c->val, val + 1, c->vlen);
}

/* Advance a full scan to the next visible row, holding the btree latch
 * only for the duration of this call (open + scan + close).  Visible =
 * the newest non-tombstone version of each rowid with commit_ts <=
 * snap (PostgreSQL HeapTupleSatisfiesMVCC). */
static void
xs_advance(xstore_cursor_t *c)
{
	bt_cursor_t *cur = NULL;
	const uint8_t *start = c->have_last ? c->last_key : NULL;
	uint16_t startlen = c->have_last ? XS_VKLEN : 0;

	if (bt_cursor_open(c->bt, start, startlen, &cur) != XTC_OK) {
		c->eof = 1;
		return;
	}
	for (;;) {
		const void *k = NULL, *vv = NULL;
		uint16_t klen = 0, vl = 0;
		const uint8_t *kb, *vb;
		int64_t rid;
		uint64_t ts;

		if (bt_cursor_next(cur, &k, &klen, &vv, &vl) != XTC_OK ||
		    klen != XS_VKLEN) {
			c->eof = 1;
			break;
		}
		kb = (const uint8_t *)k; vb = (const uint8_t *)vv;
		rid = dec_rowid(kb);
		ts = dec_ts(kb);
		if (c->have_last && rid == c->last_rowid)
			continue;              /* the resume key, or an older version */
		if (ts > c->snap)
			continue;              /* created after the snapshot */
		c->last_rowid = rid;
		c->have_last = 1;
		memcpy(c->last_key, kb, XS_VKLEN);
		if (vl >= 1 && (vb[0] & XS_F_DELETED))
			continue;              /* deleted at the snapshot */
		xs_stash(c, rid, vb, vl);
		c->eof = 0;
		break;
	}
	bt_cursor_close(cur);
}

static int
xs_filter(sqlite3_vtab_cursor *pc, int idxNum, const char *idxStr,
    int argc, sqlite3_value **argv)
{
	xstore_cursor_t *c = (xstore_cursor_t *)pc;
	xstore_vtab_t *v = (xstore_vtab_t *)pc->pVtab;
	uint64_t snap;
	(void)idxStr;

	snap = atomic_load_explicit(&v->ctx->read_snap, memory_order_relaxed);
	if (snap == 0)
		snap = atomic_load_explicit(&g_xclock, memory_order_relaxed);
	c->snap = snap;
	c->have_last = 0;
	c->eof = 1;

	if (idxNum == 1 && argc >= 1) {
		/* Point lookup: open a short-lived cursor at the newest version
		 * of the rowid with commit_ts <= snap, copy it out, and close
		 * (release the latch) before returning to the VDBE. */
		bt_cursor_t *cur = NULL;
		uint8_t startk[XS_VKLEN];
		const void *k = NULL, *vv = NULL;
		uint16_t klen = 0, vl = 0;
		int64_t want = sqlite3_value_int64(argv[0]);
		c->point = 1;
		enc_vkey(want, snap, startk);
		if (bt_cursor_open(c->bt, startk, XS_VKLEN, &cur) != XTC_OK)
			return SQLITE_OK;
		if (bt_cursor_next(cur, &k, &klen, &vv, &vl) == XTC_OK &&
		    klen == XS_VKLEN && dec_rowid((const uint8_t *)k) == want &&
		    !(vl >= 1 && (((const uint8_t *)vv)[0] & XS_F_DELETED))) {
			xs_stash(c, want, (const uint8_t *)vv, vl);
			c->eof = 0;
		}
		bt_cursor_close(cur);
		return SQLITE_OK;
	}
	c->point = 0;
	xs_advance(c);
	return SQLITE_OK;
}

static int
xs_next(sqlite3_vtab_cursor *pc)
{
	xstore_cursor_t *c = (xstore_cursor_t *)pc;
	if (c->point) { c->eof = 1; return SQLITE_OK; }
	xs_advance(c);
	return SQLITE_OK;
}

static int
xs_eof(sqlite3_vtab_cursor *pc)
{
	return ((xstore_cursor_t *)pc)->eof;
}

static int
xs_column(sqlite3_vtab_cursor *pc, sqlite3_context *ctx, int i)
{
	xstore_cursor_t *c = (xstore_cursor_t *)pc;
	if (i == 0)
		sqlite3_result_int64(ctx, c->rowid);
	else
		sqlite3_result_blob(ctx, c->val, c->vlen, SQLITE_TRANSIENT);
	return SQLITE_OK;
}

static int
xs_rowid(sqlite3_vtab_cursor *pc, sqlite3_int64 *pRowid)
{
	*pRowid = ((xstore_cursor_t *)pc)->rowid;
	return SQLITE_OK;
}

/* Write one version of `rowid`: a tombstone if deleted, else a copy of
 * `blob`, stamped with a fresh commit timestamp (autocommit). */
static int
xs_put(bt_t *bt, int64_t rowid, const void *blob, int n, int deleted)
{
	uint8_t key[XS_VKLEN];
	uint8_t buf[1 + XS_VMAX];
	uint64_t ts = atomic_fetch_add_explicit(&g_xclock, 1,
	    memory_order_relaxed) + 1;
	if (n < 0) n = 0;
	if (n > XS_VMAX) n = XS_VMAX;
	enc_vkey(rowid, ts, key);
	buf[0] = deleted ? XS_F_DELETED : 0;
	if (!deleted && n > 0) memcpy(buf + 1, blob, (size_t)n);
	return bt_insert(bt, key, XS_VKLEN, buf, (uint16_t)(1 + (deleted ? 0 : n)))
	    == XTC_OK ? SQLITE_OK : SQLITE_ERROR;
}

static int
xs_update(sqlite3_vtab *pv, int argc, sqlite3_value **argv,
    sqlite3_int64 *pRowid)
{
	xstore_vtab_t *v = (xstore_vtab_t *)pv;
	bt_t *bt = v->ctx->bt;

	if (argc == 1) {
		/* DELETE -> write a tombstone version. */
		return xs_put(bt, sqlite3_value_int64(argv[0]), NULL, 0, 1);
	}
	{
		int64_t rowid = (sqlite3_value_type(argv[1]) == SQLITE_NULL)
		    ? sqlite3_value_int64(argv[2])
		    : sqlite3_value_int64(argv[1]);
		const void *blob = sqlite3_value_blob(argv[3]);
		int n = sqlite3_value_bytes(argv[3]);

		/* An UPDATE that moves the rowid tombstones the old one. */
		if (sqlite3_value_type(argv[0]) != SQLITE_NULL) {
			int64_t oldid = sqlite3_value_int64(argv[0]);
			if (oldid != rowid)
				(void)xs_put(bt, oldid, NULL, 0, 1);
		}
		if (pRowid != NULL) *pRowid = rowid;
		return xs_put(bt, rowid, blob, n, 0);
	}
}

/* SQL functions: xstore_now() -> current clock; xstore_as_of(ts) pins
 * this connection's read snapshot (0 = latest) and returns it. */
static void
fn_now(sqlite3_context *ctx, int argc, sqlite3_value **argv)
{
	(void)argc; (void)argv;
	sqlite3_result_int64(ctx,
	    (sqlite3_int64)atomic_load_explicit(&g_xclock, memory_order_relaxed));
}
static void
fn_as_of(sqlite3_context *ctx, int argc, sqlite3_value **argv)
{
	xstore_ctx_t *c = (xstore_ctx_t *)sqlite3_user_data(ctx);
	int64_t ts = (argc >= 1) ? sqlite3_value_int64(argv[0]) : 0;
	if (ts < 0) ts = 0;
	atomic_store_explicit(&c->read_snap, (uint64_t)ts, memory_order_relaxed);
	sqlite3_result_int64(ctx, ts);
}

static const sqlite3_module xstore_module = {
	.iVersion    = 0,
	.xCreate     = xs_connect,
	.xConnect    = xs_connect,
	.xBestIndex  = xs_best_index,
	.xDisconnect = xs_disconnect,
	.xDestroy    = xs_disconnect,
	.xOpen       = xs_open,
	.xClose      = xs_close,
	.xFilter     = xs_filter,
	.xNext       = xs_next,
	.xEof        = xs_eof,
	.xColumn     = xs_column,
	.xRowid      = xs_rowid,
	.xUpdate     = xs_update,
};

static void
ctx_free(void *p)
{
	sqlite3_free(p);
}

int
xstore_register(sqlite3 *db, bt_t *bt)
{
	xstore_ctx_t *ctx = sqlite3_malloc(sizeof *ctx);
	int rc;
	if (ctx == NULL)
		return SQLITE_NOMEM;
	ctx->bt = bt;
	atomic_store(&ctx->read_snap, 0);
	rc = sqlite3_create_module_v2(db, "xstore", &xstore_module, ctx, ctx_free);
	if (rc != SQLITE_OK)
		return rc;
	(void)sqlite3_create_function(db, "xstore_now", 0,
	    SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, fn_now, NULL, NULL);
	(void)sqlite3_create_function(db, "xstore_as_of", 1, SQLITE_UTF8,
	    ctx, fn_as_of, NULL, NULL);
	return SQLITE_OK;
}
