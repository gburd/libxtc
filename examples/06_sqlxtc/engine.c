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

#include "sqlite3.h"
#include "xtc_async.h"     /* xtc_yield -- the fiber-yielding busy handler */
#include "xtc_proc.h"      /* xtc_proc_sleep -- park, do not spin */

#include <assert.h>
#include <string.h>

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
	return sqlite3_initialize();
}

int
sx_shutdown(void)
{
	return sqlite3_shutdown();
}

int
sx_config_mutex(const void *methods)
{
	return sqlite3_config(SQLITE_CONFIG_MUTEX,
	    (const sqlite3_mutex_methods *)methods);
}

int
sx_config_mem(const void *methods)
{
	return sqlite3_config(SQLITE_CONFIG_MALLOC,
	    (const sqlite3_mem_methods *)methods);
}

int
sx_config_serialized(void)
{
	return sqlite3_config(SQLITE_CONFIG_SERIALIZED);
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

int
sx_open(const char *path, sx_db **out)
{
	sqlite3 *h = NULL;
	int memlike = (path == NULL || path[0] == '\0' ||
	    strcmp(path, ":memory:") == 0);
	int rc;

	/* File-backed databases go through the xtc VFS (instrumented,
	 * offloaded I/O); in-memory databases do no file I/O. */
	if (!memlike)
		(void)vfs_register(0);
	rc = sqlite3_open_v2(path, &h,
	    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE,
	    memlike ? NULL : "sqlxtc");
	if (rc != SQLITE_OK) {
		if (h) sqlite3_close(h);
		*out = NULL;
		return rc;
	}

	/* Concurrency policy.  WAL lets readers run concurrently with a
	 * writer; a busy timeout makes concurrent writers queue and
	 * retry rather than fail with SQLITE_BUSY; NORMAL sync is the
	 * WAL-safe durability tradeoff.  (:memory: ignores journal_mode
	 * but honours busy_timeout harmlessly.) */
	(void)sqlite3_exec(h, "PRAGMA journal_mode=WAL;", NULL, NULL, NULL);
	(void)sqlite3_exec(h, "PRAGMA synchronous=NORMAL;", NULL, NULL, NULL);
	sqlite3_busy_handler((sqlite3 *)h, sx_busy_handler, NULL);

	*out = (sx_db *)h;
	return SQLITE_OK;
}

void
sx_close(sx_db *h)
{
	(void)sqlite3_close((sqlite3 *)h);
}

int
sx_exec(sx_db *h, const char *sql, char **errmsg)
{
	return sqlite3_exec((sqlite3 *)h, sql, NULL, NULL, errmsg);
}

int
sx_prepare(sx_db *h, const char *sql, int n_bytes, sx_stmt **out,
           const char **tail)
{
	return sqlite3_prepare_v2((sqlite3 *)h, sql, n_bytes,
	    (sqlite3_stmt **)out, tail);
}

int   sx_step(sx_stmt *st)        { return sqlite3_step((sqlite3_stmt *)st); }
int   sx_reset(sx_stmt *st)       { return sqlite3_reset((sqlite3_stmt *)st); }
void  sx_finalize(sx_stmt *st)    { (void)sqlite3_finalize((sqlite3_stmt *)st); }

int
sx_bind_count(sx_stmt *st)
{
	return sqlite3_bind_parameter_count((sqlite3_stmt *)st);
}
int
sx_bind_int64(sx_stmt *st, int idx, int64_t v)
{
	return sqlite3_bind_int64((sqlite3_stmt *)st, idx, v);
}
int
sx_bind_double(sx_stmt *st, int idx, double v)
{
	return sqlite3_bind_double((sqlite3_stmt *)st, idx, v);
}
int
sx_bind_text(sx_stmt *st, int idx, const char *s, int n)
{
	return sqlite3_bind_text((sqlite3_stmt *)st, idx, s, n,
	    SQLITE_TRANSIENT);
}
int
sx_bind_blob(sx_stmt *st, int idx, const void *p, int n)
{
	return sqlite3_bind_blob((sqlite3_stmt *)st, idx, p, n,
	    SQLITE_TRANSIENT);
}
int
sx_bind_null(sx_stmt *st, int idx)
{
	return sqlite3_bind_null((sqlite3_stmt *)st, idx);
}

int
sx_column_count(sx_stmt *st)
{
	return sqlite3_column_count((sqlite3_stmt *)st);
}
const char *
sx_column_name(sx_stmt *st, int i)
{
	return sqlite3_column_name((sqlite3_stmt *)st, i);
}
int
sx_column_type(sx_stmt *st, int i)
{
	return sqlite3_column_type((sqlite3_stmt *)st, i);
}
int64_t
sx_column_int64(sx_stmt *st, int i)
{
	return sqlite3_column_int64((sqlite3_stmt *)st, i);
}
double
sx_column_double(sx_stmt *st, int i)
{
	return sqlite3_column_double((sqlite3_stmt *)st, i);
}
const char *
sx_column_text(sx_stmt *st, int i)
{
	return (const char *)sqlite3_column_text((sqlite3_stmt *)st, i);
}
const void *
sx_column_blob(sx_stmt *st, int i)
{
	return sqlite3_column_blob((sqlite3_stmt *)st, i);
}
int
sx_column_bytes(sx_stmt *st, int i)
{
	return sqlite3_column_bytes((sqlite3_stmt *)st, i);
}

const char *
sx_errmsg(sx_db *h)
{
	return sqlite3_errmsg((sqlite3 *)h);
}
int64_t
sx_changes(sx_db *h)
{
	return (int64_t)sqlite3_changes64((sqlite3 *)h);
}
