/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * bench/bench_timer_scale.c
 *	Timer-heap scaling microbenchmark.  Proves (or disproves) that
 *	libxtc's per-loop timer heap is share-nothing: each loop owns its
 *	own xtc_timer_t min-heap on its own xtc_loop_t, so timers set /
 *	cancelled / fired on loop i touch no state shared with loop j.
 *	If that holds, aggregate timers/sec must scale near-linearly with
 *	the loop count (1, 2, 4, 8, ...) -- there is no global timer lock
 *	to serialize on.  A flat or sublinear curve would expose a hidden
 *	cross-loop contention wall.
 *
 *	Each loop runs a task that, for g_rounds rounds, arms BATCH timers
 *	(short deadlines), cancels half of them (exercising the lazy-delete
 *	+ heap path), then runs the loop so the survivors fire.  The
 *	per-loop counter is cache-line isolated (no false sharing), exactly
 *	as bench_exec_scale does, so the measured curve is the timer
 *	subsystem, not a benchmark artifact.
 *
 *	Reports timers/sec and speedup vs 1 loop.
 *
 *	Usage:
 *	  bench_timer_scale [loops] [rounds] [batch]
 *	    loops   number of executor loops   (default: online CPUs)
 *	    rounds  arm/cancel/fire rounds/loop (default: 2000)
 *	    batch   timers armed per round      (default: 512)
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
#define MAX_BATCH 4096

/* Per-loop fired-timer counter, each on its own cache line. */
typedef struct {
	_Alignas(64) atomic_long v;
	char pad[64 - sizeof(atomic_long)];
} counter_t;
static counter_t g_ctr[MAX_LOOPS];

static long g_rounds;
static int  g_batch;

struct loopwork {
	xtc_loop_t *loop;
	int         idx;
	long        round;
	xtc_timer_t *held[MAX_BATCH];    /* handles for this round's cancels */
};
static struct loopwork g_work[MAX_LOOPS];

static void
fire_cb(void *u)
{
	int idx = (int)(intptr_t)u;
	atomic_fetch_add_explicit(&g_ctr[idx].v, 1, memory_order_relaxed);
}

/*
 * The driver task: one round arms `batch` timers with tiny, staggered
 * deadlines, cancels the even-indexed half (lazy delete + heap skip),
 * then RESCHED's.  When it reschedules the loop drains the run queue,
 * finds the queue empty, and blocks in xtc_io_poll until the nearest
 * timer deadline -- i.e. the survivors fire via the unified deadline
 * park path (B3) -- before the task runs its next round.  After
 * g_rounds rounds the task completes.
 *
 * All timer state (the heap array, n_timers, the slab) lives on
 * work->loop; nothing here touches another loop's loop struct, so N of
 * these run fully in parallel if the subsystem is share-nothing.
 */
static int
driver(xtc_task_t *self, void *u)
{
	struct loopwork *w = u;
	int i;
	(void)self;

	if (w->round >= g_rounds)
		return XTC_TASK_DONE;
	w->round++;

	for (i = 0; i < g_batch; i++) {
		xtc_timer_t *t = NULL;
		/* Staggered short deadlines: 0..(batch-1) microseconds. */
		(void)xtc_timer_set(w->loop, (int64_t)i * 1000,
		    fire_cb, (void *)(intptr_t)w->idx, &t);
		w->held[i] = t;
	}
	/* Cancel the even half -- exercises O(1) lazy cancel + the
	 * skip-cancelled path on the next heap extraction. */
	for (i = 0; i < g_batch; i += 2)
		(void)xtc_timer_cancel(w->held[i]);

	return XTC_TASK_RESCHED;
}

int
main(int argc, char **argv)
{
	int loops = argc > 1 ? atoi(argv[1]) :
	    (int)sysconf(_SC_NPROCESSORS_ONLN);
	long rounds = argc > 2 ? atol(argv[2]) : 2000L;
	int batch = argc > 3 ? atoi(argv[3]) : 512;
	xtc_exec_t *e = NULL;
	struct timespec t0, t1;
	long fired = 0, armed;
	double s;
	int i;

	if (loops < 1) loops = 1;
	if (loops > MAX_LOOPS) loops = MAX_LOOPS;
	if (batch < 2) batch = 2;
	if (batch > MAX_BATCH) batch = MAX_BATCH;
	g_rounds = rounds;
	g_batch = batch;
	memset(g_ctr, 0, sizeof g_ctr);

	if (xtc_exec_init(&e, loops) != XTC_OK) {
		fprintf(stderr, "exec_init failed\n");
		return 1;
	}
	for (i = 0; i < loops; i++) {
		xtc_res_caps_t c = XTC_RES_CAPS_DEFAULT;
		/* Each round arms `batch` timers; the loop holds at most one
		 * round's worth live at a time, but timer structs live on the
		 * per-loop slab / all_timers until fini, so budget generously. */
		c.tasks = 1024;
		c.mem_bytes = 8L * 1024 * 1024 * 1024;
		(void)xtc_res_init(xtc_loop_res(xtc_exec_loop(e, i)), &c);
	}

	clock_gettime(CLOCK_MONOTONIC, &t0);
	for (i = 0; i < loops; i++) {
		g_work[i].loop = xtc_exec_loop(e, i);
		g_work[i].idx = i;
		g_work[i].round = 0;
		/* Pin: the driver mutates loop i's OWNER-ONLY timer heap, so
		 * it must stay on loop i.  A plain xtc_task_spawn goes on the
		 * stealable deque and a thief on loop j would then mutate loop
		 * i's heap from the wrong thread (data race).  Pinning is also
		 * the honest model: timers belong to the loop that owns them. */
		(void)xtc_exec_spawn_on(e, i, driver, &g_work[i], NULL);
	}
	(void)xtc_exec_run(e);
	clock_gettime(CLOCK_MONOTONIC, &t1);

	for (i = 0; i < loops; i++)
		fired += atomic_load(&g_ctr[i].v);
	s = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec - t0.tv_nsec) / 1e9;

	/* Timers ARMED = loops * rounds * batch; half fire, half cancel. */
	armed = (long)loops * rounds * batch;

	printf("loops=%-3d rounds=%-6ld batch=%-4d armed=%-11ld fired=%-11ld "
	    "%7.3fs  %10.0f timers/sec (%.2f M/s)\n",
	    loops, rounds, batch, armed, fired, s,
	    armed / s, armed / s / 1e6);

	(void)xtc_exec_fini(e);

	/* Self-check: exactly the odd (un-cancelled) half must fire.  A
	 * miscount would mean the cancel path or the deadline-park firing
	 * path is broken, so fail loudly. */
	{
		long expect = (long)loops * rounds * (batch / 2);
		if (fired != expect) {
			fprintf(stderr, "FAIL: fired=%ld != expected %ld\n",
			    fired, expect);
			return 1;
		}
	}
	return 0;
}
