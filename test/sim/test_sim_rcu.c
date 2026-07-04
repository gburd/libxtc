#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/resource.h>
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_rcu.h"
#include "xtc_async.h"     /* xtc_yield */
#include "xtc_sim.h"

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
	_Atomic uint64_t sentinel;   /* LIVE while published; FREED after */
	uint64_t         gen;        /* monotonically increasing */
};

static _Atomic(struct node *) g_ptr;
static atomic_int   g_alloc;         /* nodes allocated (incl. the seed) */
static atomic_int   g_freed;         /* free callback fired count */
static atomic_int   g_uaf;           /* reader saw a freed/torn node (bug) */
static atomic_int   g_rd_done;
static atomic_int   g_wr_done;
static atomic_long  g_hash;          /* order-sensitive fold of observed gens */

static void
rcu_fold(long v)
{
	long h = atomic_load_explicit(&g_hash, memory_order_relaxed);
	h = h * 1000003L + (v + 1);
	atomic_store_explicit(&g_hash, h, memory_order_relaxed);
}

static void
node_free(void *p)
{
	struct node *n = p;
	/* Poison BEFORE free so a racing reader that still holds this
	 * node (a bug) sees FREED even under a non-ASan build. */
	atomic_store_explicit(&n->sentinel, NODE_FREED_MAGIC,
	    memory_order_release);
	atomic_fetch_add_explicit(&g_freed, 1, memory_order_relaxed);
	free(n);
}

static struct node *
node_new(uint64_t gen)
{
	struct node *n = malloc(sizeof *n);
	if (n == NULL) return NULL;
	atomic_store_explicit(&n->sentinel, NODE_LIVE_MAGIC,
	    memory_order_relaxed);
	n->gen = gen;
	atomic_fetch_add_explicit(&g_alloc, 1, memory_order_relaxed);
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
		n = atomic_load_explicit(&g_ptr, memory_order_acquire);
		if (n == NULL) { xtc_rcu_read_unlock(); xtc_yield(); continue; }
		s0 = atomic_load_explicit(&n->sentinel, memory_order_acquire);
		gen0 = n->gen;
		if (s0 != NODE_LIVE_MAGIC)
			atomic_fetch_add_explicit(&g_uaf, 1,
			    memory_order_relaxed);
		/* Hold the read-side across a yield: a broken synchronize
		 * (freeing a node a reader still holds) surfaces here. */
		xtc_yield();
		s1 = atomic_load_explicit(&n->sentinel, memory_order_acquire);
		gen1 = n->gen;
		if (s1 != NODE_LIVE_MAGIC || gen1 != gen0)
			atomic_fetch_add_explicit(&g_uaf, 1,
			    memory_order_relaxed);
		rcu_fold((long)(gen0 + id));
		xtc_rcu_read_unlock();
		xtc_yield();
	}
	atomic_fetch_add_explicit(&g_rd_done, 1, memory_order_relaxed);
}

static void
rcu_writer(void *arg)
{
	int id = (int)(intptr_t)arg;
	int it;

	for (it = 0; it < RCU_WR_ITERS; it++) {
		struct node *old, *nu;
		uint64_t g;

		old = atomic_load_explicit(&g_ptr, memory_order_acquire);
		g = (old ? old->gen : 0) + 1;
		nu = node_new(g);
		if (nu == NULL) break;
		/* Publish the new node, then retire the old one and wait a
		 * grace period.  synchronize() must not reclaim `old` while
		 * a reader that already loaded it is still inside its
		 * read-side. */
		old = atomic_exchange_explicit(&g_ptr, nu,
		    memory_order_acq_rel);
		if (old != NULL)
			xtc_rcu_retire(old, node_free);
		xtc_rcu_synchronize();
		rcu_fold((long)(g * 7 + id));
		xtc_yield();
	}
	atomic_fetch_add_explicit(&g_wr_done, 1, memory_order_relaxed);
}

static int
run_rcu(uint64_t seed, int *out_rd, int *out_wr, int *out_uaf,
    int *out_alloc, int *out_freed, long *out_hash, uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	struct node *seed_node, *last;
	int i, rc, next = 0;

	atomic_store(&g_ptr, NULL);
	atomic_store(&g_alloc, 0);
	atomic_store(&g_freed, 0);
	atomic_store(&g_uaf, 0);
	atomic_store(&g_rd_done, 0);
	atomic_store(&g_wr_done, 0);
	atomic_store(&g_hash, 0);

	if (xtc_rcu_init() != XTC_OK) return -1;
	seed_node = node_new(1);
	if (seed_node == NULL) { xtc_rcu_fini(); return -1; }
	atomic_store_explicit(&g_ptr, seed_node, memory_order_release);

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
	last = atomic_load_explicit(&g_ptr, memory_order_acquire);
	if (last != NULL) {
		atomic_store_explicit(&g_ptr, NULL, memory_order_release);
		xtc_rcu_retire(last, node_free);
	}
	xtc_rcu_synchronize();
	xtc_rcu_synchronize();
	xtc_rcu_synchronize();
	xtc_rcu_synchronize();

	*out_rd = atomic_load(&g_rd_done);
	*out_wr = atomic_load(&g_wr_done);
	*out_uaf = atomic_load(&g_uaf);
	*out_alloc = atomic_load(&g_alloc);
	*out_freed = atomic_load(&g_freed);
	*out_hash = atomic_load(&g_hash);
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

	printf("OK: rcu under DST -- per-fiber reader slot keeps read-sides "
	    "isolated (no reader saw a freed node), cooperative-yield "
	    "synchronize drains without hanging, every retired node "
	    "reclaimed; replayed byte-identically and a different seed "
	    "reorders but stays consistent\n");
	return 0;
}
