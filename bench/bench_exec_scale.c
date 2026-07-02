/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * bench/bench_exec_scale.c
 *	Multi-loop executor scaling benchmark, corrected to isolate the
 *	scheduler from benchmark artifacts (see docs/M_EC2_SCALE.md).
 *
 *	The naive first cut folded every task's completion into ONE global
 *	atomic counter; that single cache line ping-ponged across all
 *	cores and masked the real scaling with false contention.  This
 *	benchmark:
 *	  - uses a PER-LOOP, cache-line-isolated completion counter (no
 *	    shared line touched on the hot path);
 *	  - produces work DISTRIBUTED (each loop's generator spawns its own
 *	    share) so there is no single-producer bottleneck;
 *	  - offers a --reuse mode that RESCHEDULES a fixed worker pool
 *	    instead of spawning per unit, to separate pure scheduler
 *	    throughput from the per-task allocation cost.
 *
 *	Reports tasks/sec and the speedup vs 1 loop, so the scaling curve
 *	across a large core count is directly visible.
 *
 *	Usage:
 *	  bench_exec_scale [loops] [total_tasks] [mode]
 *	    loops       number of executor loops (default: online CPUs)
 *	    total_tasks total leaf units (default 20000000)
 *	    mode        "spawn" (default: one task per unit) or
 *	                "reuse" (a worker pool reschedules; no per-unit
 *	                alloc -- measures the scheduler alone), or
 *	                "churn" (steady-state spawn-as-you-complete with a
 *	                bounded concurrent width -- exercises the per-loop
 *	                task free-list recycling, the realistic pattern)
 */

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_res.h"

#define MAX_LOOPS 512
#define WORKERS_PER_LOOP 64

/* Per-loop counter, each on its own cache line (no false sharing). */
typedef struct {
	_Alignas(64) atomic_long v;
	char pad[64 - sizeof(atomic_long)];
} counter_t;
static counter_t g_ctr[MAX_LOOPS];

static long g_units;           /* per generator (spawn) or per worker (reuse) */
static int  g_reuse;

static void
work_chunk(void)
{
	volatile long a = 0;
	int i;
	for (i = 0; i < 200; i++)
		a += i;
	(void)a;
}

/* spawn mode: a leaf task = one chunk + one per-loop counter bump. */
static int
leaf(xtc_task_t *self, void *u)
{
	(void)self;
	work_chunk();
	atomic_fetch_add_explicit(&g_ctr[(int)(intptr_t)u].v, 1,
	    memory_order_relaxed);
	return XTC_TASK_DONE;
}

struct genarg { xtc_loop_t *loop; int idx; };
static struct genarg g_gen[MAX_LOOPS];

/* spawn-mode generator: spawns its share of leaves on its own loop. */
static int
generator(xtc_task_t *self, void *u)
{
	struct genarg *g = u;
	long i;
	(void)self;
	for (i = 0; i < g_units; i++)
		(void)xtc_task_spawn(g->loop, leaf, (void *)(intptr_t)g->idx,
		    NULL);
	return XTC_TASK_DONE;
}

/*
 * churn-mode leaf: does a chunk, then -- while the loop still owes work
 * -- spawns its REPLACEMENT before completing, keeping a bounded number
 * of tasks concurrently live.  This is the steady-state
 * spawn-as-you-complete pattern (a real server: each finished request
 * makes room for the next), where the per-loop free-list recycles the
 * just-freed struct into the next spawn -- the case the free-list
 * optimizes.  g_units here is the TOTAL units the loop must retire. */
static _Alignas(64) atomic_long g_owed[MAX_LOOPS];
#define CHURN_WIDTH 256   /* concurrent live tasks per loop */
static struct genarg g_churn[MAX_LOOPS];
static int
churn_leaf(xtc_task_t *self, void *u)
{
	struct genarg *g = u;
	(void)self;
	work_chunk();
	atomic_fetch_add_explicit(&g_ctr[g->idx].v, 1, memory_order_relaxed);
	/* Spawn a replacement if the loop still owes retirements. */
	if (atomic_fetch_sub_explicit(&g_owed[g->idx], 1,
	    memory_order_relaxed) > CHURN_WIDTH)
		(void)xtc_task_spawn(g->loop, churn_leaf, g, NULL);
	return XTC_TASK_DONE;
}

/* reuse-mode worker: reschedules for g_units chunks -- no per-unit
 * alloc, so this measures the scheduler's dispatch throughput alone. */
