/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_scan_resist.c
 *	Scan resistance: a large scan must not evict the hot working set.
 *
 *	Scan resistance comes from the cooling stage used as a
 *	probationary FIFO (the 2Q insight, Johnson & Shasha 1994): a
 *	demand-LOADED page is admitted COOL, not HOT, and is promoted to
 *	HOT only on a SECOND access (a rescue).  A scan touches each page
 *	once, so its pages stay COOL and are evicted without ever
 *	displacing the hot set -- so it does not depend on a CLOCK
 *	reference bit getting the recency ordering right under a flood.
 *
 *	This test warms a hot set, runs a scan many times larger than the
 *	pool, then re-touches the hot set and counts how many pages had to
 *	be reloaded.  With scan resistance on, the hot set survives (near
 *	zero reloads); with it off (legacy admit-HOT), the scan trashes
 *	the pool.  Off a loop -- demand eviction with synchronous I/O.
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
#define N_FRAMES  32
#define HOT       16          /* hot working set (fits with room to spare) */
#define COLD      3000        /* scan set, many times the pool */

static bm_pid_t  g_pid[HOT + COLD];

static void
fill_page(void *p, bm_pid_t pid, uint64_t k)
{
	uint64_t *u = p;
	u[0] = pid; u[1] = k;
	((unsigned char *)p)[PAGE_SZ - 1] = (unsigned char)(k & 0xff);
}
static int
check_page(const void *p, bm_pid_t pid, uint64_t k)
{
	const uint64_t *u = p;
	return u[0] == pid && u[1] == k &&
	    ((const unsigned char *)p)[PAGE_SZ - 1] == (unsigned char)(k & 0xff);
}

/* Returns the number of hot-set pages that had to be RELOADED after the
 * scan (0 == the scan did not disturb the hot set), or UINT64_MAX on
 * error. */
static uint64_t
run(int scan_resist)
{
	bm_opts_t bo = BM_OPTS_DEFAULT;
	char path[256]; t_tmpl(path, sizeof path, "sqlxtc-scan");
	bm_t *bm = NULL;
	bm_frame_t *f;
	bm_stats_t before, after;
	int k, r, fd;
	uint64_t reloads;

	fd = mkstemp(path); if (fd < 0) return UINT64_MAX; close(fd);
	bo.path = path; bo.page_size = PAGE_SZ; bo.n_frames = N_FRAMES;
	bo.cool_pct = 25; bo.scan_resist = scan_resist;
	if (bm_create(&bo, &bm) != XTC_OK) { unlink(path); return UINT64_MAX; }

	/* Allocate every page (the pool overflows immediately). */
	for (k = 0; k < HOT + COLD; k++) {
		if (bm_alloc_pid(bm, &f, &g_pid[k]) != XTC_OK) goto err;
		fill_page(bm_page(f), g_pid[k], (uint64_t)k);
		bm_unfix(bm, f, 1);
	}
	/* Warm the hot set: repeated access promotes it to HOT and resident
	 * (the second access rescues each from COOL). */
	for (r = 0; r < 12; r++)
		for (k = 0; k < HOT; k++) {
			if (bm_fix_pid(bm, g_pid[k], &f) != XTC_OK) goto err;
			if (!check_page(bm_page(f), g_pid[k], (uint64_t)k)) goto err;
			bm_unfix(bm, f, 0);
		}
	/* Scan the cold set once each -- many times the pool size. */
	for (k = HOT; k < HOT + COLD; k++) {
		if (bm_fix_pid(bm, g_pid[k], &f) != XTC_OK) goto err;
		if (!check_page(bm_page(f), g_pid[k], (uint64_t)k)) goto err;
		bm_unfix(bm, f, 0);
	}
	/* Re-touch the hot set; count how many pages the scan evicted. */
	bm_get_stats(bm, &before);
	for (k = 0; k < HOT; k++) {
		if (bm_fix_pid(bm, g_pid[k], &f) != XTC_OK) goto err;
		if (!check_page(bm_page(f), g_pid[k], (uint64_t)k)) goto err;
		bm_unfix(bm, f, 0);
	}
	bm_get_stats(bm, &after);
	reloads = after.loads - before.loads;
	bm_destroy(bm); unlink(path);
	return reloads;
err:
	bm_destroy(bm); unlink(path);
	return UINT64_MAX;
}

int
main(void)
{
	uint64_t resist, legacy;

	resist = run(1);
	legacy = run(0);
	if (resist == UINT64_MAX || legacy == UINT64_MAX) {
		fprintf(stderr, "FAIL: workload error\n");
		return 1;
	}

	/* The scan (COLD == %d pages through %d frames) must not evict the
	 * hot set under scan resistance: near-zero hot reloads.  Legacy
	 * admit-HOT lets the scan trash it. */
	if (resist > HOT / 4) {
		fprintf(stderr, "FAIL: scan resistance did not protect the hot set "
		    "(%llu of %d hot pages reloaded after the scan)\n",
		    (unsigned long long)resist, HOT);
		return 1;
	}
	if (legacy <= resist) {
		fprintf(stderr, "FAIL: expected the legacy policy to be trashed by "
		    "the scan (legacy reloads=%llu, resist reloads=%llu)\n",
		    (unsigned long long)legacy, (unsigned long long)resist);
		return 1;
	}

	printf("  ok   scan resistance: a %d-page scan through a %d-frame pool "
	    "reloaded %llu/%d hot pages with probation on, %llu/%d with it off\n",
	    COLD, N_FRAMES, (unsigned long long)resist, HOT,
	    (unsigned long long)legacy, HOT);
	printf("All sqlxtc scan-resistance tests passed.\n");
	return 0;
}
