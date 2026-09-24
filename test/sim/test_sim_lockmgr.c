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
#include "xtc_async.h"
#include "xtc_lockmgr.h"
#include "xtc_sim.h"

/*
 * DST coverage of the heavyweight lock manager (src/ptc/lock_mgr.c).
 * Under contention a waiter that runs inside a fiber (a proc on a loop)
 * PARKS -- it arms a waker, drops the partition lock, and yields to the
 * loop -- instead of blocking the OS thread on the condvar.  A release
 * (or the deadlock detector's abort) re-grants the entry and wakes the
 * waker.  This is the same fiber-park discipline xtc_amutex uses, so the
 * seeded scheduler exercises the lost-wakeup / stuck-waiter / mis-grant
 * interleavings a threaded lock manager hides.
 *
 * Two workloads, both across N loops under the deterministic scheduler:
 *
 *   contend: N procs, each with its own locker id, acquire a lock on one
 *            of a small set of objects (a mix of S and X modes so
 *            waiters actually queue and get granted on release), hold it
 *            across a yield, then release.  Every acquire uses a POSITIVE
 *            timeout so a conflict PARKS the fiber in the wait loop.
 *            Invariants: the run reaches quiescence (rc == XTC_OK, no
 *            hang), every acquire/release pair completes, and the run
 *            replays identically from the seed (both an app-level
 *            acquire-order hash and the engine state hash match).
 *
 *   deadlock: two procs form a classic cycle -- A holds X on obj1 and
 *             waits X on obj2; B holds X on obj2 and waits X on obj1.
 *             With DETECT_ON_BLOCK the detector runs synchronously when
 *             the second waiter parks; exactly one victim is aborted
 *             with XTC_E_DEADLK, deterministically across replays.
 */

#define N_LOOPS   4
#define N_WORK    24
#define N_OBJS    4

/* ---- contention workload ---- */
static xtc_lockmgr_t *g_mgr;
static atomic_int      g_done;
static atomic_int      g_pairs;      /* completed acquire+release pairs */
static atomic_uint_fast64_t g_order_hash;  /* order-sensitive app hash */

struct work_arg {
	int      id;
	uint64_t obj;
	int      mode;
};
static struct work_arg g_args[N_WORK];

static void
mix_hash(uint64_t v)
{
	/* Fold into a running hash under a CAS loop so the fold itself is
	 * deterministic regardless of the (deterministic) interleaving. */
	uint_fast64_t cur, nxt;
	do {
		cur = atomic_load_explicit(&g_order_hash, memory_order_relaxed);
		nxt = (cur ^ v) * 1099511628211ULL;
		nxt ^= nxt >> 29;
	} while (!atomic_compare_exchange_weak_explicit(&g_order_hash,
	    &cur, nxt, memory_order_relaxed, memory_order_relaxed));
}

static void
contend_worker(void *arg)
{
	struct work_arg *w = arg;
	xtc_locker_t id = 0;
	int rc;

	if (xtc_lockmgr_id(g_mgr, &id) != XTC_OK) {
		atomic_fetch_add_explicit(&g_done, 1, memory_order_relaxed);
		return;
	}

	/* Positive timeout: a conflict parks the fiber in the wait loop,
	 * a release re-grants us.  1s of virtual time is ample -- the
	 * critical sections are a single yield. */
	rc = xtc_lock_get(g_mgr, id, &w->obj, sizeof w->obj,
	    (xtc_lock_mode_t)w->mode, 1000000000LL);
	if (rc == XTC_OK) {
		/* Record grant order (app-visible, order-sensitive). */
		mix_hash(((uint64_t)w->id << 8) ^ w->obj ^
		    ((uint64_t)w->mode << 32));
		/* Hold across a yield so waiters queue behind us. */
		xtc_yield();
		(void)xtc_lock_put(g_mgr, id, &w->obj, sizeof w->obj);
		atomic_fetch_add_explicit(&g_pairs, 1, memory_order_relaxed);
	}
	(void)xtc_lockmgr_id_free(g_mgr, id);
	atomic_fetch_add_explicit(&g_done, 1, memory_order_relaxed);
}

