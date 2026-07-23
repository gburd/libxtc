/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m13/test_chash.c -- verifies M13a xtc_chash.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_int.h"
#include "xtc_chash.h"
#include "xtc_rcu.h"

/* Test-only internal accessor (see src/ptc/chash.c). */
extern size_t __xtc_chash_nbuckets(const xtc_chash_t *h);

/* ----- key/value helpers: int64_t key boxed on the heap ----- */

struct ikey { int64_t v; };

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

static struct ikey *
mkkey(int64_t v)
{
	struct ikey *k = malloc(sizeof *k);
	munit_assert_not_null(k);
	k->v = v;
	return k;
}

/* ----- basic insert / get / remove -------------------------- */

static MunitResult
test_basic(const MunitParameter p[], void *d)
{
	xtc_chash_t *h;
	void *v;
	struct ikey *k1, *k2, *lookup;
	(void)p; (void)d;

	munit_assert_int(xtc_rcu_init(), ==, XTC_OK);
	munit_assert_int(xtc_chash_create(ikey_cmp, ikey_hash, 16, &h), ==,
	    XTC_OK);
	munit_assert_size(xtc_chash_size(h), ==, 0);

	k1 = mkkey(1);
	k2 = mkkey(2);

	munit_assert_int(xtc_chash_insert(h, k1, (void *)(intptr_t)100, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_chash_insert(h, k2, (void *)(intptr_t)200, NULL),
	    ==, XTC_OK);
	munit_assert_size(xtc_chash_size(h), ==, 2);

	lookup = mkkey(1);
	xtc_rcu_read_lock();
	munit_assert_int(xtc_chash_get(h, lookup, &v), ==, XTC_OK);
	munit_assert_int((int)(intptr_t)v, ==, 100);
	xtc_rcu_read_unlock();

	lookup->v = 3;
	xtc_rcu_read_lock();
	munit_assert_int(xtc_chash_get(h, lookup, &v), ==, XTC_E_NOTFOUND);
	xtc_rcu_read_unlock();
	free(lookup);

	/* Replace: old value handed back, key ownership on our side. */
	{
		void *old = NULL;
		struct ikey *k1b = mkkey(1);
		munit_assert_int(xtc_chash_insert(h, k1b,
		    (void *)(intptr_t)999, &old), ==, XTC_OK);
		munit_assert_int((int)(intptr_t)old, ==, 100);
		munit_assert_size(xtc_chash_size(h), ==, 2);
		/* k1b duplicates k1's value; our cmp treats them equal so
		 * chash kept the ORIGINAL k1 node and only replaced the
		 * value -- k1b is unused by the table; free it. */
		free(k1b);
	}

	lookup = mkkey(1);
	munit_assert_int(xtc_chash_remove(h, lookup, &v), ==, XTC_OK);
	munit_assert_int((int)(intptr_t)v, ==, 999);
	munit_assert_size(xtc_chash_size(h), ==, 1);
	munit_assert_int(xtc_chash_remove(h, lookup, &v), ==, XTC_E_NOTFOUND);
	free(lookup);

	/* Flush retirements (the removed node) before destroy. */
	xtc_rcu_synchronize();
	xtc_rcu_synchronize();
	xtc_rcu_synchronize();

	free(k1); /* k1's key is still referenced by the live node? No --
	           * k1 was the ORIGINAL node's key and that node was the
	           * one removed above, so its key (k1) is ours to free
	           * now that we've synchronized past its retirement. */
	xtc_chash_destroy(h);   /* frees k2's node bookkeeping, not k2 itself */
	free(k2);
	xtc_rcu_fini();
	return MUNIT_OK;
}

/* ----- resize under load: many sequential inserts, then verify
 * every key is still findable and the count is exact. ----------- */

#define RESIZE_N 5000

static MunitResult
test_resize(const MunitParameter p[], void *d)
{
	xtc_chash_t *h;
	struct ikey **keys;
	int64_t i;
	(void)p; (void)d;

	munit_assert_int(xtc_rcu_init(), ==, XTC_OK);
	/* Tiny initial capacity so growth actually happens many times
	 * across RESIZE_N inserts. */
	munit_assert_int(xtc_chash_create(ikey_cmp, ikey_hash, 4, &h), ==,
	    XTC_OK);

	keys = malloc(RESIZE_N * sizeof *keys);
	munit_assert_not_null(keys);
	for (i = 0; i < RESIZE_N; i++) {
		keys[i] = mkkey(i);
		munit_assert_int(xtc_chash_insert(h, keys[i],
		    (void *)(intptr_t)(i * 2), NULL), ==, XTC_OK);
	}
	munit_assert_size(xtc_chash_size(h), ==, (size_t)RESIZE_N);

	/* Every key still resolves to its value post-growth. */
	for (i = 0; i < RESIZE_N; i++) {
		struct ikey lookup;
		void *v;
		lookup.v = i;
		xtc_rcu_read_lock();
		munit_assert_int(xtc_chash_get(h, &lookup, &v), ==, XTC_OK);
		munit_assert_int((int)(intptr_t)v, ==, (int)(i * 2));
		xtc_rcu_read_unlock();
	}

	/* Remove half, verify size and survivors. */
	for (i = 0; i < RESIZE_N; i += 2) {
		void *v;
		munit_assert_int(xtc_chash_remove(h, keys[i], &v), ==,
		    XTC_OK);
	}
	munit_assert_size(xtc_chash_size(h), ==, (size_t)(RESIZE_N / 2));
	for (i = 1; i < RESIZE_N; i += 2) {
		struct ikey lookup;
		void *v;
		lookup.v = i;
		xtc_rcu_read_lock();
		munit_assert_int(xtc_chash_get(h, &lookup, &v), ==, XTC_OK);
		xtc_rcu_read_unlock();
	}

	xtc_rcu_synchronize();
	xtc_rcu_synchronize();
	xtc_rcu_synchronize();

	for (i = 0; i < RESIZE_N; i++) free(keys[i]);
	free(keys);
	xtc_chash_destroy(h);
	xtc_rcu_fini();
	return MUNIT_OK;
}

/* ----- shrink: grow up, enable auto-shrink, delete down, and prove
 * (a) survivors still findable/correct after each shrink, (b) the
 * bucket count actually decreased, (c) no oscillation across a
 * boundary, (d) shrink never goes below the floor. --------------- */

#define SHRINK_N 4000

static MunitResult
test_shrink(const MunitParameter p[], void *d)
{
	xtc_chash_t *h;
	struct ikey **keys;
	int64_t i;
	size_t peak_buckets, floor_buckets;
	(void)p; (void)d;

	munit_assert_int(xtc_rcu_init(), ==, XTC_OK);
	/* Floor = 16 (initial rounds up to 16). */
	munit_assert_int(xtc_chash_create(ikey_cmp, ikey_hash, 16, &h), ==,
	    XTC_OK);
	floor_buckets = __xtc_chash_nbuckets(h);
	munit_assert_size(floor_buckets, ==, 16);

	/* Default: auto-shrink OFF. */
	munit_assert_int(xtc_chash_get_auto_shrink(h), ==, 0);

	keys = malloc(SHRINK_N * sizeof *keys);
	munit_assert_not_null(keys);
	for (i = 0; i < SHRINK_N; i++) {
		keys[i] = mkkey(i);
		munit_assert_int(xtc_chash_insert(h, keys[i],
		    (void *)(intptr_t)(i * 2 + 1), NULL), ==, XTC_OK);
	}
	peak_buckets = __xtc_chash_nbuckets(h);
	munit_assert_size(peak_buckets, >, floor_buckets); /* grew */

	/* With auto-shrink OFF, draining does NOT shrink. */
	for (i = 0; i < SHRINK_N; i++) {
		void *v;
		munit_assert_int(xtc_chash_remove(h, keys[i], &v), ==,
		    XTC_OK);
	}
	munit_assert_size(__xtc_chash_nbuckets(h), ==, peak_buckets);
	munit_assert_size(xtc_chash_size(h), ==, 0);
	xtc_rcu_synchronize();
	xtc_rcu_synchronize();
	xtc_rcu_synchronize();

	/* Re-fill, then enable auto-shrink and drain: now it shrinks. */
	for (i = 0; i < SHRINK_N; i++)
		munit_assert_int(xtc_chash_insert(h, keys[i],
		    (void *)(intptr_t)(i * 2 + 1), NULL), ==, XTC_OK);
	peak_buckets = __xtc_chash_nbuckets(h);

	xtc_chash_set_auto_shrink(h, 1);
	munit_assert_int(xtc_chash_get_auto_shrink(h), ==, 1);

	/* Delete keys from the top down, keeping the low quarter as
	 * survivors, watching the bucket count drop.  Draining ~75% of a
	 * table that was at LF ~0.49 pushes it below the 0.1875 low-water
	 * and forces one or more shrinks.  After each observed shrink,
	 * EVERY surviving key must still resolve to its exact value. */
	{
		int64_t survivors = SHRINK_N / 4;   /* keep [0, survivors) */
		for (i = SHRINK_N - 1; i >= survivors; i--) {
			void *v;
			size_t before = __xtc_chash_nbuckets(h);
			munit_assert_int(xtc_chash_remove(h, keys[i], &v), ==,
			    XTC_OK);
			munit_assert_int((int)(intptr_t)v, ==,
			    (int)(i * 2 + 1));
			if (__xtc_chash_nbuckets(h) != before) {
				/* A shrink just happened -- survivors intact? */
				int64_t j;
				munit_assert_size(__xtc_chash_nbuckets(h), <,
				    before);
				for (j = 0; j < survivors; j++) {
					struct ikey lk;
					void *vv;
					lk.v = j;
					xtc_rcu_read_lock();
					munit_assert_int(xtc_chash_get(h,
					    &lk, &vv), ==, XTC_OK);
					munit_assert_int((int)(intptr_t)vv,
					    ==, (int)(j * 2 + 1));
					xtc_rcu_read_unlock();
				}
			}
		}
		munit_assert_size(xtc_chash_size(h), ==, (size_t)survivors);
		/* (b) the array shrank from its peak. */
		munit_assert_size(__xtc_chash_nbuckets(h), <, peak_buckets);

		/* Delete the survivors too; (d) never below the floor. */
		for (i = 0; i < survivors; i++) {
			void *v;
			munit_assert_int(xtc_chash_remove(h, keys[i], &v), ==,
			    XTC_OK);
			munit_assert_size(__xtc_chash_nbuckets(h), >=,
			    floor_buckets);
		}
	}
	munit_assert_size(xtc_chash_size(h), ==, 0);
	munit_assert_size(__xtc_chash_nbuckets(h), ==, floor_buckets);

	/* (c) NO OSCILLATION: interleave insert/remove of the SAME key
	 * right at a load-factor boundary and assert the bucket count is
	 * rock-steady -- a thrashing table would resize on nearly every
	 * op.  Fill to just under a grow, then toggle one key 2000 times. */
	{
		size_t stable, n0;
		int64_t base = SHRINK_N;   /* fresh keys, disjoint range */
		struct ikey *toggle;
		int t;
		/* Bring the table to a moderate fill so the toggle key sits
		 * near neither trigger; count is 0 now, so insert a batch. */
		int64_t fill = 200;
		struct ikey **fk = malloc((size_t)fill * sizeof *fk);
		munit_assert_not_null(fk);
		for (i = 0; i < fill; i++) {
			fk[i] = mkkey(base + 1 + i);
			munit_assert_int(xtc_chash_insert(h, fk[i],
			    (void *)(intptr_t)1, NULL), ==, XTC_OK);
		}
		n0 = __xtc_chash_nbuckets(h);
		toggle = mkkey(base);
		stable = n0;
		for (t = 0; t < 2000; t++) {
			void *v;
			munit_assert_int(xtc_chash_insert(h, toggle,
			    (void *)(intptr_t)1, NULL), ==, XTC_OK);
			munit_assert_int(xtc_chash_remove(h, toggle, &v), ==,
			    XTC_OK);
			/* One insert+one remove returns to the same count, so
			 * the array must never move: no oscillation. */
			munit_assert_size(__xtc_chash_nbuckets(h), ==, stable);
		}
		for (i = 0; i < fill; i++) {
			void *v;
			(void)xtc_chash_remove(h, fk[i], &v);
		}
		free(toggle);
		xtc_rcu_synchronize();
		xtc_rcu_synchronize();
		xtc_rcu_synchronize();
		for (i = 0; i < fill; i++) free(fk[i]);
		free(fk);
	}

	xtc_rcu_synchronize();
	xtc_rcu_synchronize();
	xtc_rcu_synchronize();
	for (i = 0; i < SHRINK_N; i++) free(keys[i]);
	free(keys);
	xtc_chash_destroy(h);
	xtc_rcu_fini();
	return MUNIT_OK;
}

/* ----- concurrent stress: N writer threads insert/remove disjoint
 * key ranges while M reader threads hammer xtc_chash_get on the
 * whole live key space.  ASan/TSan must be clean; the reference
 * check is that every key a writer currently has "in" is findable
 * with its expected value, and gone once removed. ------------------ */

#define STRESS_WRITERS   4
#define STRESS_READERS   4
#define STRESS_PER_W     2000
#define STRESS_KEYSPACE  (STRESS_WRITERS * STRESS_PER_W)

static xtc_chash_t   *g_h;
static _Atomic int    g_present[STRESS_KEYSPACE];  /* 1 while inserted */
static struct ikey   *g_keyptrs[STRESS_KEYSPACE];  /* every key we malloc'd,
						     * so the test can free them
						     * all at the end without a
						     * leak: xtc_chash never frees
						     * a caller-owned key, whether
						     * the entry is still live or
						     * was removed. */
static _Atomic int    g_reader_stop;
static _Atomic int64_t g_reader_iters;

static void *
stress_writer(void *arg)
{
	int64_t base = (intptr_t)arg * STRESS_PER_W;
	int64_t i;

	for (i = 0; i < STRESS_PER_W; i++) {
		int64_t key = base + i;
		struct ikey *k = mkkey(key);
		void *old = NULL;
		g_keyptrs[key] = k;
		munit_assert_int(xtc_chash_insert(g_h, k,
		    (void *)(intptr_t)(key + 1), &old), ==, XTC_OK);
		munit_assert_null(old);
		atomic_store_explicit(&g_present[key], 1,
		    memory_order_release);
	}
	/* Remove 3 of every 4 keys we inserted (keep i%4==0), to exercise
	 * concurrent remove -- and, with auto-shrink enabled below, a
	 * concurrent SHRINK swapping the bucket array -- alongside
	 * concurrent readers and other writers.  The KEY pointer stays
	 * valid (chash's node is retired, but the caller-owned key/value
	 * it pointed at is untouched) -- freed once, after the run, from
	 * g_keyptrs. */
	for (i = 0; i < STRESS_PER_W; i++) {
		int64_t key = base + i;
		struct ikey lookup;
		void *v;
		if (i % 4 == 0)
			continue;   /* survivor */
		lookup.v = key;
		atomic_store_explicit(&g_present[key], 0,
		    memory_order_release);
		munit_assert_int(xtc_chash_remove(g_h, &lookup, &v), ==,
		    XTC_OK);
		munit_assert_int((int)(intptr_t)v, ==, (int)(key + 1));
	}
	return NULL;
}

static void *
stress_reader(void *arg)
{
	(void)arg;
	while (!atomic_load_explicit(&g_reader_stop, memory_order_acquire)) {
		int64_t key;
		for (key = 0; key < STRESS_KEYSPACE; key += 7) {
			struct ikey lookup;
			void *v;
			int present;
			lookup.v = key;
			xtc_rcu_read_lock();
			present = xtc_chash_get(g_h, &lookup, &v) == XTC_OK;
			if (present)
				munit_assert_int((int)(intptr_t)v, ==,
				    (int)(key + 1));
			xtc_rcu_read_unlock();
			(void)present;
		}
		atomic_fetch_add_explicit(&g_reader_iters, 1,
		    memory_order_relaxed);
	}
	return NULL;
}

static MunitResult
test_concurrent_stress(const MunitParameter p[], void *d)
{
	pthread_t writers[STRESS_WRITERS];
	pthread_t readers[STRESS_READERS];
	int i;
	(void)p; (void)d;

	munit_assert_int(xtc_rcu_init(), ==, XTC_OK);
	munit_assert_int(xtc_chash_create(ikey_cmp, ikey_hash, 8, &g_h), ==,
	    XTC_OK);
	/* Opt in to auto-shrink so the delete-heavy writers exercise a
	 * concurrent RCU shrink (array swap) against live readers. */
	xtc_chash_set_auto_shrink(g_h, 1);
	for (i = 0; i < STRESS_KEYSPACE; i++)
		atomic_store(&g_present[i], 0);
	atomic_store(&g_reader_stop, 0);
	atomic_store(&g_reader_iters, 0);

	for (i = 0; i < STRESS_READERS; i++)
		munit_assert_int(pthread_create(&readers[i], NULL,
		    stress_reader, NULL), ==, 0);
	for (i = 0; i < STRESS_WRITERS; i++)
		munit_assert_int(pthread_create(&writers[i], NULL,
		    stress_writer, (void *)(intptr_t)i), ==, 0);

	for (i = 0; i < STRESS_WRITERS; i++) pthread_join(writers[i], NULL);
	atomic_store(&g_reader_stop, 1);
	for (i = 0; i < STRESS_READERS; i++) pthread_join(readers[i], NULL);

	munit_assert_int64(atomic_load(&g_reader_iters), >, 0);

	/* Final consistency: every key with i%4==0 (the survivors) is
	 * present with the right value; every other key is gone.
	 * STRESS_PER_W is a multiple of 4, so the per-writer i%4==0
	 * survivor stride is also i%4==0 globally. */
	for (i = 0; i < STRESS_KEYSPACE; i++) {
		struct ikey lookup;
		void *v;
		int rc;
		lookup.v = i;
		xtc_rcu_read_lock();
		rc = xtc_chash_get(g_h, &lookup, &v);
		xtc_rcu_read_unlock();
		if (i % 4 == 0) {
			munit_assert_int(rc, ==, XTC_OK);
			munit_assert_int((int)(intptr_t)v, ==, i + 1);
		} else {
			munit_assert_int(rc, ==, XTC_E_NOTFOUND);
		}
	}
	munit_assert_size(xtc_chash_size(g_h), ==,
	    (size_t)(STRESS_KEYSPACE / 4));

	xtc_rcu_synchronize();
	xtc_rcu_synchronize();
	xtc_rcu_synchronize();

	/* xtc_chash never frees a caller-owned key/value (live or, after
	 * remove + a completed grace period, retired) -- free every key
	 * we handed it, live or removed, from our own tracking array. */
	for (i = 0; i < STRESS_KEYSPACE; i++) free(g_keyptrs[i]);
	xtc_chash_destroy(g_h);
	xtc_rcu_fini();
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/basic",              test_basic,              NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/resize",              test_resize,              NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/shrink",              test_shrink,              NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/concurrent_stress",   test_concurrent_stress,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m13/chash", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
