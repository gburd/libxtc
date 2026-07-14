/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * bench/bench_chash.c
 *	Throughput of xtc_chash's three operations, single-threaded, on a
 *	warm table of N keys:
 *	  - get   (the wait-free reader path, RCU read-side bracketed)
 *	  - insert (overwrite of an existing key -- one striped-lock take
 *	            + one atomic value store, no allocation)
 *	  - remove + re-insert (the churn path: unlink + retire, then
 *	            re-add; this is the allocating path)
 *
 *	This is a cost baseline, not a scalability study -- the concurrent
 *	scaling story (disjoint stripes run in parallel; readers never
 *	block) is a property, covered by the DST test.  Here we just want
 *	the per-op nanoseconds so a regression is visible.
 *
 *	Usage: bench_chash [n_keys] [iters]   (default 4096 keys, 2e6 iters)
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "xtc.h"
#include "xtc_rcu.h"
#include "xtc_chash.h"

struct kv { int64_t k; int64_t v; };

static int
kv_cmp(const void *a, const void *b)
{
	int64_t x = ((const struct kv *)a)->k, y = ((const struct kv *)b)->k;
	return x < y ? -1 : (x > y ? 1 : 0);
}
static uint64_t
kv_hash(const void *key)
{
	uint64_t x = (uint64_t)((const struct kv *)key)->k;
	x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33;
	return x;
}

static double
now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec * 1e9 + (double)ts.tv_nsec;
}

int
main(int argc, char **argv)
{
	long n_keys = argc > 1 ? atol(argv[1]) : 4096;
	long iters  = argc > 2 ? atol(argv[2]) : 2000000;
	xtc_chash_t *h = NULL;
	struct kv *boxes;
	long i;
	double t0, t1;
	volatile int64_t sink = 0;

	if (n_keys < 1) n_keys = 1;
	if (xtc_rcu_init() != XTC_OK ||
	    xtc_chash_create(kv_cmp, kv_hash, 64, &h) != XTC_OK) {
		fprintf(stderr, "setup failed\n");
		return 1;
	}
	boxes = calloc((size_t)n_keys, sizeof *boxes);
	if (boxes == NULL) { fprintf(stderr, "oom\n"); return 1; }
	for (i = 0; i < n_keys; i++) {
		void *old = NULL;
		boxes[i].k = i; boxes[i].v = i * 7 + 1;
		(void)xtc_chash_insert(h, &boxes[i], &boxes[i], &old);
	}

	/* get */
	t0 = now_ns();
	for (i = 0; i < iters; i++) {
		struct kv lookup; void *v = NULL;
		lookup.k = i % n_keys;
		xtc_rcu_read_lock();
		if (xtc_chash_get(h, &lookup, &v) == XTC_OK && v != NULL)
			sink += ((struct kv *)v)->v;
		xtc_rcu_read_unlock();
	}
	t1 = now_ns();
	printf("get:            %8.2f ns/op  (%ld keys, %ld iters)\n",
	    (t1 - t0) / (double)iters, n_keys, iters);

	/* insert (overwrite existing -- non-allocating) */
	t0 = now_ns();
	for (i = 0; i < iters; i++) {
		void *old = NULL;
		long k = i % n_keys;
		(void)xtc_chash_insert(h, &boxes[k], &boxes[k], &old);
	}
	t1 = now_ns();
	printf("insert(update): %8.2f ns/op\n", (t1 - t0) / (double)iters);

	/* remove + re-insert churn (allocating path) */
	t0 = now_ns();
	for (i = 0; i < iters; i++) {
		struct kv lookup; void *removed = NULL, *old = NULL;
		long k = i % n_keys;
		lookup.k = k;
		if (xtc_chash_remove(h, &lookup, &removed) == XTC_OK)
			(void)xtc_chash_insert(h, &boxes[k], &boxes[k], &old);
	}
	t1 = now_ns();
	printf("remove+insert:  %8.2f ns/op\n", (t1 - t0) / (double)iters);

	printf("(sink=%lld, size=%zu)\n", (long long)sink, xtc_chash_size(h));
	xtc_chash_destroy(h);
	xtc_rcu_synchronize();
	free(boxes);
	xtc_rcu_fini();
	return 0;
}
