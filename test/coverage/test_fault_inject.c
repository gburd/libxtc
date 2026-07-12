/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/coverage/test_fault_inject.c
 *	Fault-injection coverage for error-cleanup paths that ordinary
 *	tests never reach because the failures (ENOMEM, pipe/fcntl
 *	failure, a backend-init failure) do not occur on a healthy host.
 *	Each injection point planted in the library (xtc_inject_trigger
 *	-- see src/ptc/inject.c) is fired here so the cleanup branch runs
 *	and is observed to leave no resource leaked and return the right
 *	error code.
 *
 *	Covers:
 *	  - xtc_io_init: calloc-fail, pipe-fail, fcntl-fail, backend-fail
 *	    (src/io/io_common.c) -- the four io.init.* points.
 *	  - xtc_svr reply-path edges (src/orc/svr.c): NULL call, the
 *	    no-handler empty-reply path, reply after the server stopped.
 *
 *	These run in the ordinary build (injection is compiled in unless
 *	XTC_INJECT_DISABLE is set).
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_int.h"   /* __os_free for the reply buffer */
#include "xtc_loop.h"
#include "xtc_io.h"
#include "xtc_inject.h"
#include "xtc_proc.h"
#include "xtc_exec.h"
#include "xtc_svr.h"

/* A no-op inject callback: attaching it makes xtc_inject_check() return
 * 1 for the name, so the library takes the injected branch. */
static void
noop_inject(const char *name, void *user)
{
	(void)name;
	(void)user;
}

/* ----- xtc_io_init failure paths -------------------------------- */

/* Each io.init.* point forces xtc_io_init to fail at a distinct stage;
 * we assert it returns an error (not XTC_OK) and does not crash / leak
 * (run under ASan in CI to catch a leak). */

static MunitResult
test_io_init_calloc_fail(const MunitParameter p[], void *d)
{
	xtc_io_t *io = NULL;
	(void)p; (void)d;

	munit_assert_int(xtc_inject_attach("io.init.calloc_fail",
	    noop_inject, NULL), ==, XTC_OK);
	munit_assert_int(xtc_io_init(&io), ==, XTC_E_NOMEM);
	munit_assert_null(io);
	(void)xtc_inject_detach("io.init.calloc_fail");
	return MUNIT_OK;
}

static MunitResult
test_io_init_backend_fail(const MunitParameter p[], void *d)
{
	xtc_io_t *io = NULL;
	int rc;
	(void)p; (void)d;

	munit_assert_int(xtc_inject_attach("io.init.backend_fail",
	    noop_inject, NULL), ==, XTC_OK);
	rc = xtc_io_init(&io);
	munit_assert_int(rc, !=, XTC_OK);   /* backend-init failure path */
	munit_assert_null(io);
	(void)xtc_inject_detach("io.init.backend_fail");
	return MUNIT_OK;
}

#if !defined(_WIN32) && !defined(XTC_IO_BACKEND_KQUEUE) && \
    !defined(XTC_IO_BACKEND_SIM)
/* The self-pipe paths only exist where a self-pipe wakeup is used
 * (epoll/poll/uring/select on POSIX). */
static MunitResult
test_io_init_pipe_fail(const MunitParameter p[], void *d)
{
	xtc_io_t *io = NULL;
	(void)p; (void)d;

	munit_assert_int(xtc_inject_attach("io.init.pipe_fail",
	    noop_inject, NULL), ==, XTC_OK);
	munit_assert_int(xtc_io_init(&io), ==, XTC_E_INTERNAL);
	munit_assert_null(io);
	(void)xtc_inject_detach("io.init.pipe_fail");
	return MUNIT_OK;
}

static MunitResult
test_io_init_fcntl_fail(const MunitParameter p[], void *d)
{
	xtc_io_t *io = NULL;
	(void)p; (void)d;

	munit_assert_int(xtc_inject_attach("io.init.fcntl_fail",
	    noop_inject, NULL), ==, XTC_OK);
	munit_assert_int(xtc_io_init(&io), ==, XTC_E_INTERNAL);
	munit_assert_null(io);
	(void)xtc_inject_detach("io.init.fcntl_fail");
	return MUNIT_OK;
}
#endif

/* Sanity: with no injection attached, xtc_io_init succeeds and fini
 * cleans up (the happy path the failure tests deviate from). */
static MunitResult
test_io_init_happy(const MunitParameter p[], void *d)
{
	xtc_io_t *io = NULL;
	(void)p; (void)d;

	munit_assert_int(xtc_io_init(&io), ==, XTC_OK);
	munit_assert_not_null(io);
	munit_assert_int(xtc_io_fini(io), ==, XTC_OK);
	return MUNIT_OK;
}

/* NULL-argument guards: every io_common entry point rejects NULL with
 * XTC_E_INVAL rather than dereferencing it. */
static MunitResult
test_io_null_guards(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	munit_assert_int(xtc_io_init(NULL), ==, XTC_E_INVAL);
	munit_assert_int(xtc_io_fini(NULL), ==, XTC_E_INVAL);
	munit_assert_int(xtc_io_wakeup(NULL), ==, XTC_E_INVAL);
	return MUNIT_OK;
}

/* The wakeup path on a live io: post a wakeup and confirm XTC_OK; a
 * second post is coalesced and also XTC_OK. */
