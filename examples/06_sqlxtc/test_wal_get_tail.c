/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_wal_get_tail.c
 *	wal_get_tail: the tail of an open, quiesced log, so a clean close can
 *	record where the log ends and the next open skip the scan.  After
 *	each way the tail can move -- small appends, a direct (oversized)
 *	append, a compaction (wal_checkpoint), an online compaction, a reopen
 *	with a verified hint, an empty log -- the reported tail must equal
 *	what wal_scan_ex finds in the file, and wal_open must accept it.
 *	After a reopen that SCANNED a non-empty log the handle does not know
 *	where its last frame starts: valid = 0 (the caller then records no
 *	hint and the next open scans -- slow, never wrong).
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "wal.h"
#include "t_tmp.h"

static int nop_cb(uint64_t lsn, const void *rec, uint32_t len, void *user)
{
	(void)lsn; (void)rec; (void)len; (void)user;
	return 0;
}

/* The handle's tail must equal the file's (a fresh scan). */
static int
same_as_scan(wal_t *w, const char *path, const char *what)
{
	wal_tail_t a, b;
	int ok;
	if (wal_get_tail(w, &a) != XTC_OK || !a.valid) {
		fprintf(stderr, "  %s: wal_get_tail invalid\n", what);
		return 0;
	}
	if (wal_scan_ex(path, nop_cb, NULL, &b) != XTC_OK || !b.valid)
		return 0;
	ok = a.end == b.end && a.last_off == b.last_off && a.max_lsn == b.max_lsn;
	printf("  %-22s end %llu last %llu lsn %llu (scan: %llu %llu %llu) %s\n",
	    what, (unsigned long long)a.end, (unsigned long long)a.last_off,
	    (unsigned long long)a.max_lsn, (unsigned long long)b.end,
	    (unsigned long long)b.last_off, (unsigned long long)b.max_lsn,
	    ok ? "ok" : "MISMATCH");
	return ok;
}

static void
dump2(wal_emit_fn emit, void *ctx, void *user)
{
	(void)user;
	emit(ctx, "checkpoint", 10);
	emit(ctx, "row-1", 5);
	emit(ctx, "row-two", 7);
}

/* Reopen with hint `t`; 1 if the open trusted it (end matches and the
 * next commit gets max_lsn + 1). */
static int
open_with(const char *path, const wal_tail_t *t, wal_t **out)
{
	wal_opts_t wo;
	memset(&wo, 0, sizeof wo);
	wo.path = path;
	wo.append = 1;
	wo.tail = t;
	return wal_open(&wo, out) == XTC_OK;
}

int
main(void)
{
	char path[256];
	wal_opts_t wo;
	wal_t *w = NULL;
	wal_tail_t t;
	wal_ock_t *k = NULL;
	wal_emit_fn fn;
	void *ctx;
	uint8_t *big;
	uint64_t lsn = 0, left = 0;
	int fd, i;

	t_tmpl(path, sizeof path, "sqlxtc-walgettail");
	if ((fd = mkstemp(path)) < 0)
		return 1;
	close(fd);
	big = calloc(1, 3u << 20);       /* > WAL_RING / 4: the direct path */
	if (big == NULL)
		return 1;

	/* empty log */
	memset(&wo, 0, sizeof wo);
	wo.path = path;
	CK(wal_open(&wo, &w) == XTC_OK);
	CK(same_as_scan(w, path, "empty"));

	/* appends, then a direct append, then appends */
	for (i = 0; i < 200; i++)
		CK(wal_commit_sync(w, "abcdefgh", 1 + (uint32_t)(i % 8), &lsn) == XTC_OK);
	CK(same_as_scan(w, path, "appends"));
	CK(wal_commit_sync(w, big, 3u << 20, &lsn) == XTC_OK);
	CK(same_as_scan(w, path, "direct append"));
	CK(wal_commit_sync(w, "x", 1, &lsn) == XTC_OK);
	CK(same_as_scan(w, path, "append after direct"));

	/* a compaction rebinds the handle to the new file */
	CK(wal_checkpoint(w, path, dump2, NULL) == XTC_OK);
	CK(same_as_scan(w, path, "checkpoint"));
	CK(wal_get_tail(w, &t) == XTC_OK);
	wal_close(w);

	/* the recorded tail is accepted by the next open, which then knows
	 * its tail without a scan; appending moves it */
	CK(open_with(path, &t, &w));
	CK(wal_size(w) == t.end);
	CK(same_as_scan(w, path, "hinted reopen"));
	CK(wal_commit_sync(w, "after", 5, &lsn) == XTC_OK);
	CK(lsn == t.max_lsn + 1);
	CK(same_as_scan(w, path, "append after hint"));

	/* an online compaction */
	CK(wal_ockpt_begin(w, path, &k) == XTC_OK);
	wal_ockpt_emitter(k, &fn, &ctx);
	fn(ctx, "ock", 3);
	CK(wal_commit_sync(w, "concurrent", 10, &lsn) == XTC_OK);
	CK(wal_ockpt_catchup(k, &left) == XTC_OK);
	CK(wal_ockpt_finish(k) == XTC_OK);
	CK(same_as_scan(w, path, "online checkpoint"));
	wal_close(w);

	/* a reopen that scans a non-empty log does not know its last frame */
	CK(open_with(path, NULL, &w));
	CK(wal_get_tail(w, &t) == XTC_E_AGAIN && !t.valid);
	printf("  scanned reopen         valid %d (want 0)\n", t.valid);
	CK(wal_commit_sync(w, "y", 1, &lsn) == XTC_OK);  /* ...until it appends */
	CK(same_as_scan(w, path, "append after scan"));
	wal_close(w);

	unlink(path);
	free(big);
	if (g_fail) {
		fprintf(stderr, "FAIL: test_wal_get_tail\n");
		return 1;
	}
	printf("  ok   wal_get_tail equals the scanned tail after every way the "
	    "tail moves\n");
	return 0;
}
