/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/engine.c
 *	The single boundary between sqlxtc and the embedded SQL engine.
 *	This is the only application file that names the vendored
 *	engine's C API; the rest of the server speaks sx_ (engine.h).
 *	Replacing the backend with a from-scratch xtc-native engine is a
 *	rewrite of this file alone.
 */

#include "engine.h"
#include "xstore.h"
#include "btree.h"
#include "bufmgr.h"
#include "wal.h"

#ifdef SQLXTC_HAVE_LIME
#include "vexec.h"        /* vx_run -- the vectorized-executor fast path */
#include "sql_parse.h"    /* sql_parse_ast -- classify statements natively */
#include "sql_ast.h"
#endif
#include "xtc_async.h"     /* xtc_yield -- the fiber-yielding busy handler */
#include "xtc_proc.h"      /* xtc_proc_sleep -- park, do not spin */
#include "xtc_loop.h"      /* xtc_loop_t -- background storage procs */

#include <assert.h>
#include <stdatomic.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <unistd.h>      /* access, unlink, F_OK */

/*
 * sx_stmt -- the statement wrapper.  native == 0 wraps a VDBE statement
 * and every accessor below forwards to SQLite exactly as the engine
 * always has (the default, byte-for-byte unchanged).  native == 1 holds
 * a plan the native driver executes WITHOUT the VDBE: vexec for a
 * SELECT, the native write path for DML, the xstore native-txn API for
 * BEGIN/COMMIT/ROLLBACK/SAVEPOINT.  The driver is opt-in per process
 * (g_native_driver); until it is enabled sx_stmt.native is always 0.
 */
enum sx_native_kind {
	SXN_NONE = 0, SXN_SELECT, SXN_WRITE, SXN_BEGIN, SXN_COMMIT,
	SXN_ROLLBACK, SXN_SAVEPOINT, SXN_RELEASE, SXN_ROLLBACK_TO,
	SXN_CREATE, SXN_DROP, SXN_PRAGMA_NOP, SXN_PRAGMA_TABLE_INFO,
	SXN_CREATE_VIEW, SXN_DROP_VIEW, SXN_ALTER,
	SXN_CREATE_INDEX, SXN_DROP_INDEX, SXN_SET_ISO
};

struct sx_stmt {
	int            native;     /* always 1 (native-only); kept for the
	                            * public sx_stmt_is_native accessor */
	sx_db         *db;
#ifdef SQLXTC_HAVE_LIME
	enum sx_native_kind nkind;
	char          *sql;        /* statement text (write / re-runnable) */
	int            splevel;    /* SAVEPOINT/RELEASE/ROLLBACK TO level */
	char          *ddl_name;   /* CREATE/DROP table name */
	char          *ddl_cols;   /* CREATE coldefs (comma-joined) */
	vx_cell_t      binds[32];
	int            nbind;
	vx_result_t   *vres;       /* SELECT: materialized result */
	vx_pcache_t   *pcache;     /* SELECT / write: parse + plan, kept across
	                            * sx_reset (built on first execution) */
	int            cur;        /* current row in vres (-1 before first) */
	int            ran;        /* 1 once executed (one-shot for DML/txn) */
	int64_t        nchanges;
#endif
};

/* Native-driver enable.  ON by default: the engine runs every
 * recognized statement (the whole tested corpus -- reads, writes,
 * transactions, CREATE/DROP TABLE, value PRAGMA) through the native
 * sx_stmt driver with NO VDBE program, byte-identical to the VDBE.
 * SQLXTC_NATIVE_DRIVER=0 forces the VDBE (the differential oracle). */
static int g_native_driver = 1;

void sx_native_driver(int on) { g_native_driver = on ? 1 : 0; }
int  sx_native_driver_enabled(void) { return g_native_driver; }
/* The connection is now always SQLite-free native; sx_native_conn is a
 * retained no-op for API compatibility (the legacy handle is gone). */
void sx_native_conn(int on) { (void)on; }

#ifdef SQLXTC_HAVE_LIME
/*
 * A NATIVE connection handle.  When the native driver is on and the
 * libxtc storage engine is open, sx_open returns one of these instead
 * of a SQLite (xsql) connection: the engine then runs the entire
 * workload (the whole server test surface runs with zero VDBE
 * fallback -- verified) on xstore + vexec with NO sqlite3 connection
 * object at all.  It is identified by a magic first word, which the
 * handle-level entry points (sx_close / sx_errmsg / sx_changes /
 * sx_exec) check to route native vs SQLite.  The pointer's identity is
 * its key in xstore's g_dbmap (bt + ctx), exactly as a SQLite handle
 * was used -- xstore/vexec never dereference the connection.
 */
#define SX_NATIVE_MAGIC 0x73786E63u   /* "sxnc" */
struct sx_native_db {
	uint32_t   magic;          /* SX_NATIVE_MAGIC */
	char      *errmsg;         /* last error text (owned), or NULL */
	int64_t    changes;        /* rows changed by the last write */
	/* Named-savepoint stack: SQL savepoints nest by name, the storage
	 * engine tracks them by integer level (0-based depth).  spname[i] is
	 * the name of the savepoint opened at level i; spn is the count. */
	char       spname[32][64];
	int        spn;
	/* A statement failed on a write conflict inside a transaction: the
	 * engine rolled the transaction back at once (releasing its row
	 * write intents to the transactions waiting on them), but the client
	 * still believes it is open.  As in PostgreSQL every statement fails
	 * until the client ends the block: ROLLBACK succeeds, COMMIT fails
	 * (nothing was committed).  Without this a client that went on would
	 * run the rest of its "transaction" as autocommit statements. */
	int        aborted;
};

static int
sx_is_native_db(const sx_db *h)
{
	return h != NULL &&
	    ((const struct sx_native_db *)h)->magic == SX_NATIVE_MAGIC;
}

/* Is the connection's transaction block aborted (see sx_native_db)? */
static int
nat_aborted(const sx_db *h)
{
	return sx_is_native_db(h) && ((const struct sx_native_db *)h)->aborted;
}
#else
static int sx_is_native_db(const sx_db *h) { (void)h; return 0; }
#endif

/* Is this a native (VDBE-free) prepared statement? */
int  sx_stmt_is_native(const sx_stmt *st) { return st != NULL && st->native; }

/* Is `st` a native CREATE/DROP TABLE?  The server's live path keeps DDL
 * on the VDBE (its CREATE TABLE -> CREATE VIRTUAL TABLE rewrite + the
 * vtab schema must stay consistent), so db_exec_cached declines a
 * native DDL plan to the VDBE; native DDL is exercised by the dedicated
 * sx_prepare-direct path (test_native_driver). */
int  sx_stmt_is_ddl(const sx_stmt *st)
{
#ifdef SQLXTC_HAVE_LIME
	return st != NULL && st->native &&
	    (st->nkind == SXN_CREATE || st->nkind == SXN_DROP);
#else
	(void)st; return 0;
#endif
}

/* Native DML change count (0 for a SELECT / txn / DDL). */
int64_t sx_stmt_changes(const sx_stmt *st)
{
#ifdef SQLXTC_HAVE_LIME
	return (st != NULL && st->native) ? st->nchanges : 0;
#else
	(void)st; return 0;
#endif
}

int
sx_init(void)
{
	const char *nd = getenv("SQLXTC_NATIVE_DRIVER");
	if (nd != NULL && nd[0] == '0')
		g_native_driver = 0;
	return SX_OK;
}

int
sx_shutdown(void)
{
	return SX_OK;
}

int
sx_config_mutex(const void *methods)
{
	(void)methods;   /* native engine: no SQLite mutex subsystem */
	return SX_OK;
}

int
sx_config_mem(const void *methods)
{
	(void)methods;   /* native engine: no SQLite allocator subsystem */
	return SX_OK;
}

int
sx_config_serialized(void)
{
	return SX_OK;
}

/*
 * MULTITHREAD mode: the engine may be used by multiple threads, but a
 * single connection handle is never used by two threads at once.  This
 * is the connection-per-proc model -- each connection owns its handle
 * and runs on one loop thread -- and avoids the per-call mutexing that
 * SERIALIZED imposes.
 */
int
sx_config_multithread(void)
{
	return SX_OK;   /* native engine: no SQLite threading-mode subsystem */
}

/* ---- engine-native storage (xstore) lifecycle ---- */
static bm_t *g_xbm;
static bt_t *g_xbt;
static wal_t *g_xwal;
static char   g_xwal_path[1024];
static int    g_xrunning;        /* background procs spawned */

/*
 * Periodic checkpoint.  The log is compacted (xstore_checkpoint_wal: a
 * CHECKPOINT record plus the live rows replace the whole log) whenever
 * it grows past max(g_ckpt_min, 2 x its size after the last checkpoint)
 * -- the doubling keeps the O(live data) dump amortized against the
 * writes that triggered it.  The checkpoint is online
 * (xstore_checkpoint_wal_online): commits are held back only to take
 * its start point and to swap the files, not while the dump runs (the
 * frames committed meanwhile are copied after it).  Recovery replays the
 * compacted log, so its work is O(live data + writes since the last
 * checkpoint).  A failed checkpoint leaves the old (complete) log in
 * place and is retried.
 */
static uint64_t     g_ckpt_min = 64ull << 20;  /* bytes; sx_storage_set_checkpoint */
static _Atomic int  g_ckpt_stop, g_ckpt_alive;
static xtc_pid_t    g_ckpt_pid;
static _Atomic uint64_t g_ckpt_count;

void
sx_storage_set_checkpoint(uint64_t min_bytes)
{
	g_ckpt_min = min_bytes > 0 ? min_bytes : (64ull << 20);
}

uint64_t
sx_storage_checkpoints(void)
{
	return atomic_load(&g_ckpt_count);
}

static void
sx_ckpt_proc(void *arg)
{
	uint64_t floor = 0;
	(void)arg;
	while (!atomic_load(&g_ckpt_stop)) {
		uint64_t sz, trig;
		void *m = NULL;
		size_t mn = 0;
		/* Tick every 50 ms; sx_ckpt_stop's message ends the wait early. */
		if (xtc_recv(&m, &mn, 50LL * 1000 * 1000) == XTC_OK)
			xtc_free(m);
		if (atomic_load(&g_ckpt_stop))
			break;
		if (g_xwal == NULL || wal_failed(g_xwal))
			continue;
		sz = wal_size(g_xwal);
		trig = floor * 2 > g_ckpt_min ? floor * 2 : g_ckpt_min;
		if (sz < trig)
			continue;
		/* Online: commits keep running during the dump (they used to
		 * stop for all of it: 0.5-3.5 s at TPC-C W=1, ~16 s at W=10). */
		if (xstore_checkpoint_wal_online(g_xbt, (struct wal *)g_xwal,
		    g_xwal_path, NULL) == XTC_OK) {
			floor = wal_size(g_xwal);
			atomic_fetch_add(&g_ckpt_count, 1);
		}
	}
	atomic_store(&g_ckpt_alive, 0);
}

/* Stop the checkpoint proc and, on a proc, wait for it to finish any
 * checkpoint in progress.  Off a proc there is no wait, as for the
 * other storage procs: the caller is past xtc_exec_run / xtc_loop_run,
 * so the proc's loop no longer runs (waiting there cost 20 s per
 * shutdown) and the proc cannot touch the engine again. */
