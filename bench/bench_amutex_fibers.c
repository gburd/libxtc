/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * bench/bench_amutex_fibers.c
 *	One hot xtc_amutex, contended by FIBERS instead of plain OS threads:
 *	the workaround KNOWN_ISSUES gives for 1.59's single-hot-amutex cost.
 *	bench_sync_offloop measures the plain-thread case (`amutex w N`);
 *	this runs the same critical section (8 counter increments plus CS
 *	spin iterations, then THINK spin iterations outside) from N fibers
 *	spread over L loops of one executor.  A contended fiber parks on the
 *	loop instead of sleeping its OS thread, so a release hands the mutex
 *	to a parked fiber without waking a sleeping thread.
 *
 *	usage: bench_amutex_fibers LOOPS FIBERS [SECS] [CS] [THINK]
 *	prints: loops=L fibers=N ops=X ops_per_s=Y violations=V
 */

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "xtc.h"
#include "xtc_async.h"
#include "xtc_exec.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_sync.h"

static xtc_amutex_t *g_m;
static int g_cs = 20, g_think = 200;
static _Atomic int g_stop, g_inside, g_viol;
static _Atomic uint64_t g_ops;
static uint64_t g_data[8];
static int64_t g_end_ns;

static void
spin_work(int n)
{
	volatile uint64_t x = 0;
	int i;

	for (i = 0; i < n; i++)
		x += (uint64_t)i;
}

static void
worker(void *arg)
{
	uint64_t ops = 0;
	int i;

	(void)arg;
	while (!atomic_load_explicit(&g_stop, memory_order_relaxed)) {
		(void)xtc_amutex_lock(g_m, -1);
		if (atomic_fetch_add_explicit(&g_inside, 1,
		    memory_order_relaxed) != 0)
			atomic_store(&g_viol, 1);
		for (i = 0; i < 8; i++)
			g_data[i]++;
		spin_work(g_cs);
		atomic_fetch_sub_explicit(&g_inside, 1, memory_order_relaxed);
		(void)xtc_amutex_unlock(g_m);
		ops++;
		spin_work(g_think);
		if ((ops & 63) == 0) {
			if (xtc_clock_mono() >= g_end_ns)
				atomic_store(&g_stop, 1);
			xtc_yield();    /* let the loop's other fibers in */
		}
	}
	atomic_fetch_add(&g_ops, ops);
}

int
main(int argc, char **argv)
{
	xtc_exec_t *e = NULL;
	int loops, fibers, secs, i;
	int64_t t0, t1;

	if (argc < 3) {
		fprintf(stderr, "usage: bench_amutex_fibers LOOPS FIBERS "
		    "[SECS] [CS] [THINK]\n");
		return 2;
	}
	loops = atoi(argv[1]);
	fibers = atoi(argv[2]);
	secs = argc > 3 ? atoi(argv[3]) : 3;
	if (argc > 4)
		g_cs = atoi(argv[4]);
	if (argc > 5)
		g_think = atoi(argv[5]);
	if (loops < 1 || fibers < 1 || secs < 1)
		return 2;
	if (xtc_amutex_create(&g_m) != XTC_OK ||
	    xtc_exec_init(&e, loops) != XTC_OK)
		return 1;
	for (i = 0; i < fibers; i++)
		if (xtc_proc_spawn(xtc_exec_loop(e, i % loops), worker, NULL,
		    NULL, NULL) != XTC_OK)
			return 1;
	t0 = xtc_clock_mono();
	g_end_ns = t0 + (int64_t)secs * 1000000000LL;
	(void)xtc_exec_run(e);
	t1 = xtc_clock_mono();
	printf("loops=%d fibers=%d ops=%llu ops_per_s=%.0f violations=%d\n",
	    loops, fibers, (unsigned long long)atomic_load(&g_ops),
	    (double)atomic_load(&g_ops) * 1e9 / (double)(t1 - t0),
	    atomic_load(&g_viol));
	(void)xtc_exec_fini(e);
	xtc_amutex_destroy(g_m);
	return atomic_load(&g_viol) ? 1 : 0;
}
