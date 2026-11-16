#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <sys/resource.h>
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_rcu.h"
#include "xtc_async.h"     /* xtc_yield */
#include "xtc_sim.h"
#include "xtc_inject.h"

/*
 * DST coverage of RCU epoch reclamation (src/ptc/rcu.c) -- the LAST
 * concurrency primitive to come under deterministic simulation.
 *
 * RCU could not run under sim before for two reasons, both now fixed
 * PURELY ADDITIVELY and gated on __xtc_current_task() != NULL (the
 * production OS-thread path -- test/m13/test_rcu -- is byte-identical):
 *
 *   1. The reader slot was a single thread-local __rcu_self.  Under the
 *      single-thread sim ALL fibers share one OS thread, so fiber A's
 *      read_lock (which publishes the global epoch into its slot) would
 *      collide with fiber B's -- corrupting the per-reader epoch that
 *      synchronize() scans, and freeing a node a concurrent reader still
 *      holds.  FIX: on a fiber, key the reader slot on the CURRENT TASK
 *      (each fiber gets its own struct rcu_tls, still registered into the
 *      same global registry synchronize scans).
 *
 *   2. synchronize() spun sched_yield() waiting for readers to drain.
 *      Under sim sched_yield does not reach the DST scheduler, so the
 *      writer fiber would spin forever and the reader fibers would never
 *      run to drain -- a hang.  FIX: on a fiber, yield to the loop
 *      (xtc_yield) so reader fibers get scheduled and clear active_epoch.
 *
 * Workload, all across N loops under xtc_sim_exec_run:
 *
 *   A shared RCU-protected pointer g_ptr points at a `node` carrying a
 *   monotonically-increasing generation and a sentinel word.  READER
 *   fibers repeatedly: read_lock, load g_ptr, remember node->gen, YIELD
 *   (holding the read-side across the yield), then RE-READ node->gen and
 *   node->sentinel -- the node MUST still be live and unchanged, because
 *   RCU must not free a node any active reader could reach.  A freed node
 *   fails the sentinel check (and trips ASan as a heap-use-after-free).
 *   A WRITER fiber repeatedly: allocate a fresh node (gen+1), swap g_ptr,
 *   retire the old node (freed via a callback that bumps a counter and
 *   poisons the sentinel), synchronize().  At the end the writer drains
 *   with extra synchronize()s so every retired node is reclaimed.
 *
 * Asserts: (a) NO reader observed a freed/torn node (use_after_free ==
 * 0), (b) every retired node was eventually reclaimed (freed count ==
 * nodes allocated minus the one still published), (c) quiescence (rc ==
 * XTC_OK -- if synchronize hangs the yield fix is incomplete), (d)
 * byte-identical replay from the seed (app hash + sim state hash), (e) a
 * different seed reorders but stays consistent.  An RLIMIT_AS cap bounds
 * memory so a reclamation bug cannot OOM the box.
 */

#define N_LOOPS 4

#define RCU_READERS   6
#define RCU_WRITERS   2
#define RCU_RD_ITERS  6
#define RCU_WR_ITERS  8

#define NODE_LIVE_MAGIC  0x11FE1111FE111111ULL
#define NODE_FREED_MAGIC 0xDEADBEEFDEADBEEFULL

struct node {
	uint64_t sentinel;           /* LIVE while published; FREED after */
	uint64_t         gen;        /* monotonically increasing */
};

static struct node *g_ptr;
static int g_alloc; /* nodes allocated (incl. the seed) */
static int g_freed; /* free callback fired count */
static int g_uaf;   /* reader saw a freed/torn node (bug) */
static int g_rd_done;
static int g_wr_done;
static long g_hash; /* order-sensitive fold of observed gens */

static void
rcu_fold(long v)
{
	long h = xtc_atomic_load_long(&g_hash, xtc_mo_relaxed);
	h = (long)((unsigned long)h * 1000003UL +
		    (unsigned long)(v + 1));
	xtc_atomic_store_long(&g_hash, h, xtc_mo_relaxed);
}

static void
node_free(void *p)
{
	struct node *n = p;
	/* Poison BEFORE free so a racing reader that still holds this
	 * node (a bug) sees FREED even under a non-ASan build. */
	xtc_atomic_store_u64(&n->sentinel, NODE_FREED_MAGIC, xtc_mo_release);
	xtc_atomic_fetch_add_i32(&g_freed, 1, xtc_mo_relaxed);
	free(n);
}

