/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_freelist_persist.c
 *	Freed page ids survive a clean close (bufmgr.c fl_save /
 *	freelist_load).
 *
 *	The id freelist lived only in memory: a clean close dropped it and
 *	the next trusted open resumed allocation at the end of the file, so
 *	every page free at close was lost for good (a TPC-C load leaves
 *	about 72,000 of them, from its STEAL staging).
 *
 *	(1) bufmgr: allocate 300 pages, free 200 of them, write a CLEAN
 *	    superblock, reopen: the 200 come back (fl_loaded), the next 200
 *	    allocations reuse them (lowest first) and the file does not grow.
 *	(2) A non-clean superblock write after the clean one drops the
 *	    list: a reopen loads nothing (a base trusted in place after a
 *	    crash must never be handed a page id the run reused).
 *	(3) A corrupt chain (an id outside the file, or a cycle) loads
 *	    nothing.
 *	(4) Engine: a transaction big enough to spill to the STEAL staging
 *	    area, then a clean close; after reopening, more rows are written
 *	    with no file growth while saved ids remain, and every row reads
 *	    back.
 *	Off a loop (synchronous bufmgr I/O).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bufmgr.h"
#include "btree.h"
#include "engine.h"
#include "t_tmp.h"

#define PS 4096

static off_t
fsz(const char *p)
{
	struct stat st;
	return stat(p, &st) == 0 ? st.st_size : -1;
}

/* Bytes the file occupies on disk. */
static long long
fdisk(const char *p)
{
	struct stat st;
	return stat(p, &st) == 0 ? (long long)st.st_blocks * 512 : -1;
}

static bm_t *
open_bm(const char *path, int reopen)
{
	bm_opts_t bo = BM_OPTS_DEFAULT;
	bm_t *bm = NULL;
	bo.path = path;
	bo.page_size = PS;
	bo.n_frames = 512;
	bo.reopen = (uint8_t)reopen;
	return bm_create(&bo, &bm) == XTC_OK ? bm : NULL;
}

static const char super[40] = "test superblock";

/* Allocate n pages (written: unfix dirty, then checkpoint). */
static int
alloc_n(bm_t *bm, int n, bm_pid_t *pids)
{
	int i;
	for (i = 0; i < n; i++) {
		bm_frame_t *f;
		if (bm_alloc_pid(bm, &f, &pids[i]) != XTC_OK) return -1;
		memset(bm_page(f), 0x5A, 64);
		bm_unfix(bm, f, 1);
	}
	return bm_checkpoint(bm) == XTC_OK ? 0 : -1;
}

