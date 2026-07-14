/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * bench/bench_cskip.c
 *	Per-op cost of xtc_cskip (the RCU ordered map), single-threaded,
 *	on a warm table of N keys:
 *	  - get   (lock-free reader, O(log N) tower walk, RCU-bracketed)
 *	  - floor (the ordered query -- same walk, returns predecessor)
 *	  - insert-update (overwrite: locate + one atomic value store)
 *	  - remove+re-insert (churn: unlink + retire, then re-add)
 *
 *	A cost baseline (not a scalability study; readers scaling is a
 *	property covered by the DST test).  Usage:
 *	  bench_cskip [n_keys] [iters]   (default 4096 keys, 1e6 iters)
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

#include "xtc.h"
#include "xtc_rcu.h"
#include "xtc_cskip.h"

struct kv { int64_t k; int64_t v; };

static int
kv_cmp(const void *a, const void *b)
{
	int64_t x = ((const struct kv *)a)->k, y = ((const struct kv *)b)->k;
	return x < y ? -1 : (x > y ? 1 : 0);
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
	long iters  = argc > 2 ? atol(argv[2]) : 1000000;
	xtc_cskip_t *s = NULL;
	struct kv *boxes;
	long i;
	double t0, t1;
	volatile int64_t sink = 0;

	if (n_keys < 1) n_keys = 1;
	if (xtc_rcu_init() != XTC_OK || xtc_cskip_create(kv_cmp, &s) != XTC_OK) {
		fprintf(stderr, "setup failed\n");
		return 1;
	}
	boxes = calloc((size_t)n_keys, sizeof *boxes);
	if (boxes == NULL) { fprintf(stderr, "oom\n"); return 1; }
	for (i = 0; i < n_keys; i++) {
		void *old = NULL;
		boxes[i].k = i; boxes[i].v = i * 7 + 1;
		(void)xtc_cskip_insert(s, &boxes[i], &boxes[i], &old);
	}

	t0 = now_ns();
	for (i = 0; i < iters; i++) {
		struct kv lookup; void *v = NULL;
		lookup.k = i % n_keys;
		xtc_rcu_read_lock();
		if (xtc_cskip_get(s, &lookup, &v) == XTC_OK && v != NULL)
			sink += ((struct kv *)v)->v;
		xtc_rcu_read_unlock();
	}
	t1 = now_ns();
	printf("get:            %8.2f ns/op  (%ld keys, %ld iters)\n",
	    (t1 - t0) / (double)iters, n_keys, iters);

	t0 = now_ns();
	for (i = 0; i < iters; i++) {
		struct kv q; void *fk = NULL;
		q.k = i % n_keys;
		xtc_rcu_read_lock();
		if (xtc_cskip_floor(s, &q, &fk, NULL) == XTC_OK && fk != NULL)
			sink += ((struct kv *)fk)->k;
		xtc_rcu_read_unlock();
	}
	t1 = now_ns();
	printf("floor:          %8.2f ns/op\n", (t1 - t0) / (double)iters);

	t0 = now_ns();
	for (i = 0; i < iters; i++) {
		void *old = NULL;
		long k = i % n_keys;
		(void)xtc_cskip_insert(s, &boxes[k], &boxes[k], &old);
	}
	t1 = now_ns();
	printf("insert(update): %8.2f ns/op\n", (t1 - t0) / (double)iters);

	t0 = now_ns();
	for (i = 0; i < iters; i++) {
		struct kv lookup; void *removed = NULL, *old = NULL;
		long k = i % n_keys;
		lookup.k = k;
		if (xtc_cskip_remove(s, &lookup, &removed) == XTC_OK)
			(void)xtc_cskip_insert(s, &boxes[k], &boxes[k], &old);
	}
	t1 = now_ns();
	printf("remove+insert:  %8.2f ns/op\n", (t1 - t0) / (double)iters);

	printf("(sink=%lld, size=%zu)\n", (long long)sink, xtc_cskip_size(s));
	xtc_cskip_destroy(s);
	xtc_rcu_synchronize();
	free(boxes);
	xtc_rcu_fini();
	return 0;
}
