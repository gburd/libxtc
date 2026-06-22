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
#include "vfs.h"
#include "xstore.h"
#include "btree.h"
#include "bufmgr.h"
#include "wal.h"

#include "sqlite3.h"
#ifdef SQLXTC_HAVE_LIME
#include "vexec.h"        /* vx_run -- the vectorized-executor fast path */
#include "sql_parse.h"    /* sql_parse_ast -- classify statements natively */
#include "sql_ast.h"
#endif
#include "xtc_async.h"     /* xtc_yield -- the fiber-yielding busy handler */
#include "xtc_proc.h"      /* xtc_proc_sleep -- park, do not spin */
#include "xtc_loop.h"      /* xtc_loop_t -- background storage procs */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>      /* access, unlink, F_OK */

/* The sx_ result/type codes are the engine's ABI values; keep them in
 * lockstep so the wrappers need no translation. */
_Static_assert(SX_OK == SQLITE_OK, "SX_OK");
_Static_assert(SX_ROW == SQLITE_ROW, "SX_ROW");
_Static_assert(SX_DONE == SQLITE_DONE, "SX_DONE");
_Static_assert(SX_INTEGER == SQLITE_INTEGER, "SX_INTEGER");
_Static_assert(SX_FLOAT == SQLITE_FLOAT, "SX_FLOAT");
_Static_assert(SX_TEXT == SQLITE_TEXT, "SX_TEXT");
_Static_assert(SX_BLOB == SQLITE_BLOB, "SX_BLOB");
_Static_assert(SX_NULL == SQLITE_NULL, "SX_NULL");

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
	SXN_CREATE, SXN_DROP, SXN_PRAGMA_NOP
};

struct sx_stmt {
	int            native;     /* 0 = VDBE (vdbe set); 1 = native plan */
	xsql_stmt     *vdbe;       /* native == 0 */

	/* native == 1 */
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
	int            cur;        /* current row in vres (-1 before first) */
	int            ran;        /* 1 once executed (one-shot for DML/txn) */
	int64_t        nchanges;
#endif
};

/* Native-driver enable.  ON by default: the engine runs every
 * recognized statement (the whole tested corpus -- reads, writes,
 * transactions, CREATE/DROP TABLE, value PRAGMA) through the native
 * sx_stmt driver with NO VDBE program.  A statement the driver does not
 * yet classify natively still declines to the VDBE wrapper (correct-by-
 * fallback) until sqlite3.c is removed, at which point the decline
 * becomes an error.  SQLXTC_NATIVE_DRIVER=0 forces the VDBE for the
 * differential oracle. */
static int g_native_driver = 1;

void sx_native_driver(int on) { g_native_driver = on ? 1 : 0; }
int  sx_native_driver_enabled(void) { return g_native_driver; }

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
	/* The native sx_stmt driver is on by default; SQLXTC_NATIVE_DRIVER=0
	 * forces the VDBE (used by the differential oracle to produce the
	 * reference result).  Any other value (or unset) leaves it on. */
	const char *nd = getenv("SQLXTC_NATIVE_DRIVER");
	if (nd != NULL && nd[0] == '0')
		g_native_driver = 0;
	return xsql_initialize();
}

int
sx_shutdown(void)
{
	return xsql_shutdown();
}

int
sx_config_mutex(const void *methods)
{
	return xsql_config(SQLITE_CONFIG_MUTEX,
	    (const xsql_mutex_methods *)methods);
}

int
sx_config_mem(const void *methods)
{
	return xsql_config(SQLITE_CONFIG_MALLOC,
	    (const xsql_mem_methods *)methods);
}

int
sx_config_serialized(void)
{
	return xsql_config(SQLITE_CONFIG_SERIALIZED);
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
	return xsql_config(SQLITE_CONFIG_MULTITHREAD);
}

/*
 * Busy handler: when a connection finds the database locked (another
 * connection holds the WAL write lock), do NOT sleep the OS thread --
 * that would wedge a cooperative loop, since the lock holder may be a
 * parked fiber on this same thread (e.g. mid-fsync via the offloaded
 * VFS) that can only resume once the loop runs.  Instead yield the
 * fiber, letting the holder run, finish, and release; then retry.
 * Off a loop xtc_yield is a no-op and this becomes a bounded spin.
 * Give up after a generous cap so a genuinely stuck lock still
 * surfaces as an error rather than hanging.
 */
