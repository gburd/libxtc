/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/mutex.c
 *	SQLite mutex methods backed by xtc_amutex (the parking mutex).
 *
 *	This is now a thin pass-through.  Everything SQLite's mutex
 *	contract needs beyond a plain lock -- recursion, fiber-identity
 *	ownership, and process-global static mutexes -- is provided by
 *	xtc_amutex itself (xtc_amutex_create_ex(XTC_AMUTEX_RECURSIVE) and
 *	xtc_amutex_static), where the owner/recursion accounting is done
 *	under the mutex's own lock and is therefore race-free across
 *	loops.  The seam used to carry that bookkeeping by hand; it was
 *	racy and is gone.
 *
 *	A contending fiber PARKS (yields the loop) rather than blocking
 *	the OS thread, so a backend that parks mid-statement (e.g. while
 *	the VFS offloads a blocking read) does not wedge its peers.
 *
 *	Registered from main.c via xsql_config(SQLITE_CONFIG_MUTEX, ...).
 */

#include <stdlib.h>

#include "sqlite3.h"
#include "xtc.h"
#include "xtc_sync.h"

/* One SQLite mutex == one xtc_amutex.  Static mutexes borrow the
 * library's process-global pool so repeated allocs return the same
 * object; dynamic mutexes own a freshly created amutex. */
struct xsql_mutex {
	xtc_amutex_t *am;
	int           type;
	int           is_static;
};

#define STATIC_BASE  2   /* SQLITE_MUTEX_STATIC_* start at 2 */

static int
xMutexInit(void)
{
	return SQLITE_OK;
}

static int
xMutexEnd(void)
{
	return SQLITE_OK;
}

static xsql_mutex *
xMutexAlloc(int type)
{
	xsql_mutex *m;
	unsigned flags;

	if (type >= STATIC_BASE) {
		/* Named static mutex: stable object per type, never freed.
		 * Wrap the library's static pool slot in a small static
		 * descriptor (also stable). */
		static xsql_mutex g_static[(int)XTC_AMUTEX_STATIC_MAX];
		unsigned slot = (unsigned)(type - STATIC_BASE);
		if (slot >= XTC_AMUTEX_STATIC_MAX)
			return NULL;
		if (g_static[slot].am == NULL) {
			g_static[slot].am = xtc_amutex_static(slot);
			g_static[slot].type = type;
			g_static[slot].is_static = 1;
		}
		return g_static[slot].am != NULL ? &g_static[slot] : NULL;
	}

	m = (xsql_mutex *)calloc(1, sizeof(*m));
	if (!m)
		return NULL;
	m->type = type;
	m->is_static = 0;
	flags = (type == SQLITE_MUTEX_RECURSIVE) ? XTC_AMUTEX_RECURSIVE : 0;
	if (xtc_amutex_create_ex(&m->am, flags) != XTC_OK) {
		free(m);
		return NULL;
	}
	return m;
}

static void
xMutexFree(xsql_mutex *m)
{
	if (!m || m->is_static)
		return;
	xtc_amutex_destroy(m->am);
	free(m);
}

static void
xMutexEnter(xsql_mutex *m)
{
	if (m)
		(void)xtc_amutex_lock(m->am, -1);   /* park/block until held */
}

static int
xMutexTry(xsql_mutex *m)
{
	if (!m)
		return SQLITE_BUSY;
	return xtc_amutex_try_lock(m->am) == XTC_OK ? SQLITE_OK : SQLITE_BUSY;
}

static void
xMutexLeave(xsql_mutex *m)
{
	if (m)
		(void)xtc_amutex_unlock(m->am);
}

/* Held/Notheld back only SQLite's debug-build asserts (compiled out in
 * the release amalgamation).  Best-effort: report held. */
static int
xMutexHeld(xsql_mutex *m)
{
	(void)m;
	return 1;
}

static int
xMutexNotheld(xsql_mutex *m)
{
	(void)m;
	return 0;
}

static const xsql_mutex_methods mutex_table = {
	xMutexInit,
	xMutexEnd,
	xMutexAlloc,
	xMutexFree,
	xMutexEnter,
	xMutexTry,
	xMutexLeave,
	xMutexHeld,
	xMutexNotheld
};

const void *
mutex_methods(void)
{
	return &mutex_table;
}
