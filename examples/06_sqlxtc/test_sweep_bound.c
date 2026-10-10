/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_sweep_bound.c
 *	The eviction sweep must do BOUNDED work per allocation.
 *
 *	evict_one's only exit is a frame that is COOL, clean, unpinned and
 *	not recently referenced.  Cooling a HOT frame and clearing a
 *	reference bit both count as progress, so in a HOT pool -- every
 *	resident page re-touched more often than the hand comes round --
 *	one allocation can drive the hand around the whole pool several
 *	times before it finds a victim.  That is the pathology the
 *	PostgreSQL bounded-clock-sweep work measured (worst case exactly
 *	one full pass of shared_buffers, scaling linearly with the pool)
 *	and fixed by claiming the next unpinned frame outright once a call
 *	has made `claim_threshold` non-productive advances.
 *
 *	This test makes every resident page HOT and referenced, then
 *	allocates once per round, re-referencing the whole pool between
 *	allocations, and asserts the WORST-CASE advances of any single
 *	allocation (bm_stats_t.adv_max):
 *	  - bounded arm (claim_threshold = 64): adv_max <= 64 + slack, and
 *	    the bound actually fired (forced_claims > 0);
 *	  - unbounded arm (claim_threshold = 0, the legacy behaviour):
 *	    adv_max >= the pool size -- proving the pathology is real and
 *	    the test is not vacuous.
 *	Off a loop -- demand eviction with synchronous I/O.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bufmgr.h"
#include "xtc.h"
#include "t_tmp.h"

#define PAGE_SZ   4096
#define N_FRAMES  512
#define N_PAGES   (N_FRAMES * 2)   /* dataset twice the pool */
#define ROUNDS    64
#define THETA     64
#define SLACK     4                /* the claim itself + a re-validate */

static bm_pid_t g_pid[N_PAGES];

/* Touch every resident page twice: the first fix rescues a COOL page to
 * HOT, the second sets its CLOCK reference bit, so the whole pool is HOT
 * and referenced when the next allocation sweeps. */
static int
heat_pool(bm_t *bm, int first, int n)
{
	bm_frame_t *f;
	int k, r;
	for (r = 0; r < 2; r++)
		for (k = first; k < first + n; k++) {
			if (bm_fix_pid(bm, g_pid[k], &f) != XTC_OK)
				return -1;
			if (((const unsigned char *)bm_page(f))[0] != (k & 0xff)) {
				bm_unfix(bm, f, 0);
				return -1;
			}
			bm_unfix(bm, f, 0);
		}
	return 0;
}

static int
run(uint32_t theta, bm_stats_t *out)
{
	bm_opts_t bo = BM_OPTS_DEFAULT;
	char path[256];
	bm_t *bm = NULL;
	bm_frame_t *f;
	int fd, k, r, window;

	t_tmpl(path, sizeof path, "sqlxtc-sweep");
	fd = mkstemp(path);
	if (fd < 0) return -1;
	close(fd);
	bo.path = path;
	bo.page_size = PAGE_SZ;
	bo.n_frames = N_FRAMES;
	bo.claim_threshold = theta;
	if (bm_create(&bo, &bm) != XTC_OK) { unlink(path); return -1; }

	for (k = 0; k < N_PAGES; k++) {
		if (bm_alloc_pid(bm, &f, &g_pid[k]) != XTC_OK) goto err;
		memset(bm_page(f), (int)(k & 0xff), PAGE_SZ);
		bm_unfix(bm, f, 1);
	}
	/* Write every dirty page out before the read phase. Stats still
	 * include setup, including its forced dirty-page reclaims. */
	if (bm_checkpoint(bm) != XTC_OK) goto err;

	/* A window of pages slightly smaller than the pool: all of it fits
	 * resident, and each round re-heats all of it, then faults in ONE
	 * page from outside it -- an allocation into a hot, full pool. */
	window = N_FRAMES - 8;
	if (heat_pool(bm, 0, window) != 0) goto err;
	for (r = 0; r < ROUNDS; r++) {
		if (heat_pool(bm, 0, window) != 0) goto err;
		if (bm_fix_pid(bm, g_pid[window + r], &f) != XTC_OK) goto err;
		if (((const unsigned char *)bm_page(f))[0] != ((window + r) & 0xff)) {
			bm_unfix(bm, f, 0);
			goto err;
		}
		bm_unfix(bm, f, 0);
	}
	bm_get_stats(bm, out);
	bm_destroy(bm);
	unlink(path);
	return 0;
err:
	bm_destroy(bm);
	unlink(path);
	return -1;
}

int
main(void)
{
	bm_stats_t b, u;

	if (run(THETA, &b) != 0 || run(0, &u) != 0) {
		fprintf(stderr, "FAIL: workload error\n");
		return 1;
	}
	printf("  bounded   (theta=%d): adv_max=%llu allocs=%llu "
	    "advances=%llu forced_claims=%llu\n", THETA,
	    (unsigned long long)b.adv_max, (unsigned long long)b.allocs,
	    (unsigned long long)b.advances,
	    (unsigned long long)b.forced_claims);
	printf("  unbounded (theta=0):  adv_max=%llu allocs=%llu "
	    "advances=%llu forced_claims=%llu\n",
	    (unsigned long long)u.adv_max, (unsigned long long)u.allocs,
	    (unsigned long long)u.advances,
	    (unsigned long long)u.forced_claims);

	if (u.adv_max < N_FRAMES) {
		fprintf(stderr, "FAIL: the unbounded sweep never exceeded one "
		    "pool pass (adv_max=%llu < %d): the workload does not "
		    "reproduce the hot-pool regime, so the test is vacuous\n",
		    (unsigned long long)u.adv_max, N_FRAMES);
		return 1;
	}
	if (u.forced_claims != 0) {
		fprintf(stderr, "FAIL: claim_threshold=0 must never force a "
		    "claim (forced_claims=%llu)\n",
		    (unsigned long long)u.forced_claims);
		return 1;
	}
	if (b.adv_max > THETA + SLACK) {
		fprintf(stderr, "FAIL: bounded sweep exceeded theta+%d "
		    "(adv_max=%llu, theta=%d)\n", SLACK,
		    (unsigned long long)b.adv_max, THETA);
		return 1;
	}
	if (b.forced_claims == 0) {
		fprintf(stderr, "FAIL: the bound never fired in a hot pool\n");
		return 1;
	}
	printf("  ok   sweep bound: worst-case advances per allocation "
	    "%llu (bounded) vs %llu (unbounded) in a %d-frame hot pool\n",
	    (unsigned long long)b.adv_max, (unsigned long long)u.adv_max,
	    N_FRAMES);
	printf("All sqlxtc sweep-bound tests passed.\n");
	return 0;
}