static struct node *
node_new(uint64_t gen)
{
	struct node *n = malloc(sizeof *n);
	if (n == NULL) return NULL;
	xtc_atomic_store_u64(&n->sentinel, NODE_LIVE_MAGIC, xtc_mo_relaxed);
	n->gen = gen;
	xtc_atomic_fetch_add_i32(&g_alloc, 1, xtc_mo_relaxed);
	return n;
}

static void
rcu_reader(void *arg)
{
	int id = (int)(intptr_t)arg;
	int it;

	for (it = 0; it < RCU_RD_ITERS; it++) {
		struct node *n;
		uint64_t gen0, gen1, s0, s1;

		xtc_rcu_read_lock();
		/* Load the protected pointer INSIDE the read-side.  RCU
		 * guarantees this node stays live until we read_unlock. */
		n = xtc_atomic_load_ptr(&g_ptr, xtc_mo_acquire);
		if (n == NULL) { xtc_rcu_read_unlock(); xtc_yield(); continue; }
		s0 = xtc_atomic_load_u64(&n->sentinel, xtc_mo_acquire);
		gen0 = n->gen;
		if (s0 != NODE_LIVE_MAGIC)
			xtc_atomic_fetch_add_i32(&g_uaf, 1, xtc_mo_relaxed);
		/* Hold the read-side across a yield: a broken synchronize
		 * (freeing a node a reader still holds) surfaces here. */
		xtc_yield();
		s1 = xtc_atomic_load_u64(&n->sentinel, xtc_mo_acquire);
		gen1 = n->gen;
		if (s1 != NODE_LIVE_MAGIC || gen1 != gen0)
			xtc_atomic_fetch_add_i32(&g_uaf, 1, xtc_mo_relaxed);
		rcu_fold((long)(gen0 + id));
		xtc_rcu_read_unlock();
		xtc_yield();
	}
	xtc_atomic_fetch_add_i32(&g_rd_done, 1, xtc_mo_relaxed);
}

static void
rcu_writer(void *arg)
{
	int id = (int)(intptr_t)arg;
	int it;

	for (it = 0; it < RCU_WR_ITERS; it++) {
		struct node *old, *nu;
		uint64_t g;

		old = xtc_atomic_load_ptr(&g_ptr, xtc_mo_acquire);
		g = (old ? old->gen : 0) + 1;
		nu = node_new(g);
		if (nu == NULL) break;
		/* Publish the new node, then retire the old one and wait a
		 * grace period.  synchronize() must not reclaim `old` while
		 * a reader that already loaded it is still inside its
		 * read-side. */
		old = xtc_atomic_exchange_ptr(&g_ptr, nu, xtc_mo_acq_rel);
		if (old != NULL)
			xtc_rcu_retire(old, node_free);
		xtc_rcu_synchronize();
		rcu_fold((long)(g * 7 + id));
		xtc_yield();
	}
	xtc_atomic_fetch_add_i32(&g_wr_done, 1, xtc_mo_relaxed);
}

/*
 * Reinit phase: reader fibers that live through an xtc_rcu_fini +
 * xtc_rcu_init (taken while none of them is inside a read-side, as the
 * contract requires) must still be seen by synchronize afterwards.  A
 * fiber caches its reader slot in its task; fini frees every slot, so a
 * fiber that kept using the cached one would publish into a freed,
 * unregistered slot and a writer's synchronize would free a node under
 * it (plant 54, RCU_SLOT_GEN).  Each reader holds its round-2 read-side
 * across RI_HOLD yields so the writers' grace periods overlap it.
 */
#define RI_READERS 4
#define RI_HOLD 4
static int g_ri_phase;  /* 0 round 1, 1 reinit done */
static int g_ri_round1; /* readers done with round 1 */

