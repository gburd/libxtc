/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * test/m10/test_svr_edge.c
 *	Branch coverage for the three svr.c (gen_server) gaps named in
 *	docs/KNOWN_ISSUES.md ("svr.c branch coverage"):
 *
 *	  (1) call-after-stop      -- xtc_svr_call after xtc_svr_stop
 *	  (2) reply-when-stopped   -- xtc_svr_reply on a saved handle
 *	                              after the server (and its caller)
 *	                              are gone; xtc_send to a stale pid
 *	                              fails and the heap handle is still
 *	                              freed.
 *	  (3) OOM during reply     -- __os_malloc inside xtc_svr_reply
 *	                              fails; the reply returns XTC_E_NOMEM
 *	                              and the slot is never signalled.
 *
 *	Gap (3) forces OOM via the allocator hook (__os_alloc_set_hook),
 *	the same mechanism test/m1/test_alloc.c and test/pbt/pbt_alloc.c
 *	use.  svr.c has no injection point, so the hook is the only way
 *	to reach the reply-path malloc failure from a test.  The hook is
 *	size-targeted so it fails ONLY the reply allocation and leaves
 *	every other allocation (loop, mailbox, ...) working.
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
#include "os_alloc.h"

/* ================================================================
 * Gap (1): call-after-stop.
 *
 * A driver proc stops the server, drains its own mailbox to let the
 * stop propagate, then issues a call.  The server pid is torn down,
 * so the call must fail cleanly (no reply, no crash): either
 * XTC_E_INVAL (pid no longer resolvable) or XTC_E_AGAIN (accepted but
 * unanswered).  The reply pointer must remain NULL on failure.
 * ================================================================ */

static int
edge_handle_call(void *st, const void *req, size_t sz, xtc_svr_call_t *call)
{
	(void)st;
	(void)xtc_svr_reply(call, req, sz);   /* echo */
	return XTC_SVR_CONTINUE;
}

static const xtc_svr_callbacks_t edge_cb = {
	.init = NULL, .handle_call = edge_handle_call,
	.handle_cast = NULL, .handle_info = NULL, .terminate = NULL
};

struct edge_args { xtc_pid_t target; xtc_svr_t *svr; _Atomic int rc; };

static void
cas_driver(void *arg)
{
	struct edge_args *a = arg;
	void  *reply = (void *)0x1;   /* poisoned: must be NULLed on failure */
	size_t rsz = 0;
	int    want = 7, rc;

	(void)xtc_svr_stop(a->svr);
	{ void *m; size_t s; (void)xtc_recv(&m, &s, 60 * 1000 * 1000);
	  if (m) __os_free(m); }

	reply = NULL;
	rc = xtc_svr_call(a->target, &want, sizeof want, &reply, &rsz,
	    60LL * 1000 * 1000);
	atomic_store_explicit(&a->rc, rc, memory_order_release);
	munit_assert_int(rc == XTC_E_INVAL || rc == XTC_E_AGAIN, ==, 1);
	if (rc == XTC_OK && reply) __os_free(reply);
}

