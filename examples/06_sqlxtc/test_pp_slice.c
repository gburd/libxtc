/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * SPDX-License-Identifier: ISC
 *
 * The page provider's tick is bounded: each one visits at most
 * 2 x PP_SLICE frames (pass 1 + pass 2) whatever the pool size, and
 * successive ticks resume where the last stopped, so every frame is
 * still visited within ceil(n_frames / PP_SLICE) ticks.  Before 1.58
 * every 1 ms tick walked all n_frames on loop 0 (the WAL writer's loop):
 * 47% of that loop at 262144 frames, halving TPC-C at one connection.
 * Includes the implementation for the static pp_tick.
 */
#include "bufmgr.h"
#include "bufmgr.c"
#include "t_tmp.h"

static int
run(uint32_t n_frames)
{
	bm_opts_t bo = BM_OPTS_DEFAULT;
	char path[256];
	bm_t *bm = NULL;
	uint32_t cursor = 0, v, t, ticks, max_v = 0;
	uint64_t total = 0;
	int bad = 0, fd;
	t_tmpl(path, sizeof path, "sqlxtc-pp-slice");
	if ((fd = mkstemp(path)) < 0) return 1;
	close(fd);
	bo.path = path;
	bo.n_frames = n_frames;
	if (bm_create(&bo, &bm) != XTC_OK) { unlink(path); return 1; }
	/* A fresh pool is all free, and a tick with the free list at its
	 * target does nothing (test_pp_idle_writes): ask for more than the
	 * pool so every tick runs its passes. */
	bm->cool_target = n_frames + 1;
	ticks = (n_frames + PP_SLICE - 1) / PP_SLICE;
	for (t = 0; t < ticks; t++) {
		v = pp_tick(bm, &cursor);
		if (v > max_v) max_v = v;
		total += v;
	}
	/* Bound: one tick never exceeds both passes' slices. */
	if (max_v > 2u * PP_SLICE) {
		fprintf(stderr, "FAIL n=%u: a tick visited %u > %u frames\n",
		    n_frames, max_v, 2u * PP_SLICE);
		bad = 1;
	}
	/* Coverage: ticks resume, so ceil(n/slice) ticks visit >= n frames
	 * (pass 1 alone covers the pool once). */
	if (total < n_frames) {
		fprintf(stderr, "FAIL n=%u: %u ticks visited only %llu frames\n",
		    n_frames, ticks, (unsigned long long)total);
		bad = 1;
	}
	bm_destroy(bm);
	unlink(path);
	return bad;
}

int
main(void)
{
	static const uint32_t ns[] = { 16, 256, 8191, 8192, 8193, 65536, 262144 };
	size_t i;
	int fails = 0;
	for (i = 0; i < sizeof ns / sizeof ns[0]; i++)
		fails += run(ns[i]);
	if (fails) { fprintf(stderr, "FAIL: test_pp_slice\n"); return 1; }
	printf("  ok   page-provider tick bounded to 2 x %u frames, full coverage "
	    "(%zu pool sizes)\n", PP_SLICE, sizeof ns / sizeof ns[0]);
	return 0;
}
