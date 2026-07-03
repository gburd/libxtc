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
#include "xtc_reg.h"
#include "xtc_async.h"     /* xtc_yield */
#include "xtc_sim.h"

/*
 * DST coverage of the process registry (src/orc/reg.c): a chained
 * hash table (name -> xtc_pid_t) under a single mutex.  register /
 * whereis / unregister never block (no cond_wait), so the registry is
 * sim-safe as-is -- no shim needed.  Under the seeded scheduler N
 * worker fibers across N loops RACE to register/lookup/unregister a
 * shared set of names; the sim owns the interleaving, so this exercises
 * the register-vs-register, register-vs-unregister, and lookup-vs-both
 * races a threaded registry hides.
 *
 * Two workloads:
 *
 *   contend -- W fibers each try to register their OWN pid under one of
 *            K shared names, then (whoever succeeds) look it up and
 *            unregister it.  INVARIANTS: for each name, register
 *            succeeds AT MOST once at a time (a duplicate returns
 *            XTC_E_INVAL -- no lost/duplicate registration); a whereis
 *            by the winner resolves to ITS OWN pid (deterministic
 *            resolution -- never a stale/other pid); after the run the
 *            registry count is 0 (every registration was unregistered).
 *            The count of winning registrations and the order-sensitive
 *            resolution hash REPLAY from the seed.
 *
 *   churn  -- a longer register/whereis/unregister loop over a small
 *            name space per fiber (its own private names, so every
 *            register must succeed), asserting register/unregister
 *            pair up exactly and whereis between them always resolves
 *            to the registered pid; the retry buggify
 *            (reg.whereis.transient_miss) makes a lookup transiently
 *            miss so the caller retries -- exercised for progress +
 *            replay.
 *
 * Each asserts (a) quiescence (rc == XTC_OK -- no hang), (b) the
 * invariant, (c) byte-identical replay (app hash + sim state hash),
 * (d) a different seed reorders but stays consistent.  Footprint is
 * tiny (few fibers, few names, per-run free).
 */

#define N_LOOPS 4

/* ================= contend ================= */

#define CON_WORK   16
#define CON_NAMES  4

static xtc_reg_t  *g_reg;
static atomic_int   g_wins;        /* successful registrations */
static atomic_int   g_bad_resolve; /* whereis returned the wrong pid (bug) */
static atomic_int   g_bad_dup;     /* two winners held a name at once (bug) */
static atomic_int   g_held[CON_NAMES];  /* live registrations per name */
static atomic_int   g_done;
static atomic_long  g_hash;        /* order-sensitive resolution fold */

static const char *g_names[CON_NAMES] = { "svc.a", "svc.b", "svc.c", "svc.d" };

static void
fold(long v)
{
	long h = atomic_load_explicit(&g_hash, memory_order_relaxed);
	h = h * 1000003L + (v + 1);
	atomic_store_explicit(&g_hash, h, memory_order_relaxed);
}

static void
con_worker(void *arg)
{
	int id = (int)(intptr_t)arg;
	int slot = id % CON_NAMES;
	const char *name = g_names[slot];
	/* A distinct, resolvable pid per worker (loop_id 1, index id+1). */
	xtc_pid_t self = { .loop_id = 1, .local_id = (uint32_t)(id + 1),
	    .gen = 1 };
	xtc_pid_t got;
	int rc;

	rc = xtc_reg_register(g_reg, name, self);
	if (rc == XTC_E_INVAL) {
		/* Name already taken by another fiber -- a legal, expected
		 * loser; do not touch it.  Yield so a winner can proceed. */
		xtc_yield();
		atomic_fetch_add_explicit(&g_done, 1, memory_order_relaxed);
		return;
	}
	if (rc != XTC_OK) {
		atomic_fetch_add_explicit(&g_done, 1, memory_order_relaxed);
		return;
	}
	/* Won the name.  While held, exactly one holder must exist. */
	atomic_fetch_add_explicit(&g_wins, 1, memory_order_relaxed);
	if (atomic_fetch_add_explicit(&g_held[slot], 1, memory_order_relaxed)
	    != 0)
		atomic_fetch_add_explicit(&g_bad_dup, 1, memory_order_relaxed);

	xtc_yield();     /* hold across a yield so others contend the name */

	/* whereis must resolve to OUR pid (deterministic resolution). */
	if (xtc_reg_whereis(g_reg, name, &got) != XTC_OK ||
	    got.loop_id != self.loop_id || got.local_id != self.local_id ||
	    got.gen != self.gen)
		atomic_fetch_add_explicit(&g_bad_resolve, 1,
		    memory_order_relaxed);
	fold((long)((uint64_t)slot << 16 ^ got.local_id));

	atomic_fetch_sub_explicit(&g_held[slot], 1, memory_order_relaxed);
	(void)xtc_reg_unregister(g_reg, name);
	atomic_fetch_add_explicit(&g_done, 1, memory_order_relaxed);
}

