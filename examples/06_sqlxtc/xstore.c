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
 *	STATUS: implemented.  Transaction-level snapshot isolation spans
 *	multiple statements -- xBegin captures one read snapshot, writes
 *	buffer, and xCommit assigns a single commit timestamp and flushes
 *	the whole transaction (so all of its rows share one xmin); a
 *	multi-statement BEGIN..COMMIT (or separate Quack messages on the
 *	same handle) forms one transaction.  Cahill SSI serializability is
 *	enforced: xs_sync (2PC phase 1) calls ssi_in_conflict and returns
 *	SQLITE_BUSY on a dangerous rw-antidependency pivot, rolling the
 *	transaction back -- with predicate (range) locking so disjoint
 *	readers do not falsely conflict.  See docs/M_SQLXTC_MVCC_SQL.md and
 *	the scenario_serializable / SSI-range tests in test_xstore.c.
 */

#include "xstore.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

#include "sqlite3.h"
#include "btree.h"
#include "wal.h"
#include "xtc.h"
#include "xtc_proc.h"

/* Global logical commit clock for the shared engine B-tree. */
static _Atomic uint64_t g_xclock = 1;

/* Optional process-global write-ahead log (see xstore_set_wal). */
static wal_t *g_xwal = NULL;
void xstore_set_wal(struct wal *w) { g_xwal = (wal_t *)w; }

#define XS_F_DELETED 0x01u     /* value[0] flag: this version is a tombstone */
#define XS_VKLEN     16        /* (rowid:8) + (inverted commit_ts:8) */
#define XS_VMAX      4096       /* max row payload bytes (one page-ish) */

/* A buffered write within an open transaction (see xs_begin/xs_commit). */
typedef struct xs_wrec {
	int64_t  rowid;
	int      deleted;
	uint16_t len;
	uint8_t *data;        /* malloc'd; NULL for a delete */
} xs_wrec_t;

/* Per-connection state, shared by the module and the SQL functions:
 * the engine storage, this connection's read snapshot, and the
 * in-flight transaction's buffered writes. */
typedef struct ssi_txn ssi_txn_t;     /* SSI registry slot (defined below) */
typedef struct xstore_ctx {
	bt_t            *bt;
	_Atomic uint64_t read_snap;   /* 0 == read latest committed */
	int              in_txn;      /* between xBegin and xCommit/xRollback */
	uint64_t         txn_snap;    /* snapshot captured at xBegin */
	xs_wrec_t       *wbuf;        /* buffered writes, applied atomically */
	int              wn, wcap;
	int              serializable; /* 1 == validate the read set on commit */
	int64_t         *rset;        /* rowids read in this txn (serializable) */
	int              rn, rcap;
	int              did_scan;    /* txn did a full scan (table-level read) */
	ssi_txn_t       *ssi;         /* this txn's SSI registry slot (or NULL) */
	int              snap_slot;   /* GC snapshot-hold slot (-1 == none) */
	int              autovacuum;  /* 1 == prune dead versions inline on write */
} xstore_ctx_t;

static void
wbuf_clear(xstore_ctx_t *c)
{
	int i;
	for (i = 0; i < c->wn; i++)
		free(c->wbuf[i].data);
	c->wn = 0;
}
static int
wbuf_add(xstore_ctx_t *c, int64_t rowid, const void *blob, int n, int deleted)
{
	xs_wrec_t *w;
	if (c->wn == c->wcap) {
		int nc = c->wcap ? c->wcap * 2 : 16;
		xs_wrec_t *nb = realloc(c->wbuf, (size_t)nc * sizeof *nb);
		if (nb == NULL) return SQLITE_NOMEM;
		c->wbuf = nb; c->wcap = nc;
	}
	w = &c->wbuf[c->wn];
	w->rowid = rowid; w->deleted = deleted; w->len = 0; w->data = NULL;
	if (!deleted && n > 0) {
		if (n > XS_VMAX) n = XS_VMAX;
		w->data = malloc((size_t)n);
		if (w->data == NULL) return SQLITE_NOMEM;
		memcpy(w->data, blob, (size_t)n);
		w->len = (uint16_t)n;
	}
	c->wn++;
	return SQLITE_OK;
}
/* Read-your-writes: the newest buffered write for `rowid`, or NULL. */
static const xs_wrec_t *
wbuf_find(const xstore_ctx_t *c, int64_t rowid)
{
	int i;
	for (i = c->wn - 1; i >= 0; i--)
		if (c->wbuf[i].rowid == rowid)
			return &c->wbuf[i];
	return NULL;
}

/* Record a rowid in the transaction's read set (serializable mode). */
static void
rset_add(xstore_ctx_t *c, int64_t rowid)
{
	int i;
	for (i = 0; i < c->rn; i++)
		if (c->rset[i] == rowid) return;   /* already recorded */
	if (c->rn == c->rcap) {
		int nc = c->rcap ? c->rcap * 2 : 16;
		int64_t *nb = realloc(c->rset, (size_t)nc * sizeof *nb);
		if (nb == NULL) return;            /* best-effort; a miss is safe-ish */
		c->rset = nb; c->rcap = nc;
	}
	c->rset[c->rn++] = rowid;
}

/*
 * Serializable Snapshot Isolation (Cahill et al., SIGMOD 2008; the
 * model PostgreSQL 9.1+ uses in predicate.c).  The read-set check in
 * xs_sync alone is conservative "precision validation" -- it aborts a
 * transaction whenever something it read was overwritten, i.e. on any
 * outgoing rw-antidependency.  SSI is more permissive: it aborts only
 * a PIVOT -- a transaction with BOTH an incoming and an outgoing
 * rw-antidependency -- because Fekete et al. (2005) proved every
 * serialization-graph cycle under snapshot isolation contains such a
 * pivot, so aborting all pivots breaks all cycles while letting
 * read-mostly transactions (outgoing edge only) commit.
 *
 * The outgoing edge (something I read was overwritten by a committed
 * transaction after my snapshot) is detected from the shared B-tree's
 * version timestamps (xs_newest_ts), so it catches overwrites by any
 * committer regardless of its isolation level.  The incoming edge
 * (someone read what I am about to write) cannot be read from the
 * B-tree -- reads leave no version -- so serializable transactions
 * publish their read sets to this small in-memory registry, and a
 * committing writer scans it for a concurrent reader of any rowid it
 * writes.  Following PostgreSQL, the outgoing edge counts toward an
 * abort only when its target has already COMMITTED (the pivot's
 * out-neighbor commits first): so in a write-skew the first committer
 * (whose out-neighbor is still active) commits and the second aborts.
 */
#define SSI_MAX 256
enum { SSI_FREE = 0, SSI_ACTIVE, SSI_COMMITTED };
struct ssi_txn {
	int       state;
	uint64_t  snap;        /* read snapshot */
	uint64_t  commit_ts;   /* set when COMMITTED */
	int64_t  *reads;       /* rowids this txn read (point predicates) */
	int       nreads, rcap;
	struct ssi_rng { int64_t lo, hi; } *rngs;  /* range predicates (scans) */
	int       nrng, rngcap;
};

