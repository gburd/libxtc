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
#include "xtc_async.h"     /* xtc_yield -- the fiber-yielding busy handler */
#include "xtc_proc.h"      /* xtc_proc_sleep -- park, do not spin */
#include "xtc_loop.h"      /* xtc_loop_t -- background storage procs */

#include <assert.h>
#include <stdio.h>
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

int
sx_init(void)
{
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

int
sx_prepare(sx_db *h, const char *sql, int n_bytes, sx_stmt **out,
           const char **tail)
{
	return xsql_prepare_v2((xsql *)h, sql, n_bytes,
	    (xsql_stmt **)out, tail);
}

int   sx_step(sx_stmt *st)        { return xsql_step((xsql_stmt *)st); }
int   sx_reset(sx_stmt *st)       { return xsql_reset((xsql_stmt *)st); }
int   sx_clear_bindings(sx_stmt *st) { return xsql_clear_bindings((xsql_stmt *)st); }
void  sx_finalize(sx_stmt *st)    { (void)xsql_finalize((xsql_stmt *)st); }

int
sx_bind_count(sx_stmt *st)
{
	return xsql_bind_parameter_count((xsql_stmt *)st);
}
int
sx_bind_int64(sx_stmt *st, int idx, int64_t v)
{
	return xsql_bind_int64((xsql_stmt *)st, idx, v);
}
int
sx_bind_double(sx_stmt *st, int idx, double v)
{
	return xsql_bind_double((xsql_stmt *)st, idx, v);
}
int
sx_bind_text(sx_stmt *st, int idx, const char *s, int n)
{
	return xsql_bind_text((xsql_stmt *)st, idx, s, n,
	    SQLITE_TRANSIENT);
}
int
sx_bind_blob(sx_stmt *st, int idx, const void *p, int n)
{
	return xsql_bind_blob((xsql_stmt *)st, idx, p, n,
	    SQLITE_TRANSIENT);
}
int
sx_bind_null(sx_stmt *st, int idx)
{
	return xsql_bind_null((xsql_stmt *)st, idx);
}

int
sx_column_count(sx_stmt *st)
{
	return xsql_column_count((xsql_stmt *)st);
}
const char *
sx_column_name(sx_stmt *st, int i)
{
	return xsql_column_name((xsql_stmt *)st, i);
}
int
sx_column_type(sx_stmt *st, int i)
{
	return xsql_column_type((xsql_stmt *)st, i);
}
int64_t
sx_column_int64(sx_stmt *st, int i)
{
	return xsql_column_int64((xsql_stmt *)st, i);
}
double
sx_column_double(sx_stmt *st, int i)
{
	return xsql_column_double((xsql_stmt *)st, i);
}
const char *
sx_column_text(sx_stmt *st, int i)
{
	return (const char *)xsql_column_text((xsql_stmt *)st, i);
}
const void *
sx_column_blob(sx_stmt *st, int i)
{
	return xsql_column_blob((xsql_stmt *)st, i);
}
int
sx_column_bytes(sx_stmt *st, int i)
{
	return xsql_column_bytes((xsql_stmt *)st, i);
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
