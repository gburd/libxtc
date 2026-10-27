/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/m8/test_kill_late.c
 *	When does xtc_exit_pid reach a proc that is parked?
 *	  /await         a proc parked in xtc_await on a task that runs for
 *	                 another 2 s is killed: its DOWN must arrive at once
 *	                 (before 2.0 it arrived only when the awaited task
 *	                 finished), and the awaited task must still finish and
 *	                 be awaitable by someone else.
 *	  /await_masked  the same kill inside xtc_mask_enter: latched, not
 *	                 delivered, until the await returns and the mask is
 *	                 left.
 *	  /blocking_run  a proc parked in xtc_blocking_run is masked by design
 *	                 (the pool worker writes into its frame): the DOWN
 *	                 arrives only once the offload returns.
 *	  /aio           a proc doing back-to-back xtc_aio_pwrite + fsync is
 *	                 killed: the DOWN arrives once the operation in flight
 *	                 completes (the kernel owns the buffer and the request
 *	                 until then).  Before 2.0 it never arrived: the woken
 *	                 park re-parked until the op was done, nothing
 *	                 delivered the kill after it, and the next op parked
 *	                 again (no DOWN in 10 s).
 */

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_async.h"
#include "xtc_proc.h"
#include "xtc_blocking.h"
#include "xtc_aio.h"

static xtc_loop_t *g_loop;
static xtc_task_t *g_slow;
static atomic_int  g_slow_done, g_victim_past, g_victim_parked, g_stop;
static int64_t     g_slow_ns, g_kill_ns, g_down_ns;
static int         g_down_ok, g_down_reason, g_second_ok, g_masked;
static intptr_t    g_second_r;

static int64_t
now_ns(void)
{
	return xtc_clock_mono();
}

/* The awaited task: an xtc_async coroutine that sleeps g_slow_ns in 1 ms
 * steps (a coroutine, not a proc: xtc_yield only, no proc sleep). */
static intptr_t
slow_task(void *arg)
{
	int64_t end = now_ns() + g_slow_ns;
	(void)arg;
	while (now_ns() < end)
		xtc_yield();
	atomic_store(&g_slow_done, 1);
	return 42;
}

static void
await_victim(void *arg)
{
	intptr_t r = 0;
	(void)arg;
	if (g_masked)
		munit_assert_int(xtc_mask_enter(), ==, XTC_OK);
	atomic_store(&g_victim_parked, 1);
	(void)xtc_await(g_slow, &r);
	if (g_masked) {
		/* The kill was latched, not delivered, while masked. */
		munit_assert_int((int)r, ==, 42);
		atomic_store(&g_victim_past, 1);
		(void)xtc_mask_leave();   /* delivers it: does not return */
	}
	atomic_store(&g_victim_past, 2);   /* never reached */
}

static int
blocking_fn(void *arg)
{
	(void)arg;
	usleep(400 * 1000);   /* the pool worker owns the victim's frame */
	return 0;
}

static void
blocking_victim(void *arg)
{
	int rc = 0;
	(void)arg;
	atomic_store(&g_victim_parked, 1);
	(void)xtc_blocking_run(blocking_fn, NULL, &rc);
	atomic_store(&g_victim_past, 2);   /* never reached */
}

static int g_aio_fd = -1;
static int g_aio_mib = 1;   /* MiB written before each fsync (env KILL_LATE_MIB) */
static void
aio_victim(void *arg)
{
	static char buf[1 << 20];
	int k, j;
	(void)arg;
	atomic_store(&g_victim_parked, 1);
	for (k = 0; !atomic_load(&g_stop); k++) {
		for (j = 0; j < g_aio_mib; j++)
			(void)xtc_aio_pwrite(g_aio_fd, buf, sizeof buf,
			    (int64_t)j * (int64_t)sizeof buf);
		(void)xtc_aio_fsync(g_aio_fd);
	}
	atomic_store(&g_victim_past, 2);
}

/* The driver: spawn the victim monitored, wait until it parks, kill it,
 * time the DOWN. */
static void (*g_victim_fn)(void *);
static void
driver(void *arg)
{
	xtc_pid_t victim;
	uint64_t ref;
	void *m = NULL;
	size_t n = 0;
	int i;
	(void)arg;
	munit_assert_int(xtc_proc_spawn_monitor(g_loop, g_victim_fn, NULL,
	    NULL, &victim, &ref), ==, XTC_OK);
	for (i = 0; i < 1000 && !atomic_load(&g_victim_parked); i++)
		xtc_yield();
	(void)xtc_proc_sleep(20LL * 1000 * 1000);   /* well inside the park */
	g_kill_ns = now_ns();
	munit_assert_int(xtc_exit_pid(victim, 9), ==, XTC_OK);
	g_down_ok = xtc_recv(&m, &n, 10LL * 1000 * 1000 * 1000) == XTC_OK &&
	    xtc_down_decode(m, n, NULL, &g_down_reason) == XTC_OK;
	g_down_ns = now_ns();
	atomic_store(&g_stop, 1);   /* a victim that was never killed ends */
	if (m != NULL)
		xtc_free(m);
	if (g_slow != NULL) {
		/* The awaited task is intact for another awaiter. */
		g_second_ok = xtc_await(g_slow, &g_second_r) == XTC_OK;
	}
}

