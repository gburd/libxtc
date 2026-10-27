/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_wal_dsync.c
 *	A log flush is one O_DSYNC write, no separate fdatasync.
 *
 *	  1. the log fd is O_DSYNC, after wal_open and after an in-WAL
 *	     checkpoint rebinds the handle to the compacted file;
 *	  2. committers on fibers (io_uring where the build has it) and on
 *	     plain threads: every commit acked, and the log replays every
 *	     record in LSN order;
 *	  3. the per-commit cost on one fiber is printed (the fdatasync was
 *	     an extra io_uring op that always runs on a worker thread).
 *
 *	Before the change 1 fails (the fd was plain O_RDWR).
 */

#include <fcntl.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "wal.h"
#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "t_tmp.h"

#define NF  4                     /* fiber committers */
#define NT  4                     /* thread committers */
#define PER 500

static wal_t           *g_wal;
static _Atomic long     g_ok, g_bad;
static int64_t          g_fiber_ns;

static void
commit_n(long id)
{
	char rec[64];
	long i;
	for (i = 0; i < PER; i++) {
		uint64_t lsn;
		int n = snprintf(rec, sizeof rec, "c%ld-%ld", id, i);
		if (wal_commit(g_wal, rec, (uint32_t)n, &lsn) == XTC_OK)
			atomic_fetch_add(&g_ok, 1);
		else
			atomic_fetch_add(&g_bad, 1);
	}
}

static void
fiber_main(void *arg)
{
	int64_t t0 = xtc_clock_mono();
	commit_n((long)(intptr_t)arg);
	if ((intptr_t)arg == 0)
		g_fiber_ns = xtc_clock_mono() - t0;
}

static void *
thread_main(void *arg)
{
	commit_n((long)(intptr_t)arg);
	return NULL;
}

struct sc { uint64_t prev, n; int ordered; };

static int
scan_cb(uint64_t lsn, const void *rec, uint32_t len, void *user)
{
	struct sc *s = user;
	(void)rec;
	(void)len;
	if (s->n > 0 && lsn <= s->prev)
		s->ordered = 0;
	s->prev = lsn;
	s->n++;
	return 0;
}

static void
dump1(wal_emit_fn emit, void *ctx, void *user)
{
	(void)user;
	emit(ctx, "ckpt", 4);
}

int
main(void)
{
	char path[256];
	wal_opts_t wo;
	xtc_exec_t *ex = NULL;
	xtc_proc_opts_t po = { .size = sizeof(xtc_proc_opts_t), .name = "committer" };
	pthread_t th[NT];
	struct sc s;
	xtc_pid_t p;
	long i;
	int fd, fl;

	t_tmpl(path, sizeof path, "sqlxtc-waldsync");
	if ((fd = mkstemp(path)) < 0)
		return 1;
	close(fd);
	memset(&wo, 0, sizeof wo);
	wo.path = path;
	CK(wal_open(&wo, &g_wal) == XTC_OK);
	if (g_wal == NULL)
		return 1;
	fl = fcntl(wal_fd(g_wal), F_GETFL);
	CK(fl >= 0 && (fl & O_DSYNC) == O_DSYNC);

	/* one fiber alone first: the per-commit cost of a lone committer */
	CK(xtc_exec_init(&ex, 1) == XTC_OK);
	CK(xtc_proc_spawn(xtc_exec_loop(ex, 0), fiber_main, (void *)(intptr_t)0,
	    &po, &p) == XTC_OK);
	(void)xtc_exec_run(ex);
	(void)xtc_exec_fini(ex);
	printf("  one fiber: %.1f us per commit\n",
	    (double)g_fiber_ns / 1e3 / PER);

	/* fibers on 2 loops and plain threads at once */
	CK(xtc_exec_init(&ex, 2) == XTC_OK);
	for (i = 1; i <= NF; i++)
		CK(xtc_proc_spawn(xtc_exec_loop(ex, (int)(i % 2)), fiber_main,
		    (void *)(intptr_t)i, &po, &p) == XTC_OK);
	for (i = 0; i < NT; i++)
		CK(pthread_create(&th[i], NULL, thread_main,
		    (void *)(intptr_t)(100 + i)) == 0);
	(void)xtc_exec_run(ex);
	for (i = 0; i < NT; i++)
		(void)pthread_join(th[i], NULL);
	(void)xtc_exec_fini(ex);
	CK(atomic_load(&g_ok) == (long)(1 + NF + NT) * PER);
	CK(atomic_load(&g_bad) == 0);
	memset(&s, 0, sizeof s);
	s.ordered = 1;
	CK(wal_scan(path, scan_cb, &s) == XTC_OK);
	CK(s.n == (uint64_t)(1 + NF + NT) * PER && s.ordered);

	/* the checkpoint rebinds the handle to the compacted file */
	CK(wal_checkpoint(g_wal, path, dump1, NULL) == XTC_OK);
	fl = fcntl(wal_fd(g_wal), F_GETFL);
	CK(fl >= 0 && (fl & O_DSYNC) == O_DSYNC);
	CK(wal_commit_sync(g_wal, "after", 5, NULL) == XTC_OK);
	wal_close(g_wal);

	memset(&s, 0, sizeof s);
	s.ordered = 1;
	CK(wal_scan(path, scan_cb, &s) == XTC_OK);
	CK(s.n == 2 && s.ordered);         /* the checkpoint record + "after" */
	unlink(path);
	if (g_fail) {
		fprintf(stderr, "FAIL: test_wal_dsync\n");
		return 1;
	}
	printf("  ok   the log fd is O_DSYNC (a flush is one write); %ld commits "
	    "from fibers and threads acked\n", atomic_load(&g_ok));
	return 0;
}
