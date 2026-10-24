/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m2/test_io_uring_setup.c -- io_uring ring setup: optional setup
 * flags are probed with a fallback for older kernels, and the thread
 * that creates a ring submits nothing to it.
 *
 *   /coop_taskrun     the main ring asks for COOP_TASKRUN|TASKRUN_FLAG;
 *                     the kernel either grants both or (pre-5.19) the
 *                     ring is built without them, and still works.
 *   /setup_fallback   a kernel that rejects the optional flags with
 *                     EINVAL ("io.uring.setup_einval") still gets a
 *                     working ring, with none of them set.
 *   /creator_no_submit xtc_io_init submits no SQE (the wakeup poll's
 *                     completion work runs on its SUBMITTER, so a
 *                     creator-submitted poll bound every loop's first
 *                     cross-thread wake to the creator).  The first poll,
 *                     on the owner, arms it, and a wakeup written BEFORE
 *                     that arm is still delivered.
 *
 * Other backends: every case is a skip.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <sys/utsname.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_io.h"
#include "xtc_inject.h"

int __xtc_io_uring_info(xtc_io_t *, unsigned *, unsigned *);

/* Kernel UAPI values (stable), spelled here so the test builds without
 * liburing on the non-uring backends. */
#define SETUP_COOP_TASKRUN   (1u << 8)
#define SETUP_TASKRUN_FLAG   (1u << 9)
#define OPT (SETUP_COOP_TASKRUN | SETUP_TASKRUN_FLAG)

static int
is_uring(void)
{
	return strcmp(xtc_io_backend_name(), "uring") == 0;
}

/* Running kernel at least maj.min? */
static int
kernel_at_least(int maj, int min)
{
	struct utsname u;
	int a = 0, b = 0;

	if (uname(&u) != 0 || sscanf(u.release, "%d.%d", &a, &b) != 2)
		return 0;
	return a > maj || (a == maj && b >= min);
}

/* Wakeup round trip on io: post, then a bounded poll must see it. */
static void
assert_wakes(xtc_io_t *io)
{
	xtc_io_event_t evs[8];
	int n = 0;

	munit_assert_int(xtc_io_wakeup(io), ==, XTC_OK);
	munit_assert_int(xtc_io_poll(io, evs, 8, 2000LL * 1000000, &n), ==,
	    XTC_OK);
	munit_assert_int(n, ==, 1);
	munit_assert_uint(evs[0].flags & XTC_IO_WAKEUP, ==, XTC_IO_WAKEUP);
}

static MunitResult
test_coop_taskrun(const MunitParameter p[], void *d)
{
	xtc_io_t *io = NULL;
	unsigned fl = 0;
	(void)p; (void)d;

	if (!is_uring())
		return MUNIT_SKIP;
	munit_assert_int(xtc_io_init(&io), ==, XTC_OK);
	munit_assert_int(__xtc_io_uring_info(io, &fl, NULL), ==, XTC_OK);
	/* Both or neither: TASKRUN_FLAG alone is invalid.  5.19 has both. */
	munit_assert_true((fl & OPT) == OPT || (fl & OPT) == 0);
	if (kernel_at_least(5, 19))
		munit_assert_uint(fl & OPT, ==, OPT);
	munit_logf(MUNIT_LOG_INFO, "ring flags 0x%x (coop_taskrun %s)", fl,
	    (fl & OPT) == OPT ? "on" : "off: kernel < 5.19");
	assert_wakes(io);
	munit_assert_int(xtc_io_fini(io), ==, XTC_OK);
	return MUNIT_OK;
}

static void
inj_nop(const char *name, void *user)
{
	(void)name; (void)user;
}

static MunitResult
test_setup_fallback(const MunitParameter p[], void *d)
{
	xtc_io_t *io = NULL;
	unsigned fl = ~0u;
	int rc;
	(void)p; (void)d;

	if (!is_uring())
		return MUNIT_SKIP;
	munit_assert_int(xtc_inject_attach("io.uring.setup_einval", inj_nop,
	    NULL), ==, XTC_OK);
	rc = xtc_io_init(&io);
	(void)xtc_inject_detach("io.uring.setup_einval");
	munit_assert_int(rc, ==, XTC_OK);
	munit_assert_int(__xtc_io_uring_info(io, &fl, NULL), ==, XTC_OK);
	munit_assert_uint(fl & OPT, ==, 0);
	assert_wakes(io);
	munit_assert_int(xtc_io_fini(io), ==, XTC_OK);
	return MUNIT_OK;
}

static MunitResult
test_creator_no_submit(const MunitParameter p[], void *d)
{
	xtc_io_t *io = NULL;
	xtc_io_event_t evs[8];
	unsigned head = ~0u;
	int n = -1;
	(void)p; (void)d;

	if (!is_uring())
		return MUNIT_SKIP;
	munit_assert_int(xtc_io_init(&io), ==, XTC_OK);
	munit_assert_int(__xtc_io_uring_info(io, NULL, &head), ==, XTC_OK);
	munit_assert_uint(head, ==, 0);   /* nothing reached the kernel */
	/* Written before any arm: the first poll's arm must still see it. */
	munit_assert_int(xtc_io_wakeup(io), ==, XTC_OK);
	munit_assert_int(xtc_io_poll(io, evs, 8, 2000LL * 1000000, &n), ==,
	    XTC_OK);
	munit_assert_int(n, ==, 1);
	munit_assert_uint(evs[0].flags & XTC_IO_WAKEUP, ==, XTC_IO_WAKEUP);
	munit_assert_int(__xtc_io_uring_info(io, NULL, &head), ==, XTC_OK);
	munit_assert_uint(head, >=, 1);
	/* And it stays armed. */
	assert_wakes(io);
	munit_assert_int(xtc_io_poll(io, evs, 8, 0, &n), ==, XTC_OK);
	munit_assert_int(n, ==, 0);
	munit_assert_int(xtc_io_fini(io), ==, XTC_OK);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/coop_taskrun", test_coop_taskrun, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/setup_fallback", test_setup_fallback, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/creator_no_submit", test_creator_no_submit, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m2/io_uring_setup", tests, NULL, 1,
    MUNIT_SUITE_OPTION_NONE };

int
main(int argc, char *argv[])
{
	return munit_suite_main(&suite, NULL, argc, argv);
}
