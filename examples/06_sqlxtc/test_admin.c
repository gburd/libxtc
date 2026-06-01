/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_admin.c
 *	In-process test of the "SHOW PROCESSES" admin command.  No
 *	daemon, no socket: spawn a few procs on one loop (workers park
 *	on a timer so they stay alive), then from a driver proc call
 *	admin_show_processes into a Quack buffer and assert it emitted
 *	at least one row per live worker.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"

#include "admin.h"
#include "quack.h"

#define N_WORKERS 3

static int     g_failed = 0;
static int     g_nonempty = 0;
static int64_t g_rows = 0;

/* Worker: park on a timer long enough for the driver to snapshot it. */
static void
worker_proc(void *arg)
{
	(void)arg;
	(void)xtc_proc_sleep(200LL * 1000 * 1000);
}

/* Count non-overlapping occurrences of `needle` in [hay, hay+n). */
static size_t
count_substr(const char *hay, size_t n, const char *needle)
{
	size_t nlen = strlen(needle);
	size_t cnt = 0;
	size_t i;

	if (nlen == 0 || n < nlen) return 0;
	for (i = 0; i + nlen <= n; i++) {
		if (memcmp(hay + i, needle, nlen) == 0) {
			cnt++;
			i += nlen - 1;
		}
	}
	return cnt;
}

/* Driver: yield so the workers park, then render SHOW PROCESSES. */
static void
driver_proc(void *arg)
{
	quack_buf_t buf;
	(void)arg;

	/* Let the workers get scheduled and park on their long sleep. */
	(void)xtc_proc_sleep(20LL * 1000 * 1000);

	if (quack_buf_init(&buf, 256) < 0) {
		g_failed = 1;
		return;
	}

	if (admin_show_processes(&buf) != 0) {
		g_failed = 1;
		quack_buf_free(&buf);
		return;
	}

	g_nonempty = (buf.len > 0);
	g_rows = (int64_t)count_substr(buf.p, buf.len, "{\"row\":[");

	quack_buf_free(&buf);
}

int
main(void)
{
	xtc_loop_t     *loop = NULL;
	xtc_proc_opts_t opts = { 0 };
	xtc_pid_t       pid;
	int             i;

	if (xtc_loop_init(&loop) != XTC_OK) {
		fprintf(stderr, "FAIL: loop_init\n");
		return 1;
	}

	for (i = 0; i < N_WORKERS; i++) {
		opts.name = "worker";
		if (xtc_proc_spawn(loop, worker_proc, NULL, &opts, &pid)
		    != XTC_OK) {
			fprintf(stderr, "FAIL: spawn worker %d\n", i);
			return 1;
		}
	}

	opts.name = "driver";
	if (xtc_proc_spawn(loop, driver_proc, NULL, &opts, &pid) != XTC_OK) {
		fprintf(stderr, "FAIL: spawn driver\n");
		return 1;
	}

	if (xtc_loop_run(loop) != XTC_OK) {
		fprintf(stderr, "FAIL: loop_run\n");
		return 1;
	}
	(void)xtc_loop_fini(loop);

	if (g_failed) {
		fprintf(stderr, "FAIL: driver reported an error\n");
		return 1;
	}
	if (!g_nonempty) {
		fprintf(stderr, "FAIL: emitted buffer was empty\n");
		return 1;
	}
	if (g_rows < N_WORKERS) {
		fprintf(stderr, "FAIL: expected >= %d proc rows, got %lld\n",
		    N_WORKERS, (long long)g_rows);
		return 1;
	}

	printf("  ok   admin_show_processes emitted %lld proc rows "
	    "(>= %d live procs)\n", (long long)g_rows, N_WORKERS);
	printf("All sqlxtc admin tests passed.\n");
	return 0;
}
