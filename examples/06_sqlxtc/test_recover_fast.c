/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_recover_fast.c
 *	Crash recovery must not pay a double-write fdatasync per evicted
 *	page.  A store with a small buffer pool takes enough rows that
 *	replay evicts many pages.  The test crashes it (sx_storage_abandon:
 *	no checkpoint, no clean flag), reopens it, and checks:
 *	  - every committed row is back, after the reopen and again after a
 *	    second crash straight after recovery (recovery can be repeated);
 *	  - replay wrote no page image to the double-write ring.  Before
 *	    the fix every eviction during replay did, one fdatasync each:
 *	    about 2.5 s per MB of log on the server.
 *	  - a checkpoint after recovery still writes no page through the
 *	    ring: the storage engine never trusts its page file after a
 *	    crash, so it runs without one (see test_torn_base).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bufmgr.h"
#include "engine.h"
#include "xtc.h"
#include "t_tmp.h"

#define NROW   4000
#define FRAMES 32

static long
count_ok(void)
{
	sx_db *h = NULL;
	sx_stmt *st = NULL;
	long n = -1;
	if (sx_open(":memory:", &h) != SX_OK) return -1;
	if (sx_prepare(h, "SELECT count(*), sum(v) FROM t", -1, &st, NULL) == SX_OK) {
		if (sx_step(st) == SX_ROW &&
		    sx_column_int64(st, 1) == (int64_t)NROW * (NROW - 1) / 2)
			n = (long)sx_column_int64(st, 0);
		sx_finalize(st);
	}
	sx_close(h);
	return n;
}

int
main(void)
{
	char path[256], sql[320], pad[201], side[320];
	const char *sfx[] = { "", "-wal", ".dwb", "-wal.compact" };
	sx_db *h = NULL;
	bm_stats_t bs;
	int64_t t0, t1;
	long n;
	int fd, i;

	t_tmpl(path, sizeof path, "sqlxtc-rfast");
	if ((fd = mkstemp(path)) < 0) return 1;
	close(fd);
	memset(pad, 'p', 200); pad[200] = '\0';
	CK(sx_init() == SX_OK);

	/* Populate off-loop (synchronous log), then crash. */
	CK(sx_storage_open(path, FRAMES) == SX_OK);
	CK(sx_open(":memory:", &h) == SX_OK);
	CK(sx_exec(h, "CREATE TABLE t(k INTEGER PRIMARY KEY, v INTEGER, pad TEXT)", NULL) == SX_OK);
	for (i = 0; i < NROW; i++) {
		snprintf(sql, sizeof sql, "INSERT INTO t VALUES(%d,%d,'%s')", i, i, pad);
		CK(sx_exec(h, sql, NULL) == SX_OK);
	}
	sx_close(h);
	sx_storage_abandon();

	/* Recover.  The open replays the log and then checkpoints it. */
	t0 = xtc_clock_mono();
	CK(sx_storage_open(path, FRAMES) == SX_OK);
	t1 = xtc_clock_mono();
	sx_storage_bm_stats(&bs);
	n = count_ok();
	printf("  rows=%d frames=%d recover=%.1f ms replay-evictions=%llu "
	    "dw-page-writes=%llu rows-after=%ld\n", NROW, FRAMES,
	    (double)(t1 - t0) / 1e6, (unsigned long long)bs.evicted,
	    (unsigned long long)bs.dw_writes, n);
	CK(bs.evicted > 0);                 /* the pool really overflowed */
	CK(bs.dw_writes == 0);              /* no ring write during replay */
	CK(n == NROW);

	/* A crash right after recovery must recover again. */
	sx_storage_abandon();
	CK(sx_storage_open(path, FRAMES) == SX_OK);
	CK(count_ok() == NROW);

	/* Flushing replay's dirty pages writes them in place (no ring). */
	CK(sx_storage_checkpoint() == SX_OK);
	sx_storage_bm_stats(&bs);
	CK(bs.dw_writes == 0);
	CK(sx_storage_close() == SX_OK);
	CK(sx_storage_open(path, FRAMES) == SX_OK);
	CK(count_ok() == NROW);
	(void)sx_storage_close();
	(void)sx_shutdown();
	for (i = 0; i < 4; i++) {
		snprintf(side, sizeof side, "%s%s", path, sfx[i]);
		unlink(side);
	}
	if (g_fail) { fprintf(stderr, "FAIL: test_recover_fast\n"); return 1; }
	printf("  ok   crash recovery replays without a double-write sync per page\n");
	return 0;
}
