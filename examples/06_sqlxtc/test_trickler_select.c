/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * SPDX-License-Identifier: ISC
 *
 * The trickler's priority selection (COOL first, then oldest-dirtied)
 * keeps exactly the batch a full sort would pick, at O(n log k).  It
 * used to qsort every dirty candidate (up to n_frames) each 5 ms pass to
 * take 16: with a 64k-frame pool at TPC-C W=4 C=32 that sort was ~49%
 * of loop 0, the loop that also runs the WAL group-commit writer.
 * Includes the implementation for the static selection helpers.
 */
#include "bufmgr.h"
#include "bufmgr.c"
#include "t_tmp.h"

static int
check(unsigned seed, int n, int k)
{
	struct tr_cand *all = calloc((size_t)(n > 0 ? n : 1), sizeof *all);
	struct tr_cand *sel = calloc((size_t)(k > 0 ? k : 1), sizeof *sel);
	unsigned x = seed;
	int i, ns = 0, want, bad = 0;
	if (all == NULL || sel == NULL) { free(all); free(sel); return 1; }
	for (i = 0; i < n; i++) {
		x = x * 1103515245u + 12345u;
		all[i].f = (bm_frame_t *)(uintptr_t)(i + 1);
		all[i].cool = (uint8_t)((x >> 16) & 1u);
		all[i].seq = (x >> 8) % (unsigned)(n / 2 + 1);   /* with ties */
		tr_topk_offer(sel, &ns, k, &all[i]);
	}
	qsort(all, (size_t)n, sizeof *all, tr_cmp);
	qsort(sel, (size_t)ns, sizeof *sel, tr_cmp);
	want = n < k ? n : k;
	if (ns != want) bad = 1;
	for (i = 0; !bad && i < want; i++)
		if (tr_cmp(&sel[i], &all[i]) != 0) bad = 1;   /* same keys, in order */
	free(all); free(sel);
	return bad;
}

int
main(void)
{
	static const int ns[] = { 0, 1, 5, 16, 17, 100, 4096, 65536 };
	static const int ks[] = { 1, 16, 256 };
	unsigned seed;
	size_t a, b;
	int fails = 0, cases = 0;
	for (seed = 1; seed <= 20; seed++)
		for (a = 0; a < sizeof ns / sizeof ns[0]; a++)
			for (b = 0; b < sizeof ks / sizeof ks[0]; b++) {
				cases++;
				if (check(seed, ns[a], ks[b])) {
					fprintf(stderr, "FAIL seed %u n %d k %d\n", seed,
					    ns[a], ks[b]);
					fails++;
				}
			}
	if (fails) { fprintf(stderr, "FAIL: test_trickler_select\n"); return 1; }
	printf("  ok   trickler batch selection == full sort, %d cases\n", cases);
	return 0;
}
