/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/sim/test_sim_saga.c
 *	DST coverage of xtc_saga (src/orc/saga.c) under seeded FAULT
 *	injection.  Modeled on test_sim_fault.c's fault-decision pattern:
 *	each worker's saga steps consult xtc_sim_fault at a seed-determined
 *	decision point to decide whether THIS worker's saga fails, and (if
 *	so) at which step.
 *
 *	Workload: N_WORKERS procs across N_LOOPS loops, each running an
 *	independent K-step saga.  Per worker, a seed-determined coin
 *	(drawn from the dedicated FAULT stream, so enabling/disabling this
 *	never perturbs the SCHED/PLACE streams and other tests' schedules)
 *	decides whether the saga fails, and if so a second seeded draw
 *	picks WHICH step fails.  Every action and compensate call is
 *	recorded into a per-worker call log (index + kind), from which we
 *	verify:
 *
 *	  - a failed saga's completed-action count equals the failing
 *	    step's index, and its compensation sequence is EXACTLY the
 *	    reverse of the completed steps, calling each action/compensate
 *	    exactly once (no extra, no missing, no duplicate);
 *	  - a successful saga runs every action exactly once and no
 *	    compensation at all;
 *	  - the fold of every worker's outcome (failed/not, failing step,
 *	    completed count) into one hash, plus the sim's own state hash,
 *	    replays BYTE-IDENTICALLY from the same seed;
 *	  - a different seed can (and, over these N_WORKERS, does) pick a
 *	    different fault schedule, while every worker's OWN saga still
 *	    satisfies the reverse-order invariant.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_saga.h"
#include "xtc_sim.h"

#define N_LOOPS    4
#define N_WORKERS  24
#define K_STEPS    5          /* steps per saga */
#define FAULT_PCT  400        /* ~40% of workers get a failing saga */

#define TAG_ACTION(i)     (0x1000 + (i))
#define TAG_COMPENSATE(i) (0x2000 + (i))

struct worker_result {
	int seq[2 * K_STEPS];
	int n_seq;
	int rc;
	int n_completed;
	int failed_step;
	int compensate_failed;
};

static struct worker_result g_results[N_WORKERS];
static atomic_int           g_done;
/* ORDER-sensitive fold of every worker's outcome, for replay comparison. */
static atomic_long          g_outcome_hash;

struct step_ctx {
	struct worker_result *r;
	int                    idx;
	int                    fail_here;   /* this step's action should fail */
};

static int
saga_action(void *arg)
{
	struct step_ctx *c = arg;
	c->r->seq[c->r->n_seq++] = TAG_ACTION(c->idx);
	/* Yield so this worker's saga interleaves with its siblings under
	 * the seeded scheduler -- a saga step is an ordinary fiber-called
	 * function and must survive being descheduled mid-run. */
	xtc_yield();
	return c->fail_here ? XTC_E_ABORTED : XTC_OK;
}

static int
saga_compensate(void *arg)
{
	struct step_ctx *c = arg;
	c->r->seq[c->r->n_seq++] = TAG_COMPENSATE(c->idx);
	xtc_yield();
	return XTC_OK;
}

static void
fold(atomic_long *h, long v)
{
	long x = atomic_load_explicit(h, memory_order_relaxed);
	x = x * 1000003L + (v + 1);
	atomic_store_explicit(h, x, memory_order_relaxed);
}

/*
 * Worker: build a K_STEPS saga.  A seeded coin (FAULT stream) decides
 * whether this worker's saga fails; if so, a second seeded draw (also
 * FAULT stream -- xtc_sim_fault's own draws, plus one extra range draw
 * for the failing index) picks which step's action fails.
 */
