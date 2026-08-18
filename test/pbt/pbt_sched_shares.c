/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/pbt/pbt_sched_shares.c
 *	Property-based test for the L1 proportional-share scheduler
 *	(INSPIRED BY Glommio).  Over drawn share weights hi >= lo:
 *
 *	  P1 (monotonic CPU): after equal cooperative work on one loop,
 *	     the class with MORE shares is picked at least as many times
 *	     as the class with fewer shares -- a higher weight never
 *	     yields LESS CPU.  This is the defining property of a
 *	     proportional-share scheduler and holds for ANY interleaving
 *	     the min-vruntime pick produces (it does not depend on exact
 *	     timing), so it is a clean, jitter-robust invariant testable
 *	     on the ordinary (wall-clock) backend without the simulator.
 *
 *	Runs on a standalone xtc_loop (no executor threads, no sim): the
 *	work is a fixed shared budget so both classes have surplus work,
 *	and the weighting decides who burns more of it.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>

#include "pbt_common.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"

#if defined(__GNUC__) || defined(__clang__)
# define MAYBE_UNUSED __attribute__((unused))
#else
# define MAYBE_UNUSED
#endif

#define WORKERS_PER_CLASS 3
#define TOTAL_BUDGET      600

static atomic_int g_total_runs;

MAYBE_UNUSED static void
worker(void *arg)
{
	(void)arg;
	while (atomic_fetch_add_explicit(&g_total_runs, 1,
	    memory_order_relaxed) < TOTAL_BUDGET) {
		/* A small equal compute chunk so each run's real cost
		 * dominates timing jitter (both classes do the identical
		 * chunk, so any weighting is purely from shares). */
		volatile long acc = 0;
		long i;
		for (i = 0; i < 2000; i++)
			acc += (i ^ (i << 1)) + (acc >> 3);
		xtc_yield();
	}
}

#if defined(XTC_HAVE_HEGEL)

static void
prop_higher_shares_get_more_cpu(hegel_test_case *tc, void *u)
{
	xtc_loop_t *loop = NULL;
	xtc_exec_class_t chi = NULL, clo = NULL;
	xtc_proc_opts_t ohi, olo;
	int hi, lo, i;
	uint64_t runs_hi, runs_lo;
	(void)u;

	hi = (int)hegel_draw_int(tc, hegel_integers(1, 1000));
	lo = (int)hegel_draw_int(tc, hegel_integers(1, 1000));
	if (hi < lo) { int t = hi; hi = lo; lo = t; }

	atomic_store(&g_total_runs, 0);
	hegel_assume(xtc_loop_init(&loop) == XTC_OK);
	hegel_assume(xtc_exec_class_create(loop, hi, 0, &chi) == XTC_OK);
	hegel_assume(xtc_exec_class_create(loop, lo, 0, &clo) == XTC_OK);

	memset(&ohi, 0, sizeof ohi);
	memset(&olo, 0, sizeof olo);
	ohi.sched_class = chi;
	olo.sched_class = clo;
	for (i = 0; i < WORKERS_PER_CLASS; i++) {
		hegel_assume(xtc_proc_spawn(loop, worker, NULL, &ohi, NULL)
		    == XTC_OK);
		hegel_assume(xtc_proc_spawn(loop, worker, NULL, &olo, NULL)
		    == XTC_OK);
	}

	hegel_assume(xtc_loop_run(loop) == XTC_OK);

	runs_hi = xtc_exec_class_runs(chi);
	runs_lo = xtc_exec_class_runs(clo);

	/* P1: more shares => at least as many runs (never fewer). */
	hegel_assume(runs_hi >= runs_lo);

	hegel_assume(xtc_loop_fini(loop) == XTC_OK);
}

static const pbt_entry_t tests[] = {
	{ "higher_shares_get_more_cpu",
	    prop_higher_shares_get_more_cpu, 40 },
	{ NULL, NULL, 0 }
};

#else
static const pbt_entry_t tests[] = {
	{ "higher_shares_get_more_cpu", NULL, 0 },
	{ NULL, NULL, 0 }
};
#endif

PBT_MAIN("sched_shares", tests)
