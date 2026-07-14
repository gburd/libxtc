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
#include "xtc_cskip.h"
#include "xtc_sim.h"

/*
 * DST coverage of xtc_cskip (src/ptc/cskip.c) -- the RCU-protected
 * concurrent ordered map (lock-free-reader skiplist).  Like xtc_chash
 * it builds on xtc_rcu (already DST-safe), so the whole map runs under
 * the single-thread sim scheduler unchanged.
 *
 * This is the ordered sibling of test_sim_chash: it covers the SAME
 * RCU-reclamation UAF-safety (a reader that chased into the tower must
 * keep seeing a valid node/value until it leaves its read-side, even
 * while a writer unlinks-and-retires that node), PLUS the ordered
 * queries a hash table cannot serve -- min and floor must return a
 * consistent, correctly-ordered answer under the seeded schedule.
 *
 * Workload across N loops under xtc_sim_exec_run:
 *   Keys are int64_t boxed on the heap (caller-owned).  Each VALUE is a
 *   heap `struct val` with a LIVE sentinel + its key; freed (poisoned
 *   first) only via a retire callback a grace period after removal.
 *   WRITER fibers own a disjoint key stride: insert every key, then
 *   remove every other one (retiring the removed value).  READER fibers
 *   repeatedly, inside ONE read-side: (1) get a key, remember its
 *   value sentinel+key, YIELD holding the read-side, re-check it is
 *   still live/unchanged; (2) call min + floor and assert the answers
 *   are well-ordered (min <= floor(K) <= K, and both point at live
 *   values).
 *
 * Asserts: (a) no reader saw a freed/torn value (uaf == 0), (b) no
 * reader saw an out-of-order min/floor (ord == 0), (c) final contents
 * match the reference model (odd-stride survivors present, even-stride
 * absent) and min == the smallest surviving key, (d) quiescence, (e)
 * byte-identical replay from the seed, (f) a different seed reorders
 * but stays consistent.  RLIMIT_AS caps memory.
 */

#define N_LOOPS 4

#define CK_WRITERS   4
#define CK_READERS   4
#define CK_PER_W     40                    /* keys each writer owns */
#define CK_KEYSPACE  (CK_WRITERS * CK_PER_W)
#define CK_RD_ITERS  6

#define VAL_LIVE_MAGIC  0x7A17E571A17E5711ULL
#define VAL_FREED_MAGIC 0xDEADBEEFDEADBEEFULL

struct ikey { int64_t v; };

struct val {
	_Atomic uint64_t sentinel;
	int64_t          key;
};

static int
ikey_cmp(const void *a, const void *b)
{
	int64_t x = ((const struct ikey *)a)->v;
	int64_t y = ((const struct ikey *)b)->v;
	return x < y ? -1 : (x > y ? 1 : 0);
}

static xtc_cskip_t   *g_s;
static struct ikey   *g_keyptrs[CK_KEYSPACE];
static struct val    *g_valptrs[CK_KEYSPACE];
static _Atomic int     g_valfreed[CK_KEYSPACE];
static atomic_int      g_uaf;
static atomic_int      g_ord;      /* out-of-order min/floor observed */
static atomic_int      g_rd_done;
static atomic_int      g_wr_done;
static atomic_long     g_hash;

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
	if (key >= 0 && key < CK_KEYSPACE)
		atomic_store_explicit(&g_valfreed[key], 1,
		    memory_order_release);
	atomic_store_explicit(&vp->sentinel, VAL_FREED_MAGIC,
	    memory_order_release);
	free(vp);
}

static void
ck_writer(void *arg)
{
	int64_t base = (intptr_t)arg * CK_PER_W;
	int64_t i;

	for (i = 0; i < CK_PER_W; i++) {
		int64_t key = base + i;
		struct ikey *k = g_keyptrs[key];
		struct val *vp = g_valptrs[key];
		void *old = NULL;
		atomic_store_explicit(&vp->sentinel, VAL_LIVE_MAGIC,
		    memory_order_relaxed);
		vp->key = key;
		(void)xtc_cskip_insert(g_s, k, vp, &old);
		ch_fold((long)(key * 3));
		xtc_yield();
	}
	for (i = 0; i < CK_PER_W; i += 2) {
		int64_t key = base + i;
		struct ikey lookup;
		void *removed = NULL;
		lookup.v = key;
		if (xtc_cskip_remove(g_s, &lookup, &removed) == XTC_OK &&
		    removed != NULL)
			xtc_rcu_retire(removed, val_free);
		ch_fold((long)(key * 5 + 1));
		xtc_yield();
	}
	atomic_fetch_add_explicit(&g_wr_done, 1, memory_order_relaxed);
}