static ssi_txn_t       g_ssi[SSI_MAX];
static pthread_mutex_t g_ssi_mu = PTHREAD_MUTEX_INITIALIZER;

/* Free committed slots no active transaction can still conflict with:
 * a committed W is needed only while some active U started before W
 * committed (U.snap < W.commit_ts).  Caller holds g_ssi_mu. */
static void
ssi_gc_locked(void)
{
	uint64_t min_active = ~(uint64_t)0;
	int i;
	for (i = 0; i < SSI_MAX; i++)
		if (g_ssi[i].state == SSI_ACTIVE && g_ssi[i].snap < min_active)
			min_active = g_ssi[i].snap;
	for (i = 0; i < SSI_MAX; i++)
		if (g_ssi[i].state == SSI_COMMITTED &&
		    g_ssi[i].commit_ts <= min_active) {
			free(g_ssi[i].reads);
			g_ssi[i].reads = NULL;
			g_ssi[i].nreads = g_ssi[i].rcap = 0;
			free(g_ssi[i].rngs);
			g_ssi[i].rngs = NULL;
			g_ssi[i].nrng = g_ssi[i].rngcap = 0;
			g_ssi[i].state = SSI_FREE;
		}
}

/* Begin tracking a serializable transaction.  Returns its slot, or
 * NULL if the registry is full (the caller then falls back to
 * precision validation, which is correct, just less permissive). */
static ssi_txn_t *
ssi_begin(uint64_t snap)
{
	ssi_txn_t *t = NULL;
	int i;
	pthread_mutex_lock(&g_ssi_mu);
	ssi_gc_locked();
	for (i = 0; i < SSI_MAX; i++)
		if (g_ssi[i].state == SSI_FREE) {
			t = &g_ssi[i];
			t->state = SSI_ACTIVE;
			t->snap = snap;
			t->commit_ts = 0;
			t->nreads = 0;        /* keep any allocated rcap/reads */
			t->nrng = 0;          /* keep any allocated rngcap/rngs */
			break;
		}
	pthread_mutex_unlock(&g_ssi_mu);
	return t;
}

static void
ssi_record_read(ssi_txn_t *t, int64_t rowid)
{
	int i;
	if (t == NULL) return;
	pthread_mutex_lock(&g_ssi_mu);
	for (i = 0; i < t->nreads; i++)
		if (t->reads[i] == rowid) { pthread_mutex_unlock(&g_ssi_mu); return; }
	if (t->nreads == t->rcap) {
		int nc = t->rcap ? t->rcap * 2 : 16;
		int64_t *nb = realloc(t->reads, (size_t)nc * sizeof *nb);
		if (nb == NULL) { pthread_mutex_unlock(&g_ssi_mu); return; }
		t->reads = nb; t->rcap = nc;
	}
	t->reads[t->nreads++] = rowid;
	pthread_mutex_unlock(&g_ssi_mu);
}

/* Record a RANGE predicate [lo, hi] this txn read (a scan).  A full
 * table scan records [INT64_MIN, INT64_MAX]; a bounded scan its actual
 * key bounds.  This is the Cahill/PostgreSQL predicate lock: a future
 * writer's rowid creates an incoming rw-edge only if it falls inside a
 * range some concurrent reader actually scanned -- not "any write vs
 * any scan". */
static void
ssi_record_range(ssi_txn_t *t, int64_t lo, int64_t hi)
{
	if (t == NULL || lo > hi) return;
	pthread_mutex_lock(&g_ssi_mu);
	if (t->nrng == t->rngcap) {
		int nc = t->rngcap ? t->rngcap * 2 : 8;
		struct ssi_rng *nb = realloc(t->rngs, (size_t)nc * sizeof *nb);
		if (nb == NULL) { pthread_mutex_unlock(&g_ssi_mu); return; }
		t->rngs = nb; t->rngcap = nc;
	}
	t->rngs[t->nrng].lo = lo;
	t->rngs[t->nrng].hi = hi;
	t->nrng++;
	pthread_mutex_unlock(&g_ssi_mu);
}

/* Does any OTHER serializable transaction, concurrent with `me`, have
 * an incoming rw-edge to me -- i.e. did it read a rowid in my write
 * set `wr[0..nwr)` (or scan the whole table)?  Returns 1 if so, or if
 * `me` is NULL (registry full -> conservative). */
static int
ssi_in_conflict(ssi_txn_t *me, const int64_t *wr, int nwr)
{
	int i, j, k, in = 0;
	if (me == NULL) return 1;
	if (nwr == 0) return 0;
	pthread_mutex_lock(&g_ssi_mu);
	for (i = 0; i < SSI_MAX && !in; i++) {
		ssi_txn_t *u = &g_ssi[i];
		if (u == me || u->state == SSI_FREE) continue;
		/* Concurrent: an active txn always overlaps me; a committed
		 * one only if it committed after my snapshot (I didn't see
		 * it).  My own commit is in the future, so it never saw me. */
		if (u->state == SSI_COMMITTED && u->commit_ts <= me->snap) continue;
		for (k = 0; k < nwr; k++) {
			for (j = 0; j < u->nreads; j++)
				if (u->reads[j] == wr[k]) { in = 1; break; }
			if (in) break;
			for (j = 0; j < u->nrng; j++)
				if (wr[k] >= u->rngs[j].lo && wr[k] <= u->rngs[j].hi) {
					in = 1; break;
				}
			if (in) break;
		}
	}
	pthread_mutex_unlock(&g_ssi_mu);
	return in;
}

static void
ssi_commit(ssi_txn_t *t, uint64_t commit_ts)
{
	if (t == NULL) return;
	pthread_mutex_lock(&g_ssi_mu);
	t->state = SSI_COMMITTED;
	t->commit_ts = commit_ts;
	pthread_mutex_unlock(&g_ssi_mu);
}

static void
ssi_abort(ssi_txn_t *t)
{
	if (t == NULL) return;
	pthread_mutex_lock(&g_ssi_mu);
	t->state = SSI_FREE;
	t->nreads = 0;
	t->nrng = 0;
	pthread_mutex_unlock(&g_ssi_mu);
}

/*
 * Snapshot-hold registry, for the version-GC horizon.  A version is
 * dead once NO live read snapshot can reach it: per rowid, every
 * version older than the newest one at-or-before the oldest live
 * snapshot is unreachable (a tombstone that is itself the newest such
 * version, with nothing newer, is unreachable too).  This is the
 * MVCC vacuum horizon -- PostgreSQL's OldestXmin / RecentGlobalXmin.
 *
 * Autocommit statement reads do not register: each reads at the
 * current clock, so it sees at least the surviving newest version,
 * which GC never removes.  Only LONG-LIVED snapshots pin the horizon:
 * an open explicit transaction (txn_snap, held to commit) and an
 * xstore_as_of() time-travel pin (read_snap).  An as_of pin set below
 * the reclaimed horizon is best-effort (its versions may already be
 * gone); explicit transactions always snapshot the current clock and
 * are never below the horizon.
 */
