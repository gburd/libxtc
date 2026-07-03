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
#include "xtc_slab.h"
#include "xtc_async.h"     /* xtc_yield */
#include "xtc_sim.h"

/*
 * DST coverage of the slab allocator (src/ptc/slab.c): a
 * libumem-style slab + per-loop magazine cache over a shared cache
 * mutex.  NON-blocking (the fast path is a lock-free magazine pop; the
 * slow path takes the cache mutex over a short section, no yield), so
 * it is sim-safe as-is -- NO shim.  Under the single-thread sim all
 * fibers share one TLS magazine per cache, but that only caches FREED
 * pointers; a LIVE object is owned by its allocator across yields, and
 * alloc/free never yield internally, so no two fibers can ever hold the
 * same live object.  The seeded scheduler owns the alloc/free
 * interleaving.
 *
 * Each fiber runs ITERS rounds: alloc an object, stamp it with a
 * pointer-unique tag {id, iter, magic}, yield (so the free-list /
 * magazine churns under other fibers), verify the tag is UNCHANGED
 * (a double-alloc handing this address to another fiber would corrupt
 * it), then free.
 *
 * INVARIANTS: (a) quiescence (rc == XTC_OK); (b) NO DOUBLE-ALLOC (every
 * verify sees its own tag), NO leak (n_inuse == 0 after the run), and
 * alloc/free counts balance; (c) byte-identical REPLAY (an
 * order-sensitive fold + sim state hash); (d) a different seed reorders
 * but stays consistent.  A slab.alloc.magazine_miss buggify forces the
 * magazine-miss (cache-lock) slow path so the shared free-list is
 * exercised even when the fast path would hit.  Footprint tiny + per-run
 * free (ASan-clean).
 */

#define N_LOOPS  4
#define WORKERS  8
#define ITERS    6
#define OBJ_SIZE 48
#define MAGIC    0x5A1Bu

struct obj {
	uint32_t magic;
	uint16_t id;
	uint16_t iter;
	uint8_t  pad[OBJ_SIZE - 8];
};

static xtc_slab_t *g_slab;
static atomic_int  g_dbl;        /* a live object was corrupted (double-alloc) */
static atomic_int  g_null;       /* alloc returned NULL (OOM -- bug here) */
static atomic_int  g_done;
static atomic_long g_hash;

static void
fold(long v)
{
	long h = atomic_load_explicit(&g_hash, memory_order_relaxed);
	h = h * 1000003L + (v + 1);
	atomic_store_explicit(&g_hash, h, memory_order_relaxed);
}

static void
slab_worker(void *arg)
{
	int id = (int)(intptr_t)arg;
	int it;

	for (it = 0; it < ITERS; it++) {
		struct obj *o = xtc_slab_alloc(g_slab);
		if (o == NULL) {
			atomic_fetch_add_explicit(&g_null, 1,
			    memory_order_relaxed);
			continue;
		}
		o->magic = MAGIC;
		o->id = (uint16_t)id;
		o->iter = (uint16_t)it;
		xtc_yield();            /* churn the allocator under peers */
		/* Verify our tag survived: a double-alloc that gave this
		 * address to another fiber would have overwritten it. */
		if (o->magic != MAGIC || o->id != (uint16_t)id ||
		    o->iter != (uint16_t)it)
			atomic_fetch_add_explicit(&g_dbl, 1,
			    memory_order_relaxed);
		fold((long)id * 131 + it);
		xtc_slab_free(g_slab, o);
		xtc_yield();
	}
	atomic_fetch_add_explicit(&g_done, 1, memory_order_relaxed);
}

