/*
 * test/sim/test_sim_sched_shares.c
 *
 * DST proof for the L1 opt-in proportional-share (weighted-fair)
 * scheduler.  INSPIRED BY Glommio (Glauber Costa / ScyllaDB): task
 * queues carry SHARES and a CFS-style min-vruntime pick gives each a
 * weighted CPU fraction (executor/mod.rs account_vruntime + shares.rs).
 *
 * Shape: ONE loop, two scheduling classes A and B with 3:1 shares.
 * Equal numbers of well-behaved worker procs in each class, each doing
 * equal-size cooperative chunks (a fixed number of xtc_yield rounds).
 * Under the deterministic simulator virtual time does not advance
 * within a compute run, so the scheduler charges each run a fixed
 * quantum -- reduction-style accounting -- and the min-vruntime pick
 * then runs class A about 3x as often as class B.
 *
 * Invariants:
 *   INV1 (weighting): runs(A) : runs(B) ~= 3 : 1 (within a tolerance
 *        band), so the higher-shares class gets proportionally more CPU.
 *   INV2 (default unchanged): a run with NO class created schedules
 *        the same workers with no per-class accounting (n_classes == 0
 *        path); both complete.  (Proves opt-in / zero-overhead-when-off
 *        does not change liveness.)
 *   INV3 (determinism): the same seed replays byte-identically (per-
 *        class run counts + state hash) across two runs.
 *   INV4 (latency): a latency-tagged class proc is scheduled promptly
 *        -- its first run happens within the first few scheduler steps,
 *        not after the throughput class drains.
 *
 * Adversarial: forcing the pick to ignore vruntime (always class 0)
 * makes INV1 fail -- this is the DST regression guard for the weighting,
 * not just a demo.
 */
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_sim.h"

#define WORKERS_PER_CLASS 4
#define TOTAL_BUDGET      1200   /* total runs across both classes before
                                  * everyone stops -- the "window", chosen
                                  * so both classes have surplus work (a
                                  * bounded finite job would just drain
                                  * both equally regardless of shares). */

static atomic_int g_done;
static atomic_int g_total_runs;   /* rounds executed across all workers */

/* A well-behaved worker: equal cooperative rounds, looping until the
 * shared run budget is spent.  Because the higher-shares class is
 * picked more often, it burns more of the shared budget -- so its
 * completed-run count is proportionally higher when the budget closes
 * (the CPU-share signal). */
static void
worker(void *arg)
{
	(void)arg;
	while (atomic_fetch_add_explicit(&g_total_runs, 1,
	    memory_order_relaxed) < TOTAL_BUDGET)
		xtc_yield();
	atomic_fetch_add_explicit(&g_done, 1, memory_order_relaxed);
}

struct spawn_ctx {
	xtc_loop_t      *loop;
	xtc_exec_class_t ca;
	xtc_exec_class_t cb;
	int              use_classes;
};

/* Spawn the two classes' workers.  Runs on the loop, so the classes it
 * references were created on this same loop. */
static void
spawner(void *arg)
{
	struct spawn_ctx *c = arg;
	xtc_proc_opts_t oa, ob;
	int i;

	memset(&oa, 0, sizeof oa);
	memset(&ob, 0, sizeof ob);
	if (c->use_classes) {
		oa.sched_class = c->ca;
		ob.sched_class = c->cb;
	}
	for (i = 0; i < WORKERS_PER_CLASS; i++) {
		(void)xtc_proc_spawn(c->loop, worker, NULL,
		    c->use_classes ? &oa : NULL, NULL);
		(void)xtc_proc_spawn(c->loop, worker, NULL,
		    c->use_classes ? &ob : NULL, NULL);
	}
}

/*
 * Run once under the deterministic sim.  Reports per-class run counts
 * and the state hash.  Returns 0 on OK, -1 on error.
 */