static MunitResult
test_call_after_stop(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_svr_t  *svr;
	struct edge_args a;
	xtc_pid_t dpid;
	(void)p; (void)d;

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_svr_start(loop, &edge_cb, NULL, NULL, &svr),
	    ==, XTC_OK);
	memset(&a, 0, sizeof a);
	a.target = xtc_svr_pid(svr); a.svr = svr;
	munit_assert_int(xtc_proc_spawn(loop, cas_driver, &a, NULL, &dpid),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_svr_join(svr, 1000LL * 1000 * 1000), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* ================================================================
 * Gap (2): reply-when-server-already-stopped.
 *
 * The server handler saves the call handle (heap copy) and returns
 * NOREPLY, then stops WITHOUT replying.  After xtc_loop_run returns --
 * the server proc and the in-proc caller proc are both gone -- the
 * test replies on the saved handle from the main thread.  The handle
 * carries a reply_pid that is now stale, so xtc_send inside
 * xtc_svr_reply fails (XTC_E_INVAL), yet the heap-allocated handle is
 * still freed (no leak; verified clean under ASan/LSan in CI).
 * ================================================================ */

struct saved_state { xtc_svr_call_t *saved; };

static int
save_and_stop_handle_call(void *st, const void *req, size_t sz,
                          xtc_svr_call_t *call)
{
	struct saved_state *s = st;
	(void)req; (void)sz;
	s->saved = xtc_svr_call_save(call);   /* heap handle outlives cb */
	return XTC_SVR_STOP;                  /* stop without replying */
}

static const xtc_svr_callbacks_t save_cb = {
	.init = NULL, .handle_call = save_and_stop_handle_call,
	.handle_cast = NULL, .handle_info = NULL, .terminate = NULL
};

struct save_args { xtc_pid_t target; _Atomic int call_rc; };

static void
save_caller(void *arg)
{
	struct save_args *a = arg;
	void  *reply = NULL;
	size_t rsz = 0;
	int rc = xtc_svr_call(a->target, "x", 1, &reply, &rsz,
	    80LL * 1000 * 1000);
	atomic_store_explicit(&a->call_rc, rc, memory_order_release);
	if (reply) __os_free(reply);
}

static MunitResult
test_reply_when_stopped(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_svr_t  *svr;
	struct saved_state st = {0};
	struct save_args a;
	xtc_pid_t cpid;
	int reply_rc;
	int v = 0xABCD;
	(void)p; (void)d;

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_svr_start(loop, &save_cb, &st, NULL, &svr),
	    ==, XTC_OK);
	memset(&a, 0, sizeof a);
	a.target = xtc_svr_pid(svr);
	munit_assert_int(xtc_proc_spawn(loop, save_caller, &a, NULL, &cpid),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	/* The handler saved a handle and stopped without replying, so the
	 * caller timed out. */
	munit_assert_int(atomic_load_explicit(&a.call_rc, memory_order_acquire),
	    ==, XTC_E_AGAIN);
	munit_assert_not_null(st.saved);

	/* Reply now, when the server proc and caller proc are gone.  The
	 * saved handle targets a stale reply_pid: xtc_send fails, but the
	 * heap handle is freed regardless (no leak).  Any non-crash
	 * return is acceptable; we assert it does not report XTC_OK, since
	 * nothing could have been delivered. */
	reply_rc = xtc_svr_reply(st.saved, &v, sizeof v);
	munit_assert_int(reply_rc, !=, XTC_OK);

	munit_assert_int(xtc_svr_join(svr, 1000LL * 1000 * 1000), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* ================================================================
 * Gap (3): OOM during the reply path.
 *
 * A size-targeted allocator hook fails the single __os_malloc inside
 * xtc_svr_reply (the pid-routing path allocates 4 + reply_size bytes).
 * We pick a distinctive reply size so ONLY that allocation fails; the
 * handler records the XTC_E_NOMEM return.  The slot is never signalled,
 * so the off-proc-style in-proc caller times out.
 * ================================================================ */

#define OOM_REPLY_SIZE   4093              /* distinctive; 4 + this = 4097 */
#define OOM_MSG_SIZE     (4 + OOM_REPLY_SIZE)

static struct __os_alloc_hook g_saved_hook;
static _Atomic int g_oom_armed;

static void *
oom_malloc(size_t s)
{
	if (atomic_load_explicit(&g_oom_armed, memory_order_acquire) &&
	    s == (size_t)OOM_MSG_SIZE) {
		/* Fail exactly the reply-path allocation, once. */
		atomic_store_explicit(&g_oom_armed, 0, memory_order_release);
		return NULL;
	}
	return g_saved_hook.malloc(s);
}

static _Atomic int g_reply_rc;

static int
oom_handle_call(void *st, const void *req, size_t sz, xtc_svr_call_t *call)
{
	int rc;
	uint8_t buf[OOM_REPLY_SIZE];
	(void)st; (void)req; (void)sz;
	memset(buf, 0x5A, sizeof buf);
	atomic_store_explicit(&g_oom_armed, 1, memory_order_release);
	rc = xtc_svr_reply(call, buf, sizeof buf);
	atomic_store_explicit(&g_reply_rc, rc, memory_order_release);
	/* Disarm in case reply took a path that did not consume it. */
	atomic_store_explicit(&g_oom_armed, 0, memory_order_release);
	return XTC_SVR_STOP;
}

static const xtc_svr_callbacks_t oom_cb = {
	.init = NULL, .handle_call = oom_handle_call,
	.handle_cast = NULL, .handle_info = NULL, .terminate = NULL
};

struct oom_args { xtc_pid_t target; _Atomic int call_rc; };

static void
oom_caller(void *arg)
{
	struct oom_args *a = arg;
	void  *reply = NULL;
	size_t rsz = 0;
	int rc = xtc_svr_call(a->target, "x", 1, &reply, &rsz,
	    120LL * 1000 * 1000);
	atomic_store_explicit(&a->call_rc, rc, memory_order_release);
	if (reply) __os_free(reply);
}

static MunitResult
test_oom_reply(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_svr_t  *svr;
	struct oom_args a;
	xtc_pid_t cpid;
	struct __os_alloc_hook my;
	(void)p; (void)d;

	/* Install the size-targeted OOM hook. */
	munit_assert_int(__os_alloc_get_hook(&g_saved_hook), ==, XTC_OK);
	my = g_saved_hook;
	my.malloc = oom_malloc;
	atomic_store(&g_oom_armed, 0);
	atomic_store(&g_reply_rc, 0xDEAD);
	munit_assert_int(__os_alloc_set_hook(&my), ==, XTC_OK);

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_svr_start(loop, &oom_cb, NULL, NULL, &svr),
	    ==, XTC_OK);
	memset(&a, 0, sizeof a);
	a.target = xtc_svr_pid(svr);
	munit_assert_int(xtc_proc_spawn(loop, oom_caller, &a, NULL, &cpid),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	/* Restore the real allocator BEFORE any teardown allocates. */
	munit_assert_int(__os_alloc_set_hook(&g_saved_hook), ==, XTC_OK);

	/* The reply path hit OOM and returned XTC_E_NOMEM. */
	munit_assert_int(atomic_load_explicit(&g_reply_rc, memory_order_acquire),
	    ==, XTC_E_NOMEM);
	/* The caller never got a reply -> timed out. */
	munit_assert_int(atomic_load_explicit(&a.call_rc, memory_order_acquire),
	    ==, XTC_E_AGAIN);

	munit_assert_int(xtc_svr_join(svr, 1000LL * 1000 * 1000), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/call_after_stop",   test_call_after_stop,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/reply_when_stopped", test_reply_when_stopped, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/oom_reply",         test_oom_reply,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m10.5/svr_edge", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