static int
run_contend(uint64_t seed, int *out_done, int *out_pairs,
    uint64_t *out_order, uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	xtc_lockmgr_opts_t opts = XTC_LOCKMGR_OPTS_DEFAULT;
	int i, rc;

	/* ON_BLOCK detection: synchronous, no background thread (which
	 * cannot run on the single sim thread). */
	opts.detect_mode = XTC_LOCK_DETECT_ON_BLOCK;
	opts.n_partitions = 8;

	atomic_store(&g_done, 0);
	atomic_store(&g_pairs, 0);
	atomic_store(&g_order_hash, 1469598103934665603ULL);

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK) return -1;
	if (xtc_lockmgr_create(&opts, &g_mgr) != XTC_OK) {
		xtc_exec_fini(e); return -1;
	}
	for (i = 0; i < N_WORK; i++) {
		xtc_loop_t *l = xtc_exec_loop(e, (unsigned)(i % N_LOOPS));
		g_args[i].id = i;
		g_args[i].obj = (uint64_t)(i % N_OBJS);
		/* Every 3rd worker takes X (exclusive), rest take S (shared):
		 * S waiters coalesce, an X in the middle forces queuing. */
		g_args[i].mode = (i % 3 == 0) ? XTC_LOCK_X : XTC_LOCK_S;
		(void)xtc_proc_spawn(l, contend_worker, &g_args[i], NULL, NULL);
	}
	rc = xtc_sim_exec_run(e, seed, 5000000);
	*out_done = atomic_load(&g_done);
	*out_pairs = atomic_load(&g_pairs);
	*out_order = (uint64_t)atomic_load(&g_order_hash);
	if (out_state) *out_state = xtc_sim_state_hash(e);
	xtc_lockmgr_destroy(g_mgr);
	g_mgr = NULL;
	(void)xtc_exec_fini(e);
	return rc;
}

/* ---- deadlock workload ---- */
static xtc_lockmgr_t *g_dl_mgr;
static atomic_int      g_dl_ready;      /* both hold their first lock */
static atomic_int      g_dl_ok;         /* acquired both -> would-be no-deadlock */
static atomic_int      g_dl_deadlk;     /* got XTC_E_DEADLK */
static atomic_int      g_dl_done;

static const uint64_t  g_obj1 = 0x1111;
static const uint64_t  g_obj2 = 0x2222;

struct dl_arg { int which; };          /* 0 = A (1 then 2), 1 = B (2 then 1) */
static struct dl_arg g_dl_a = { 0 };
static struct dl_arg g_dl_b = { 1 };

static void
dl_worker(void *arg)
{
	struct dl_arg *d = arg;
	const uint64_t *first = d->which == 0 ? &g_obj1 : &g_obj2;
	const uint64_t *second = d->which == 0 ? &g_obj2 : &g_obj1;
	xtc_locker_t id = 0;
	int rc, spins;

	if (xtc_lockmgr_id(g_dl_mgr, &id) != XTC_OK) {
		atomic_fetch_add_explicit(&g_dl_done, 1, memory_order_relaxed);
		return;
	}
	/* Take the first lock (uncontended -> immediate). */
	rc = xtc_lock_get(g_dl_mgr, id, first, sizeof *first, XTC_LOCK_X, -1);
	if (rc != XTC_OK) goto out;

	/* Wait until BOTH procs hold their first lock, so the second
	 * acquire is guaranteed to conflict and form the cycle.  Bounded
	 * spin-with-yield so a lost peer cannot hang the sim. */
	atomic_fetch_add_explicit(&g_dl_ready, 1, memory_order_relaxed);
	for (spins = 0; spins < 100000 &&
	    atomic_load_explicit(&g_dl_ready, memory_order_relaxed) < 2;
	    spins++)
		xtc_yield();

	/* Cross-acquire: A wants obj2 (held by B), B wants obj1 (held by A).
	 * A positive timeout still parks the fiber; the detector aborts one
	 * victim.  Forever (-1) would also work under ON_BLOCK detection. */
	rc = xtc_lock_get(g_dl_mgr, id, second, sizeof *second, XTC_LOCK_X, -1);
	if (rc == XTC_E_DEADLK)
		atomic_fetch_add_explicit(&g_dl_deadlk, 1, memory_order_relaxed);
	else if (rc == XTC_OK)
		atomic_fetch_add_explicit(&g_dl_ok, 1, memory_order_relaxed);
out:
	(void)xtc_lock_release_all(g_dl_mgr, id);
	(void)xtc_lockmgr_id_free(g_dl_mgr, id);
	atomic_fetch_add_explicit(&g_dl_done, 1, memory_order_relaxed);
}

