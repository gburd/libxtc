/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * SPDX-License-Identifier: ISC
 *
 * The page provider writes nothing while the free list holds its
 * target.  Its tick cooled HOT frames until the CLEAN COOL supply
 * reached cool_target and flushed every dirty COOL frame, whatever the
 * free list held.  With a pool larger than the store (the in-process
 * ledger: 262144 frames, a 489 MB store, 143k frames free, 0 evictions)
 * it cycled the whole store through cool -> write -> re-dirty -> hot:
 * 7k-42k buffered page writes a second that nothing needed, which the
 * kernel's 30 s dirty expiry wrote back in one ~550 MB burst, and the
 * log's O_DSYNC commits queued behind it (open-loop p99.9 13-20 ms
 * instead of 2.5 ms).  An eviction takes the free list first, so while
 * free_n >= cool_target no cool or clean frame can be needed.
 *
 *   (a) free list at its target, every resident page HOT and dirty:
 *       ticks over the whole pool cool 0 frames and write 0 pages;
 *   (b) free list drained below the target: the tick cools and writes
 *       again (the provider still refills it).
 * Includes the implementation for the static pp_tick.
 */

#include "bufmgr.h"
#include "bufmgr.c"
#include "t_tmp.h"

#define NF 4096                 /* frames; cool_target 10% = 409 */

static int
ticks(bm_t *bm, uint32_t *cursor, int n)
{
	int t;
	for (t = 0; t < n; t++)
		(void)pp_tick(bm, cursor);
	return t;
}

int
main(void)
{
	bm_opts_t bo = BM_OPTS_DEFAULT;
	char path[256];
	bm_t *bm = NULL;
	bm_frame_t *f;
	bm_stats_t s0, s1;
	bm_pid_t pid;
	uint32_t cursor = 0, used, i;
	int fd;

	t_tmpl(path, sizeof path, "sqlxtc-pp-idle");
	if ((fd = mkstemp(path)) < 0) return 1;
	close(fd);
	bo.path = path;
	bo.n_frames = NF;
	if (bm_create(&bo, &bm) != XTC_OK) { unlink(path); return 1; }

	/* (a) Half the pool in use, HOT and dirty; the rest free (>= target). */
	used = NF / 2;
	for (i = 0; i < used; i++) {
		CK(bm_alloc_pid(bm, &f, &pid) == XTC_OK);
		memset(bm_page(f), (int)(i & 0xff), bm->page_size);
		bm_unfix(bm, f, 1);
	}
	CK(atomic_load(&bm->free_n) >= bm->cool_target);
	bm_get_stats(bm, &s0);
	(void)ticks(bm, &cursor, 4 * ((NF + PP_SLICE - 1) / PP_SLICE));
	bm_get_stats(bm, &s1);
	printf("  (a) free %u >= target %u: cooled %llu, flushed %llu\n",
	    atomic_load(&bm->free_n), bm->cool_target,
	    (unsigned long long)(s1.cooled - s0.cooled),
	    (unsigned long long)(s1.flushed - s0.flushed));
	CK(s1.cooled == s0.cooled);
	CK(s1.flushed == s0.flushed);

	/* (b) Drain the free list to half its target: the provider works. */
	while (atomic_load(&bm->free_n) > bm->cool_target / 2) {
		if (bm_alloc_pid(bm, &f, &pid) != XTC_OK) break;
		memset(bm_page(f), 0x77, bm->page_size);
		bm_unfix(bm, f, 1);
	}
	CK(atomic_load(&bm->free_n) < bm->cool_target);
	bm_get_stats(bm, &s0);
	(void)ticks(bm, &cursor, 4 * ((NF + PP_SLICE - 1) / PP_SLICE));
	bm_get_stats(bm, &s1);
	printf("  (b) free below target: cooled %llu, flushed %llu, free %u\n",
	    (unsigned long long)(s1.cooled - s0.cooled),
	    (unsigned long long)(s1.flushed - s0.flushed),
	    atomic_load(&bm->free_n));
	CK(s1.cooled > s0.cooled);
	CK(s1.flushed > s0.flushed);
	CK(atomic_load(&bm->free_n) >= bm->cool_target);   /* refilled */

	bm_destroy(bm);
	unlink(path);
	if (g_fail) { fprintf(stderr, "FAIL: test_pp_idle_writes\n"); return 1; }
	printf("  ok   the page provider writes nothing while the free list "
	    "holds its target\n");
	return 0;
}