static int
sx_busy_handler(void *arg, int n_prior)
{
	(void)arg;
	if (n_prior > 100000)
		return 0;               /* give up -> SQLITE_BUSY */
	/* Park briefly (not spin): a timer park drains the run queue so
	 * the loop polls I/O and the lock holder -- which may be a parked
	 * fiber on this same thread doing an offloaded fsync -- can
	 * resume, finish, and release.  A bare xtc_yield would keep the
	 * run queue hot and starve that I/O.  Off a loop xtc_proc_sleep
	 * is a no-op (XTC_E_INVAL) and this falls back to a yield. */
	if (xtc_proc_sleep(200LL * 1000) != XTC_OK)   /* 0.2ms */
		xtc_yield();
	return 1;                       /* retry */
}

/* ---- engine-native storage (xstore) lifecycle ---- */
static bm_t *g_xbm;
static bt_t *g_xbt;
static wal_t *g_xwal;
static char   g_xwal_path[1024];
static int    g_xrunning;        /* background procs spawned */

int
sx_open(const char *path, sx_db **out)
{
	xsql *h = NULL;
	int memlike = (path == NULL || path[0] == '\0' ||
	    strcmp(path, ":memory:") == 0 ||
	    strstr(path, "mode=memory") != NULL);   /* shared-cache :memory: URI */
	int rc;

	/* File-backed databases go through the xtc VFS (instrumented,
	 * offloaded I/O); in-memory databases do no file I/O. */
	if (!memlike)
		(void)vfs_register(0);
	rc = xsql_open_v2(path, &h,
	    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_URI,
	    memlike ? NULL : "sqlxtc");
	if (rc != SQLITE_OK) {
		if (h) xsql_close(h);
		*out = NULL;
		return rc;
	}

	/* Concurrency policy.  WAL lets readers run concurrently with a
	 * writer; a busy timeout makes concurrent writers queue and
	 * retry rather than fail with SQLITE_BUSY; NORMAL sync is the
	 * WAL-safe durability tradeoff.  (:memory: ignores journal_mode
	 * but honours busy_timeout harmlessly.) */
	(void)xsql_exec(h, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
	(void)xsql_exec(h, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);
	xsql_busy_handler((xsql *)h, sx_busy_handler, NULL);

	/* If the libxtc-native storage engine is open, expose it to this
	 * connection as the "xstore" virtual table.  SQL against an
	 * xstore table executes on our on-disk B-tree (cooling buffer
	 * pool, larger-than-RAM) instead of SQLite's built-in B-tree.
	 * The B-tree is concurrent (parallel-writer crabbing), so the
	 * single shared instance is safe across connection procs. */
	if (g_xbt != NULL)
		(void)xstore_register((xsql *)h, g_xbt);

	*out = (sx_db *)h;
	return SQLITE_OK;
}

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
 *   any work, so a crash from here on falls back to a rebuild.
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

/* Configure direct I/O + adaptive writeback for the next sx_storage_open. */
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
	const char *dpath = (path && path[0]) ? path : "sqlxtc.xdb";
	uint64_t clean = 0, clock = 0;
	int trusted = 0;

	if (g_xbt != NULL)
		return SX_OK;                 /* already open */

	o.path = dpath;
	if (n_frames > 0)
		o.n_frames = n_frames;
	o.double_write = 1;   /* torn-page protection for the persistent store */
	o.lsn_off = 0;        /* ARIES page LSN: first field of every btnode */
	o.direct = g_storage_direct;             /* cache-bypass page store */
	o.adaptive_writeback = g_storage_adaptive;  /* GA-tuned trickler */

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

	if (trusted) {
		xstore_set_clock(clock);          /* restore the commit clock */
		bt_set_meta(g_xbt, 0, clock);     /* mark dirty: a crash now rebuilds */
		bt_write_super(g_xbt);
		(void)bm_sync(g_xbm);
	} else {
		/* Fresh page file; rebuild the tree from the (bounded) log. */
		o.reopen = 0;
		if (bm_create(&o, &g_xbm) != XTC_OK)
			return SQLITE_ERROR;
		if (bt_open(g_xbm, &g_xbt) != XTC_OK) {
			bm_destroy(g_xbm); g_xbm = NULL;
			return SQLITE_ERROR;
		}
		(void)xstore_recover(g_xbt, g_xwal_path);  /* replay the log */
	}

	/* Open the log (append past whatever is there). */
	memset(&wo, 0, sizeof wo);
	wo.path = g_xwal_path;
	wo.window_ns = 500LL * 1000;       /* 0.5ms group-commit window */
	wo.max_batch = 256;
	wo.append = 1;
	if (wal_open(&wo, &g_xwal) != XTC_OK) {
		bt_close(g_xbt); g_xbt = NULL;
		bm_destroy(g_xbm); g_xbm = NULL;
		return SQLITE_ERROR;
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
	if (g_xbm == NULL)
		return SX_OK;
	(void)bm_checkpoint(g_xbm);            /* flush dirty pages (not for durability) */
	if (g_xwal != NULL &&
	    xstore_checkpoint_wal(g_xbt, (struct wal *)g_xwal, g_xwal_path) != XTC_OK)
		return SQLITE_ERROR;
	return SX_OK;
}

/*
 * Stop the background storage procs (WAL writer, page provider,
 * trickler) without tearing down the engine, so an executor running
 * them can go idle and xtc_exec_run can return -- after which the
 * caller does sx_storage_close off the loop.  Idempotent.  Safe to
 * call from a proc once all committers are done (no commit may follow,
 * since the group-commit writer is stopped).
 */
void
sx_storage_quiesce(void)
{
	if (!g_xrunning)
		return;
	if (g_xwal != NULL) wal_writer_stop(g_xwal);
	bm_trickler_stop(g_xbm);
	bm_provider_stop(g_xbm);
	g_xrunning = 0;
}

void
sx_storage_close(void)
{
	if (g_xrunning) {
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
	 * A crash anywhere before the clean marker is durable leaves the
	 * flag unset, so recovery rebuilds -- always safe.
	 */
	if (g_xbt != NULL && g_xbm != NULL) {
		(void)bm_checkpoint(g_xbm);        /* flush all dirty pages + fsync */
		if (g_xwal != NULL)
			(void)xstore_checkpoint_wal(g_xbt, (struct wal *)g_xwal,
			    g_xwal_path);
		bm_set_wal_flush(g_xbm, NULL, NULL);   /* drop the hook before closing the log */
	}
	xstore_set_wal(NULL);
	if (g_xwal != NULL) { wal_close(g_xwal); g_xwal = NULL; }
	if (g_xbt != NULL && g_xbm != NULL) {
		bt_set_meta(g_xbt, 1, xstore_clock());  /* base is consistent up to here */
		bt_write_super(g_xbt);
		(void)bm_sync(g_xbm);              /* make the clean marker durable */
	}
	if (g_xbt != NULL) { bt_close(g_xbt); g_xbt = NULL; }
	if (g_xbm != NULL) { bm_destroy(g_xbm); g_xbm = NULL; }
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

int
sx_storage_active(void)
{
	return g_xbt != NULL;
}

void
sx_close(sx_db *h)
{
	(void)xsql_close((xsql *)h);
}

int
sx_exec(sx_db *h, const char *sql, char **errmsg)
{
	return xsql_exec((xsql *)h, sql, NULL, NULL, errmsg);
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
sx_classify(const char *sql, int *tail_more)
{
	sql_arena_t *ast = NULL;
	sql_stmt_t  *root = NULL;
	const char  *perr = NULL;
	enum sx_native_kind k = SXN_NONE;

	*tail_more = 0;
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
	case SQL_KIND_ROLLBACK: k = SXN_ROLLBACK; break;
	case SQL_KIND_CREATE:
	case SQL_KIND_DROP:
		/* DDL is kept on the VDBE in the live path (the server's CREATE
		 * TABLE -> CREATE VIRTUAL TABLE rewrite + the vtab schema must
		 * stay consistent).  Native DDL over the xstore catalog is
		 * available directly via xstore_create_table / xstore_drop_table
		 * (used by the from-scratch engine once the vtab is retired).
		 * Decline here -> VDBE wrapper. */
		k = SXN_NONE;
		break;
	case SQL_KIND_PRAGMA: {
		/* A PRAGMA that SETS a value (journal_mode=WAL, synchronous=...,
		 * busy_timeout=...) is a no-op for the native engine -- it has no
		 * SQLite journal/cache to configure -- so classify it native and
		 * return no rows.  A bare read PRAGMA that returns rows (e.g.
		 * table_info) still declines to the VDBE for now (native rows
		 * are a later refinement). */
		const sql_pragma_t *p = root->u.pragma;
		k = (p != NULL && p->value != NULL) ? SXN_PRAGMA_NOP : SXN_NONE;
		break;
	}
	default:                k = SXN_NONE;     break;   /* other */
	}
	sql_arena_destroy(ast);
	return k;
}
#endif /* SQLXTC_HAVE_LIME */

int
sx_prepare(sx_db *h, const char *sql, int n_bytes, sx_stmt **out,
           const char **tail)
{
	struct sx_stmt *st;
	xsql_stmt *vdbe = NULL;
	int rc;

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
		int more = 0;
		enum sx_native_kind k = sx_classify(sql, &more);
		if (k != SXN_NONE) {
			st = (struct sx_stmt *)calloc(1, sizeof *st);
			if (st == NULL) return SQLITE_NOMEM;
			st->native = 1;
			st->db = h;
			st->nkind = k;
			st->cur = -1;
			st->sql = strdup(sql);
			if (st->sql == NULL) { free(st); return SQLITE_NOMEM; }
			if (tail) *tail = sql + strlen(sql);   /* single statement */
			*out = st;
			return SQLITE_OK;
		}
		/* SXN_NONE: fall through to the VDBE wrapper. */
	}
#endif
	/*
	 * Default (and native decline): wrap a VDBE statement.  The prepared
	 * statement is byte-for-byte what it always was, just behind the
	 * wrapper.  A NULL VDBE stmt (blank/comment-only fragment) is
	 * preserved as a NULL *out so the caller's existing logic is
	 * unchanged.
	 */
	rc = xsql_prepare_v2((xsql *)h, sql, n_bytes, &vdbe, tail);
	if (rc != SQLITE_OK)
		return rc;
	if (vdbe == NULL)
		return SQLITE_OK;            /* blank fragment: *out stays NULL */
	st = (struct sx_stmt *)calloc(1, sizeof *st);
	if (st == NULL) { (void)xsql_finalize(vdbe); return SQLITE_NOMEM; }
	st->native = 0;
	st->vdbe = vdbe;
	*out = st;
	return SQLITE_OK;
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
	rc = vx_run_p((sqlite3 *)st->db, st->sql,
	    st->nbind ? st->binds : NULL, st->nbind, 1, &st->vres, &verr);
	st->ran = 1;
	if (verr) free(verr);
	return (rc == 1 && st->vres != NULL);
}

/* Convert a native sx_stmt to a VDBE statement when the native executor
 * declines a statement at run time (e.g. an in-txn explicit-PK INSERT
 * the native write path does not handle, or a read vexec cannot run).
 * Prepares the VDBE, re-binds the cells, flips the stmt to VDBE mode.
 * Returns 1 on success, 0 on failure.  This is the transition safety
 * net; it vanishes when sqlite3.c is removed (a decline then errors). */
static int
nat_fallback_to_vdbe(struct sx_stmt *st)
{
	xsql_stmt *vdbe = NULL;
	int i;
	if (st->sql == NULL) return 0;
	if (xsql_prepare_v2((xsql *)st->db, st->sql, -1, &vdbe, NULL) != SQLITE_OK ||
	    vdbe == NULL)
		return 0;
	for (i = 0; i < st->nbind; i++) {
		switch (st->binds[i].type) {
		case VX_INT:  xsql_bind_int64(vdbe, i + 1, st->binds[i].i); break;
		case VX_REAL: xsql_bind_double(vdbe, i + 1, st->binds[i].r); break;
		case VX_TEXT: xsql_bind_text(vdbe, i + 1, (const char *)st->binds[i].bytes,
		                             (int)st->binds[i].nbytes, SQLITE_TRANSIENT); break;
		case VX_BLOB: xsql_bind_blob(vdbe, i + 1, st->binds[i].bytes,
		                             (int)st->binds[i].nbytes, SQLITE_TRANSIENT); break;
		default:      xsql_bind_null(vdbe, i + 1); break;
		}
	}
	if (st->vres) { vx_result_free(st->vres); st->vres = NULL; }
	st->vdbe = vdbe;
	st->native = 0;   /* from here the wrapper forwards to the VDBE */
	return 1;
}
#endif /* SQLXTC_HAVE_LIME */

int
sx_step(sx_stmt *st)
{
	if (st == NULL) return SQLITE_MISUSE;
#ifdef SQLXTC_HAVE_LIME
	if (st->native) {
		if (st->nkind == SXN_SELECT) {
			/* First step: run the SELECT through vexec into vres; if vexec
			 * declines, fall the whole statement back to the VDBE. */
			if (!st->ran && !nat_select_run(st)) {
				if (!nat_fallback_to_vdbe(st)) return SQLITE_ERROR;
				return xsql_step(st->vdbe);
			}
			if (st->vres == NULL) {
				if (!nat_fallback_to_vdbe(st)) return SQLITE_ERROR;
				return xsql_step(st->vdbe);
			}
			st->cur++;
			return (st->cur < vx_result_nrow(st->vres)) ? SQLITE_ROW
			                                             : SQLITE_DONE;
		}
		if (st->ran) return SQLITE_DONE;   /* one-shot kinds */
		st->ran = 1;
		switch (st->nkind) {
		case SXN_WRITE: {
			int64_t nch = 0;
			int rc = vx_run_write_p((sqlite3 *)st->db, st->sql,
			    st->nbind ? st->binds : NULL, st->nbind, &nch, NULL);
			if (rc == 1) { st->nchanges = nch; return SQLITE_DONE; }
			/* The native write path declined (e.g. an in-txn explicit-PK
			 * INSERT): fall the statement back to the VDBE. */
			if (!nat_fallback_to_vdbe(st)) return SQLITE_ERROR;
			return xsql_step(st->vdbe);
		}
		case SXN_BEGIN:
			return xstore_native_begin(st->db) == 0 ? SQLITE_DONE : SQLITE_ERROR;
		case SXN_COMMIT:
			return xstore_commit(st->db) == 0 ? SQLITE_DONE : SQLITE_ERROR;
		case SXN_ROLLBACK:
			return xstore_rollback(st->db) == 0 ? SQLITE_DONE : SQLITE_ERROR;
		case SXN_CREATE:
			return xstore_create_table(st->db, st->ddl_name, st->ddl_cols) != 0
			    ? SQLITE_DONE : SQLITE_ERROR;
		case SXN_DROP:
			return xstore_drop_table(st->db, st->ddl_name) == 0
			    ? SQLITE_DONE : SQLITE_ERROR;
		case SXN_PRAGMA_NOP:
			return SQLITE_DONE;   /* value-setting PRAGMA: no-op, no rows */
		default:
			return SQLITE_ERROR;
		}
	}
#endif
	return xsql_step(st->vdbe);
}
int
sx_reset(sx_stmt *st)
{
	if (st == NULL) return SQLITE_OK;
#ifdef SQLXTC_HAVE_LIME
	if (st->native) {
		if (st->vres) { vx_result_free(st->vres); st->vres = NULL; }
		st->cur = -1; st->ran = 0;
		return SQLITE_OK;
	}
#endif
	return xsql_reset(st->vdbe);
}
int
sx_clear_bindings(sx_stmt *st)
{
	if (st == NULL) return SQLITE_OK;
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
		return SQLITE_OK;
	}
#endif
	return xsql_clear_bindings(st->vdbe);
}
void
sx_finalize(sx_stmt *st)
{
	if (st == NULL) return;
	if (st->vdbe != NULL)
		(void)xsql_finalize(st->vdbe);
#ifdef SQLXTC_HAVE_LIME
	/* Free the native-plan resources whether or not the stmt fell back
	 * to the VDBE (nat_fallback_to_vdbe sets native=0 but the sql / bind
	 * buffers / ddl strings were allocated in native mode). */
	{
		int i;
		if (st->vres) vx_result_free(st->vres);
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
	return xsql_bind_parameter_count(st->vdbe);
}
int
sx_bind_int64(sx_stmt *st, int idx, int64_t v)
{
#ifdef SQLXTC_HAVE_LIME
	if (st->native) {
		if (idx < 1 || idx > 32) return SQLITE_RANGE;
		st->binds[idx-1].type = VX_INT; st->binds[idx-1].i = v;
		if (idx > st->nbind) st->nbind = idx;
		return SQLITE_OK;
	}
#endif
	return xsql_bind_int64(st->vdbe, idx, v);
}
int
sx_bind_double(sx_stmt *st, int idx, double v)
{
#ifdef SQLXTC_HAVE_LIME
	if (st->native) {
		if (idx < 1 || idx > 32) return SQLITE_RANGE;
		st->binds[idx-1].type = VX_REAL; st->binds[idx-1].r = v;
		if (idx > st->nbind) st->nbind = idx;
		return SQLITE_OK;
	}
#endif
	return xsql_bind_double(st->vdbe, idx, v);
}
int
sx_bind_text(sx_stmt *st, int idx, const char *s, int n)
{
#ifdef SQLXTC_HAVE_LIME
	if (st->native) {
		uint8_t *p;
		if (idx < 1 || idx > 32) return SQLITE_RANGE;
		if (n < 0) n = s ? (int)strlen(s) : 0;
		p = (uint8_t *)malloc((size_t)n + 1);
		if (p == NULL) return SQLITE_NOMEM;
		if (n && s) memcpy(p, s, (size_t)n);
		p[n] = '\0';
		/* The bind buffer is owned by the cell and freed on finalize/
		 * reset/clear (see bind cleanup). */
		st->binds[idx-1].type = VX_TEXT; st->binds[idx-1].bytes = p;
		st->binds[idx-1].nbytes = (uint32_t)n;
		if (idx > st->nbind) st->nbind = idx;
		return SQLITE_OK;
	}
#endif
	return xsql_bind_text(st->vdbe, idx, s, n, SQLITE_TRANSIENT);
}
int
sx_bind_blob(sx_stmt *st, int idx, const void *p, int n)
{
#ifdef SQLXTC_HAVE_LIME
	if (st->native) {
		uint8_t *b;
		if (idx < 1 || idx > 32) return SQLITE_RANGE;
		if (n < 0) n = 0;
		b = (uint8_t *)malloc((size_t)n + 1);
		if (b == NULL) return SQLITE_NOMEM;
		if (n && p) memcpy(b, p, (size_t)n);
		b[n] = '\0';
		st->binds[idx-1].type = VX_BLOB; st->binds[idx-1].bytes = b;
		st->binds[idx-1].nbytes = (uint32_t)n;
		if (idx > st->nbind) st->nbind = idx;
		return SQLITE_OK;
	}
#endif
	return xsql_bind_blob(st->vdbe, idx, p, n, SQLITE_TRANSIENT);
}
int
sx_bind_null(sx_stmt *st, int idx)
{
#ifdef SQLXTC_HAVE_LIME
	if (st->native) {
		if (idx < 1 || idx > 32) return SQLITE_RANGE;
		st->binds[idx-1].type = VX_NULL; st->binds[idx-1].nbytes = 0;
		if (idx > st->nbind) st->nbind = idx;
		return SQLITE_OK;
	}
#endif
	return xsql_bind_null(st->vdbe, idx);
}

int
sx_column_count(sx_stmt *st)
{
#ifdef SQLXTC_HAVE_LIME
	if (st->native) {
		if (st->nkind != SXN_SELECT) return 0;
		if (!st->ran && !nat_select_run(st)) {
			if (!nat_fallback_to_vdbe(st)) return 0;
			return xsql_column_count(st->vdbe);
		}
		if (st->vres == NULL) {
			if (!nat_fallback_to_vdbe(st)) return 0;
			return xsql_column_count(st->vdbe);
		}
		return vx_result_ncol(st->vres);
	}
#endif
	return xsql_column_count(st->vdbe);
}
const char *
sx_column_name(sx_stmt *st, int i)
{
#ifdef SQLXTC_HAVE_LIME
	if (st->native) {
		if (st->nkind != SXN_SELECT) return NULL;
		if (!st->ran) (void)nat_select_run(st);
		if (st->vres == NULL) {
			/* declined + fell back via sx_column_count earlier, or now */
			if (st->native && !nat_fallback_to_vdbe(st)) return NULL;
			return st->native ? NULL : xsql_column_name(st->vdbe, i);
		}
		return vx_result_name(st->vres, i);
	}
#endif
	return xsql_column_name(st->vdbe, i);
}
int
sx_column_type(sx_stmt *st, int i)
{
#ifdef SQLXTC_HAVE_LIME
	if (st->native) {
		if (st->nkind != SXN_SELECT || st->vres == NULL || st->cur < 0) return SX_NULL;
		switch (vx_result_type(st->vres, st->cur, i)) {
		case VX_INT:  return SX_INTEGER;
		case VX_REAL: return SX_FLOAT;
		case VX_TEXT: return SX_TEXT;
		case VX_BLOB: return SX_BLOB;
		default:      return SX_NULL;
		}
	}
#endif
	return xsql_column_type(st->vdbe, i);
}
int64_t
sx_column_int64(sx_stmt *st, int i)
{
#ifdef SQLXTC_HAVE_LIME
	if (st->native)
		return (st->vres && st->cur >= 0) ? vx_result_int64(st->vres, st->cur, i) : 0;
#endif
	return xsql_column_int64(st->vdbe, i);
}
double
sx_column_double(sx_stmt *st, int i)
{
#ifdef SQLXTC_HAVE_LIME
	if (st->native)
		return (st->vres && st->cur >= 0) ? vx_result_double(st->vres, st->cur, i) : 0.0;
#endif
	return xsql_column_double(st->vdbe, i);
}
const char *
sx_column_text(sx_stmt *st, int i)
{
#ifdef SQLXTC_HAVE_LIME
	if (st->native)
		return (st->vres && st->cur >= 0) ? vx_result_text(st->vres, st->cur, i) : NULL;
#endif
	return (const char *)xsql_column_text(st->vdbe, i);
}
const void *
sx_column_blob(sx_stmt *st, int i)
{
#ifdef SQLXTC_HAVE_LIME
	if (st->native)
		return (st->vres && st->cur >= 0) ? (const void *)vx_result_text(st->vres, st->cur, i) : NULL;
#endif
	return xsql_column_blob(st->vdbe, i);
}
int
sx_column_bytes(sx_stmt *st, int i)
{
#ifdef SQLXTC_HAVE_LIME
	if (st->native)
		return (st->vres && st->cur >= 0) ? vx_result_bytes(st->vres, st->cur, i) : 0;
#endif
	return xsql_column_bytes(st->vdbe, i);
}

const char *
sx_errmsg(sx_db *h)
{
	return xsql_errmsg((xsql *)h);
}
int64_t
sx_changes(sx_db *h)
{
	return (int64_t)xsql_changes64((xsql *)h);
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
	rc = vx_run((xsql *)h, sql, n_workers, &r, &verr);
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
	rc = vx_run_p((xsql *)h, sql, (const vx_cell_t *)binds, nbinds,
	              n_workers, &r, &verr);
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
	int rc = vx_run_write((xsql *)h, sql, nchanges, &verr);
	if (verr) free(verr);          /* caller falls back; no message surfaced */
	return rc;
}

int
sx_vexec_write_p(sx_db *h, const char *sql, const void *binds, int nbinds,
                 int64_t *nchanges)
{
	char *verr = NULL;
	int rc = vx_run_write_p((xsql *)h, sql, (const vx_cell_t *)binds, nbinds,
	                        nchanges, &verr);
	if (verr) free(verr);
	return rc;
}

void sx_vexec_commit(sx_db *h)   { (void)xstore_commit((xsql *)h); }
void sx_vexec_rollback(sx_db *h) { (void)xstore_rollback((xsql *)h); }

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

