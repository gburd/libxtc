/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m7/test_chan_mpmc_bcast.c -- M7.5 mpmc + broadcast.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_chan.h"

/* mpmc: P producers + C consumers; every message delivered exactly once. */
#define MPMC_PRODS 4
#define MPMC_CONS  4
#define MPMC_PER   500

struct mpmc_p { xtc_chan_mpmc_t *c; int id; };
struct mpmc_c { xtc_chan_mpmc_t *c; int taken; _Atomic int *seen; };

static void *
mpmc_prod(void *a)
{
	struct mpmc_p *p = a;
	int i;
	for (i = 0; i < MPMC_PER; i++) {
		intptr_t v = ((intptr_t)p->id << 24) | (i + 1);
		while (xtc_chan_mpmc_try_send(p->c, (void *)v) == XTC_E_AGAIN)
			sched_yield();
	}
	return NULL;
}

static void *
mpmc_cons(void *a)
{
	struct mpmc_c *c = a;
	int total = MPMC_PRODS * MPMC_PER;
	for (;;) {
		void *v;
		int rc = xtc_chan_mpmc_try_recv(c->c, &v);
		if (rc == XTC_E_AGAIN) {
			if (atomic_load_explicit(c->seen, memory_order_relaxed) == total)
				break;
			sched_yield();
			continue;
		}
		if (rc != XTC_OK) break;
		c->taken++;
		atomic_fetch_add_explicit(c->seen, 1, memory_order_relaxed);
	}
	return NULL;
}

static MunitResult
test_mpmc(const MunitParameter p[], void *d)
{
	xtc_chan_mpmc_t *c;
	pthread_t prods[MPMC_PRODS], cons[MPMC_CONS];
	struct mpmc_p ps[MPMC_PRODS];
	struct mpmc_c cs[MPMC_CONS];
	_Atomic int seen = 0;
	int total_taken = 0, i;
	(void)p; (void)d;

	munit_assert_int(xtc_chan_mpmc_create(NULL, 32, &c), ==, XTC_OK);
	for (i = 0; i < MPMC_PRODS; i++) {
		ps[i].c = c; ps[i].id = i;
		pthread_create(&prods[i], NULL, mpmc_prod, &ps[i]);
	}
	for (i = 0; i < MPMC_CONS; i++) {
		cs[i].c = c; cs[i].taken = 0; cs[i].seen = &seen;
		pthread_create(&cons[i], NULL, mpmc_cons, &cs[i]);
	}
	for (i = 0; i < MPMC_PRODS; i++) pthread_join(prods[i], NULL);
	for (i = 0; i < MPMC_CONS; i++) pthread_join(cons[i], NULL);
	for (i = 0; i < MPMC_CONS; i++) total_taken += cs[i].taken;
	munit_assert_int(total_taken, ==, MPMC_PRODS * MPMC_PER);
	munit_assert_int((int)atomic_load(&seen), ==, MPMC_PRODS * MPMC_PER);
	xtc_chan_mpmc_destroy(c);
	return MUNIT_OK;
}

/* broadcast: every subscriber sees every message until it lags. */
static MunitResult
test_broadcast_basic(const MunitParameter p[], void *d)
{
	xtc_chan_broadcast_t *c;
	xtc_chan_broadcast_recv_t *r1, *r2;
	int items[5] = { 10, 20, 30, 40, 50 };
	void *v; int lagged;
	int i;
	(void)p; (void)d;
	munit_assert_int(xtc_chan_broadcast_create(NULL, 8, &c), ==, XTC_OK);
	munit_assert_int(xtc_chan_broadcast_subscribe(c, &r1), ==, XTC_OK);
	munit_assert_int(xtc_chan_broadcast_subscribe(c, &r2), ==, XTC_OK);
	for (i = 0; i < 5; i++)
		munit_assert_int(xtc_chan_broadcast_send(c, &items[i]), ==, XTC_OK);
	for (i = 0; i < 5; i++) {
		munit_assert_int(xtc_chan_broadcast_recv(r1, &v, &lagged), ==, XTC_OK);
		munit_assert_ptr(v, ==, &items[i]);
		munit_assert_int(lagged, ==, 0);
		munit_assert_int(xtc_chan_broadcast_recv(r2, &v, &lagged), ==, XTC_OK);
		munit_assert_ptr(v, ==, &items[i]);
	}
	munit_assert_int(xtc_chan_broadcast_recv(r1, &v, &lagged), ==, XTC_E_AGAIN);
	xtc_chan_broadcast_unsubscribe(r1);
	xtc_chan_broadcast_unsubscribe(r2);
	xtc_chan_broadcast_destroy(c);
	return MUNIT_OK;
}

/* broadcast: lagger reports drops. */
static MunitResult
test_broadcast_lag(const MunitParameter p[], void *d)
{
	xtc_chan_broadcast_t *c;
	xtc_chan_broadcast_recv_t *r;
	int items[20], i;
	void *v; int lagged;
	(void)p; (void)d;
	munit_assert_int(xtc_chan_broadcast_create(NULL, 4, &c), ==, XTC_OK);
	munit_assert_int(xtc_chan_broadcast_subscribe(c, &r), ==, XTC_OK);
	for (i = 0; i < 20; i++) {
		items[i] = i;
		munit_assert_int(xtc_chan_broadcast_send(c, &items[i]), ==, XTC_OK);
	}
	/* Receiver wasn't consuming; cap=4 so we lost 16. */
	munit_assert_int(xtc_chan_broadcast_recv(r, &v, &lagged), ==, XTC_OK);
	munit_assert_int(lagged, ==, 16);
	/* The remaining 4 items are visible. */
	for (i = 0; i < 3; i++) {
		munit_assert_int(xtc_chan_broadcast_recv(r, &v, &lagged), ==, XTC_OK);
		munit_assert_int(lagged, ==, 0);
	}
	xtc_chan_broadcast_unsubscribe(r);
	xtc_chan_broadcast_destroy(c);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/mpmc",            test_mpmc,            NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/broadcast_basic", test_broadcast_basic, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/broadcast_lag",   test_broadcast_lag,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m7.5/chan", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
