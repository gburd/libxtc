/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_torn_base.c
 *	The storage engine never relies on its page file after a crash, so
 *	torn pages in it cannot lose a committed row -- and running it writes
 *	no page through the double-write ring.
 *
 *	sx_storage_open trusts the page file only when its superblock says it
 *	was closed cleanly, and clears that flag (durably) before the first
 *	change; after any crash it truncates the file and rebuilds the tree
 *	from the log, which holds every committed row (the log checkpoint
 *	dumps the live rows into it).  So the double-write ring -- a page
 *	image plus an fdatasync before every page write, InnoDB's torn-page
 *	defence for engines that DO recover in place -- protected nothing
 *	here and cost one extra fdatasync per page written back (about 1,000
 *	a second in TPC-C at 10 warehouses, one connection).
 *
 *	The test commits rows through a pool small enough that pages are
 *	evicted (written back) while it runs, then:
 *	  (1) asserts no page went through the double-write ring;
 *	  (2) crashes (no flush, no clean flag), overwrites EVERY other page
 *	      of the page file with garbage (a torn write on each), reopens,
 *	      and checks every row;
 *	  (3) closes cleanly, reopens (now trusting the base), checks again,
 *	      then crashes after more commits with the base scribbled again,
 *	      and checks that recovery still has every row.
 */

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bufmgr.h"
#include "engine.h"
#include "xtc.h"
#include "t_tmp.h"

#define NROW   3000
#define FRAMES 64

static int g_fail;
#ifdef CK
#undef CK
#endif
#define CK(x) do { if (!(x)) { \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
	g_fail = 1; } } while (0)

static long
count_ok(long want)
{
	sx_db *h = NULL;
	sx_stmt *st = NULL;
	long n = -1;
	if (sx_open(":memory:", &h) != SX_OK) return -1;
	if (sx_prepare(h, "SELECT count(*), sum(v) FROM t", -1, &st, NULL) == SX_OK) {
		if (sx_step(st) == SX_ROW &&
		    sx_column_int64(st, 1) == (int64_t)want * (want - 1) / 2)
			n = (long)sx_column_int64(st, 0);
		sx_finalize(st);
	}
	sx_close(h);
	return n;
}

static void
insert_range(int lo, int hi)
{
	char sql[320], pad[201];
	sx_db *h = NULL;
	int i;
	memset(pad, 'p', 200); pad[200] = '\0';
	CK(sx_open(":memory:", &h) == SX_OK);
	for (i = lo; i < hi; i++) {
		snprintf(sql, sizeof sql, "INSERT INTO t VALUES(%d,%d,'%s')", i, i, pad);
		CK(sx_exec(h, sql, NULL) == SX_OK);
	}
	sx_close(h);
}

/* Overwrite every other page (skipping the superblock) with garbage. */
static long
scribble(const char *path)
{
	struct stat sb;
	uint8_t junk[4096];
	long pages, p, n = 0;
	int fd = open(path, O_RDWR);
	if (fd < 0 || fstat(fd, &sb) != 0) return -1;
	memset(junk, 0xA5, sizeof junk);
	pages = (long)(sb.st_size / 4096);
	for (p = 1; p < pages; p += 2) {
		/* half a page: a torn write */
		if (pwrite(fd, junk, 2048, (off_t)p * 4096) == 2048) n++;
	}
	(void)fsync(fd);
	close(fd);
	return n;
}

int
main(void)
{
	char path[256], side[320];
	const char *sfx[] = { "", "-wal", ".dwb", "-wal.compact", "-wal.ocompact" };
	sx_db *h = NULL;
	bm_stats_t bs;
	long n, torn;
	int fd, i;

	t_tmpl(path, sizeof path, "sqlxtc-tornbase");
	if ((fd = mkstemp(path)) < 0) return 1;
	close(fd);
	CK(sx_init() == SX_OK);

	CK(sx_storage_open(path, FRAMES) == SX_OK);
	CK(sx_open(":memory:", &h) == SX_OK);
	CK(sx_exec(h, "CREATE TABLE t(k INTEGER PRIMARY KEY, v INTEGER, pad TEXT)", NULL) == SX_OK);
	sx_close(h);
	insert_range(0, NROW / 2);
	sx_storage_bm_stats(&bs);
	printf("  running: evicted=%llu dw-page-writes=%llu\n",
	    (unsigned long long)bs.evicted, (unsigned long long)bs.dw_writes);
	CK(bs.evicted > 0);                 /* pages really were written back */
	CK(bs.dw_writes == 0);              /* (1) none through the ring */

	/* (2) crash, tear half the base, recover from the log */
	sx_storage_abandon();
	torn = scribble(path);
	CK(sx_storage_open(path, FRAMES) == SX_OK);
	n = count_ok(NROW / 2);
	printf("  crash + %ld torn pages: rows=%ld (want %d)\n", torn, n, NROW / 2);
	CK(torn > 0);
	CK(n == NROW / 2);

	/* (3) clean close + trusted reopen, more rows, crash, tear, recover */
	CK(sx_storage_close() == SX_OK);
	CK(sx_storage_open(path, FRAMES) == SX_OK);
	CK(count_ok(NROW / 2) == NROW / 2);
	insert_range(NROW / 2, NROW);
	sx_storage_abandon();
	torn = scribble(path);
	CK(sx_storage_open(path, FRAMES) == SX_OK);
	n = count_ok(NROW);
	printf("  trusted reopen, more rows, crash + %ld torn pages: rows=%ld (want %d)\n",
	    torn, n, NROW);
	CK(n == NROW);
	(void)sx_storage_close();
	(void)sx_shutdown();
	for (i = 0; i < 5; i++) {
		snprintf(side, sizeof side, "%s%s", path, sfx[i]);
		unlink(side);
	}
	if (g_fail) { fprintf(stderr, "FAIL: test_torn_base\n"); return 1; }
	printf("  ok   torn base pages after a crash lose nothing; no double-write cost\n");
	return 0;
}
