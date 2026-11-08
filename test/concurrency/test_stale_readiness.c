/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * test/concurrency/test_stale_readiness.c
 *	A stale readiness event must not steal a newer park on another loop.
 *
 *	A migratable fiber parks on fd F on loop A's io.  The readiness for
 *	F is reaped by A, but before A dispatches it the fiber is woken by
 *	another path, migrates to loop B, and parks on F again -- now on B's
 *	io.  A then dispatches the event it already reaped.  Through 2.1.1
 *	__xtc_loop_dispatch_event matched the park by fd NUMBER only: it
 *	cleared the task's park_fd (the claim on B's registration) although
 *	the event came from A's registry.  The fiber's cleanup on B then
 *	found -1 and skipped its unregister, B's registration was orphaned,
 *	and the next xtc_proc_wait_fd on F failed with XTC_E_INVAL as a
 *	duplicate (reported 2026-11-02 against the PostgreSQL xtc branch,
 *	reproduced 20/20 with an injection fixture on io_uring).
 *
 *	This test builds that exact state without a schedule race: a task
 *	whose park is recorded on loop B's io (park_io = B->io, park_fd = F),
 *	and the event A would have reaped (tag = task, fd = F), dispatched on
 *	A.  The fixed dispatch leaves the claim (park_fd unchanged) because
 *	the event's io is not the park's io; A's own registration of F is
 *	still dropped.  Runs on every backend (the dispatch is backend-
 *	independent for ev->fd >= 0).
 *
 *	Control: the same event dispatched on B (the park's own io) DOES
 *	claim it (park_fd -> none), so the test cannot pass by a dispatch
 *	that never claims.
 *
 *	/claim_cas_rejects_newer_park covers the window the io test alone
 *	left open (the same fd re-parked between the dispatch's load and its
 *	CAS), which CBMC found in the first version of this fix.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_loop.h"
#include "xtc_io.h"
#include "loop_int.h" /* struct xtc_task park fields, dispatch */

static int
fresh_task(xtc_task_t *t, xtc_loop_t *home)
{
	/* Only the fields __xtc_loop_dispatch_event touches.  The wake at
	 * its end goes through xtc_waker_wake on a task that is not PARKED,
	 * which is a no-op latch -- the state stays RUNNING. */
	memset(t, 0, sizeof *t);
	xtc_atomic_store_i32(&t->state, XTC_TS_RUNNING, xtc_mo_seq_cst);
	xtc_atomic_store_i64(&t->park_fd, __PARK_TOK_NONE, xtc_mo_seq_cst);
	xtc_atomic_store_ptr(&t->park_io, NULL, xtc_mo_seq_cst);
	t->loop = home;
	return 0;
}

static MunitResult
test_stale_event_other_io(const MunitParameter p[], void *d)
{
	xtc_exec_t *e;
	xtc_loop_t *a, *b;
	xtc_task_t t;
	xtc_io_event_t ev;
	int64_t tok;
	int sv[2];
	(void)p;
	(void)d;

	munit_assert_int(xtc_exec_init(&e, 2), ==, XTC_OK);
	a = xtc_exec_loop(e, 0);
	b = xtc_exec_loop(e, 1);
	munit_assert_ptr_not_null(a);
	munit_assert_ptr_not_null(b);
	munit_assert_int(pipe(sv), ==, 0);

	/* The task's newer park: recorded on B's io, fd F. */
	fresh_task(&t, a);
	xtc_atomic_store_ptr(&t.park_io, b->io, xtc_mo_seq_cst);
	tok = __park_tok(&t, sv[0]);
	xtc_atomic_store_i64(&t.park_fd, tok, xtc_mo_seq_cst);

	/* The stale event A reaped for its own (older) registration of F. */
	memset(&ev, 0, sizeof ev);
	ev.tag = &t;
	ev.fd = sv[0];
	ev.flags = XTC_IO_READABLE;
	(void)__xtc_loop_dispatch_event(a, &ev);

	/* A must not have claimed B's park. */
	munit_assert_int64(xtc_atomic_load_i64(&t.park_fd, xtc_mo_seq_cst), ==,
	    tok);
	munit_assert_ptr_equal(xtc_atomic_load_ptr(&t.park_io, xtc_mo_seq_cst),
	    b->io);

	/* Control: the same event from the park's own io claims it. */
	(void)__xtc_loop_dispatch_event(b, &ev);
	munit_assert_int64(xtc_atomic_load_i64(&t.park_fd, xtc_mo_seq_cst), ==,
	    __PARK_TOK_NONE);

	(void)close(sv[0]);
	(void)close(sv[1]);
	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);
	return MUNIT_OK;
}

/*
 * The ABA the io guard alone left open (CBMC, test/cbmc/fdclaim_harness.c):
 * the dispatch loads the claim for park P1 on F, then -- before its CAS --
 * the fiber cleans up and parks on F again (P2, same fd, same io).  A CAS
 * against the bare fd F succeeded against P2 and orphaned it.  The claim
 * word is a token (fd + generation), so the CAS loaded for P1 must fail
 * against P2.  This drives exactly that CAS: token loaded for P1, field
 * holding P2.
 */
static MunitResult
test_claim_cas_rejects_newer_park(const MunitParameter p[], void *d)
{
	xtc_task_t t;
	int64_t p1, p2, expect;
	(void)p;
	(void)d;

	fresh_task(&t, NULL);
	p1 = __park_tok(&t, 7);
	p2 = __park_tok(&t, 7);           /* same fd, the next park */
	munit_assert_int(__park_tok_fd(p1), ==, 7);
	munit_assert_int(__park_tok_fd(p2), ==, 7);
	munit_assert_int64(p1, !=, p2);
	munit_assert_int64(p1, !=, __PARK_TOK_NONE);
	munit_assert_int(__park_tok_fd(__PARK_TOK_NONE), ==, -1);

	xtc_atomic_store_i64(&t.park_fd, p2, xtc_mo_seq_cst);
	expect = p1;                      /* what the stale dispatch loaded */
	munit_assert_false(xtc_atomic_cas_i64(&t.park_fd, &expect,
	    __PARK_TOK_NONE, xtc_mo_seq_cst, xtc_mo_seq_cst));
	munit_assert_int64(xtc_atomic_load_i64(&t.park_fd, xtc_mo_seq_cst), ==,
	    p2);

	/* The generation never yields the no-park value (all ones), even
	 * across its wrap, and the fd survives the round trip. */
	t.park_gen = UINT32_MAX - 2;
	p1 = __park_tok(&t, 0x7fffffff);   /* gen UINT32_MAX - 1 */
	p2 = __park_tok(&t, 0x7fffffff);   /* would be gen UINT32_MAX */
	munit_assert_int64(p1, !=, __PARK_TOK_NONE);
	munit_assert_int64(p2, !=, __PARK_TOK_NONE);
	munit_assert_int64(p1, !=, p2);
	munit_assert_int(__park_tok_fd(p2), ==, 0x7fffffff);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/stale_event_other_io", test_stale_event_other_io, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/claim_cas_rejects_newer_park", test_claim_cas_rejects_newer_park,
	    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/concurrency/stale_readiness", tests,
	NULL, 1, MUNIT_SUITE_OPTION_NONE };

int
main(int argc, char *argv[])
{
	return munit_suite_main(&suite, NULL, argc, argv);
}