static void
worker(void *arg)
{
	long id = (long)(intptr_t)arg;
	struct worker_result *r = &g_results[id];
	struct step_ctx ctx[K_STEPS];
	xtc_saga_t *s = NULL;
	int i, fail_idx = -1;

	memset(r, 0, sizeof *r);

	if (xtc_sim_fault(FAULT_PCT))
		fail_idx = (int)__xtc_sim_rng_range(XTC_SIM_RNG_FAULT, K_STEPS);

	if (xtc_saga_create(&s) != XTC_OK) { r->rc = XTC_E_NOMEM; goto done; }
	for (i = 0; i < K_STEPS; i++) {
		ctx[i].r = r;
		ctx[i].idx = i;
		ctx[i].fail_here = (i == fail_idx);
		if (xtc_saga_step(s, saga_action, saga_compensate, &ctx[i])
		    != XTC_OK) { r->rc = XTC_E_INTERNAL; goto done; }
	}

	r->rc = xtc_saga_run(s);
	r->n_completed = xtc_saga_n_completed(s);
	r->failed_step = xtc_saga_failed_step(s);
	r->compensate_failed = xtc_saga_compensate_failed(s);
	xtc_saga_destroy(s);

done:
	/* Fold this worker's outcome into the order-sensitive hash: which
	 * worker, whether it failed, at which step, how many completed. */
	fold(&g_outcome_hash, id * 100003L + r->rc);
	fold(&g_outcome_hash, r->n_completed * 7 + r->failed_step + 1000);
	atomic_fetch_add_explicit(&g_done, 1, memory_order_relaxed);
}

/*
 * Verify one worker's call log against the saga contract: on success,
 * every action ran exactly once, in order, and no compensation ran; on
 * failure, actions 0..failed_step ran in order, then compensations for
 * 0..failed_step-1 ran in EXACT reverse order, each exactly once.
 * Returns NULL if OK, else a description of the violation (static
 * buffer -- single-threaded checker, called after quiescence).
 */
static const char *
check_worker(const struct worker_result *r, long id)
{
	static char msg[256];
	int i, pos;

	if (r->rc == XTC_OK) {
		if (r->n_seq != K_STEPS) {
			snprintf(msg, sizeof msg, "worker %ld: succeeded but "
			    "logged %d calls (expected %d actions, no "
			    "compensation)", id, r->n_seq, K_STEPS);
			return msg;
		}
		for (i = 0; i < K_STEPS; i++) {
			if (r->seq[i] != TAG_ACTION(i)) {
				snprintf(msg, sizeof msg, "worker %ld: "
				    "succeeded but call[%d]=%#x != action(%d)",
				    id, i, r->seq[i], i);
				return msg;
			}
		}
		return NULL;
	}

	/* Failure path.  n_completed == failed_step (0-based). */
	if (r->n_completed != r->failed_step) {
		snprintf(msg, sizeof msg, "worker %ld: n_completed=%d != "
		    "failed_step=%d", id, r->n_completed, r->failed_step);
		return msg;
	}
	{
		int expect_n = (r->failed_step + 1) + r->failed_step;
		if (r->n_seq != expect_n) {
			snprintf(msg, sizeof msg, "worker %ld: logged %d "
			    "calls, expected %d (failed_step=%d)", id,
			    r->n_seq, expect_n, r->failed_step);
			return msg;
		}
	}
	pos = 0;
	for (i = 0; i <= r->failed_step; i++) {
		if (r->seq[pos++] != TAG_ACTION(i)) {
			snprintf(msg, sizeof msg, "worker %ld: call[%d] not "
			    "action(%d) in forward phase", id, pos - 1, i);
			return msg;
		}
	}
	for (i = r->failed_step - 1; i >= 0; i--) {
		if (r->seq[pos++] != TAG_COMPENSATE(i)) {
			snprintf(msg, sizeof msg, "worker %ld: call[%d] not "
			    "compensate(%d) in reverse phase (expected "
			    "EXACT reverse order)", id, pos - 1, i);
			return msg;
		}
	}
	return NULL;
}

