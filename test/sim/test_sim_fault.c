#define _GNU_SOURCE
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_sim.h"

/*
 * DST phase 5 -- seeded fault injection.  Workers consult xtc_sim_fault
 * at a decision point; the fault schedule is a deterministic function
 * of the seed (drawn from the dedicated FAULT stream), so:
 *   - the SAME seed reproduces the identical set of faulted workers; and
 *   - because the FAULT stream is isolated from the SCHED/PLACE streams,
 *     turning faults on/off does NOT change the scheduling state hash
 *     (per-stream isolation == stable replay regardless of fault config).
 */

#define N_LOOPS   4
#define N_WORKERS 32

static int g_done;
static long g_fault_mask_lo; /* bit i set if worker i "faulted" */

static void
worker(void *arg)
{
	long id = (long)(intptr_t)arg;
	/* Deterministic per-worker fault decision (~25%). */
	if (id < 32 && xtc_sim_fault(250)) {
		long m = xtc_atomic_load_long(&g_fault_mask_lo, xtc_mo_relaxed);
		m |= (1L << id);
		xtc_atomic_store_long(&g_fault_mask_lo, m, xtc_mo_relaxed);
	}
	xtc_yield();
	xtc_atomic_fetch_add_i32(&g_done, 1, xtc_mo_relaxed);
}

static void
run_once(uint64_t seed, int *out_done, long *out_faults, uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	long w;
	xtc_atomic_store_i32(&g_done, 0, xtc_mo_seq_cst);
	xtc_atomic_store_long(&g_fault_mask_lo, 0, xtc_mo_seq_cst);
	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK) { *out_done = -1; return; }
	for (w = 0; w < N_WORKERS; w++) {
		xtc_loop_t *l = xtc_exec_loop(e, (unsigned)(w % N_LOOPS));
		(void)xtc_proc_spawn(l, worker, (void *)(intptr_t)w, NULL, NULL);
	}
	(void)xtc_sim_exec_run(e, seed, 1000000);
	*out_done = xtc_atomic_load_i32(&g_done, xtc_mo_seq_cst);
	*out_faults = xtc_atomic_load_long(&g_fault_mask_lo, xtc_mo_seq_cst);
	*out_state = xtc_sim_state_hash(e);
	(void)xtc_exec_fini(e);
}

int
main(void)
{
	int d1 = 0, d2 = 0;
	long f1 = 0, f2 = 0;
	uint64_t s1 = 0, s2 = 0;

	run_once(0xFEED, &d1, &f1, &s1);
	run_once(0xFEED, &d2, &f2, &s2);

	printf("run1: done=%d faults=0x%08lx state=%016llx\n", d1, f1,
	    (unsigned long long)s1);
	printf("run2: done=%d faults=0x%08lx state=%016llx\n", d2, f2,
	    (unsigned long long)s2);

	if (d1 != N_WORKERS || d2 != N_WORKERS) {
		printf("FAIL: not all workers completed\n");
		return 1;
	}
	if (f1 != f2) {
		printf("FAIL: fault schedule did not replay (0x%lx != 0x%lx)\n", f1, f2);
		return 1;
	}
	if (s1 != s2) {
		printf("FAIL: state hash did not replay\n");
		return 1;
	}
	if (f1 == 0) {
		printf("FAIL: no faults injected (expected ~25%% of 32)\n");
		return 1;
	}
	printf("OK: seeded fault injection replays (faults=0x%08lx of %d "
	       "workers); state hash stable\n", f1, N_WORKERS);
	return 0;
}
