/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m7/test_chan.c -- verifies M7 channel APIs.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_chan.h"
#include "xtc_loop.h"
#include "xtc_int.h"

/* ----- oneshot ---------------------------------------------------- */

static MunitResult
test_oneshot_basic(const MunitParameter p[], void *d)
{
	xtc_chan_oneshot_t *c;
	void *out = NULL;
	int marker = 7;
	(void)p; (void)d;

	munit_assert_int(xtc_chan_oneshot_create(NULL, &c), ==, XTC_OK);
	munit_assert_int(xtc_chan_oneshot_try_recv(c, &out), ==, XTC_E_AGAIN);
	munit_assert_int(xtc_chan_oneshot_send(c, &marker), ==, XTC_OK);
	munit_assert_int(xtc_chan_oneshot_try_recv(c, &out), ==, XTC_OK);
	munit_assert_ptr(out, ==, &marker);
	/* Second send must fail. */
	munit_assert_int(xtc_chan_oneshot_send(c, &marker), ==, XTC_E_INVAL);
	xtc_chan_oneshot_destroy(c);
	return MUNIT_OK;
}

/* ----- mpsc ------------------------------------------------------- */

static MunitResult
test_mpsc_basic(const MunitParameter p[], void *d)
{
	xtc_chan_mpsc_t *c;
	int items[8], i;
	void *out = NULL;
	(void)p; (void)d;
	munit_assert_int(xtc_chan_mpsc_create(NULL, 8, &c), ==, XTC_OK);
	for (i = 0; i < 8; i++) {
		items[i] = i;
		munit_assert_int(xtc_chan_mpsc_try_send(c, &items[i]),
		    ==, XTC_OK);
	}
	/* Capacity is now 8 -- next send must fail. */
	munit_assert_int(xtc_chan_mpsc_try_send(c, &items[0]),
	    ==, XTC_E_AGAIN);
	for (i = 0; i < 8; i++) {
		munit_assert_int(xtc_chan_mpsc_try_recv(c, &out), ==, XTC_OK);
		munit_assert_ptr(out, ==, &items[i]);   /* FIFO */
	}
	munit_assert_int(xtc_chan_mpsc_try_recv(c, &out), ==, XTC_E_AGAIN);
	xtc_chan_mpsc_destroy(c);
	return MUNIT_OK;
}

/* mpsc set_waker: NULL guards + the wake_now branch (a message already
 * buffered when the waker is registered) + close firing the waker. */
static MunitResult
test_mpsc_set_waker(const MunitParameter p[], void *d)
{
	xtc_chan_mpsc_t *c;
	xtc_waker_t inert = { NULL, NULL };
	int item = 3;
	(void)p; (void)d;

	munit_assert_int(xtc_chan_mpsc_create(NULL, 4, &c), ==, XTC_OK);
	munit_assert_int(xtc_chan_mpsc_set_waker(NULL, &inert), ==, XTC_E_INVAL);
	munit_assert_int(xtc_chan_mpsc_set_waker(c, NULL), ==, XTC_E_INVAL);
	/* Buffer a message, then register the waker -> wake_now branch. */
	munit_assert_int(xtc_chan_mpsc_try_send(c, &item), ==, XTC_OK);
	munit_assert_int(xtc_chan_mpsc_set_waker(c, &inert), ==, XTC_OK);
	/* Close fires the registered waker too. */
	munit_assert_int(xtc_chan_mpsc_close(c), ==, XTC_OK);
	xtc_chan_mpsc_destroy(c);
	return MUNIT_OK;
}

/* mpsc N producers, 1 consumer -- no message lost, no duplicates. */
#define NP 8
#define MSG_PER 1000
struct mpsc_ctx { xtc_chan_mpsc_t *c; int id; };

static void *
mpsc_producer(void *arg)
{
	struct mpsc_ctx *cx = arg;
	int i;
	for (i = 0; i < MSG_PER; i++) {
		intptr_t v = ((intptr_t)cx->id << 24) | (i + 1);
		while (xtc_chan_mpsc_try_send(cx->c, (void *)v) == XTC_E_AGAIN)
			__os_thread_yield();
	}
	return NULL;
}