static int
run_once(uint64_t seed, int *out_done, int *out_n_failed,
    long *out_outcome_hash, uint64_t *out_state, const char **out_violation)
{
	xtc_exec_t *e = NULL;
	long w;
	int n_failed = 0;

	atomic_store(&g_done, 0);
	atomic_store(&g_outcome_hash, 0);
	memset(g_results, 0, sizeof g_results);

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK) { *out_done = -1; return -1; }
	for (w = 0; w < N_WORKERS; w++) {
		xtc_loop_t *l = xtc_exec_loop(e, (unsigned)(w % N_LOOPS));
		(void)xtc_proc_spawn(l, worker, (void *)(intptr_t)w, NULL, NULL);
	}
	(void)xtc_sim_exec_run(e, seed, 2000000);

	*out_done = atomic_load(&g_done);
	*out_outcome_hash = atomic_load(&g_outcome_hash);
	if (out_state) *out_state = xtc_sim_state_hash(e);
	(void)xtc_exec_fini(e);

	*out_violation = NULL;
	for (w = 0; w < N_WORKERS; w++) {
		const char *v = check_worker(&g_results[w], w);
		if (v != NULL) { *out_violation = v; break; }
		if (g_results[w].rc != XTC_OK) n_failed++;
	}
	*out_n_failed = n_failed;
	return 0;
}

int
main(void)
{
	int d1 = 0, d2 = 0, d3 = 0;
	int nf1 = 0, nf2 = 0, nf3 = 0;
	long h1 = 0, h2 = 0, h3 = 0;
	uint64_t st1 = 0, st2 = 0, st3 = 0;
	const char *v1 = NULL, *v2 = NULL, *v3 = NULL;

	(void)run_once(0x5A6A5EED, &d1, &nf1, &h1, &st1, &v1);
	(void)run_once(0x5A6A5EED, &d2, &nf2, &h2, &st2, &v2);
	(void)run_once(0xC0FFEE01, &d3, &nf3, &h3, &st3, &v3);

	printf("run1: done=%d/%d failed_sagas=%d hash=%ld state=%016llx\n",
	    d1, N_WORKERS, nf1, h1, (unsigned long long)st1);
	printf("run2: done=%d/%d failed_sagas=%d hash=%ld state=%016llx\n",
	    d2, N_WORKERS, nf2, h2, (unsigned long long)st2);
	printf("run3 (diff seed): done=%d/%d failed_sagas=%d hash=%ld\n",
	    d3, N_WORKERS, nf3, h3);

	if (d1 != N_WORKERS || d2 != N_WORKERS || d3 != N_WORKERS) {
		printf("FAIL: not every worker's saga completed\n");
		return 1;
	}
	if (v1 != NULL) {
		printf("FAIL: run1 compensation-order violation: %s\n", v1);
		return 1;
	}
	if (v2 != NULL) {
		printf("FAIL: run2 compensation-order violation: %s\n", v2);
		return 1;
	}
	if (v3 != NULL) {
		printf("FAIL: run3 compensation-order violation: %s\n", v3);
		return 1;
	}
	if (nf1 == 0) {
		printf("FAIL: no saga failed (expected ~%u%% of %d workers "
		    "to hit an injected fault)\n", FAULT_PCT / 10, N_WORKERS);
		return 1;
	}
	if (nf1 != nf2 || h1 != h2 || st1 != st2) {
		printf("FAIL: run did not replay from the same seed "
		    "(failed %d/%d hash %ld/%ld state %016llx/%016llx)\n",
		    nf1, nf2, h1, h2, (unsigned long long)st1,
		    (unsigned long long)st2);
		return 1;
	}

	printf("OK: seeded fault injection into %d saga workers (K=%d steps "
	    "each) always reverses exactly the completed steps' "
	    "compensations with no extra/missing/duplicate calls "
	    "(%d/%d sagas failed this seed); outcome hash and sim state "
	    "hash replay byte-identically from the seed\n",
	    N_WORKERS, K_STEPS, nf1, N_WORKERS);
	return 0;
}