static void
t_bufmgr(void)
{
	char path[256];
	bm_t *bm;
	bm_pid_t pids[300], again[200];
	bm_stats_t st;
	off_t sz0, sz1;
	long long d0, d1;
	int i, fd, inlist, lowest = 1;

	t_tmpl(path, sizeof path, "sqlxtc-fl");
	if ((fd = mkstemp(path)) >= 0) close(fd);

	/* (1) free 200 of 300, clean close, reopen, reuse */
	CK((bm = open_bm(path, 0)) != NULL);
	if (bm == NULL) return;
	CK(alloc_n(bm, 300, pids) == 0);
	CK(bm_write_super(bm, super, sizeof super) == XTC_OK);
	CK(bm_sync(bm) == XTC_OK);
	d0 = fdisk(path);
	for (i = 0; i < 300; i += 3) {             /* free 2 of every 3 */
		CK(bm_free_pid(bm, pids[i + 1]) == XTC_OK);
		CK(bm_free_pid(bm, pids[i + 2]) == XTC_OK);
	}
	CK(bm_write_super_clean(bm, super, sizeof super, 1) == XTC_OK);
	CK(bm_sync(bm) == XTC_OK);
	bm_destroy(bm);
	sz0 = fsz(path);
	d1 = fdisk(path);
	/* 199 of the 200 free pages are holes (one holds the chain), where
	 * the filesystem has holes */
	printf("  clean close: %lld -> %lld bytes on disk (file size %lld)\n",
	    d0, d1, (long long)sz0);
	CK(d1 <= d0 - 150LL * PS || getenv("SQLXTC_NO_HOLES") != NULL);

	CK((bm = open_bm(path, 1)) != NULL);
	if (bm == NULL) return;
	bm_get_stats(bm, &st);
	printf("  reopen: %llu free page ids loaded from %llu chain page(s)\n",
	    (unsigned long long)st.fl_loaded, (unsigned long long)st.fl_pages);
	CK(st.fl_loaded == 200);
	CK(alloc_n(bm, 200, again) == 0);
	for (i = 0; i < 200; i++) {
		int j;
		inlist = 0;
		for (j = 0; j < 300; j += 3)
			if (again[i] == pids[j + 1] || again[i] == pids[j + 2]) inlist = 1;
		CK(inlist);                            /* a freed id, not a new one */
		if (i > 0 && again[i] < again[i - 1]) lowest = 0;
	}
	CK(lowest);                                /* lowest id first */
	(void)bm_write_super(bm, super, sizeof super);
	(void)bm_sync(bm);
	bm_destroy(bm);
	sz1 = fsz(path);
	printf("  200 allocations after the reopen: file %lld -> %lld bytes\n",
	    (long long)sz0, (long long)sz1);
	CK(sz1 == sz0);

	/* (2) the dirty superblock just written dropped any list */
	CK((bm = open_bm(path, 1)) != NULL);
	if (bm == NULL) return;
	bm_get_stats(bm, &st);
	CK(st.fl_loaded == 0);
	/* free some, save clean, then mark dirty (as a trusted open does) */
	for (i = 0; i < 50; i++) CK(bm_free_pid(bm, again[i]) == XTC_OK);
	CK(bm_write_super_clean(bm, super, sizeof super, 1) == XTC_OK);
	CK(bm_write_super(bm, super, sizeof super) == XTC_OK);
	bm_destroy(bm);
	CK((bm = open_bm(path, 1)) != NULL);
	if (bm == NULL) return;
	bm_get_stats(bm, &st);
	CK(st.fl_loaded == 0);                     /* not trusted: dropped */
	bm_destroy(bm);
	printf("  ok   a dirty superblock write drops the saved list\n");

	/* (3) a corrupt chain loads nothing: save, then break the head
	 * page's first id (point it past the end of the file) */
	CK((bm = open_bm(path, 1)) != NULL);
	if (bm == NULL) return;
	for (i = 50; i < 120; i++) CK(bm_free_pid(bm, again[i]) == XTC_OK);
	CK(bm_write_super_clean(bm, super, sizeof super, 1) == XTC_OK);
	bm_destroy(bm);
	{
		uint8_t pg[PS], tr[32];
		uint64_t head = 0, bad = 1u << 30;
		int k;
		fd = open(path, O_RDWR);
		CK(fd >= 0 && pread(fd, tr, 32, PS - 32) == 32);
		for (k = 7; k >= 0; k--) head = (head << 8) | tr[8 + k];
		CK(head != 0 && pread(fd, pg, PS, (off_t)(head * PS)) == PS);
		memcpy(pg + 24, &bad, 8);              /* first id; little-endian host */
		CK(pwrite(fd, pg, PS, (off_t)(head * PS)) == PS);
		close(fd);
	}
	CK((bm = open_bm(path, 1)) != NULL);
	if (bm == NULL) return;
	bm_get_stats(bm, &st);
	CK(st.fl_loaded == 0);
	bm_destroy(bm);
	printf("  ok   a corrupt chain loads nothing\n");
	unlink(path);
	if (!g_fail) printf("  ok   bufmgr: freed ids survive a clean close and are reused\n");
}

static int64_t
q1(sx_db *db, const char *sql)
{
	sx_stmt *st = NULL;
	int64_t v = -1;
	if (sx_prepare(db, sql, -1, &st, NULL) != SX_OK) return -1;
	if (sx_step(st) == SX_ROW) v = sx_column_int64(st, 0);
	sx_finalize(st);
	return v;
}

