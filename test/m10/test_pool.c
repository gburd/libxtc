/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m10/test_pool.c
 *	R7 bounded resource pool: add/checkout/checkin accounting, at-cap
 *	add rejection, try-once timeout when empty, and a fiber that blocks
 *	on checkout until another fiber checks a resource back in.
 */

#include <stdint.h>
#include <string.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_async.h"
#include "xtc_pool.h"

/* ---------- accounting, no blocking ---------- */

static MunitResult
test_pool_accounting(const MunitParameter p[], void *d)
{
	xtc_pool_t *pool = NULL;
	int r0 = 10, r1 = 11, r2 = 12;
	void *a = NULL, *b = NULL, *c = NULL;
	(void)p; (void)d;

	munit_assert_int(xtc_pool_create(2, &pool), ==, XTC_OK);
	munit_assert_int(xtc_pool_add(pool, &r0), ==, XTC_OK);
	munit_assert_int(xtc_pool_add(pool, &r1), ==, XTC_OK);
	/* at capacity */
	munit_assert_int(xtc_pool_add(pool, &r2), ==, XTC_E_RESOURCE);

	munit_assert_size(xtc_pool_capacity(pool), ==, 2);
	munit_assert_size(xtc_pool_available(pool), ==, 2);

	/* Non-blocking checkout of both. */
	munit_assert_int(xtc_pool_checkout(pool, 0, &a), ==, XTC_OK);
	munit_assert_int(xtc_pool_checkout(pool, 0, &b), ==, XTC_OK);
	munit_assert_ptr_not_null(a);
	munit_assert_ptr_not_null(b);
	munit_assert_ptr_not_equal(a, b);
	munit_assert_size(xtc_pool_available(pool), ==, 0);

	/* Empty: try-once times out. */
	munit_assert_int(xtc_pool_checkout(pool, 0, &c), ==, XTC_E_AGAIN);
	munit_assert_ptr_null(c);

	/* Return one; a checkin of an unowned pointer fails. */
	munit_assert_int(xtc_pool_checkin(pool, a), ==, XTC_OK);
	munit_assert_int(xtc_pool_checkin(pool, &r2), ==, XTC_E_INVAL);
	munit_assert_size(xtc_pool_available(pool), ==, 1);

	munit_assert_int(xtc_pool_checkin(pool, b), ==, XTC_OK);
	munit_assert_size(xtc_pool_available(pool), ==, 2);

	xtc_pool_destroy(pool);
	return MUNIT_OK;
}

/* ---------- checkout blocks until a checkin ---------- */

struct block_ctx {
	xtc_pool_t *pool;
	void       *held;      /* the single resource, held by holder */
	int         got_after; /* waiter got a resource after the checkin */
};

/* Holder: checks out the only resource, then yields so the waiter runs
 * and blocks, then checks it back in. */
static void
holder_proc(void *arg)
{
	struct block_ctx *c = arg;
	munit_assert_int(xtc_pool_checkout(c->pool, 0, &c->held), ==, XTC_OK);
	/* Let the waiter run and park on its blocking checkout. */
	xtc_yield();
	xtc_yield();
	munit_assert_int(xtc_pool_checkin(c->pool, c->held), ==, XTC_OK);
}

/* Waiter: blocks on checkout (pool empty) until the holder checks in. */
static void
waiter_proc(void *arg)
{
	struct block_ctx *c = arg;
	void *r = NULL;
	if (xtc_pool_checkout(c->pool, 2LL * 1000 * 1000 * 1000, &r) == XTC_OK) {
		c->got_after = 1;
		(void)xtc_pool_checkin(c->pool, r);
	}
}

static MunitResult
test_pool_block_until_checkin(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_pool_t *pool = NULL;
	struct block_ctx c;
	int res = 99;
	(void)p; (void)d;

	memset(&c, 0, sizeof c);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_pool_create(1, &pool), ==, XTC_OK);
	munit_assert_int(xtc_pool_add(pool, &res), ==, XTC_OK);
	c.pool = pool;

	/* Spawn holder first so it checks out the sole resource before the
	 * waiter runs. */
	munit_assert_int(xtc_proc_spawn(loop, holder_proc, &c, NULL, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, waiter_proc, &c, NULL, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	munit_assert_int(c.got_after, ==, 1);   /* waiter unblocked on checkin */
	munit_assert_size(xtc_pool_available(pool), ==, 1);

	xtc_pool_destroy(pool);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/accounting", test_pool_accounting, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/block_until_checkin", test_pool_block_until_checkin, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m10.7/pool", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
