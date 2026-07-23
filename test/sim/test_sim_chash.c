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
#include "xtc_chash.h"
#include "xtc_sim.h"

/*
 * DST coverage of xtc_chash (src/ptc/chash.c) -- the RCU-protected
 * concurrent hash table.  chash builds directly on xtc_rcu, which is
 * already DST-safe (per-fiber reader slot + cooperative-yield
 * synchronize -- see test_sim_rcu.c), so the whole table runs under
 * the single-thread sim scheduler unchanged.
 *
 * The tier this covers, and the reason chash shipped "provisional"
 * until now: its RCU-reclamation UAF-safety.  A reader that chases a
 * node into a bucket chain (or loaded the pre-resize bucket array)
 * must keep seeing a fully valid node/value until it leaves its own
 * read-side, even while a concurrent writer unlinks-and-retires that
 * node or swaps in a resized array.  A freed node fails a sentinel
 * check (and trips ASan as a heap-use-after-free).
 *
 * Workload, all across N loops under xtc_sim_exec_run:
 *
 *   Keys are int64_t boxed on the heap (caller-owned, chash never
 *   frees them).  Each VALUE is a heap `struct val` carrying a LIVE
 *   sentinel and the key it belongs to; the value is freed (sentinel
 *   poisoned first) only via a retire callback the writer schedules
 *   AFTER a grace period, mirroring how a real consumer would reclaim
 *   a chash value's own storage.  WRITER fibers each own a disjoint
 *   key stride: insert every key, then remove 3 of every 4 (keeping
 *   off%4==0) and retire each removed value.  With auto-shrink ENABLED
 *   this delete-heavy phase drops ~75% of the table and forces one or
 *   more RCU SHRINKS -- the grow path in reverse -- exercised
 *   deterministically here.  READER fibers repeatedly: rcu_read_lock,
 *   get a key, if present remember value->sentinel and value->key,
 *   YIELD (holding the read-side), then RE-CHECK the sentinel and key
 *   -- the value MUST still be live and unchanged even across a
 *   concurrent shrink that swaps the bucket array under the reader.
 *
 * Asserts: (a) NO reader observed a freed/torn value (uaf == 0),
 * (b) the final table contents exactly equal the reference model (the
 * off%4==0 survivors present with their expected value, the rest
 * removed), (c) quiescence (rc == XTC_OK), (d) byte-identical replay
 * from the seed (app hash + sim state hash) -- shrink is as
 * deterministic as grow given the same op sequence, (e) a different
 * seed reorders but stays consistent.  An RLIMIT_AS cap bounds memory
 * so a reclamation bug cannot OOM the box.
 */

#define N_LOOPS 4

#define CH_WRITERS   4
#define CH_READERS   4
#define CH_PER_W     40                    /* keys each writer owns */
#define CH_KEYSPACE  (CH_WRITERS * CH_PER_W)
#define CH_RD_ITERS  6

#define VAL_LIVE_MAGIC  0x7A17E571A17E5711ULL
#define VAL_FREED_MAGIC 0xDEADBEEFDEADBEEFULL

struct ikey { int64_t v; };

struct val {
	_Atomic uint64_t sentinel;   /* LIVE while in the table; FREED after */
	int64_t          key;        /* the key this value belongs to */
};

static int
ikey_cmp(const void *a, const void *b)
{
	int64_t x = ((const struct ikey *)a)->v;
	int64_t y = ((const struct ikey *)b)->v;
	return x < y ? -1 : (x > y ? 1 : 0);
}

static uint64_t
ikey_hash(const void *key)
{
	uint64_t x = (uint64_t)((const struct ikey *)key)->v;
	x ^= x >> 33; x *= 0xff51afd7ed558ccdULL;
	x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ULL;
	x ^= x >> 33;
	return x;
}

static xtc_chash_t   *g_h;
static struct ikey   *g_keyptrs[CH_KEYSPACE];  /* every key we malloc'd */
static struct val    *g_valptrs[CH_KEYSPACE];  /* every value we malloc'd */
static _Atomic int     g_valfreed[CH_KEYSPACE]; /* set by val_free BEFORE the
                                                 * free, so end-of-run cleanup
                                                 * knows which values it must
                                                 * NOT double-free -- read from
                                                 * this side table, never from
                                                 * the freed value's own memory */
static atomic_int      g_uaf;                  /* reader saw freed/torn val */
static atomic_int      g_rd_done;
static atomic_int      g_wr_done;
static atomic_long     g_hash;                 /* order-sensitive fold */

static void
ch_fold(long v)
{
	long h = atomic_load_explicit(&g_hash, memory_order_relaxed);
	h = h * 1000003L + (v + 1);
	atomic_store_explicit(&g_hash, h, memory_order_relaxed);
}

static void
val_free(void *p)
{
	struct val *vp = p;
	int64_t key = vp->key;
	/* Record freed-ness in the side table BEFORE freeing (read vp->key
	 * while the value is still valid), so end-of-run cleanup never has
	 * to touch this freed memory to decide whether to free it. */
	if (key >= 0 && key < CH_KEYSPACE)
		atomic_store_explicit(&g_valfreed[key], 1,
		    memory_order_release);
	/* Poison BEFORE free so a racing reader still holding this value
	 * (a bug) sees FREED even under a non-ASan build. */
	atomic_store_explicit(&vp->sentinel, VAL_FREED_MAGIC,
	    memory_order_release);
	free(vp);
}