static int
run_deadlock(uint64_t seed, int *out_done, int *out_deadlk, int *out_ok,
    uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	xtc_lockmgr_opts_t opts = XTC_LOCKMGR_OPTS_DEFAULT;
	int rc;

	opts.detect_mode = XTC_LOCK_DETECT_ON_BLOCK;
	opts.victim = XTC_LOCK_VICTIM_DEFAULT;   /* seeded under sim -> replays */
	opts.n_partitions = 8;

	atomic_store(&g_dl_ready, 0);
	atomic_store(&g_dl_ok, 0);
	atomic_store(&g_dl_deadlk, 0);
	atomic_store(&g_dl_done, 0);

	if (xtc_exec_init(&e, 2) != XTC_OK) return -1;
	if (xtc_lockmgr_create(&opts, &g_dl_mgr) != XTC_OK) {
		xtc_exec_fini(e); return -1;
	}
	(void)xtc_proc_spawn(xtc_exec_loop(e, 0), dl_worker, &g_dl_a, NULL, NULL);
	(void)xtc_proc_spawn(xtc_exec_loop(e, 1), dl_worker, &g_dl_b, NULL, NULL);

	rc = xtc_sim_exec_run(e, seed, 5000000);
	*out_done = atomic_load(&g_dl_done);
	*out_deadlk = atomic_load(&g_dl_deadlk);
	*out_ok = atomic_load(&g_dl_ok);
	if (out_state) *out_state = xtc_sim_state_hash(e);
	xtc_lockmgr_destroy(g_dl_mgr);
	g_dl_mgr = NULL;
	(void)xtc_exec_fini(e);
	return rc;
}

/* ---- re-request workload: the MUTUAL-EXCLUSION oracle ----
 *
 * N procs share ONE object.  Each takes a first mode, yields, then
 * RE-REQUESTS a second mode on the same object (the same-locker path),
 * yields, and releases everything.  The mode pairs deliberately include
 * a weaker re-request (S then IS) and a sideways one (IS then S, IX then
 * S) -- the shapes 1.49.5 got wrong.
 *
 * The oracle: a per-proc record of the mode each locker CURRENTLY holds
 * (its strongest granted mode, as the lock manager must enforce it),
 * updated on every grant and checked against every other record through
 * the default RIW conflict matrix.  Any pair of concurrent holders whose
 * modes conflict is a mutual-exclusion violation.  A re-request that is
 * refused (XTC_E_AGAIN / DEADLK) is fine: the invariant is only
 * "granted implies compatible".  The lock manager's own "held" view
 * cannot be the oracle -- the bug was exactly that it recorded the wrong
 * mode -- so the oracle tracks what each proc was TOLD it holds: after a
 * successful re-request of M while holding P, it holds (at least) both.
 */
#define RR_WORK 16
static xtc_lockmgr_t *g_rr_mgr;
static atomic_int     g_rr_done, g_rr_viol, g_rr_grants;
static const uint64_t g_rr_obj = 0x5151;
/* held[i] is a bitmask of modes proc i was granted and still holds. */
static atomic_uint    g_rr_held[RR_WORK];
static const uint8_t RR_CONF[9 * 9] = {
	/*         NL  S   X   WT  IX  IS  IWR RU  WW  */
	/* NL  */   0,  0,  0,  0,  0,  0,  0,  0,  0,
	/* S   */   0,  0,  1,  0,  1,  0,  1,  0,  1,
	/* X   */   0,  1,  1,  1,  1,  1,  1,  1,  1,
	/* WT  */   0,  0,  0,  0,  0,  0,  0,  0,  0,
	/* IX  */   0,  1,  1,  0,  0,  0,  0,  1,  1,
	/* IS  */   0,  0,  1,  0,  0,  0,  0,  0,  1,
	/* IWR */   0,  1,  1,  0,  0,  0,  0,  1,  1,
	/* RU  */   0,  0,  1,  0,  1,  0,  1,  0,  0,
	/* WW  */   0,  1,  1,  0,  1,  1,  1,  0,  1
};
static const int RR_PAIRS[][2] = {
	{ XTC_LOCK_S,  XTC_LOCK_IS },   /* weaker re-request */
	{ XTC_LOCK_IS, XTC_LOCK_S  },   /* sideways, S vs others' IX */
	{ XTC_LOCK_IX, XTC_LOCK_S  },   /* sideways */
	{ XTC_LOCK_IS, XTC_LOCK_IX },
	{ XTC_LOCK_S,  XTC_LOCK_X  },   /* real upgrade */
	{ XTC_LOCK_IX, XTC_LOCK_IS },
	{ XTC_LOCK_RU, XTC_LOCK_S  },
	{ XTC_LOCK_IS, XTC_LOCK_IS },
};
#define RR_NPAIRS ((int)(sizeof RR_PAIRS / sizeof RR_PAIRS[0]))