static void
sx_ckpt_stop(void)
{
	int i;
	if (!atomic_load(&g_ckpt_alive))
		return;
	atomic_store(&g_ckpt_stop, 1);
	xstore_checkpoint_cancel(1);            /* an online one in progress: abort */
	(void)xtc_send(g_ckpt_pid, "s", 1);     /* end its tick wait now */
	if (xtc_pid_is_none(xtc_self()))
		return;
	for (i = 0; i < 2000 && atomic_load(&g_ckpt_alive); i++)
		(void)xtc_proc_sleep(1LL * 1000 * 1000);
}

int
sx_open(const char *path, sx_db **out)
{
	int memlike = (path == NULL || path[0] == '\0' ||
	    strcmp(path, ":memory:") == 0 ||
	    strstr(path, "mode=memory") != NULL);   /* shared-cache :memory: URI */
	int rc;
	struct sx_native_db *nd;

	/*
	 * The connection is always a SQLite-free native handle over the
	 * global storage engine: the entire workload runs on xstore + vexec
	 * with no SQLite object.  (A connection opened before the storage
	 * engine is up has no B-tree to bind -- an error.)
	 */
	if (g_xbt == NULL) { *out = NULL; return SX_ERROR; }
	nd = (struct sx_native_db *)calloc(1, sizeof *nd);
	if (nd == NULL) { *out = NULL; return SX_NOMEM; }
	nd->magic = SX_NATIVE_MAGIC;
	nd->errmsg = NULL;
	nd->changes = 0;
	(void)memlike;
	rc = xstore_register_native((struct xsql *)nd, g_xbt);
	if (rc != SX_OK) { free(nd); *out = NULL; return rc; }
	*out = (sx_db *)nd;
	return SX_OK;
}

#ifdef SQLXTC_HAVE_LIME
/*
 * Open a SQLite-free native connection over a caller-provided B-tree
 * (not the global storage engine).  Used by the storage-engine tests,
 * which create their own bm/bt and want to drive it through SQL with no
 * SQLite object.  The handle is a tagged native handle registered to
 * `bt`; close it with sx_close.
 */
int
sx_open_bt(bt_t *bt, sx_db **out)
{
	struct sx_native_db *nd;
	int rc;
	*out = NULL;
	if (bt == NULL) return SX_MISUSE;
	nd = (struct sx_native_db *)calloc(1, sizeof *nd);
	if (nd == NULL) return SX_NOMEM;
	nd->magic = SX_NATIVE_MAGIC;
	nd->errmsg = NULL;
	nd->changes = 0;
	nd->spn = 0;
	rc = xstore_register_native((struct xsql *)nd, bt);
	if (rc != SX_OK) { free(nd); return rc; }
	*out = (sx_db *)nd;
	return SX_OK;
}
#endif

/* Buffer-manager write-ahead hook: ensure the log is durable through a
 * page's LSN before the page is written. */
static int
sx_wal_flush_cb(void *ctx, uint64_t lsn)
{
	return wal_flush_through((wal_t *)ctx, lsn);
}

/*
 * Open (or reopen) the libxtc-native storage engine.  Two paths:
 *
 *   Clean restart (fast): if the base was cleanly shut down (its
 *   superblock clean flag is set), trust it -- reopen the page file in
 *   place, restore the commit clock from the superblock, and skip
 *   recovery entirely.  This is the ARIES clean-restart case (no
 *   losers, nothing to redo).  The flag is cleared and fsync'd before
 *   any work, so a crash from here on falls back to a rebuild.  The
 *   secondary indexes are trusted too when the close recorded them
 *   complete (SX_SUP_IDX), and the log is reopened at the end the close
 *   recorded (SX_SUP_TAIL_*) instead of being scanned.
 *
 *   Crash recovery (rebuild): otherwise the base may be structurally
 *   torn by partial mid-SMO eviction, so it is discarded and the tree
 *   is rebuilt by replaying the (bounded) log onto a fresh page file --
 *   the redo-only path proven by test_torn_smo.  An open then
 *   re-checkpoints so the working page file plus the log are fresh.
 *
 * No loop is required; call sx_storage_run once the loop is up.
 */
static int g_storage_direct = 0;
static int g_storage_adaptive = 0;

/* Superblock words (bt_set_ext) a clean close records and the next open
 * reads: the index trust tag (xstore_index_trust_tag) and where the log
 * ends (a wal_tail_t; TAIL_END 0 with TAIL_LSN 0 = none recorded).  A
 * store from before them reads zeros: rebuild the indexes, scan the log. */
#define SX_SUP_IDX       0
#define SX_SUP_TAIL_END  1
#define SX_SUP_TAIL_LAST 2
#define SX_SUP_TAIL_LSN  3
#define SX_SUP_TAIL_OK   4   /* 1: the three words above are a tail */
#define SX_SUP_IDX_GARB  5   /* stale index entries left (xstore_index_close) */

/* The log's last LSN once the open finished: the log is then a compacted
 * dump of the base (the last clean close's, or the open's own after a
 * recovery), so a close that finds no frame appended since can keep it. */
static uint64_t g_open_lsn;

/* Configure direct I/O + adaptive writeback for the next sx_storage_open. */
/* CLOCK sweep bound for the next sx_storage_open (bm_opts_t
 * claim_threshold).  The bufmgr's default is 0 (unbounded); the server
 * uses 128, the value bufmgr.h documents for opt-in.  TPC-C at 10
 * warehouses, 8 connections, 65536 frames: 0 -> 11-14k tpmC with the
 * CLOCK hand circling a hot pool (evict_one 78% of the server), 128 ->
 * 64k; elsewhere within +-3% (1, 32 connections; 262144 frames). */
static uint32_t g_storage_claim = 128;
void
sx_storage_set_claim(uint32_t threshold)
{
	g_storage_claim = threshold;
}

void
sx_storage_set_io(int direct, int adaptive)
{
	g_storage_direct = direct ? 1 : 0;
	g_storage_adaptive = adaptive ? 1 : 0;
}

int
sx_storage_open(const char *path, unsigned int n_frames)
{
	bm_opts_t o = BM_OPTS_DEFAULT;
	wal_opts_t wo;
	wal_tail_t tail;
	const char *dpath = (path && path[0]) ? path : "sqlxtc.xdb";
	uint64_t clean = 0, clock = 0, idxtag = 0, idxgarb = 0;
	int trusted = 0, i;

	if (g_xbt != NULL)
		return SX_OK;                 /* already open */
	/* Prune each written row's dead versions on the write (HOT-style,
	 * adaptive: it backs off when there is nothing to prune).  Without it
	 * every UPDATE kept its predecessor until a full xstore_gc, and a
	 * ledger store of 100k accounts after 2M transfers was 1.17 GB
	 * against 693 MB with it -- and faster with it (smaller tree, fewer
	 * evictions).  Time travel (xstore_as_of) then reaches only
	 * snapshots someone pins, as in PostgreSQL. */
	xstore_autovacuum_default(1);

	o.path = dpath;
	if (n_frames > 0)
		o.n_frames = n_frames;
	/* No double-write ring.  The page file is trusted only after a CLEAN
	 * close (every page written and fdatasynced before the superblock
	 * says so, below and in sx_storage_close), and the flag is cleared
	 * durably before the first change; after any crash the file is
	 * truncated and rebuilt from the log.  A torn page can therefore
	 * never be read, and the ring -- an image plus an fdatasync before
	 * every page write -- cost ~1,000 fdatasyncs/s under TPC-C for no
	 * protection (test_torn_base).  The ring stays in bufmgr for the
	 * in-place (ARIES) recovery path, which does trust the base. */
	o.double_write = 0;
	o.lsn_off = 0;        /* ARIES page LSN: first field of every btnode */
	o.direct = g_storage_direct;             /* cache-bypass page store */
	o.adaptive_writeback = g_storage_adaptive;  /* GA-tuned trickler */
	o.claim_threshold = g_storage_claim;       /* sweep bound (0 = off) */

	snprintf(g_xwal_path, sizeof g_xwal_path, "%s-wal", dpath);

	/* Try to trust a cleanly-shut-down base. */
	o.reopen = 1;
	if (bm_create(&o, &g_xbm) == XTC_OK) {
		if (bt_reopen(g_xbm, &g_xbt) == XTC_OK) {
			bt_get_meta(g_xbt, &clean, &clock);
			if (clean == 1)
				trusted = 1;
			else { bt_close(g_xbt); g_xbt = NULL; }  /* torn base */
		}
		if (!trusted) { bm_destroy(g_xbm); g_xbm = NULL; }
	}

	memset(&tail, 0, sizeof tail);
	if (trusted) {
		idxtag = bt_get_ext(g_xbt, SX_SUP_IDX);
		idxgarb = bt_get_ext(g_xbt, SX_SUP_IDX_GARB);
		if (bt_get_ext(g_xbt, SX_SUP_TAIL_OK) == 1) {
			tail.valid = 1;
			tail.end = bt_get_ext(g_xbt, SX_SUP_TAIL_END);
			tail.last_off = bt_get_ext(g_xbt, SX_SUP_TAIL_LAST);
			tail.max_lsn = bt_get_ext(g_xbt, SX_SUP_TAIL_LSN);
		}
		/* Cleared with the clean flag: a crash from here on must not
		 * trust the indexes or the old tail. */
		for (i = 0; i < BT_SUPER_EXT; i++)
			bt_set_ext(g_xbt, i, 0);
		xstore_set_clock(clock);          /* restore the commit clock */
		bt_set_meta(g_xbt, 0, clock);     /* mark dirty: a crash now rebuilds */
		/* The cleared flag MUST be durable before any change: otherwise
		 * a crash leaves a "clean" base that misses later commits and
		 * the next open would skip the log replay that holds them. */
		if (bt_write_super(g_xbt) != XTC_OK || bm_sync(g_xbm) != XTC_OK) {
			bt_close(g_xbt); g_xbt = NULL;
			bm_destroy(g_xbm); g_xbm = NULL;
			return SX_ERROR;
		}
	} else {
		/* Fresh page file; rebuild the tree from the (bounded) log. */
		o.reopen = 0;
		if (bm_create(&o, &g_xbm) != XTC_OK)
			return SX_ERROR;
		if (bt_open(g_xbm, &g_xbt) != XTC_OK) {
			bm_destroy(g_xbm); g_xbm = NULL;
			return SX_ERROR;
		}
		/*
		 * Replay.  The page file is fresh and no open trusts it until a
		 * clean shutdown has flushed it and set the clean flag, so a
		 * crash during or after replay finds the flag clear and rebuilds
		 * again from the log.  Replay only reads the log (undo appends
		 * CLRs, which replay idempotently), so recovery can be repeated.
		 * The checkpoint below makes the log compact and durable before
		 * any new commit.
		 */
		(void)xstore_recover_ex(g_xbt, g_xwal_path, &tail);  /* replay */
	}

	/* Open the log (append past whatever is there). */
	memset(&wo, 0, sizeof wo);
	wo.path = g_xwal_path;
	wo.window_ns = 500LL * 1000;       /* 0.5ms group-commit window */
	wo.max_batch = 256;
	wo.append = 1;
	wo.tail = &tail;                   /* verified in O(1); else a scan */
	if (wal_open(&wo, &g_xwal) != XTC_OK) {
		bt_close(g_xbt); g_xbt = NULL;
		bm_destroy(g_xbm); g_xbm = NULL;
		return SX_ERROR;
	}
	xstore_set_wal(g_xwal);
	/* Write-ahead enforcement: before the buffer manager writes a dirty
	 * page it flushes the log through that page's LSN via this hook. */
	bm_set_wal_flush(g_xbm, sx_wal_flush_cb, g_xwal);
	/* On a rebuild, compact the log so this run starts bounded; on a
	 * trusted reopen the log is already the compacted log from the clean
	 * shutdown, so leave it. */
	if (!trusted)
		(void)xstore_checkpoint_wal(g_xbt, (struct wal *)g_xwal, g_xwal_path);
	g_open_lsn = wal_failed(g_xwal) ? 0 : wal_durable_lsn(g_xwal);
	/* Secondary indexes are DERIVED data not carried by the WAL body.  A
	 * clean close that left them complete (the trust tag) lets the
	 * trusted base keep them; otherwise every index is rebuilt from the
	 * base rows (on a log rebuild the page file is fresh: nothing to
	 * clear). */
	/* A rebuild runs one index per thread; each pins a descent path of
	 * frames at a time, so a small pool gets one thread. */
	i = xtc_ncpus();
	if (i > 16) i = 16;
	if ((unsigned)i > o.n_frames / 1024u) i = (int)(o.n_frames / 1024u);
	(void)xstore_index_open(g_xbt, trusted ? idxtag : 0, idxgarb, !trusted,
	    i);
	return SX_OK;
}

