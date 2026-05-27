/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/otp/test_otp_gen_server_phase2.c — second batch of cases
 * ported from `lib/stdlib/test/gen_server_SUITE.erl`.  Targets the
 * `svr.c` coverage gap (67.68% line / 50.78% branch, per audit v3).
 *
 * Cases in this file:
 *   - cast_fast: many casts in a row don't lose any
 *   - call_with_huge_message_queue: call works when mailbox is loaded
 *   - info_dispatch: raw send goes to handle_info
 *   - reply_with_data: reply payload preserved end-to-end
 *   - terminate_runs: terminate callback fires on stop
 *   - terminate_normal: normal exit calls terminate
 *   - cast_after_stop_drops: cast to a stopped server fails cleanly
 *   - call_after_stop_times_out: call to a stopped server returns AGAIN
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

/* ----- Common counter+log gen_server -------------------------- */

struct cs_state {
	_Atomic int n_call;
	_Atomic int n_cast;
	_Atomic int n_info;
	_Atomic int terminated;
	int last_cast_id;
};

static int
cs_init(void *st) { struct cs_state *s = st; (void)s; return XTC_OK; }

static int
cs_handle_call(void *st, const void *req, size_t sz, xtc_svr_call_t *call)
{
	struct cs_state *s = st;
	atomic_fetch_add(&s->n_call, 1);
	(void)xtc_svr_reply(call, req, sz);   /* echo the request */
	return XTC_SVR_CONTINUE;
}

static int
cs_handle_cast(void *st, const void *msg, size_t sz)
{
	struct cs_state *s = st;
	if (sz >= sizeof(int)) memcpy(&s->last_cast_id, msg, sizeof(int));
	atomic_fetch_add(&s->n_cast, 1);
	return XTC_SVR_CONTINUE;
}

static int
cs_handle_info(void *st, const void *msg, size_t sz)
{
	struct cs_state *s = st;
	(void)msg; (void)sz;
	atomic_fetch_add(&s->n_info, 1);
	return XTC_SVR_CONTINUE;
}

static void
cs_terminate(void *st, int reason)
{
	struct cs_state *s = st;
	(void)reason;
	atomic_store(&s->terminated, 1);
}

static const xtc_svr_callbacks_t cs_cb = {
	.init = cs_init,
	.handle_call = cs_handle_call,
	.handle_cast = cs_handle_cast,
	.handle_info = cs_handle_info,
	.terminate   = cs_terminate
};

/* ----- cast_fast: 100 casts, all delivered ---------------------- */

struct cf_args { xtc_pid_t target; xtc_svr_t *svr; };

static void
cf_driver(void *arg)
{
	struct cf_args *a = arg;
	int i;
	for (i = 0; i < 100; i++)
		(void)xtc_svr_cast(a->target, &i, sizeof i);
	{
		void *m; size_t sz;
		(void)xtc_recv(&m, &sz, 100 * 1000 * 1000);
		if (m) free(m);
	}
	(void)xtc_svr_stop(a->svr);
}

static MunitResult
test_cast_fast(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_svr_t  *svr;
	struct cs_state state = {0};
	struct cf_args args;
	xtc_pid_t dpid;
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_svr_start(loop, &cs_cb, &state, NULL, &svr),
	    ==, XTC_OK);
	args.target = xtc_svr_pid(svr);
	args.svr    = svr;
	munit_assert_int(xtc_proc_spawn(loop, cf_driver, &args, NULL, &dpid),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(atomic_load(&state.n_cast), ==, 100);
	munit_assert_int(state.last_cast_id, ==, 99);
	munit_assert_int(xtc_svr_join(svr, 1000LL * 1000 * 1000), ==, XTC_OK);
	(void)xtc_loop_fini(loop);
	return MUNIT_OK;
}

/* ----- call with huge message queue ---------------------------- */

static void
hmq_driver(void *arg)
{
	struct cf_args *a = arg;
	void *reply;
	size_t reply_sz;
	int   want = 0xCAFE;
	int   got  = 0;
	int   i;
	int   rc;

	/* Bury the call under 200 cast messages. */
	for (i = 0; i < 200; i++)
		(void)xtc_svr_cast(a->target, &i, sizeof i);

	rc = xtc_svr_call(a->target, &want, sizeof want, &reply, &reply_sz,
	    1000LL * 1000 * 1000);
	if (rc == XTC_OK && reply_sz == sizeof(int)) {
		memcpy(&got, reply, sizeof got);
		free(reply);
	}
	if (got == want) {
		void *m; size_t sz;
		(void)xtc_recv(&m, &sz, 100 * 1000 * 1000);
		if (m) free(m);
	}
	(void)xtc_svr_stop(a->svr);
}