static void
rr_check(int self)
{
	unsigned mine = atomic_load(&g_rr_held[self]);
	int j, a, b;
	for (j = 0; j < RR_WORK; j++) {
		unsigned theirs;
		if (j == self) continue;
		theirs = atomic_load(&g_rr_held[j]);
		for (a = 1; a < 9; a++) {
			if (!(mine & (1u << a))) continue;
			for (b = 1; b < 9; b++)
				if ((theirs & (1u << b)) && RR_CONF[a * 9 + b])
					atomic_store(&g_rr_viol, 1);
		}
	}
}

static void
rr_worker(void *arg)
{
	int i = (int)(intptr_t)arg;
	int m1 = RR_PAIRS[i % RR_NPAIRS][0], m2 = RR_PAIRS[i % RR_NPAIRS][1];
	xtc_locker_t id = 0;

	if (xtc_lockmgr_id(g_rr_mgr, &id) != XTC_OK)
		goto out;
	if (xtc_lock_get(g_rr_mgr, id, &g_rr_obj, sizeof g_rr_obj,
	    (xtc_lock_mode_t)m1, 1000000LL) == XTC_OK) {
		atomic_fetch_or(&g_rr_held[i], 1u << m1);
		atomic_fetch_add(&g_rr_grants, 1);
		rr_check(i);
		xtc_yield();
		/* Short timeout: a refused re-request is legal. */
		if (xtc_lock_get(g_rr_mgr, id, &g_rr_obj, sizeof g_rr_obj,
		    (xtc_lock_mode_t)m2, 1000000LL) == XTC_OK) {
			atomic_fetch_or(&g_rr_held[i], 1u << m2);
			atomic_fetch_add(&g_rr_grants, 1);
			rr_check(i);
		}
		xtc_yield();
		rr_check(i);
		atomic_store(&g_rr_held[i], 0);        /* before release */
		(void)xtc_lock_release_all(g_rr_mgr, id);
	}
	(void)xtc_lockmgr_id_free(g_rr_mgr, id);
out:
	atomic_fetch_add(&g_rr_done, 1);
}

static int
run_reget(uint64_t seed, int *out_done, int *out_viol, int *out_grants,
    uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	xtc_lockmgr_opts_t opts = XTC_LOCKMGR_OPTS_DEFAULT;
	int i, rc;

	/* NO deadlock detection: a victim's locks are released by the
	 * detector BEFORE the victim runs to see XTC_E_DEADLK, so its oracle
	 * record would briefly overstate what it holds (a false violation).
	 * Short timeouts break every cycle instead; then a lock is released
	 * only by its owner, after the owner has cleared its record, and the
	 * oracle is exact. */
	opts.detect_mode = XTC_LOCK_DETECT_NONE;
	opts.n_partitions = 4;
	atomic_store(&g_rr_done, 0);
	atomic_store(&g_rr_viol, 0);
	atomic_store(&g_rr_grants, 0);
	for (i = 0; i < RR_WORK; i++)
		atomic_store(&g_rr_held[i], 0);
	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK) return -1;
	if (xtc_lockmgr_create(&opts, &g_rr_mgr) != XTC_OK) {
		xtc_exec_fini(e); return -1;
	}
	for (i = 0; i < RR_WORK; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e, (unsigned)(i % N_LOOPS)),
		    rr_worker, (void *)(intptr_t)i, NULL, NULL);
	rc = xtc_sim_exec_run(e, seed, 5000000);
	*out_done = atomic_load(&g_rr_done);
	*out_viol = atomic_load(&g_rr_viol);
	*out_grants = atomic_load(&g_rr_grants);
	if (out_state) *out_state = xtc_sim_state_hash(e);
	xtc_lockmgr_destroy(g_rr_mgr);
	g_rr_mgr = NULL;
	(void)xtc_exec_fini(e);
	return rc;
}

