/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_page_eio.c
 *	A page of the page file that cannot be read (pread fails, as a disk
 *	reports a latent sector error) must never cost a committed row.
 *	pread is interposed: once armed, every 4 KiB read of ONE data page
 *	of the page file fails with the chosen errno.
 *	  replay  a crash, then a rebuild open whose log replay reads the
 *	          bad page: the open must be refused with the log
 *	          byte-identical (or keep every row); 1.60-2.1.1 skipped the
 *	          failed insert, compacted the log over the short tree and
 *	          returned SX_OK.
 *	The run is bounded by alarm(120).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE /* RTLD_NEXT */
#endif
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "engine.h"
#include "t_tmp.h"

#define ROWS 2000
#define PS 4096

static char g_path[256], g_wal[300];
static int g_armed, g_errno = EIO;
static dev_t g_dev;
static ino_t g_ino;
static off_t g_off = -1; /* the bad page; -1 = the first read */
static long g_hits;

ssize_t
pread(int fd, void *buf, size_t n, off_t off)
{
	static ssize_t (*real)(int, void *, size_t, off_t);
	struct stat sb;

	if (real == NULL)
		*(void **)&real = dlsym(RTLD_NEXT, "pread");
	if (g_armed && n == PS && off >= PS && fstat(fd, &sb) == 0 &&
	    sb.st_dev == g_dev && sb.st_ino == g_ino) {
		if (g_off < 0)
			g_off = off; /* latch the first data page read */
		if (off == g_off) {
			g_hits++;
			errno = g_errno;
			return -1;
		}
	}
	return real(fd, buf, n, off);
}

/* Arm the fault on the page file: page `pg`, or the first one read. */
static void
arm(off_t pg, int err)
{
	struct stat sb;

	CK(stat(g_path, &sb) == 0);
	g_dev = sb.st_dev;
	g_ino = sb.st_ino;
	g_off = pg < 0 ? -1 : pg * PS;
	g_errno = err;
	g_hits = 0;
	g_armed = 1;
}

/* The whole file, malloc'd (*len bytes), or NULL. */
static char *
slurp(const char *p, size_t *len)
{
	struct stat sb;
	char *b;
	int fd;

	if ((fd = open(p, O_RDONLY)) < 0)
		return NULL;
	if (fstat(fd, &sb) != 0 ||
	    (b = malloc((size_t)sb.st_size + 1)) == NULL) {
		close(fd);
		return NULL;
	}
	if (read(fd, b, (size_t)sb.st_size) != sb.st_size) {
		free(b);
		b = NULL;
	}
	*len = (size_t)sb.st_size;
	close(fd);
	return b;
}

static long
count_rows(void)
{
	sx_db *h = NULL;
	sx_stmt *st = NULL;
	long n = -1;

	if (sx_open(":memory:", &h) != SX_OK)
		return -1;
	if (sx_prepare(h, "SELECT count(*) FROM t", -1, &st, NULL) == SX_OK) {
		if (sx_step(st) == SX_ROW)
			n = (long)sx_column_int64(st, 0);
		sx_finalize(st);
	}
	sx_close(h);
	return n;
}

/* Rows [from, to) in a shuffled key order (so a replay of them revisits
 * leaves the small pool evicted), ~200 bytes each. */
static void
fill(int from, int to)
{
	sx_db *h = NULL;
	char sql[400];
	int i, k;

	CK(sx_open(":memory:", &h) == SX_OK);
	if (h == NULL)
		return;
	if (from == 0)
		CK(sx_exec(h, "CREATE TABLE t(k INTEGER PRIMARY KEY, v TEXT)",
		       NULL) == SX_OK);
	for (i = from; i < to; i++) {
		k = to - from == ROWS ? from + (int)((i * 7919L) % ROWS) : i;
		snprintf(sql, sizeof sql,
		    "INSERT INTO t VALUES(%d, 'ROW%05d-"
		    "%0180d')",
		    k, k, 0);
		CK(sx_exec(h, sql, NULL) == SX_OK);
	}
	sx_close(h);
}

static void
setup(const char *name)
{
	int fd;

	t_tmpl(g_path, sizeof g_path, name);
	if ((fd = mkstemp(g_path)) >= 0)
		close(fd);
	snprintf(g_wal, sizeof g_wal, "%s-wal", g_path);
	CK(sx_storage_open(g_path, 64) == SX_OK);
	fill(0, ROWS);
}

static void
teardown(void)
{
	char c[320];

	(void)sx_storage_close();
	unlink(g_path);
	unlink(g_wal);
	snprintf(c, sizeof c, "%s.compact", g_wal);
	unlink(c);
}

/* A crash, then a rebuild open whose replay cannot read a page. */
static void
t_replay(void)
{
	char *w0, *w1;
	size_t l0 = 0, l1 = 0;
	long n;
	int rc;

	setup("eioreplay");
	sx_storage_abandon(); /* crash: the log holds every row */
	w0 = slurp(g_wal, &l0);
	CK(w0 != NULL);
	arm(-1, EIO);
	rc = sx_storage_open(g_path, 64);
	g_armed = 0;
	CK(g_hits > 0);
	if (rc == SX_OK) {
		/* Allowed only if nothing was lost, in this open and after. */
		CK((n = count_rows()) == ROWS);
		sx_storage_abandon();
		printf("  replay: open rc=%d rows=%ld\n", rc, n);
	} else {
		w1 = slurp(g_wal, &l1);
		CK(w1 != NULL && l1 == l0 && memcmp(w0, w1, l0) == 0);
		printf("  replay: open refused (rc=%d, %ld failed reads), log "
		       "byte-identical (%zu bytes)\n",
		    rc, g_hits, l0);
		free(w1);
	}
	free(w0);
	CK(sx_storage_open(g_path, 64) == SX_OK);
	CK((n = count_rows()) == ROWS);
	printf("  replay: reopen without the fault: %ld of %d rows\n", n, ROWS);
	teardown();
}

int
main(void)
{
	alarm(120);
	CK(sx_init() == SX_OK);
	t_replay();
	if (!g_fail)
		printf("ok test_page_eio\n");
	return g_fail;
}
