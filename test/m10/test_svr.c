/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m10/test_svr.c -- verifies M10.5 gen_server (xtc_svr).
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_svr.h"
#include "xtc_int.h"

/* A trivial counter-server: cast to increment, call to read. */
struct counter_state {
	int count;
};

static int
counter_handle_call(void *st, const void *req, size_t size,
                    xtc_svr_call_t *call)
{
	struct counter_state *s = st;
	int v = s->count;
	(void)req; (void)size;
	(void)xtc_svr_reply(call, &v, sizeof v);
	return XTC_SVR_CONTINUE;
}

static int
counter_handle_cast(void *st, const void *msg, size_t size)
{
	struct counter_state *s = st;
	(void)msg; (void)size;
	s->count++;
	return XTC_SVR_CONTINUE;
}

static int g_info_seen;
static int
counter_handle_info(void *st, const void *msg, size_t size)
{
	(void)st; (void)msg; (void)size;
	__os_atomic_fetch_add_i32(&g_info_seen, 1);
	return XTC_SVR_CONTINUE;
}

/* Driver proc that exercises the server, then stops it. */
static xtc_svr_t *g_svr_for_driver;

struct driver_args {
	xtc_pid_t target;
	_Atomic int call_result;
};

static void
driver_proc(void *arg)
{
	struct driver_args *da = arg;
	xtc_pid_t target = da->target;
	void  *reply;
	size_t reply_size;
	int    rc;
	int    val;

	/* Cast a bunch. */
	(void)xtc_svr_cast(target, NULL, 0);
	(void)xtc_svr_cast(target, NULL, 0);
	(void)xtc_svr_cast(target, NULL, 0);

	/* Send a raw message -- should hit handle_info. */
	{ uint8_t raw[2] = { 'Z', 0 };
	  (void)xtc_send(target, raw, sizeof raw); }

	/* Yield once so the server has a chance to drain its mailbox. */
	{ void *m; size_t s;
	  (void)xtc_recv(&m, &s, 50 * 1000 * 1000);
	  if (m) __os_free(m); }

	/* Synchronous call. */
	rc = xtc_svr_call(target, NULL, 0, &reply, &reply_size,
	    1000LL * 1000 * 1000);
	if (rc == XTC_OK && reply_size == sizeof(int)) {
		memcpy(&val, reply, sizeof val);
		__os_free(reply);
	} else {
		val = -1;
	}
	atomic_store_explicit(&da->call_result, val, memory_order_release);

	(void)xtc_svr_stop(g_svr_for_driver);
}

static MunitResult
test_svr_basic(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_svr_t  *svr;
	xtc_svr_callbacks_t cb = {
		.init        = NULL,
		.handle_call = counter_handle_call,
		.handle_cast = counter_handle_cast,
		.handle_info = counter_handle_info,
		.terminate   = NULL
	};
	xtc_svr_opts_t opts = { .name = "counter", .mailbox_cap = 0 };
	struct counter_state state = {0};
	struct driver_args da;
	xtc_pid_t dpid;
	(void)p; (void)d;

	__os_atomic_store_i32(&g_info_seen, 0);

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_svr_start(loop, &cb, &state, &opts, &svr),
	    ==, XTC_OK);
	g_svr_for_driver = svr;

	da.target = xtc_svr_pid(svr);
	atomic_store_explicit(&da.call_result, -2, memory_order_relaxed);
	munit_assert_int(xtc_proc_spawn(loop, driver_proc, &da, NULL,
	    &dpid), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	/* Three casts incremented the counter to 3.  The synchronous
	 * call read 3 back. */
	munit_assert_int(state.count, ==, 3);
	munit_assert_int(atomic_load_explicit(&da.call_result,
	    memory_order_acquire), ==, 3);
	/* The raw-send hit handle_info exactly once. */
	munit_assert_int(__os_atomic_load_i32(&g_info_seen), ==, 1);

	munit_assert_int(xtc_svr_join(svr, 1LL * 1000 * 1000 * 1000), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/svr_basic", test_svr_basic, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m10.5/svr", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