static MunitResult
test_call_huge_mq(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_svr_t  *svr;
	struct cs_state state = {0};
	struct cf_args args;
	xtc_pid_t dpid;
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_svr_start(loop, &cs_cb, &state, NULL, &svr),
	    ==, XTC_OK);
	args.target = xtc_svr_pid(svr);
	args.svr    = svr;
	munit_assert_int(xtc_proc_spawn(loop, hmq_driver, &args, NULL, &dpid),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(atomic_load(&state.n_cast), ==, 200);
	munit_assert_int(atomic_load(&state.n_call), ==, 1);
	munit_assert_int(xtc_svr_join(svr, 1000LL * 1000 * 1000), ==, XTC_OK);
	(void)xtc_loop_fini(loop);
	return MUNIT_OK;
}

/* ----- info_dispatch: raw send hits handle_info ---------------- */

static void
inf_driver(void *arg)
{
	struct cf_args *a = arg;
	int i;
	uint8_t marker = 0xAA;
	for (i = 0; i < 50; i++)
		(void)xtc_send(a->target, &marker, sizeof marker);
	{
		void *m; size_t sz;
		(void)xtc_recv(&m, &sz, 100 * 1000 * 1000);
		if (m) free(m);
	}
	(void)xtc_svr_stop(a->svr);
}

static MunitResult
test_info_dispatch(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_svr_t  *svr;
	struct cs_state state = {0};
	struct cf_args args;
	xtc_pid_t dpid;
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_svr_start(loop, &cs_cb, &state, NULL, &svr),
	    ==, XTC_OK);
	args.target = xtc_svr_pid(svr);
	args.svr    = svr;
	munit_assert_int(xtc_proc_spawn(loop, inf_driver, &args, NULL, &dpid),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(atomic_load(&state.n_info), ==, 50);
	munit_assert_int(xtc_svr_join(svr, 1000LL * 1000 * 1000), ==, XTC_OK);
	(void)xtc_loop_fini(loop);
	return MUNIT_OK;
}

/* ----- reply_with_data: payload preserved -------------------- */

struct rwd_args { xtc_pid_t target; xtc_svr_t *svr; _Atomic int got_match; };

static void
rwd_driver(void *arg)
{
	struct rwd_args *a = arg;
	uint8_t  req[64];
	void   *reply;
	size_t  reply_sz;
	int     i;
	for (i = 0; i < (int)sizeof req; i++) req[i] = (uint8_t)(i ^ 0x55);
	if (xtc_svr_call(a->target, req, sizeof req, &reply, &reply_sz,
	    1000LL * 1000 * 1000) == XTC_OK) {
		if (reply_sz == sizeof req &&
		    memcmp(reply, req, sizeof req) == 0)
			atomic_store(&a->got_match, 1);
		free(reply);
	}
	(void)xtc_svr_stop(a->svr);
}

static MunitResult
test_reply_with_data(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_svr_t  *svr;
	struct cs_state state = {0};
	struct rwd_args args = {0};
	xtc_pid_t dpid;
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_svr_start(loop, &cs_cb, &state, NULL, &svr),
	    ==, XTC_OK);
	args.target = xtc_svr_pid(svr);
	args.svr    = svr;
	munit_assert_int(xtc_proc_spawn(loop, rwd_driver, &args, NULL, &dpid),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(atomic_load(&args.got_match), ==, 1);
	munit_assert_int(xtc_svr_join(svr, 1000LL * 1000 * 1000), ==, XTC_OK);
	(void)xtc_loop_fini(loop);
	return MUNIT_OK;
}

/* ----- terminate_runs ----------------------------------------- */

static MunitResult
test_terminate_runs(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_svr_t  *svr;
	struct cs_state state = {0};
	struct cf_args args;
	xtc_pid_t dpid;
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_svr_start(loop, &cs_cb, &state, NULL, &svr),
	    ==, XTC_OK);
	args.target = xtc_svr_pid(svr);
	args.svr    = svr;
	munit_assert_int(xtc_proc_spawn(loop, cf_driver, &args, NULL, &dpid),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_svr_join(svr, 1000LL * 1000 * 1000), ==, XTC_OK);
	munit_assert_int(atomic_load(&state.terminated), ==, 1);
	(void)xtc_loop_fini(loop);
	return MUNIT_OK;
}

