/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * bench/conformance/w1_spawn/xtc/main.c
 *   M17 W1 — spawn-N-await-all, xtc runtime.
 *
 *   Spawns N tasks via xtc_task_spawn; each task increments an atomic
 *   counter and returns XTC_TASK_DONE.  Measures wall-time elapsed,
 *   CPU time (user + sys), and peak RSS, then emits one line on stdout
 *   in the M17 results format.
 *
 * Build:
 *   cd bench/conformance/w1_spawn/xtc && make
 *
 * Usage:
 *   ./bench                  # N=10000 (default)
 *   ./bench --N=50000
 *   ./bench --params=N=50000
 */

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

#include "xtc.h"
#include "xtc_loop.h"

/* -------------------------------------------------------------------------
 * Task payload
 * ------------------------------------------------------------------------- */

static atomic_int g_done;

static int
task_bump(xtc_task_t *self, void *user)
{
	(void)self;
	(void)user;
	atomic_fetch_add_explicit(&g_done, 1, memory_order_relaxed);
	return XTC_TASK_DONE;
}

/* -------------------------------------------------------------------------
 * Timing helpers
 * ------------------------------------------------------------------------- */

static uint64_t
mono_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * UINT64_C(1000000000) + (uint64_t)ts.tv_nsec;
}

/* -------------------------------------------------------------------------
 * Argument parsing
 * Accepts:  --N=<int>  or  --params=N=<int>
 * ------------------------------------------------------------------------- */

static long
parse_n(int argc, char *argv[])
{
	int i;
	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (strncmp(a, "--N=", 4) == 0)
			return atol(a + 4);
		if (strncmp(a, "--params=N=", 11) == 0)
			return atol(a + 11);
		/* Also accept --params=N=<int> with extra keys joined by ':' */
		if (strncmp(a, "--params=", 9) == 0) {
			const char *p = strstr(a + 9, "N=");
			if (p != NULL)
				return atol(p + 2);
		}
	}
	return 10000;
}

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

int
main(int argc, char *argv[])
{
	long          N;
	xtc_loop_t   *loop = NULL;
	long          i;
	uint64_t      t0_ns, t1_ns, elapsed_ns;
	struct rusage ru;
	uint64_t      cpu_us, rss_kb;

	N = parse_n(argc, argv);
	if (N <= 0) N = 10000;

	atomic_store_explicit(&g_done, 0, memory_order_relaxed);

	if (xtc_loop_init(&loop) != XTC_OK) {
		fprintf(stderr, "xtc_loop_init failed\n");
		return 1;
	}

	/* ---- start wall clock ---- */
	t0_ns = mono_ns();

	for (i = 0; i < N; i++) {
		if (xtc_task_spawn(loop, task_bump, NULL, NULL) != XTC_OK) {
			fprintf(stderr, "xtc_task_spawn failed at i=%ld\n", i);
			(void)xtc_loop_fini(loop);
			return 1;
		}
	}

	if (xtc_loop_run(loop) != XTC_OK) {
		fprintf(stderr, "xtc_loop_run failed\n");
		(void)xtc_loop_fini(loop);
		return 1;
	}

	/* ---- stop wall clock ---- */
	t1_ns = mono_ns();

	(void)xtc_loop_fini(loop);

	/* ---- resource usage ---- */
	if (getrusage(RUSAGE_SELF, &ru) != 0) {
		fprintf(stderr, "getrusage failed\n");
		return 1;
	}

	elapsed_ns = t1_ns - t0_ns;
	cpu_us     = (uint64_t)(ru.ru_utime.tv_sec  + ru.ru_stime.tv_sec)
	             * UINT64_C(1000000)
	           + (uint64_t)(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec);
	/* Linux: ru_maxrss is in KiB already. */
	rss_kb     = (uint64_t)ru.ru_maxrss;

	/* ---- emit M17 result line ---- */
	printf("workload=W1 runtime=xtc params=N=%ld"
	       " elapsed_ns=%llu cpu_us=%llu rss_kb=%llu"
	       " p50_ns=0 p95_ns=0 p99_ns=0 p999_ns=0\n",
	       N,
	       (unsigned long long)elapsed_ns,
	       (unsigned long long)cpu_us,
	       (unsigned long long)rss_kb);

	return 0;
}
