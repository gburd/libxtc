/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * bench/bench_mem_per_task.c
 *	Measure libxtc's MEMORY PER CONCURRENT TASK -- the axis where the
 *	stackful-fiber model is known to cost more than a stackless
 *	runtime (Tokio's Future is tens of bytes; an xtc fiber needs a
 *	real stack).  This benchmark quantifies the actual cost so it is
 *	a measured number, not a guess, and shows how it scales with the
 *	configured fiber stack size.
 *
 *	Method: take a baseline RSS, spawn N fibers that each park
 *	immediately (so all N are live simultaneously), measure RSS
 *	again, and report (delta_RSS / N) = marginal bytes per live
 *	task.  Repeat across a sweep of stack sizes via
 *	xtc_set_stack_size().  Park-immediately keeps every fiber's
 *	stack committed at its minimum, isolating the structural cost.
 *
 *	Usage: bench_mem_per_task [n_tasks]   (default 100000)
 *
 *	Interpreting the result: the per-task cost is dominated by the
 *	fiber stack.  A smaller stack -> less per task, but too small
 *	risks overflow for deep call chains; xtc enforces a floor.  This
 *	is the libxtc-vs-Tokio memory trade: simpler programming model
 *	(ordinary C call stacks, no async coloring) for a higher
 *	per-task floor.  Compare the printed bytes/task against Tokio's
 *	~order-of-100-bytes-per-task to size the gap for your workload.
 */

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_async.h"
#include "xtc_proc.h"
#include "xtc_res.h"

static atomic_int g_parked;
static xtc_loop_t *g_loop;

/* A fiber that signals it is live, then parks forever (until the loop
 * tears down).  All N are simultaneously resident, so RSS reflects N
 * live fiber stacks. */
static void
parker(void *arg)
{
	(void)arg;
	atomic_fetch_add_explicit(&g_parked, 1, memory_order_relaxed);
	/* Park indefinitely: recv with no sender ever -- the loop will be
	 * torn down to release us.  This keeps the stack live + minimal. */
	{
		void *m = NULL;
		size_t n = 0;
		(void)xtc_recv(&m, &n, 60LL * 1000 * 1000 * 1000);  /* 60s cap */
	}
}

static long
rss_kib(void)
{
	FILE *f = fopen("/proc/self/status", "r");
	char line[256];
	long kib = -1;
	if (f == NULL)
		return -1;
	while (fgets(line, sizeof line, f) != NULL)
		if (strncmp(line, "VmRSS:", 6) == 0) {
			(void)sscanf(line, "VmRSS: %ld kB", &kib);
			break;
		}
	(void)fclose(f);
	return kib;
}

static void
measure(int n_tasks, size_t stack_bytes)
{
	xtc_res_caps_t caps = XTC_RES_CAPS_DEFAULT;
	long rss_before, rss_after;
	int i;

	(void)xtc_set_stack_size(stack_bytes);
	atomic_store(&g_parked, 0);

	if (xtc_loop_init(&g_loop) != XTC_OK) {
		fprintf(stderr, "loop_init failed\n");
		return;
	}
	/* Lift caps so N parkers can all live at once. */
	caps.tasks = (int64_t)n_tasks * 2 + 1024;
	caps.inbox_msgs = (int64_t)n_tasks * 2 + 1024;
	caps.mem_bytes = 8LL * 1024 * 1024 * 1024;   /* 8 GiB measurement headroom */
	(void)xtc_res_init(xtc_loop_res(g_loop), &caps);

	rss_before = rss_kib();

	for (i = 0; i < n_tasks; i++)
		if (xtc_proc_spawn(g_loop, parker, NULL, NULL, NULL) != XTC_OK)
			break;

	/* Step the loop once-ish so each fiber runs to its park.  We do
	 * not run to completion (the parkers never finish); instead run a
	 * bounded number of steps so all spawn + first-run, then sample. */
	{
		extern int __xtc_loop_step_once(xtc_loop_t *loop);
		int spun = 0;
		while (atomic_load(&g_parked) < i && spun < n_tasks * 4) {
			(void)__xtc_loop_step_once(g_loop);
			spun++;
		}
	}

	rss_after = rss_kib();

	{
		int live = atomic_load(&g_parked);
		double per_task = (live > 0 && rss_after > rss_before)
		    ? ((double)(rss_after - rss_before) * 1024.0) / live
		    : 0.0;
		printf("stack=%6zuKiB  tasks=%-8d live=%-8d  "
		    "rss_delta=%6ldKiB  bytes/task=%8.0f\n",
		    stack_bytes / 1024, n_tasks, live,
		    rss_after - rss_before, per_task);
	}

	(void)xtc_loop_fini(g_loop);
	g_loop = NULL;
}

int
main(int argc, char **argv)
{
	int n = argc > 1 ? atoi(argv[1]) : 100000;
	size_t sweep[] = { 16 * 1024, 32 * 1024, 64 * 1024, 128 * 1024 };
	size_t i;

	if (n < 1)
		n = 1;

	printf("# libxtc memory-per-task (stackful fibers): %d parked "
	    "fibers per run\n", n);
	printf("# bytes/task is the marginal RSS cost of one live fiber, "
	    "dominated by its stack.\n");
	printf("# Compare against a stackless runtime (Tokio: ~10^2 "
	    "bytes/task) to size the trade.\n\n");

	for (i = 0; i < sizeof sweep / sizeof sweep[0]; i++)
		measure(n, sweep[i]);

	return 0;
}