static void
ri_reader(void *arg)
{
	int id = (int)(intptr_t)arg;
	int it, k;

	for (it = 0; it < 2; it++) { /* round 1: cache the slot */
		xtc_rcu_read_lock();
		rcu_fold((long)id);
		xtc_rcu_read_unlock();
		xtc_yield();
	}
	xtc_atomic_fetch_add_i32(&g_ri_round1, 1, xtc_mo_relaxed);
	while (xtc_atomic_load_i32(&g_ri_phase, xtc_mo_acquire) == 0)
		xtc_yield();
	for (it = 0; it < RCU_RD_ITERS; it++) {
		struct node *n;
		uint64_t gen0;

		xtc_rcu_read_lock();
		n = xtc_atomic_load_ptr(&g_ptr, xtc_mo_acquire);
		gen0 = n->gen;
		for (k = 0; k < RI_HOLD; k++) {
			xtc_yield();
			if (xtc_atomic_load_u64(&n->sentinel, xtc_mo_acquire) !=
			        NODE_LIVE_MAGIC ||
			    n->gen != gen0)
				xtc_atomic_fetch_add_i32(&g_uaf, 1,
				    xtc_mo_relaxed);
		}
		rcu_fold((long)(gen0 + id));
		xtc_rcu_read_unlock();
		xtc_yield();
	}
	xtc_atomic_fetch_add_i32(&g_rd_done, 1, xtc_mo_relaxed);
}

/* Reinit the RCU subsystem once every reader is between read-sides. */
static void
ri_reinit(void *arg)
{
	(void)arg;
	while (xtc_atomic_load_i32(&g_ri_round1, xtc_mo_acquire) < RI_READERS)
		xtc_yield();
	xtc_rcu_fini();
	(void)xtc_rcu_init();
	xtc_atomic_store_i32(&g_ri_phase, 1, xtc_mo_release);
}

static void
ri_writer(void *arg)
{
	while (xtc_atomic_load_i32(&g_ri_phase, xtc_mo_acquire) == 0)
		xtc_yield();
	rcu_writer(arg);
}

static int
run_reinit(uint64_t seed, int *out_uaf, int *out_rd, int *out_wr,
    long *out_hash, uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	struct node *last;
	int i, rc;

	xtc_atomic_store_ptr(&g_ptr, NULL, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_uaf, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_rd_done, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_wr_done, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_ri_phase, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_ri_round1, 0, xtc_mo_seq_cst);
	xtc_atomic_store_long(&g_hash, 0, xtc_mo_seq_cst);

	if (xtc_rcu_init() != XTC_OK)
		return -1;
	xtc_atomic_store_ptr(&g_ptr, node_new(1), xtc_mo_release);
	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK)
		return -1;
	for (i = 0; i < RI_READERS + RCU_WRITERS + 1; i++) {
		xtc_loop_t *l = xtc_exec_loop(e, (unsigned)(i % N_LOOPS));
		if (i < RI_READERS)
			(void)xtc_proc_spawn(l, ri_reader, (void *)(intptr_t)i,
			    NULL, NULL);
		else if (i < RI_READERS + RCU_WRITERS)
			(void)xtc_proc_spawn(l, ri_writer,
			    (void *)(intptr_t)(i - RI_READERS), NULL, NULL);
		else
			(void)xtc_proc_spawn(l, ri_reinit, NULL, NULL, NULL);
	}

	rc = xtc_sim_exec_run(e, seed, 5000000);

	last = xtc_atomic_exchange_ptr(&g_ptr, NULL, xtc_mo_acq_rel);
	if (last != NULL)
		xtc_rcu_retire(last, node_free);
	for (i = 0; i < 4; i++)
		xtc_rcu_synchronize();
	*out_uaf = xtc_atomic_load_i32(&g_uaf, xtc_mo_seq_cst);
	*out_rd = xtc_atomic_load_i32(&g_rd_done, xtc_mo_seq_cst);
	*out_wr = xtc_atomic_load_i32(&g_wr_done, xtc_mo_seq_cst);
	*out_hash = xtc_atomic_load_long(&g_hash, xtc_mo_seq_cst);
	*out_state = xtc_sim_state_hash(e);
	(void)xtc_exec_fini(e);
	xtc_rcu_fini();
	return rc;
}

