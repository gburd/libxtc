/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * test/concurrency/repro_blocking_epoll.c
 *
 *	MANUAL reproducer for the open epoll lost-wakeup in
 *	xtc_proc_wait_fd / xtc_blocking_run (see docs/KNOWN_ISSUES.md).
 *	NOT built or run by `make check`: under the epoll backend it
 *	HANGS, which is the bug.  Build it by hand against an
 *	epoll-configured libxtc to drive a fix:
 *
 *	    ./dist/configure --with-io-backend=epoll && make -C build
 *	    cc -std=c11 -O2 -g -D_GNU_SOURCE -Isrc/inc \
 *	       -o /tmp/repro test/concurrency/repro_blocking_epoll.c \
 *	       build/libxtc.a -pthread -ldl -lm -luring
 *	    /tmp/repro     # exits 0 when fixed; watchdog fires (7) on hang
 *
 *	Many procs on a multi-loop executor each hammer xtc_blocking_run.
 *	The offloaded work just returns a constant; the bug is that some
 *	proc's completion wakeup is lost and xtc_exec_run never returns.
 *	io_uring is unaffected; epoll hangs.  The hang is purely in the
 *	library primitive (no buffer manager involved).
 */

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>

#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_blocking.h"

#define NLOOPS 4
#define NPROCS 16
#define ITERS  4000

static _Atomic int  g_done;
static _Atomic long g_calls;

static int noop_fn(void *a) { (void)a; return 42; }

static void *
watchdog(void *a)
{
	struct timespec ts = { 25, 0 };
	(void)a;
	nanosleep(&ts, 0);
	fprintf(stderr, "=== WATCHDOG: HANG (%d/%d done, %ld calls) ===\n",
	    atomic_load(&g_done), NPROCS, atomic_load(&g_calls));
	_exit(7);
}

static void
worker(void *a)
{
	int i, r;
	(void)a;
	for (i = 0; i < ITERS; i++) {
		r = 0;
		(void)xtc_blocking_run(noop_fn, NULL, &r);
		atomic_fetch_add(&g_calls, 1);
		if (r != 42)
			fprintf(stderr, "bad result %d\n", r);
		if ((i & 31) == 0)
			xtc_yield();
	}
	atomic_fetch_add(&g_done, 1);
}

int
main(void)
{
	xtc_exec_t *exec = NULL;
	xtc_pid_t pid;
	pthread_t wd;
	int i;

	pthread_create(&wd, NULL, watchdog, NULL);
	pthread_detach(wd);

	if (xtc_exec_init(&exec, NLOOPS) != XTC_OK) {
		fprintf(stderr, "exec_init\n");
		return 1;
	}
	for (i = 0; i < NPROCS; i++) {
		xtc_proc_opts_t po = { .name = "w" };
		if (xtc_proc_spawn(xtc_exec_loop(exec, i % NLOOPS), worker, NULL,
		    &po, &pid) != XTC_OK) {
			fprintf(stderr, "spawn %d\n", i);
			return 1;
		}
	}
	if (xtc_exec_run(exec) != XTC_OK) {
		fprintf(stderr, "exec_run\n");
		return 1;
	}
	(void)xtc_exec_fini(exec);

	if (atomic_load(&g_done) != NPROCS) {
		fprintf(stderr, "FAIL: only %d/%d procs done (%ld calls)\n",
		    atomic_load(&g_done), NPROCS, atomic_load(&g_calls));
		return 1;
	}
	printf("ok: %d procs x %d blocking_run = %ld calls\n",
	    NPROCS, ITERS, atomic_load(&g_calls));
	return 0;
}