static void
check_live(struct val *vp, int64_t expect_key)
{
	uint64_t s = atomic_load_explicit(&vp->sentinel, memory_order_acquire);
	if (s != VAL_LIVE_MAGIC || vp->key != expect_key)
		atomic_fetch_add_explicit(&g_uaf, 1, memory_order_relaxed);
}

static void
ck_reader(void *arg)
{
	int id = (int)(intptr_t)arg;
	int it;

	for (it = 0; it < CK_RD_ITERS; it++) {
		int64_t key;
		for (key = 0; key < CK_KEYSPACE; key += 3) {
			struct ikey lookup;
			void *v;
			lookup.v = key;
			xtc_rcu_read_lock();
			if (xtc_cskip_get(g_s, &lookup, &v) == XTC_OK) {
				struct val *vp = v;
				check_live(vp, key);
				xtc_yield();           /* hold read-side */
				check_live(vp, key);
				ch_fold((long)(key + id));
			}
			xtc_rcu_read_unlock();
		}
		/* Ordered-query invariant, all inside one read-side:
		 * min <= floor(K) <= K, and both values are live. */
		{
			void *mink = NULL, *minv = NULL;
			void *fk = NULL, *fv = NULL;
			int64_t probe = (id * 37 + it * 11) % CK_KEYSPACE;
			struct ikey q;
			q.v = probe;
			xtc_rcu_read_lock();
			if (xtc_cskip_min(g_s, &mink, &minv) == XTC_OK) {
				int64_t mk = ((struct ikey *)mink)->v;
				check_live((struct val *)minv, mk);
				if (xtc_cskip_floor(g_s, &q, &fk, &fv) ==
				    XTC_OK) {
					int64_t fkey = ((struct ikey *)fk)->v;
					if (!(mk <= fkey && fkey <= probe))
						atomic_fetch_add_explicit(&g_ord,
						    1, memory_order_relaxed);
					check_live((struct val *)fv, fkey);
				}
			}
			xtc_rcu_read_unlock();
		}
		xtc_yield();
	}
	atomic_fetch_add_explicit(&g_rd_done, 1, memory_order_relaxed);
}

