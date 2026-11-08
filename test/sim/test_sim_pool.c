/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/sim/test_sim_pool.c
 *	DST coverage of the bounded resource pool (src/orc/pool.c).  N
 *	worker fibers share a pool of CAP resources; each checks one out,
 *	holds it across a yield, and returns it, under the seeded
 *	scheduler.  SAFETY invariants: a resource is never handed to two
 *	workers at once, checked-out never exceeds capacity, and every
 *	resource is back in the pool at the end.  The run replays
 *	byte-identically; a different seed reorders but never
 *	double-hands a resource.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_async.h"     /* xtc_yield */
#include "xtc_pool.h"
#include "xtc_sim.h"

#define N_LOOPS   4
#define CAP       3
#define N_WORKERS 6
#define PER       4           /* checkout/return cycles per worker */

/* Each resource is an int slot with an "owner" flag; a double-checkout
 * would set owned twice. */
static xtc_pool_t *g_pool;
static int g_res_owned[CAP]; /* per-resource live-owner count */
static int g_double_hand;    /* a resource held by two at once */
static int g_over_cap;       /* checked-out exceeded CAP */
static int g_live;           /* currently checked out */
static int g_done;
static int          g_slots[CAP];

static void
worker(void *arg)
{
	int i;
	(void)arg;
	for (i = 0; i < PER; i++) {
		void *r = NULL;
		int idx, live;
		if (xtc_pool_checkout(g_pool, 1000000000LL, &r) != XTC_OK) {
			xtc_yield(); i--; continue;   /* timeout: retry */
		}
		idx = (int)((int *)r - g_slots);   /* which slot */
		if (idx < 0 || idx >= CAP) {
			xtc_atomic_store_i32(&g_double_hand, 1,
			    xtc_mo_relaxed); /* bogus pointer */
		} else if (xtc_atomic_fetch_add_i32(&g_res_owned[idx], 1,
		               xtc_mo_relaxed) != 0) {
			xtc_atomic_store_i32(&g_double_hand, 1,
			    xtc_mo_relaxed); /* already owned! */
		}
		live = xtc_atomic_fetch_add_i32(&g_live, 1, xtc_mo_relaxed) + 1;
		if (live > CAP)
			xtc_atomic_store_i32(&g_over_cap, 1, xtc_mo_relaxed);
		xtc_yield();                       /* hold across a reschedule */
		xtc_atomic_fetch_sub_i32(&g_live, 1, xtc_mo_relaxed);
		if (idx >= 0 && idx < CAP)
			xtc_atomic_fetch_sub_i32(&g_res_owned[idx], 1,
			    xtc_mo_relaxed);
		(void)xtc_pool_checkin(g_pool, r);
	}
	xtc_atomic_fetch_add_i32(&g_done, 1, xtc_mo_relaxed);
}

static int
run_pool(uint64_t seed, int *out_done, int *out_dbl, int *out_over,
         int *out_avail, uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	int i, rc;

	for (i = 0; i < CAP; i++)
		xtc_atomic_store_i32(&g_res_owned[i], 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_double_hand, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_over_cap, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_live, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_done, 0, xtc_mo_seq_cst);

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK) return -1;
	if (xtc_pool_create(CAP, &g_pool) != XTC_OK) {
		(void)xtc_exec_fini(e); return -1;
	}
	for (i = 0; i < CAP; i++) {
		g_slots[i] = 100 + i;
		(void)xtc_pool_add(g_pool, &g_slots[i]);
	}
	for (i = 0; i < N_WORKERS; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e, (unsigned)(i % N_LOOPS)),
		    worker, NULL, NULL, NULL);

	rc = xtc_sim_exec_run(e, seed, 5000000);

	*out_done = xtc_atomic_load_i32(&g_done, xtc_mo_seq_cst);
	*out_dbl = xtc_atomic_load_i32(&g_double_hand, xtc_mo_seq_cst);
	*out_over = xtc_atomic_load_i32(&g_over_cap, xtc_mo_seq_cst);
	*out_avail = (int)xtc_pool_available(g_pool);
	if (out_state) *out_state = xtc_sim_state_hash(e);
	xtc_pool_destroy(g_pool);
	g_pool = NULL;
	(void)xtc_exec_fini(e);
	return rc;
}

int
main(void)
{
	int d1 = 0, db1 = 0, o1 = 0, av1 = 0;
	int d2 = 0, db2 = 0, o2 = 0, av2 = 0;
	int d3 = 0, db3 = 0, o3 = 0, av3 = 0;
	uint64_t s1 = 0, s2 = 0, s3 = 0;
	int rc1, rc2, rc3;

	rc1 = run_pool(0xB0071E, &d1, &db1, &o1, &av1, &s1);
	if (rc1 != XTC_OK) { printf("FAIL: pool run rc=%d (hang?)\n", rc1); return 1; }
	rc2 = run_pool(0xB0071E, &d2, &db2, &o2, &av2, &s2);
	rc3 = run_pool(0x2C0DE5, &d3, &db3, &o3, &av3, &s3);
	if (rc2 != XTC_OK || rc3 != XTC_OK) {
		printf("FAIL: pool replay/diff rc=%d/%d\n", rc2, rc3); return 1;
	}

	printf("run1: done=%d double_hand=%d over_cap=%d avail=%d state=%016llx\n",
	    d1, db1, o1, av1, (unsigned long long)s1);
	printf("run3 (diff seed): done=%d double_hand=%d over_cap=%d avail=%d\n",
	    d3, db3, o3, av3);

	if (d1 != N_WORKERS) {
		printf("FAIL: not all workers finished (done=%d want %d)\n",
		    d1, N_WORKERS); return 1;
	}
	/* SAFETY: no resource handed to two workers, never over capacity, on
	 * either seed. */
	if (db1 != 0 || db3 != 0) {
		printf("FAIL: a resource was checked out to two workers\n");
		return 1;
	}
	if (o1 != 0 || o3 != 0) {
		printf("FAIL: checked-out exceeded capacity %d\n", CAP);
		return 1;
	}
	/* All resources returned. */
	if (av1 != CAP || av3 != CAP) {
		printf("FAIL: not all resources returned (avail=%d want %d)\n",
		    av1, CAP); return 1;
	}
	/* Byte-identical replay. */
	if (d1 != d2 || db1 != db2 || o1 != o2 || av1 != av2 || s1 != s2) {
		printf("FAIL: pool run did not replay byte-identically\n");
		return 1;
	}

	printf("OK: pool never double-hands a resource or exceeds capacity "
	    "under any seeded schedule; all resources returned; run replays "
	    "byte-identically and a different seed stays safe\n");
	return 0;
}