static int
fill(sx_db *db, int from, int n)
{
	sx_stmt *st = NULL;
	char pad[400];
	int i;
	memset(pad, 'p', sizeof pad - 1);
	pad[sizeof pad - 1] = '\0';
	if (sx_exec(db, "BEGIN", NULL) != SX_OK) return -1;
	if (sx_prepare(db, "INSERT INTO t VALUES(?, ?)", -1, &st, NULL) != SX_OK)
		return -1;
	for (i = from; i < from + n; i++) {
		sx_bind_int64(st, 1, i);
		sx_bind_text(st, 2, pad, -1);
		if (sx_step(st) != SX_DONE) { sx_finalize(st); return -1; }
		sx_reset(st);
	}
	sx_finalize(st);
	return sx_exec(db, "COMMIT", NULL) == SX_OK ? 0 : -1;
}

static void
t_engine(void)
{
	char path[256], wal[300];
	sx_db *db = NULL;
	bm_stats_t st;
	off_t s0, s1;
	int fd;

	t_tmpl(path, sizeof path, "sqlxtc-flsx");
	if ((fd = mkstemp(path)) >= 0) close(fd);
	snprintf(wal, sizeof wal, "%s-wal", path);
	CK(sx_init() == SX_OK);
	CK(sx_storage_open(path, 4096) == SX_OK);
	CK(sx_open(":memory:", &db) == SX_OK);
	if (db == NULL) return;
	CK(sx_exec(db, "CREATE TABLE t(k INTEGER PRIMARY KEY, v)", NULL) == SX_OK);
	/* one 8 MB transaction: spills to the staging area (> 256 KiB
	 * buffered), whose pages are freed at commit */
	CK(fill(db, 0, 20000) == 0);
	sx_close(db);
	CK(sx_storage_close() == SX_OK);
	s0 = fsz(path);

	CK(sx_storage_open(path, 4096) == SX_OK);
	sx_storage_bm_stats(&st);
	printf("  engine: reopen loaded %llu free page ids (file %lld MB)\n",
	    (unsigned long long)st.fl_loaded, (long long)(s0 >> 20));
	CK(st.fl_loaded > 500);
	CK(sx_open(":memory:", &db) == SX_OK);
	if (db == NULL) return;
	(void)sx_exec(db, "CREATE TABLE t(k INTEGER PRIMARY KEY, v)", NULL);
	/* rows in small transactions (no spill): they fit in the saved ids */
	{
		int k;
		for (k = 0; k < 10; k++) CK(fill(db, 20000 + k * 50, 50) == 0);
	}
	CK(q1(db, "SELECT count(*) FROM t") == 20500);
	sx_close(db);
	CK(sx_storage_close() == SX_OK);
	s1 = fsz(path);
	printf("  500 more rows after the reopen: file %lld -> %lld bytes\n",
	    (long long)s0, (long long)s1);
	CK(s1 == s0);
	CK(sx_storage_open(path, 4096) == SX_OK);
	CK(sx_open(":memory:", &db) == SX_OK);
	if (db != NULL) {
		(void)sx_exec(db, "CREATE TABLE t(k INTEGER PRIMARY KEY, v)", NULL);
		CK(q1(db, "SELECT count(*) FROM t") == 20500);
		CK(q1(db, "SELECT count(*) FROM t WHERE length(v) = 399") == 20500);
		sx_close(db);
	}
	CK(sx_storage_close() == SX_OK);
	(void)sx_shutdown();
	unlink(path);
	unlink(wal);
	if (!g_fail) printf("  ok   engine: staging pages freed at commit are reused after a clean reopen\n");
}

int
main(void)
{
	printf("test_freelist_persist\n");
	t_bufmgr();
	t_engine();
	if (g_fail) { fprintf(stderr, "FAIL: test_freelist_persist\n"); return 1; }
	printf("  ok   freelist persists across a clean close\n");
	return 0;
}
