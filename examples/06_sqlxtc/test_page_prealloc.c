/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_page_prealloc.c
 *	The page file grows in large preallocated steps (bufmgr.c
 *	pf_grow), not one page per write-back, and a clean close gives the
 *	unused tail back.
 *
 *	A buffered io_uring write that moves the end of file goes to an
 *	io-wq worker (XFS takes the inode lock for the size update), so a
 *	page file grown one page at a time punted every extending
 *	write-back (the uring gate's crash_oracle and sqlxtc_iosoak PUNT).
 *	(1) 4096 pages written through a 16-frame pool: the file size
 *	    changes once per step (Linux), not once per write-back, and
 *	    while open it is never more than one step past the written
 *	    pages (a step is an eighth of the file, 1 to 8 MiB).
 *	(2) Free 100 pages, clean superblock, close: the file is exactly
 *	    the written pages again (no preallocated tail).
 *	(3) Reopen: the 100 freed ids come back, every page reads back,
 *	    and the first new id after them is the next one past the end.
 *	(4) A crash (no close) leaves the tail, at most 1 MiB on a small
 *	    store; a reopen skips it: every page reads back and new ids
 *	    start past it, so none is issued twice.
 *	Off a loop (synchronous bufmgr I/O).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "bufmgr.h"
#include "xtc.h"
#include "t_tmp.h"

#define PS      4096
#define NPAGES  4096          /* 16 MiB: two preallocation steps */
#define MIB     (1LL << 20)   /* bufmgr.c BM_PRE_MIN */

static bm_pid_t g_pid[NPAGES];
static const char super[40] = "test superblock";

static long long
fsz(const char *p)
{
	struct stat st;
	return stat(p, &st) == 0 ? (long long)st.st_size : -1;
}

static bm_t *
open_bm(const char *path, int reopen)
{
	bm_opts_t bo = BM_OPTS_DEFAULT;
	bm_t *bm = NULL;
	bo.path = path;
	bo.page_size = PS;
	bo.n_frames = 16;
	bo.reopen = (uint8_t)reopen;
	return bm_create(&bo, &bm) == XTC_OK ? bm : NULL;
}

static void
fill(void *p, bm_pid_t pid)
{
	memset(p, (int)(pid & 0xff), PS);
	memcpy(p, &pid, sizeof pid);
}

static int
check(bm_t *bm, bm_pid_t pid)
{
	bm_frame_t *f;
	uint8_t want[PS];
	int ok;
	if (bm_fix_pid(bm, pid, &f) != XTC_OK)
		return 0;
	fill(want, pid);
	ok = memcmp(bm_page(f), want, PS) == 0;
	bm_unfix(bm, f, 0);
	return ok;
}

/* Allocate and write n pages; count file-size changes seen meanwhile. */
static int
alloc_n(bm_t *bm, const char *path, int n, bm_pid_t *pids, int *changes,
    long long *worst)
{
	long long last = fsz(path), s;
	bm_frame_t *f;
	int i;
	for (i = 0; i < n; i++) {
		if (bm_alloc_pid(bm, &f, &pids[i]) != XTC_OK)
			return -1;
		fill(bm_page(f), pids[i]);
		bm_unfix(bm, f, 1);
		s = fsz(path);
		if (s != last) {
			(*changes)++;
			last = s;
		}
		/* bytes past the highest page allocated so far */
		if (s - (long long)(pids[i] + 1) * PS > *worst)
			*worst = s - (long long)(pids[i] + 1) * PS;
	}
	return bm_checkpoint(bm) == XTC_OK ? 0 : -1;
}

int
main(void)
{
	char path[256];
	bm_t *bm;
	bm_stats_t st;
	bm_pid_t maxpid = 0, p, more[200];
	long long worst = 0, end, sz;
	int i, fd, changes = 0, ok, status;
	pid_t child;

	printf("test_page_prealloc\n");
	t_tmpl(path, sizeof path, "sqlxtc-prealloc");
	if ((fd = mkstemp(path)) >= 0)
		close(fd);

	/* (1) */
	CK((bm = open_bm(path, 0)) != NULL);
	if (bm == NULL)
		return 1;
	CK(bm_write_super(bm, super, sizeof super) == XTC_OK);
	CK(alloc_n(bm, path, NPAGES, g_pid, &changes, &worst) == 0);
	for (i = 0; i < NPAGES; i++)
		if (g_pid[i] > maxpid)
			maxpid = g_pid[i];
	end = (long long)(maxpid + 1) * PS;
	printf("  %d pages written: the file size changed %d times, at most "
	    "%lld KiB past the pages\n", NPAGES, changes, worst >> 10);
#if defined(__linux__)
	CK(changes <= 20);         /* was one per extending write-back: 255 */
#endif
	CK(worst <= MIB + end / 8);
	CK(fsz(path) >= end);

	/* (2) */
	for (i = 0; i < 100; i++)
		CK(bm_free_pid(bm, g_pid[2 * i + 1]) == XTC_OK);
	CK(bm_write_super_clean(bm, super, sizeof super, 1) == XTC_OK);
	CK(bm_sync(bm) == XTC_OK);
	bm_destroy(bm);
	sz = fsz(path);
	printf("  clean close: %lld bytes (pages end at %lld)\n", sz, end);
	CK(sz == end);

	/* (3) */
	CK((bm = open_bm(path, 1)) != NULL);
	if (bm == NULL)
		return 1;
	bm_get_stats(bm, &st);
	CK(st.fl_loaded == 100);
	ok = 1;
	for (i = 0; i < NPAGES; i++)
		if ((i & 1) == 0 || i >= 200)
			ok &= check(bm, g_pid[i]);
	CK(ok);
	changes = 0;
	worst = 0;
	CK(alloc_n(bm, path, 101, more, &changes, &worst) == 0);
	CK(more[100] == maxpid + 1);   /* 100 reused, then the next id */
	(void)bm_write_super(bm, super, sizeof super);
	bm_destroy(bm);
	unlink(path);
	printf("  ok   reopen: 100 freed ids reused, then id %llu\n",
	    (unsigned long long)more[100]);

	/* (4) a crash keeps the tail */
	if ((fd = mkstemp(path)) >= 0)
		close(fd);
	if ((child = fork()) == 0) {
		bm = open_bm(path, 0);
		if (bm == NULL || bm_write_super(bm, super, sizeof super) != XTC_OK ||
		    alloc_n(bm, path, 1000, g_pid, &changes, &worst) != 0)
			_exit(1);
		_exit(0);                  /* no bm_destroy: a crash */
	}
	CK(child > 0 && waitpid(child, &status, 0) == child &&
	    WIFEXITED(status) && WEXITSTATUS(status) == 0);
	sz = fsz(path);
	end = 1001LL * PS;             /* the superblock + 1000 pages */
	printf("  crash: %lld bytes on disk for %lld of pages\n", sz, end);
	CK(sz >= end && sz <= end + MIB);
	CK((bm = open_bm(path, 1)) != NULL);
	if (bm == NULL)
		return 1;
	ok = 1;
	for (p = 1; p <= 1000; p++)
		ok &= check(bm, p);
	CK(ok);
	changes = 0;
	worst = 0;
	CK(alloc_n(bm, path, 1, more, &changes, &worst) == 0);
	CK((long long)more[0] * PS >= sz);    /* past the tail: never reissued */
	bm_destroy(bm);
	unlink(path);

	if (g_fail) {
		fprintf(stderr, "FAIL: test_page_prealloc\n");
		return 1;
	}
	printf("  ok   the page file grows in steps and closes at its true size\n");
	return 0;
}
