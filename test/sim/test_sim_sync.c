#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <stdint.h>
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_sync.h"
#include "xtc_async.h"     /* xtc_yield */
#include "xtc_sim.h"

/*
 * DST coverage of the blocking M9 sync primitives (src/ptc/sync.c):
 * the counting SEMAPHORE, the BARRIER, and the GATE.  These block a
 * waiter in raw pthread_cond_wait for OFF-loop callers; a fiber-park
 * path (gated on __xtc_current_task() != NULL) was added so a fiber
 * blocking on one PARKS -- arms a waker, drops the internal lock,
 * yields to the loop, re-checks on wake -- instead of freezing the
 * single sim thread.  The production OS-thread-blocking path is
 * unchanged (test/m9/test_sync still passes byte for byte).
 *
 * Three workloads, all across N loops under xtc_sim_exec_run:
 *
 *   sem   -- N worker fibers contend a counting semaphore of capacity
 *            CAP.  Each acquires 1 permit (positive timeout, so a
 *            conflict PARKS the fiber), asserts the number of holders
 *            never exceeds CAP (NO OVER-ADMISSION past the count),
 *            yields while holding, then posts the permit back.
 *            INVARIANT: peak concurrent holders <= CAP; every worker
 *            eventually acquires + releases exactly once.
 *
 *   barrier - P fiber parties rendezvous at a barrier for R rounds.
 *            A generation counter increments on each full round.
 *            INVARIANT: no party leaves round k until ALL P have
 *            arrived (checked via an arrived-counter that must equal P
 *            when the first leaver observes the release), and every
 *            party completes all R rounds (parties released TOGETHER).
 *
 *   gate  -- W worker fibers enter a gate, do a yield of "work", and
 *            leave; a closer fiber closes the gate and drains it.
 *            INVARIANT: after drain returns the gate count is 0 and no
 *            worker entered AFTER the close (gate open/closed
 *            correctness), and every admitted worker left.
 *
 * Each asserts (a) quiescence (rc == XTC_OK -- no hang), (b) the
 * invariant, (c) byte-identical replay from the seed (an app hash +
 * the sim state hash), (d) a different seed reorders but stays
 * consistent.  Footprint is tiny (few fibers, per-run free).
 */

#define N_LOOPS 4

/* ================= semaphore ================= */

#define SEM_CAP     3
#define SEM_WORK    12

static xtc_sem_t  *g_sem;
static atomic_int  g_sem_holders;    /* live holders -- must stay <= CAP */
static atomic_int  g_sem_peak;       /* max holders seen */
static atomic_int  g_sem_over;       /* count of over-admissions (bug) */
static atomic_int  g_sem_done;       /* workers that completed a pair */
static atomic_long g_sem_hash;       /* order-sensitive fold of grants */

static void
sem_fold(long v)
{
	long h = atomic_load_explicit(&g_sem_hash, memory_order_relaxed);
	h = h * 1000003L + (v + 1);
	atomic_store_explicit(&g_sem_hash, h, memory_order_relaxed);
}

static void
sem_worker(void *arg)
{
	int id = (int)(intptr_t)arg;
	int rc, tries;

	/* Positive timeout so a conflict PARKS the fiber in the wait loop.
	 * Retry on XTC_E_AGAIN (the sync.sem.spurious_timeout buggify may
	 * decline a satisfiable acquire; a fresh draw lets us succeed). */
	for (tries = 0; tries < 100000; tries++) {
		rc = xtc_sem_acquire(g_sem, 1, 1000000000LL);
		if (rc == XTC_OK) break;
		if (rc != XTC_E_AGAIN) return;   /* unexpected */
		xtc_yield();
	}
	if (rc != XTC_OK) return;

	{
		int h = atomic_fetch_add_explicit(&g_sem_holders, 1,
		    memory_order_relaxed) + 1;
		int pk;
		if (h > SEM_CAP)
			atomic_fetch_add_explicit(&g_sem_over, 1,
			    memory_order_relaxed);
		do {
			pk = atomic_load_explicit(&g_sem_peak,
			    memory_order_relaxed);
			if (h <= pk) break;
		} while (!atomic_compare_exchange_weak_explicit(&g_sem_peak,
		    &pk, h, memory_order_relaxed, memory_order_relaxed));
		sem_fold(id);
	}
	xtc_yield();                        /* hold across a yield */
	atomic_fetch_sub_explicit(&g_sem_holders, 1, memory_order_relaxed);
	(void)xtc_sem_post(g_sem, 1);
	atomic_fetch_add_explicit(&g_sem_done, 1, memory_order_relaxed);
}