/* Run the reinit phase twice on one seed: it must pass and replay. */
static int
check_reinit(uint64_t seed)
{
	int uaf1 = 0, rd1 = 0, wr1 = 0, uaf2 = 0, rd2 = 0, wr2 = 0;
	long h1 = 0, h2 = 0;
	uint64_t s1 = 0, s2 = 0;

	if (run_reinit(seed, &uaf1, &rd1, &wr1, &h1, &s1) != XTC_OK) {
		printf("FAIL: reinit: run did not quiesce (seed %llx)\n",
		    (unsigned long long)seed);
		return 1;
	}
	(void)run_reinit(seed, &uaf2, &rd2, &wr2, &h2, &s2);
	printf("rcu reinit seed %llx: rd=%d wr=%d uaf=%d hash=%ld\n",
	    (unsigned long long)seed, rd1, wr1, uaf1, h1);
	if (uaf1 != 0) {
		printf("FAIL: reinit: reader observed a FREED/TORN node %d "
		       "time(s) -- a fiber that lived through xtc_rcu_fini was "
		       "invisible to synchronize\n",
		    uaf1);
		return 1;
	}
	if (rd1 != RI_READERS || wr1 != RCU_WRITERS) {
		printf("FAIL: reinit: not all fibers finished (rd=%d wr=%d)\n",
		    rd1, wr1);
		return 1;
	}
	if (uaf1 != uaf2 || rd1 != rd2 || wr1 != wr2 || h1 != h2 || s1 != s2) {
		printf("FAIL: reinit: did not replay (hash %ld/%ld)\n", h1, h2);
		return 1;
	}
	return 0;
}

/*
 * Out-of-memory phase.  xtc_rcu must never free under a reader when it
 * has no memory.  Reader 0 enters its read-side with no slot (the
 * "rcu.slot_oom" injection point fails its slot allocation) and holds
 * it for OO_HOLD0 yields, far longer than the writer's grace periods
 * take; synchronize must still wait for it (plant 63, RCU_NOSLOT).  The
 * writer's second retirement cannot be recorded ("rcu.retire_oom"),
 * while a reader holds that node: it must be leaked, not freed (plant
 * 64, RCU_RETIRE_FREE).  The writer swaps only once a reader holds the
 * node it retires, so each plant frees a node under a reader on every
 * seed.  The test frees the leaked node itself at the end.
 */
#define OO_READERS 4
#define OO_HOLD0 200
#define OO_WR_ITERS 6
static struct node *g_oo_hold[OO_READERS]; /* node held inside, or NULL */
static struct node *g_oo_leak;             /* the unrecorded retirement */

static void
oo_noop(const char *name, void *user)
{
	(void)name;
	(void)user;
}

static void
oo_hold(int id, int n_yields)
{
	struct node *n;
	uint64_t gen0;
	int k;

	n = xtc_atomic_load_ptr(&g_ptr, xtc_mo_acquire);
	gen0 = n->gen;
	xtc_atomic_store_ptr(&g_oo_hold[id], n, xtc_mo_release);
	for (k = 0; k < n_yields; k++) {
		xtc_yield();
		if (xtc_atomic_load_u64(&n->sentinel, xtc_mo_acquire) !=
		        NODE_LIVE_MAGIC ||
		    n->gen != gen0)
			xtc_atomic_fetch_add_i32(&g_uaf, 1, xtc_mo_relaxed);
	}
	xtc_atomic_store_ptr(&g_oo_hold[id], NULL, xtc_mo_release);
	rcu_fold((long)(gen0 + id));
}

static void
oo_reader(void *arg)
{
	int id = (int)(intptr_t)arg;

	if (id == 0) {
		/* No yield between attach and detach: only this fiber's
		 * slot allocation fails. */
		(void)xtc_inject_attach("rcu.slot_oom", oo_noop, NULL);
		xtc_rcu_read_lock();
		(void)xtc_inject_detach("rcu.slot_oom");
		oo_hold(0, OO_HOLD0);
		xtc_rcu_read_unlock();
	} else {
		while (xtc_atomic_load_i32(&g_wr_done, xtc_mo_acquire) == 0) {
			xtc_rcu_read_lock();
			oo_hold(id, RI_HOLD);
			xtc_rcu_read_unlock();
			xtc_yield();
		}
	}
	xtc_atomic_fetch_add_i32(&g_rd_done, 1, xtc_mo_relaxed);
}

/* 1 if reader `id` (or any reader, id < 0) holds the published node. */
static int
oo_held(int id)
{
	struct node *cur = xtc_atomic_load_ptr(&g_ptr, xtc_mo_acquire);
	int i;

	for (i = 0; i < OO_READERS; i++)
		if ((id < 0 || i == id) &&
		    xtc_atomic_load_ptr(&g_oo_hold[i], xtc_mo_acquire) == cur)
			return 1;
	return 0;
}