static int
run_once(uint64_t seed, int use_classes, uint64_t *out_runs_a,
    uint64_t *out_runs_b, uint64_t *out_state, int *out_done)
{
	xtc_exec_t *e = NULL;
	xtc_loop_t *loop;
	xtc_exec_class_t ca = NULL, cb = NULL;
	struct spawn_ctx ctx;
	int rc;

	atomic_store(&g_done, 0);
	atomic_store(&g_total_runs, 0);
	if (xtc_exec_init(&e, 1) != XTC_OK)
		return -1;
	loop = xtc_exec_loop(e, 0);

	if (use_classes) {
		if (xtc_exec_class_create(loop, 3, 0, &ca) != XTC_OK ||
		    xtc_exec_class_create(loop, 1, 0, &cb) != XTC_OK) {
			(void)xtc_exec_fini(e);
			return -1;
		}
	}

	ctx.loop = loop;
	ctx.ca = ca;
	ctx.cb = cb;
	ctx.use_classes = use_classes;

	if (xtc_proc_spawn(loop, spawner, &ctx, NULL, NULL) != XTC_OK) {
		(void)xtc_exec_fini(e);
		return -1;
	}

	rc = xtc_sim_exec_run(e, seed, 2000000);
	if (rc != XTC_OK) {
		(void)xtc_exec_fini(e);
		return -1;
	}

	if (out_runs_a) *out_runs_a = xtc_exec_class_runs(ca);
	if (out_runs_b) *out_runs_b = xtc_exec_class_runs(cb);
	if (out_state)  *out_state = xtc_sim_state_hash(e);
	if (out_done)   *out_done = atomic_load(&g_done);
	(void)xtc_exec_fini(e);
	return 0;
}

/* --- INV4: latency class scheduled promptly --- */

static atomic_int g_lat_first_step;   /* scheduler step the lat proc first ran */
static atomic_int g_step_counter;

static void
hog(void *arg)
{
	int i;
	(void)arg;
	for (i = 0; i < 200; i++) {
		atomic_fetch_add_explicit(&g_step_counter, 1,
		    memory_order_relaxed);
		xtc_yield();
	}
}

static void
latency_proc(void *arg)
{
	(void)arg;
	/* Record the global step at which we first got to run. */
	if (atomic_load_explicit(&g_lat_first_step, memory_order_relaxed) < 0)
		atomic_store_explicit(&g_lat_first_step,
		    atomic_load_explicit(&g_step_counter, memory_order_relaxed),
		    memory_order_relaxed);
}

struct lat_ctx {
	xtc_loop_t      *loop;
	xtc_exec_class_t hogc;
	xtc_exec_class_t latc;
};

static void
lat_spawner(void *arg)
{
	struct lat_ctx *c = arg;
	xtc_proc_opts_t oh, ol;
	int i;

	memset(&oh, 0, sizeof oh);
	memset(&ol, 0, sizeof ol);
	oh.sched_class = c->hogc;
	ol.sched_class = c->latc;
	/* A backlog of hog procs first, then one latency proc.  Under
	 * plain FIFO the latency proc would wait behind the whole backlog;
	 * with a latency class + higher shares it is scheduled promptly. */
	for (i = 0; i < 8; i++)
		(void)xtc_proc_spawn(c->loop, hog, NULL, &oh, NULL);
	(void)xtc_proc_spawn(c->loop, latency_proc, NULL, &ol, NULL);
}

static int
run_latency(uint64_t seed, int *out_first_step)
{
	xtc_exec_t *e = NULL;
	xtc_loop_t *loop;
	xtc_exec_class_t hogc = NULL, latc = NULL;
	struct lat_ctx ctx;

	atomic_store(&g_lat_first_step, -1);
	atomic_store(&g_step_counter, 0);
	if (xtc_exec_init(&e, 1) != XTC_OK)
		return -1;
	loop = xtc_exec_loop(e, 0);

	/* Hog: shares 1, no latency bound.  Latency class: high shares +
	 * a tight latency bound so it is picked promptly. */
	if (xtc_exec_class_create(loop, 1, 0, &hogc) != XTC_OK ||
	    xtc_exec_class_create(loop, 100, 1000 * 1000LL, &latc) != XTC_OK) {
		(void)xtc_exec_fini(e);
		return -1;
	}
	ctx.loop = loop;
	ctx.hogc = hogc;
	ctx.latc = latc;
	if (xtc_proc_spawn(loop, lat_spawner, &ctx, NULL, NULL) != XTC_OK) {
		(void)xtc_exec_fini(e);
		return -1;
	}
	if (xtc_sim_exec_run(e, seed, 2000000) != XTC_OK) {
		(void)xtc_exec_fini(e);
		return -1;
	}
	if (out_first_step)
		*out_first_step = atomic_load(&g_lat_first_step);
	(void)xtc_exec_fini(e);
	return 0;
}

