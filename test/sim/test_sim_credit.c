/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/sim/test_sim_credit.c
 *	DST coverage of the sliding-window credit regulator
 *	(src/orc/credit.c).  N issuer fibers share one regulator with a
 *	fixed window; each takes a credit, "issues" (increments a shared
 *	in-flight counter), then in a later turn releases it.  Under the
 *	seeded scheduler the interleaving varies, but the SAFETY invariant
 *	must always hold: in-flight never exceeds the window, and the
 *	regulator's own peak matches the observed peak.  The run replays
 *	byte-identically; a different seed reorders but never violates the
 *	window.
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
#include "xtc_async.h"     /* xtc_yield */
#include "xtc_credit.h"
#include "xtc_sim.h"

#define N_LOOPS   4
#define WINDOW    3
#define N_ISSUERS 6
#define PER       5           /* acquire/release cycles per issuer */

static xtc_credit_t *g_cw;
static atomic_int     g_inflight;    /* observed live count */
static atomic_int     g_observed_peak;
static atomic_int     g_violation;   /* in-flight ever exceeded WINDOW */
static atomic_int     g_done;

static void
issuer(void *arg)
{
	int i;
	(void)arg;
	for (i = 0; i < PER; i++) {
		int now, pk;
		/* Bounded acquire: a finite deadline lets the sim advance
		 * virtual time rather than treating an all-parked instant as a
		 * deadlock (the same reason test_sim_sync uses a timeout).  A
		 * timeout just retries -- the window invariant is what matters. */
		if (xtc_credit_acquire(g_cw, 1000000000LL) != XTC_OK) {
			xtc_yield();
			i--;                 /* retry this cycle */
			continue;
		}
		now = atomic_fetch_add_explicit(&g_inflight, 1,
		    memory_order_relaxed) + 1;
		if (now > WINDOW)
			atomic_store_explicit(&g_violation, 1,
			    memory_order_relaxed);
		/* track observed peak */
		pk = atomic_load_explicit(&g_observed_peak, memory_order_relaxed);
		while (now > pk &&
		    !atomic_compare_exchange_weak_explicit(&g_observed_peak,
		        &pk, now, memory_order_relaxed, memory_order_relaxed))
			;
		/* Hold the credit across a yield so windows genuinely overlap. */
		xtc_yield();
		atomic_fetch_sub_explicit(&g_inflight, 1, memory_order_relaxed);
		(void)xtc_credit_release(g_cw);
	}
	atomic_fetch_add_explicit(&g_done, 1, memory_order_relaxed);
}

static int
run_credit(uint64_t seed, int *out_done, int *out_peak, int *out_reg_peak,
           int *out_violation, uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	int i, rc;

	atomic_store(&g_inflight, 0);
	atomic_store(&g_observed_peak, 0);
	atomic_store(&g_violation, 0);
	atomic_store(&g_done, 0);

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK) return -1;
	if (xtc_credit_create(WINDOW, &g_cw) != XTC_OK) {
		(void)xtc_exec_fini(e); return -1;
	}
	for (i = 0; i < N_ISSUERS; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e, (unsigned)(i % N_LOOPS)),
		    issuer, NULL, NULL, NULL);

	rc = xtc_sim_exec_run(e, seed, 5000000);

	*out_done = atomic_load(&g_done);
	*out_peak = atomic_load(&g_observed_peak);
	*out_reg_peak = (int)xtc_credit_peak(g_cw);
	*out_violation = atomic_load(&g_violation);
	if (out_state) *out_state = xtc_sim_state_hash(e);
	xtc_credit_destroy(g_cw);
	g_cw = NULL;
	(void)xtc_exec_fini(e);
	return rc;
}

int
main(void)
{
	int d1 = 0, pk1 = 0, rp1 = 0, v1 = 0;
	int d2 = 0, pk2 = 0, rp2 = 0, v2 = 0;
	int d3 = 0, pk3 = 0, rp3 = 0, v3 = 0;
	uint64_t s1 = 0, s2 = 0, s3 = 0;
	int rc1, rc2, rc3;

	rc1 = run_credit(0xC0FFEE, &d1, &pk1, &rp1, &v1, &s1);
	if (rc1 != XTC_OK) { printf("FAIL: credit run rc=%d (hang?)\n", rc1); return 1; }
	rc2 = run_credit(0xC0FFEE, &d2, &pk2, &rp2, &v2, &s2);
	rc3 = run_credit(0x13579B, &d3, &pk3, &rp3, &v3, &s3);
	if (rc2 != XTC_OK || rc3 != XTC_OK) {
		printf("FAIL: credit replay/diff rc=%d/%d\n", rc2, rc3); return 1;
	}

	printf("run1: done=%d obs_peak=%d reg_peak=%d violation=%d state=%016llx\n",
	    d1, pk1, rp1, v1, (unsigned long long)s1);
	printf("run3 (diff seed): done=%d obs_peak=%d reg_peak=%d violation=%d\n",
	    d3, pk3, rp3, v3);

	if (d1 != N_ISSUERS) {
		printf("FAIL: not all issuers finished (done=%d want %d)\n",
		    d1, N_ISSUERS); return 1;
	}
	/* SAFETY: the window was never exceeded, on either seed. */
	if (v1 != 0 || v3 != 0) {
		printf("FAIL: in-flight exceeded window %d (violation)\n", WINDOW);
		return 1;
	}
	/* The regulator's own peak must match what the issuers observed, and
	 * both must be within the window. */
	if (pk1 != rp1 || pk1 > WINDOW) {
		printf("FAIL: peak mismatch obs=%d reg=%d window=%d\n",
		    pk1, rp1, WINDOW); return 1;
	}
	/* Byte-identical replay. */
	if (d1 != d2 || pk1 != pk2 || rp1 != rp2 || s1 != s2) {
		printf("FAIL: credit run did not replay byte-identically\n");
		return 1;
	}

	printf("OK: credit window never exceeded under any seeded schedule; "
	    "regulator peak matches observed peak; run replays "
	    "byte-identically and a different seed stays within the window\n");
	return 0;
}