static void
oo_writer(void *arg)
{
	struct node *old, *nu;
	int it, k;

	(void)arg;
	for (it = 0; it < OO_WR_ITERS; it++) {
		if (it < 2)
			while (!oo_held(it == 0 ? 0 : -1))
				xtc_yield();
		old = xtc_atomic_load_ptr(&g_ptr, xtc_mo_acquire);
		nu = node_new(old->gen + 1);
		if (nu == NULL)
			break;
		old = xtc_atomic_exchange_ptr(&g_ptr, nu, xtc_mo_acq_rel);
		if (it == 1) {
			g_oo_leak = old;
			(void)xtc_inject_attach("rcu.retire_oom", oo_noop,
			    NULL);
			xtc_rcu_retire(old, node_free);
			(void)xtc_inject_detach("rcu.retire_oom");
		} else {
			xtc_rcu_retire(old, node_free);
		}
		for (k = 0; k < 3; k++)
			xtc_rcu_synchronize();
		rcu_fold((long)(nu->gen * 7));
		xtc_yield();
	}
	xtc_atomic_store_i32(&g_wr_done, 1, xtc_mo_release);
}

extern uint64_t __xtc_rcu_leaked(void);

static int
run_oom(uint64_t seed, int *out_uaf, int *out_rd, long *out_hash,
    uint64_t *out_state, uint64_t *out_leaked)
{
	xtc_exec_t *e = NULL;
	struct node *last;
	uint64_t leaked0;
	int i, rc;

	xtc_atomic_store_ptr(&g_ptr, NULL, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_alloc, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_freed, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_uaf, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_rd_done, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_wr_done, 0, xtc_mo_seq_cst);
	xtc_atomic_store_long(&g_hash, 0, xtc_mo_seq_cst);
	for (i = 0; i < OO_READERS; i++)
		xtc_atomic_store_ptr(&g_oo_hold[i], NULL, xtc_mo_seq_cst);
	g_oo_leak = NULL;
	leaked0 = __xtc_rcu_leaked();

	if (xtc_rcu_init() != XTC_OK)
		return -1;
	xtc_atomic_store_ptr(&g_ptr, node_new(1), xtc_mo_release);
	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK)
		return -1;
	for (i = 0; i <= OO_READERS; i++) {
		xtc_loop_t *l = xtc_exec_loop(e, (unsigned)(i % N_LOOPS));
		if (i < OO_READERS)
			(void)xtc_proc_spawn(l, oo_reader, (void *)(intptr_t)i,
			    NULL, NULL);
		else
			(void)xtc_proc_spawn(l, oo_writer, NULL, NULL, NULL);
	}

	rc = xtc_sim_exec_run(e, seed, 5000000);

	last = xtc_atomic_exchange_ptr(&g_ptr, NULL, xtc_mo_acq_rel);
	if (last != NULL)
		xtc_rcu_retire(last, node_free);
	for (i = 0; i < 4; i++)
		xtc_rcu_synchronize();
	*out_uaf = xtc_atomic_load_i32(&g_uaf, xtc_mo_seq_cst);
	*out_rd = xtc_atomic_load_i32(&g_rd_done, xtc_mo_seq_cst);
	*out_hash = xtc_atomic_load_long(&g_hash, xtc_mo_seq_cst);
	*out_state = xtc_sim_state_hash(e);
	*out_leaked = __xtc_rcu_leaked() - leaked0;
	/* Every node is freed or is the one leak, which is still live. */
	if (g_oo_leak != NULL && *out_leaked == 1) {
		if (xtc_atomic_load_u64(&g_oo_leak->sentinel, xtc_mo_acquire) !=
		    NODE_LIVE_MAGIC)
			*out_uaf += 1;
		node_free(g_oo_leak);
	}
	if (xtc_atomic_load_i32(&g_freed, xtc_mo_seq_cst) !=
	    xtc_atomic_load_i32(&g_alloc, xtc_mo_seq_cst))
		*out_uaf += 1000;
	(void)xtc_exec_fini(e);
	xtc_rcu_fini();
	return rc;
}

