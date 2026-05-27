/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/m7/test_chan.c — verifies M7 channel APIs.
 */

#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_chan.h"
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
	/* Capacity is now 8 — next send must fail. */
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

/* mpsc N producers, 1 consumer — no message lost, no duplicates. */
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

static MunitTest tests[] = {
	{ "/oneshot_basic",       test_oneshot_basic,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/mpsc_basic",          test_mpsc_basic,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/mpsc_concurrent",     test_mpsc_concurrent,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/watch_latest_wins",   test_watch_latest_wins,NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/res_caps",            test_res_caps,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m7/chan", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
