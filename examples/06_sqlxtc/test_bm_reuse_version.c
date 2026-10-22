/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_bm_reuse_version.c
 *	A frame that gets a NEW page must change its version, so a pin-free
 *	optimistic reader (bm_fix_pid_nopin + bm_read_begin/bm_read_valid:
 *	the BM_EPOCH_FASTPATH read) that sampled the version before the
 *	reuse fails its validation instead of accepting bytes read across
 *	the reuse.
 *
 *	The load path (bm_fix_pid miss) bumped the version by 2; the
 *	allocation path (bm_alloc_pid: a free frame -- or, after quarantine
 *	drains, the very same pid in the very same frame -- zeroed for a new
 *	page) did not.  Deterministic, one thread: alloc page A, write a
 *	pattern, unfix; a "reader" takes the pin-free frame pointer and
 *	samples the version; free A, drain the quarantine, allocate until
 *	the same frame is handed out again (same pid: the freelist reissues
 *	it); the reader's bm_read_valid must now FAIL.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bufmgr.h"
#include "xtc.h"
#include "t_tmp.h"

int
main(void)
{
	char path[256];
	bm_opts_t o = BM_OPTS_DEFAULT;
	bm_t *bm = NULL;
	bm_frame_t *f = NULL, *g = NULL, *seen;
	bm_pid_t pa = 0, pb = 0;
	uint64_t v = 0;
	int fd, i, same = 0, fail = 0;

	t_tmpl(path, sizeof path, "sqlxtc-bmreuse");
	if ((fd = mkstemp(path)) < 0) return 1;
	close(fd);
	o.path = path; o.page_size = 4096; o.n_frames = 8;
	if (bm_create(&o, &bm) != XTC_OK) return 1;

	if (bm_alloc_pid(bm, &f, &pa) != XTC_OK) return 1;
	memset(bm_page(f), 0xAB, 4096);
	bm_unfix(bm, f, 1);

	/* The pin-free reader: frame pointer + version, no pin. */
	seen = bm_fix_pid_nopin(bm, pa);
	if (seen == NULL || !bm_read_begin(seen, &v)) {
		fprintf(stderr, "FAIL: setup (nopin %p)\n", (void *)seen);
		return 1;
	}

	/* The page is freed and its id (and frame) handed out again. */
	if (bm_free_pid(bm, pa) != XTC_OK) return 1;
	bm_reclaim_quarantine(bm);
	for (i = 0; i < 16 && !same; i++) {
		if (bm_alloc_pid(bm, &g, &pb) != XTC_OK) break;
		same = g == seen;
		bm_unfix(bm, g, 1);
	}
	if (!same) {
		fprintf(stderr, "  note: the freed frame was not reissued in 16 "
		    "allocations; nothing to check\n");
	} else {
		/* Bytes read across the reuse would be the new (zeroed) page
		 * under pid %pb == %pa: validation must reject them. */
		printf("  frame reissued (pid %llu -> %llu): version %s\n",
		    (unsigned long long)pa, (unsigned long long)pb,
		    bm_read_valid(seen, v) ? "UNCHANGED" : "changed");
		if (bm_read_valid(seen, v)) {
			fprintf(stderr, "FAIL: a reused frame kept its version: a "
			    "pin-free reader validates bytes of the new page\n");
			fail = 1;
		}
	}
	bm_destroy(bm);
	unlink(path);
	if (fail || !same) {
		fprintf(stderr, "FAIL: test_bm_reuse_version\n");
		return 1;
	}
	printf("  ok   a frame reused for a new page changes its version\n");
	return 0;
}
