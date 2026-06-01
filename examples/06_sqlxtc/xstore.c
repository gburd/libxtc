/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/xstore.c
 *	A SQLite virtual-table module that runs SQL on the libxtc-native
 *	storage engine (btree.c over the cooling buffer pool, bufmgr.c)
 *	instead of SQLite's built-in B-tree.
 *
 *	SQLite still tokenizes, parses, plans, and runs the VDBE; only
 *	the table I/O is redirected here, where reads and writes land in
 *	our on-disk, larger-than-RAM-capable B-tree.  This is the
 *	supported way to put a custom storage engine under SQLite's SQL
 *	layer -- reimplementing the internal btree.h from the single-file
 *	amalgamation is not practical.  It is also the hardest libxtc
 *	path: when SQL runs on a connection proc, a cursor scan or an
 *	insert can park the fiber on offloaded page I/O in the MIDDLE of
 *	the VDBE, exercising the coroutine stack save/restore under a
 *	deep C call chain.
 *
 *	Schema (a key/value shape; general column storage is future work):
 *		CREATE VIRTUAL TABLE t USING xstore;   -- t(k INTEGER PRIMARY KEY, v)
 *	The B-tree key is the 8-byte order-preserving encoding of the
 *	rowid k; the stored value is the v payload.
 *
 *	MVCC NOTE: this first integration is single-version (the storage
 *	swap).  Snapshot-isolation visibility (xmin/xmax-style, the
 *	PostgreSQL heap model) and serializability (Cahill SSI, as adopted
 *	by PostgreSQL 9.1) layer on top of this storage next; see
 *	docs/M_SQLXTC_MVCC_SQL.md.
 */

#include "xstore.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sqlite3.h"
#include "btree.h"
#include "xtc.h"

/* Order-preserving 8-byte big-endian encoding of a signed rowid, so
 * the B-tree's memcmp key order matches integer order. */
static void
enc_key(int64_t rowid, uint8_t out[8])
{
	uint64_t u = (uint64_t)rowid ^ 0x8000000000000000ull;
	int i;
	for (i = 7; i >= 0; i--) { out[i] = (uint8_t)(u & 0xFF); u >>= 8; }
}
static int64_t
dec_key(const uint8_t *in)
{
	uint64_t u = 0;
	int i;
	for (i = 0; i < 8; i++) u = (u << 8) | in[i];
	return (int64_t)(u ^ 0x8000000000000000ull);
}

/* ---- vtab + cursor ---- */
typedef struct xstore_vtab {
	sqlite3_vtab base;
	bt_t        *bt;            /* shared engine storage (module pAux) */
} xstore_vtab_t;

#define XS_VMAX 4096            /* max stored value bytes per row (one page-ish) */
typedef struct xstore_cursor {
	sqlite3_vtab_cursor base;
	bt_t        *bt;
	bt_cursor_t *scan;          /* full-scan cursor (NULL for point) */
	int          eof;
	int64_t      rowid;         /* current row's key */
	uint8_t      val[XS_VMAX];  /* current row's value (copied out) */
	uint16_t     vlen;
} xstore_cursor_t;

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
	v->bt = (bt_t *)pAux;
	*ppv = &v->base;
	return SQLITE_OK;
}

static int
xs_disconnect(sqlite3_vtab *pv)
{
	sqlite3_free(pv);     /* the bt is engine-owned; do not close it */
	return SQLITE_OK;
}

/* Point lookup on k -> idxNum 1; otherwise a full scan. */
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
	c->bt = v->bt;
	c->eof = 1;
	*ppc = &c->base;
	return SQLITE_OK;
}

static int
xs_close(sqlite3_vtab_cursor *pc)
{
	xstore_cursor_t *c = (xstore_cursor_t *)pc;
	if (c->scan != NULL)
		bt_cursor_close(c->scan);
	sqlite3_free(c);
	return SQLITE_OK;
}