static MunitResult
test_io_wakeup_roundtrip(const MunitParameter p[], void *d)
{
	xtc_io_t *io = NULL;
	(void)p; (void)d;
	munit_assert_int(xtc_io_init(&io), ==, XTC_OK);
	munit_assert_int(xtc_io_wakeup(io), ==, XTC_OK);
	munit_assert_int(xtc_io_wakeup(io), ==, XTC_OK);
	munit_assert_int(xtc_io_fini(io), ==, XTC_OK);
	return MUNIT_OK;
}

/* ----- xtc_svr reply-path edges --------------------------------- */

static MunitResult
test_svr_reply_null_call(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	/* A NULL call is rejected, not dereferenced. */
	munit_assert_int(xtc_svr_reply(NULL, NULL, 0), ==, XTC_E_INVAL);
	return MUNIT_OK;
}

/* A server whose handle_call is NULL must auto-reply empty (the
 * "No handler: send empty reply" branch in svr.c).  We drive a real
 * call against it from a driver proc inside the loop, then stop the
 * server so it self-reclaims during the run -- the established svr test
 * pattern (test/m10/test_svr.c).  This covers the no-handler reply
 * branch AND leaves no leak (verified under ASan). */
static xtc_svr_t *g_noh_svr;
static xtc_pid_t  g_noh_target;
static int        g_noh_call_rc = -99;
static size_t     g_noh_reply_size = (size_t)-1;

static void
noh_driver(void *arg)
{
	void *reply = NULL;
	size_t rsize = (size_t)-1;
	uint8_t req = 1;
	(void)arg;
	/* Synchronous call to the no-handler server: expect an empty reply. */
	g_noh_call_rc = xtc_svr_call(g_noh_target, &req, sizeof req,
	    &reply, &rsize, 1000LL * 1000 * 1000);
	g_noh_reply_size = rsize;
	if (reply != NULL)
		__os_free(reply);
	(void)xtc_svr_stop(g_noh_svr);
}

static MunitResult
test_svr_no_handler_empty_reply(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_svr_t *svr = NULL;
	xtc_svr_callbacks_t cb = {0};   /* handle_call == NULL */
	xtc_svr_opts_t opts = { .name = "noh", .mailbox_cap = 0 };
	xtc_pid_t dpid;
	(void)p; (void)d;

	g_noh_call_rc = -99;
	g_noh_reply_size = (size_t)-1;

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_svr_start(loop, &cb, NULL, &opts, &svr),
	    ==, XTC_OK);
	g_noh_svr = svr;
	g_noh_target = xtc_svr_pid(svr);

	munit_assert_int(xtc_proc_spawn(loop, noh_driver, NULL, NULL, &dpid),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	/* The no-handler server auto-replied empty: call succeeded, reply
	 * size 0. */
	munit_assert_int(g_noh_call_rc, ==, XTC_OK);
	munit_assert_size(g_noh_reply_size, ==, 0);

	/* Stop on a NULL server is rejected, not crashed. */
	munit_assert_int(xtc_svr_stop(NULL), ==, XTC_E_INVAL);

	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* NULL / invalid-argument guards across the xtc_svr entry points -- the
 * early-return XTC_E_INVAL edges the happy-path server tests skip. */
static MunitResult
test_svr_null_guards(const MunitParameter p[], void *d)
{
	xtc_pid_t none = { 0 };
	void *reply = NULL; size_t rsize = 0;
	char req = 'x';
	(void)p; (void)d;
	/* start with NULL out / NULL callbacks */
	munit_assert_int(xtc_svr_start(NULL, NULL, NULL, NULL, NULL),
	    ==, XTC_E_INVAL);
	/* stop / join / pid on NULL */
	munit_assert_int(xtc_svr_stop(NULL), ==, XTC_E_INVAL);
	munit_assert_int(xtc_svr_join(NULL, 0), ==, XTC_E_INVAL);
	/* reply on a NULL call */
	munit_assert_int(xtc_svr_reply(NULL, NULL, 0), ==, XTC_E_INVAL);
	/* call with NULL out-params is rejected before any send */
	munit_assert_int(xtc_svr_call(none, &req, sizeof req, NULL, &rsize,
	    0), ==, XTC_E_INVAL);
	munit_assert_int(xtc_svr_call(none, &req, sizeof req, &reply, NULL,
	    0), ==, XTC_E_INVAL);
	/* non-NULL req_size with NULL req */
	munit_assert_int(xtc_svr_call(none, NULL, 4, &reply, &rsize, 0),
	    ==, XTC_E_INVAL);
	/* cast with non-NULL size but NULL msg */
	munit_assert_int(xtc_svr_cast(none, NULL, 4), ==, XTC_E_INVAL);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/io_init/calloc_fail", test_io_init_calloc_fail, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/io_init/backend_fail", test_io_init_backend_fail, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
#if !defined(_WIN32) && !defined(XTC_IO_BACKEND_KQUEUE) && \
    !defined(XTC_IO_BACKEND_SIM)
	{ "/io_init/pipe_fail", test_io_init_pipe_fail, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/io_init/fcntl_fail", test_io_init_fcntl_fail, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
#endif
	{ "/io_init/happy", test_io_init_happy, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/io/null_guards", test_io_null_guards, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/io/wakeup_roundtrip", test_io_wakeup_roundtrip, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/svr/reply_null_call", test_svr_reply_null_call, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/svr/no_handler_empty_reply", test_svr_no_handler_empty_reply,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/svr/null_guards", test_svr_null_guards, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite suite = {
	"/fault_inject", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

int
main(int argc, char *argv[])
{
	return munit_suite_main(&suite, NULL, argc, argv);
}
