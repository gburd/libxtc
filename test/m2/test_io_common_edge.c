/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * test/m2/test_io_common_edge.c
 *	Edge coverage for io_common.c paths that the existing
 *	fault-injection test (test_io_fault_inject.c) does not reach.
 *
 *	The four inject points in xtc_io_init (calloc/pipe/fcntl/backend
 *	fail) are already covered there.  This file drives the remaining
 *	public-API-reachable edges:
 *
 *	  - the xtc_io_wakeup EAGAIN/EWOULDBLOCK coalesce branch (the
 *	    self-pipe fills, a further wakeup must coalesce and still
 *	    return XTC_OK rather than blocking or erroring);
 *	  - xtc_io_backend_name returns a stable non-empty string;
 *	  - a fresh init/fini after a forced fcntl_fail leaves no fd
 *	    leak (init still succeeds), exercising the fcntl cleanup
 *	    branch's fd-close correctness end to end.
 *
 *	NOTE: the __xtc_io_register_wakeup failure-cleanup branch in
 *	xtc_io_init has NO injection point in src/io/io_common.c, so it
 *	is not reachable from the public API without a source edit.  It
 *	is deliberately not tested here.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_io.h"
#include "xtc_inject.h"
#include "xtc_int.h"

/* ----- xtc_io_wakeup coalesce path (EAGAIN on a full self-pipe) ---
 *
 * The self-pipe write end is O_NONBLOCK.  xtc_io_wakeup writes one
 * byte; when the pipe buffer is full the write returns EAGAIN and the
 * function must treat that as "a wakeup is already pending" and return
 * XTC_OK (coalesced), never XTC_E_INTERNAL and never block.  We hammer
 * wakeup far past a typical 64 KiB pipe buffer so the buffer is
 * guaranteed to fill, then assert every call still reported XTC_OK. */
static MunitResult
test_wakeup_coalesce(const MunitParameter p[], void *d)
{
	xtc_io_t *io = NULL;
	int i;
	(void)p; (void)d;

	munit_assert_int(xtc_io_init(&io), ==, XTC_OK);
	munit_assert_not_null(io);

	/* 256 KiB worth of 1-byte wakeups: well past any pipe buffer, so
	 * the EAGAIN coalesce branch is guaranteed to be exercised. */
	for (i = 0; i < 256 * 1024; i++)
		munit_assert_int(xtc_io_wakeup(io), ==, XTC_OK);

	munit_assert_int(xtc_io_fini(io), ==, XTC_OK);
	return MUNIT_OK;
}

/* ----- backend name is stable and non-empty --------------------- */
static MunitResult
test_backend_name(const MunitParameter p[], void *d)
{
	const char *n1, *n2;
	(void)p; (void)d;
	n1 = xtc_io_backend_name();
	n2 = xtc_io_backend_name();
	munit_assert_not_null(n1);
	munit_assert_string_equal(n1, n2);   /* stable */
	munit_assert_int(n1[0], !=, '\0');    /* non-empty */
	return MUNIT_OK;
}

/* ----- fcntl cleanup leaves no fd leak -------------------------- *
 *
 * On POSIX self-pipe backends, tripping io.init.fcntl_fail forces the
 * cleanup branch that closes BOTH pipe fds and frees io.  If that
 * cleanup were wrong (a leaked fd), enough repetitions would exhaust
 * the fd table and a later real init would fail.  We loop the forced
 * failure many times, then require a clean init/fini to still succeed,
 * proving the fcntl cleanup path returns every fd. */
#if !defined(_WIN32) && !defined(XTC_IO_BACKEND_KQUEUE)
static void
inj_noop(const char *name, void *user) { (void)name; (void)user; }

static MunitResult
test_fcntl_cleanup_no_leak(const MunitParameter p[], void *d)
{
	xtc_io_t *io = NULL;
	int i;
	(void)p; (void)d;

	munit_assert_int(xtc_inject_attach("io.init.fcntl_fail", inj_noop, NULL),
	    ==, XTC_OK);
	for (i = 0; i < 4096; i++) {
		io = NULL;
		munit_assert_int(xtc_io_init(&io), ==, XTC_E_INTERNAL);
		munit_assert_null(io);
	}
	(void)xtc_inject_detach("io.init.fcntl_fail");

	/* If any of the 4096 forced failures leaked its pipe fds, this
	 * clean init would eventually hit EMFILE and fail. */
	io = NULL;
	munit_assert_int(xtc_io_init(&io), ==, XTC_OK);
	munit_assert_not_null(io);
	munit_assert_int(xtc_io_fini(io), ==, XTC_OK);
	return MUNIT_OK;
}
#endif

static MunitTest tests[] = {
	{ "/wakeup_coalesce", test_wakeup_coalesce, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/backend_name",    test_backend_name,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
#if !defined(_WIN32) && !defined(XTC_IO_BACKEND_KQUEUE)
	{ "/fcntl_cleanup_no_leak", test_fcntl_cleanup_no_leak, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
#endif
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m2/io_common_edge", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