/*
 * Start the background storage procs on `loop`: the WAL group-commit
 * writer (one fsync for many commits), the page provider (refills the
 * free/cool list ahead of demand), and the trickler (writes dirty
 * pages out ahead of eviction).  Idempotent; safe to skip if storage
 * is not open.
 */
int
sx_storage_run(xtc_loop_t *loop)
{
	if (g_xbt == NULL || g_xrunning || loop == NULL)
		return SX_OK;
	if (g_xwal != NULL)
		(void)wal_writer_spawn(g_xwal, loop, NULL);
	if (g_xwal != NULL) {
		xtc_proc_opts_t co = { 0 };
		xtc_pid_t cp;
		co.name = "checkpoint";
		atomic_store(&g_ckpt_stop, 0);
		xstore_checkpoint_cancel(0);
		atomic_store(&g_ckpt_alive, 1);
		if (xtc_proc_spawn(loop, sx_ckpt_proc, NULL, &co, &cp) != XTC_OK)
			atomic_store(&g_ckpt_alive, 0);
		else
			g_ckpt_pid = cp;
	}
	(void)bm_provider_spawn(g_xbm, loop, 1LL * 1000 * 1000, NULL);  /* 1ms */
	(void)bm_trickler_spawn(g_xbm, loop, 5LL * 1000 * 1000, NULL);  /* 5ms */
	g_xrunning = 1;
	return SX_OK;
}

/*
 * Checkpoint: bound the log.  Flushes dirty pages (memory relief) and
 * then COMPACTS THE LOG IN PLACE via xstore_checkpoint_wal -- a
 * CHECKPOINT record plus a dump of the live tree atomically replace the
 * log, so the write history before it is discarded and replay stays
 * proportional to the live data.  Call when commits are quiesced (the
 * compaction rewrites and rebinds the log file).
 */
int
sx_storage_checkpoint(void)
{
	int rc = SX_OK;
	if (g_xbm == NULL)
		return SX_OK;
	(void)bm_checkpoint(g_xbm);            /* flush dirty pages (not for durability) */
	if (g_xwal != NULL) {
		xstore_quiesce_begin();
		if (xstore_checkpoint_wal(g_xbt, (struct wal *)g_xwal,
		    g_xwal_path) != XTC_OK)
			rc = SX_ERROR;
		else
			atomic_fetch_add(&g_ckpt_count, 1);
		xstore_quiesce_end();
	}
	return rc;
}

/*
 * Stop the background storage procs (checkpointer, WAL writer, page
 * provider, trickler) without tearing down the engine, so an executor
 * running them can go idle or be stopped -- after which the caller does
 * sx_storage_close off the loop.  Call from a proc while the loops still
 * run.  Commits already in flight finish (logged, acknowledged and
 * applied) before the writer stops; any later commit waits at the
 * checkpoint gate and is never acknowledged, so the store that
 * sx_storage_close flushes holds every acknowledged commit and no
 * half-applied one.  Idempotent.
 */
void
sx_storage_quiesce(void)
{
	if (!g_xrunning)
		return;
	sx_ckpt_stop();
	if (g_xwal != NULL)
		xstore_quiesce_begin();        /* closed until the next open */
	if (g_xwal != NULL) wal_writer_stop(g_xwal);
	bm_trickler_stop(g_xbm);
	bm_provider_stop(g_xbm);
	g_xrunning = 0;
}

int
sx_storage_close(void)
{
	wal_tail_t tail;
	uint64_t garb = 0;
	int ok = 1;

	if (g_xrunning) {
		sx_ckpt_stop();
		if (g_xwal != NULL) wal_writer_stop(g_xwal);
		bm_trickler_stop(g_xbm);
		bm_provider_stop(g_xbm);
		g_xrunning = 0;
	}
	/*
	 * Clean shutdown.  Flush the base durable while the log is still
	 * open (the write-ahead hook reads it), compact the log so a future
	 * crash replays a bounded log, then mark the base clean and fsync
	 * the marker so the next open can trust the base and skip recovery.
	 * The marker is written ONLY if every step succeeded: a page that
	 * did not reach the file, a failed sync, a failed compaction, or a
	 * log that fail-stopped earlier all leave the flag unset, so the next
	 * open rebuilds from the (complete) log -- always safe.  A crash
	 * anywhere before the marker is durable has the same effect.
	 */
	memset(&tail, 0, sizeof tail);
	if (g_xbt != NULL && g_xbm != NULL) {
		if (g_xwal != NULL && wal_failed(g_xwal))
			ok = 0;                    /* do not trust anything after an I/O error */
		/* Reap the queued superseded index entries before the flush, so
		 * the base the next open trusts carries no queued garbage. */
		if (ok)
			garb = xstore_index_close(g_xbt);
		if (ok && bm_checkpoint(g_xbm) != XTC_OK)   /* flush all dirty pages + fsync */
			ok = 0;
		/* Compact the log -- unless nothing was logged since the open,
		 * when it still is the compacted dump of this base: rewriting it
		 * reads the whole base (13 s on a cold TPC-C W=10 store).  Kept
		 * only if its end is known, so the next open need not scan it
		 * (an open that scanned the log does not know it). */
		if (ok && g_xwal != NULL &&
		    (g_open_lsn == 0 || wal_durable_lsn(g_xwal) != g_open_lsn ||
		    wal_get_tail(g_xwal, &tail) != XTC_OK) &&
		    xstore_checkpoint_wal(g_xbt, (struct wal *)g_xwal,
		    g_xwal_path) != XTC_OK)
			ok = 0;
		/* Where the compacted log ends, for the next open (no scan). */
		if (ok && g_xwal != NULL)
			(void)wal_get_tail(g_xwal, &tail);
		bm_set_wal_flush(g_xbm, NULL, NULL);   /* drop the hook before closing the log */
	}
	xstore_set_wal(NULL);
	if (g_xwal != NULL) { wal_close(g_xwal); g_xwal = NULL; }
	if (ok && g_xbt != NULL && g_xbm != NULL) {
		/* Written in the same superblock write as the clean flag, so
		 * they are trusted exactly when it is. */
		bt_set_ext(g_xbt, SX_SUP_IDX, xstore_index_trust_tag(g_xbt));
		bt_set_ext(g_xbt, SX_SUP_IDX_GARB, garb);
		bt_set_ext(g_xbt, SX_SUP_TAIL_OK, tail.valid ? 1 : 0);
		bt_set_ext(g_xbt, SX_SUP_TAIL_END, tail.valid ? tail.end : 0);
		bt_set_ext(g_xbt, SX_SUP_TAIL_LAST, tail.valid ? tail.last_off : 0);
		bt_set_ext(g_xbt, SX_SUP_TAIL_LSN, tail.valid ? tail.max_lsn : 0);
		bt_set_meta(g_xbt, 1, xstore_clock());  /* base is consistent up to here */
		if (bt_write_super(g_xbt) != XTC_OK || bm_sync(g_xbm) != XTC_OK)
			ok = 0;                    /* marker may be torn: next open rebuilds */
	}
	if (g_xbt != NULL) { bt_close(g_xbt); g_xbt = NULL; }
	if (g_xbm != NULL) { bm_destroy(g_xbm); g_xbm = NULL; }
	return ok ? SX_OK : SX_ERROR;
}

void
sx_storage_abandon(void)
{
	/* Crash simulation: stop the background procs, then drop all state
	 * WITHOUT a checkpoint or a clean marker.  Dirty pages are lost
	 * (bm_destroy does not flush); the log is closed but NOT truncated,
	 * so it keeps the full durable history.  The next open finds no
	 * marker and rebuilds the tree from the log. */
	if (g_xrunning) {
		sx_ckpt_stop();
		if (g_xwal != NULL) wal_writer_stop(g_xwal);
		bm_trickler_stop(g_xbm);
		bm_provider_stop(g_xbm);
		g_xrunning = 0;
	}
	xstore_set_wal(NULL);
	if (g_xwal != NULL) { wal_close(g_xwal); g_xwal = NULL; }
	if (g_xbt != NULL) { bt_close(g_xbt); g_xbt = NULL; }
	if (g_xbm != NULL) { bm_destroy(g_xbm); g_xbm = NULL; }
}

struct bt *
sx_storage_bt(void)
{
	return (struct bt *)g_xbt;
}

struct wal *
sx_storage_wal(void)
{
	return (struct wal *)g_xwal;
}

const char *
sx_storage_wal_path(void)
{
	return g_xwal != NULL ? g_xwal_path : NULL;
}

void
sx_storage_bm_stats(struct bm_stats *out)
{
	if (g_xbm != NULL)
		bm_get_stats(g_xbm, out);
	else
		memset(out, 0, sizeof *out);
}

void
sx_storage_wal_counts(uint64_t *commits, uint64_t *fsyncs)
{
	wal_stats_t ws;
	memset(&ws, 0, sizeof ws);
	if (g_xwal != NULL) wal_get_stats(g_xwal, &ws);
	*commits = ws.commits;
	*fsyncs = ws.batches;
}

int
sx_storage_active(void)
{
	return g_xbt != NULL;
}

void
sx_close(sx_db *h)
{
	struct sx_native_db *nd = (struct sx_native_db *)h;
	if (h == NULL) return;
	xstore_unregister_native((struct xsql *)h);   /* frees the ctx too */
	free(nd->errmsg);
	free(nd);
}

int
sx_exec(sx_db *h, const char *sql, char **errmsg)
{
	/* Native multi-statement exec: split the script on top-level ';'
	 * (respecting '...' string and x'...' blob literals), then
	 * prepare/step/finalize each statement through the native driver. */
	const char *p = sql;
	if (errmsg) *errmsg = NULL;
	while (p != NULL && *p != '\0') {
		const char *q = p;
			int in_str = 0;
			char *stmt; size_t n;
			sx_stmt *st = NULL;
			int rc, sc, blank = 1;
			/* Find the end of this statement (next top-level ';'). */
			for (; *q != '\0'; q++) {
				if (in_str) { if (*q == '\'') in_str = 0; continue; }
				if (*q == '\'') { in_str = 1; continue; }
				if (*q == ';') break;
			}
			n = (size_t)(q - p);
			/* Skip a blank/whitespace-only fragment. */
			{ size_t i; for (i = 0; i < n; i++)
				if (p[i] != ' ' && p[i] != '\t' && p[i] != '\n' &&
				    p[i] != '\r') { blank = 0; break; } }
			if (blank) { if (*q == ';') q++; p = q; continue; }
			stmt = (char *)malloc(n + 1);
			if (stmt == NULL) return SX_NOMEM;
			memcpy(stmt, p, n); stmt[n] = '\0';
			rc = sx_prepare(h, stmt, -1, &st, NULL);
			if (rc != SX_OK) { free(stmt); return rc; }
			sc = SX_OK;
			if (st != NULL) {
				(void)sx_column_count(st);   /* run a SELECT/table_info */
				while ((sc = sx_step(st)) == SX_ROW)
					;
				sx_finalize(st);
			}
			free(stmt);
			if (sc != SX_DONE && sc != SX_OK)
				return sc;
			if (*q == ';') q++;
			p = q;
		}
	return SX_OK;
}