#define SNAP_MAX 256
static uint64_t        g_holds[SNAP_MAX];   /* 0 == free slot */
static pthread_mutex_t g_snap_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t g_gc_horizon = 0;   /* newest reclaimed commit_ts */

/* Set connection slot *slotp to pin `ts` (0 releases it). */
static void
snap_set(int *slotp, uint64_t ts)
{
	int i;
	pthread_mutex_lock(&g_snap_mu);
	if (ts == 0) {
		if (*slotp >= 0) { g_holds[*slotp] = 0; *slotp = -1; }
	} else {
		if (*slotp < 0)
			for (i = 0; i < SNAP_MAX; i++)
				if (g_holds[i] == 0) { *slotp = i; break; }
		if (*slotp >= 0) g_holds[*slotp] = ts;
		/* registry full: leave unpinned -- horizon stays conservative
		 * (g_xclock-bounded), so correctness holds; only an as_of pin
		 * could then be GC'd early. */
	}
	pthread_mutex_unlock(&g_snap_mu);
}

/* The GC horizon: the oldest live pinned snapshot, or `now` if none. */
static uint64_t
snap_horizon(uint64_t now)
{
	uint64_t h = now;
	int i;
	pthread_mutex_lock(&g_snap_mu);
	for (i = 0; i < SNAP_MAX; i++)
		if (g_holds[i] != 0 && g_holds[i] < h) h = g_holds[i];
	pthread_mutex_unlock(&g_snap_mu);
	return h;
}

/* Republish this connection's effective GC pin: its open-transaction
 * snapshot, else its as_of read snapshot, else none.  Called wherever
 * those change. */
static void
xs_pin_recompute(xstore_ctx_t *cx)
{
	uint64_t pin = 0;
	if (cx->in_txn)
		pin = cx->txn_snap;
	else {
		uint64_t rs = atomic_load_explicit(&cx->read_snap, memory_order_relaxed);
		if (rs != 0) pin = rs;
	}
	snap_set(&cx->snap_slot, pin);
}

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
	xsql_vtab base;
	xstore_ctx_t *ctx;
	xsql      *db;     /* for xsql_get_autocommit: detect explicit txn */
} xstore_vtab_t;

/*
 * Enter the SQL transaction on first vtab access.  SQLite fires xBegin
 * at the first WRITE, not the first read, so reads before the first
 * write would otherwise miss the transaction (and the read set, and
 * the start snapshot).  xsql_get_autocommit() == 0 means an explicit
 * BEGIN..COMMIT is open; capture the snapshot and open the
 * buffer/read-set on the first access within it.  Autocommit
 * single-statement work leaves in_txn == 0 (immediate write, latest
 * read).
 */
static void
xs_enter(xstore_vtab_t *v)
{
	xstore_ctx_t *cx = v->ctx;
	if (!xsql_get_autocommit(v->db) && !cx->in_txn) {
		cx->in_txn = 1;
		cx->txn_snap = atomic_load_explicit(&g_xclock, memory_order_relaxed);
		wbuf_clear(cx);
		cx->rn = 0;
		cx->did_scan = 0;
		cx->ssi = cx->serializable ? ssi_begin(cx->txn_snap) : NULL;
		xs_pin_recompute(cx);     /* pin txn_snap against version GC */
	}
}

