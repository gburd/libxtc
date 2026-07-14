/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m13/test_cskip.c -- verifies M13b xtc_cskip (RCU ordered map).
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
#include "xtc_cskip.h"
#include "xtc_rcu.h"

/* ----- int64_t key boxed on the heap ------------------------ */

struct ikey { int64_t v; };

static int
ikey_cmp(const void *a, const void *b)
{
	int64_t x = ((const struct ikey *)a)->v;
	int64_t y = ((const struct ikey *)b)->v;
	return x < y ? -1 : (x > y ? 1 : 0);
}

static struct ikey *
mkkey(int64_t v)
{
	struct ikey *k = malloc(sizeof *k);
	munit_assert_not_null(k);
	k->v = v;
	return k;
}

/* ----- basic insert / get / remove / overwrite -------------- */

static MunitResult
test_basic(const MunitParameter p[], void *d)
{
	xtc_cskip_t *s;
	void *v, *old;
	struct ikey *k1, *k2, *k3, lookup;
	(void)p; (void)d;

	munit_assert_int(xtc_rcu_init(), ==, XTC_OK);
	munit_assert_int(xtc_cskip_create(ikey_cmp, &s), ==, XTC_OK);
	munit_assert_size(xtc_cskip_size(s), ==, 0);

	/* insert out of order: 2, 1, 3 -- ordering is the map's job */
	k2 = mkkey(2); k1 = mkkey(1); k3 = mkkey(3);
	old = (void *)0xdead;
	munit_assert_int(xtc_cskip_insert(s, k2, (void *)(intptr_t)200, &old),
	    ==, XTC_OK);
	munit_assert_null(old);
	munit_assert_int(xtc_cskip_insert(s, k1, (void *)(intptr_t)100, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_cskip_insert(s, k3, (void *)(intptr_t)300, NULL),
	    ==, XTC_OK);
	munit_assert_size(xtc_cskip_size(s), ==, 3);

	/* get each */
	lookup.v = 1;
	xtc_rcu_read_lock();
	munit_assert_int(xtc_cskip_get(s, &lookup, &v), ==, XTC_OK);
	munit_assert_int((int)(intptr_t)v, ==, 100);
	xtc_rcu_read_unlock();

	/* miss */
	lookup.v = 99;
	xtc_rcu_read_lock();
	munit_assert_int(xtc_cskip_get(s, &lookup, &v), ==, XTC_E_NOTFOUND);
	xtc_rcu_read_unlock();

	/* overwrite returns old, size unchanged */
	old = NULL;
	munit_assert_int(xtc_cskip_insert(s, k1, (void *)(intptr_t)111, &old),
	    ==, XTC_OK);
	munit_assert_int((int)(intptr_t)old, ==, 100);
	munit_assert_size(xtc_cskip_size(s), ==, 3);
	lookup.v = 1;
	xtc_rcu_read_lock();
	munit_assert_int(xtc_cskip_get(s, &lookup, &v), ==, XTC_OK);
	munit_assert_int((int)(intptr_t)v, ==, 111);
	xtc_rcu_read_unlock();

	/* remove middle, size drops, get misses */
	{
		void *rem = NULL;
		lookup.v = 2;
		munit_assert_int(xtc_cskip_remove(s, &lookup, &rem), ==,
		    XTC_OK);
		munit_assert_int((int)(intptr_t)rem, ==, 200);
		munit_assert_size(xtc_cskip_size(s), ==, 2);
		xtc_rcu_read_lock();
		munit_assert_int(xtc_cskip_get(s, &lookup, &v), ==,
		    XTC_E_NOTFOUND);
		xtc_rcu_read_unlock();
		/* remove again -> NOTFOUND */
		rem = NULL;
		munit_assert_int(xtc_cskip_remove(s, &lookup, &rem), ==,
		    XTC_E_NOTFOUND);
		munit_assert_null(rem);
	}

	xtc_rcu_synchronize();
	free(k1); free(k2); free(k3);
	xtc_cskip_destroy(s);
	xtc_rcu_fini();
	return MUNIT_OK;
}

/* ----- ordered queries: min + floor ------------------------- */

static MunitResult
test_ordered(const MunitParameter p[], void *d)
{
	xtc_cskip_t *s;
	struct ikey *keys[64];
	void *ok, *ov;
	int i;
	(void)p; (void)d;

	munit_assert_int(xtc_rcu_init(), ==, XTC_OK);
	munit_assert_int(xtc_cskip_create(ikey_cmp, &s), ==, XTC_OK);

	/* insert 0, 10, 20, ... 630 out of order */
	for (i = 0; i < 64; i++) {
		int j = (i * 37) % 64;   /* pseudo-shuffle */
		keys[j] = mkkey(j * 10);
		munit_assert_int(xtc_cskip_insert(s, keys[j],
		    (void *)(intptr_t)(j * 10), NULL), ==, XTC_OK);
	}

	/* min is 0 */
	xtc_rcu_read_lock();
	munit_assert_int(xtc_cskip_min(s, &ok, &ov), ==, XTC_OK);
	munit_assert_int64(((struct ikey *)ok)->v, ==, 0);
	munit_assert_int((int)(intptr_t)ov, ==, 0);
	xtc_rcu_read_unlock();

	/* floor(155) -> 150; floor(150) -> 150; floor(9) -> 0; floor(-1) miss */
	{
		struct ikey q;
		q.v = 155;
		xtc_rcu_read_lock();
		munit_assert_int(xtc_cskip_floor(s, &q, &ok, NULL), ==, XTC_OK);
		munit_assert_int64(((struct ikey *)ok)->v, ==, 150);
		xtc_rcu_read_unlock();
		q.v = 150;
		xtc_rcu_read_lock();
		munit_assert_int(xtc_cskip_floor(s, &q, &ok, NULL), ==, XTC_OK);
		munit_assert_int64(((struct ikey *)ok)->v, ==, 150);
		xtc_rcu_read_unlock();
		q.v = 9;
		xtc_rcu_read_lock();
		munit_assert_int(xtc_cskip_floor(s, &q, &ok, NULL), ==, XTC_OK);
		munit_assert_int64(((struct ikey *)ok)->v, ==, 0);
		xtc_rcu_read_unlock();
		q.v = -1;
		xtc_rcu_read_lock();
		munit_assert_int(xtc_cskip_floor(s, &q, NULL, NULL), ==,
		    XTC_E_NOTFOUND);
		xtc_rcu_read_unlock();
	}

	/* remove min, new min is 10 */
	{
		struct ikey q; q.v = 0;
		munit_assert_int(xtc_cskip_remove(s, &q, NULL), ==, XTC_OK);
		xtc_rcu_read_lock();
		munit_assert_int(xtc_cskip_min(s, &ok, NULL), ==, XTC_OK);
		munit_assert_int64(((struct ikey *)ok)->v, ==, 10);
		xtc_rcu_read_unlock();
	}

	xtc_rcu_synchronize();
	for (i = 0; i < 64; i++) free(keys[i]);
	xtc_cskip_destroy(s);
	xtc_rcu_fini();
	return MUNIT_OK;
}

/* ----- concurrent writers + lock-free readers --------------- */

#define STRESS_KEYSPACE 512
#define STRESS_WRITERS  4
#define STRESS_READERS  4
#define STRESS_ROUNDS   200

static xtc_cskip_t *g_s;
static struct ikey  *g_keyptrs[STRESS_KEYSPACE];
static atomic_int    g_reader_bad;   /* torn/UAF/ordering read seen */
static atomic_int    g_go;

static void *
stress_writer(void *arg)
{
	intptr_t id = (intptr_t)arg;
	int64_t base = id * (STRESS_KEYSPACE / STRESS_WRITERS);
	int64_t span = STRESS_KEYSPACE / STRESS_WRITERS;
	int r;
	while (!atomic_load(&g_go)) { }
	for (r = 0; r < STRESS_ROUNDS; r++) {
		int64_t i;
		for (i = 0; i < span; i++) {
			int64_t k = base + i;
			void *old = NULL;
			(void)xtc_cskip_insert(g_s, g_keyptrs[k],
			    (void *)(intptr_t)(k * 3 + 1), &old);
		}
		for (i = 0; i < span; i += 2) {
			int64_t k = base + i;
			struct ikey lk; void *rem = NULL;
			lk.v = k;
			(void)xtc_cskip_remove(g_s, &lk, &rem);
		}
	}
	return NULL;
}

static void *
stress_reader(void *arg)
{
	(void)arg;
	int r;
	while (!atomic_load(&g_go)) { }
	for (r = 0; r < STRESS_ROUNDS; r++) {
		int64_t k;
		for (k = 0; k < STRESS_KEYSPACE; k++) {
			struct ikey lk; void *v;
			lk.v = k;
			xtc_rcu_read_lock();
			if (xtc_cskip_get(g_s, &lk, &v) == XTC_OK) {
				/* value encodes the key -- a torn or freed
				 * read would not satisfy this. */
				if ((intptr_t)v != (intptr_t)(k * 3 + 1))
					atomic_fetch_add(&g_reader_bad, 1);
			}
			xtc_rcu_read_unlock();
		}
		/* min must be monotone within a single read-side snapshot */
		{
			void *mk; int rc;
			xtc_rcu_read_lock();
			rc = xtc_cskip_min(g_s, &mk, NULL);
			if (rc == XTC_OK && ((struct ikey *)mk)->v < 0)
				atomic_fetch_add(&g_reader_bad, 1);
			xtc_rcu_read_unlock();
		}
	}
	return NULL;
}

static MunitResult
test_concurrent_stress(const MunitParameter p[], void *d)
{
	pthread_t wt[STRESS_WRITERS], rt[STRESS_READERS];
	int i;
	(void)p; (void)d;

	munit_assert_int(xtc_rcu_init(), ==, XTC_OK);
	munit_assert_int(xtc_cskip_create(ikey_cmp, &g_s), ==, XTC_OK);
	atomic_store(&g_reader_bad, 0);
	atomic_store(&g_go, 0);
	for (i = 0; i < STRESS_KEYSPACE; i++)
		g_keyptrs[i] = mkkey(i);

	for (i = 0; i < STRESS_WRITERS; i++)
		munit_assert_int(pthread_create(&wt[i], NULL, stress_writer,
		    (void *)(intptr_t)i), ==, 0);
	for (i = 0; i < STRESS_READERS; i++)
		munit_assert_int(pthread_create(&rt[i], NULL, stress_reader,
		    NULL), ==, 0);
	atomic_store(&g_go, 1);
	for (i = 0; i < STRESS_WRITERS; i++) pthread_join(wt[i], NULL);
	for (i = 0; i < STRESS_READERS; i++) pthread_join(rt[i], NULL);

	munit_assert_int(atomic_load(&g_reader_bad), ==, 0);

	/* Final contents: for each writer's stride, odd offsets survive. */
	{
		size_t expect = 0;
		int64_t k;
		for (k = 0; k < STRESS_KEYSPACE; k++) {
			int64_t off = k % (STRESS_KEYSPACE / STRESS_WRITERS);
			struct ikey lk; void *v; int rc;
			lk.v = k;
			xtc_rcu_read_lock();
			rc = xtc_cskip_get(g_s, &lk, &v);
			xtc_rcu_read_unlock();
			if (off % 2 != 0) {
				munit_assert_int(rc, ==, XTC_OK);
				expect++;
			} else {
				munit_assert_int(rc, ==, XTC_E_NOTFOUND);
			}
		}
		munit_assert_size(xtc_cskip_size(g_s), ==, expect);
	}

	xtc_rcu_synchronize();
	xtc_rcu_synchronize();
	xtc_rcu_synchronize();
	for (i = 0; i < STRESS_KEYSPACE; i++) free(g_keyptrs[i]);
	xtc_cskip_destroy(g_s);
	xtc_rcu_fini();
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/basic",             test_basic,             NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/ordered",           test_ordered,           NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/concurrent_stress", test_concurrent_stress, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m13/cskip", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
