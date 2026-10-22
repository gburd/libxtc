/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/sim/test_sim_stop_tls.c
 *	After a run that a FIBER stopped (xtc_exec_stop from inside a proc,
 *	the "crash the run here" idiom of the composition-crash DST), the
 *	calling thread is no longer on a proc: xtc_self() is XTC_PID_NONE,
 *	xtc_mask_enter / xtc_mask_leave return XTC_E_INVAL (no-ops), and
 *	none of them touches the stopped proc -- which xtc_exec_fini frees.
 *
 *	The scheduler left this thread's current-proc pointer naming the
 *	last proc it ran when that proc stopped the executor.  Once the
 *	exec was finalized the pointer dangled: xtc_self() returned a dead
 *	proc's pid, and xtc_mask_enter (reached by an embedder's engine
 *	locks on the main thread after the run) wrote its mask depth into
 *	freed memory -- the heap corruption ("free(): invalid next size")
 *	that failed test_sim_compose_crash 12/12 seeds.
 *
 *	Per seed: N loops, M procs that each yield a few times; one of them
 *	stops the executor mid-run.  After xtc_sim_exec_run and again after
 *	xtc_exec_fini the checks above must hold.  Run it under valgrind or
 *	ASan for the use-after-free half; the value checks fail without
 *	any tool.
 */

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "xtc.h"
#include "xtc_async.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_sim.h"

#define N_LOOPS 3
#define N_PROCS 6

static xtc_exec_t *g_exec;
static _Atomic int g_steps;

static void
worker(void *arg)
{
	int i;
	(void)arg;
	for (i = 0; i < 50; i++) {
		if (atomic_fetch_add(&g_steps, 1) == 40)
			(void)xtc_exec_stop(g_exec);   /* stop the run from a fiber */
		xtc_yield();
	}
}

static int
check(const char *when)
{
	int bad = 0;
	if (!xtc_pid_is_none(xtc_self())) {
		fprintf(stderr, "  %s: xtc_self() is a proc, not NONE\n", when);
		bad = 1;
	}
	if (xtc_mask_enter() != XTC_E_INVAL) {
		fprintf(stderr, "  %s: xtc_mask_enter acted on a proc\n", when);
		bad = 1;
	}
	if (xtc_mask_leave() != XTC_E_INVAL) {
		fprintf(stderr, "  %s: xtc_mask_leave acted on a proc\n", when);
		bad = 1;
	}
	if (xtc_cancel_requested() != 0) {
		fprintf(stderr, "  %s: xtc_cancel_requested read a proc\n", when);
		bad = 1;
	}
	return bad;
}

static int
run_one(uint64_t seed)
{
	int i, bad = 0, rc;
	atomic_store(&g_steps, 0);
	if (xtc_exec_init(&g_exec, N_LOOPS) != XTC_OK)
		return 1;
	for (i = 0; i < N_PROCS; i++)
		if (xtc_proc_spawn(xtc_exec_loop(g_exec, i % N_LOOPS), worker,
		    NULL, NULL, NULL) != XTC_OK)
			return 1;
	rc = xtc_sim_exec_run(g_exec, seed, 1000000);
	(void)rc;               /* stopped early: any return is fine here */
	bad |= check("after the run");
	(void)xtc_exec_fini(g_exec);
	g_exec = NULL;
	bad |= check("after xtc_exec_fini");
	return bad;
}

int
main(void)
{
	uint64_t s;
	int fails = 0, n = 0;
	for (s = 1; s <= 8; s++, n++)
		if (run_one(0x5709u * s) != 0) {
			fprintf(stderr, "  seed %llu: FAIL\n", (unsigned long long)s);
			fails++;
		}
	if (fails) {
		printf("FAIL: %d/%d seeds: the thread still names a proc after a "
		    "fiber stopped the run\n", fails, n);
		return 1;
	}
	printf("OK: stop-from-fiber DST -- %d seeds: after the run and after "
	    "exec_fini the caller is off-proc (self NONE, mask no-op)\n", n);
	return 0;
}