static int
run_sem(uint64_t seed, int use_buggify, int *out_done, int *out_peak,
    int *out_over, long *out_hash, uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	int i, rc;

	atomic_store(&g_sem_holders, 0);
	atomic_store(&g_sem_peak, 0);
	atomic_store(&g_sem_over, 0);
	atomic_store(&g_sem_done, 0);
	atomic_store(&g_sem_hash, 0);

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK) return -1;
	if (xtc_sem_create(SEM_CAP, &g_sem) != XTC_OK) {
		(void)xtc_exec_fini(e); return -1;
	}
	if (use_buggify)
		xtc_sim_buggify_enable(1000);   /* every site coin live */
	for (i = 0; i < SEM_WORK; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e, (unsigned)(i % N_LOOPS)),
		    sem_worker, (void *)(intptr_t)i, NULL, NULL);

	rc = xtc_sim_exec_run(e, seed, 5000000);

	*out_done = atomic_load(&g_sem_done);
	*out_peak = atomic_load(&g_sem_peak);
	*out_over = atomic_load(&g_sem_over);
	*out_hash = atomic_load(&g_sem_hash);
	if (out_state) *out_state = xtc_sim_state_hash(e);
	if (use_buggify) xtc_sim_buggify_disable();
	xtc_sem_destroy(g_sem);
	g_sem = NULL;
	(void)xtc_exec_fini(e);
	return rc;
}

/* ================= barrier ================= */

#define BAR_PARTIES 4
#define BAR_ROUNDS  3

static xtc_barrier_t *g_bar;
static atomic_int      g_bar_entered[BAR_ROUNDS]; /* parties that reached wait */
static atomic_int      g_bar_bad;       /* a party passed before all entered */
static atomic_int      g_bar_done;      /* parties that finished all rounds */

static void
bar_party(void *arg)
{
	int r;
	(void)arg;
	for (r = 0; r < BAR_ROUNDS; r++) {
		/* Record entry into round r (monotonic, only grows), then
		 * rendezvous.  A party may only PASS the barrier once all
		 * PARTIES have entered this round.  entered[] only grows, so
		 * sampling it after wait returns is race-free: if a party
		 * returns while entered < PARTIES the barrier released early
		 * -- a bug (parties must be released TOGETHER). */
		atomic_fetch_add_explicit(&g_bar_entered[r], 1,
		    memory_order_relaxed);
		(void)xtc_barrier_wait(g_bar);
		if (atomic_load_explicit(&g_bar_entered[r],
		    memory_order_relaxed) < BAR_PARTIES)
			atomic_fetch_add_explicit(&g_bar_bad, 1,
			    memory_order_relaxed);
	}
	atomic_fetch_add_explicit(&g_bar_done, 1, memory_order_relaxed);
}

static int
run_barrier(uint64_t seed, int *out_done, int *out_bad, uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	int i, rc;

	for (i = 0; i < BAR_ROUNDS; i++)
		atomic_store(&g_bar_entered[i], 0);
	atomic_store(&g_bar_bad, 0);
	atomic_store(&g_bar_done, 0);

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK) return -1;
	if (xtc_barrier_create(BAR_PARTIES, &g_bar) != XTC_OK) {
		(void)xtc_exec_fini(e); return -1;
	}
	for (i = 0; i < BAR_PARTIES; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e, (unsigned)(i % N_LOOPS)),
		    bar_party, NULL, NULL, NULL);

	rc = xtc_sim_exec_run(e, seed, 5000000);

	*out_done = atomic_load(&g_bar_done);
	*out_bad = atomic_load(&g_bar_bad);
	if (out_state) *out_state = xtc_sim_state_hash(e);
	xtc_barrier_destroy(g_bar);
	g_bar = NULL;
	(void)xtc_exec_fini(e);
	return rc;
}

/* ================= gate ================= */

#define GATE_WORK  8

static xtc_gate_t *g_gate;
static atomic_int  g_gate_left;      /* workers that entered+left */
static atomic_int  g_gate_closed;    /* set once the gate is closed */
static atomic_int  g_gate_after;     /* entries admitted AFTER close (bug) */
static atomic_int  g_gate_drained;   /* 1 once drain returned with count 0 */

static void
gate_worker(void *arg)
{
	(void)arg;
	if (xtc_gate_enter(g_gate) != XTC_OK)
		return;                     /* refused (gate already closed) */
	if (atomic_load_explicit(&g_gate_closed, memory_order_relaxed))
		atomic_fetch_add_explicit(&g_gate_after, 1,
		    memory_order_relaxed);
	xtc_yield();                        /* "work" inside the gate */
	(void)xtc_gate_leave(g_gate);
	atomic_fetch_add_explicit(&g_gate_left, 1, memory_order_relaxed);
}