#ifdef SQLXTC_HAVE_LIME
/*
 * Classify a single SQL statement for the native driver.  Returns the
 * native kind (SXN_*), or SXN_NONE if the statement is not one the
 * driver handles natively yet (PRAGMA, SAVEPOINT, multi-statement,
 * EXPLAIN, ATTACH, an INDEX/VIEW CREATE, or unparseable) -- the caller
 * then builds a VDBE wrapper.  *tail_more is set when more SQL follows.
 * For CREATE TABLE / DROP TABLE the table name (and, for CREATE, the
 * comma-joined coldefs with the PK column first) are written to the
 * caller's buffers so the AST can be freed here.
 */
static enum sx_native_kind
sx_classify(const char *sql, int *tail_more,
            char *namebuf, size_t namecap, char *colbuf, size_t colcap)
{
	sql_arena_t *ast = NULL;
	sql_stmt_t  *root = NULL;
	const char  *perr = NULL;
	enum sx_native_kind k = SXN_NONE;

	*tail_more = 0;
	if (namebuf) namebuf[0] = '\0';
	if (colbuf) colbuf[0] = '\0';
	if (sql_parse_ast(sql, strlen(sql), &ast, &root, &perr) != 0) {
		if (ast) sql_arena_destroy(ast);
		return SXN_NONE;
	}
	if (root == NULL) { if (ast) sql_arena_destroy(ast); return SXN_NONE; }
	if (root->next != NULL) { *tail_more = 1; sql_arena_destroy(ast); return SXN_NONE; }
	if (root->explain) { sql_arena_destroy(ast); return SXN_NONE; }
	switch (root->kind) {
	case SQL_KIND_SELECT:   k = SXN_SELECT;   break;
	case SQL_KIND_INSERT:
	case SQL_KIND_UPDATE:
	case SQL_KIND_DELETE:   k = SXN_WRITE;    break;
	case SQL_KIND_BEGIN:    k = SXN_BEGIN;    break;
	case SQL_KIND_COMMIT:   k = SXN_COMMIT;   break;
	case SQL_KIND_ROLLBACK:
		/* Bare ROLLBACK -> SXN_ROLLBACK; ROLLBACK TO <name> -> ROLLBACK_TO
		 * with the savepoint name captured. */
		if (root->u.txn != NULL && root->u.txn->name.len > 0 &&
		    (size_t)root->u.txn->name.len < namecap) {
			memcpy(namebuf, root->u.txn->name.p, root->u.txn->name.len);
			namebuf[root->u.txn->name.len] = '\0';
			k = SXN_ROLLBACK_TO;
		} else k = SXN_ROLLBACK;
		break;
	case SQL_KIND_SAVEPOINT:
		if (root->u.txn != NULL && root->u.txn->name.len > 0 &&
		    (size_t)root->u.txn->name.len < namecap) {
			memcpy(namebuf, root->u.txn->name.p, root->u.txn->name.len);
			namebuf[root->u.txn->name.len] = '\0';
			k = SXN_SAVEPOINT;
		} else k = SXN_NONE;
		break;
	case SQL_KIND_RELEASE:
		if (root->u.txn != NULL && root->u.txn->name.len > 0 &&
		    (size_t)root->u.txn->name.len < namecap) {
			memcpy(namebuf, root->u.txn->name.p, root->u.txn->name.len);
			namebuf[root->u.txn->name.len] = '\0';
			k = SXN_RELEASE;
		} else k = SXN_NONE;
		break;
	case SQL_KIND_CREATE: {
		/* Plain CREATE TABLE -> native xstore catalog (no vtab).
		 * CREATE VIEW -> the view registry (name in namebuf, the view's
		 * SELECT text in colbuf).  TABLE AS / INDEX decline.  Coldefs is
		 * "<name> <type>" per column, the INTEGER PRIMARY KEY column first
		 * (it is the rowid).  A table with no single INTEGER PRIMARY KEY
		 * column gets an implicit rowid: a synthetic hidden
		 * "__rowid__ INTEGER" column is prepended as the storage key, and
		 * all declared columns follow as payload (SQLite-equivalent
		 * implicit-rowid table). */
		const sql_create_t *c = root->u.create;
		const sql_coldef_t *col, *pk = NULL;
		size_t co = 0;
		int npk = 0, implicit, has_pkidx = 0;
		int ok;
		/* CREATE [UNIQUE] INDEX name ON table(c1 [ASC|DESC], ...).
		 * namebuf = index name; colbuf = "table\0c1,c2..\0unique\0
		 * descmask\0" (packed). */
		if (c != NULL && c->kind == SX_CR_INDEX && c->name.len > 0 &&
		    (size_t)c->name.len < namecap && c->on_table.len > 0 &&
		    c->index_cols != NULL && c->index_cols->head != NULL) {
			const sql_exprlist_item_t *ic;
			size_t o = 0;
			unsigned dm = 0;
			int ni = 0, w;
			ok = (size_t)c->on_table.len + 2 < colcap;
			if (ok) {
				memcpy(colbuf, c->on_table.p, c->on_table.len);
				o = c->on_table.len; colbuf[o++] = '\0';
			}
			for (ic = c->index_cols->head; ic && ok; ic = ic->next, ni++) {
				const sql_expr_t *ce = ic->expr;
				if (ni >= 16 || ce == NULL || ce->op != SX_E_COLUMN ||
				    ce->nname != 1 || ce->name[0].len == 0) { ok = 0; break; }
				if (ic->sort == 2) dm |= 1u << ni;
				w = snprintf(colbuf + o, colcap - o, "%s%.*s", ni ? "," : "",
				    (int)ce->name[0].len, ce->name[0].p);
				if (w < 0 || o + (size_t)w + 16 >= colcap) { ok = 0; break; }
				o += (size_t)w;
			}
			if (ok) {
				colbuf[o++] = '\0';
				w = snprintf(colbuf + o, colcap - o, "%c%c%u", c->unique ? '1' : '0',
				    '\0', dm);
				memcpy(namebuf, c->name.p, c->name.len);
				namebuf[c->name.len] = '\0';
				k = SXN_CREATE_INDEX;
				break;
			}
			k = SXN_NONE; break;   /* expression index etc.: decline */
		}
		if (c != NULL && c->kind == SX_CR_VIEW && c->select != NULL &&
		    c->name.len > 0 && (size_t)c->name.len < namecap &&
		    c->select->src != NULL) {
			const char *vs = c->select->src;
			size_t vlen = strlen(vs);   /* src runs to end-of-statement */
			while (vlen > 0 && (vs[vlen-1] == ';' || vs[vlen-1] == ' ' ||
			       vs[vlen-1] == '\n' || vs[vlen-1] == '\t')) vlen--;
			if (vlen > 0 && vlen < colcap) {
				memcpy(namebuf, c->name.p, c->name.len);
				namebuf[c->name.len] = '\0';
				memcpy(colbuf, vs, vlen);
				colbuf[vlen] = '\0';
				k = SXN_CREATE_VIEW;
				break;
			}
			k = SXN_NONE; break;
		}
		ok = (c != NULL && c->cols != NULL && c->select == NULL &&
		      c->name.len > 0 && (size_t)c->name.len < namecap);
		if (ok) {
			const sql_coldef_t *pkcons = NULL;
			memcpy(namebuf, c->name.p, c->name.len);
			namebuf[c->name.len] = '\0';
			for (col = c->cols; col; col = col->next) {
				if (col->cons == SX_CONS_PRIMARY) { npk++; pkcons = col; }
				else if (col->cons == SX_CONS_NONE && col->primary) { npk++; pk = col; }
			}
			if (npk > 1) ok = 0;     /* two PRIMARY KEY clauses: an error */
			/* PRIMARY KEY(x) over one column is the same as x PRIMARY KEY. */
			if (ok && pkcons != NULL && pkcons->cons_cols != NULL &&
			    pkcons->cons_cols->n == 1) {
				const sql_expr_t *pe = pkcons->cons_cols->head->expr;
				for (col = c->cols; col; col = col->next)
					if (col->cons == SX_CONS_NONE && pe != NULL &&
					    col->name.len == pe->name[0].len &&
					    memcmp(col->name.p, pe->name[0].p, col->name.len) == 0)
						pk = col;
			}
			/* SQLite: only a PK on one column declared exactly INTEGER is
			 * the rowid; any other PRIMARY KEY is a UNIQUE autoindex over
			 * an implicit rowid. */
			if (pk != NULL && !(pk->type.len == 7 &&
			    strncasecmp(pk->type.p, "INTEGER", 7) == 0))
				pk = NULL;
			if (pk == NULL && npk == 1) has_pkidx = 1;
		}
		implicit = ok && (pk == NULL);
		if (ok && implicit) {
			/* synthetic hidden rowid as storage-column 0 */
			int w = snprintf(colbuf, colcap, "__rowid__ INTEGER");
			if (w < 0 || (size_t)w >= colcap) ok = 0;
			else co = (size_t)w;
		}
		if (ok) {
			int phase, phases = implicit ? 1 : 2;
			/* explicit PK: PK column first (phase 0), then the rest.
			 * implicit: a single pass over all declared columns. */
			for (phase = 0; phase < phases && ok; phase++)
				for (col = c->cols; col && ok; col = col->next) {
					int w, is_pk = (!implicit && col == pk);
					if (col->cons != SX_CONS_NONE) continue;
					if (!implicit && (phase == 0 ? !is_pk : is_pk)) continue;
					/* The catalog's "name type,..." form cannot hold a
					 * name with a space or comma (a quoted "b c"). */
					if (col->name.len == 0 || memchr(col->name.p, ' ', col->name.len) ||
					    memchr(col->name.p, ',', col->name.len)) { ok = 0; break; }
					if (col->type.len > 0)
						w = snprintf(colbuf + co, colcap - co, "%s%.*s %.*s",
						    co ? "," : "", (int)col->name.len, col->name.p,
						    (int)col->type.len, col->type.p);
					else
						w = snprintf(colbuf + co, colcap - co, "%s%.*s",
						    co ? "," : "", (int)col->name.len, col->name.p);
					if (w < 0 || (size_t)(co + (size_t)w) >= colcap) { ok = 0; break; }
					co += (size_t)w;
				}
		}
		/* Constraint autoindexes, after the coldefs and a NUL:
		 * "p:c1,c2;u:c3;" -- one per non-rowid PRIMARY KEY / UNIQUE, in
		 * order of appearance (SQLite numbers sqlite_autoindex_<t>_<n>
		 * the same way). */
		if (ok) {
			size_t so = co + 1;
			if (so + 1 >= colcap) ok = 0;
			else colbuf[co] = '\0';
			for (col = c->cols; col && ok; col = col->next) {
				const sql_exprlist_item_t *ci;
				int w, isp, first = 1;
				isp = (col->cons == SX_CONS_PRIMARY ||
				    (col->cons == SX_CONS_NONE && col->primary));
				if (isp && !has_pkidx) continue;   /* the rowid alias */
				if (!isp && !(col->cons == SX_CONS_UNIQUE ||
				    (col->cons == SX_CONS_NONE && col->unique)))
					continue;
				w = snprintf(colbuf + so, colcap - so, "%c:", isp ? 'p' : 'u');
				if (w < 0 || so + (size_t)w >= colcap) { ok = 0; break; }
				so += (size_t)w;
				if (col->cons == SX_CONS_NONE) {
					w = snprintf(colbuf + so, colcap - so, "%.*s",
					    (int)col->name.len, col->name.p);
					if (w < 0 || so + (size_t)w >= colcap) { ok = 0; break; }
					so += (size_t)w;
				} else {
					for (ci = col->cons_cols ? col->cons_cols->head : NULL;
					    ci; ci = ci->next) {
						const sql_expr_t *ce = ci->expr;
						if (ce == NULL || ce->nname != 1) { ok = 0; break; }
						w = snprintf(colbuf + so, colcap - so, "%s%.*s",
						    first ? "" : ",", (int)ce->name[0].len, ce->name[0].p);
						if (w < 0 || so + (size_t)w >= colcap) { ok = 0; break; }
						so += (size_t)w;
						first = 0;
					}
				}
				if (ok && so + 2 < colcap) { colbuf[so++] = ';'; colbuf[so] = '\0'; }
				else ok = 0;
			}
			if (ok && so == co + 1) colbuf[so] = '\0';
		}
		k = ok ? SXN_CREATE : SXN_NONE;
		break;
	}
	case SQL_KIND_DROP: {
		const sql_drop_t *d = root->u.drop;
		if (d != NULL && d->kind == SX_DR_TABLE &&
		    d->name.len > 0 && (size_t)d->name.len < namecap) {
			memcpy(namebuf, d->name.p, d->name.len);
			namebuf[d->name.len] = '\0';
			k = SXN_DROP;
		} else if (d != NULL && d->kind == SX_DR_VIEW &&
		           d->name.len > 0 && (size_t)d->name.len < namecap) {
			memcpy(namebuf, d->name.p, d->name.len);
			namebuf[d->name.len] = '\0';
			k = SXN_DROP_VIEW;
		} else if (d != NULL && d->kind == SX_DR_INDEX &&
		           d->name.len > 0 && (size_t)d->name.len < namecap) {
			memcpy(namebuf, d->name.p, d->name.len);
			namebuf[d->name.len] = '\0';
			k = SXN_DROP_INDEX;
		} else k = SXN_NONE;
		break;
	}
	case SQL_KIND_ALTER: {
		/* ALTER TABLE old RENAME TO new: old in create->name, new in
		 * create->on_table.  Old -> namebuf, new -> colbuf. */
		const sql_create_t *c = root->u.create;
		if (c != NULL && c->name.len > 0 && (size_t)c->name.len < namecap &&
		    c->on_table.len > 0 && (size_t)c->on_table.len < colcap) {
			memcpy(namebuf, c->name.p, c->name.len);
			namebuf[c->name.len] = '\0';
			memcpy(colbuf, c->on_table.p, c->on_table.len);
			colbuf[c->on_table.len] = '\0';
			k = SXN_ALTER;
		} else k = SXN_NONE;
		break;
	}
	case SQL_KIND_PRAGMA: {
		/* A PRAGMA that SETS a value (journal_mode=WAL, synchronous=...,
		 * busy_timeout=...) is a no-op for the native engine -- it has no
		 * SQLite journal/cache to configure -- so classify it native and
		 * return no rows.  table_info(<t>) is served natively from the
		 * xstore catalog (its table name is captured into namebuf).  Any
		 * other bare read PRAGMA declines. */
		const sql_pragma_t *p = root->u.pragma;
		if (p != NULL && p->name.len == 10 &&
		    strncasecmp(p->name.p, "table_info", 10) == 0 &&
		    p->value != NULL) {
			const sql_expr_t *v = p->value;
			const char *tp = NULL; size_t tl = 0;
			if (v->op == SX_E_COLUMN && v->nname >= 1) {
				tp = v->name[0].p; tl = v->name[0].len;
			} else if (v->op == SX_E_STRING) {
				tp = v->lit.p; tl = v->lit.len;
			}
			if (tp != NULL && tl > 0 && tl < namecap) {
				memcpy(namebuf, tp, tl);
				namebuf[tl] = '\0';
				k = SXN_PRAGMA_TABLE_INFO;
			} else k = SXN_NONE;
		} else if (p != NULL && p->value != NULL) {
			k = SXN_PRAGMA_NOP;
		} else k = SXN_NONE;
		break;
	}
	default:                k = SXN_NONE;     break;   /* other */
	}
	sql_arena_destroy(ast);
	return k;
}
#endif /* SQLXTC_HAVE_LIME */