static int
run_contend(uint64_t seed, int *out_done, int *out_wins, int *out_badr,
    int *out_badd, int *out_count, long *out_hash, uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	int i, rc;

	atomic_store(&g_wins, 0);
	atomic_store(&g_bad_resolve, 0);
	atomic_store(&g_bad_dup, 0);
	atomic_store(&g_done, 0);
	atomic_store(&g_hash, 0);
	for (i = 0; i < CON_NAMES; i++) atomic_store(&g_held[i], 0);

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK) return -1;
	if (xtc_reg_create(&g_reg) != XTC_OK) {
		(void)xtc_exec_fini(e); return -1;
	}
	for (i = 0; i < CON_WORK; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e, (unsigned)(i % N_LOOPS)),
		    con_worker, (void *)(intptr_t)i, NULL, NULL);

	rc = xtc_sim_exec_run(e, seed, 5000000);

	*out_done = atomic_load(&g_done);
	*out_wins = atomic_load(&g_wins);
	*out_badr = atomic_load(&g_bad_resolve);
	*out_badd = atomic_load(&g_bad_dup);
	*out_count = xtc_reg_count(g_reg);
	*out_hash = atomic_load(&g_hash);
	if (out_state) *out_state = xtc_sim_state_hash(e);
	xtc_reg_destroy(g_reg);
	g_reg = NULL;
	(void)xtc_exec_fini(e);
	return rc;
}

/* ================= churn (private names + retry buggify) ================= */

#define CHURN_WORK  6
#define CHURN_ITERS 5

static xtc_reg_t  *g_creg;
static atomic_int   g_creg_ok;       /* register/whereis/unregister triples */
static atomic_int   g_creg_bad;      /* a lookup missed its own registration */
static atomic_int   g_creg_done;

static void
churn_worker(void *arg)
{
	int id = (int)(intptr_t)arg;
	char name[32];
	xtc_pid_t self = { .loop_id = 1, .local_id = (uint32_t)(id + 100),
	    .gen = 1 };
	xtc_pid_t got;
	int it;

	/* Private name per fiber, so every register succeeds. */
	snprintf(name, sizeof name, "priv.%d", id);
	for (it = 0; it < CHURN_ITERS; it++) {
		int tries;
		if (xtc_reg_register(g_creg, name, self) != XTC_OK) {
			atomic_fetch_add_explicit(&g_creg_bad, 1,
			    memory_order_relaxed);
			break;
		}
		xtc_yield();
		/* whereis must resolve to self.  The retry buggify may make
		 * a lookup transiently miss; retry (a fresh draw succeeds). */
		for (tries = 0; tries < 100000; tries++) {
			if (xtc_reg_whereis(g_creg, name, &got) == XTC_OK)
				break;
			xtc_yield();
		}
		if (got.local_id != self.local_id)
			atomic_fetch_add_explicit(&g_creg_bad, 1,
			    memory_order_relaxed);
		(void)xtc_reg_unregister(g_creg, name);
		atomic_fetch_add_explicit(&g_creg_ok, 1, memory_order_relaxed);
	}
	atomic_fetch_add_explicit(&g_creg_done, 1, memory_order_relaxed);
}

static int
run_churn(uint64_t seed, int use_buggify, int *out_done, int *out_ok,
    int *out_bad, int *out_count, uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	int i, rc;

	atomic_store(&g_creg_ok, 0);
	atomic_store(&g_creg_bad, 0);
	atomic_store(&g_creg_done, 0);

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK) return -1;
	if (xtc_reg_create(&g_creg) != XTC_OK) {
		(void)xtc_exec_fini(e); return -1;
	}
	if (use_buggify) xtc_sim_buggify_enable(1000);
	for (i = 0; i < CHURN_WORK; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e, (unsigned)(i % N_LOOPS)),
		    churn_worker, (void *)(intptr_t)i, NULL, NULL);

	rc = xtc_sim_exec_run(e, seed, 5000000);

	*out_done = atomic_load(&g_creg_done);
	*out_ok = atomic_load(&g_creg_ok);
	*out_bad = atomic_load(&g_creg_bad);
	*out_count = xtc_reg_count(g_creg);
	if (out_state) *out_state = xtc_sim_state_hash(e);
	if (use_buggify) xtc_sim_buggify_disable();
	xtc_reg_destroy(g_creg);
	g_creg = NULL;
	(void)xtc_exec_fini(e);
	return rc;
}