struct wk { long remaining; int idx; };
static struct wk g_wk[MAX_LOOPS * WORKERS_PER_LOOP];

static int
worker(xtc_task_t *self, void *u)
{
	struct wk *w = u;
	(void)self;
	work_chunk();
	atomic_fetch_add_explicit(&g_ctr[w->idx].v, 1, memory_order_relaxed);
	if (--w->remaining > 0)
		return XTC_TASK_RESCHED;
	return XTC_TASK_DONE;
}

int
main(int argc, char **argv)
{
	int loops = argc > 1 ? atoi(argv[1]) :
	    (int)sysconf(_SC_NPROCESSORS_ONLN);
	long total = argc > 2 ? atol(argv[2]) : 20000000L;
	const char *mode = argc > 3 ? argv[3] : "spawn";
	xtc_exec_t *e = NULL;
	struct timespec t0, t1;
	long done = 0;
	double s;
	int i;

	if (loops < 1) loops = 1;
	if (loops > MAX_LOOPS) loops = MAX_LOOPS;
	g_reuse = (strcmp(mode, "reuse") == 0);
	{
		int churn = (strcmp(mode, "churn") == 0);
		(void)churn;
	}
	memset(g_ctr, 0, sizeof g_ctr);

	if (xtc_exec_init(&e, loops) != XTC_OK) {
		fprintf(stderr, "exec_init failed\n");
		return 1;
	}
	for (i = 0; i < loops; i++) {
		xtc_res_caps_t c = XTC_RES_CAPS_DEFAULT;
		c.tasks = total + 1024;
		c.inbox_msgs = total + 1024;
		c.mem_bytes = 200L * 1024 * 1024 * 1024;
		(void)xtc_res_init(xtc_loop_res(xtc_exec_loop(e, i)), &c);
	}

	clock_gettime(CLOCK_MONOTONIC, &t0);
	if (strcmp(mode, "churn") == 0) {
		/* Steady-state: seed CHURN_WIDTH live tasks per loop; each
		 * completing task spawns its replacement until the loop's
		 * owed count is retired.  Exercises free-list recycling. */
		long per = total / loops;
		int j;
		for (i = 0; i < loops; i++) {
			g_churn[i].loop = xtc_exec_loop(e, i);
			g_churn[i].idx = i;
			atomic_store(&g_owed[i], per);
			for (j = 0; j < CHURN_WIDTH; j++)
				(void)xtc_task_spawn(g_churn[i].loop,
				    churn_leaf, &g_churn[i], NULL);
		}
	} else if (g_reuse) {
		long wpl = WORKERS_PER_LOOP;
		g_units = total / loops / wpl;
		for (i = 0; i < loops; i++) {
			xtc_loop_t *l = xtc_exec_loop(e, i);
			long w;
			for (w = 0; w < wpl; w++) {
				struct wk *wk = &g_wk[i * wpl + w];
				wk->remaining = g_units;
				wk->idx = i;
				(void)xtc_task_spawn(l, worker, wk, NULL);
			}
		}
	} else {
		g_units = total / loops;
		for (i = 0; i < loops; i++) {
			g_gen[i].loop = xtc_exec_loop(e, i);
			g_gen[i].idx = i;
			(void)xtc_task_spawn(g_gen[i].loop, generator, &g_gen[i],
			    NULL);
		}
	}
	(void)xtc_exec_run(e);
	clock_gettime(CLOCK_MONOTONIC, &t1);

	for (i = 0; i < loops; i++)
		done += atomic_load(&g_ctr[i].v);
	s = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

	printf("mode=%-5s loops=%-3d done=%-10ld %7.3fs  %8.0f tasks/sec "
	    "(%.2f M/s)\n", mode, loops, done, s, done / s, done / s / 1e6);

	(void)xtc_exec_fini(e);
	/* Self-check: the scheduler must retire essentially every unit.  A
	 * dropped task would silently inflate throughput, so fail loudly.
	 * Allow the integer-division remainder: total/loops (spawn/churn)
	 * or total/loops/WORKERS_PER_LOOP rounded per worker (reuse). */
	{
		long slack = g_reuse ? (long)loops * WORKERS_PER_LOOP : loops;
		if (done < total - slack) {
			fprintf(stderr, "FAIL: done=%ld < expected ~%ld\n",
			    done, total);
			return 1;
		}
	}
	return 0;
}