#ifdef SQLXTC_HAVE_LIME
/* Case-insensitive match of the next word in *pp against `w`; on a
 * match advance *pp past it (and following whitespace) and return 1. */
static int
sx_word(const char **pp, const char *w)
{
	const char *p = *pp;
	size_t n = strlen(w);
	if (strncasecmp(p, w, n) != 0) return 0;
	if (isalnum((unsigned char)p[n]) || p[n] == '_') return 0;
	p += n;
	while (isspace((unsigned char)*p)) p++;
	*pp = p;
	return 1;
}

/*
 * Isolation-level statements, recognized here rather than in the Lime
 * grammar (so the committed generated parser is unchanged):
 *   SET TRANSACTION ISOLATION LEVEL <lvl>              -> txn level
 *   SET SESSION CHARACTERISTICS AS TRANSACTION
 *       ISOLATION LEVEL <lvl>                          -> session level
 *   BEGIN [TRANSACTION] ISOLATION LEVEL <lvl>          -> BEGIN + txn level
 *   START TRANSACTION ISOLATION LEVEL <lvl>            -> BEGIN + txn level
 * <lvl> is SERIALIZABLE | REPEATABLE READ | READ COMMITTED |
 * READ UNCOMMITTED | SNAPSHOT.  Returns the kind (SXN_SET_ISO or
 * SXN_BEGIN) with the canonical level in lvl and *session set, or
 * SXN_NONE if the text is not one of these.
 */
static enum sx_native_kind
sx_classify_iso(const char *sql, char *lvl, size_t cap, int *session)
{
	const char *p = sql, *names[] = { "serializable", "repeatable read",
	    "read committed", "read uncommitted", "snapshot" };
	enum sx_native_kind k;
	size_t i;

	*session = 0;
	while (isspace((unsigned char)*p)) p++;
	if (sx_word(&p, "BEGIN")) {
		(void)sx_word(&p, "TRANSACTION");
		k = SXN_BEGIN;
	} else if (sx_word(&p, "START")) {      /* START TRANSACTION ISOLATION ... */
		if (!sx_word(&p, "TRANSACTION")) return SXN_NONE;
		k = SXN_BEGIN;
	} else if (sx_word(&p, "SET")) {
		if (sx_word(&p, "SESSION")) {
			if (!sx_word(&p, "CHARACTERISTICS") || !sx_word(&p, "AS"))
				return SXN_NONE;
			*session = 1;
		}
		if (!sx_word(&p, "TRANSACTION")) return SXN_NONE;
		k = SXN_SET_ISO;
	} else
		return SXN_NONE;
	if (!sx_word(&p, "ISOLATION") || !sx_word(&p, "LEVEL")) return SXN_NONE;
	for (i = 0; i < sizeof names / sizeof names[0]; i++) {
		const char *q = p, *w = names[i], *sp = strchr(w, ' ');
		char first[16];
		if (sp != NULL) {
			snprintf(first, sizeof first, "%.*s", (int)(sp - w), w);
			if (!sx_word(&q, first) || !sx_word(&q, sp + 1)) continue;
		} else if (!sx_word(&q, w))
			continue;
		while (*q == ';' || isspace((unsigned char)*q)) q++;
		if (*q != '\0') return SXN_NONE;
		snprintf(lvl, cap, "%s", w);
		return k;
	}
	return SXN_NONE;
}
#endif

int
sx_prepare(sx_db *h, const char *sql, int n_bytes, sx_stmt **out,
           const char **tail)
{
	struct sx_stmt *st;

	*out = NULL;
#ifdef SQLXTC_HAVE_LIME
	/*
	 * Native-driver path: when enabled, classify the statement.  A kind
	 * the driver handles natively (SELECT / DML / BEGIN / COMMIT /
	 * ROLLBACK, single-statement) becomes a native sx_stmt with NO VDBE
	 * prepare.  Anything else (DDL, PRAGMA, SAVEPOINT, multi-statement,
	 * unparseable) declines to the VDBE wrapper below -- during the
	 * transition the VDBE still covers what the driver does not.  (When
	 * the driver is complete this decline becomes an error instead.)
	 */
	if (g_native_driver && (n_bytes < 0 || (size_t)n_bytes == strlen(sql))) {
		int more = 0, session = 0;
		char nm[128], cols[1024];
		enum sx_native_kind k = sx_classify_iso(sql, nm, sizeof nm, &session);
		if (k == SXN_NONE)
			k = sx_classify(sql, &more, nm, sizeof nm, cols, sizeof cols);
		else
			cols[0] = session ? 's' : 't';    /* level scope */
		if (k != SXN_NONE) {
			st = (struct sx_stmt *)calloc(1, sizeof *st);
			if (st == NULL) return SX_NOMEM;
			st->native = 1;
			st->db = h;
			st->nkind = k;
			st->cur = -1;
			st->sql = strdup(sql);
			if (st->sql == NULL) { free(st); return SX_NOMEM; }
			if (k == SXN_CREATE || k == SXN_DROP) {
				st->ddl_name = strdup(nm);
				if (st->ddl_name == NULL) { free(st->sql); free(st); return SX_NOMEM; }
				if (k == SXN_CREATE) {
					/* "coldefs\0p:c1,c2;u:c3;\0": copy both parts. */
					size_t cl = strlen(cols);
					size_t xl = strlen(cols + cl + 1);
					st->ddl_cols = (char *)malloc(cl + xl + 2);
					if (st->ddl_cols == NULL) {
						free(st->ddl_name); free(st->sql); free(st); return SX_NOMEM; }
					memcpy(st->ddl_cols, cols, cl + xl + 2);
				}
			} else if (k == SXN_CREATE_VIEW) {
				st->ddl_name = strdup(nm);
				if (st->ddl_name == NULL) { free(st->sql); free(st); return SX_NOMEM; }
				st->ddl_cols = strdup(cols);   /* the view's SELECT text */
				if (st->ddl_cols == NULL) {
					free(st->ddl_name); free(st->sql); free(st); return SX_NOMEM; }
			} else if (k == SXN_ALTER) {
				st->ddl_name = strdup(nm);     /* old table name */
				if (st->ddl_name == NULL) { free(st->sql); free(st); return SX_NOMEM; }
				st->ddl_cols = strdup(cols);   /* new table name */
				if (st->ddl_cols == NULL) {
					free(st->ddl_name); free(st->sql); free(st); return SX_NOMEM; }
			} else if (k == SXN_CREATE_INDEX) {
				st->ddl_name = strdup(nm);     /* index name */
				if (st->ddl_name == NULL) { free(st->sql); free(st); return SX_NOMEM; }
				/* cols is "table\0c1,c2\0u\0descmask\0"; strdup stops at
				 * the first NUL, so copy the whole packed buffer. */
				{
					size_t tl = strlen(cols);
					size_t cl = strlen(cols + tl + 1);
					size_t ul = strlen(cols + tl + cl + 2);
					size_t dl = strlen(cols + tl + cl + ul + 3);
					size_t total = tl + cl + ul + dl + 4;
					st->ddl_cols = (char *)malloc(total);
					if (st->ddl_cols == NULL) {
						free(st->ddl_name); free(st->sql); free(st); return SX_NOMEM; }
					memcpy(st->ddl_cols, cols, total);
				}
			} else if (k == SXN_SET_ISO || (k == SXN_BEGIN && nm[0] != '\0')) {
				st->ddl_name = strdup(nm);     /* the isolation level */
				if (st->ddl_name == NULL) { free(st->sql); free(st); return SX_NOMEM; }
				st->splevel = (cols[0] == 's');   /* 1 = session scope */
			} else if (k == SXN_DROP_INDEX) {
				st->ddl_name = strdup(nm);     /* index name to drop */
				if (st->ddl_name == NULL) { free(st->sql); free(st); return SX_NOMEM; }
			} else if (k == SXN_DROP_VIEW || k == SXN_PRAGMA_TABLE_INFO) {
				st->ddl_name = strdup(nm);   /* view to drop / table to introspect */
				if (st->ddl_name == NULL) { free(st->sql); free(st); return SX_NOMEM; }
			} else if (k == SXN_SAVEPOINT || k == SXN_RELEASE ||
			           k == SXN_ROLLBACK_TO) {
				st->ddl_name = strdup(nm);   /* the savepoint name */
				if (st->ddl_name == NULL) { free(st->sql); free(st); return SX_NOMEM; }
			}
			if (tail) *tail = sql + strlen(sql);   /* single statement */
			*out = st;
			return SX_OK;
		}
		/* SXN_NONE: fall through to the hard-error path below. */
	}
	/*
	 * A fully native connection has no SQLite object: an unrecognized
	 * statement is a hard error (no VDBE fallback).
	 */
	{
		struct sx_native_db *nd = (struct sx_native_db *)h;
		free(nd->errmsg);
		nd->errmsg = strdup("unsupported SQL (native engine)");
		return SX_ERROR;
	}
#else
	(void)h; (void)sql; (void)n_bytes; (void)tail; (void)st;
	return SX_ERROR;
#endif
}

