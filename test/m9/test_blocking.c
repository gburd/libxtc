/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m9/test_blocking.c -- xtc_blocking thread-pool offload.
 *
 *	Verifies that a blocking call offloaded with xtc_blocking_run
 *	returns the right result, that the calling loop keeps running
 *	other processes while one is parked on an offload (the whole
 *	point), that many concurrent offloads through a small pool all
 *	complete correctly, and that the no-loop fallback runs the work
 *	synchronously.
 */

#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_blocking.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "os_time.h"

/* A blocking function: sleep `arg` milliseconds, return arg * 2. */
static int
sleep_fn(void *arg)
{
	long ms = (long)(intptr_t)arg;
	struct timespec ts;
	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (ms % 1000) * 1000000L;
	(void)nanosleep(&ts, NULL);
	return (int)(ms * 2);
}

/* ---- fallback: called with no loop, runs synchronously ---- */
static MunitResult
test_fallback(const MunitParameter p[], void *d)
{
	int out = -1;
	(void)p; (void)d;
	munit_assert_int(xtc_blocking_run(sleep_fn, (void *)(intptr_t)5, &out),
	    ==, XTC_OK);
	munit_assert_int(out, ==, 10);
	return MUNIT_OK;
}

/* ---- in-proc offload + loop liveness ---- */
static _Atomic long g_t_start, g_t_end, g_t_b;
static _Atomic int  g_a_ok, g_b_ran;

static void
proc_a(void *arg)
{
	int out = -1;
	int64_t t0 = 0, t1 = 0;
	(void)arg;
	(void)__os_clock_mono(&t0);
	atomic_store(&g_t_start, (long)t0);
	(void)xtc_blocking_run(sleep_fn, (void *)(intptr_t)60, &out);
	(void)__os_clock_mono(&t1);
	atomic_store(&g_t_end, (long)t1);
	atomic_store(&g_a_ok, out == 120 ? 1 : 0);
}

static void
proc_b(void *arg)
{
	int64_t t = 0;
	(void)arg;
	(void)__os_clock_mono(&t);
	atomic_store(&g_t_b, (long)t);
	atomic_store(&g_b_ran, 1);
}

static MunitResult
test_in_proc_liveness(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t opts = { 0 };
	xtc_pid_t a, b;
	(void)p; (void)d;

	atomic_store(&g_t_start, 0); atomic_store(&g_t_end, 0);
	atomic_store(&g_t_b, 0); atomic_store(&g_a_ok, 0);
	atomic_store(&g_b_ran, 0);

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	opts.name = "A";
	munit_assert_int(xtc_proc_spawn(loop, proc_a, NULL, &opts, &a),
	    ==, XTC_OK);
	opts.name = "B";
	munit_assert_int(xtc_proc_spawn(loop, proc_b, NULL, &opts, &b),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);

	/* A's offload returned the right value. */
	munit_assert_int(atomic_load(&g_a_ok), ==, 1);
	/* B ran, and it ran before A's blocking call finished -- proof
	 * the loop was not blocked by the offload (A is spawned first,
	 * so it parks first; if the offload blocked the loop thread, B
	 * could not run until after t_end). */
	munit_assert_int(atomic_load(&g_b_ran), ==, 1);
	munit_assert_int64(atomic_load(&g_t_b), <=, atomic_load(&g_t_end));
	munit_assert_int64(atomic_load(&g_t_b), >=, atomic_load(&g_t_start));
	return MUNIT_OK;
}

/* ---- many concurrent offloads through a small pool ---- */
#define NCONC 16
static _Atomic int g_conc_ok;

static void
conc_proc(void *arg)
{
	int val = (int)(intptr_t)arg;
	int local = val;             /* stable while parked */
	int out = -1;
	(void)xtc_blocking_run(sleep_fn, (void *)(intptr_t)local, &out);
	if (out == local * 2)
		atomic_fetch_add(&g_conc_ok, 1);
}

static MunitResult
test_concurrent(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t opts = { 0 };
	xtc_pid_t pid;
	int i;
	(void)p; (void)d;

	atomic_store(&g_conc_ok, 0);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	for (i = 0; i < NCONC; i++) {
		/* values 1..NCONC ms; all offloaded through a 4-thread pool */
		opts.name = "c";
		munit_assert_int(xtc_proc_spawn(loop, conc_proc,
		    (void *)(intptr_t)(i + 1), &opts, &pid), ==, XTC_OK);
	}
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	munit_assert_int(atomic_load(&g_conc_ok), ==, NCONC);
	return MUNIT_OK;
}

/* ---- offloading real file I/O from a proc (the async-VFS pattern) ---- */
struct pread_ctx {
	int    fd;
	char   buf[512];
	int    n;
	off_t  off;
};

static int
pread_fn(void *arg)
{
	struct pread_ctx *c = arg;
	ssize_t r = pread(c->fd, c->buf, (size_t)c->n, c->off);
	return (int)r;
}

static _Atomic int g_file_ok;

static void
file_proc(void *arg)
{
	struct pread_ctx ctx;
	int out = -1;
	int fd = (int)(intptr_t)arg;
	ctx.fd = fd;
	ctx.n = 512;
	ctx.off = 0;
	memset(ctx.buf, 0, sizeof ctx.buf);
	/* Offload the read to the pool; the loop runs others meanwhile. */
	(void)xtc_blocking_run(pread_fn, &ctx, &out);
	if (out == 512 && ctx.buf[0] == 'Z' && ctx.buf[511] == 'Z')
		atomic_store(&g_file_ok, 1);
}

