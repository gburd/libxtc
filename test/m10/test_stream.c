/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m10/test_stream.c
 *	R10 async streams: base next(), map, filter, for_each, and the
 *	demand-channel adapter (backpressured pull).
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_chan.h"
#include "xtc_stream.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_async.h"

/* ---------- base + map + filter + for_each ---------- */

/* A base stream over a fixed int array. */
struct arr_ctx { int *a; size_t n, i; };
static int
arr_next(void *c, void **out)
{
	struct arr_ctx *x = c;
	if (x->i >= x->n) return XTC_E_NOTFOUND;
	*out = &x->a[x->i++];
	return XTC_OK;
}

static int is_even(void *v, void *u) { (void)u; return (*(int *)v % 2) == 0; }
static void *times10(void *v, void *u) { (void)u; *(int *)v *= 10; return v; }

struct sink { int vals[16]; int n; };
static int collect(void *v, void *u)
{
	struct sink *s = u;
	s->vals[s->n++] = *(int *)v;
	return 0;
}

static MunitResult
test_stream_map_filter(const MunitParameter p[], void *d)
{
	int a[6] = { 1, 2, 3, 4, 5, 6 };
	struct arr_ctx ctx = { a, 6, 0 };
	struct sink out;
	xtc_stream_t *base = NULL, *f = NULL, *m = NULL;
	(void)p; (void)d;

	memset(&out, 0, sizeof out);
	munit_assert_int(xtc_stream_create(arr_next, &ctx, &base), ==, XTC_OK);
	munit_assert_int(xtc_stream_filter(base, is_even, NULL, &f), ==, XTC_OK);
	munit_assert_int(xtc_stream_map(f, times10, NULL, &m), ==, XTC_OK);

	/* evens {2,4,6} times 10 -> {20,40,60} */
	munit_assert_int(xtc_stream_for_each(m, collect, &out), ==, XTC_OK);
	munit_assert_int(out.n, ==, 3);
	munit_assert_int(out.vals[0], ==, 20);
	munit_assert_int(out.vals[1], ==, 40);
	munit_assert_int(out.vals[2], ==, 60);

	xtc_stream_destroy(m);   /* frees m -> f -> base */
	return MUNIT_OK;
}

/* early stop from for_each */
static int stop_at_second(void *v, void *u)
{
	struct sink *s = u;
	s->vals[s->n++] = *(int *)v;
	return (s->n >= 2) ? 42 : 0;
}

static MunitResult
test_stream_early_stop(const MunitParameter p[], void *d)
{
	int a[4] = { 7, 8, 9, 10 };
	struct arr_ctx ctx = { a, 4, 0 };
	struct sink out;
	xtc_stream_t *base = NULL;
	(void)p; (void)d;

	memset(&out, 0, sizeof out);
	munit_assert_int(xtc_stream_create(arr_next, &ctx, &base), ==, XTC_OK);
	munit_assert_int(xtc_stream_for_each(base, stop_at_second, &out), ==, 42);
	munit_assert_int(out.n, ==, 2);
	xtc_stream_destroy(base);
	return MUNIT_OK;
}

/* ---------- demand-channel adapter ---------- */

