/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m5/test_deque.c -- verifies M5_CLAIMS.md Dq1-Dq5.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "munit.h"
#include "xtc.h"
#include "deque.h"

/* [Dq1] basic round-trip. */
static MunitResult
test_basic(const MunitParameter p[], void *d)
{
	xtc_deque_t q;
	int x = 42;
	(void)p; (void)d;
	xtc_deque_init(&q);
	munit_assert_int64(xtc_deque_len(&q), ==, 0);
	munit_assert_int(xtc_deque_push(&q, &x), ==, XTC_OK);
	munit_assert_int64(xtc_deque_len(&q), ==, 1);
	munit_assert_ptr(xtc_deque_pop(&q), ==, &x);
	munit_assert_int64(xtc_deque_len(&q), ==, 0);
	munit_assert_ptr(xtc_deque_pop(&q), ==, NULL);
	return MUNIT_OK;
}

/* [Dq2] LIFO order on owner side. */
static MunitResult
test_lifo(const MunitParameter p[], void *d)
{
	xtc_deque_t q;
	int items[8], i;
	(void)p; (void)d;
	xtc_deque_init(&q);
	for (i = 0; i < 8; i++) {
		items[i] = i;
		munit_assert_int(xtc_deque_push(&q, &items[i]), ==, XTC_OK);
	}
	for (i = 7; i >= 0; i--)
		munit_assert_ptr(xtc_deque_pop(&q), ==, &items[i]);
	return MUNIT_OK;
}

/* [Dq3] thief sees FIFO order. */
static MunitResult
test_thief_fifo(const MunitParameter p[], void *d)
{
	xtc_deque_t q;
	int items[8], i;
	(void)p; (void)d;
	xtc_deque_init(&q);
	for (i = 0; i < 8; i++) {
		items[i] = i;
		munit_assert_int(xtc_deque_push(&q, &items[i]), ==, XTC_OK);
	}
	for (i = 0; i < 8; i++)
		munit_assert_ptr(xtc_deque_steal(&q), ==, &items[i]);
	return MUNIT_OK;
}

/* [Dq5] capacity overflow returns AGAIN. */
static MunitResult
test_overflow(const MunitParameter p[], void *d)
{
	xtc_deque_t q;
	static int items[XTC_DEQUE_CAP + 4];
	int i, rc;
	(void)p; (void)d;
	xtc_deque_init(&q);
	for (i = 0; i < XTC_DEQUE_CAP; i++) {
		items[i] = i;
		munit_assert_int(xtc_deque_push(&q, &items[i]), ==, XTC_OK);
	}
	rc = xtc_deque_push(&q, &items[XTC_DEQUE_CAP]);
	munit_assert_int(rc, ==, XTC_E_AGAIN);
	return MUNIT_OK;
}

/* [Dq4] no double-take under owner-pop vs thief-steal. */
#define DQ4_ITEMS 200
#define DQ4_THIEVES 4
struct dq4 { xtc_deque_t *q; _Atomic int *seen; int *counts; int idx; };

static void *
dq4_thief(void *arg)
{
	struct dq4 *c = arg;
	for (;;) {
		void *p = xtc_deque_steal(c->q);
		if (p == NULL) {
			/* Try a few more spins before giving up -- owner may
			 * still be producing. */
			int i;
			for (i = 0; i < 100; i++) {
				p = xtc_deque_steal(c->q);
				if (p != NULL) break;
			}
			if (p == NULL) break;
		}
		{
			int idx = (int)(intptr_t)p - 1;
			int prev = atomic_fetch_add_explicit(&c->seen[idx], 1,
			    memory_order_relaxed);
			(void)prev;
		}
		c->counts[c->idx]++;
	}
	return NULL;
}

static MunitResult
test_no_double_take(const MunitParameter p[], void *d)
{
	xtc_deque_t q;
	pthread_t thieves[DQ4_THIEVES];
	struct dq4 ctxs[DQ4_THIEVES];
	int counts[DQ4_THIEVES] = {0};
	_Atomic int *seen;
	int i, total_taken = 0, owner_took = 0;
	(void)p; (void)d;

	seen = calloc(DQ4_ITEMS, sizeof *seen);
	munit_assert_not_null(seen);
	xtc_deque_init(&q);

	/* Push first to give thieves something to chase.
	 * The deque convention is "non-NULL pointers only"; item 0 would
	 * push NULL and break the steal-on-empty contract.  Use 1-based
	 * indices, then subtract 1 on the receive side. */
	for (i = 0; i < DQ4_ITEMS; i++)
		munit_assert_int(xtc_deque_push(&q, (void *)(intptr_t)(i + 1)),
		    ==, XTC_OK);

	/* Start thieves. */
	for (i = 0; i < DQ4_THIEVES; i++) {
		ctxs[i].q = &q; ctxs[i].seen = seen;
		ctxs[i].counts = counts; ctxs[i].idx = i;
		munit_assert_int(pthread_create(&thieves[i], NULL, dq4_thief, &ctxs[i]),
		    ==, 0);
	}

	/* Owner pops what thieves haven't stolen. */
	for (;;) {
		void *p = xtc_deque_pop(&q);
		if (p == NULL) break;
		{
			int idx = (int)(intptr_t)p - 1;
			(void)atomic_fetch_add_explicit(&seen[idx], 1,
			    memory_order_relaxed);
			owner_took++;
		}
	}
	for (i = 0; i < DQ4_THIEVES; i++)
		pthread_join(thieves[i], NULL);

	for (i = 0; i < DQ4_THIEVES; i++) total_taken += counts[i];
	total_taken += owner_took;

	/* Every item taken exactly once. */
	munit_assert_int(total_taken, ==, DQ4_ITEMS);
	for (i = 0; i < DQ4_ITEMS; i++)
		munit_assert_int((int)atomic_load_explicit(&seen[i],
		    memory_order_relaxed), ==, 1);
	free(seen);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/Dq1_basic",        test_basic,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Dq2_lifo",         test_lifo,          NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Dq3_thief_fifo",   test_thief_fifo,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Dq4_no_double_take",test_no_double_take,NULL,NULL,MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Dq5_overflow",     test_overflow,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m5/deque", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