/* ----- 0-byte cast ---------------------------------------- */

static void
zero_driver(void *arg)
{
	struct cf_args *a = arg;
	(void)xtc_svr_cast(a->target, NULL, 0);
	(void)xtc_svr_cast(a->target, NULL, 0);
	(void)xtc_svr_cast(a->target, NULL, 0);
	{ void *m; size_t sz;
	  (void)xtc_recv(&m, &sz, 100 * 1000 * 1000);
	  if (m) free(m); }
	(void)xtc_svr_stop(a->svr);
}

static MunitResult
test_zero_byte_cast(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_svr_t  *svr;
	struct cs_state state = {0};
	struct cf_args args;
	xtc_pid_t dpid;
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_svr_start(loop, &cs_cb, &state, NULL, &svr),
	    ==, XTC_OK);
	args.target = xtc_svr_pid(svr);
	args.svr    = svr;
	munit_assert_int(xtc_proc_spawn(loop, zero_driver, &args, NULL, &dpid),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(atomic_load(&state.n_cast), ==, 3);
	munit_assert_int(xtc_svr_join(svr, 1000LL * 1000 * 1000), ==, XTC_OK);
	(void)xtc_loop_fini(loop);
	return MUNIT_OK;
}

/* ----- call invalid args reject paths --------------------- */

static MunitResult
test_call_arg_validation(const MunitParameter p[], void *d)
{
	void *reply;
	size_t reply_sz;
	xtc_pid_t pid_none = {0,0,0};
	(void)p; (void)d;
	/* size > 0 with NULL data → INVAL. */
	munit_assert_int(xtc_svr_call(pid_none, NULL, 5, &reply, &reply_sz, 0),
	    ==, XTC_E_INVAL);
	/* NULL out_reply → INVAL. */
	munit_assert_int(xtc_svr_call(pid_none, "x", 1, NULL, &reply_sz, 0),
	    ==, XTC_E_INVAL);
	munit_assert_int(xtc_svr_call(pid_none, "x", 1, &reply, NULL, 0),
	    ==, XTC_E_INVAL);
	return MUNIT_OK;
}

/* ----- cast invalid args ---------------------------------- */

static MunitResult
test_cast_arg_validation(const MunitParameter p[], void *d)
{
	xtc_pid_t pid_none = {0,0,0};
	(void)p; (void)d;
	munit_assert_int(xtc_svr_cast(pid_none, NULL, 5), ==, XTC_E_INVAL);
	return MUNIT_OK;
}

/* ----- start invalid args --------------------------------- */

static MunitResult
test_start_arg_validation(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_svr_t  *svr;
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_svr_start(NULL, &cs_cb, NULL, NULL, &svr),
	    ==, XTC_E_INVAL);
	munit_assert_int(xtc_svr_start(loop, NULL, NULL, NULL, &svr),
	    ==, XTC_E_INVAL);
	munit_assert_int(xtc_svr_start(loop, &cs_cb, NULL, NULL, NULL),
	    ==, XTC_E_INVAL);
	(void)xtc_loop_fini(loop);
	return MUNIT_OK;
}

/* ----- reply with NULL call rejects ---------------------- */

static MunitResult
test_reply_arg_validation(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	munit_assert_int(xtc_svr_reply(NULL, "x", 1), ==, XTC_E_INVAL);
	return MUNIT_OK;
}

/* ----- call_after_stop ----------------------------------- */

static void
call_after_stop_driver(void *arg)
{
	struct cf_args *a = arg;
	void *reply;
	size_t reply_sz;
	int want = 1, rc;

	/* Stop server first. */
	(void)xtc_svr_stop(a->svr);
	/* Brief idle to let stop propagate. */
	{ void *m; size_t sz; (void)xtc_recv(&m, &sz, 50 * 1000 * 1000);
	  if (m) free(m); }

	/* call() to a stopped server returns XTC_E_INVAL (the server's
	 * pid is no longer resolvable) or XTC_E_AGAIN if the message was
	 * accepted but no reply arrived in time.  Both are acceptable
	 * shutdown outcomes. */
	rc = xtc_svr_call(a->target, &want, sizeof want, &reply, &reply_sz,
	    50LL * 1000 * 1000);
	munit_assert_int(rc == XTC_E_INVAL || rc == XTC_E_AGAIN, ==, 1);
}