static void
ch_writer(void *arg)
{
	int64_t base = (intptr_t)arg * CH_PER_W;
	int64_t i;

	for (i = 0; i < CH_PER_W; i++) {
		int64_t key = base + i;
		struct ikey *k = g_keyptrs[key];
		struct val *vp = g_valptrs[key];
		void *old = NULL;
		atomic_store_explicit(&vp->sentinel, VAL_LIVE_MAGIC,
		    memory_order_relaxed);
		vp->key = key;
		(void)xtc_chash_insert(g_h, k, vp, &old);
		ch_fold((long)(key * 3));
		xtc_yield();
	}
	/* Delete-heavy phase: remove 3 of every 4 keys (keep off%4==0),
	 * retiring the removed value (freed a grace period later -- a
	 * reader mid-lookup keeps a live value).  Dropping ~75% of the
	 * table with auto-shrink ON forces one or more RCU shrinks under
	 * DST, exercised deterministically. */
	for (i = 0; i < CH_PER_W; i++) {
		int64_t key = base + i;
		struct ikey lookup;
		void *removed = NULL;
		if (i % 4 == 0)
			continue;   /* survivor */
		lookup.v = key;
		if (xtc_chash_remove(g_h, &lookup, &removed) == XTC_OK &&
		    removed != NULL)
			xtc_rcu_retire(removed, val_free);
		ch_fold((long)(key * 5 + 1));
		xtc_yield();
	}
	atomic_fetch_add_explicit(&g_wr_done, 1, memory_order_relaxed);
}

static void
ch_reader(void *arg)
{
	int id = (int)(intptr_t)arg;
	int it;

	for (it = 0; it < CH_RD_ITERS; it++) {
		int64_t key;
		for (key = 0; key < CH_KEYSPACE; key += 3) {
			struct ikey lookup;
			void *v;
			lookup.v = key;
			xtc_rcu_read_lock();
			if (xtc_chash_get(g_h, &lookup, &v) == XTC_OK) {
				struct val *vp = v;
				uint64_t s0, s1;
				int64_t k0, k1;
				s0 = atomic_load_explicit(&vp->sentinel,
				    memory_order_acquire);
				k0 = vp->key;
				if (s0 != VAL_LIVE_MAGIC || k0 != key)
					atomic_fetch_add_explicit(&g_uaf, 1,
					    memory_order_relaxed);
				/* Hold the read-side across a yield: a
				 * broken reclamation surfaces here. */
				xtc_yield();
				s1 = atomic_load_explicit(&vp->sentinel,
				    memory_order_acquire);
				k1 = vp->key;
				if (s1 != VAL_LIVE_MAGIC || k1 != key)
					atomic_fetch_add_explicit(&g_uaf, 1,
					    memory_order_relaxed);
				ch_fold((long)(key + id));
			}
			xtc_rcu_read_unlock();
		}
		xtc_yield();
	}
	atomic_fetch_add_explicit(&g_rd_done, 1, memory_order_relaxed);
}

static int
run_chash(uint64_t seed, int *out_rd, int *out_wr, int *out_uaf,
    int *out_size, long *out_hash, uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	int i, rc, next = 0;

	atomic_store(&g_uaf, 0);
	atomic_store(&g_rd_done, 0);
	atomic_store(&g_wr_done, 0);
	atomic_store(&g_hash, 0);

	if (xtc_rcu_init() != XTC_OK) return -1;
	if (xtc_chash_create(ikey_cmp, ikey_hash, 8, &g_h) != XTC_OK) {
		xtc_rcu_fini();
		return -1;
	}
	/* Opt in to auto-shrink so the delete-heavy phase exercises the
	 * RCU shrink path deterministically under DST. */
	xtc_chash_set_auto_shrink(g_h, 1);
	for (i = 0; i < CH_KEYSPACE; i++) {
		g_keyptrs[i] = malloc(sizeof(struct ikey));
		g_valptrs[i] = malloc(sizeof(struct val));
		if (g_keyptrs[i] == NULL || g_valptrs[i] == NULL) {
			/* best-effort cleanup on OOM; test will fail loudly */
			return -1;
		}
		g_keyptrs[i]->v = i;
		atomic_store(&g_valptrs[i]->sentinel, VAL_LIVE_MAGIC);
		g_valptrs[i]->key = i;
		atomic_store(&g_valfreed[i], 0);
	}

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK) {
		xtc_chash_destroy(g_h); xtc_rcu_fini(); return -1;
	}
	for (i = 0; i < CH_WRITERS; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e, (unsigned)(next++ %
		    N_LOOPS)), ch_writer, (void *)(intptr_t)i, NULL, NULL);
	for (i = 0; i < CH_READERS; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e, (unsigned)(next++ %
		    N_LOOPS)), ch_reader, (void *)(intptr_t)i, NULL, NULL);

	rc = xtc_sim_exec_run(e, seed, 5000000);

	*out_rd = atomic_load(&g_rd_done);
	*out_wr = atomic_load(&g_wr_done);
	*out_uaf = atomic_load(&g_uaf);
	*out_size = (int)xtc_chash_size(g_h);
	*out_hash = atomic_load(&g_hash);
	if (out_state) *out_state = xtc_sim_state_hash(e);

	/* Drain retirements so every removed value is reclaimed before we
	 * free the survivors + teardown (off a fiber now -- plain path). */
	xtc_rcu_synchronize();
	xtc_rcu_synchronize();
	xtc_rcu_synchronize();
	xtc_rcu_synchronize();

	/* Free every value we allocated: survivors are still in the table
	 * (chash never frees a caller value), removed ones were retired +
	 * freed by val_free.  The g_valfreed side table (set by val_free
	 * BEFORE it freed) tells us which -- never read the freed value's
	 * own memory here (that would be a use-after-free). */
	for (i = 0; i < CH_KEYSPACE; i++) {
		if (!atomic_load_explicit(&g_valfreed[i], memory_order_acquire))
			free(g_valptrs[i]);   /* survivor (or never-removed) */
		free(g_keyptrs[i]);           /* keys are always ours */
	}
	xtc_chash_destroy(g_h);
	xtc_rcu_fini();
	return rc;
}

