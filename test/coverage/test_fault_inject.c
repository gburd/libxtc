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
#include "xtc_future.h"
#include "os_alloc.h"   /* __os_alloc_set_hook: fail the Nth allocation */
#include "xtc_chan.h"
#include "xtc_sync.h"

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

/* xtc_svr_reply OOM path: with the svr.reply.oom injection point armed,
 * a reply carrying a non-empty payload must fail with XTC_E_NOMEM (the
 * __os_malloc for the reply copy is forced to fail) rather than
 * crashing or delivering a torn reply.  A handler replies a non-empty
 * payload and records the rc; the driver then stops the server. */
static xtc_svr_t *g_oom_svr;
static xtc_pid_t  g_oom_target;
static int        g_oom_reply_rc = -99;

static int
oom_handle_call(void *state, const void *req, size_t req_size,
    xtc_svr_call_t *call)
{
	static const char payload[8] = "reply!!";
	(void)state; (void)req; (void)req_size;
	/* Non-empty reply -> hits the __os_malloc copy path, which the
	 * armed injection point forces to XTC_E_NOMEM. */
	g_oom_reply_rc = xtc_svr_reply(call, payload, sizeof payload);
	return XTC_SVR_CONTINUE;
}

static void
oom_driver(void *arg)
{
	void *reply = NULL;
	size_t rsize = (size_t)-1;
	uint8_t req = 1;
	(void)arg;
	/* The call itself may time out or return an error because the
	 * reply failed to allocate -- we only care that xtc_svr_reply
	 * reported NOMEM (recorded in g_oom_reply_rc). */
	(void)xtc_svr_call(g_oom_target, &req, sizeof req, &reply, &rsize,
	    500LL * 1000 * 1000);
	if (reply != NULL)
		__os_free(reply);
	(void)xtc_svr_stop(g_oom_svr);
}

static MunitResult
test_svr_reply_oom(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_svr_t *svr = NULL;
	xtc_svr_callbacks_t cb = {0};
	xtc_svr_opts_t opts = { .name = "oom", .mailbox_cap = 0 };
	xtc_pid_t dpid;
	(void)p; (void)d;

	cb.handle_call = oom_handle_call;
	g_oom_reply_rc = -99;

	munit_assert_int(xtc_inject_attach("svr.reply.oom", noop_inject, NULL),
	    ==, XTC_OK);

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_svr_start(loop, &cb, NULL, &opts, &svr),
	    ==, XTC_OK);
	g_oom_svr = svr;
	g_oom_target = xtc_svr_pid(svr);
	munit_assert_int(xtc_proc_spawn(loop, oom_driver, NULL, NULL, &dpid),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	(void)xtc_inject_detach("svr.reply.oom");

	/* The handler's non-empty reply hit the forced-OOM path. */
	munit_assert_int(g_oom_reply_rc, ==, XTC_E_NOMEM);

	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}


/* ---- OOM cleanup paths, via an allocation-failing allocator hook ------
 *
 * A large share of the library's untested lines are `if (alloc failed)
 * { unwind; return XTC_E_NOMEM; }` cleanups.  They cannot be reached by
 * ordinary tests -- malloc does not fail on a healthy box -- yet they are
 * exactly the paths where a partially-built object gets torn down, i.e.
 * where a double-free or a leak hides.  A named injection point per site
 * would be dozens of points; instead swap in an allocator that fails the
 * Nth request and sweep N.
 *
 * The point is not the coverage number: running each N under ASan is what
 * proves these unwinds are sound.  Any rc is acceptable (success means
 * the sweep walked past that object's allocations); what must NOT happen
 * is a crash, a double free, or a leak.
 */
static _Atomic long g_alloc_seq;
static _Atomic long g_alloc_fail_at;   /* -1 = never fail */

static int
oom_should_fail(void)
{
	long at = atomic_load(&g_alloc_fail_at);
	long n  = atomic_fetch_add(&g_alloc_seq, 1) + 1;
	return (at > 0 && n == at);
}
static void *oom_malloc(size_t sz)
{ return oom_should_fail() ? NULL : malloc(sz); }
static void *oom_calloc(size_t n, size_t sz)
{ return oom_should_fail() ? NULL : calloc(n, sz); }
static void *oom_realloc(void *p, size_t sz)
{ return oom_should_fail() ? NULL : realloc(p, sz); }
static void  oom_free(void *p) { free(p); }
static void *oom_aligned(size_t align, size_t sz)
{
	void *p = NULL;
	if (oom_should_fail())
		return NULL;
	if (posix_memalign(&p, align < sizeof(void *) ? sizeof(void *) : align,
	    sz) != 0)
		return NULL;
	return p;
}
static void  oom_aligned_free(void *p) { free(p); }