typedef struct xstore_cursor {
	xsql_vtab_cursor base;
	bt_t        *bt;
	xstore_ctx_t *ctx;
	uint64_t     snap;          /* the read snapshot for this scan */
	int          point;         /* 1 == point lookup (<= one row) */
	int          eof;
	int64_t      lo, hi;         /* range scan bounds (rowid), if has_* */
	int          has_lo, has_hi;
	int          have_last;      /* last_key/last_rowid are valid */
	uint8_t      last_key[XS_VKLEN]; /* resume point (key last consumed) */
	int64_t      last_rowid;     /* rowid already resolved (scan dedup) */
	int64_t      rowid;          /* current visible row */
	uint8_t      val[XS_VMAX];
	uint16_t     vlen;
	bt_cursor_t *btc;            /* persistent scan cursor, parked between calls */
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

static const xsql_module xstore_module;   /* fwd */

static int
xs_connect(xsql *db, void *pAux, int argc, const char *const *argv,
    xsql_vtab **ppv, char **pzErr)
{
	xstore_vtab_t *v;
	int rc;
	(void)argc; (void)argv; (void)pzErr;

	rc = xsql_declare_vtab(db, "CREATE TABLE x(k INTEGER PRIMARY KEY, v)");
	if (rc != SQLITE_OK)
		return rc;
	v = xsql_malloc(sizeof *v);
	if (v == NULL)
		return SQLITE_NOMEM;
	memset(v, 0, sizeof *v);
	v->ctx = (xstore_ctx_t *)pAux;
	v->db = db;
	*ppv = &v->base;
	return SQLITE_OK;
}

static int
xs_disconnect(xsql_vtab *pv)
{
	xsql_free(pv);
	return SQLITE_OK;
}

static int
xs_best_index(xsql_vtab *pv, xsql_index_info *info)
{
	int i, idx = 0, argc = 0, eq = -1, lo = -1, hi = -1;
	(void)pv;
	for (i = 0; i < info->nConstraint; i++) {
		if (!info->aConstraint[i].usable || info->aConstraint[i].iColumn != 0)
			continue;
		switch (info->aConstraint[i].op) {
		case SQLITE_INDEX_CONSTRAINT_EQ: eq = i; break;
		case SQLITE_INDEX_CONSTRAINT_GT:
		case SQLITE_INDEX_CONSTRAINT_GE: lo = i; break;
		case SQLITE_INDEX_CONSTRAINT_LT:
		case SQLITE_INDEX_CONSTRAINT_LE: hi = i; break;
		default: break;
		}
	}
	if (eq >= 0) {                          /* point lookup */
		info->aConstraintUsage[eq].argvIndex = 1;
		info->aConstraintUsage[eq].omit = 1;
		info->idxNum = 1;
		info->estimatedCost = 1.0;
		info->estimatedRows = 1;
		return SQLITE_OK;
	}
	/* Bounded range: pass the bounds to xFilter (idxNum bits 2/4), but
	 * do NOT omit -- the bounds drive the scan seek/stop and the SSI
	 * predicate; SQLite re-applies the exact (possibly exclusive)
	 * comparison, so an inclusive scan superset is safe. */
	if (lo >= 0) { info->aConstraintUsage[lo].argvIndex = ++argc; idx |= 2; }
	if (hi >= 0) { info->aConstraintUsage[hi].argvIndex = ++argc; idx |= 4; }
	info->idxNum = idx;
	info->estimatedCost = idx ? 100.0 : 1.0e6;
	return SQLITE_OK;
}

static int
xs_open(xsql_vtab *pv, xsql_vtab_cursor **ppc)
{
	xstore_vtab_t *v = (xstore_vtab_t *)pv;
	xstore_cursor_t *c = xsql_malloc(sizeof *c);
	if (c == NULL)
		return SQLITE_NOMEM;
	memset(c, 0, sizeof *c);
	c->bt = v->ctx->bt;
	c->ctx = v->ctx;
	c->eof = 1;
	*ppc = &c->base;
	return SQLITE_OK;
}

static int
xs_close(xsql_vtab_cursor *pc)
{
	xstore_cursor_t *c = (xstore_cursor_t *)pc;
	if (c->btc != NULL) {           /* release any parked scan cursor */
		bt_cursor_close(c->btc);
		c->btc = NULL;
	}
	xsql_free(pc);          /* no latch/cursor held between calls */
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

/* Advance a full scan to the next visible row.  The scan cursor is
 * persistent: opened once with a full descent, then resumed in O(1)
 * past the last key consumed.  The leaf latch is held only for the
 * duration of this call -- the cursor is PARKED (latch released)
 * before control returns to the VDBE, so an interleaved xUpdate cannot
 * self-deadlock against a held content latch.  Visible = the newest
 * non-tombstone version of each rowid with commit_ts <= snap
 * (PostgreSQL HeapTupleSatisfiesMVCC). */
static void
xs_advance(xstore_cursor_t *c)
{
	if (c->btc == NULL) {
		uint8_t startbuf[XS_VKLEN];
		const uint8_t *start;
		uint16_t startlen;
		if (c->have_last) {
			start = c->last_key; startlen = XS_VKLEN;
		} else if (c->has_lo) {       /* seek to the start of the lo bound */
			enc_vkey(c->lo, ~(uint64_t)0, startbuf);
			start = startbuf; startlen = XS_VKLEN;
		} else {
			start = NULL; startlen = 0;
		}
		if (bt_cursor_open(c->bt, start, startlen, &c->btc) != XTC_OK) {
			c->eof = 1;
			return;
		}
	} else if (bt_cursor_resume(c->btc) != XTC_OK) {
		c->eof = 1;
		return;
	}
	for (;;) {
		const void *k = NULL, *vv = NULL;
		uint16_t klen = 0, vl = 0;
		const uint8_t *kb, *vb;
		int64_t rid;
		uint64_t ts;

		if (bt_cursor_next(c->btc, &k, &klen, &vv, &vl) != XTC_OK ||
		    klen != XS_VKLEN) {
			c->eof = 1;
			break;
		}
		kb = (const uint8_t *)k; vb = (const uint8_t *)vv;
		rid = dec_rowid(kb);
		ts = dec_ts(kb);
		if (c->has_hi && rid > c->hi) {
			c->eof = 1;             /* past the upper bound: range exhausted */
			break;
		}
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
	bt_cursor_park(c->btc);            /* release the latch before returning */
}

static int
xs_filter(xsql_vtab_cursor *pc, int idxNum, const char *idxStr,
    int argc, xsql_value **argv)
{
	xstore_cursor_t *c = (xstore_cursor_t *)pc;
	xstore_vtab_t *v = (xstore_vtab_t *)pc->pVtab;
	uint64_t snap;
	(void)idxStr;

	if (c->btc != NULL) {           /* restart: drop any prior scan cursor */
		bt_cursor_close(c->btc);
		c->btc = NULL;
	}
	xs_enter(v);
	snap = atomic_load_explicit(&v->ctx->read_snap, memory_order_relaxed);
	if (snap == 0)
		snap = v->ctx->in_txn
		    ? v->ctx->txn_snap          /* repeatable snapshot for the txn */
		    : atomic_load_explicit(&g_xclock, memory_order_relaxed);
	c->snap = snap;
	c->have_last = 0;
	c->eof = 1;

	if (idxNum == 1 && argc >= 1) {
		/* Point lookup.  Read-your-writes first: a write buffered in
		 * this open transaction supersedes the committed version. */
		bt_cursor_t *cur = NULL;
		uint8_t startk[XS_VKLEN];
		const void *k = NULL, *vv = NULL;
		uint16_t klen = 0, vl = 0;
		int64_t want = xsql_value_int64(argv[0]);
		c->point = 1;
		if (v->ctx->in_txn && v->ctx->serializable) {
			rset_add(v->ctx, want);   /* read set for serializable validation */
			ssi_record_read(v->ctx->ssi, want);
		}
		if (v->ctx->in_txn) {
			const xs_wrec_t *w = wbuf_find(v->ctx, want);
			if (w != NULL) {
				if (!w->deleted) {
					c->rowid = want;
					c->vlen = w->len;
					if (w->len) memcpy(c->val, w->data, w->len);
					c->eof = 0;
				}
				return SQLITE_OK;       /* buffered write decides it */
			}
		}
		/* Else the newest committed version at-or-before the snapshot. */
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
	/* Range bounds from best_index (idxNum bit 2 = lo, bit 4 = hi). */
	c->has_lo = c->has_hi = 0;
	{
		int ai = 0;
		if ((idxNum & 2) && argc > ai) { c->lo = xsql_value_int64(argv[ai++]); c->has_lo = 1; }
		if ((idxNum & 4) && argc > ai) { c->hi = xsql_value_int64(argv[ai++]); c->has_hi = 1; }
	}
	if (v->ctx->in_txn && v->ctx->serializable) {
		int64_t rlo = c->has_lo ? c->lo : INT64_MIN;
		int64_t rhi = c->has_hi ? c->hi : INT64_MAX;
		v->ctx->did_scan = 1;     /* conservative outgoing edge */
		ssi_record_range(v->ctx->ssi, rlo, rhi);  /* precise incoming predicate */
	}
	xs_advance(c);
	return SQLITE_OK;
}

static int
xs_next(xsql_vtab_cursor *pc)
{
	xstore_cursor_t *c = (xstore_cursor_t *)pc;
	if (c->point) { c->eof = 1; return SQLITE_OK; }
	xs_advance(c);
	return SQLITE_OK;
}

static int
xs_eof(xsql_vtab_cursor *pc)
{
	return ((xstore_cursor_t *)pc)->eof;
}

static int
xs_column(xsql_vtab_cursor *pc, xsql_context *ctx, int i)
{
	xstore_cursor_t *c = (xstore_cursor_t *)pc;
	if (i == 0)
		xsql_result_int64(ctx, c->rowid);
	else
		xsql_result_blob(ctx, c->val, c->vlen, SQLITE_TRANSIENT);
	return SQLITE_OK;
}

static int
xs_rowid(xsql_vtab_cursor *pc, xsql_int64 *pRowid)
{
	*pRowid = ((xstore_cursor_t *)pc)->rowid;
	return SQLITE_OK;
}

/* Write one version of `rowid`: a tombstone if deleted, else a copy of
 * `blob`, stamped with a fresh commit timestamp (autocommit). */
/* Dispatch one WAL record to the log durably: group commit on a loop
 * (parks the fiber on the writer's ack), synchronous append off a loop.
 * No-op when no WAL is attached. */
static void
xs_wal_emit(const uint8_t *rec, size_t sz)
{
	uint64_t lsn = 0;
	if (g_xwal == NULL)
		return;
	if (xtc_pid_is_none(xtc_self()))
		(void)wal_commit_sync(g_xwal, rec, (uint32_t)sz, &lsn);
	else
		(void)wal_commit(g_xwal, rec, (uint32_t)sz, &lsn);
}

/* Log a single committed version (the autocommit single-statement
 * path); same record layout as the multi-version xs_wal_log. */
static void
xs_wal_put(uint64_t ts, int64_t rowid, const void *val, int vlen, int deleted)
{
	uint8_t rec[12 + 11 + XS_VMAX];
	uint8_t *p = rec;
	uint32_t n = 1;
	uint8_t flags = deleted ? XS_F_DELETED : 0;
	uint16_t vl = (deleted || vlen < 0) ? 0
	    : (uint16_t)(vlen > XS_VMAX ? XS_VMAX : vlen);
	memcpy(p, &ts, 8); p += 8;
	memcpy(p, &n, 4); p += 4;
	memcpy(p, &rowid, 8); p += 8;
	*p++ = flags;
	memcpy(p, &vl, 2); p += 2;
	if (vl) { memcpy(p, val, vl); p += vl; }
	xs_wal_emit(rec, (size_t)(p - rec));
}

static int
xs_put(bt_t *bt, int64_t rowid, const void *blob, int n, int deleted)
{
	uint8_t key[XS_VKLEN];
	uint8_t buf[1 + XS_VMAX];
	uint64_t ts = atomic_fetch_add_explicit(&g_xclock, 1,
	    memory_order_relaxed) + 1;
	if (n < 0) n = 0;
	if (n > XS_VMAX) n = XS_VMAX;
	if (g_xwal != NULL)
		xs_wal_put(ts, rowid, blob, n, deleted);   /* durable BEFORE apply */
	enc_vkey(rowid, ts, key);
	buf[0] = deleted ? XS_F_DELETED : 0;
	if (!deleted && n > 0) memcpy(buf + 1, blob, (size_t)n);
	return bt_insert(bt, key, XS_VKLEN, buf, (uint16_t)(1 + (deleted ? 0 : n)))
	    == XTC_OK ? SQLITE_OK : SQLITE_ERROR;
}

/* The commit_ts of the newest committed version of `rowid`, or 0 if the
 * rowid has no version.  Used by serializable validation to detect a
 * concurrent write to a key this transaction read. */
static uint64_t
xs_newest_ts(bt_t *bt, int64_t rowid)
{
	bt_cursor_t *cur = NULL;
	uint8_t startk[XS_VKLEN];
	const void *k = NULL, *vv = NULL;
	uint16_t klen = 0, vl = 0;
	uint64_t ts = 0;
	enc_vkey(rowid, ~(uint64_t)0, startk);   /* newest version sorts first */
	if (bt_cursor_open(bt, startk, XS_VKLEN, &cur) != XTC_OK)
		return 0;
	if (bt_cursor_next(cur, &k, &klen, &vv, &vl) == XTC_OK &&
	    klen == XS_VKLEN && dec_rowid((const uint8_t *)k) == rowid)
		ts = dec_ts((const uint8_t *)k);
	bt_cursor_close(cur);
	return ts;
}

/*
 * Version GC (MVCC vacuum).  Reclaims versions no live snapshot can
 * reach: scanning in key order the cursor sees each rowid's versions
 * newest-first (commit_ts descending), so for each rowid we keep every
 * version newer than `horizon`, keep the first version at-or-before
 * `horizon` (the survivor that snapshots at the horizon still need),
 * and delete the rest.  A survivor that is a tombstone with nothing
 * newer is itself unreachable and is dropped, so a deleted row
 * eventually leaves no trace.
 *
 * Latches must not span the vtable/btree boundary, and bt_delete
 * wants the exclusive latch a live cursor would hold, so this collects
 * the dead keys under an open cursor, closes it, then deletes -- the
 * same discipline the xstore cursor uses.  Deletes run in bounded
 * batches so memory stays O(batch), not O(dead).  Concurrent inserts
 * are safe: a committer writes at a timestamp above the horizon, which
 * GC never touches.
 */
#define XS_GC_BATCH 512
static int
xs_gc(bt_t *bt, uint64_t horizon, int *out_reclaimed)
{
	uint8_t resume[XS_VKLEN];
	int have_resume = 0, reclaimed = 0, rc = XTC_OK;

	for (;;) {
		bt_cursor_t *cur = NULL;
		uint8_t dead[XS_GC_BATCH][XS_VKLEN];
		int ndead = 0, i;
		int64_t prev_rowid = 0;
		int in_group = 0, survivor_found = 0;
		const uint8_t *start = have_resume ? resume : NULL;

		if (bt_cursor_open(bt, start, have_resume ? XS_VKLEN : 0, &cur) != XTC_OK)
			break;
		for (;;) {
			const void *k = NULL, *vv = NULL;
			uint16_t klen = 0, vl = 0;
			int64_t rid; uint64_t ts; int deleted, first_in_group;

			if (bt_cursor_next(cur, &k, &klen, &vv, &vl) != XTC_OK ||
			    klen != XS_VKLEN)
				break;
			/* Skip the resume key itself (already processed). */
			if (have_resume && memcmp(k, resume, XS_VKLEN) == 0)
				continue;
			rid = dec_rowid((const uint8_t *)k);
			ts = dec_ts((const uint8_t *)k);
			deleted = (vl >= 1 && (((const uint8_t *)vv)[0] & XS_F_DELETED));
			first_in_group = (!in_group || rid != prev_rowid);
			if (first_in_group) {
				prev_rowid = rid; in_group = 1; survivor_found = 0;
			}
			if (!survivor_found) {
				if (ts <= horizon) {
					survivor_found = 1;
					if (deleted && first_in_group)
						memcpy(dead[ndead++], k, XS_VKLEN);
				}
				/* else ts > horizon: keep (newer than horizon). */
			} else {
				memcpy(dead[ndead++], k, XS_VKLEN);   /* older than survivor */
			}
			if (ndead == XS_GC_BATCH) {
				memcpy(resume, k, XS_VKLEN);
				have_resume = 1;
				break;
			}
		}
		bt_cursor_close(cur);
		for (i = 0; i < ndead; i++) {
			if (bt_delete(bt, dead[i], XS_VKLEN) == XTC_OK) reclaimed++;
		}
		if (ndead < XS_GC_BATCH)
			break;     /* scanned to the end */
	}
	if (out_reclaimed != NULL) *out_reclaimed = reclaimed;
	return rc;
}

/*
 * Opportunistic inline prune of ONE rowid's dead versions, up to the
 * given horizon -- PostgreSQL's HOT-style pruning: triggered on the
 * write that creates a new version, it touches only this rowid's
 * version chain (a handful of entries, on pages already hot from the
 * write), so it is incremental and cache-friendly, unlike a full-tree
 * vacuum.  Same keep rule as xs_gc, restricted to one rowid; same
 * collect-then-delete discipline (no latch across the cursor).
 */
static int
xs_prune_rowid(bt_t *bt, int64_t rowid, uint64_t horizon)
{
	bt_cursor_t *cur = NULL;
	uint8_t startk[XS_VKLEN];
	uint8_t dead[16][XS_VKLEN];
	int ndead = 0, i, survivor_found = 0, first = 1, reclaimed = 0;

	enc_vkey(rowid, ~(uint64_t)0, startk);    /* newest version first */
	if (bt_cursor_open(bt, startk, XS_VKLEN, &cur) != XTC_OK)
		return 0;
	for (;;) {
		const void *k = NULL, *vv = NULL;
		uint16_t klen = 0, vl = 0;
		uint64_t ts; int deleted;

		if (bt_cursor_next(cur, &k, &klen, &vv, &vl) != XTC_OK ||
		    klen != XS_VKLEN ||
		    dec_rowid((const uint8_t *)k) != rowid)
			break;                            /* past this rowid */
		ts = dec_ts((const uint8_t *)k);
		deleted = (vl >= 1 && (((const uint8_t *)vv)[0] & XS_F_DELETED));
		if (!survivor_found) {
			if (ts <= horizon) {
				survivor_found = 1;
				if (deleted && first) memcpy(dead[ndead++], k, XS_VKLEN);
			}
		} else {
			memcpy(dead[ndead++], k, XS_VKLEN);
		}
		first = 0;
		if (ndead == (int)(sizeof dead / sizeof dead[0])) break;
	}
	bt_cursor_close(cur);
	for (i = 0; i < ndead; i++)
		if (bt_delete(bt, dead[i], XS_VKLEN) == XTC_OK) reclaimed++;
	return reclaimed;
}

/*
 * Adaptive autovacuum gate.  Pruning every write is pure overhead on a
 * workload whose version chains are naturally short (each prune walks
 * the chain and finds nothing).  This self-tunes: prune every write
 * while prunes keep reclaiming versions (hot keys, long chains), but
 * back off geometrically -- pruning only 1 in `interval` writes -- when
 * a prune finds nothing, so a uniform workload pays almost no prune
 * cost.  Global (the engine is one shared store); a couple of relaxed
 * atomics, no per-rowid state. */
#define XS_AV_MAX_INTERVAL 256
static _Atomic int g_av_interval = 1;
static _Atomic int g_av_countdown = 1;
static _Atomic uint64_t g_av_prunes = 0;   /* prune passes actually run */
static void
xs_maybe_prune(bt_t *bt, int64_t rowid, uint64_t horizon)
{
	int reclaimed, iv;
	if (atomic_fetch_sub_explicit(&g_av_countdown, 1, memory_order_relaxed) > 1)
		return;                               /* not this write */
	reclaimed = xs_prune_rowid(bt, rowid, horizon);
	atomic_fetch_add_explicit(&g_av_prunes, 1, memory_order_relaxed);
	if (reclaimed > 0) {
		iv = 1;                               /* productive: prune every write */
	} else {
		iv = atomic_load_explicit(&g_av_interval, memory_order_relaxed) * 2;
		if (iv > XS_AV_MAX_INTERVAL) iv = XS_AV_MAX_INTERVAL;
	}
	atomic_store_explicit(&g_av_interval, iv, memory_order_relaxed);
	atomic_store_explicit(&g_av_countdown, iv, memory_order_relaxed);
}

/* Write a version, then (if this connection enabled autovacuum) prune
 * the rowid's now-dead older versions up to the live-snapshot horizon. */
static int
xs_put_pruned(xstore_ctx_t *cx, int64_t rowid, const void *blob, int n, int deleted)
{
	int rc = xs_put(cx->bt, rowid, blob, n, deleted);
	if (rc == SQLITE_OK && cx->autovacuum)
		xs_maybe_prune(cx->bt, rowid,
		    snap_horizon(atomic_load_explicit(&g_xclock, memory_order_relaxed)));
	return rc;
}

static int
xs_update(xsql_vtab *pv, int argc, xsql_value **argv,
    xsql_int64 *pRowid)
{
	xstore_vtab_t *v = (xstore_vtab_t *)pv;
	xstore_ctx_t *cx = v->ctx;

	xs_enter(v);
	/*
	 * Buffer the write; xCommit flushes the whole transaction at ONE
	 * commit timestamp (atomic multi-row commit).  SQLite wraps every
	 * write -- explicit BEGIN..COMMIT or an implicit single-statement
	 * transaction -- in xBegin/xCommit, so a buffer is always open
	 * here.  (Fallback: if no transaction is open, apply immediately.)
	 */
	if (argc == 1) {
		int64_t rid = xsql_value_int64(argv[0]);   /* DELETE */
		if (cx->in_txn)
			return wbuf_add(cx, rid, NULL, 0, 1);
		return xs_put_pruned(cx, rid, NULL, 0, 1);
	}
	{
		int64_t rowid = (xsql_value_type(argv[1]) == SQLITE_NULL)
		    ? xsql_value_int64(argv[2])
		    : xsql_value_int64(argv[1]);
		const void *blob = xsql_value_blob(argv[3]);
		int n = xsql_value_bytes(argv[3]);

		/* An UPDATE that moves the rowid tombstones the old one. */
		if (xsql_value_type(argv[0]) != SQLITE_NULL) {
			int64_t oldid = xsql_value_int64(argv[0]);
			if (oldid != rowid) {
				if (cx->in_txn) (void)wbuf_add(cx, oldid, NULL, 0, 1);
				else (void)xs_put_pruned(cx, oldid, NULL, 0, 1);
			}
		}
		if (pRowid != NULL) *pRowid = rowid;
		if (cx->in_txn)
			return wbuf_add(cx, rowid, blob, n, 0);
		return xs_put_pruned(cx, rowid, blob, n, 0);
	}
}

/*
 * Transaction hooks.  xBegin captures the read snapshot and opens the
 * write buffer; xCommit assigns ONE commit timestamp and flushes every
 * buffered write at it (so a multi-row transaction is atomic and all
 * its rows share an xmin); xRollback discards the buffer.  This is
 * mvcc.c's stage-then-commit, applied to the B-tree-backed store.
 */
static int
xs_begin(xsql_vtab *pv)
{
	xs_enter((xstore_vtab_t *)pv);
	return SQLITE_OK;
}
static int
xs_sync(xsql_vtab *pv)
{
	xstore_ctx_t *cx = ((xstore_vtab_t *)pv)->ctx;
	int i, out_edge = 0, in_edge;
	int64_t *wr;
	int nwr;
	if (!cx->in_txn || !cx->serializable)
		return SQLITE_OK;
	/*
	 * Serializable validation, in xSync (2PC phase 1) so a failure
	 * rolls the transaction back -- xCommit (phase 2) is too late.
	 * Cahill SSI pivot detection (docs/M_SQLXTC_MVCC_SQL.md): abort
	 * only if this transaction has BOTH an outgoing and an incoming
	 * rw-antidependency.  This commits read-mostly transactions that
	 * the older precision validation (any overwritten read -> abort)
	 * would have failed, while still forbidding write-skew.
	 *
	 * Outgoing edge: a key we read was overwritten by a transaction
	 * that committed after our snapshot -- read straight from the
	 * shared B-tree's version timestamps, so it sees every committer.
	 * Counts toward an abort only because such a writer has, by
	 * definition, already committed (the pivot's out-neighbor commits
	 * first); in a write-skew this makes the second committer abort.
	 */
	for (i = 0; i < cx->rn; i++)
		if (xs_newest_ts(cx->bt, cx->rset[i]) > cx->txn_snap) { out_edge = 1; break; }
	if (cx->did_scan &&
	    atomic_load_explicit(&g_xclock, memory_order_relaxed) > cx->txn_snap)
		out_edge = 1;
	if (!out_edge)
		return SQLITE_OK;          /* no outgoing edge -> cannot be a pivot */
	/*
	 * Incoming edge: another concurrent serializable transaction read
	 * a rowid we are about to write.  Reads leave no version in the
	 * B-tree, so this is answered from the in-memory SSI registry,
	 * scanned against our buffered write set.
	 */
	wr = malloc((size_t)(cx->wn ? cx->wn : 1) * sizeof *wr);
	nwr = 0;
	if (wr != NULL)
		for (i = 0; i < cx->wn; i++)
			wr[nwr++] = cx->wbuf[i].rowid;
	in_edge = ssi_in_conflict(cx->ssi, wr, nwr);
	free(wr);
	return in_edge ? SQLITE_BUSY : SQLITE_OK;   /* pivot -> serialization failure */
}

/*
 * Serialize the transaction's buffered versions into one WAL record and
 * make it DURABLE before any of them touch the B-tree.  Record layout:
 *
 *	[u64 commit_ts][u32 n]  then n x { [i64 rowid][u8 flags][u16 vlen][vlen] }
 *
 * Logging before applying is what lets recovery be redo-only: a crash
 * after this returns can redo the commit from the log (idempotent --
 * version keys (rowid, ~commit_ts) are immutable), and a crash before
 * it has not touched the B-tree, so there is nothing to undo.  On a
 * loop the commit joins the group-commit batch (one fsync for many);
 * off a loop it appends synchronously.
 */
static void
xs_wal_log(xstore_ctx_t *cx, uint64_t ts)
{
	size_t sz = 12;
	uint8_t *rec, *p;
	uint32_t n;
	int i;

	for (i = 0; i < cx->wn; i++)
		sz += 11 + (cx->wbuf[i].deleted ? 0 : cx->wbuf[i].len);
	rec = malloc(sz);
	if (rec == NULL)
		return;                       /* best-effort; durability lost on OOM */
	p = rec;
	memcpy(p, &ts, 8); p += 8;
	n = (uint32_t)cx->wn;
	memcpy(p, &n, 4); p += 4;
	for (i = 0; i < cx->wn; i++) {
		xs_wrec_t *w = &cx->wbuf[i];
		uint8_t flags = w->deleted ? XS_F_DELETED : 0;
		uint16_t vlen = w->deleted ? 0 : w->len;
		memcpy(p, &w->rowid, 8); p += 8;
		*p++ = flags;
		memcpy(p, &vlen, 2); p += 2;
		if (vlen) { memcpy(p, w->data, vlen); p += vlen; }
	}
	xs_wal_emit(rec, (size_t)(p - rec));
	free(rec);
}

static int
xs_commit(xsql_vtab *pv)
{
	xstore_ctx_t *cx = ((xstore_vtab_t *)pv)->ctx;
	uint64_t ts;
	int i, rc = SQLITE_OK;
	if (!cx->in_txn)
		return SQLITE_OK;
	if (cx->wn > 0) {
		ts = atomic_fetch_add_explicit(&g_xclock, 1,
		    memory_order_relaxed) + 1;     /* one timestamp for the txn */
		if (g_xwal != NULL)
			xs_wal_log(cx, ts);            /* durable BEFORE apply */
		for (i = 0; i < cx->wn; i++) {
			uint8_t key[XS_VKLEN];
			uint8_t buf[1 + XS_VMAX];
			xs_wrec_t *w = &cx->wbuf[i];
			enc_vkey(w->rowid, ts, key);
			buf[0] = w->deleted ? XS_F_DELETED : 0;
			if (!w->deleted && w->len) memcpy(buf + 1, w->data, w->len);
			if (bt_insert(cx->bt, key, XS_VKLEN, buf,
			    (uint16_t)(1 + (w->deleted ? 0 : w->len))) != XTC_OK)
				rc = SQLITE_ERROR;
			else if (cx->autovacuum)
				xs_maybe_prune(cx->bt, w->rowid, snap_horizon(ts));
		}
		ssi_commit(cx->ssi, ts);     /* publish commit_ts for SSI peers */
	} else {
		/* read-only: keep the read set visible to concurrent peers
		 * until GC, stamped at the current clock. */
		ssi_commit(cx->ssi,
		    atomic_load_explicit(&g_xclock, memory_order_relaxed));
	}
	cx->ssi = NULL;
	wbuf_clear(cx);
	cx->in_txn = 0;
	xs_pin_recompute(cx);     /* txn done: drop (or re-pin as_of) */
	return rc;
}
static int
xs_rollback(xsql_vtab *pv)
{
	xstore_ctx_t *cx = ((xstore_vtab_t *)pv)->ctx;
	ssi_abort(cx->ssi);
	cx->ssi = NULL;
	wbuf_clear(cx);
	cx->in_txn = 0;
	xs_pin_recompute(cx);
	return SQLITE_OK;
}

/* SQL functions: xstore_now() -> current clock; xstore_as_of(ts) pins
 * this connection's read snapshot (0 = latest) and returns it. */
static void
fn_now(xsql_context *ctx, int argc, xsql_value **argv)
{
	(void)argc; (void)argv;
	xsql_result_int64(ctx,
	    (xsql_int64)atomic_load_explicit(&g_xclock, memory_order_relaxed));
}
static void
fn_as_of(xsql_context *ctx, int argc, xsql_value **argv)
{
	xstore_ctx_t *c = (xstore_ctx_t *)xsql_user_data(ctx);
	int64_t ts = (argc >= 1) ? xsql_value_int64(argv[0]) : 0;
	if (ts < 0) ts = 0;
	atomic_store_explicit(&c->read_snap, (uint64_t)ts, memory_order_relaxed);
	xs_pin_recompute(c);      /* pin (or release) the as_of snapshot */
	xsql_result_int64(ctx, ts);
}
/* xstore_serializable(on): set this connection's isolation to
 * serializable (on != 0) or snapshot isolation (0).  Returns the new
 * setting.  Serializable validates the transaction's read set at
 * commit and aborts (SQLITE_BUSY) on a read-write conflict. */
static void
fn_serializable(xsql_context *ctx, int argc, xsql_value **argv)
{
	xstore_ctx_t *c = (xstore_ctx_t *)xsql_user_data(ctx);
	int on = (argc >= 1) ? (xsql_value_int(argv[0]) != 0) : 1;
	c->serializable = on;
	xsql_result_int(ctx, on);
}
/* xstore_gc(): vacuum dead versions up to the current GC horizon (the
 * oldest live snapshot, or the clock if none).  Returns the number of
 * versions reclaimed.  The horizon advances g_gc_horizon so an as_of
 * read below it is known to be best-effort. */
static void
fn_gc(xsql_context *ctx, int argc, xsql_value **argv)
{
	xstore_ctx_t *c = (xstore_ctx_t *)xsql_user_data(ctx);
	uint64_t now = atomic_load_explicit(&g_xclock, memory_order_relaxed);
	uint64_t horizon = snap_horizon(now);
	int reclaimed = 0;
	(void)argc; (void)argv;
	(void)xs_gc(c->bt, horizon, &reclaimed);
	if (horizon > atomic_load_explicit(&g_gc_horizon, memory_order_relaxed))
		atomic_store_explicit(&g_gc_horizon, horizon, memory_order_relaxed);
	xsql_result_int(ctx, reclaimed);
}

/* xstore_autovacuum(on): when on (default off), each write prunes that
 * rowid's dead versions inline (HOT-style), keeping version chains
 * short on a write-heavy workload without a full-tree scan.  Off keeps
 * all versions, so xstore_as_of() time travel can reach any historical
 * snapshot; on bounds time travel to the live-snapshot horizon. */
static void
fn_autovacuum(xsql_context *ctx, int argc, xsql_value **argv)
{
	xstore_ctx_t *c = (xstore_ctx_t *)xsql_user_data(ctx);
	int on = (argc >= 1) ? (xsql_value_int(argv[0]) != 0) : 1;
	c->autovacuum = on;
	xsql_result_int(ctx, on);
}
/* xstore_prune_count(): number of inline-prune passes adaptive
 * autovacuum has actually run (vs the number of writes) -- lets a test
 * see the backoff on a low-garbage workload. */
static void
fn_prune_count(xsql_context *ctx, int argc, xsql_value **argv)
{
	(void)argc; (void)argv;
	xsql_result_int64(ctx,
	    (xsql_int64)atomic_load_explicit(&g_av_prunes, memory_order_relaxed));
}

static const xsql_module xstore_module = {
	.iVersion    = 1,
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
	.xBegin      = xs_begin,
	.xSync       = xs_sync,
	.xCommit     = xs_commit,
	.xRollback   = xs_rollback,
};

static void
ctx_free(void *p)
{
	xstore_ctx_t *cx = p;
	if (cx != NULL) { ssi_abort(cx->ssi); snap_set(&cx->snap_slot, 0);
	    wbuf_clear(cx); free(cx->wbuf); free(cx->rset); }
	xsql_free(p);
}

int
xstore_register(xsql *db, bt_t *bt)
{
	xstore_ctx_t *ctx = xsql_malloc(sizeof *ctx);
	int rc;
	if (ctx == NULL)
		return SQLITE_NOMEM;
	ctx->bt = bt;
	atomic_store(&ctx->read_snap, 0);
	ctx->in_txn = 0;
	ctx->txn_snap = 0;
	ctx->wbuf = NULL;
	ctx->wn = ctx->wcap = 0;
	ctx->serializable = 0;
	ctx->rset = NULL;
	ctx->rn = ctx->rcap = 0;
	ctx->did_scan = 0;
	ctx->ssi = NULL;
	ctx->snap_slot = -1;
	ctx->autovacuum = 0;
	rc = xsql_create_module_v2(db, "xstore", &xstore_module, ctx, ctx_free);
	if (rc != SQLITE_OK)
		return rc;
	(void)xsql_create_function(db, "xstore_now", 0,
	    SQLITE_UTF8 | SQLITE_DETERMINISTIC, NULL, fn_now, NULL, NULL);
	(void)xsql_create_function(db, "xstore_as_of", 1, SQLITE_UTF8,
	    ctx, fn_as_of, NULL, NULL);
	(void)xsql_create_function(db, "xstore_serializable", 1, SQLITE_UTF8,
	    ctx, fn_serializable, NULL, NULL);
	(void)xsql_create_function(db, "xstore_gc", 0, SQLITE_UTF8,
	    ctx, fn_gc, NULL, NULL);
	(void)xsql_create_function(db, "xstore_autovacuum", 1, SQLITE_UTF8,
	    ctx, fn_autovacuum, NULL, NULL);
	(void)xsql_create_function(db, "xstore_prune_count", 0, SQLITE_UTF8,
	    ctx, fn_prune_count, NULL, NULL);
	return SQLITE_OK;
}

/*
 * Recovery: replay every committed transaction's versions from the log
 * into the B-tree.  Redo only -- there is no uncommitted data on disk
 * (the commit logs durably before touching the B-tree), and replay is
 * idempotent because version keys (rowid, ~commit_ts) are immutable, so
 * re-inserting one yields the identical entry.  Advances the commit
 * clock past the highest recovered timestamp so new commits do not
 * collide with recovered ones.
 */
struct xs_recover {
	bt_t    *bt;
	uint64_t max_ts;
	uint64_t records;
};
static int
xs_recover_cb(uint64_t lsn, const void *rec, uint32_t len, void *user)
{
	struct xs_recover *r = user;
	const uint8_t *p = rec;
	const uint8_t *end = (const uint8_t *)rec + len;
	uint64_t ts;
	uint32_t n, i;
	(void)lsn;

	if (len < 12)
		return 0;                     /* malformed: skip */
	memcpy(&ts, p, 8); p += 8;
	memcpy(&n, p, 4); p += 4;
	for (i = 0; i < n; i++) {
		int64_t rowid;
		uint8_t flags;
		uint16_t vlen;
		uint8_t key[XS_VKLEN];
		uint8_t buf[1 + XS_VMAX];

		if (p + 11 > end) break;      /* torn record */
		memcpy(&rowid, p, 8); p += 8;
		flags = *p++;
		memcpy(&vlen, p, 2); p += 2;
		if (vlen > XS_VMAX || p + vlen > end) break;
		enc_vkey(rowid, ts, key);
		buf[0] = flags;
		if (vlen) memcpy(buf + 1, p, vlen);
		p += vlen;
		(void)bt_insert(r->bt, key, XS_VKLEN, buf, (uint16_t)(1 + vlen));
	}
	if (ts > r->max_ts) r->max_ts = ts;
	r->records++;
	return 0;
}

int
xstore_recover(bt_t *bt, const char *wal_path)
{
	struct xs_recover r = { bt, 0, 0 };
	int rc = wal_scan(wal_path, xs_recover_cb, &r);
	if (r.max_ts >= atomic_load_explicit(&g_xclock, memory_order_relaxed))
		atomic_store_explicit(&g_xclock, r.max_ts + 1, memory_order_relaxed);
	return rc;
}