static int
run_slab(uint64_t seed, int *out_done, int *out_dbl,
    int *out_null, uint64_t *out_inuse, long *out_hash, uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	xtc_slab_opts_t opts = XTC_SLAB_OPTS_DEFAULT;
	xtc_slab_stats_t st;
	int i, rc;

	atomic_store(&g_dbl, 0);
	atomic_store(&g_null, 0);
	atomic_store(&g_done, 0);
	atomic_store(&g_hash, 0);

	opts.name = "slab.dst";
	opts.obj_size = sizeof(struct obj);
	opts.align = 8;
	opts.chunk_size = 4096;      /* small chunks -> exercise slow path */
	opts.magazine_size = 4;      /* small magazine -> more misses */

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK) return -1;
	if (xtc_slab_create(&opts, &g_slab) != XTC_OK) {
		(void)xtc_exec_fini(e); return -1;
	}
	for (i = 0; i < WORKERS; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e, (unsigned)(i % N_LOOPS)),
		    slab_worker, (void *)(intptr_t)i, NULL, NULL);

	rc = xtc_sim_exec_run(e, seed, 5000000);

	*out_done = atomic_load(&g_done);
	*out_dbl = atomic_load(&g_dbl);
	*out_null = atomic_load(&g_null);
	memset(&st, 0, sizeof st);
	(void)xtc_slab_stat(g_slab, &st);
	*out_inuse = st.n_inuse;
	*out_hash = atomic_load(&g_hash);
	if (out_state) *out_state = xtc_sim_state_hash(e);
	xtc_slab_destroy(g_slab);
	g_slab = NULL;
	(void)xtc_exec_fini(e);
	return rc;
}

static int
check(const char *tag, uint64_t seed_a, uint64_t seed_b)
{
	int d1 = 0, x1 = 0, n1 = 0, d2 = 0, x2 = 0, n2 = 0;
	int d3 = 0, x3 = 0, n3 = 0;
	uint64_t iu1 = 1, iu2 = 1, iu3 = 1, s1 = 0, s2 = 0, s3 = 0;
	long h1 = 0, h2 = 0, h3 = 0;
	int rc;

	rc = run_slab(seed_a, &d1, &x1, &n1, &iu1, &h1, &s1);
	if (rc != XTC_OK) {
		printf("FAIL: slab %s rc=%d (hang?)\n", tag, rc);
		return 1;
	}
	(void)run_slab(seed_a, &d2, &x2, &n2, &iu2, &h2, &s2);
	rc = run_slab(seed_b, &d3, &x3, &n3, &iu3, &h3, &s3);
	if (rc != XTC_OK) {
		printf("FAIL: slab %s diff-seed rc=%d\n", tag, rc);
		return 1;
	}
	printf("slab %-8s run1: done=%d double-alloc=%d null=%d "
	    "inuse=%llu hash=%ld state=%016llx\n", tag, d1, x1, n1,
	    (unsigned long long)iu1, h1, (unsigned long long)s1);
	if (d1 != WORKERS) {
		printf("FAIL: not all slab workers finished (done=%d "
		    "want %d)\n", d1, WORKERS); return 1;
	}
	if (x1 != 0) {
		printf("FAIL: DOUBLE-ALLOC -- %d live objects corrupted\n",
		    x1); return 1;
	}
	if (n1 != 0) {
		printf("FAIL: %d allocs returned NULL (unexpected OOM)\n", n1);
		return 1;
	}
	if (iu1 != 0) {
		printf("FAIL: slab leak -- %llu objects still in use after "
		    "run\n", (unsigned long long)iu1); return 1;
	}
	if (d1 != d2 || x1 != x2 || n1 != n2 || iu1 != iu2 || h1 != h2 ||
	    s1 != s2) {
		printf("FAIL: slab %s did not replay (hash %ld/%ld state "
		    "%016llx/%016llx)\n", tag, h1, h2,
		    (unsigned long long)s1, (unsigned long long)s2);
		return 1;
	}
	if (d3 != WORKERS || x3 != 0 || n3 != 0 || iu3 != 0) {
		printf("FAIL: slab %s diff-seed inconsistent (done=%d "
		    "double=%d null=%d inuse=%llu)\n", tag, d3, x3, n3,
		    (unsigned long long)iu3); return 1;
	}
	return 0;
}

int
main(void)
{
	if (check("seedA", 0x51AB1, 0x62BC2) != 0) return 1;
	if (check("seedC", 0x73CD3, 0x84DE4) != 0) return 1;

	printf("OK: slab no double-alloc + no leak (inuse 0) + balanced "
	    "alloc/free, replayed; different seed reorders and stays "
	    "consistent\n");
	return 0;
}
