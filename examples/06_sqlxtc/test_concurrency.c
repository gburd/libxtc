/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_concurrency.c
 *	In-process test of the xtc_amutex-backed SQLite mutex methods.
 *
 *	Proves the property the shared-handle server depends on: a
 *	process that holds a SQLite mutex across a park (the VFS
 *	offload case) does NOT wedge the loop -- a contending process
 *	parks (yields the loop) rather than blocking the single OS
 *	thread, so the holder can resume, release, and hand off.  A
 *	thread-blocking mutex would deadlock this exact arrangement.
 *
 *	Also exercises recursive enter/leave from one process.  No
 *	network, no daemon.
 */

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>

#include "sqlite/sqlite3.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"

extern const sqlite3_mutex_methods *mutex_methods(void);

static sqlite3_mutex *g_m;
static _Atomic int    g_seq;          /* monotonically issued tickets */
static int            g_a_acquire = -1, g_a_release = -1, g_b_acquire = -1;
static int            g_a_recursion_ok = 0;

/* Holder: acquires first, recurses, then PARKS while still holding the
 * mutex (xtc_recv timeout == a voluntary yield), then releases. */
static void
holder_proc(void *arg)
{
	void *m = NULL; size_t n = 0;
	(void)arg;

	sqlite3_mutex_enter(g_m);
	g_a_acquire = atomic_fetch_add(&g_seq, 1);

	sqlite3_mutex_enter(g_m);             /* recursive re-entry */
	sqlite3_mutex_leave(g_m);
	g_a_recursion_ok = 1;                 /* survived recursion */

	/* Hold across a park so the contender is forced to wait. */
	(void)xtc_recv(&m, &n, 60LL * 1000 * 1000);
	if (m) m = NULL;

	g_a_release = atomic_fetch_add(&g_seq, 1);
	sqlite3_mutex_leave(g_m);
}

/* Contender: yields briefly so the holder grabs the lock first, then
 * blocks on it.  On a loop this must PARK, not thread-block. */
static void
contender_proc(void *arg)
{
	void *m = NULL; size_t n = 0;
	(void)arg;

	(void)xtc_recv(&m, &n, 5LL * 1000 * 1000);
	if (m) m = NULL;

	sqlite3_mutex_enter(g_m);             /* parks until holder leaves */
	g_b_acquire = atomic_fetch_add(&g_seq, 1);
	sqlite3_mutex_leave(g_m);
}

int
main(void)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t opts = { 0 };
	xtc_pid_t a, b;
	int rc;

	rc = sqlite3_config(SQLITE_CONFIG_MUTEX, mutex_methods());
	if (rc != SQLITE_OK) { fprintf(stderr, "config(MUTEX)=%d\n", rc); return 1; }
	rc = sqlite3_config(SQLITE_CONFIG_SERIALIZED);
	if (rc != SQLITE_OK) { fprintf(stderr, "config(SERIALIZED)=%d\n", rc); return 1; }
	if ((rc = sqlite3_initialize()) != SQLITE_OK) {
		fprintf(stderr, "sqlite3_initialize=%d\n", rc); return 1;
	}

	g_m = sqlite3_mutex_alloc(SQLITE_MUTEX_RECURSIVE);
	if (g_m == NULL) { fprintf(stderr, "mutex_alloc failed\n"); return 1; }

	if (xtc_loop_init(&loop) != XTC_OK) { fprintf(stderr, "loop_init\n"); return 1; }
	opts.name = "holder";
	if (xtc_proc_spawn(loop, holder_proc, NULL, &opts, &a) != XTC_OK) return 1;
	opts.name = "contender";
	if (xtc_proc_spawn(loop, contender_proc, NULL, &opts, &b) != XTC_OK) return 1;

	/* If the contended enter blocked the loop thread instead of
	 * parking, the holder's 60ms timer would never fire and this
	 * call would hang -- the test would never print a result. */
	if (xtc_loop_run(loop) != XTC_OK) { fprintf(stderr, "loop_run\n"); return 1; }
	(void)xtc_loop_fini(loop);

	sqlite3_mutex_free(g_m);
	(void)sqlite3_shutdown();

	if (!g_a_recursion_ok) {
		fprintf(stderr, "FAIL: recursive enter/leave broke\n");
		return 1;
	}
	if (!(g_a_acquire >= 0 && g_a_release >= 0 && g_b_acquire >= 0)) {
		fprintf(stderr, "FAIL: a proc never ran (acquire=%d release=%d "
		    "b=%d)\n", g_a_acquire, g_a_release, g_b_acquire);
		return 1;
	}
	if (!(g_a_acquire < g_a_release && g_a_release < g_b_acquire)) {
		fprintf(stderr, "FAIL: contender did not wait for holder "
		    "(a_acq=%d a_rel=%d b_acq=%d)\n",
		    g_a_acquire, g_a_release, g_b_acquire);
		return 1;
	}

	printf("  ok   recursive enter/leave from one process\n");
	printf("  ok   contender parked (not thread-blocked): holder held "
	    "across a park and released first (a_acq=%d a_rel=%d b_acq=%d)\n",
	    g_a_acquire, g_a_release, g_b_acquire);
	printf("All sqlxtc mutex concurrency tests passed.\n");
	return 0;
}
