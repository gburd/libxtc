/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_wal_lsn_monotone.c
 *	LSNs must keep rising across a log checkpoint (compaction).
 *
 *	ARIES ties the log to the data pages through the page LSN: a page
 *	image is redone only if its log record's LSN is newer than the LSN
 *	already stamped on the page (bm_apply_page_image_at gates on exactly
 *	that).  That only works if LSNs never go backwards.  wal_checkpoint
 *	rewrites the log into a compacted file; if it renumbered that file
 *	from 1, every record written after a checkpoint would carry an LSN
 *	SMALLER than the LSNs on pages flushed before it, and in-place
 *	recovery would refuse those records' page images -- silently losing
 *	post-checkpoint changes on any page that was last written before the
 *	checkpoint.
 *
 *	This test commits records, notes the durable LSN, checkpoints the
 *	log, commits more, and asserts that (1) the durable LSN never moves
 *	backwards across the checkpoint and (2) every record in the
 *	compacted log, read back with wal_scan, has an LSN greater than the
 *	pre-checkpoint durable LSN, in strictly increasing order.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "wal.h"
#include "xtc.h"
#include "t_tmp.h"

#define N_PRE   50
#define N_POST  20

/* The compaction dump: one placeholder record, the checkpoint marker. */
static void
dump_one(wal_emit_fn emit, void *emit_ctx, void *user)
{
	static const char ck[] = "checkpoint";
	(void)user;
	emit(emit_ctx, ck, (uint32_t)sizeof ck);
}

static void
dump_empty(wal_emit_fn emit, void *emit_ctx, void *user)
{
	(void)emit;
	(void)emit_ctx;
	(void)user;
}

struct scan_st { uint64_t prev, min, n; int ordered; };

static int
scan_cb(uint64_t lsn, const void *rec, uint32_t len, void *user)
{
	struct scan_st *s = user;
	(void)rec; (void)len;
	if (s->n > 0 && lsn <= s->prev)
		s->ordered = 0;
	if (s->n == 0 || lsn < s->min)
		s->min = lsn;
	s->prev = lsn;
	s->n++;
	return 0;
}

int
main(void)
{
	char path[256];
	wal_opts_t wo;
	wal_t *w = NULL;
	uint64_t lsn = 0, pre, post;
	struct scan_st s;
	int i, fd;

	t_tmpl(path, sizeof path, "wal-lsn");
	if ((fd = mkstemp(path)) < 0) { perror("mkstemp"); return 1; }
	close(fd);
	memset(&wo, 0, sizeof wo);
	wo.path = path; wo.window_ns = 0; wo.max_batch = 1;
	if (wal_open(&wo, &w) != XTC_OK) {
		fprintf(stderr, "FAIL: wal_open\n");
		return 1;
	}
	for (i = 0; i < N_PRE; i++)
		if (wal_commit_sync(w, "pre-record", 10, &lsn) != XTC_OK) {
			fprintf(stderr, "FAIL: pre commit %d\n", i);
			return 1;
		}
	pre = wal_durable_lsn(w);

	if (wal_checkpoint(w, path, dump_one, NULL) != XTC_OK) {
		fprintf(stderr, "FAIL: wal_checkpoint\n");
		return 1;
	}
	post = wal_durable_lsn(w);
	if (post < pre) {
		fprintf(stderr, "FAIL: durable LSN went BACKWARDS across a "
		    "checkpoint (%llu -> %llu): post-checkpoint records would "
		    "carry LSNs older than pages already on disk\n",
		    (unsigned long long)pre, (unsigned long long)post);
		return 1;
	}
	for (i = 0; i < N_POST; i++)
		if (wal_commit_sync(w, "post-record", 11, &lsn) != XTC_OK) {
			fprintf(stderr, "FAIL: post commit %d\n", i);
			return 1;
		}
	if (lsn <= pre) {
		fprintf(stderr, "FAIL: a post-checkpoint commit got LSN %llu, "
		    "not above the pre-checkpoint durable LSN %llu\n",
		    (unsigned long long)lsn, (unsigned long long)pre);
		return 1;
	}
	post = wal_durable_lsn(w);
	if (wal_checkpoint(w, path, NULL, NULL) != XTC_E_INVAL ||
	    wal_checkpoint(w, path, dump_empty, NULL) != XTC_E_INVAL ||
	    wal_durable_lsn(w) != post) {
		fprintf(stderr, "FAIL: empty checkpoint must reject without resetting LSN\n");
		return 1;
	}
	wal_close(w);
	w = NULL;
	wo.append = 1;
	if (wal_open(&wo, &w) != XTC_OK || wal_durable_lsn(w) != post) {
		fprintf(stderr, "FAIL: rejected checkpoint changed durable log on reopen\n");
		return 1;
	}
	wal_close(w);

	memset(&s, 0, sizeof s);
	s.ordered = 1;
	if (wal_scan(path, scan_cb, &s) != XTC_OK) {
		fprintf(stderr, "FAIL: wal_scan\n");
		return 1;
	}
	unlink(path);
	if (s.n != 1 + N_POST || !s.ordered || s.min <= pre) {
		fprintf(stderr, "FAIL: compacted log: %llu records, ordered=%d, "
		    "min LSN %llu (must be > pre-checkpoint %llu)\n",
		    (unsigned long long)s.n, s.ordered,
		    (unsigned long long)s.min, (unsigned long long)pre);
		return 1;
	}
	printf("  ok   LSNs rise across a checkpoint: pre-checkpoint durable "
	    "%llu, compacted log %llu..%llu (%llu records, strictly "
	    "increasing)\n", (unsigned long long)pre,
	    (unsigned long long)s.min, (unsigned long long)s.prev,
	    (unsigned long long)s.n);
	printf("All sqlxtc WAL LSN-monotonicity tests passed.\n");
	return 0;
}