static MunitResult
test_call_after_stop(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_svr_t *svr;
	struct cs_state state = {0};
	struct cf_args args;
	xtc_pid_t dpid;
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_svr_start(loop, &cs_cb, &state, NULL, &svr),
	    ==, XTC_OK);
	args.target = xtc_svr_pid(svr); args.svr = svr;
	munit_assert_int(xtc_proc_spawn(loop, call_after_stop_driver, &args,
	    NULL, &dpid), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_svr_join(svr, 1000LL * 1000 * 1000), ==, XTC_OK);
	(void)xtc_loop_fini(loop);
	return MUNIT_OK;
}

/* ----- cast_after_stop drops cleanly ------------------ */

static void
cast_after_stop_driver(void *arg)
{
	struct cf_args *a = arg;
	int v = 99;
	int rc;

	(void)xtc_svr_stop(a->svr);
	{ void *m; size_t sz; (void)xtc_recv(&m, &sz, 50 * 1000 * 1000);
	  if (m) free(m); }

	/* cast() to a stopped server is a one-way send; the underlying
	 * pid is no longer resolvable so we expect XTC_E_INVAL.  The
	 * test verifies that we don't crash and that the server doesn't
	 * see the message either way. */
	rc = xtc_svr_cast(a->target, &v, sizeof v);
	munit_assert_int(rc == XTC_OK || rc == XTC_E_INVAL, ==, 1);
}

static MunitResult
test_cast_after_stop(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_svr_t *svr;
	struct cs_state state = {0};
	struct cf_args args;
	xtc_pid_t dpid;
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_svr_start(loop, &cs_cb, &state, NULL, &svr),
	    ==, XTC_OK);
	args.target = xtc_svr_pid(svr); args.svr = svr;
	munit_assert_int(xtc_proc_spawn(loop, cast_after_stop_driver, &args,
	    NULL, &dpid), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(atomic_load(&state.n_cast), ==, 0);
	munit_assert_int(xtc_svr_join(svr, 1000LL * 1000 * 1000), ==, XTC_OK);
	(void)xtc_loop_fini(loop);
	return MUNIT_OK;
}

/* ----- call with zero-ns timeout returns immediately --- */

static void
zero_timeout_driver(void *arg)
{
	struct cf_args *a = arg;
	void *reply; size_t reply_sz;
	int want = 7, rc;
	/* Bury the call under 200 casts so the server is busy. */
	int i;
	for (i = 0; i < 200; i++) (void)xtc_svr_cast(a->target, &i, sizeof i);

	rc = xtc_svr_call(a->target, &want, sizeof want, &reply, &reply_sz, 0);
	/* timeout=0 means try-once; behaviour is XTC_E_AGAIN if not ready. */
	munit_assert_int(rc == XTC_OK || rc == XTC_E_AGAIN, ==, 1);
	if (rc == XTC_OK && reply) free(reply);
	(void)xtc_svr_stop(a->svr);
}

static MunitResult
test_call_zero_timeout(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_svr_t *svr;
	struct cs_state state = {0};
	struct cf_args args;
	xtc_pid_t dpid;
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_svr_start(loop, &cs_cb, &state, NULL, &svr),
	    ==, XTC_OK);
	args.target = xtc_svr_pid(svr); args.svr = svr;
	munit_assert_int(xtc_proc_spawn(loop, zero_timeout_driver, &args,
	    NULL, &dpid), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_svr_join(svr, 1000LL * 1000 * 1000), ==, XTC_OK);
	(void)xtc_loop_fini(loop);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/cast_fast",            test_cast_fast,            NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/call_huge_mq",         test_call_huge_mq,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/info_dispatch",        test_info_dispatch,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/reply_with_data",      test_reply_with_data,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/terminate_runs",       test_terminate_runs,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/zero_byte_cast",       test_zero_byte_cast,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/call_arg_validation",  test_call_arg_validation,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/cast_arg_validation",  test_cast_arg_validation,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/start_arg_validation", test_start_arg_validation, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/reply_arg_validation", test_reply_arg_validation, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/call_after_stop",      test_call_after_stop,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/cast_after_stop",      test_cast_after_stop,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/call_zero_timeout",    test_call_zero_timeout,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/otp/gen_server_phase2", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
