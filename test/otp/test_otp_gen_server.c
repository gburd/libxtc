/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/otp/test_otp_gen_server.c -- port of selected gen_server_SUITE
 * test cases from `lib/stdlib/test/gen_server_SUITE.erl`.
 *
 * gen_server in OTP is the canonical request/reply server pattern.
 * xtc maps it to xtc_svr.  The behavior contract:
 *   init/1            -> svr_callbacks_t.init
 *   handle_call/3     -> handle_call(state, req, size, call_handle)
 *   handle_cast/2     -> handle_cast
 *   handle_info/2     -> handle_info  (any non-call non-cast message)
 *   terminate/2       -> terminate(state, reason)
 *   code_change/3     -> not modeled (xtc has no hot-code-load)
 *   format_status/2   -> not modeled (no introspection)
 *
 * Tests omitted (BEAM-only):
 *   - hibernate (no heap to suspend)
 *   - global registration / multi_call across nodes
 *   - undef_callbacks (Erlang's notion of "no-such-function" exception)
 *   - format_log_* (Logger integration)
 *   - error_format_status (introspection)
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_svr.h"
#include "xtc_int.h"

/* ----- A simple counter gen_server ----------------------------- */

struct counter_state {
	int count;
	int  init_called;
	int  terminated_with;
	_Atomic int n_casts;
	_Atomic int n_infos;
};

static int
counter_init(void *st)
{
	struct counter_state *s = st;
	s->init_called = 1;
	s->count = 0;
	return XTC_OK;
}

/* Request layout: 1 byte op ('I'=increment, 'R'=read, 'A'=add). */
static int
counter_handle_call(void *st, const void *req, size_t sz,
                    xtc_svr_call_t *call)
{
	struct counter_state *s = st;
	const uint8_t *p = req;
	if (sz < 1) {
		(void)xtc_svr_reply(call, NULL, 0);
		return XTC_SVR_CONTINUE;
	}
	if (p[0] == 'R') {
		(void)xtc_svr_reply(call, &s->count, sizeof(int));
	} else if (p[0] == 'I') {
		s->count++;
		(void)xtc_svr_reply(call, &s->count, sizeof(int));
	} else if (p[0] == 'A' && sz >= 1 + sizeof(int)) {
		int n;
		memcpy(&n, p + 1, sizeof(int));
		s->count += n;
		(void)xtc_svr_reply(call, &s->count, sizeof(int));
	}
	return XTC_SVR_CONTINUE;
}

static int
counter_handle_cast(void *st, const void *msg, size_t sz)
{
	struct counter_state *s = st;
	(void)msg; (void)sz;
	atomic_fetch_add(&s->n_casts, 1);
	s->count++;
	return XTC_SVR_CONTINUE;
}

static int
counter_handle_info(void *st, const void *msg, size_t sz)
{
	struct counter_state *s = st;
	(void)msg; (void)sz;
	atomic_fetch_add(&s->n_infos, 1);
	return XTC_SVR_CONTINUE;
}

static void
counter_terminate(void *st, int reason)
{
	struct counter_state *s = st;
	s->terminated_with = reason;
}

static const xtc_svr_callbacks_t counter_cb = {
	.init        = counter_init,
	.handle_call = counter_handle_call,
	.handle_cast = counter_handle_cast,
	.handle_info = counter_handle_info,
	.terminate   = counter_terminate
};

/* ----- gen_server_SUITE: start ---------------------------------- */

static void
start_driver(void *arg)
{
	xtc_svr_t *svr = arg;
	(void)xtc_svr_stop(svr);
}

static MunitResult
test_start_stop(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_svr_t  *svr;
	struct counter_state state = {0};
	xtc_pid_t driver;
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_svr_start(loop, &counter_cb, &state, NULL, &svr),
	    ==, XTC_OK);
	/* init runs inside the server's proc -- only after loop starts. */
	munit_assert_int(xtc_proc_spawn(loop, start_driver, svr, NULL, &driver),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_true(state.init_called);
	munit_assert_int(xtc_svr_join(svr, 1000LL * 1000 * 1000), ==, XTC_OK);
	(void)xtc_loop_fini(loop);
	return MUNIT_OK;
}

/* ----- gen_server_SUITE: call / cast / info -------------------- */

struct call_driver_args {
	xtc_pid_t target;
	_Atomic int got_value;
	xtc_svr_t  *svr;
};

static void
call_cast_info_driver(void *arg)
{
	struct call_driver_args *da = arg;
	void  *reply;
	size_t reply_sz;
	uint8_t  read_op = 'R';
	uint8_t  inc_op  = 'I';
	uint8_t  add_buf[1 + sizeof(int)];
	int      add_n = 5;
	int      val = 0;

	/* Cast 3 times (each increments count). */
	(void)xtc_svr_cast(da->target, NULL, 0);
	(void)xtc_svr_cast(da->target, NULL, 0);
	(void)xtc_svr_cast(da->target, NULL, 0);

	/* Send an info message (raw send, no header). */
	{ uint8_t info[2] = {'Z', 0};
	  (void)xtc_send(da->target, info, sizeof info); }

	/* Yield so server drains. */
	{ void *m; size_t s; (void)xtc_recv(&m, &s, 50 * 1000 * 1000);
	  if (m) __os_free(m); }

	/* Increment via call. */
	if (xtc_svr_call(da->target, &inc_op, 1, &reply, &reply_sz,
	    1000LL * 1000 * 1000) == XTC_OK && reply_sz == sizeof(int)) {
		memcpy(&val, reply, sizeof val);
		__os_free(reply);
	}

	/* Add 5 via call. */
	add_buf[0] = 'A';
	memcpy(add_buf + 1, &add_n, sizeof add_n);
	if (xtc_svr_call(da->target, add_buf, sizeof add_buf,
	    &reply, &reply_sz, 1000LL * 1000 * 1000) == XTC_OK
	    && reply_sz == sizeof(int)) {
		memcpy(&val, reply, sizeof val);
		__os_free(reply);
	}

	/* Read final value. */
	if (xtc_svr_call(da->target, &read_op, 1, &reply, &reply_sz,
	    1000LL * 1000 * 1000) == XTC_OK && reply_sz == sizeof(int)) {
		memcpy(&val, reply, sizeof val);
		__os_free(reply);
	}
	atomic_store(&da->got_value, val);

	(void)xtc_svr_stop(da->svr);
}

static MunitResult
test_call_cast_info(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_svr_t  *svr;
	struct counter_state state = {0};
	struct call_driver_args da = {0};
	xtc_pid_t  dpid;
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_svr_start(loop, &counter_cb, &state, NULL, &svr),
	    ==, XTC_OK);
	da.target = xtc_svr_pid(svr);
	da.svr    = svr;
	munit_assert_int(xtc_proc_spawn(loop, call_cast_info_driver, &da, NULL,
	    &dpid), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	/* 3 casts (+3) + 1 inc call (+1) + 1 add 5 (+5) = 9. */
	munit_assert_int(atomic_load(&da.got_value), ==, 9);
	munit_assert_int(state.count, ==, 9);
	munit_assert_int(atomic_load(&state.n_casts), ==, 3);
	munit_assert_int(atomic_load(&state.n_infos), ==, 1);

	munit_assert_int(xtc_svr_join(svr, 1000LL * 1000 * 1000), ==, XTC_OK);
	(void)xtc_loop_fini(loop);
	return MUNIT_OK;
}

/* ----- gen_server_SUITE: handle_call returning STOP ------------- */

static int g_stop_init_count;
static int g_stop_terminated;

static int
stop_init(void *st) { (void)st; g_stop_init_count++; return XTC_OK; }

static int
stop_handle_call(void *st, const void *req, size_t sz, xtc_svr_call_t *call)
{
	(void)st; (void)req; (void)sz;
	(void)xtc_svr_reply(call, NULL, 0);
	return XTC_SVR_STOP;       /* request server termination */
}

static void
stop_terminate(void *st, int reason)
{
	(void)st; (void)reason;
	g_stop_terminated = 1;
}

static const xtc_svr_callbacks_t stop_cb = {
	.init = stop_init,
	.handle_call = stop_handle_call,
	.terminate = stop_terminate
};

static void
stop_driver(void *arg)
{
	xtc_pid_t target = *(xtc_pid_t *)arg;
	void *r; size_t rs;
	uint8_t req = 0;
	(void)xtc_svr_call(target, &req, 1, &r, &rs, 1000LL * 1000 * 1000);
	if (r) __os_free(r);
}

static MunitResult
test_stop_via_handle_call(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_svr_t  *svr;
	xtc_pid_t target, driver;
	int dummy_state = 0;
	(void)p; (void)d;
	g_stop_init_count = 0;
	g_stop_terminated = 0;

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_svr_start(loop, &stop_cb, &dummy_state, NULL, &svr),
	    ==, XTC_OK);
	target = xtc_svr_pid(svr);
	munit_assert_int(xtc_proc_spawn(loop, stop_driver, &target, NULL,
	    &driver), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(g_stop_init_count, ==, 1);
	munit_assert_int(g_stop_terminated, ==, 1);
	munit_assert_int(xtc_svr_join(svr, 1000LL * 1000 * 1000), ==, XTC_OK);
	(void)xtc_loop_fini(loop);
	return MUNIT_OK;
}

/* ----- gen_server_SUITE: call timeout -------------------------- */

static int
slow_handle_call(void *st, const void *req, size_t sz, xtc_svr_call_t *call)
{
	(void)st; (void)req; (void)sz;
	/* Don't reply -- caller times out. */
	(void)call;
	return XTC_SVR_CONTINUE;
}

static const xtc_svr_callbacks_t slow_cb = {
	.handle_call = slow_handle_call
};

static _Atomic int g_call_timeout_seen;

static void
timeout_call_driver(void *arg)
{
	xtc_pid_t target = *(xtc_pid_t *)arg;
	void *r = NULL; size_t rs = 0;
	int rc;
	uint8_t req = 0;
	rc = xtc_svr_call(target, &req, 1, &r, &rs, 30 * 1000 * 1000);
	if (rc == XTC_E_AGAIN) atomic_store(&g_call_timeout_seen, 1);
	if (r) __os_free(r);
}

static void
timeout_stopper(void *arg)
{
	xtc_svr_t *svr = arg;
	void *m; size_t sz;
	(void)xtc_recv(&m, &sz, 100 * 1000 * 1000);
	if (m) __os_free(m);
	(void)xtc_svr_stop(svr);
}

static MunitResult
test_call_timeout(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_svr_t  *svr;
	xtc_pid_t target, dpid, spid;
	int dummy = 0;
	(void)p; (void)d;
	atomic_store(&g_call_timeout_seen, 0);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_svr_start(loop, &slow_cb, &dummy, NULL, &svr),
	    ==, XTC_OK);
	target = xtc_svr_pid(svr);
	munit_assert_int(xtc_proc_spawn(loop, timeout_call_driver, &target,
	    NULL, &dpid), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, timeout_stopper, svr, NULL,
	    &spid), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(atomic_load(&g_call_timeout_seen), ==, 1);
	munit_assert_int(xtc_svr_join(svr, 1000LL * 1000 * 1000), ==, XTC_OK);
	(void)xtc_loop_fini(loop);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/start_stop",            test_start_stop,            NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/call_cast_info",        test_call_cast_info,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/stop_via_handle_call",  test_stop_via_handle_call,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/call_timeout",          test_call_timeout,          NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/otp/gen_server", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