static MunitResult
test_stream_from_demand(const MunitParameter p[], void *d)
{
	xtc_chan_demand_t *ch = NULL;
	xtc_stream_t *s = NULL;
	int items[3] = { 100, 200, 300 };
	void *v = NULL;
	struct sink out;
	int i;
	(void)p; (void)d;

	munit_assert_int(xtc_chan_demand_create(NULL, 8, &ch), ==, XTC_OK);
	munit_assert_int(xtc_stream_from_demand(ch, &s), ==, XTC_OK);

	/* Nothing sent, no demand: pulling grants demand but finds nothing. */
	munit_assert_int(xtc_stream_next(s, &v), ==, XTC_E_AGAIN);

	/* The pull above granted 1 unit of demand; a producer can now send
	 * one.  Grant a couple more and fill the buffer. */
	munit_assert_int(xtc_chan_demand_ask(ch, 3), ==, XTC_OK);
	for (i = 0; i < 3; i++)
		munit_assert_int(xtc_chan_demand_send(ch, &items[i]), ==, XTC_OK);

	/* Now pulls succeed in order. */
	memset(&out, 0, sizeof out);
	munit_assert_int(xtc_stream_next(s, &v), ==, XTC_OK);
	munit_assert_int(*(int *)v, ==, 100);
	munit_assert_int(xtc_stream_next(s, &v), ==, XTC_OK);
	munit_assert_int(*(int *)v, ==, 200);
	munit_assert_int(xtc_stream_next(s, &v), ==, XTC_OK);
	munit_assert_int(*(int *)v, ==, 300);

	/* Empty but open: XTC_E_AGAIN.  After close+drain: XTC_E_NOTFOUND. */
	munit_assert_int(xtc_stream_next(s, &v), ==, XTC_E_AGAIN);
	munit_assert_int(xtc_chan_demand_close(ch), ==, XTC_OK);
	munit_assert_int(xtc_stream_next(s, &v), ==, XTC_E_NOTFOUND);

	xtc_stream_destroy(s);          /* does NOT free ch */
	xtc_chan_demand_destroy(ch);
	return MUNIT_OK;
}

/* for_each over a demand stream whose producer is a fiber on the SAME
 * loop.  The source returns XTC_E_AGAIN until the producer runs; 1.55.0
 * retried without yielding, so the producer never ran and the loop
 * livelocked (the test then dies on its timeout).  for_each now yields
 * between retries. */
struct fe_ctx {
	xtc_chan_demand_t *ch;
	xtc_stream_t      *s;
	int                items[4];
	int                sum;
	int                rc;
};
static int
fe_add(void *v, void *user)
{
	((struct fe_ctx *)user)->sum += *(int *)v;
	return 0;
}
static void
fe_consumer(void *arg)
{
	struct fe_ctx *c = arg;
	c->rc = xtc_stream_for_each(c->s, fe_add, c);
}
static void
fe_producer(void *arg)
{
	struct fe_ctx *c = arg;
	int i = 0;
	while (i < 4) {
		if (xtc_chan_demand_send(c->ch, &c->items[i]) == XTC_OK) i++;
		else xtc_yield();   /* no demand yet */
	}
	(void)xtc_chan_demand_close(c->ch);
}
/* Polls a done flag rather than being pthread_cancel'd: the MSVC
 * pthread shim (src/inc/compat/pthread.h) has no pthread_cancel. */
static _Atomic int fe_done;
static void *
fe_watchdog(void *arg)
{
	int i;
	(void)arg;
	for (i = 0; i < 500 && !atomic_load(&fe_done); i++)
		(void)xtc_sleep_ns(10LL * 1000 * 1000);
	if (!atomic_load(&fe_done))
		munit_error("xtc_stream_for_each livelocked the loop");
	return NULL;
}
static MunitResult
test_stream_for_each_same_loop_producer(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	struct fe_ctx c;
	pthread_t wd;
	int i;
	(void)p; (void)d;

	memset(&c, 0, sizeof c);
	for (i = 0; i < 4; i++) c.items[i] = i + 1;
	c.rc = -12345;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_chan_demand_create(NULL, 8, &c.ch), ==, XTC_OK);
	munit_assert_int(xtc_stream_from_demand(c.ch, &c.s), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, fe_consumer, &c, NULL, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, fe_producer, &c, NULL, NULL),
	    ==, XTC_OK);
	atomic_store(&fe_done, 0);
	munit_assert_int(pthread_create(&wd, NULL, fe_watchdog, NULL), ==, 0);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	atomic_store(&fe_done, 1);
	(void)pthread_join(wd, NULL);
	munit_assert_int(c.rc, ==, XTC_OK);
	munit_assert_int(c.sum, ==, 10);
	xtc_stream_destroy(c.s);
	xtc_chan_demand_destroy(c.ch);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/map_filter",  test_stream_map_filter,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/early_stop",  test_stream_early_stop,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/from_demand", test_stream_from_demand,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/for_each_same_loop_producer", test_stream_for_each_same_loop_producer, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m10.8/stream", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