int
main(void)
{
	int rc;

	/* ---- contend: at-most-one holder, deterministic resolve, replay ---- */
	{
		int d1 = 0, w1 = 0, br1 = 0, bd1 = 0, c1 = 0;
		int d2 = 0, w2 = 0, br2 = 0, bd2 = 0, c2 = 0;
		int d3 = 0, w3 = 0, br3 = 0, bd3 = 0, c3 = 0;
		long h1 = 0, h2 = 0, h3 = 0;
		uint64_t s1 = 0, s2 = 0, s3 = 0;

		rc = run_contend(0x2E611, &d1, &w1, &br1, &bd1, &c1, &h1, &s1);
		if (rc != XTC_OK) {
			printf("FAIL: reg contend rc=%d (hang?)\n", rc);
			return 1;
		}
		(void)run_contend(0x2E611, &d2, &w2, &br2, &bd2, &c2, &h2, &s2);
		rc = run_contend(0x99A22, &d3, &w3, &br3, &bd3, &c3, &h3, &s3);
		if (rc != XTC_OK) {
			printf("FAIL: reg contend diff-seed rc=%d\n", rc);
			return 1;
		}
		printf("contend run1: done=%d wins=%d bad-resolve=%d "
		    "bad-dup=%d count=%d hash=%ld state=%016llx\n",
		    d1, w1, br1, bd1, c1, h1, (unsigned long long)s1);
		if (d1 != CON_WORK) {
			printf("FAIL: not all reg workers finished "
			    "(done=%d want %d)\n", d1, CON_WORK); return 1;
		}
		if (br1 != 0) {
			printf("FAIL: whereis mis-resolved %d times "
			    "(non-deterministic lookup)\n", br1); return 1;
		}
		if (bd1 != 0) {
			printf("FAIL: %d duplicate concurrent registrations "
			    "of one name\n", bd1); return 1;
		}
		if (c1 != 0) {
			printf("FAIL: registry not empty after run "
			    "(count=%d -- lost unregister)\n", c1); return 1;
		}
		if (w1 < CON_NAMES) {
			printf("FAIL: too few winning registrations "
			    "(wins=%d < names=%d)\n", w1, CON_NAMES); return 1;
		}
		if (d1 != d2 || w1 != w2 || h1 != h2 || s1 != s2) {
			printf("FAIL: reg contend did not replay "
			    "(wins %d/%d hash %ld/%ld state %016llx/%016llx)\n",
			    w1, w2, h1, h2, (unsigned long long)s1,
			    (unsigned long long)s2); return 1;
		}
		if (br3 != 0 || bd3 != 0 || c3 != 0 || d3 != CON_WORK) {
			printf("FAIL: reg contend diff-seed inconsistent "
			    "(done=%d badr=%d badd=%d count=%d)\n",
			    d3, br3, bd3, c3); return 1;
		}
	}

	/* ---- churn under the transient-miss retry buggify ---- */
	{
		int d1 = 0, ok1 = 0, b1 = 0, c1 = 0;
		int d2 = 0, ok2 = 0, b2 = 0, c2 = 0;
		uint64_t s1 = 0, s2 = 0;
		int want = CHURN_WORK * CHURN_ITERS;

		rc = run_churn(0xC44E7, 1, &d1, &ok1, &b1, &c1, &s1);
		if (rc != XTC_OK) {
			printf("FAIL: reg churn+buggify rc=%d\n", rc);
			return 1;
		}
		(void)run_churn(0xC44E7, 1, &d2, &ok2, &b2, &c2, &s2);
		printf("churn+buggify run1: done=%d ok=%d (want %d) bad=%d "
		    "count=%d state=%016llx\n", d1, ok1, want, b1, c1,
		    (unsigned long long)s1);
		if (d1 != CHURN_WORK || ok1 != want || b1 != 0 || c1 != 0) {
			printf("FAIL: reg churn broke invariant "
			    "(done=%d ok=%d bad=%d count=%d)\n",
			    d1, ok1, b1, c1); return 1;
		}
		if (d1 != d2 || ok1 != ok2 || b1 != b2 || s1 != s2) {
			printf("FAIL: reg churn+buggify did not replay\n");
			return 1;
		}
	}

	printf("OK: registry at-most-one-holder + deterministic whereis + "
	    "exact unregister (empty after run), replayed; transient-miss "
	    "retry buggify holds the invariant and replays\n");
	return 0;
}