static void
gate_closer(void *arg)
{
	int rc;
	(void)arg;
	/* Let some workers enter first. */
	xtc_yield(); xtc_yield();
	atomic_store_explicit(&g_gate_closed, 1, memory_order_relaxed);
	(void)xtc_gate_close(g_gate);
	/* Drain: parks the fiber until every entered worker has left.
	 * Positive timeout so a stuck worker surfaces, not a hang. */
	rc = xtc_gate_drain(g_gate, 1000000000LL);
	if (rc == XTC_OK && xtc_gate_count(g_gate) == 0)
		atomic_store_explicit(&g_gate_drained, 1, memory_order_relaxed);
}

static int
run_gate(uint64_t seed, int *out_left, int *out_after, int *out_drained,
    uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	int i, rc;

	atomic_store(&g_gate_left, 0);
	atomic_store(&g_gate_closed, 0);
	atomic_store(&g_gate_after, 0);
	atomic_store(&g_gate_drained, 0);

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK) return -1;
	if (xtc_gate_create(&g_gate) != XTC_OK) {
		(void)xtc_exec_fini(e); return -1;
	}
	for (i = 0; i < GATE_WORK; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e, (unsigned)(i % N_LOOPS)),
		    gate_worker, NULL, NULL, NULL);
	(void)xtc_proc_spawn(xtc_exec_loop(e, (unsigned)(N_LOOPS - 1)),
	    gate_closer, NULL, NULL, NULL);

	rc = xtc_sim_exec_run(e, seed, 5000000);

	*out_left = atomic_load(&g_gate_left);
	*out_after = atomic_load(&g_gate_after);
	*out_drained = atomic_load(&g_gate_drained);
	if (out_state) *out_state = xtc_sim_state_hash(e);
	xtc_gate_destroy(g_gate);
	g_gate = NULL;
	(void)xtc_exec_fini(e);
	return rc;
}