/* Run a native SELECT through vexec into st->vres, once (idempotent).
 * Returns 1 on success (vres ready), 0 on error.  Called from sx_step
 * on the first step AND from sx_column_count / sx_column_name, since the
 * caller (exec_stmt) reads the column count BEFORE the first step. */
#ifdef SQLXTC_HAVE_LIME
static int
nat_select_run(struct sx_stmt *st)
{
	char *verr = NULL;
	int rc;
	if (st->ran) return st->vres != NULL;
	if (nat_aborted(st->db)) return 0;   /* sx_step reports it */
	if (st->nkind == SXN_PRAGMA_TABLE_INFO) {
		rc = vx_pragma_table_info((struct xsql *)st->db, st->ddl_name, &st->vres);
		st->ran = 1;
		return (rc == 1 && st->vres != NULL);
	}
	if (st->pcache == NULL) st->pcache = vx_pcache_new(st->sql);
	/* One pinned snapshot for the whole SELECT (the result is
	 * materialized here): without it an autocommit SELECT read each row
	 * at the then-latest clock, unpinned, so GC could reclaim the version
	 * it was about to read. */
	xstore_stmt_begin((struct xsql *)st->db);
	if (st->pcache != NULL)
		rc = vx_pcache_run(st->pcache, (struct xsql *)st->db,
		    st->nbind ? st->binds : NULL, st->nbind, &st->vres, &verr);
	else
		rc = vx_run_p((struct xsql *)st->db, st->sql,
		    st->nbind ? st->binds : NULL, st->nbind, 1, &st->vres, &verr);
	xstore_stmt_end((struct xsql *)st->db);
	st->ran = 1;
	if (verr) free(verr);
	return (rc == 1 && st->vres != NULL);
}

/* The native engine has no VDBE fallback: a runtime decline is a hard
 * error (nat_set_run_error reports it). */
#endif /* SQLXTC_HAVE_LIME */

#ifdef SQLXTC_HAVE_LIME
/* Set a descriptive error on a native connection when the native engine
 * declines a statement at run time.  For a SELECT/DML over a table that
 * is not in the catalog, produce SQLite's "no such table: NAME"; for
 * any other decline, a generic "unsupported SQL".  This keeps the error
 * surface compatible with SQLite for the common case (a typo'd table).
 */
/* Message for a write that the log refused (see wal_commit fail-stop). */
#define SX_MSG_WAL_IO "I/O error: write-ahead log write or fsync failed; " \
	"the transaction was not committed and the database is read-only " \
	"until restart"
#define SX_MSG_SERIALIZE "could not serialize access due to concurrent " \
	"update; transaction rolled back"
#define SX_MSG_ABORTED "current transaction is aborted, commands ignored " \
	"until end of transaction block"

static int
nat_set_err(sx_db *db, const char *msg, int rc)
{
	struct sx_native_db *nd;
	if (!sx_is_native_db(db)) return rc;
	nd = (struct sx_native_db *)db;
	free(nd->errmsg);
	nd->errmsg = strdup(msg);
	return rc;
}

static int
nat_set_run_error(struct sx_stmt *st)
{
	struct sx_native_db *nd;
	char buf[160];
	const char *tab = NULL;
	if (!sx_is_native_db(st->db)) return SX_ERROR;
	if (xstore_wal_failed())
		return nat_set_err(st->db, SX_MSG_WAL_IO, SX_ERROR);
	nd = (struct sx_native_db *)st->db;
	free(nd->errmsg);
	/* A first unresolved base table name -> "no such table: NAME". */
	tab = vx_unknown_table((struct xsql *)st->db, st->sql);
	if (tab != NULL) {
		snprintf(buf, sizeof buf, "no such table: %s", tab);
		nd->errmsg = strdup(buf);
	} else {
		nd->errmsg = strdup("unsupported SQL (native engine)");
	}
	return SX_ERROR;
}
#endif

#ifdef SQLXTC_HAVE_LIME
/* CREATE INDEX (or a constraint autoindex): `csv` is the comma-joined
 * column list.  A UNIQUE index over duplicate rows fails with SQLite's
 * message. */
static int
nat_create_index(struct sx_stmt *st, const char *name, const char *table,
    const char *csv, int unique, uint32_t descmask, int kind)
{
	char buf[1100], *p, *cols[16];
	int n = 0, rc;
	if (strlen(csv) >= sizeof buf) return SX_ERROR;
	snprintf(buf, sizeof buf, "%s", csv);
	for (p = buf; *p && n < 16; ) {
		cols[n++] = p;
		while (*p && *p != ',') p++;
		if (*p == ',') *p++ = '\0';
	}
	rc = xstore_create_index_ex(st->db, name, table, (const char *const *)cols,
	    n, descmask, unique, kind);
	if (rc == 1) return SX_DONE;
	if (rc < 0) {
		char m[400];
		size_t off;
		int i;
		off = (size_t)snprintf(m, sizeof m, "UNIQUE constraint failed: ");
		for (i = 0; i < n && off < sizeof m; i++)
			off += (size_t)snprintf(m + off, sizeof m - off, "%s%s.%s",
			    i ? ", " : "", table, cols[i]);
		return nat_set_err(st->db, m, SX_CONSTRAINT);
	}
	return SX_ERROR;
}

/* CREATE TABLE: the table, then one UNIQUE autoindex per non-rowid
 * PRIMARY KEY / UNIQUE constraint ("p:c1,c2;u:c3;" after the coldefs),
 * named sqlite_autoindex_<t>_<n> as SQLite does. */
static int
nat_create_table(struct sx_stmt *st)
{
	const char *ix = st->ddl_cols + strlen(st->ddl_cols) + 1;
	char name[128], csv[1100];
	int n = 0, rc;
	if (xstore_create_table(st->db, st->ddl_name, st->ddl_cols) == 0)
		return SX_ERROR;
	while (*ix) {
		char kind = ix[0];
		const char *e = strchr(ix, ';');
		size_t cl;
		if (e == NULL || ix[1] != ':') return SX_ERROR;
		cl = (size_t)(e - (ix + 2));
		if (cl >= sizeof csv) return SX_ERROR;
		memcpy(csv, ix + 2, cl);
		csv[cl] = '\0';
		snprintf(name, sizeof name, "sqlite_autoindex_%s_%d", st->ddl_name, ++n);
		rc = nat_create_index(st, name, st->ddl_name, csv, 1, 0, kind);
		if (rc != SX_DONE) return rc;
		ix = e + 1;
	}
	return SX_DONE;
}
#endif