static int
check_oom(uint64_t seed)
{
	int uaf1 = 0, rd1 = 0, uaf2 = 0, rd2 = 0;
	long h1 = 0, h2 = 0;
	uint64_t s1 = 0, s2 = 0, lk1 = 0, lk2 = 0;

	if (run_oom(seed, &uaf1, &rd1, &h1, &s1, &lk1) != XTC_OK) {
		printf("FAIL: oom: run did not quiesce (seed %llx)\n",
		    (unsigned long long)seed);
		return 1;
	}
	(void)run_oom(seed, &uaf2, &rd2, &h2, &s2, &lk2);
	printf("rcu oom seed %llx: rd=%d uaf=%d leaked=%llu hash=%ld\n",
	    (unsigned long long)seed, rd1, uaf1, (unsigned long long)lk1, h1);
	if (lk1 != 1) {
		printf("FAIL: oom: an unrecorded retire freed a node under a "
		       "reader (leaked %llu, want 1)\n",
		    (unsigned long long)lk1);
		return 1;
	}
	if (uaf1 != 0) {
		printf("FAIL: oom: a slotless reader saw a freed node (%d)\n",
		    uaf1);
		return 1;
	}
	if (rd1 != OO_READERS) {
		printf("FAIL: oom: not all readers finished (rd=%d)\n", rd1);
		return 1;
	}
	if (uaf1 != uaf2 || rd1 != rd2 || h1 != h2 || s1 != s2 || lk1 != lk2) {
		printf("FAIL: oom: did not replay (hash %ld/%ld)\n", h1, h2);
		return 1;
	}
	return 0;
}

static int
run_rcu(uint64_t seed, int *out_rd, int *out_wr, int *out_uaf,
    int *out_alloc, int *out_freed, long *out_hash, uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	struct node *seed_node, *last;
	int i, rc, next = 0;

	xtc_atomic_store_ptr(&g_ptr, NULL, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_alloc, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_freed, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_uaf, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_rd_done, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_wr_done, 0, xtc_mo_seq_cst);
	xtc_atomic_store_long(&g_hash, 0, xtc_mo_seq_cst);

	if (xtc_rcu_init() != XTC_OK) return -1;
	seed_node = node_new(1);
	if (seed_node == NULL) { xtc_rcu_fini(); return -1; }
	xtc_atomic_store_ptr(&g_ptr, seed_node, xtc_mo_release);

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK) {
		free(seed_node); xtc_rcu_fini(); return -1;
	}
	for (i = 0; i < RCU_WRITERS; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e, (unsigned)(next++ %
		    N_LOOPS)), rcu_writer, (void *)(intptr_t)i, NULL, NULL);
	for (i = 0; i < RCU_READERS; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e, (unsigned)(next++ %
		    N_LOOPS)), rcu_reader, (void *)(intptr_t)i, NULL, NULL);

	rc = xtc_sim_exec_run(e, seed, 5000000);

	/* Drain: retire the last published node and flush all buckets so
	 * every retired node is reclaimed.  Off a fiber here (the sim run
	 * has returned), so synchronize takes the plain thread path. */
	last = xtc_atomic_load_ptr(&g_ptr, xtc_mo_acquire);
	if (last != NULL) {
		xtc_atomic_store_ptr(&g_ptr, NULL, xtc_mo_release);
		xtc_rcu_retire(last, node_free);
	}
	xtc_rcu_synchronize();
	xtc_rcu_synchronize();
	xtc_rcu_synchronize();
	xtc_rcu_synchronize();

	*out_rd = xtc_atomic_load_i32(&g_rd_done, xtc_mo_seq_cst);
	*out_wr = xtc_atomic_load_i32(&g_wr_done, xtc_mo_seq_cst);
	*out_uaf = xtc_atomic_load_i32(&g_uaf, xtc_mo_seq_cst);
	*out_alloc = xtc_atomic_load_i32(&g_alloc, xtc_mo_seq_cst);
	*out_freed = xtc_atomic_load_i32(&g_freed, xtc_mo_seq_cst);
	*out_hash = xtc_atomic_load_long(&g_hash, xtc_mo_seq_cst);
	if (out_state) *out_state = xtc_sim_state_hash(e);
	(void)xtc_exec_fini(e);
	xtc_rcu_fini();
	return rc;
}