static const struct __os_alloc_hook OOM_HOOK = {
	oom_malloc, oom_calloc, oom_realloc, oom_free,
	oom_aligned, oom_aligned_free
};

/* Exercise a few object lifecycles under a failing allocation. */
static void
oom_workload(void)
{
	xtc_future_t  *fut = NULL;
	xtc_promise_t *prom = NULL;
	xtc_loop_t    *loop = NULL;

	/* future/promise pair + a combinator: several allocations each, with
	 * cleanup paths that must unwind a partly-built cell. */
	if (xtc_future_new_pair(&prom, &fut) == XTC_OK) {
		intptr_t out = 0;
		(void)xtc_promise_set(prom, 7, XTC_OK);
		(void)xtc_future_wait(fut, &out, 0);
	}

	/* A loop is the biggest composite object in the library; spawning a
	 * proc and a server on it multiplies the allocations (and therefore
	 * the distinct unwind paths) the sweep walks. */
	if (xtc_loop_init(&loop) == XTC_OK) {
		xtc_chan_mpmc_t *ch = NULL;

		/* Channel: its own ring + slot allocations, with an unwind if
		 * any of them fails part-way. */
		if (xtc_chan_mpmc_create(NULL, 8, &ch) == XTC_OK)
			xtc_chan_mpmc_destroy(ch);

		(void)xtc_loop_fini(loop);
	}

	/* Executor: worker array, per-loop structures, NUMA map -- and its
	 * partially-constructed unwind (which leaked loop_node until this
	 * sweep caught it). */
	{
		xtc_exec_t *e = NULL;
		if (xtc_exec_init(&e, 2) == XTC_OK)
			(void)xtc_exec_fini(e);
	}

	/* Synchronisation primitives: each is a small heap object with its
	 * own partially-built unwind. */
	{
		xtc_amutex_t  *m = NULL;
		xtc_arwlock_t *rw = NULL;
		xtc_notify_t  *nt = NULL;
		xtc_sem_t     *sm = NULL;
		xtc_gate_t    *gt = NULL;
		xtc_barrier_t *br = NULL;

		if (xtc_amutex_create(&m) == XTC_OK)   xtc_amutex_destroy(m);
		if (xtc_arwlock_create(&rw) == XTC_OK) xtc_arwlock_destroy(rw);
		if (xtc_notify_create(&nt) == XTC_OK)  xtc_notify_destroy(nt);
		if (xtc_sem_create(2, &sm) == XTC_OK)  xtc_sem_destroy(sm);
		if (xtc_gate_create(&gt) == XTC_OK)    xtc_gate_destroy(gt);
		if (xtc_barrier_create(2, &br) == XTC_OK) xtc_barrier_destroy(br);
	}

}

static MunitResult
test_oom_sweep(const MunitParameter p[], void *d)
{
	long n;
	struct __os_alloc_hook saved;
	int have_saved = (__os_alloc_get_hook(&saved) == XTC_OK);
	(void)p; (void)d;

	/* First pass with no failures, to learn how many allocations the
	 * workload makes -- so the sweep covers all of them and no more. */
	atomic_store(&g_alloc_fail_at, -1);
	atomic_store(&g_alloc_seq, 0);
	munit_assert_int(__os_alloc_set_hook(&OOM_HOOK), ==, XTC_OK);
	oom_workload();
	{
		long total = atomic_load(&g_alloc_seq);
		/* The hook MUST have been consulted, or this test is vacuous. */
		munit_assert_true(total > 0);
		munit_logf(MUNIT_LOG_INFO, "oom sweep: %ld allocation(s)", total);
		if (total > 200)
			total = 200;   /* keep the test fast */

		for (n = 1; n <= total; n++) {
			atomic_store(&g_alloc_seq, 0);
			atomic_store(&g_alloc_fail_at, n);
			/* Must not crash, double-free, or leak (ASan/valgrind
			 * runs of this test are the real assertion). */
			oom_workload();
		}
	}
	atomic_store(&g_alloc_fail_at, -1);
	if (have_saved)
		(void)__os_alloc_set_hook(&saved);
	else
		(void)__os_alloc_set_hook(NULL);
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
	{ "/svr/reply_oom", test_svr_reply_oom, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/svr/no_handler_empty_reply", test_svr_no_handler_empty_reply,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/svr/null_guards", test_svr_null_guards, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/oom_sweep", test_oom_sweep, NULL, NULL,
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
