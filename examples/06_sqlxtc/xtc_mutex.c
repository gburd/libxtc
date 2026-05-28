/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/xtc_mutex.c
 *	SQLite mutex methods backed by xtc_lwlock.
 *
 *	SQLite distinguishes "fast" and "recursive" mutex types plus a
 *	handful of well-known statics.  xtc_lwlock is non-recursive, so
 *	the recursive case wraps the lwlock with a thread-id + counter.
 *	The static slots are pre-allocated so that any of SQLite's
 *	zero-init quirks are tolerated.
 *
 *	This file builds against SQLite's amalgamation header.  The
 *	resulting sqlite3_mutex_methods is registered from main.c via
 *	sqlite3_config(SQLITE_CONFIG_MUTEX, ...).
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "sqlite/sqlite3.h"
#include "xtc.h"
#include "xtc_lwlock.h"

#define XTC_MUTEX_TRANCHE_FAST       100
#define XTC_MUTEX_TRANCHE_RECURSIVE  101
#define XTC_MUTEX_TRANCHE_STATIC     102

/* Wraps an xtc_lwlock with optional recursive accounting. */
struct sqlite3_mutex {
	xtc_lwlock_t      lw;
	int               type;
	int               recursive;     /* 1 if SQLITE_MUTEX_RECURSIVE */

	/* Recursive-only state; protected by the lwlock itself. */
	pthread_t         owner;
	int               owner_set;
	int               count;
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
		(void)xtc_lwlock_init(&g_static_mutexes[i].lw,
		                      XTC_MUTEX_TRANCHE_STATIC);
		g_static_mutexes[i].type = i;
		g_static_mutexes[i].recursive = 1;     /* be safe */
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
		xtc_lwlock_destroy(&g_static_mutexes[i].lw);
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
	if (xtc_lwlock_init(&m->lw,
	    m->recursive ? XTC_MUTEX_TRANCHE_RECURSIVE :
	                   XTC_MUTEX_TRANCHE_FAST) != XTC_OK) {
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
	xtc_lwlock_destroy(&m->lw);
	free(m);
}

static void
xMutexEnter(sqlite3_mutex *m)
{
	if (!m) return;

	/* Recursive: owner check before taking the lock. */
	if (m->recursive) {
		pthread_t self = pthread_self();
		if (m->owner_set && pthread_equal(m->owner, self)) {
			m->count++;
			return;
		}
		(void)xtc_lwlock_acquire(&m->lw, XTC_LW_EXCLUSIVE);
		m->owner = self;
		m->owner_set = 1;
		m->count = 1;
		return;
	}

	(void)xtc_lwlock_acquire(&m->lw, XTC_LW_EXCLUSIVE);
}

static int
xMutexTry(sqlite3_mutex *m)
{
	if (!m) return SQLITE_BUSY;

	if (m->recursive) {
		pthread_t self = pthread_self();
		if (m->owner_set && pthread_equal(m->owner, self)) {
			m->count++;
			return SQLITE_OK;
		}
		if (xtc_lwlock_acquire_cond(&m->lw, XTC_LW_EXCLUSIVE) ==
		    XTC_OK) {
			m->owner = self;
			m->owner_set = 1;
			m->count = 1;
			return SQLITE_OK;
		}
		return SQLITE_BUSY;
	}

	if (xtc_lwlock_acquire_cond(&m->lw, XTC_LW_EXCLUSIVE) == XTC_OK)
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
		m->owner_set = 0;
	}
	xtc_lwlock_release(&m->lw);
}

static int
xMutexHeld(sqlite3_mutex *m)
{
	if (!m) return 1;
	if (m->recursive && m->owner_set &&
	    pthread_equal(m->owner, pthread_self())) {
		return 1;
	}
	return xtc_lwlock_held_by_me(&m->lw);
}

static int
xMutexNotheld(sqlite3_mutex *m)
{
	return !xMutexHeld(m);
}

static const sqlite3_mutex_methods xtc_methods = {
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
xtc_sqlite_mutex_methods(void)
{
	return &xtc_methods;
}