static void
run(void (*victim)(void *), int with_slow)
{
	atomic_store(&g_slow_done, 0);
	atomic_store(&g_victim_past, 0);
	atomic_store(&g_victim_parked, 0);
	atomic_store(&g_stop, 0);
	g_down_ok = g_second_ok = 0;
	g_down_reason = -1;
	g_second_r = 0;
	g_slow = NULL;
	g_victim_fn = victim;
	munit_assert_int(xtc_loop_init(&g_loop), ==, XTC_OK);
	if (with_slow)
		munit_assert_int(xtc_async(g_loop, slow_task, NULL, &g_slow),
		    ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(g_loop, driver, NULL, NULL, NULL), ==,
	    XTC_OK);
	munit_assert_int(xtc_loop_run(g_loop), ==, XTC_OK);
	(void)xtc_loop_fini(g_loop);
}

static MunitResult
test_await(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	g_slow_ns = 2000LL * 1000 * 1000;
	g_masked = 0;
	run(await_victim, 1);
	munit_logf(MUNIT_LOG_INFO, "kill -> DOWN %.1f ms (the awaited task "
	    "runs 2000 ms)", (double)(g_down_ns - g_kill_ns) / 1e6);
	munit_assert_int(g_down_ok, ==, 1);
	munit_assert_int(g_down_reason, ==, 9);
	munit_assert_int(atomic_load(&g_victim_past), ==, 0);
	/* Delivered at once, not when the awaited task finished. */
	munit_assert_int64(g_down_ns - g_kill_ns, <, 500LL * 1000 * 1000);
	munit_assert_int(g_second_ok, ==, 1);
	munit_assert_int((int)g_second_r, ==, 42);
	munit_assert_int(atomic_load(&g_slow_done), ==, 1);
	return MUNIT_OK;
}

static MunitResult
test_await_masked(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	g_slow_ns = 300LL * 1000 * 1000;
	g_masked = 1;
	run(await_victim, 1);
	g_masked = 0;
	munit_assert_int(g_down_ok, ==, 1);
	munit_assert_int(g_down_reason, ==, 9);
	/* Latched while masked: the await completed, then the leave
	 * delivered the kill. */
	munit_assert_int(atomic_load(&g_victim_past), ==, 1);
	munit_assert_int64(g_down_ns - g_kill_ns, >=, 200LL * 1000 * 1000);
	return MUNIT_OK;
}

static MunitResult
test_blocking_run(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	run(blocking_victim, 0);
	munit_logf(MUNIT_LOG_INFO, "kill -> DOWN %.1f ms (offload 400 ms)",
	    (double)(g_down_ns - g_kill_ns) / 1e6);
	munit_assert_int(g_down_ok, ==, 1);
	munit_assert_int(g_down_reason, ==, 9);
	munit_assert_int(atomic_load(&g_victim_past), ==, 0);
	/* Masked by design: the DOWN waits for the offload. */
	munit_assert_int64(g_down_ns - g_kill_ns, >=, 300LL * 1000 * 1000);
	return MUNIT_OK;
}

static MunitResult
test_aio(const MunitParameter p[], void *d)
{
	char path[] = "/tmp/xtc_kill_late_XXXXXX";
	const char *tmp = getenv("TMPDIR");
	char tpath[512];
	(void)p; (void)d;
	if (tmp != NULL && strlen(tmp) < 400) {
		(void)snprintf(tpath, sizeof tpath, "%s/xtc_kill_late_XXXXXX",
		    tmp);
		g_aio_fd = mkstemp(tpath);
	} else {
		g_aio_fd = mkstemp(path);
	}
	munit_assert_int(g_aio_fd, >=, 0);
	if (getenv("KILL_LATE_MIB") != NULL)
		g_aio_mib = atoi(getenv("KILL_LATE_MIB"));
	(void)unlink(tmp != NULL && strlen(tmp) < 400 ? tpath : path);
	run(aio_victim, 0);
	(void)close(g_aio_fd);
	munit_logf(MUNIT_LOG_INFO, "kill -> DOWN %.1f ms (%d MiB pwrite + "
	    "fsync loop)", (double)(g_down_ns - g_kill_ns) / 1e6, g_aio_mib);
	munit_assert_int(g_down_ok, ==, 1);
	munit_assert_int(g_down_reason, ==, 9);
	munit_assert_int(atomic_load(&g_victim_past), ==, 0);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/await", test_await, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/await_masked", test_await_masked, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/blocking_run", test_blocking_run, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/aio", test_aio, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite suite = {
	"/m8/kill_late", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

int
main(int argc, char *argv[])
{
	return munit_suite_main(&suite, NULL, argc, argv);
}