static MunitResult
test_file_offload(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t opts = { 0 };
	xtc_pid_t pid;
	char path[] = "/tmp/xtc-blk-file-XXXXXX";
	char pattern[512];
	int fd;
	(void)p; (void)d;

	atomic_store(&g_file_ok, 0);
	fd = mkstemp(path);
	munit_assert_int(fd, >=, 0);
	memset(pattern, 'Z', sizeof pattern);
	munit_assert_int((int)write(fd, pattern, sizeof pattern), ==, 512);

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	opts.name = "file";
	munit_assert_int(xtc_proc_spawn(loop, file_proc,
	    (void *)(intptr_t)fd, &opts, &pid), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);

	(void)close(fd);
	(void)unlink(path);
	munit_assert_int(atomic_load(&g_file_ok), ==, 1);
	return MUNIT_OK;
}

/* ---- clean shutdown (and restart) ---- */
static MunitResult
test_shutdown(const MunitParameter p[], void *d)
{
	int out = -1;
	(void)p; (void)d;
	xtc_blocking_shutdown();          /* join pool threads */
	xtc_blocking_shutdown();          /* idempotent */
	/* A subsequent fallback call still works (no loop here). */
	munit_assert_int(xtc_blocking_run(sleep_fn, (void *)(intptr_t)1, &out),
	    ==, XTC_OK);
	munit_assert_int(out, ==, 2);
	return MUNIT_OK;
}

/* ---- fire-and-forget submit ---- */
static _Atomic int g_submit_ran;

static int
submit_marker_fn(void *arg)
{
	/* arg is a heap int the fn owns and frees -- exercises the
	 * "fn owns arg" lifetime contract. */
	int *p = arg;
	atomic_fetch_add(&g_submit_ran, *p);
	free(p);
	return 0;
}

static MunitResult
test_submit_fire_forget(const MunitParameter p[], void *d)
{
	int i, spins;
	(void)p; (void)d;

	munit_assert_int(xtc_blocking_submit(NULL, NULL), ==, XTC_E_INVAL);

	atomic_store(&g_submit_ran, 0);
	for (i = 0; i < 10; i++) {
		int *v = malloc(sizeof *v);
		munit_assert_not_null(v);
		*v = 1;
		munit_assert_int(xtc_blocking_submit(submit_marker_fn, v), ==, XTC_OK);
	}
	/* No completion signal: poll the counter until all 10 ran. */
	for (spins = 0; spins < 5000 && atomic_load(&g_submit_ran) < 10; spins++) {
		struct timespec ts = { 0, 1000000 };  /* 1ms */
		(void)nanosleep(&ts, NULL);
	}
	munit_assert_int(atomic_load(&g_submit_ran), ==, 10);
	return MUNIT_OK;
}

/* ---- on-demand pool growth: many same-duration offloads run
 * concurrently (the pool grows past its initial size), so wall-clock
 * time is ~one sleep, not (N / initial_threads) sleeps ---- */
#define NGROW 32
#define GROW_MS 80
static _Atomic int   g_grow_ok;
static _Atomic long  g_grow_wall_start, g_grow_wall_end;

static void
grow_proc(void *arg)
{
	int out = -1;
	(void)arg;
	(void)xtc_blocking_run(sleep_fn, (void *)(intptr_t)GROW_MS, &out);
	if (out == GROW_MS * 2)
		atomic_fetch_add(&g_grow_ok, 1);
}

static void
grow_timer_proc(void *arg)
{
	int64_t t = 0;
	(void)arg;
	(void)__os_clock_mono(&t);
	atomic_store(&g_grow_wall_start, (long)t);
}

static MunitResult
test_pool_grows(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t opts = { 0 };
	xtc_pid_t pid;
	int64_t t1 = 0;
	long elapsed_ms;
	int i;
	(void)p; (void)d;

	/* A prior test may have pinned / started the pool; restart it clean
	 * so this runs against the auto-sizing + growth default. */
	xtc_blocking_shutdown();

	atomic_store(&g_grow_ok, 0);
	atomic_store(&g_grow_wall_start, 0);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	opts.name = "t";
	munit_assert_int(xtc_proc_spawn(loop, grow_timer_proc, NULL, &opts, &pid),
	    ==, XTC_OK);
	for (i = 0; i < NGROW; i++) {
		opts.name = "g";
		munit_assert_int(xtc_proc_spawn(loop, grow_proc, NULL, &opts, &pid),
		    ==, XTC_OK);
	}
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	(void)__os_clock_mono(&t1);
	atomic_store(&g_grow_wall_end, (long)t1);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);

	/* All offloads returned correctly. */
	munit_assert_int(atomic_load(&g_grow_ok), ==, NGROW);

	/* With the pool growing toward NGROW workers, the offloads run
	 * largely in parallel: total wall time is a small multiple of one
	 * GROW_MS sleep, not NGROW * GROW_MS (fully serial) and not
	 * (NGROW / 4) * GROW_MS (the old fixed-4 pool, ~640ms here).  Allow
	 * generous slack for scheduling and thread spin-up: assert it
	 * finished well under the old fixed-4 floor. */
	elapsed_ms = (atomic_load(&g_grow_wall_end) -
	              atomic_load(&g_grow_wall_start)) / 1000000L;
	munit_assert_int64(elapsed_ms, <, (NGROW / 4) * GROW_MS);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/fallback",   test_fallback,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/submit",     test_submit_fire_forget, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/liveness",   test_in_proc_liveness, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/concurrent", test_concurrent,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/pool_grows", test_pool_grows,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/file_offload", test_file_offload,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/shutdown",   test_shutdown,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = {
	"/m9/blocking", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
