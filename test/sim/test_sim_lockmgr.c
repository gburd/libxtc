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

int
main(void)
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

	printf("OK: lock manager parks fibers under contention (all %d "
	    "acquire/release pairs complete, no hang) and detects a "
	    "deadlock (1 victim); both replay from seed\n", N_WORK);
	return 0;
}
