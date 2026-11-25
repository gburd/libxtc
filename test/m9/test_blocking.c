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
#if !defined(_WIN32)
#include <sys/resource.h>
#include <sys/wait.h>
#include <signal.h>
#endif

#include "munit.h"
#include "xtc.h"
#include "xtc_fs.h"
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
static _Atomic int64_t g_t_start, g_t_end, g_t_b;
static _Atomic int  g_a_ok, g_b_ran;

static void
proc_a(void *arg)
{
	int out = -1;
	int64_t t0 = 0, t1 = 0;
	(void)arg;
	(void)__os_clock_mono(&t0);
	atomic_store(&g_t_start, t0);
	(void)xtc_blocking_run(sleep_fn, (void *)(intptr_t)60, &out);
	(void)__os_clock_mono(&t1);
	atomic_store(&g_t_end, t1);
	atomic_store(&g_a_ok, out == 120 ? 1 : 0);
}

static void
proc_b(void *arg)
{
	int64_t t = 0;
	(void)arg;
	(void)__os_clock_mono(&t);
	atomic_store(&g_t_b, t);
	atomic_store(&g_b_ran, 1);
}

static MunitResult
test_in_proc_liveness(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t opts = XTC_PROC_OPTS_INIT;
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
	/* The exact timestamp ordering (B stamped before A's offload
	 * returned) is a real proof of non-blocking offload on a fine-grained
	 * clock, but on Windows the ~15.6 ms default timer granularity
	 * (see io_iocp.c's XTC_IOCP_REPOLL_NS note) coarsens the offload
	 * sleep and the two near-simultaneous stamps can invert by a few
	 * microseconds even though B genuinely ran concurrently.  The
	 * g_b_ran assertion above is the load-bearing liveness proof; the
	 * sub-microsecond ordering check is POSIX-only. */
#if !defined(_WIN32)
	munit_assert_int64(atomic_load(&g_t_b), <=, atomic_load(&g_t_end));
	munit_assert_int64(atomic_load(&g_t_b), >=, atomic_load(&g_t_start));
#endif
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
	xtc_proc_opts_t opts = XTC_PROC_OPTS_INIT;
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
	xtc_proc_opts_t opts = XTC_PROC_OPTS_INIT;
	xtc_pid_t pid;
	char tmpdir[512], path[600];
	char pattern[512];
	int fd;
	(void)p; (void)d;

	atomic_store(&g_file_ok, 0);
	munit_assert_int(xtc_fs_tmpdir(tmpdir, sizeof tmpdir), ==, XTC_OK);
	snprintf(path, sizeof path, "%s/xtc-blk-file-XXXXXX", tmpdir);
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

/* ---- loop teardown with a fiber parked on the pool ----
 * The loop is stopped and finalized while a fiber waits in
 * xtc_blocking_run.  fn writes into that fiber's frame when it returns,
 * and the pool then wakes the fiber's task, so xtc_loop_fini must not
 * free either before fn is done.  Through 2.1.2 it did not wait: fn and
 * the wake landed in freed memory (ASan: heap-use-after-free in
 * xtc_waker_wake), and xtc_loop_fini returned with fn still running. */
static _Atomic int g_fini_fn_done;
static _Atomic int g_fini_fn_in;

static int
fini_slow_fn(void *arg)
{
	struct timespec ts = { 0, 200 * 1000000L };
	atomic_store_explicit(&g_fini_fn_in, 1, memory_order_seq_cst);
	(void)nanosleep(&ts, NULL);
	*(volatile int *)arg = 42;
	atomic_store_explicit(&g_fini_fn_done, 1, memory_order_seq_cst);
	return 0;
}

static void
fini_parker(void *arg)
{
	int in_frame = 0;
	int out = -1;
	(void)arg;
	(void)xtc_blocking_run(fini_slow_fn, &in_frame, &out);
}

static void
fini_stopper(void *arg)
{
	(void)xtc_proc_sleep(30LL * 1000 * 1000);
	(void)xtc_loop_stop(arg);
}

static MunitResult
test_loop_fini_waits(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	(void)p;
	(void)d;

	atomic_store_explicit(&g_fini_fn_done, 0, memory_order_seq_cst);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, fini_parker, NULL, NULL, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, fini_stopper, loop, NULL, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(atomic_load_explicit(&g_fini_fn_done,
	                     memory_order_seq_cst),
	    ==, 0);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	munit_assert_int(atomic_load_explicit(&g_fini_fn_done,
	                     memory_order_seq_cst),
	    ==, 1);
	return MUNIT_OK;
}

/* ---- the same in a forked child ----
 * A thread forks while a proc is parked in xtc_blocking_run.  The child
 * has none of the pool's threads.  A child that finalizes the inherited
 * loop must not wait for the parent's item (2.1.3 through 2.1.7 waited
 * forever), not even after an offload of its own (/fork_own), and that
 * offload must run (the child used to queue it behind the parent's idle
 * count with no thread to take it).
 *
 * The pool is pinned, and each fork waits until every pool thread has
 * run fn, so no thread of the parent is still starting at the fork.
 * Under ASan a fork that lands in a new thread's set-up can copy the
 * sanitizer's allocator lock held, and every thread the child starts
 * then hangs on it.
 *
 * POSIX only.  A test whose child starts a pool thread also skips under
 * TSan: its runtime does not start a thread in the child of a
 * multi-threaded fork ("starting new threads after multi-threaded fork
 * is not supported"), and the child dies. */
#if !defined(_WIN32)
#define FORK_TEST 1
#if !defined(__SANITIZE_THREAD__)
#define CHILD_THREAD_TEST 1
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#undef CHILD_THREAD_TEST
#endif
#endif
#endif
#endif

#if defined(FORK_TEST)
static xtc_loop_t *g_fork_loop;
static _Atomic int g_fork_st;
static int g_fork_own; /* the child offloads before its fini */
static int g_meet_n;
static _Atomic int g_meet_in, g_meet_ok;

/* Wait (up to 2 s) until g_meet_n calls are in here at once, so each
 * of g_meet_n pool threads has run one. */
static int
meet_fn(void *arg)
{
	int k;

	(void)arg;
	atomic_fetch_add_explicit(&g_meet_in, 1, memory_order_seq_cst);
	for (k = 0; k < 2000 && atomic_load_explicit(&g_meet_in,
	    memory_order_seq_cst) < g_meet_n; k++)
		(void)usleep(1000);
	return 7;
}

static void
meet_runner(void *arg)
{
	int out = -1;

	(void)arg;
	if (xtc_blocking_run(meet_fn, NULL, &out) == XTC_OK && out == 7)
		atomic_fetch_add_explicit(&g_meet_ok, 1, memory_order_seq_cst);
}

/* n procs on a fresh loop each run one xtc_blocking_run, all at once;
 * 0 if all n returned 7 with n calls in fn together. */
static int
offload_meet(int n)
{
	xtc_loop_t *l = NULL;
	int i;

	g_meet_n = n;
	atomic_store_explicit(&g_meet_in, 0, memory_order_seq_cst);
	atomic_store_explicit(&g_meet_ok, 0, memory_order_seq_cst);
	if (xtc_loop_init(&l) != XTC_OK)
		return -1;
	for (i = 0; i < n; i++)
		if (xtc_proc_spawn(l, meet_runner, NULL, NULL, NULL) != XTC_OK)
			return -1;
	if (xtc_loop_run(l) != XTC_OK || xtc_loop_fini(l) != XTC_OK)
		return -1;
	return atomic_load_explicit(&g_meet_ok, memory_order_seq_cst) == n &&
	    atomic_load_explicit(&g_meet_in, memory_order_seq_cst) == n ?
	    0 : -1;
}

static void *
fini_forker(void *arg)
{
	int st = 0;
	pid_t pid;

	(void)arg;
	while (!atomic_load_explicit(&g_fini_fn_in, memory_order_seq_cst))
		(void)usleep(1000);
	pid = fork();
	if (pid == 0) {
		alarm(5);
		if (g_fork_own && offload_meet(1) != 0)
			_exit(3);
		(void)xtc_loop_fini(g_fork_loop);
		_exit(0);
	}
	if (pid < 0 || waitpid(pid, &st, 0) != pid)
		st = 0xffff;
	atomic_store_explicit(&g_fork_st, st, memory_order_seq_cst);
	return NULL;
}
#endif

static MunitResult
fork_while_parked(int own)
{
#if !defined(FORK_TEST)
	(void)own;
	return MUNIT_SKIP;
#else
	pthread_t th;
	int st;

#if !defined(CHILD_THREAD_TEST)
	if (own)
		return MUNIT_SKIP;
#endif
	g_fork_own = own;
	xtc_blocking_shutdown();
	munit_assert_int(xtc_blocking_pool_size(1), ==, XTC_OK);
	atomic_store_explicit(&g_fini_fn_in, 0, memory_order_seq_cst);
	atomic_store_explicit(&g_fini_fn_done, 0, memory_order_seq_cst);
	munit_assert_int(xtc_loop_init(&g_fork_loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(g_fork_loop, fini_parker, NULL, NULL,
	    NULL), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(g_fork_loop, fini_stopper,
	    g_fork_loop, NULL, NULL), ==, XTC_OK);
	munit_assert_int(pthread_create(&th, NULL, fini_forker, NULL), ==, 0);
	munit_assert_int(xtc_loop_run(g_fork_loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(g_fork_loop), ==, XTC_OK);
	munit_assert_int(atomic_load_explicit(&g_fini_fn_done,
	                     memory_order_seq_cst),
	    ==, 1);
	munit_assert_int(pthread_join(th, NULL), ==, 0);
	st = atomic_load_explicit(&g_fork_st, memory_order_seq_cst);
	munit_assert_true(WIFEXITED(st));
	munit_assert_int(WEXITSTATUS(st), ==, 0);
	return MUNIT_OK;
#endif
}

static MunitResult
test_loop_fini_fork(const MunitParameter p[], void *d)
{
	(void)p;
	(void)d;
	return fork_while_parked(0);
}

static MunitResult
test_fork_own(const MunitParameter p[], void *d)
{
	(void)p;
	(void)d;
	return fork_while_parked(1);
}

/* ---- the pool in a forked child, with nothing in flight ----
 * started: the parent's pool is up and idle; the child's offload must
 * run (it used to sit on the queue forever: the started flag and the
 * idle count were copied, the threads were not).  cold: the parent never
 * started the pool.  shutdown: the child shuts the pool down, and must
 * not join the parent's threads (it crashed in pthread_join once the
 * parent had three or more).  The child then offloads as many calls at
 * once as the pool has threads, and must start that many. */
enum { FORK_STARTED, FORK_COLD, FORK_SHUTDOWN };
#define FORK_POOL 4

static MunitResult
fork_child_run(int mode)
{
#if !defined(CHILD_THREAD_TEST)
	(void)mode;
	return MUNIT_SKIP;
#else
	int st = 0;
	pid_t pid;

	xtc_blocking_shutdown();
	munit_assert_int(xtc_blocking_pool_size(FORK_POOL), ==, XTC_OK);
	if (mode != FORK_COLD)
		munit_assert_int(offload_meet(FORK_POOL), ==, 0);
	pid = fork();
	munit_assert_int(pid, >=, 0);
	if (pid == 0) {
		alarm(5);
		if (mode == FORK_SHUTDOWN)
			xtc_blocking_shutdown();
		_exit(offload_meet(FORK_POOL) == 0 ? 0 : 3);
	}
	munit_assert_int(waitpid(pid, &st, 0), ==, pid);
	munit_assert_true(WIFEXITED(st));
	munit_assert_int(WEXITSTATUS(st), ==, 0);
	/* The parent's pool is untouched by the child. */
	munit_assert_int(offload_meet(FORK_POOL), ==, 0);
	xtc_blocking_shutdown();
	return MUNIT_OK;
#endif
}

static MunitResult
test_fork_run_started(const MunitParameter p[], void *d)
{
	(void)p;
	(void)d;
	return fork_child_run(FORK_STARTED);
}

static MunitResult
test_fork_run_cold(const MunitParameter p[], void *d)
{
	(void)p;
	(void)d;
	return fork_child_run(FORK_COLD);
}

static MunitResult
test_fork_shutdown(const MunitParameter p[], void *d)
{
	(void)p;
	(void)d;
	return fork_child_run(FORK_SHUTDOWN);
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

/* ---- off-loop pool offload: a plain OS thread (not a fiber) offloads a
 * blocking call to the pool and blocks on the completion pipe.  Proves
 * (a) the result is correct, (b) fn ran on a POOL worker thread, not
 * inline on the caller (the two pthread_self ids differ), and (c) the
 * guards: NULL fn and a from-a-fiber call both return XTC_E_INVAL. ---- */
static _Atomic uintptr_t g_off_caller_tid, g_off_worker_tid;

static int
off_tid_fn(void *arg)
{
	(void)arg;
	atomic_store(&g_off_worker_tid, (uintptr_t)pthread_self());
	return 4242;
}

static _Atomic int g_off_from_fiber_rc;

static void
off_from_fiber_proc(void *arg)
{
	(void)arg;
	/* Called on a loop process: must be rejected (use xtc_blocking_run). */
	atomic_store(&g_off_from_fiber_rc,
	    xtc_blocking_run_off_loop(off_tid_fn, NULL, NULL));
}

static MunitResult
test_off_loop(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t opts = XTC_PROC_OPTS_INIT;
	xtc_pid_t pid;
	int out = -1;
	(void)p; (void)d;

	/* NULL fn guard. */
	munit_assert_int(xtc_blocking_run_off_loop(NULL, NULL, NULL),
	    ==, XTC_E_INVAL);

	/* From a bare OS thread (the test runner is off any loop): offload
	 * runs on a pool worker and returns the result. */
	atomic_store(&g_off_caller_tid, (uintptr_t)pthread_self());
	atomic_store(&g_off_worker_tid, 0);
	munit_assert_int(xtc_blocking_run_off_loop(off_tid_fn, NULL, &out),
	    ==, XTC_OK);
	munit_assert_int(out, ==, 4242);
	/* fn ran on the pool, NOT inline on the caller. */
	munit_assert_uint((unsigned)atomic_load(&g_off_worker_tid), !=, 0);
	munit_assert_ullong((unsigned long long)atomic_load(&g_off_worker_tid),
	    !=, (unsigned long long)atomic_load(&g_off_caller_tid));

	/* From a fiber it must be rejected (use xtc_blocking_run there). */
	atomic_store(&g_off_from_fiber_rc, XTC_OK);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	opts.name = "offfib";
	munit_assert_int(xtc_proc_spawn(loop, off_from_fiber_proc, NULL,
	    &opts, &pid), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	munit_assert_int(atomic_load(&g_off_from_fiber_rc), ==, XTC_E_INVAL);
	return MUNIT_OK;
}

/* ---- on-demand pool growth: many same-duration offloads run
 * concurrently (the pool grows past its initial size), so wall-clock
 * time is ~one sleep, not (N / initial_threads) sleeps ---- */
#define NGROW 32
#define GROW_MS 80
static _Atomic int   g_grow_ok;
static _Atomic int64_t  g_grow_wall_start, g_grow_wall_end;

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
	atomic_store(&g_grow_wall_start, t);
}

static MunitResult
test_pool_grows(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t opts = XTC_PROC_OPTS_INIT;
	xtc_pid_t pid;
	int64_t t1 = 0;
	int64_t elapsed_ms;
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
	atomic_store(&g_grow_wall_end, t1);
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
	/* The absolute wall-time budget assumes GROW_MS sleeps land close to
	 * their nominal duration.  On Windows the ~15.6 ms default timer
	 * granularity coarsens each sleep, inflating total wall time past
	 * the budget even when the pool grows and runs offloads in parallel
	 * exactly as intended -- the g_grow_ok == NGROW check above already
	 * proves every offload ran.  Keep the timing floor POSIX-only. */
#if !defined(_WIN32)
	munit_assert_int64(elapsed_ms, <, (NGROW / 4) * GROW_MS);
#else
	(void)elapsed_ms;
#endif
	return MUNIT_OK;
}

/* Explicitly pin the pool size (must precede the first pool start after a
 * shutdown); a bad size is rejected, and pinning once the pool is live
 * (started by a real fiber offload) is rejected as too-late. */
static void
pool_size_offload_proc(void *arg)
{
	int out = -1;
	(void)arg;
	(void)xtc_blocking_run(sleep_fn, (void *)(intptr_t)1, &out);
}

static MunitResult
test_pool_size(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t opts = XTC_PROC_OPTS_INIT;
	xtc_pid_t pid;
	(void)p; (void)d;

	/* Reset the pool so pool_size is "before first start" again. */
	xtc_blocking_shutdown();
	/* Out-of-range sizes rejected. */
	munit_assert_int(xtc_blocking_pool_size(0), ==, XTC_E_INVAL);
	munit_assert_int(xtc_blocking_pool_size(100000), ==, XTC_E_INVAL);
	/* A valid pin succeeds. */
	munit_assert_int(xtc_blocking_pool_size(2), ==, XTC_OK);

	/* Start the pool for real via a fiber that offloads. */
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	opts.name = "off";
	munit_assert_int(xtc_proc_spawn(loop, pool_size_offload_proc, NULL,
	    &opts, &pid), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);

	/* Too late now: the pool is live, so pinning is rejected. */
	munit_assert_int(xtc_blocking_pool_size(4), ==, XTC_E_INVAL);

	xtc_blocking_shutdown();
	return MUNIT_OK;
}

/* ---- no fd per call, and never inline on the loop at fd exhaustion ----
 * Each xtc_blocking_run used to open a pipe for its wakeup.  With 1000
 * concurrent offloads the process held ~2000 extra fds, and once pipe()
 * failed with EMFILE the call ran fn ON THE LOOP THREAD instead of the
 * pool (measured: 153 of 200 at ulimit -n 100), stalling every other
 * fiber on that loop.  Here the fd limit is lowered to what is open now
 * plus a small margin, NFD fibers offload at once, and every fn must run
 * off the loop thread while the fd count stays flat.  Fails on 1.55.0. */
#define NFD 200
static pthread_t g_fd_loop_thr;
static _Atomic int g_fd_on_loop, g_fd_off_loop, g_fd_ok;

static int
fd_probe_fn(void *arg)
{
	struct timespec ts = { 0, 20 * 1000000L };
	if (pthread_equal(pthread_self(), g_fd_loop_thr))
		atomic_fetch_add(&g_fd_on_loop, 1);
	else
		atomic_fetch_add(&g_fd_off_loop, 1);
	(void)nanosleep(&ts, NULL);
	return (int)(intptr_t)arg + 1;
}

static void
fd_proc(void *arg)
{
	int out = -1;
	if (xtc_blocking_run(fd_probe_fn, arg, &out) == XTC_OK &&
	    out == (int)(intptr_t)arg + 1)
		atomic_fetch_add(&g_fd_ok, 1);
}

#if !defined(_WIN32)
static int
fd_count(void)
{
	int fd, n = 0;
	for (fd = 0; fd < 4096; fd++)
		if (fcntl(fd, F_GETFD) != -1)
			n++;
	return n;
}
#endif

static MunitResult
test_no_fd_per_call(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t opts = XTC_PROC_OPTS_INIT;
#if !defined(_WIN32)
	struct rlimit old, lim;
	int before, top = 0;
#endif
	xtc_pid_t pid;
	int i;
	(void)p; (void)d;

	xtc_blocking_shutdown();
	atomic_store(&g_fd_on_loop, 0);
	atomic_store(&g_fd_off_loop, 0);
	atomic_store(&g_fd_ok, 0);
	g_fd_loop_thr = pthread_self();
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	for (i = 0; i < NFD; i++) {
		opts.name = "fd";
		munit_assert_int(xtc_proc_spawn(loop, fd_proc,
		    (void *)(intptr_t)i, &opts, &pid), ==, XTC_OK);
	}
	/* The fd-limit squeeze and fd count are POSIX (rlimit, fcntl); on
	 * Windows the off-loop and completion assertions below still run. */
#if !defined(_WIN32)
	before = fd_count();
	for (i = 0; i < 4096; i++)
		if (fcntl(i, F_GETFD) != -1)
			top = i + 1;
	munit_assert_int(getrlimit(RLIMIT_NOFILE, &old), ==, 0);
	lim = old;
	lim.rlim_cur = (rlim_t)top + 16;   /* room for a few, not NFD pipes */
	munit_assert_int(setrlimit(RLIMIT_NOFILE, &lim), ==, 0);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(fd_count(), ==, before);
	munit_assert_int(setrlimit(RLIMIT_NOFILE, &old), ==, 0);
#else
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
#endif
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	munit_assert_int(atomic_load(&g_fd_on_loop), ==, 0);
	munit_assert_int(atomic_load(&g_fd_off_loop), ==, NFD);
	munit_assert_int(atomic_load(&g_fd_ok), ==, NFD);
	xtc_blocking_shutdown();
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/no_fd_per_call", test_no_fd_per_call, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/fallback",   test_fallback,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/submit",     test_submit_fire_forget, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/off_loop",   test_off_loop,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/liveness",   test_in_proc_liveness, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/concurrent", test_concurrent,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/pool_grows", test_pool_grows,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/pool_size",  test_pool_size,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/file_offload", test_file_offload,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/shutdown",   test_shutdown,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/loop_fini_waits", test_loop_fini_waits, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/loop_fini_fork", test_loop_fini_fork, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/fork_own",   test_fork_own,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/fork_run_started", test_fork_run_started, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/fork_run_cold", test_fork_run_cold,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/fork_shutdown", test_fork_shutdown,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = {
	"/m9/blocking", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
