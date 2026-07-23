/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m13/test_accel.c -- xtc_accel attached-compute (GPU/NPU) bridge.
 *
 *	The module has no GPU/NPU dependency to TEST: the fence-fd wait
 *	is exercised with an eventfd standing in for a real accelerator
 *	completion fence (both are pollable fds that become readable when
 *	signalled -- to poll(2) and thus to the loop, a dma-fence
 *	sync_file and an eventfd are indistinguishable, which is exactly
 *	the property the design leans on).  So this runs identically on a
 *	CI box with no accelerator and on the dev laptop that has one.
 *
 *	Coverage:
 *	  - probe: always XTC_OK; any reported device has sane fields;
 *	    on this build we just print what was found (0 is fine).
 *	  - wait_fence: a fiber parks on an unsignalled eventfd, a peer
 *	    signals it, the fiber wakes with XTC_OK -- the real
 *	    park-on-completion path, no OS thread held.
 *	  - wait_fence timeout: parking on a never-signalled fd returns
 *	    XTC_E_TIMEDOUT (or XTC_E_NOSYS if built without accel).
 *	  - wait_fence bad-fd: XTC_E_INVAL (or NOSYS).
 *	  - run_blocking: runs the consumer closure, returns its result.
 */

#define _GNU_SOURCE
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/eventfd.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_accel.h"
#include "xtc_loop.h"
#include "xtc_proc.h"

/* True iff this build has accelerator (fence-wait) support. */
#if defined(XTC_HAVE_ACCEL)
# define ACCEL_BUILT 1
#else
# define ACCEL_BUILT 0
#endif

/* ---- probe: always succeeds; validate any device it reports ---- */
static MunitResult
test_probe(const MunitParameter p[], void *d)
{
	xtc_accel_dev_t devs[16];
	int n = -1, i;
	(void)p; (void)d;

	munit_assert_int(xtc_accel_probe(devs, 16, &n), ==, XTC_OK);
	munit_assert_int(n, >=, 0);
	printf("# xtc_accel_probe: %d device(s)%s\n", n,
	    ACCEL_BUILT ? "" : " (accel support not built in)");
	for (i = 0; i < n && i < 16; i++) {
		munit_assert_true(devs[i].kind == XTC_ACCEL_KIND_GPU ||
		    devs[i].kind == XTC_ACCEL_KIND_NPU);
		munit_assert_true(devs[i].node[0] != '\0');
		printf("#   [%d] kind=%s name=%s node=%s driver=%s\n", i,
		    devs[i].kind == XTC_ACCEL_KIND_GPU ? "GPU" : "NPU",
		    devs[i].name, devs[i].node,
		    devs[i].driver[0] ? devs[i].driver : "?");
	}
	/* NULL out_n is rejected; a zero-max probe with NULL buf is fine. */
	munit_assert_int(xtc_accel_probe(devs, 16, NULL), ==, XTC_E_INVAL);
	munit_assert_int(xtc_accel_probe(NULL, 0, &n), ==, XTC_OK);
	return MUNIT_OK;
}

/* ---- wait_fence: park a fiber on an eventfd "fence", signal it ---- */
static _Atomic int g_wait_rc;
static int         g_fence_fd;

/* Waiter proc: park on the fence fd until it signals. */
static void
waiter_proc(void *arg)
{
	int fd = (int)(intptr_t)arg;
	int rc = xtc_accel_wait_fence(fd, 5LL * 1000 * 1000 * 1000); /* 5s */
	atomic_store(&g_wait_rc, rc);
}

/* Signaller proc: after a short yield-loop (so the waiter parks first),
 * write to the eventfd -- the "accelerator completed" signal. */
static void
signaller_proc(void *arg)
{
	int fd = (int)(intptr_t)arg;
	uint64_t one = 1;
	int i;
	for (i = 0; i < 50; i++)
		xtc_yield();          /* let the waiter reach its park */
	if (write(fd, &one, sizeof one) != (ssize_t)sizeof one)
		atomic_store(&g_wait_rc, -99);   /* signal write failed */
}