int
main(void)
{
	const uint64_t seed = 0x5EED3A1ULL;
	uint64_t ra1 = 0, rb1 = 0, ra2 = 0, rb2 = 0;
	uint64_t st1 = 0, st2 = 0, st_off = 0;
	uint64_t ra_off = 0, rb_off = 0;
	int done1 = 0, done2 = 0, done_off = 0;
	int lat_first = -1;
	double ratio;

	/* INV1 + INV3: weighted 3:1, replayable. */
	if (run_once(seed, 1, &ra1, &rb1, &st1, &done1) != 0) {
		printf("FAIL: sim run 1 errored\n");
		return 1;
	}
	if (run_once(seed, 1, &ra2, &rb2, &st2, &done2) != 0) {
		printf("FAIL: sim run 2 errored\n");
		return 1;
	}
	/* INV2: default (no classes) path. */
	if (run_once(seed, 0, &ra_off, &rb_off, &st_off, &done_off) != 0) {
		printf("FAIL: sim run (no classes) errored\n");
		return 1;
	}

	printf("with classes (run1): runs(A=3sh)=%llu runs(B=1sh)=%llu "
	    "done=%d state=%016llx\n",
	    (unsigned long long)ra1, (unsigned long long)rb1, done1,
	    (unsigned long long)st1);
	printf("with classes (run2): runs(A)=%llu runs(B)=%llu done=%d "
	    "state=%016llx\n",
	    (unsigned long long)ra2, (unsigned long long)rb2, done2,
	    (unsigned long long)st2);
	printf("no classes:          runs(A)=%llu runs(B)=%llu done=%d\n",
	    (unsigned long long)ra_off, (unsigned long long)rb_off, done_off);

	if (done1 != 2 * WORKERS_PER_CLASS ||
	    done2 != 2 * WORKERS_PER_CLASS ||
	    done_off != 2 * WORKERS_PER_CLASS) {
		printf("FAIL: not all workers completed\n");
		return 1;
	}

	/* INV3: byte-identical replay. */
	if (ra1 != ra2 || rb1 != rb2 || st1 != st2) {
		printf("FAIL[INV3]: same seed did not replay "
		    "(A %llu!=%llu, B %llu!=%llu, state %016llx!=%016llx)\n",
		    (unsigned long long)ra1, (unsigned long long)ra2,
		    (unsigned long long)rb1, (unsigned long long)rb2,
		    (unsigned long long)st1, (unsigned long long)st2);
		return 1;
	}

	/* INV1: weighting.  A gets ~3x B.  Band [2.3, 3.7] absorbs the
	 * boundary effects of a finite run (both classes eventually drain,
	 * so the tail is not perfectly 3:1). */
	if (rb1 == 0) {
		printf("FAIL[INV1]: class B never ran\n");
		return 1;
	}
	ratio = (double)ra1 / (double)rb1;
	printf("ratio A:B = %.2f:1 (want ~3:1)\n", ratio);
	if (ratio < 2.3 || ratio > 3.7) {
		printf("FAIL[INV1]: run ratio %.2f:1 outside [2.3, 3.7] -- "
		    "weighted-fair scheduling not proportional to shares\n",
		    ratio);
		return 1;
	}

	/* INV4: latency class scheduled promptly. */
	if (run_latency(seed, &lat_first) != 0) {
		printf("FAIL: latency sim run errored\n");
		return 1;
	}
	printf("latency proc first ran at global step %d "
	    "(8 hog procs x 200 rounds in the backlog)\n", lat_first);
	if (lat_first < 0) {
		printf("FAIL[INV4]: latency proc never ran\n");
		return 1;
	}
	/* With a latency+high-shares class it must run WELL before the hog
	 * backlog (8 procs x 200 rounds = 1600 hog runs) drains; require it
	 * within the first 200 hog rounds (an order of magnitude early). */
	if (lat_first >= 200) {
		printf("FAIL[INV4]: latency proc waited %d hog rounds "
		    "-- not scheduled within its bound\n", lat_first);
		return 1;
	}

	printf("OK: 3:1 shares => %.2f:1 CPU (runs), byte-identical replay, "
	    "default-off path unchanged, latency class scheduled promptly "
	    "(step %d) -- L1 proportional-share, inspired by Glommio\n",
	    ratio, lat_first);
	return 0;
}