int
main(void)
{
	int rc;

#if !defined(__SANITIZE_ADDRESS__)
# if defined(__has_feature)
#  if __has_feature(address_sanitizer)
#   define XTC_CHASH_TEST_ASAN 1
#  endif
# endif
#endif
#if !defined(__SANITIZE_ADDRESS__) && !defined(XTC_CHASH_TEST_ASAN)
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
		int rd1 = 0, wr1 = 0, uaf1 = 0, sz1 = 0;
		int rd2 = 0, wr2 = 0, uaf2 = 0, sz2 = 0;
		int rd3 = 0, wr3 = 0, uaf3 = 0, sz3 = 0;
		long h1 = 0, h2 = 0, h3 = 0;
		uint64_t s1 = 0, s2 = 0, s3 = 0;
		int expect_survivors = 0, i;

		/* Reference model: every key with off%4==0 in each writer's
		 * range survives; the other 3-in-4 are removed. */
		for (i = 0; i < CH_KEYSPACE; i++) {
			int64_t off = i % CH_PER_W;
			if (off % 4 == 0) expect_survivors++;
		}

		rc = run_chash(0x5C0FF, &rd1, &wr1, &uaf1, &sz1, &h1, &s1);
		if (rc != XTC_OK) {
			printf("FAIL: chash run rc=%d (synchronize hang?)\n",
			    rc);
			return 1;
		}
		(void)run_chash(0x5C0FF, &rd2, &wr2, &uaf2, &sz2, &h2, &s2);
		rc = run_chash(0xB1DE7, &rd3, &wr3, &uaf3, &sz3, &h3, &s3);
		if (rc != XTC_OK) {
			printf("FAIL: chash diff-seed rc=%d\n", rc);
			return 1;
		}

		printf("chash run1: rd=%d wr=%d uaf=%d size=%d (expect %d) "
		    "hash=%ld state=%016llx\n", rd1, wr1, uaf1, sz1,
		    expect_survivors, h1, (unsigned long long)s1);

		if (rd1 != CH_READERS || wr1 != CH_WRITERS) {
			printf("FAIL: not all fibers finished (rd=%d/%d "
			    "wr=%d/%d)\n", rd1, CH_READERS, wr1, CH_WRITERS);
			return 1;
		}
		if (uaf1 != 0) {
			printf("FAIL: reader observed a FREED/TORN value %d "
			    "time(s) -- chash freed a value a reader held\n",
			    uaf1);
			return 1;
		}
		if (sz1 != expect_survivors) {
			printf("FAIL: final size %d != expected %d "
			    "(lost/duplicated entry)\n", sz1, expect_survivors);
			return 1;
		}
		if (rd1 != rd2 || wr1 != wr2 || uaf1 != uaf2 || sz1 != sz2 ||
		    h1 != h2 || s1 != s2) {
			printf("FAIL: chash did not replay (hash %ld/%ld "
			    "state %016llx/%016llx size %d/%d)\n", h1, h2,
			    (unsigned long long)s1, (unsigned long long)s2,
			    sz1, sz2);
			return 1;
		}
		if (rd3 != CH_READERS || wr3 != CH_WRITERS || uaf3 != 0 ||
		    sz3 != expect_survivors) {
			printf("FAIL: chash diff-seed inconsistent (rd=%d "
			    "wr=%d uaf=%d size=%d)\n", rd3, wr3, uaf3, sz3);
			return 1;
		}
	}

	printf("OK: xtc_chash under DST -- concurrent insert/remove/get "
	    "across loops, no reader saw a freed/torn value (RCU-retired "
	    "nodes outlive every reader that held them), final contents "
	    "match the reference model, replayed byte-identically and a "
	    "different seed reorders but stays consistent\n");
	return 0;
}
