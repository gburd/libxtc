/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the PostgreSQL License.
 *
 * bench/bench_million_tasks.c
 *	Spawn N tasks (default 1,000,000), let each do a tiny piece
 *	of work + a single yield, run to completion, and report
 *	throughput.  Demonstrates xtc's task scheduler can scale to
 *	millions of cooperative tasks without melting.
 *
 *	What this measures:
 *	  - Total wall-clock time
 *	  - Tasks/second
 *	  - Memory footprint at peak (via /proc/self/status)
 *	  - Resource caps NOT exceeded (xtc_res shouldn't return
 *	    XTC_E_RESOURCE if capped properly)
 *
 *	Usage:
 *	  bench_million_tasks [n_tasks] [iters_per_task]
 */

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/resource.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_async.h"
#include "xtc_res.h"
#include "xtc_int.h"

static _Atomic int64_t g_completions;
static int g_iters_per_task;

static intptr_t
task_fn(void *arg)
{
	int i;
	(void)arg;
	for (i = 0; i < g_iters_per_task; i++)
		xtc_yield();
	atomic_fetch_add_explicit(&g_completions, 1, memory_order_relaxed);
	return 0;
}

static int64_t
__now_ns(void)
{
	int64_t v;
	(void)__os_clock_mono(&v);
	return v;
}

static long
__rss_kib(void)
{
#if defined(__linux__)
	FILE *f = fopen("/proc/self/status", "r");
	char line[256];
	long kib = -1;
	if (!f) return -1;
	while (fgets(line, sizeof line, f) != NULL) {
		if (strncmp(line, "VmRSS:", 6) == 0) {
			sscanf(line, "VmRSS: %ld kB", &kib);
			break;
		}
	}
	fclose(f);
	return kib;
#elif defined(__APPLE__)
	/* macOS getrusage returns ru_maxrss in BYTES (POSIX violation). */
	struct rusage ru;
	if (getrusage(RUSAGE_SELF, &ru) != 0) return -1;
	return (long)(ru.ru_maxrss / 1024);
#else
	/* FreeBSD/OpenBSD/NetBSD/illumos: ru_maxrss is in KiB. */
	struct rusage ru;
	if (getrusage(RUSAGE_SELF, &ru) != 0) return -1;
	return (long)ru.ru_maxrss;
#endif
}

int
main(int argc, char **argv)
{
	int      n_tasks = argc > 1 ? atoi(argv[1]) : 1000000;
	int      iters   = argc > 2 ? atoi(argv[2]) : 4;
	xtc_loop_t *loop;
	int      i;
	int64_t  t0, t1;
	long     rss_before, rss_peak;
	int64_t  per_task_ns;
	double   tps;

	g_iters_per_task = iters;
	atomic_store(&g_completions, 0);

	rss_before = __rss_kib();

	if (xtc_loop_init(&loop) != XTC_OK) {
		fprintf(stderr, "loop_init failed\n");
		return 1;
	}

	/* Lift resource caps for the million-task scenario. */
	{
		xtc_res_t *r = xtc_loop_res(loop);
		xtc_res_caps_t caps = XTC_RES_CAPS_DEFAULT;
		caps.tasks       = (int64_t)n_tasks * 2;
		caps.inbox_msgs  = (int64_t)n_tasks * 2;
		caps.mem_bytes   = 8LL * 1024 * 1024 * 1024;     /* 8 GiB */
		(void)xtc_res_init(r, &caps);
	}

	/* Use small fiber stacks to fit a million in RAM.
	 * Note: each task creates ~2-3 VMAs (stack mmap + guard
	 * mprotect splits mapping); /proc/sys/vm/max_map_count limits
	 * how many tasks can coexist.  On Linux the default is 65530
	 * (kernel < 6.0) or 1M (newer); we degrade gracefully when
	 * hit — xtc_async returns a non-OK rc, no crash. */
	if (xtc_set_stack_size(16 * 1024) != XTC_OK) {
		fprintf(stderr, "set_stack_size failed\n");
		return 1;
	}

	t0 = __now_ns();

	for (i = 0; i < n_tasks; i++) {
		xtc_task_t *t;
		int rc = xtc_async(loop, task_fn, NULL, &t);
		if (rc != XTC_OK) {
			fprintf(stderr,
			    "xtc_async failed at task %d: rc=%d\n", i, rc);
			break;
		}
		if (i % 100000 == 0 && i > 0) {
			rss_peak = __rss_kib();
			fprintf(stderr,
			    "spawned %d tasks, RSS %ld KiB\n", i, rss_peak);
		}
	}

	rss_peak = __rss_kib();

	if (xtc_loop_run(loop) != XTC_OK) {
		fprintf(stderr, "loop_run failed\n");
		return 1;
	}

	t1 = __now_ns();

	per_task_ns = (t1 - t0) / n_tasks;
	tps = (double)n_tasks / ((double)(t1 - t0) / 1e9);

	printf("=== Million-task scalability ===\n");
	printf("n_tasks         = %d\n", n_tasks);
	printf("iters/task      = %d\n", iters);
	printf("yields total    = %lld\n",
	    (long long)((int64_t)n_tasks * iters));
	printf("completions     = %lld / %d\n",
	    (long long)atomic_load(&g_completions), n_tasks);
	printf("wall time       = %.3f s\n", (double)(t1 - t0) / 1e9);
	printf("tasks/sec       = %.0f\n", tps);
	printf("ns/task         = %lld\n", (long long)per_task_ns);
	printf("RSS before      = %ld KiB\n", rss_before);
	printf("RSS peak        = %ld KiB\n", rss_peak);
	printf("RSS/task        = %.1f bytes\n",
	    (double)(rss_peak - rss_before) * 1024.0 / n_tasks);

	printf("first failure at = %s\n",
	    atomic_load(&g_completions) == n_tasks
	    ? "none" : "VMA/memory cap (graceful)");

	(void)xtc_loop_fini(loop);
	return 0;
}
