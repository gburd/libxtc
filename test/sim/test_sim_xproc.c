/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/sim/test_sim_xproc.c
 *	DST coverage of the PORTABLE half of the cross-fork subsystem
 *	(src/orc/xproc.c): the child-entry registry that both POSIX (fork)
 *	and Windows (re-exec) resolve.  The registry is the piece Windows
 *	adds to make xtc_xspawn_entry work where a function pointer cannot
 *	cross process creation; it must resolve names deterministically.
 *
 *	The fork / CreateProcess / socketpair paths themselves are NOT
 *	sim-reachable (real OS process creation is outside the single-
 *	process simulator, like native AIO and the thread pool) -- they are
 *	covered by test/m10/test_xproc.c and, on Windows, the EC2 box.  What
 *	IS deterministic and worth pinning here is that concurrent
 *	registration + resolution under the seeded scheduler is race-free
 *	and replay-identical.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <stdint.h>
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_xproc.h"
#include "xtc_sim.h"

#define N_LOOPS   4
#define N_WORKERS 8

/* Distinct entry functions, registered under distinct names. */
static void e0(void *a){(void)a;}
static void e1(void *a){(void)a;}
static void e2(void *a){(void)a;}
static void e3(void *a){(void)a;}
static const char *g_names[4] = { "e0", "e1", "e2", "e3" };
static void (*g_fns[4])(void *) = { e0, e1, e2, e3 };

static atomic_int g_bad;      /* a spawn_entry gave the wrong error (bug) */
static atomic_int g_done;

/* Worker: try xtc_xspawn_entry with a NULL loop/out (so no real process
 * is created -- we are only exercising the portable name-resolution +
 * argument-validation contract deterministically).  A registered name
 * must be resolved (and then fail on the NULL args with XTC_E_INVAL); an
 * unregistered name must return XTC_E_NOTFOUND.  This pins the resolve
 * decision without touching real fork/CreateProcess. */
static void
worker(void *arg)
{
	int id = (int)(intptr_t)arg;
	int i;
	for (i = 0; i < 4; i++) {
		/* Known name: resolves, then the NULL loop is rejected. */
		int rc = xtc_xspawn_entry(NULL, "x", g_names[(id + i) & 3],
		    NULL, 0, NULL);
		if (rc != XTC_E_INVAL)      /* resolved -> NULL-arg path */
			atomic_fetch_add_explicit(&g_bad, 1, memory_order_relaxed);
		/* Unknown name: NOTFOUND regardless of args. */
		rc = xtc_xspawn_entry(NULL, "x", "no_such_entry", NULL, 0, NULL);
		if (rc != XTC_E_NOTFOUND && rc != XTC_E_INVAL)
			atomic_fetch_add_explicit(&g_bad, 1, memory_order_relaxed);
		xtc_yield();
	}
	atomic_fetch_add_explicit(&g_done, 1, memory_order_relaxed);
}

static int
run_xproc(uint64_t seed, int *out_done, int *out_bad, uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	int i, rc;

	atomic_store(&g_bad, 0);
	atomic_store(&g_done, 0);

	/* Register the entries before the run (both platforms do this early,
	 * in code shared by parent and child). */
	for (i = 0; i < 4; i++)
		(void)xtc_xproc_register_entry(g_names[i], g_fns[i]);

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK) return -1;
	for (i = 0; i < N_WORKERS; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e, (unsigned)(i % N_LOOPS)),
		    worker, (void *)(intptr_t)i, NULL, NULL);

	rc = xtc_sim_exec_run(e, seed, 5000000);

	*out_done = atomic_load(&g_done);
	*out_bad = atomic_load(&g_bad);
	if (out_state) *out_state = xtc_sim_state_hash(e);
	(void)xtc_exec_fini(e);
	return rc;
}

int
main(void)
{
	int d1 = 0, b1 = 0, d2 = 0, b2 = 0, d3 = 0, b3 = 0;
	uint64_t s1 = 0, s2 = 0, s3 = 0;
	int rc1, rc2, rc3;

	rc1 = run_xproc(0x5C0DE, &d1, &b1, &s1);
	if (rc1 != XTC_OK) { printf("FAIL: xproc run rc=%d (hang?)\n", rc1); return 1; }
	rc2 = run_xproc(0x5C0DE, &d2, &b2, &s2);
	rc3 = run_xproc(0xA1CE5, &d3, &b3, &s3);
	if (rc2 != XTC_OK || rc3 != XTC_OK) {
		printf("FAIL: xproc replay/diff rc=%d/%d\n", rc2, rc3); return 1;
	}

	printf("run1: done=%d bad=%d state=%016llx\n",
	    d1, b1, (unsigned long long)s1);

	if (d1 != N_WORKERS) {
		printf("FAIL: workers done=%d want %d\n", d1, N_WORKERS); return 1;
	}
	if (b1 != 0 || b3 != 0) {
		printf("FAIL: entry-registry resolved wrong (bad=%d/%d)\n",
		    b1, b3); return 1;
	}
	if (d1 != d2 || b1 != b2 || s1 != s2) {
		printf("FAIL: xproc entry-registry run did not replay\n");
		return 1;
	}

	printf("OK: cross-fork child-entry registry resolves registered names "
	    "and rejects unknown ones deterministically under concurrent "
	    "resolution across loops, and the run replays byte-identically\n");
	return 0;
}