int
main(int argc, char **argv)
{
	int d1 = 0, d2 = 0, p1 = 0, p2 = 0, rc;
	uint64_t o1 = 0, o2 = 0, s1 = 0, s2 = 0;
	int dd1 = 0, dd2 = 0, dok1 = 0, dok2 = 0;

	/* --- contention: quiescence + all pairs + replay --- */
	rc = run_contend(0x10C4, &d1, &p1, &o1, &s1);
	if (rc != XTC_OK) {
		printf("FAIL: contend run rc=%d (hang/deadlock?)\n", rc);
		return 1;
	}
	(void)run_contend(0x10C4, &d2, &p2, &o2, &s2);
	printf("contend: done=%d (want %d) pairs=%d (want %d) "
	    "order=%016llx state=%016llx\n",
	    d1, N_WORK, p1, N_WORK,
	    (unsigned long long)o1, (unsigned long long)s1);
	if (d1 != N_WORK || p1 != N_WORK) {
		printf("FAIL: not all lock acquire/release pairs completed "
		    "(done=%d pairs=%d want %d)\n", d1, p1, N_WORK);
		return 1;
	}
	if (d1 != d2 || p1 != p2 || o1 != o2 || s1 != s2) {
		printf("FAIL: contend run did not replay "
		    "(order %016llx/%016llx state %016llx/%016llx)\n",
		    (unsigned long long)o1, (unsigned long long)o2,
		    (unsigned long long)s1, (unsigned long long)s2);
		return 1;
	}

	/* --- deadlock: exactly one victim + replay --- */
	rc = run_deadlock(0xDEAD, &d1, &dd1, &dok1, &s1);
	if (rc != XTC_OK) {
		printf("FAIL: deadlock run rc=%d (hang?)\n", rc);
		return 1;
	}
	(void)run_deadlock(0xDEAD, &d2, &dd2, &dok2, &s2);
	printf("deadlock: done=%d (want 2) victims=%d (want 1) "
	    "acquired-both=%d state=%016llx\n",
	    d1, dd1, dok1, (unsigned long long)s1);
	if (d1 != 2) {
		printf("FAIL: deadlock workers did not all finish "
		    "(done=%d)\n", d1);
		return 1;
	}
	if (dd1 != 1) {
		printf("FAIL: expected exactly one deadlock victim, got %d "
		    "(cycle not detected or over-aborted)\n", dd1);
		return 1;
	}
	if (dd1 != dd2 || dok1 != dok2 || s1 != s2) {
		printf("FAIL: deadlock run did not replay "
		    "(victims %d/%d state %016llx/%016llx)\n", dd1, dd2,
		    (unsigned long long)s1, (unsigned long long)s2);
		return 1;
	}

	/* --- re-request: mutual-exclusion oracle over many seeds --- */
	{
		/* argv: [<base seed> [<count>]] (the corpus convention, see
		 * test/sim/corpus/seeds.txt); default 64 seeds from
		 * 0x5EED0000. */
		int seed_i, n_seeds = 64, rd = 0, rv = 0, rg = 0, rd2 = 0,
		    rv2 = 0, rg2 = 0, total_grants = 0;
		uint64_t rs = 0, rs2 = 0, base = 0x5EED0000ULL;
		if (argc > 1)
			base = strtoull(argv[1], NULL, 0);
		if (argc > 2 && atoi(argv[2]) > 0)
			n_seeds = atoi(argv[2]);
		for (seed_i = 0; seed_i < n_seeds; seed_i++) {
			uint64_t sd = base + (uint64_t)seed_i;
			rc = run_reget(sd, &rd, &rv, &rg, &rs);
			if (rc != XTC_OK || rd != RR_WORK) {
				printf("FAIL: reget seed 0x%llx rc=%d done=%d "
				    "(want %d)\n", (unsigned long long)sd, rc, rd,
				    RR_WORK);
				return 1;
			}
			if (rv != 0) {
				printf("FAIL: reget seed 0x%llx: MUTUAL EXCLUSION "
				    "VIOLATED -- two lockers held conflicting "
				    "modes (%d grants)\n",
				    (unsigned long long)sd, rg);
				return 1;
			}
			total_grants += rg;
			if (seed_i == 0) {
				(void)run_reget(sd, &rd2, &rv2, &rg2, &rs2);
				if (rd != rd2 || rv != rv2 || rg != rg2 ||
				    rs != rs2) {
					printf("FAIL: reget run did not replay "
					    "(state %016llx/%016llx)\n",
					    (unsigned long long)rs,
					    (unsigned long long)rs2);
					return 1;
				}
			}
		}
		printf("reget: %d seeds x %d lockers, %d grants, 0 "
		    "mutual-exclusion violations\n", n_seeds, RR_WORK,
		    total_grants);
	}

	printf("OK: lock manager parks fibers under contention (all %d "
	    "acquire/release pairs complete, no hang), detects a "
	    "deadlock (1 victim), and never co-grants conflicting modes on "
	    "same-locker re-requests; all replay from seed\n", N_WORK);
	return 0;
}