static int
run_cskip(uint64_t seed, int *out_rd, int *out_wr, int *out_uaf, int *out_ord,
    int *out_size, int64_t *out_min, long *out_hash, uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	int i, rc, next = 0;

	atomic_store(&g_uaf, 0);
	atomic_store(&g_ord, 0);
	atomic_store(&g_rd_done, 0);
	atomic_store(&g_wr_done, 0);
	atomic_store(&g_hash, 0);

	if (xtc_rcu_init() != XTC_OK) return -1;
	if (xtc_cskip_create(ikey_cmp, &g_s) != XTC_OK) {
		xtc_rcu_fini();
		return -1;
	}
	for (i = 0; i < CK_KEYSPACE; i++) {
		g_keyptrs[i] = malloc(sizeof(struct ikey));
		g_valptrs[i] = malloc(sizeof(struct val));
		if (g_keyptrs[i] == NULL || g_valptrs[i] == NULL) return -1;
		g_keyptrs[i]->v = i;
		atomic_store(&g_valptrs[i]->sentinel, VAL_LIVE_MAGIC);
		g_valptrs[i]->key = i;
		atomic_store(&g_valfreed[i], 0);
	}

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK) {
		xtc_cskip_destroy(g_s); xtc_rcu_fini(); return -1;
	}
	for (i = 0; i < CK_WRITERS; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e, (unsigned)(next++ %
		    N_LOOPS)), ck_writer, (void *)(intptr_t)i, NULL, NULL);
	for (i = 0; i < CK_READERS; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e, (unsigned)(next++ %
		    N_LOOPS)), ck_reader, (void *)(intptr_t)i, NULL, NULL);

	rc = xtc_sim_exec_run(e, seed, 5000000);

	*out_rd = atomic_load(&g_rd_done);
	*out_wr = atomic_load(&g_wr_done);
	*out_uaf = atomic_load(&g_uaf);
	*out_ord = atomic_load(&g_ord);
	*out_size = (int)xtc_cskip_size(g_s);
	*out_hash = atomic_load(&g_hash);
	if (out_state) *out_state = xtc_sim_state_hash(e);

	/* Off a fiber now: min on the quiesced map. */
	{
		void *mk = NULL;
		xtc_rcu_read_lock();
		*out_min = (xtc_cskip_min(g_s, &mk, NULL) == XTC_OK) ?
		    ((struct ikey *)mk)->v : -1;
		xtc_rcu_read_unlock();
	}

	xtc_rcu_synchronize();
	xtc_rcu_synchronize();
	xtc_rcu_synchronize();
	xtc_rcu_synchronize();

	for (i = 0; i < CK_KEYSPACE; i++) {
		if (!atomic_load_explicit(&g_valfreed[i], memory_order_acquire))
			free(g_valptrs[i]);
		free(g_keyptrs[i]);
	}
	xtc_cskip_destroy(g_s);
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
#   define XTC_CSKIP_TEST_ASAN 1
#  endif
# endif
#endif
#if !defined(__SANITIZE_ADDRESS__) && !defined(XTC_CSKIP_TEST_ASAN)
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
		int rd1=0, wr1=0, uaf1=0, ord1=0, sz1=0;
		int rd2=0, wr2=0, uaf2=0, ord2=0, sz2=0;
		int rd3=0, wr3=0, uaf3=0, ord3=0, sz3=0;
		int64_t min1=-1, min2=-1, min3=-1;
		long h1=0, h2=0, h3=0;
		uint64_t s1=0, s2=0, s3=0;
		int expect_survivors = 0, i;
		int64_t expect_min = -1;

		for (i = 0; i < CK_KEYSPACE; i++) {
			int64_t off = i % CK_PER_W;
			if (off % 2 != 0) {
				expect_survivors++;
				if (expect_min < 0) expect_min = i;
			}
		}

		rc = run_cskip(0x5C0FF, &rd1,&wr1,&uaf1,&ord1,&sz1,&min1,&h1,&s1);
		if (rc != XTC_OK) {
			printf("FAIL: cskip run rc=%d (synchronize hang?)\n", rc);
			return 1;
		}
		(void)run_cskip(0x5C0FF, &rd2,&wr2,&uaf2,&ord2,&sz2,&min2,&h2,&s2);
		rc = run_cskip(0xB1DE7, &rd3,&wr3,&uaf3,&ord3,&sz3,&min3,&h3,&s3);
		if (rc != XTC_OK) { printf("FAIL: cskip diff-seed rc=%d\n", rc); return 1; }

		printf("cskip run1: rd=%d wr=%d uaf=%d ord=%d size=%d (expect %d) "
		    "min=%lld (expect %lld) hash=%ld state=%016llx\n",
		    rd1, wr1, uaf1, ord1, sz1, expect_survivors,
		    (long long)min1, (long long)expect_min, h1,
		    (unsigned long long)s1);

		if (rd1 != CK_READERS || wr1 != CK_WRITERS) {
			printf("FAIL: not all fibers finished\n"); return 1;
		}
		if (uaf1 != 0) {
			printf("FAIL: reader observed a FREED/TORN value %d "
			    "time(s)\n", uaf1); return 1;
		}
		if (ord1 != 0) {
			printf("FAIL: reader observed out-of-order min/floor %d "
			    "time(s)\n", ord1); return 1;
		}
		if (sz1 != expect_survivors) {
			printf("FAIL: final size %d != expected %d\n", sz1,
			    expect_survivors); return 1;
		}
		if (min1 != expect_min) {
			printf("FAIL: final min %lld != expected %lld\n",
			    (long long)min1, (long long)expect_min); return 1;
		}
		if (rd1!=rd2 || wr1!=wr2 || uaf1!=uaf2 || ord1!=ord2 ||
		    sz1!=sz2 || min1!=min2 || h1!=h2 || s1!=s2) {
			printf("FAIL: cskip did not replay (hash %ld/%ld "
			    "state %016llx/%016llx)\n", h1, h2,
			    (unsigned long long)s1, (unsigned long long)s2);
			return 1;
		}
		if (rd3!=CK_READERS || wr3!=CK_WRITERS || uaf3!=0 || ord3!=0 ||
		    sz3!=expect_survivors || min3!=expect_min) {
			printf("FAIL: cskip diff-seed inconsistent (uaf=%d "
			    "ord=%d size=%d min=%lld)\n", uaf3, ord3, sz3,
			    (long long)min3); return 1;
		}
	}

	printf("OK: xtc_cskip under DST -- concurrent insert/remove/get/min/"
	    "floor across loops, no reader saw a freed/torn value or an "
	    "out-of-order ordered query (RCU-retired nodes outlive every "
	    "reader that held them), final contents + min match the "
	    "reference model, replayed byte-identically and a different "
	    "seed reorders but stays consistent\n");
	return 0;
}
