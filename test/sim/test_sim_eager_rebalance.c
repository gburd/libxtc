/*
 * test/sim/test_sim_eager_rebalance.c
 *
 * DST proof for xtc_exec_set_eager_rebalance (the PG
 * wake-on-stealable-work request), closing the gap identified in
 * review: the real fix (__xtc_loop_step's run-queue-empty
 * steal-before-block, reached regardless of a loop's own pending
 * timer/parked fibers) was not exercised under the deterministic
 * simulator, because __sim_loop_runnable's steal-eligibility clause
 * required NO pending timer/io on the candidate thief -- exactly the
 * "owns parked fibers" case eager rebalance targets.  __sim_loop_runnable
 * now has an eager-aware branch that mirrors __xtc_loop_step's real
 * condition, so xtc_sim_exec_run drives the REAL production steal code
 * under a seed, deterministically.
 *
 * Shape (mirrors test/concurrency/test_eager_rebalance.c, under sim):
 *   - LOOPS loops.  Loop 0 gets a migratable-worker backlog (WORKERS
 *     procs that each yield a few times).  Every other loop gets a
 *     PARKER proc parked on a timer -- so those loops are never "fully
 *     idle" (they have a pending deadline), which is exactly the
 *     condition that defeats stealing without eager rebalance.
 *
 * Invariants:
 *   INV1 (liveness, eager ON): total steals > 0 -- the backlog is
 *        actually rebalanced onto the parked-fiber loops.
 *   INV2 (honest baseline, eager OFF): total steals == 0 for this SAME
 *        shape+seed -- proves the gap is real (not vacuous) and that
 *        the default (off) is unchanged.
 *   INV3 (determinism): the same seed with eager ON replays
 *        byte-identically (state hash + steal count) across two runs.
 *
 * Adversarial proof (see the harness script referenced in the review):
 * reverting __sim_loop_runnable's eager clause (or __xtc_loop_step's
 * real fix) makes INV1 fail deterministically -- this test is the DST
 * regression guard for the eager-rebalance liveness fix, not just a
 * feature demo.
 */
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_sim.h"

#define LOOPS       4
#define WORKERS     32
#define WORK_YIELDS 10

static atomic_int g_workers_done;

static void
worker(void *arg)
{
	int i;
	(void)arg;
	for (i = 0; i < WORK_YIELDS; i++)
		xtc_yield();
	atomic_fetch_add_explicit(&g_workers_done, 1, memory_order_relaxed);
}

/* Parks on a timer well past the workers' run -- gives its loop a
 * pending deadline (n_alive > 0, not "fully idle"), the exact
 * condition that defeats stealing without eager rebalance. */
static void
parker(void *arg)
{
	(void)arg;
	(void)xtc_proc_sleep(500LL * 1000 * 1000);   /* 500ms virtual */
}

static void
spawner(void *arg)
{
	xtc_exec_t *e = (xtc_exec_t *)arg;
	xtc_proc_opts_t mopts;
	int i;

	for (i = 1; i < LOOPS; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e, (unsigned)i), parker,
		    NULL, NULL, NULL);

	memset(&mopts, 0, sizeof mopts);
	mopts.migratable = 1;
	for (i = 0; i < WORKERS; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e, 0), worker, NULL,
		    &mopts, NULL);
}

/* Run once under the deterministic sim; return total steals across all
 * loops, or -1 on error.  *out_state / *out_done report replay data. */
static long
run_once(uint64_t seed, int eager, uint64_t *out_state, int *out_done)
{
	xtc_exec_t *e = NULL;
	long total_steals = 0;
	int i, rc;

	atomic_store(&g_workers_done, 0);
	if (xtc_exec_init(&e, LOOPS) != XTC_OK)
		return -1;
	xtc_exec_set_eager_rebalance(e, eager);

	if (xtc_proc_spawn(xtc_exec_loop(e, 0), spawner, e, NULL, NULL)
	    != XTC_OK) {
		(void)xtc_exec_fini(e);
		return -1;
	}

	rc = xtc_sim_exec_run(e, seed, 2000000);
	if (rc != XTC_OK) {
		(void)xtc_exec_fini(e);
		return -1;
	}

	for (i = 0; i < LOOPS; i++) {
		xtc_loop_stats_t st;
		if (xtc_exec_loop_stats(e, i, &st) == XTC_OK)
			total_steals += (long)st.steals;
	}
	if (out_state) *out_state = xtc_sim_state_hash(e);
	if (out_done) *out_done = atomic_load(&g_workers_done);
	(void)xtc_exec_fini(e);
	return total_steals;
}

int
main(void)
{
	long steals_off, steals_on1, steals_on2;
	uint64_t state_off = 0, state_on1 = 0, state_on2 = 0;
	int done_off = 0, done_on1 = 0, done_on2 = 0;
	const uint64_t seed = 0xD57ULL;

	steals_off = run_once(seed, 0, &state_off, &done_off);
	steals_on1 = run_once(seed, 1, &state_on1, &done_on1);
	steals_on2 = run_once(seed, 1, &state_on2, &done_on2);

	printf("eager-off: steals=%ld done=%d/%d state=%016llx\n",
	    steals_off, done_off, WORKERS, (unsigned long long)state_off);
	printf("eager-on (run1): steals=%ld done=%d/%d state=%016llx\n",
	    steals_on1, done_on1, WORKERS, (unsigned long long)state_on1);
	printf("eager-on (run2): steals=%ld done=%d/%d state=%016llx\n",
	    steals_on2, done_on2, WORKERS, (unsigned long long)state_on2);

	if (steals_off < 0 || steals_on1 < 0 || steals_on2 < 0) {
		printf("FAIL: a sim run errored (bad rc from xtc_sim_exec_run "
		    "or setup)\n");
		return 1;
	}
	if (done_off != WORKERS || done_on1 != WORKERS || done_on2 != WORKERS) {
		printf("FAIL: not all workers completed\n");
		return 1;
	}

	/* INV2: the default (eager off) is unchanged -- zero steals for
	 * this exact parked-fiber shape (the reported gap, reproduced). */
	if (steals_off != 0) {
		printf("FAIL[INV2]: eager-off produced %ld steals (want 0) -- "
		    "the default-off baseline changed\n", steals_off);
		return 1;
	}

	/* INV1: eager rebalance is a genuine liveness fix under DST now --
	 * the real __xtc_loop_step steal-before-block fires for a loop that
	 * owns a parked (timer-waiting) fiber, driven by the seeded sim. */
	if (steals_on1 == 0) {
		printf("FAIL[INV1]: eager-on produced ZERO steals -- the "
		    "steal-before-block fix is not reaching the sim-driven "
		    "production code path (regression in __sim_loop_runnable's "
		    "eager clause or __xtc_loop_step itself)\n");
		return 1;
	}

	/* INV3: determinism -- same seed, same policy, byte-identical
	 * replay (steal count AND structural state hash). */
	if (steals_on1 != steals_on2 || state_on1 != state_on2) {
		printf("FAIL[INV3]: same seed did not replay (steals %ld!=%ld, "
		    "state %016llx!=%016llx)\n", steals_on1, steals_on2,
		    (unsigned long long)state_on1, (unsigned long long)state_on2);
		return 1;
	}

	printf("OK: eager rebalance closes the parked-fiber steal gap under "
	    "DST -- eager-off replays the reported 0-steal baseline "
	    "(the PG measurement), eager-on drives the REAL "
	    "__xtc_loop_step steal-before-block via the seeded scheduler "
	    "(%ld steals) with byte-identical replay\n", steals_on1);
	return 0;
}