static MunitResult
test_mpsc_concurrent(const MunitParameter p[], void *d)
{
	xtc_chan_mpsc_t *c;
	pthread_t prods[NP];
	struct mpsc_ctx ctxs[NP];
	int i, total = 0;
	int seen[NP] = {0};
	(void)p; (void)d;

	munit_assert_int(xtc_chan_mpsc_create(NULL, 64, &c), ==, XTC_OK);
	for (i = 0; i < NP; i++) {
		ctxs[i].c = c; ctxs[i].id = i;
		munit_assert_int(pthread_create(&prods[i], NULL,
		    mpsc_producer, &ctxs[i]), ==, 0);
	}
	while (total < NP * MSG_PER) {
		void *v;
		int rc = xtc_chan_mpsc_try_recv(c, &v);
		if (rc == XTC_E_AGAIN) { __os_thread_yield(); continue; }
		munit_assert_int(rc, ==, XTC_OK);
		{
			int prod = (int)((intptr_t)v >> 24);
			int seq  = (int)((intptr_t)v & 0xffffff);
			munit_assert_int(prod, >=, 0); munit_assert_int(prod, <, NP);
			munit_assert_int(seq, >=, 1);  munit_assert_int(seq, <=, MSG_PER);
			seen[prod]++;
		}
		total++;
	}
	for (i = 0; i < NP; i++) pthread_join(prods[i], NULL);
	for (i = 0; i < NP; i++) munit_assert_int(seen[i], ==, MSG_PER);
	xtc_chan_mpsc_destroy(c);
	return MUNIT_OK;
}

/* ----- watch ------------------------------------------------------ */

static MunitResult
test_watch_latest_wins(const MunitParameter p[], void *d)
{
	xtc_chan_watch_t *c;
	int a = 1, b = 2, ccc = 3;
	void *out = NULL;
	(void)p; (void)d;
	munit_assert_int(xtc_chan_watch_create(NULL, &c), ==, XTC_OK);
	munit_assert_int(xtc_chan_watch_recv(c, &out), ==, XTC_E_AGAIN);
	xtc_chan_watch_send(c, &a);
	xtc_chan_watch_send(c, &b);
	xtc_chan_watch_send(c, &ccc);
	munit_assert_int(xtc_chan_watch_recv(c, &out), ==, XTC_OK);
	munit_assert_ptr(out, ==, &ccc);
	xtc_chan_watch_destroy(c);
	return MUNIT_OK;
}

/* ----- resource governance --------------------------------------- */

static MunitResult
test_res_caps(const MunitParameter p[], void *d)
{
	xtc_res_t r;
	xtc_res_caps_t caps = XTC_RES_CAPS_DEFAULT;
	xtc_chan_mpsc_t *c1, *c2;
	(void)p; (void)d;
	caps.channels = 1;
	caps.chan_slots = 4;
	munit_assert_int(xtc_res_init(&r, &caps), ==, XTC_OK);

	munit_assert_int(xtc_chan_mpsc_create(&r, 8, &c1), ==, XTC_OK);
	/* Second channel rejected by cap. */
	munit_assert_int(xtc_chan_mpsc_create(&r, 8, &c2), ==, XTC_E_RESOURCE);
	munit_assert_int64(xtc_res_rejects(&r, XTC_RES_CHANNELS), ==, 1);

	/* Slot cap. */
	{
		intptr_t v = 1;
		munit_assert_int(xtc_chan_mpsc_try_send(c1, (void *)v), ==, XTC_OK);
		munit_assert_int(xtc_chan_mpsc_try_send(c1, (void *)v), ==, XTC_OK);
		munit_assert_int(xtc_chan_mpsc_try_send(c1, (void *)v), ==, XTC_OK);
		munit_assert_int(xtc_chan_mpsc_try_send(c1, (void *)v), ==, XTC_OK);
		/* 5th send hits the slot cap (4). */
		munit_assert_int(xtc_chan_mpsc_try_send(c1, (void *)v),
		    ==, XTC_E_RESOURCE);
		munit_assert_int64(xtc_res_rejects(&r, XTC_RES_CHAN_SLOTS),
		    ==, 1);
	}
	xtc_chan_mpsc_destroy(c1);
	munit_assert_int64(xtc_res_used(&r, XTC_RES_CHANNELS), ==, 0);
	munit_assert_int64(xtc_res_used(&r, XTC_RES_CHAN_SLOTS), ==, 0);
	return MUNIT_OK;
}

