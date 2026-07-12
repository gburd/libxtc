/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * bench/bench_win_scale.c
 *	A portable multi-core scale probe buildable on Windows/MSVC as
 *	well as POSIX.  It uses ONLY the public xtc_* API plus
 *	xtc_clock_mono for timing, so it needs neither <unistd.h> /
 *	clock_gettime (which bench_exec_scale.c uses) nor pthreads.
 *
 *	Purpose: give the Windows/MSVC build a multi-core scalability
 *	datapoint (spawn throughput across 1..N loops), so a
 *	Seastar/Tokio-style comparison has a Windows number instead of a
 *	gap.  The richer POSIX-only bench_exec_scale.c stays the
 *	authoritative POSIX/Tokio harness; this is the cross-platform
 *	floor that also runs under cl.exe.
 *
 *	Workload: N driver tasks (one per loop) each spawn K child procs
 *	that immediately exit.  We report spawns/sec at each loop count
 *	so the scaling curve is visible.  On POSIX the spawn path is
 *	mmap/guard-page bound; on Windows it is the CreateFiberEx path.
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"

/* __os_ncpus is the internal, already-Windows-aware CPU count; the
 * public surface has no ncpus accessor and this is a bench (not a
 * consumer example), so using it here is acceptable. */
int __os_ncpus(void);

static _Atomic long g_done;
static _Atomic long g_spawn_fail;
static _Atomic long g_spawn_ok;

/* Child proc: bump the global counter and exit immediately. */
static void
spawn_child(void *arg)
{
	(void)arg;
	(void)xtc_atomic_i64_add((int64_t *)&g_done, 1);
}

/* Per-loop driver task: spawn `k` children onto this loop. */
struct drv { xtc_loop_t *loop; long k; };

static int
spawn_driver(xtc_task_t *self, void *u)
{
	struct drv *d = u;
	long i;
	(void)self;
	for (i = 0; i < d->k; i++) {
		xtc_pid_t child;
		if (xtc_proc_spawn(d->loop, spawn_child, NULL, NULL, &child)
		    != XTC_OK) {
			(void)xtc_atomic_i64_add((int64_t *)&g_spawn_fail, 1);
			break;
		}
		(void)xtc_atomic_i64_add((int64_t *)&g_spawn_ok, 1);
	}
	return XTC_TASK_DONE;
}

static double
now_ms(void)
{
	return (double)xtc_clock_mono() / 1e6;
}

static void
run_spawn(int n_loops, long per_loop)
{
	xtc_exec_t *ex = NULL;
	struct drv drivers[64];
	int i;
	double t0, t1, sec, rate;
	long total = (long)n_loops * per_loop;

	if (n_loops > 64) n_loops = 64;
	if (xtc_exec_init(&ex, n_loops) != XTC_OK) {
		printf("  exec_init(%d) failed\n", n_loops);
		return;
	}
	g_done = 0;
	g_spawn_fail = 0;
	g_spawn_ok = 0;
	t0 = now_ms();
	for (i = 0; i < n_loops; i++) {
		drivers[i].loop = xtc_exec_loop(ex, i);
		drivers[i].k = per_loop;
		(void)xtc_task_spawn(drivers[i].loop, spawn_driver,
		    &drivers[i], NULL);
	}
	(void)xtc_exec_run(ex);
	t1 = now_ms();
	(void)xtc_exec_fini(ex);

	sec = (t1 - t0) / 1000.0;
	rate = sec > 0 ? (double)g_done / sec : 0;
	printf("  loops=%2d  total=%-9ld  %8.1f ms  %12.0f spawns/s"
	    "  (spawn_ok=%ld done=%ld fail=%ld)\n", n_loops, total,
	    t1 - t0, rate, (long)g_spawn_ok, (long)g_done,
	    (long)g_spawn_fail);
}

int
main(int argc, char **argv)
{
	int ncpu = __os_ncpus();
	int loops[8];
	int nl = 0, i;
	long per_loop = (argc > 1) ? atol(argv[1]) : 50000;

	if (ncpu < 1) ncpu = 1;
	printf("bench_win_scale: %d CPUs, backend=%s, per_loop=%ld\n",
	    ncpu, xtc_io_backend_name(), per_loop);

	/* Sweep 1, 2, 4, ... up to ncpu (plus ncpu itself if not a power
	 * of two), capped at 8 points. */
	for (i = 1; i <= ncpu && nl < 8; i *= 2)
		loops[nl++] = i;
	if (nl > 0 && loops[nl - 1] != ncpu && nl < 8)
		loops[nl++] = ncpu;

	printf("== spawn throughput (spawn + immediate exit) ==\n");
	for (i = 0; i < nl; i++)
		run_spawn(loops[i], per_loop);

	printf("done\n");
	return 0;
}