int
main(void)
{
	int rc;

	/* ---- semaphore: no over-admission + all pairs + replay ---- */
	{
		int d1 = 0, pk1 = 0, ov1 = 0, d2 = 0, pk2 = 0, ov2 = 0;
		int d3 = 0, pk3 = 0, ov3 = 0;
		long h1 = 0, h2 = 0, h3 = 0;
		uint64_t s1 = 0, s2 = 0, s3 = 0;

		rc = run_sem(0x5E301, 0, &d1, &pk1, &ov1, &h1, &s1);
		if (rc != XTC_OK) {
			printf("FAIL: sem run rc=%d (hang?)\n", rc); return 1;
		}
		(void)run_sem(0x5E301, 0, &d2, &pk2, &ov2, &h2, &s2);
		rc = run_sem(0xA17E2, 0, &d3, &pk3, &ov3, &h3, &s3);
		if (rc != XTC_OK) {
			printf("FAIL: sem diff-seed rc=%d\n", rc); return 1;
		}
		printf("sem   run1: done=%d peak=%d over=%d hash=%ld "
		    "state=%016llx\n", d1, pk1, ov1, h1,
		    (unsigned long long)s1);
		if (d1 != SEM_WORK) {
			printf("FAIL: not all sem workers completed "
			    "(done=%d want %d)\n", d1, SEM_WORK); return 1;
		}
		if (ov1 != 0 || pk1 > SEM_CAP) {
			printf("FAIL: sem OVER-ADMISSION (peak=%d over=%d "
			    "cap=%d)\n", pk1, ov1, SEM_CAP); return 1;
		}
		if (d1 != d2 || pk1 != pk2 || h1 != h2 || s1 != s2) {
			printf("FAIL: sem did not replay "
			    "(hash %ld/%ld state %016llx/%016llx)\n",
			    h1, h2, (unsigned long long)s1,
			    (unsigned long long)s2); return 1;
		}
		if (d3 != SEM_WORK || ov3 != 0 || pk3 > SEM_CAP) {
			printf("FAIL: sem diff-seed inconsistent "
			    "(done=%d peak=%d over=%d)\n", d3, pk3, ov3);
			return 1;
		}
	}

	/* ---- semaphore under the spurious-timeout buggify ---- */
	{
		int d1 = 0, pk1 = 0, ov1 = 0, d2 = 0, pk2 = 0, ov2 = 0;
		long h1 = 0, h2 = 0;
		uint64_t s1 = 0, s2 = 0;

		rc = run_sem(0xB0661F, 1, &d1, &pk1, &ov1, &h1, &s1);
		if (rc != XTC_OK) {
			printf("FAIL: sem+buggify rc=%d\n", rc); return 1;
		}
		(void)run_sem(0xB0661F, 1, &d2, &pk2, &ov2, &h2, &s2);
		printf("sem+buggify run1: done=%d peak=%d over=%d "
		    "state=%016llx\n", d1, pk1, ov1,
		    (unsigned long long)s1);
		/* Buggify may delay acquires (retries) but must never break
		 * the invariant nor lose a worker, and must replay. */
		if (d1 != SEM_WORK || ov1 != 0 || pk1 > SEM_CAP) {
			printf("FAIL: sem+buggify broke invariant "
			    "(done=%d peak=%d over=%d)\n", d1, pk1, ov1);
			return 1;
		}
		if (d1 != d2 || pk1 != pk2 || h1 != h2 || s1 != s2) {
			printf("FAIL: sem+buggify did not replay\n");
			return 1;
		}
	}

	/* ---- barrier: all parties released together + replay ---- */
	{
		int d1 = 0, b1 = 0, d2 = 0, b2 = 0, d3 = 0, b3 = 0;
		uint64_t s1 = 0, s2 = 0, s3 = 0;

		rc = run_barrier(0xBA881, &d1, &b1, &s1);
		if (rc != XTC_OK) {
			printf("FAIL: barrier run rc=%d (hang?)\n", rc);
			return 1;
		}
		(void)run_barrier(0xBA881, &d2, &b2, &s2);
		rc = run_barrier(0x1CE99, &d3, &b3, &s3);
		if (rc != XTC_OK) {
			printf("FAIL: barrier diff-seed rc=%d\n", rc);
			return 1;
		}
		printf("bar   run1: done=%d (want %d) bad=%d state=%016llx\n",
		    d1, BAR_PARTIES, b1, (unsigned long long)s1);
		if (d1 != BAR_PARTIES || b1 != 0) {
			printf("FAIL: barrier parties not released together "
			    "(done=%d bad=%d)\n", d1, b1); return 1;
		}
		if (d1 != d2 || b1 != b2 || s1 != s2) {
			printf("FAIL: barrier did not replay "
			    "(state %016llx/%016llx)\n",
			    (unsigned long long)s1,
			    (unsigned long long)s2); return 1;
		}
		if (d3 != BAR_PARTIES || b3 != 0) {
			printf("FAIL: barrier diff-seed inconsistent "
			    "(done=%d bad=%d)\n", d3, b3); return 1;
		}
	}

	/* ---- gate: open/close correctness + drain + replay ---- */
	{
		int l1 = 0, a1 = 0, dr1 = 0, l2 = 0, a2 = 0, dr2 = 0;
		int l3 = 0, a3 = 0, dr3 = 0;
		uint64_t s1 = 0, s2 = 0, s3 = 0;

		rc = run_gate(0x9A7E1, &l1, &a1, &dr1, &s1);
		if (rc != XTC_OK) {
			printf("FAIL: gate run rc=%d (hang?)\n", rc);
			return 1;
		}
		(void)run_gate(0x9A7E1, &l2, &a2, &dr2, &s2);
		rc = run_gate(0x33EE2, &l3, &a3, &dr3, &s3);
		if (rc != XTC_OK) {
			printf("FAIL: gate diff-seed rc=%d\n", rc); return 1;
		}
		printf("gate  run1: left=%d after-close=%d drained=%d "
		    "state=%016llx\n", l1, a1, dr1,
		    (unsigned long long)s1);
		if (dr1 != 1) {
			printf("FAIL: gate drain did not complete with count "
			    "0 (drained=%d)\n", dr1); return 1;
		}
		if (a1 != 0) {
			printf("FAIL: gate admitted %d workers AFTER close\n",
			    a1); return 1;
		}
		if (l1 != l2 || a1 != a2 || dr1 != dr2 || s1 != s2) {
			printf("FAIL: gate did not replay "
			    "(left %d/%d state %016llx/%016llx)\n", l1, l2,
			    (unsigned long long)s1,
			    (unsigned long long)s2); return 1;
		}
		if (dr3 != 1 || a3 != 0) {
			printf("FAIL: gate diff-seed inconsistent "
			    "(drained=%d after=%d)\n", dr3, a3); return 1;
		}
	}

	printf("OK: sem (no over-admission, replay), barrier (all parties "
	    "released together, replay), gate (open/close correctness + "
	    "drain, replay); spurious-timeout buggify holds the sem "
	    "invariant and replays\n");
	return 0;
}