static MunitResult
test_demand_backpressure(const MunitParameter p[], void *d)
{
	xtc_chan_demand_t *c = NULL;
	int items[5] = { 10, 11, 12, 13, 14 };
	void *out = NULL;
	int i;
	(void)p; (void)d;

	munit_assert_int(xtc_chan_demand_create(NULL, 4, &c), ==, XTC_OK);

	/* No demand yet: a send is refused (backpressure). */
	munit_assert_int(xtc_chan_demand_send(c, &items[0]), ==, XTC_E_AGAIN);
	munit_assert_size(xtc_chan_demand_outstanding(c), ==, 0);

	/* Consumer asks for 2; producer may send exactly 2. */
	munit_assert_int(xtc_chan_demand_ask(c, 2), ==, XTC_OK);
	munit_assert_size(xtc_chan_demand_outstanding(c), ==, 2);
	munit_assert_int(xtc_chan_demand_send(c, &items[0]), ==, XTC_OK);
	munit_assert_int(xtc_chan_demand_send(c, &items[1]), ==, XTC_OK);
	/* Demand exhausted: the third send is refused. */
	munit_assert_int(xtc_chan_demand_send(c, &items[2]), ==, XTC_E_AGAIN);
	munit_assert_size(xtc_chan_demand_outstanding(c), ==, 0);
	munit_assert_size(xtc_chan_demand_len(c), ==, 2);

	/* Consumer drains -- recv does NOT itself grant demand. */
	munit_assert_int(xtc_chan_demand_try_recv(c, &out), ==, XTC_OK);
	munit_assert_ptr_equal(out, &items[0]);
	munit_assert_int(xtc_chan_demand_try_recv(c, &out), ==, XTC_OK);
	munit_assert_ptr_equal(out, &items[1]);
	munit_assert_int(xtc_chan_demand_try_recv(c, &out), ==, XTC_E_AGAIN);
	/* Still no demand, so still no send. */
	munit_assert_int(xtc_chan_demand_send(c, &items[2]), ==, XTC_E_AGAIN);

	/* Ask for more than the buffer holds: demand accrues but the
	 * buffer cap (4) still bounds in-flight items. */
	munit_assert_int(xtc_chan_demand_ask(c, 10), ==, XTC_OK);
	for (i = 0; i < 4; i++)
		munit_assert_int(xtc_chan_demand_send(c, &items[i]), ==, XTC_OK);
	/* Buffer full (4) even though demand remains (6). */
	munit_assert_int(xtc_chan_demand_send(c, &items[4]), ==, XTC_E_AGAIN);
	munit_assert_size(xtc_chan_demand_len(c), ==, 4);
	munit_assert_size(xtc_chan_demand_outstanding(c), ==, 6);

	/* Close: buffered items still drain, then XTC_E_INVAL. */
	munit_assert_int(xtc_chan_demand_close(c), ==, XTC_OK);
	munit_assert_int(xtc_chan_demand_send(c, &items[4]), ==, XTC_E_INVAL);
	for (i = 0; i < 4; i++)
		munit_assert_int(xtc_chan_demand_try_recv(c, &out), ==, XTC_OK);
	munit_assert_int(xtc_chan_demand_try_recv(c, &out), ==, XTC_E_INVAL);

	xtc_chan_demand_destroy(c);
	return MUNIT_OK;
}

/*
 * Demand-channel wakers.  Two fibers on one loop:
 *   - the CONSUMER parks after registering its consumer waker on an
 *     empty channel (deferred: no immediate wake), then asks for demand
 *     via its own waker so the producer can send; when the producer
 *     sends, the consumer's waker fires and it drains + finishes.
 *   - the PRODUCER parks after registering its producer waker on a
 *     channel with no demand; when the consumer asks, the producer
 *     waker fires and it sends, consuming the demand.
 * This drives the deferred branch of both set_*_waker (wake_now == 0)
 * and both wake paths (ask -> producer waker, send -> consumer waker).
 */
struct demand_pair {
	xtc_chan_demand_t *c;
	xtc_waker_t        prod_waker;
	xtc_waker_t        cons_waker;
	int                prod_runs;
	int                cons_runs;
	int                got;          /* item value the consumer received */
	int                item;
};

static int
demand_producer(xtc_task_t *self, void *u)
{
	struct demand_pair *dp = u;
	dp->prod_runs++;
	if (dp->prod_runs == 1) {
		/* No demand yet: register producer waker (deferred) and park. */
		(void)xtc_task_waker(self, &dp->prod_waker);
		munit_assert_int(
		    xtc_chan_demand_set_producer_waker(dp->c, &dp->prod_waker),
		    ==, XTC_OK);
		return XTC_TASK_PENDING;
	}
	/* Woken by the consumer's ask: demand is available, send one. */
	munit_assert_int(xtc_chan_demand_send(dp->c, &dp->item), ==, XTC_OK);
	return XTC_TASK_DONE;
}