static MunitResult
test_wait_fence_signalled(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t opts = { 0 };
	xtc_pid_t w, s;
	int fd;
	(void)p; (void)d;

	if (!ACCEL_BUILT) {
		/* Contract on a non-accel build: NOSYS regardless of fd. */
		munit_assert_int(xtc_accel_wait_fence(0, 0), ==, XTC_E_NOSYS);
		return MUNIT_SKIP;
	}

	fd = eventfd(0, EFD_NONBLOCK);
	munit_assert_int(fd, >=, 0);
	g_fence_fd = fd;
	atomic_store(&g_wait_rc, 12345);

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	opts.name = "waiter";
	munit_assert_int(xtc_proc_spawn(loop, waiter_proc,
	    (void *)(intptr_t)fd, &opts, &w), ==, XTC_OK);
	opts.name = "signaller";
	munit_assert_int(xtc_proc_spawn(loop, signaller_proc,
	    (void *)(intptr_t)fd, &opts, &s), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);

	/* The waiter woke because the "fence" signalled -> XTC_OK. */
	munit_assert_int(atomic_load(&g_wait_rc), ==, XTC_OK);
	(void)close(fd);
	return MUNIT_OK;
}

/* ---- wait_fence: never-signalled fd times out ---- */
static _Atomic int g_to_rc;

static void
timeout_proc(void *arg)
{
	int fd = (int)(intptr_t)arg;
	/* Short timeout on a fence that never fires. */
	int rc = xtc_accel_wait_fence(fd, 50 * 1000 * 1000); /* 50ms */
	atomic_store(&g_to_rc, rc);
}

static MunitResult
test_wait_fence_timeout(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t opts = { 0 };
	xtc_pid_t t;
	int fd;
	(void)p; (void)d;

	if (!ACCEL_BUILT)
		return MUNIT_SKIP;

	fd = eventfd(0, EFD_NONBLOCK);
	munit_assert_int(fd, >=, 0);
	atomic_store(&g_to_rc, 12345);

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	opts.name = "timeout";
	munit_assert_int(xtc_proc_spawn(loop, timeout_proc,
	    (void *)(intptr_t)fd, &opts, &t), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);

	munit_assert_int(atomic_load(&g_to_rc), ==, XTC_E_AGAIN);
	(void)close(fd);
	return MUNIT_OK;
}

/* ---- wait_fence bad fd ---- */
static MunitResult
test_wait_fence_badfd(const MunitParameter p[], void *d)
{
	int rc;
	(void)p; (void)d;
	rc = xtc_accel_wait_fence(-1, 0);
	if (ACCEL_BUILT)
		munit_assert_int(rc, ==, XTC_E_INVAL);
	else
		munit_assert_int(rc, ==, XTC_E_NOSYS);
	return MUNIT_OK;
}

/* ---- run_blocking: runs the closure, returns its result ---- */
static int
compute_fn(void *arg)
{
	int x = (int)(intptr_t)arg;
	return x * 3;
}

static MunitResult
test_run_blocking(const MunitParameter p[], void *d)
{
	int out = -1;
	(void)p; (void)d;

	/* Works with no loop (synchronous fallback) regardless of build. */
	munit_assert_int(
	    xtc_accel_run_blocking(compute_fn, (void *)(intptr_t)7, &out),
	    ==, XTC_OK);
	munit_assert_int(out, ==, 21);
	/* NULL fn rejected. */
	munit_assert_int(xtc_accel_run_blocking(NULL, NULL, &out),
	    ==, XTC_E_INVAL);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/probe",              test_probe,                NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/wait_fence_signal",  test_wait_fence_signalled, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/wait_fence_timeout", test_wait_fence_timeout,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/wait_fence_badfd",   test_wait_fence_badfd,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/run_blocking",       test_run_blocking,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = {
	"/m13/accel", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
