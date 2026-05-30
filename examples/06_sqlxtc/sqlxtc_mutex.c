/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/sqlxtc_mutex.c
 *	SQLite mutex methods backed by xtc_amutex (the parking mutex).
 *
 *	sqlxtc runs many connection processes on one event loop, all
 *	sharing a single serialized sqlite3 handle.  A contending
 *	process must therefore PARK (yield the loop) rather than block
 *	the OS thread -- otherwise a backend that parks mid-statement
 *	(e.g. while the VFS offloads a blocking read to the thread pool)
 *	would wedge every other process on the loop.  xtc_amutex parks
 *	the fiber on a loop and falls back to a condvar off a loop, so
 *	the same methods work for the in-process server and the
 *	off-loop standalone tests.
 *
 *	SQLite distinguishes "fast" and "recursive" mutexes plus a set
 *	of well-known statics.  xtc_amutex is non-recursive, so the
 *	recursive case wraps it with an owner identity + a count.  The
 *	owner is tracked by FIBER identity (the proc pid) on a loop, not
 *	by thread id: two fibers sharing one OS thread must not be
 *	mistaken for the same lock owner, or mutual exclusion breaks the
 *	instant one of them parks while holding the lock.
 *
 *	The methods are registered from main.c via
 *	sqlite3_config(SQLITE_CONFIG_MUTEX, ...).
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sqlite/sqlite3.h"
#include "xtc.h"
#include "xtc_proc.h"
#include "xtc_sync.h"

/* ------------------------------------------------------------------ *
 * Owner identity
 *
 * On a loop the holder is a process (fiber); off a loop it is a plain
 * OS thread (startup, the standalone tests).  Recursion accounting
 * compares whichever identity applies.
 * ------------------------------------------------------------------ */
enum mutex_owner_kind {
	OWNER_NONE = 0,
	OWNER_PROC,
	OWNER_THREAD
};

struct mutex_owner {
	enum mutex_owner_kind kind;
	xtc_pid_t             pid;     /* when kind == OWNER_PROC */
	pthread_t             thr;     /* when kind == OWNER_THREAD */
};

static struct mutex_owner
owner_current(void)
{
	struct mutex_owner o;
	xtc_pid_t self = xtc_self();

	memset(&o, 0, sizeof o);
	if (!xtc_pid_is_none(self)) {
		o.kind = OWNER_PROC;
		o.pid = self;
	} else {
		o.kind = OWNER_THREAD;
		o.thr = pthread_self();
	}
	return o;
}

static int
owner_eq(const struct mutex_owner *a, const struct mutex_owner *b)
{
	if (a->kind != b->kind || a->kind == OWNER_NONE)
		return 0;
	if (a->kind == OWNER_PROC)
		return xtc_pid_eq(a->pid, b->pid);
	return pthread_equal(a->thr, b->thr);
}

/* Wraps an xtc_amutex with optional recursive accounting. */
struct sqlite3_mutex {
	xtc_amutex_t      *am;
	int                type;
	int                recursive;     /* 1 if SQLITE_MUTEX_RECURSIVE */

	/* Recursive-only state; protected by the amutex itself. */
	struct mutex_owner owner;
	int                count;
};

/* Static mutex pool: SQLite expects identical pointers for repeated
 * xMutexAlloc(SQLITE_MUTEX_STATIC_*) calls. */
#define STATIC_MUTEX_COUNT  16
static sqlite3_mutex g_static_mutexes[STATIC_MUTEX_COUNT];
static _Atomic int   g_static_inited = 0;

static void
init_static_pool(void)
{
	int i;
	int expected = 0;
	if (!atomic_compare_exchange_strong(&g_static_inited, &expected, 1))
		return;
	for (i = 0; i < STATIC_MUTEX_COUNT; i++) {
		(void)xtc_amutex_create(&g_static_mutexes[i].am);
		g_static_mutexes[i].type = i;
		g_static_mutexes[i].recursive = 1;     /* statics may recurse */
	}
}

static int
xMutexInit(void)
{
	init_static_pool();
	return SQLITE_OK;
}

static int
xMutexEnd(void)
{
	int i;
	for (i = 0; i < STATIC_MUTEX_COUNT; i++) {
		xtc_amutex_destroy(g_static_mutexes[i].am);
		g_static_mutexes[i].am = NULL;
	}
	atomic_store(&g_static_inited, 0);
	return SQLITE_OK;
}

static sqlite3_mutex *
xMutexAlloc(int type)
{
	sqlite3_mutex *m;

	if (type >= 2 && type < STATIC_MUTEX_COUNT) {
		init_static_pool();
		return &g_static_mutexes[type];
	}

	m = (sqlite3_mutex *)calloc(1, sizeof(*m));
	if (!m) return NULL;
	m->type = type;
	m->recursive = (type == SQLITE_MUTEX_RECURSIVE);
	if (xtc_amutex_create(&m->am) != XTC_OK) {
		free(m);
		return NULL;
	}
	return m;
}

static void
xMutexFree(sqlite3_mutex *m)
{
	if (!m) return;
	if (m->type >= 2 && m->type < STATIC_MUTEX_COUNT) return;
	xtc_amutex_destroy(m->am);
	free(m);
}

static void
xMutexEnter(sqlite3_mutex *m)
{
	if (!m) return;

	if (m->recursive) {
		struct mutex_owner self = owner_current();
		if (owner_eq(&m->owner, &self)) {
			m->count++;
			return;
		}
		(void)xtc_amutex_lock(m->am, -1);    /* park/block until held */
		m->owner = self;
		m->count = 1;
		return;
	}

	(void)xtc_amutex_lock(m->am, -1);
}

static int
xMutexTry(sqlite3_mutex *m)
{
	if (!m) return SQLITE_BUSY;

	if (m->recursive) {
		struct mutex_owner self = owner_current();
		if (owner_eq(&m->owner, &self)) {
			m->count++;
			return SQLITE_OK;
		}
		if (xtc_amutex_try_lock(m->am) == XTC_OK) {
			m->owner = self;
			m->count = 1;
			return SQLITE_OK;
		}
		return SQLITE_BUSY;
	}

	if (xtc_amutex_try_lock(m->am) == XTC_OK)
		return SQLITE_OK;
	return SQLITE_BUSY;
}

static void
xMutexLeave(sqlite3_mutex *m)
{
	if (!m) return;
	if (m->recursive) {
		if (m->count > 1) {
			m->count--;
			return;
		}
		m->count = 0;
		m->owner.kind = OWNER_NONE;
	}
	(void)xtc_amutex_unlock(m->am);
}

static int
xMutexHeld(sqlite3_mutex *m)
{
	struct mutex_owner self;
	if (!m) return 1;
	if (!m->recursive) return 1;   /* fast mutexes: best-effort assert */
	self = owner_current();
	return owner_eq(&m->owner, &self);
}

static int
xMutexNotheld(sqlite3_mutex *m)
{
	return !xMutexHeld(m);
}

static const sqlite3_mutex_methods sqlxtc_mutex_table = {
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

const sqlite3_mutex_methods *
sqlxtc_mutex_methods(void)
{
	return &sqlxtc_mutex_table;
}