static int
demand_consumer(xtc_task_t *self, void *u)
{
	struct demand_pair *dp = u;
	void *out = NULL;
	dp->cons_runs++;
	if (dp->cons_runs == 1) {
		/* Empty channel: register consumer waker (deferred), grant
		 * demand (wakes the parked producer), then park for the item. */
		(void)xtc_task_waker(self, &dp->cons_waker);
		munit_assert_int(
		    xtc_chan_demand_set_consumer_waker(dp->c, &dp->cons_waker),
		    ==, XTC_OK);
		munit_assert_int(xtc_chan_demand_ask(dp->c, 1), ==, XTC_OK);
		return XTC_TASK_PENDING;
	}
	/* Woken by the producer's send: drain it. */
	munit_assert_int(xtc_chan_demand_try_recv(dp->c, &out), ==, XTC_OK);
	dp->got = *(int *)out;
	return XTC_TASK_DONE;
}

static MunitResult
test_demand_wakers(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	struct demand_pair dp;
	xtc_chan_demand_t *c = NULL;
	(void)p; (void)d;

	/* NULL-arg guards on both setters. */
	munit_assert_int(xtc_chan_demand_create(NULL, 4, &c), ==, XTC_OK);
	munit_assert_int(xtc_chan_demand_set_producer_waker(NULL, NULL),
	    ==, XTC_E_INVAL);
	munit_assert_int(xtc_chan_demand_set_producer_waker(c, NULL),
	    ==, XTC_E_INVAL);
	munit_assert_int(xtc_chan_demand_set_consumer_waker(NULL, NULL),
	    ==, XTC_E_INVAL);
	munit_assert_int(xtc_chan_demand_set_consumer_waker(c, NULL),
	    ==, XTC_E_INVAL);
	xtc_chan_demand_destroy(c);

	memset(&dp, 0, sizeof dp);
	dp.item = 77;
	munit_assert_int(xtc_chan_demand_create(NULL, 4, &dp.c), ==, XTC_OK);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	/* Spawn producer first so it parks before the consumer asks. */
	munit_assert_int(xtc_task_spawn(loop, demand_producer, &dp, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_task_spawn(loop, demand_consumer, &dp, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	munit_assert_int(dp.prod_runs, ==, 2);   /* park + resume */
	munit_assert_int(dp.cons_runs, ==, 2);   /* park + resume */
	munit_assert_int(dp.got, ==, 77);

	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	xtc_chan_demand_destroy(dp.c);
	return MUNIT_OK;
}

/*
 * Immediate (wake_now) branch of both setters: registering a producer
 * waker while demand already exists, or a consumer waker while items are
 * already buffered, fires the waker synchronously inside the setter.
 * We exercise that branch with an inert waker ({NULL,NULL}, which
 * xtc_waker_wake treats as a harmless no-op) -- no loop needed; the
 * point is the wake_now path in the setter, and that the return is
 * still XTC_OK.
 */
static MunitResult
test_demand_waker_wake_now(const MunitParameter p[], void *d)
{
	xtc_chan_demand_t *c = NULL;
	xtc_waker_t inert = { NULL, NULL };
	int item = 5;
	(void)p; (void)d;

	/* Producer waker registered while demand > 0 -> wake_now branch. */
	munit_assert_int(xtc_chan_demand_create(NULL, 4, &c), ==, XTC_OK);
	munit_assert_int(xtc_chan_demand_ask(c, 2), ==, XTC_OK);
	munit_assert_int(xtc_chan_demand_set_producer_waker(c, &inert),
	    ==, XTC_OK);
	/* Consumer waker registered while an item is buffered -> wake_now. */
	munit_assert_int(xtc_chan_demand_send(c, &item), ==, XTC_OK);
	munit_assert_int(xtc_chan_demand_set_consumer_waker(c, &inert),
	    ==, XTC_OK);
	xtc_chan_demand_destroy(c);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/oneshot_basic",       test_oneshot_basic,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/mpsc_basic",          test_mpsc_basic,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/mpsc_set_waker",      test_mpsc_set_waker,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/mpsc_concurrent",     test_mpsc_concurrent,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/watch_latest_wins",   test_watch_latest_wins,NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/res_caps",            test_res_caps,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/demand_backpressure", test_demand_backpressure, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/demand_wakers",       test_demand_wakers,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/demand_waker_wake_now", test_demand_waker_wake_now, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m7/chan", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