int
sx_step(sx_stmt *st)
{
	if (st == NULL) return SX_MISUSE;
#ifdef SQLXTC_HAVE_LIME
	if (st->native) {
		if (nat_aborted(st->db)) {
			struct sx_native_db *nd = (struct sx_native_db *)st->db;
			if (st->nkind == SXN_ROLLBACK) {
				nd->aborted = 0;   /* already rolled back */
				return SX_DONE;
			}
			if (st->nkind == SXN_COMMIT) {
				nd->aborted = 0;
				return nat_set_err(st->db, SX_MSG_SERIALIZE, SX_BUSY);
			}
			return nat_set_err(st->db, SX_MSG_ABORTED, SX_ERROR);
		}
		if (st->nkind == SXN_SELECT || st->nkind == SXN_PRAGMA_TABLE_INFO) {
			/* First step: run the SELECT (or native PRAGMA table_info)
			 * through vexec into vres; a decline is a hard error. */
			if (!st->ran && !nat_select_run(st))
				return nat_set_run_error(st);
			if (st->vres == NULL)
				return nat_set_run_error(st);
			st->cur++;
			return (st->cur < vx_result_nrow(st->vres)) ? SX_ROW
			                                             : SX_DONE;
		}
		if (st->ran) return SX_DONE;   /* one-shot kinds */
		st->ran = 1;
		switch (st->nkind) {
		case SXN_WRITE: {
			int64_t nch = 0;
			int rc, mark, rd, restarts = 0;
			char *verr = NULL;
			const char *w = st->sql;
			while (isspace((unsigned char)*w)) w++;
			/* UPDATE/DELETE compute rows from a read: give the statement
			 * one snapshot and a write-write check against it.  An INSERT
			 * reads only "the key is absent", which the insert path checks
			 * atomically with the write (xstore_insert_txn). */
			rd = sx_word(&w, "UPDATE") || sx_word(&w, "DELETE");
			/* In a transaction a failed statement undoes only its own
			 * writes; the transaction stays open (SQLite statement
			 * atomicity), so a harness can ROLLBACK or carry on. */
			mark = xstore_stmt_mark(st->db);
		restart:
			if (rd) xstore_stmt_begin(st->db);
			else xstore_stmt_pin(st->db);   /* INSERT ... SELECT reads */
			if (st->pcache == NULL) st->pcache = vx_pcache_new(st->sql);
			if (st->pcache != NULL)
				rc = vx_pcache_write(st->pcache, (struct xsql *)st->db,
				    st->nbind ? st->binds : NULL, st->nbind, &nch, &verr);
			else
				rc = vx_run_write_p((struct xsql *)st->db, st->sql,
				    st->nbind ? st->binds : NULL, st->nbind, &nch, &verr);
			xstore_stmt_end(st->db);
			if (rc != 1) xstore_stmt_undo(st->db, mark);
			if (rc == 1) { free(verr); st->nchanges = nch; return SX_DONE; }
			/* A statement whose row was committed by the writer it
			 * waited for re-runs on a new snapshot where the client
			 * cannot tell (READ COMMITTED, autocommit, a transaction's
			 * first access: see xstore_stmt_restart) instead of failing.
			 * Bounded: a hot row under constant churn still fails, with
			 * the error below. */
			if (rd && restarts < 8 && xstore_stmt_restart(st->db)) {
				free(verr); verr = NULL; nch = 0;
				restarts++;
				goto restart;
			}
			if (xstore_last_conflict(st->db)) {
				struct sx_native_db *nd = sx_is_native_db(st->db)
				    ? (struct sx_native_db *)st->db : NULL;
				free(verr);
				/* As the message says (and PostgreSQL does): the
				 * transaction is rolled back now, releasing its row
				 * write intents to the transactions waiting on them;
				 * the block stays aborted until ROLLBACK / COMMIT. */
				if (xstore_in_txn(st->db)) {
					(void)xstore_rollback(st->db);
					if (nd) { nd->spn = 0; nd->aborted = 1; }
				}
				return nat_set_err(st->db, SX_MSG_SERIALIZE, SX_BUSY);
			}
			if (verr != NULL && strncmp(verr, "UNIQUE", 6) == 0) {
				rc = nat_set_err(st->db, verr, SX_CONSTRAINT);
				free(verr);
				return rc;
			}
			free(verr);
			/* The native write path declined: a hard error (no VDBE). */
			return nat_set_run_error(st);
		}
		case SXN_BEGIN: {
			struct sx_native_db *nd = sx_is_native_db(st->db)
			    ? (struct sx_native_db *)st->db : NULL;
			if (nd) nd->spn = 0;
			if (xstore_native_begin(st->db) != 0) return SX_ERROR;
			if (st->ddl_name != NULL &&     /* BEGIN ISOLATION LEVEL x */
			    xstore_set_txn_isolation(st->db, st->ddl_name) != 0)
				return nat_set_err(st->db,
				    "cannot set the isolation level", SX_ERROR);
			return SX_DONE;
		}
		case SXN_SET_ISO: {
			int rc;
			if (st->splevel)
				rc = xstore_set_isolation(st->db, st->ddl_name);
			else
				rc = xstore_set_txn_isolation(st->db, st->ddl_name);
			if (rc == 0) return SX_DONE;
			return nat_set_err(st->db, rc == -2
			    ? "SET TRANSACTION must be inside a transaction (BEGIN first)"
			    : rc == -3
			    ? "SET TRANSACTION must precede any query in the transaction"
			    : "cannot set the isolation level", SX_ERROR);
		}
		case SXN_COMMIT: {
			struct sx_native_db *nd = sx_is_native_db(st->db)
			    ? (struct sx_native_db *)st->db : NULL;
			int rc;
			if (nd) nd->spn = 0;
			rc = xstore_commit(st->db);
			if (rc == 0) return SX_DONE;
			if (rc == SX_BUSY)      /* serialization conflict */
				return nat_set_err(st->db, SX_MSG_SERIALIZE, SX_BUSY);
			if (rc == SX_CONSTRAINT)   /* a concurrent commit took the key */
				return nat_set_err(st->db, xstore_uniq_err(st->db),
				    SX_CONSTRAINT);
			return nat_set_err(st->db, xstore_wal_failed() ? SX_MSG_WAL_IO
			    : "commit failed; transaction rolled back", SX_ERROR);
		}
		case SXN_ROLLBACK: {
			struct sx_native_db *nd = sx_is_native_db(st->db)
			    ? (struct sx_native_db *)st->db : NULL;
			if (nd) nd->spn = 0;
			return xstore_rollback(st->db) == 0 ? SX_DONE : SX_ERROR;
		}
		case SXN_SAVEPOINT: {
			/* Push a named savepoint at the next level. */
			struct sx_native_db *nd = sx_is_native_db(st->db)
			    ? (struct sx_native_db *)st->db : NULL;
			int lvl;
			if (nd == NULL || nd->spn >= 32) return SX_ERROR;
			lvl = nd->spn;
			if (xstore_savepoint(st->db, lvl) != 0) return SX_ERROR;
			snprintf(nd->spname[lvl], sizeof nd->spname[0], "%s",
			    st->ddl_name ? st->ddl_name : "");
			nd->spn = lvl + 1;
			return SX_DONE;
		}
		case SXN_RELEASE: {
			/* RELEASE name: find the innermost savepoint with that name,
			 * release it and everything nested inside it (pop to it). */
			struct sx_native_db *nd = sx_is_native_db(st->db)
			    ? (struct sx_native_db *)st->db : NULL;
			int i, lvl = -1;
			if (nd == NULL) return SX_ERROR;
			for (i = nd->spn - 1; i >= 0; i--)
				if (st->ddl_name && strcmp(nd->spname[i], st->ddl_name) == 0) { lvl = i; break; }
			if (lvl < 0) return SX_ERROR;   /* no such savepoint */
			if (xstore_release(st->db, lvl) != 0) return SX_ERROR;
			nd->spn = lvl;
			return SX_DONE;
		}
		case SXN_ROLLBACK_TO: {
			/* ROLLBACK TO name: undo to the savepoint (it stays open);
			 * any savepoints nested inside it are discarded. */
			struct sx_native_db *nd = sx_is_native_db(st->db)
			    ? (struct sx_native_db *)st->db : NULL;
			int i, lvl = -1;
			if (nd == NULL) return SX_ERROR;
			for (i = nd->spn - 1; i >= 0; i--)
				if (st->ddl_name && strcmp(nd->spname[i], st->ddl_name) == 0) { lvl = i; break; }
			if (lvl < 0) return SX_ERROR;   /* no such savepoint */
			if (xstore_rollback_to(st->db, lvl) != 0) return SX_ERROR;
			nd->spn = lvl + 1;   /* the named savepoint remains open */
			return SX_DONE;
		}
		case SXN_CREATE:
			return nat_create_table(st);
		case SXN_DROP:
			return xstore_drop_table(st->db, st->ddl_name) == 0
			    ? SX_DONE : SX_ERROR;
		case SXN_CREATE_VIEW:
			return xstore_create_view(st->db, st->ddl_name, st->ddl_cols) != 0
			    ? SX_DONE : SX_ERROR;
		case SXN_DROP_VIEW:
			return xstore_drop_view(st->db, st->ddl_name) != 0
			    ? SX_DONE : SX_ERROR;
		case SXN_ALTER:
			return xstore_rename_table(st->db, st->ddl_name, st->ddl_cols) == 0
			    ? SX_DONE : SX_ERROR;
		case SXN_CREATE_INDEX: {
			/* ddl_cols packs "table\0c1,c2\0unique\0descmask". */
			const char *table = st->ddl_cols;
			const char *cl = table + strlen(table) + 1;
			const char *uq = cl + strlen(cl) + 1;
			const char *dm = uq + strlen(uq) + 1;
			return nat_create_index(st, st->ddl_name, table, cl, uq[0] == '1',
			    (uint32_t)strtoul(dm, NULL, 10), 'i');
		}
		case SXN_DROP_INDEX:
			/* Idempotent: dropping an unknown index is not an error. */
			if (xstore_drop_index(st->db, st->ddl_name) < 0)
				return nat_set_err(st->db, "index associated with UNIQUE or "
				    "PRIMARY KEY constraint cannot be dropped", SX_ERROR);
			return SX_DONE;
		case SXN_PRAGMA_NOP:
			return SX_DONE;   /* value-setting PRAGMA: no-op, no rows */
		default:
			return SX_ERROR;
		}
	}
#endif
	return SX_MISUSE;
}
int
sx_reset(sx_stmt *st)
{
	if (st == NULL) return SX_OK;
#ifdef SQLXTC_HAVE_LIME
	if (st->native) {
		if (st->vres) { vx_result_free(st->vres); st->vres = NULL; }
		st->cur = -1; st->ran = 0;
		return SX_OK;
	}
#endif
	return SX_OK;
}
int
sx_clear_bindings(sx_stmt *st)
{
	if (st == NULL) return SX_OK;
#ifdef SQLXTC_HAVE_LIME
	if (st->native) {
		int i;
		for (i = 0; i < st->nbind; i++)
			if ((st->binds[i].type == VX_TEXT || st->binds[i].type == VX_BLOB) &&
			    st->binds[i].bytes != NULL) {
				free((void *)st->binds[i].bytes);
				st->binds[i].bytes = NULL;
			}
		st->nbind = 0;
		return SX_OK;
	}
#endif
	return SX_OK;
}
void
sx_finalize(sx_stmt *st)
{
	if (st == NULL) return;
#ifdef SQLXTC_HAVE_LIME
	/* Free the native-plan resources whether or not the stmt fell back
	 * to the VDBE (nat_fallback_to_vdbe sets native=0 but the sql / bind
	 * buffers / ddl strings were allocated in native mode). */
	{
		int i;
		if (st->vres) vx_result_free(st->vres);
		vx_pcache_free(st->pcache);
		for (i = 0; i < st->nbind; i++)
			if ((st->binds[i].type == VX_TEXT || st->binds[i].type == VX_BLOB) &&
			    st->binds[i].bytes != NULL)
				free((void *)st->binds[i].bytes);
		free(st->sql);
		free(st->ddl_name);
		free(st->ddl_cols);
	}
#endif
	free(st);
}

int
sx_bind_count(sx_stmt *st)
{
#ifdef SQLXTC_HAVE_LIME
	if (st->native) return st->nbind;   /* binds tracked as they are set */
#endif
	return 0;
}
int
sx_bind_int64(sx_stmt *st, int idx, int64_t v)
{
#ifdef SQLXTC_HAVE_LIME
	if (st->native) {
		if (idx < 1 || idx > 32) return SX_ERROR;
		st->binds[idx-1].type = VX_INT; st->binds[idx-1].i = v;
		if (idx > st->nbind) st->nbind = idx;
		return SX_OK;
	}
#endif
	return SX_MISUSE;
}
int
sx_bind_double(sx_stmt *st, int idx, double v)
{
#ifdef SQLXTC_HAVE_LIME
	if (st->native) {
		if (idx < 1 || idx > 32) return SX_ERROR;
		st->binds[idx-1].type = VX_REAL; st->binds[idx-1].r = v;
		if (idx > st->nbind) st->nbind = idx;
		return SX_OK;
	}
#endif
	return SX_MISUSE;
}
int
sx_bind_text(sx_stmt *st, int idx, const char *s, int n)
{
#ifdef SQLXTC_HAVE_LIME
	if (st->native) {
		uint8_t *p;
		if (idx < 1 || idx > 32) return SX_ERROR;
		if (n < 0) n = s ? (int)strlen(s) : 0;
		p = (uint8_t *)malloc((size_t)n + 1);
		if (p == NULL) return SX_NOMEM;
		if (n && s) memcpy(p, s, (size_t)n);
		p[n] = '\0';
		/* The bind buffer is owned by the cell and freed on finalize/
		 * reset/clear (see bind cleanup). */
		st->binds[idx-1].type = VX_TEXT; st->binds[idx-1].bytes = p;
		st->binds[idx-1].nbytes = (uint32_t)n;
		if (idx > st->nbind) st->nbind = idx;
		return SX_OK;
	}
#endif
	return SX_MISUSE;
}
int
sx_bind_blob(sx_stmt *st, int idx, const void *p, int n)
{
#ifdef SQLXTC_HAVE_LIME
	if (st->native) {
		uint8_t *b;
		if (idx < 1 || idx > 32) return SX_ERROR;
		if (n < 0) n = 0;
		b = (uint8_t *)malloc((size_t)n + 1);
		if (b == NULL) return SX_NOMEM;
		if (n && p) memcpy(b, p, (size_t)n);
		b[n] = '\0';
		st->binds[idx-1].type = VX_BLOB; st->binds[idx-1].bytes = b;
		st->binds[idx-1].nbytes = (uint32_t)n;
		if (idx > st->nbind) st->nbind = idx;
		return SX_OK;
	}
#endif
	return SX_MISUSE;
}
int
sx_bind_null(sx_stmt *st, int idx)
{
#ifdef SQLXTC_HAVE_LIME
	if (st->native) {
		if (idx < 1 || idx > 32) return SX_ERROR;
		st->binds[idx-1].type = VX_NULL; st->binds[idx-1].nbytes = 0;
		if (idx > st->nbind) st->nbind = idx;
		return SX_OK;
	}
#endif
	return SX_MISUSE;
}