/* Load the scan cursor's current row into the cursor's value buffer. */
static int
xs_scan_load(xstore_cursor_t *c)
{
	const void *k = NULL, *vv = NULL;
	uint16_t klen = 0, vl = 0;
	int rc = bt_cursor_next(c->scan, &k, &klen, &vv, &vl);
	if (rc != XTC_OK) { c->eof = 1; return SQLITE_OK; }
	if (klen != 8) { c->eof = 1; return SQLITE_OK; }
	c->rowid = dec_key((const uint8_t *)k);
	c->vlen = vl > XS_VMAX ? XS_VMAX : vl;
	if (c->vlen) memcpy(c->val, vv, c->vlen);
	c->eof = 0;
	return SQLITE_OK;
}

static int
xs_filter(sqlite3_vtab_cursor *pc, int idxNum, const char *idxStr,
    int argc, sqlite3_value **argv)
{
	xstore_cursor_t *c = (xstore_cursor_t *)pc;
	(void)idxStr;

	if (c->scan != NULL) { bt_cursor_close(c->scan); c->scan = NULL; }

	if (idxNum == 1 && argc >= 1) {
		/* Point lookup on k. */
		uint8_t key[8];
		uint16_t vl = 0;
		int rc;
		c->rowid = sqlite3_value_int64(argv[0]);
		enc_key(c->rowid, key);
		rc = bt_lookup(c->bt, key, 8, c->val, XS_VMAX, &vl);
		c->vlen = vl > XS_VMAX ? XS_VMAX : vl;
		c->eof = (rc != XTC_OK);
		return SQLITE_OK;
	}
	/* Full scan in key (rowid) order. */
	if (bt_cursor_open(c->bt, NULL, 0, &c->scan) != XTC_OK) {
		c->eof = 1;
		return SQLITE_OK;
	}
	return xs_scan_load(c);
}

static int
xs_next(sqlite3_vtab_cursor *pc)
{
	xstore_cursor_t *c = (xstore_cursor_t *)pc;
	if (c->scan != NULL)
		return xs_scan_load(c);
	c->eof = 1;            /* a point lookup yields at most one row */
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

/*
 * INSERT / UPDATE / DELETE.  argv[0] = old rowid (NULL for INSERT);
 * argv[1] = new rowid; argv[2..] = column values (k, v).
 */
static int
xs_update(sqlite3_vtab *pv, int argc, sqlite3_value **argv,
    sqlite3_int64 *pRowid)
{
	xstore_vtab_t *v = (xstore_vtab_t *)pv;
	uint8_t key[8];

	if (argc == 1) {
		/* DELETE */
		enc_key(sqlite3_value_int64(argv[0]), key);
		(void)bt_delete(v->bt, key, 8);
		return SQLITE_OK;
	}
	{
		int64_t rowid = (sqlite3_value_type(argv[1]) == SQLITE_NULL)
		    ? sqlite3_value_int64(argv[2])      /* k column == rowid */
		    : sqlite3_value_int64(argv[1]);
		const void *blob = sqlite3_value_blob(argv[3]);
		int n = sqlite3_value_bytes(argv[3]);
		if (n < 0) n = 0;
		if (n > XS_VMAX) n = XS_VMAX;

		/* On an UPDATE that moves the rowid, drop the old row. */
		if (sqlite3_value_type(argv[0]) != SQLITE_NULL) {
			int64_t oldid = sqlite3_value_int64(argv[0]);
			if (oldid != rowid) {
				uint8_t ok[8];
				enc_key(oldid, ok);
				(void)bt_delete(v->bt, ok, 8);
			}
		}
		enc_key(rowid, key);
		if (bt_insert(v->bt, key, 8, blob, (uint16_t)n) != XTC_OK)
			return SQLITE_ERROR;
		if (pRowid != NULL) *pRowid = rowid;
	}
	return SQLITE_OK;
}

static const sqlite3_module xstore_module = {
	.iVersion    = 0,
	.xCreate     = xs_connect,     /* same as connect: storage is shared */
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

int
xstore_register(sqlite3 *db, bt_t *bt)
{
	return sqlite3_create_module(db, "xstore", &xstore_module, bt);
}