int
main(void)
{
	int rc;

	/* Bound memory so a reclamation bug (unbounded retire) cannot OOM
	 * the box -- 512 MiB is far more than this tiny workload needs.
	 * Skipped under ASan: the sanitizer reserves a huge shadow VA
	 * region, so an RLIMIT_AS cap would make it abort at startup; a
	 * leak/OOM there is caught by the sanitizer's own detector. */
#if !defined(__SANITIZE_ADDRESS__)
# if defined(__has_feature)
#  if __has_feature(address_sanitizer)
#   define XTC_RCU_TEST_ASAN 1
#  endif
# endif
#endif
#if !defined(__SANITIZE_ADDRESS__) && !defined(XTC_RCU_TEST_ASAN)
	{
		struct rlimit rl;
		if (getrlimit(RLIMIT_AS, &rl) == 0) {
			rlim_t cap = (rlim_t)512 * 1024 * 1024;
			if (rl.rlim_cur == RLIM_INFINITY || rl.rlim_cur > cap) {
				rl.rlim_cur = cap;
				(void)setrlimit(RLIMIT_AS, &rl);
			}
		}
	}
#endif

	{
		int rd1 = 0, wr1 = 0, uaf1 = 0, al1 = 0, fr1 = 0;
		int rd2 = 0, wr2 = 0, uaf2 = 0, al2 = 0, fr2 = 0;
		int rd3 = 0, wr3 = 0, uaf3 = 0, al3 = 0, fr3 = 0;
		long h1 = 0, h2 = 0, h3 = 0;
		uint64_t s1 = 0, s2 = 0, s3 = 0;

		rc = run_rcu(0x5C0FF, &rd1, &wr1, &uaf1, &al1, &fr1, &h1, &s1);
		if (rc != XTC_OK) {
			printf("FAIL: rcu run rc=%d (synchronize hang?)\n", rc);
			return 1;
		}
		(void)run_rcu(0x5C0FF, &rd2, &wr2, &uaf2, &al2, &fr2, &h2, &s2);
		rc = run_rcu(0xB1DE7, &rd3, &wr3, &uaf3, &al3, &fr3, &h3, &s3);
		if (rc != XTC_OK) {
			printf("FAIL: rcu diff-seed rc=%d\n", rc);
			return 1;
		}

		printf("rcu   run1: rd=%d wr=%d uaf=%d alloc=%d freed=%d "
		    "hash=%ld state=%016llx\n", rd1, wr1, uaf1, al1, fr1, h1,
		    (unsigned long long)s1);

		if (rd1 != RCU_READERS || wr1 != RCU_WRITERS) {
			printf("FAIL: not all fibers finished (rd=%d/%d "
			    "wr=%d/%d)\n", rd1, RCU_READERS, wr1, RCU_WRITERS);
			return 1;
		}
		/* THE data-integrity assertion: no reader ever saw a freed
		 * or torn node. */
		if (uaf1 != 0) {
			printf("FAIL: reader observed a FREED/TORN node %d "
			    "time(s) -- RCU freed a node a reader held\n",
			    uaf1);
			return 1;
		}
		/* Every allocated node must be reclaimed by the end drain
		 * (g_ptr was retired + flushed, so freed == alloc). */
		if (fr1 != al1) {
			printf("FAIL: retired nodes not all reclaimed "
			    "(freed=%d alloc=%d) -- leak or lost retire\n",
			    fr1, al1);
			return 1;
		}
		if (rd1 != rd2 || wr1 != wr2 || uaf1 != uaf2 || al1 != al2 ||
		    fr1 != fr2 || h1 != h2 || s1 != s2) {
			printf("FAIL: rcu did not replay (hash %ld/%ld "
			    "state %016llx/%016llx alloc %d/%d freed %d/%d)\n",
			    h1, h2, (unsigned long long)s1,
			    (unsigned long long)s2, al1, al2, fr1, fr2);
			return 1;
		}
		if (rd3 != RCU_READERS || wr3 != RCU_WRITERS || uaf3 != 0 ||
		    fr3 != al3) {
			printf("FAIL: rcu diff-seed inconsistent (rd=%d wr=%d "
			    "uaf=%d freed=%d alloc=%d)\n", rd3, wr3, uaf3, fr3,
			    al3);
			return 1;
		}
	}

	if (check_reinit(0x5C0FF) != 0 || check_reinit(0xB1DE7) != 0)
		return 1;
	if (check_oom(0x5C0FF) != 0 || check_oom(0xB1DE7) != 0)
		return 1;

	printf(
	    "OK: rcu under DST -- per-fiber reader slot keeps read-sides "
	    "isolated (no reader saw a freed node), cooperative-yield "
	    "synchronize drains without hanging, every retired node "
	    "reclaimed, fibers that live through xtc_rcu_fini stay "
	    "visible, nothing freed under a reader out of memory; replayed byte-identically and a different seed "
	    "reorders but stays consistent\n");
	return 0;
}