int
sx_column_count(sx_stmt *st)
{
#ifdef SQLXTC_HAVE_LIME
	if (st->native) {
		if (st->nkind != SXN_SELECT && st->nkind != SXN_PRAGMA_TABLE_INFO) return 0;
		if (!st->ran && !nat_select_run(st)) return 0;
		if (st->vres == NULL) return 0;
		return vx_result_ncol(st->vres);
	}
#endif
	return 0;
}
const char *
sx_column_name(sx_stmt *st, int i)
{
#ifdef SQLXTC_HAVE_LIME
	if (st->native) {
		if (st->nkind != SXN_SELECT && st->nkind != SXN_PRAGMA_TABLE_INFO) return NULL;
		if (!st->ran) (void)nat_select_run(st);
		if (st->vres == NULL) return NULL;
		return vx_result_name(st->vres, i);
	}
#endif
	return NULL;
}
int
sx_column_type(sx_stmt *st, int i)
{
#ifdef SQLXTC_HAVE_LIME
	if (st->native) {
		if ((st->nkind != SXN_SELECT && st->nkind != SXN_PRAGMA_TABLE_INFO) || st->vres == NULL || st->cur < 0) return SX_NULL;
		switch (vx_result_type(st->vres, st->cur, i)) {
		case VX_INT:  return SX_INTEGER;
		case VX_REAL: return SX_FLOAT;
		case VX_TEXT: return SX_TEXT;
		case VX_BLOB: return SX_BLOB;
		default:      return SX_NULL;
		}
	}
#endif
	return SX_NULL;
}
int64_t
sx_column_int64(sx_stmt *st, int i)
{
#ifdef SQLXTC_HAVE_LIME
	if (st->native)
		return (st->vres && st->cur >= 0) ? vx_result_int64(st->vres, st->cur, i) : 0;
#endif
	return 0;
}
double
sx_column_double(sx_stmt *st, int i)
{
#ifdef SQLXTC_HAVE_LIME
	if (st->native)
		return (st->vres && st->cur >= 0) ? vx_result_double(st->vres, st->cur, i) : 0.0;
#endif
	return 0.0;
}
const char *
sx_column_text(sx_stmt *st, int i)
{
#ifdef SQLXTC_HAVE_LIME
	if (st->native)
		return (st->vres && st->cur >= 0) ? vx_result_text(st->vres, st->cur, i) : NULL;
#endif
	return NULL;
}
const void *
sx_column_blob(sx_stmt *st, int i)
{
#ifdef SQLXTC_HAVE_LIME
	if (st->native)
		return (st->vres && st->cur >= 0) ? (const void *)vx_result_text(st->vres, st->cur, i) : NULL;
#endif
	return NULL;
}
int
sx_column_bytes(sx_stmt *st, int i)
{
#ifdef SQLXTC_HAVE_LIME
	if (st->native)
		return (st->vres && st->cur >= 0) ? vx_result_bytes(st->vres, st->cur, i) : 0;
#endif
	return 0;
}

const char *
sx_errmsg(sx_db *h)
{
#ifdef SQLXTC_HAVE_LIME
	if (sx_is_native_db(h)) {
		const struct sx_native_db *nd = (const struct sx_native_db *)h;
		return nd->errmsg != NULL ? nd->errmsg : "";
	}
#endif
	return "";
}
int64_t
sx_changes(sx_db *h)
{
#ifdef SQLXTC_HAVE_LIME
	if (sx_is_native_db(h)) {
		const struct sx_native_db *nd = (const struct sx_native_db *)h;
		return nd->changes;
	}
#endif
	return 0;
}

/* ---- vectorized-executor seam (vexec) ---------------------------- */

#ifdef SQLXTC_HAVE_LIME

int
sx_vexec_try(sx_db *h, const char *sql, int n_workers, sx_vx_result **out)
{
	vx_result_t *r = NULL;
	char *verr = NULL;
	int rc;

	if (out == NULL) return XTC_E_INVAL;
	*out = NULL;
	xstore_stmt_begin((struct xsql *)h);     /* one pinned snapshot */
	rc = vx_run((struct xsql *)h, sql, n_workers, &r, &verr);
	xstore_stmt_end((struct xsql *)h);
	if (verr) free(verr);          /* the caller falls back; no message needed */
	if (rc == 1) { *out = (sx_vx_result *)r; return 1; }
	if (r) vx_result_free(r);
	return rc;                     /* 0 fallback / <0 error */
}

int
sx_vexec_try_p(sx_db *h, const char *sql, const void *binds, int nbinds,
               int n_workers, sx_vx_result **out)
{
	vx_result_t *r = NULL;
	char *verr = NULL;
	int rc;

	if (out == NULL) return XTC_E_INVAL;
	*out = NULL;
	xstore_stmt_begin((struct xsql *)h);     /* one pinned snapshot */
	rc = vx_run_p((struct xsql *)h, sql, (const vx_cell_t *)binds, nbinds,
	              n_workers, &r, &verr);
	xstore_stmt_end((struct xsql *)h);
	if (verr) free(verr);
	if (rc == 1) { *out = (sx_vx_result *)r; return 1; }
	if (r) vx_result_free(r);
	return rc;
}

void
sx_vexec_free(sx_vx_result *r)
{
	vx_result_free((vx_result_t *)r);
}

int sx_vexec_nrow(const sx_vx_result *r) { return vx_result_nrow((const vx_result_t *)r); }
int sx_vexec_ncol(const sx_vx_result *r) { return vx_result_ncol((const vx_result_t *)r); }

int
sx_vexec_type(const sx_vx_result *r, int row, int col)
{
	switch (vx_result_type((const vx_result_t *)r, row, col)) {
	case VX_INT:  return SX_INTEGER;
	case VX_REAL: return SX_FLOAT;
	case VX_TEXT: return SX_TEXT;
	case VX_BLOB: return SX_BLOB;
	case VX_NULL:
	default:      return SX_NULL;
	}
}

int64_t sx_vexec_int64(const sx_vx_result *r, int row, int col)
{ return vx_result_int64((const vx_result_t *)r, row, col); }
double sx_vexec_double(const sx_vx_result *r, int row, int col)
{ return vx_result_double((const vx_result_t *)r, row, col); }
const char *sx_vexec_text(const sx_vx_result *r, int row, int col)
{ return vx_result_text((const vx_result_t *)r, row, col); }
const void *sx_vexec_blob(const sx_vx_result *r, int row, int col)
{ return vx_result_text((const vx_result_t *)r, row, col); }
int sx_vexec_bytes(const sx_vx_result *r, int row, int col)
{ return vx_result_bytes((const vx_result_t *)r, row, col); }
const char *sx_vexec_name(const sx_vx_result *r, int col)
{ return vx_result_name((const vx_result_t *)r, col); }

int
sx_vexec_write(sx_db *h, const char *sql, int64_t *nchanges)
{
	char *verr = NULL;
	int rc;
	xstore_stmt_pin((struct xsql *)h);
	rc = vx_run_write((struct xsql *)h, sql, nchanges, &verr);
	xstore_stmt_end((struct xsql *)h);
	if (verr) free(verr);          /* caller falls back; no message surfaced */
	return rc;
}

int
sx_vexec_write_p(sx_db *h, const char *sql, const void *binds, int nbinds,
                 int64_t *nchanges)
{
	char *verr = NULL;
	int rc;
	xstore_stmt_pin((struct xsql *)h);
	rc = vx_run_write_p((struct xsql *)h, sql, (const vx_cell_t *)binds, nbinds,
	                    nchanges, &verr);
	xstore_stmt_end((struct xsql *)h);
	if (verr) free(verr);
	return rc;
}

void sx_vexec_commit(sx_db *h)   { (void)xstore_commit((struct xsql *)h); }
void sx_vexec_rollback(sx_db *h) { (void)xstore_rollback((struct xsql *)h); }

#else  /* !SQLXTC_HAVE_LIME -- no parser, so no vexec; always fall back. */

int sx_vexec_try(sx_db *h, const char *sql, int n_workers, sx_vx_result **out)
{ (void)h; (void)sql; (void)n_workers; if (out) *out = NULL; return 0; }
int sx_vexec_try_p(sx_db *h, const char *sql, const void *binds, int nbinds,
                   int n_workers, sx_vx_result **out)
{ (void)h; (void)sql; (void)binds; (void)nbinds; (void)n_workers;
  if (out) *out = NULL; return 0; }
void sx_vexec_free(sx_vx_result *r) { (void)r; }
int sx_vexec_nrow(const sx_vx_result *r) { (void)r; return 0; }
int sx_vexec_ncol(const sx_vx_result *r) { (void)r; return 0; }
int sx_vexec_type(const sx_vx_result *r, int row, int col)
{ (void)r; (void)row; (void)col; return SX_NULL; }
int64_t sx_vexec_int64(const sx_vx_result *r, int row, int col)
{ (void)r; (void)row; (void)col; return 0; }
double sx_vexec_double(const sx_vx_result *r, int row, int col)
{ (void)r; (void)row; (void)col; return 0.0; }
const char *sx_vexec_text(const sx_vx_result *r, int row, int col)
{ (void)r; (void)row; (void)col; return ""; }
const void *sx_vexec_blob(const sx_vx_result *r, int row, int col)
{ (void)r; (void)row; (void)col; return NULL; }
int sx_vexec_bytes(const sx_vx_result *r, int row, int col)
{ (void)r; (void)row; (void)col; return 0; }
const char *sx_vexec_name(const sx_vx_result *r, int col)
{ (void)r; (void)col; return NULL; }

int sx_vexec_write(sx_db *h, const char *sql, int64_t *nchanges)
{ (void)h; (void)sql; if (nchanges) *nchanges = 0; return 0; }
int sx_vexec_write_p(sx_db *h, const char *sql, const void *binds, int nbinds,
                     int64_t *nchanges)
{ (void)h; (void)sql; (void)binds; (void)nbinds;
  if (nchanges) *nchanges = 0; return 0; }

void sx_vexec_commit(sx_db *h)   { (void)h; }
void sx_vexec_rollback(sx_db *h) { (void)h; }

#endif /* SQLXTC_HAVE_LIME */

