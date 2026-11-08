/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m5/test_exec.c -- verifies M5_CLAIMS.md Ex1-Ex4, Sp1-Sp3.
 */

#include <stdint.h>
#include <time.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_blocking.h"
#include "xtc_async.h"
#include "xtc_int.h"
#include "os_thread.h"   /* __os_thread_* for the foreign poker thread */
#include "os_time.h"     /* __os_sleep_ns */
#include "os_cpu.h"      /* __os_ncpus: size the exec to the host */
#include "io_pipe_compat.h"
#include "loop_int.h"     /* Ex5 reads n_timers_pub, the supervisor's input */
#if !defined(_WIN32)
#include <fcntl.h>       /* O_NONBLOCK for Blk6 pipe read end */
#endif
/* [Ex1, Ex2] init/fini round-trip. */
static MunitResult
test_init_fini(const MunitParameter p[], void *d)
{
	xtc_exec_t *e;
	(void)p; (void)d;
	munit_assert_int(xtc_exec_init(&e, 4), ==, XTC_OK);
	munit_assert_int(xtc_exec_n_loops(e), ==, 4);
	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);

	/* Ex1.b: n_loops <= 0 defaults to __os_ncpus(). */
	munit_assert_int(xtc_exec_init(&e, 0), ==, XTC_OK);
	{
		int expect = __os_ncpus();
		if (expect <= 0) expect = 4;
		munit_assert_int(xtc_exec_n_loops(e), ==, expect);
	}
	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);

	munit_assert_int(xtc_exec_init(NULL, 4), ==, XTC_E_INVAL);
	munit_assert_int(xtc_exec_fini(NULL), ==, XTC_E_INVAL);
	return MUNIT_OK;
}

/* [Ex3] run-until-done with no tasks should return immediately. */
static MunitResult
test_run_until_done(const MunitParameter p[], void *d)
{
	xtc_exec_t *e;
	(void)p; (void)d;
	munit_assert_int(xtc_exec_init(&e, 2), ==, XTC_OK);
	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);
	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);
	return MUNIT_OK;
}

/* [Ex4] n_loops sanity. */
static MunitResult
test_n_loops(const MunitParameter p[], void *d)
{
	xtc_exec_t *e;
	(void)p; (void)d;
	munit_assert_int(xtc_exec_init(&e, 1), ==, XTC_OK);
	munit_assert_int(xtc_exec_n_loops(e), ==, 1);
	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);
	munit_assert_int(xtc_exec_init(&e, 7), ==, XTC_OK);
	munit_assert_int(xtc_exec_n_loops(e), ==, 7);
	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);
	return MUNIT_OK;
}

/* [Sp1] basic spawn+run. */
static int sp1_ran;
static int sp1_task(xtc_task_t *self, void *u) {
	(void)self; (void)u;
	__os_atomic_fetch_add_i32(&sp1_ran, 1);
	return XTC_TASK_DONE;
}

static MunitResult
test_spawn(const MunitParameter p[], void *d)
{
	xtc_exec_t *e;
	(void)p; (void)d;
	__os_atomic_store_i32(&sp1_ran, 0);
	munit_assert_int(xtc_exec_init(&e, 4), ==, XTC_OK);
	munit_assert_int(xtc_exec_spawn(e, sp1_task, NULL, NULL), ==, XTC_OK);
	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);
	munit_assert_int(__os_atomic_load_i32(&sp1_ran), ==, 1);
	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);
	return MUNIT_OK;
}

/* [Sp2] spawn_on places on the requested loop. */
static int sp2_observed_id;
static int sp2_task(xtc_task_t *self, void *u) {
	(void)self; (void)u;
	sp2_observed_id = xtc_exec_loop_id();
	return XTC_TASK_DONE;
}

static MunitResult
test_spawn_on(const MunitParameter p[], void *d)
{
	xtc_exec_t *e;
	(void)p; (void)d;
	munit_assert_int(xtc_exec_init(&e, 4), ==, XTC_OK);
	sp2_observed_id = -1;
	munit_assert_int(xtc_exec_spawn_on(e, 2, sp2_task, NULL, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);
	munit_assert_int(sp2_observed_id, ==, 2);
	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);
	return MUNIT_OK;
}

/* [Sp3] N spawned tasks: shared atomic counter sums correctly. */
#define SP3_N 64
static int64_t sp3_counter;
static int sp3_task(xtc_task_t *self, void *u) {
	int64_t inc = (intptr_t)u;
	(void)self;
	(void)__os_atomic_fetch_add_i64(&sp3_counter, inc);
	return XTC_TASK_DONE;
}

static MunitResult
test_n_spawned_sum(const MunitParameter p[], void *d)
{
	xtc_exec_t *e;
	int i;
	int64_t expected = 0;
	(void)p; (void)d;
	munit_assert_int(xtc_exec_init(&e, 4), ==, XTC_OK);
	__os_atomic_store_i64(&sp3_counter, 0);
	for (i = 0; i < SP3_N; i++) {
		expected += i + 1;
		munit_assert_int(xtc_exec_spawn(e, sp3_task,
		    (void *)(intptr_t)(i + 1), NULL), ==, XTC_OK);
	}
	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);
	munit_assert_int64(__os_atomic_load_i64(&sp3_counter), ==, expected);
	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);
	return MUNIT_OK;
}

/* A12: Seastar-style per-shard API. */
static int g_shard_ran[32];            /* one slot per shard -- no contention */
static int g_shard_count_seen;
static int
shard_probe(xtc_task_t *self, void *arg)
{
	int id = xtc_shard_id();
	(void)self; (void)arg;
	if (id >= 0 && id < 32)
		__os_atomic_store_i32(&g_shard_ran[id], 1);
	__os_atomic_store_i32(&g_shard_count_seen, xtc_shard_count());
	return XTC_TASK_DONE;
}

static MunitResult
test_shard_id(const MunitParameter p[], void *d)
{
	xtc_exec_t *e = NULL;
	int i, n = 4;
	(void)p; (void)d;

	/* Off a loop: id -1, count 0. */
	munit_assert_int(xtc_shard_id(), ==, -1);
	munit_assert_int(xtc_shard_count(), ==, 0);

	for (i = 0; i < 32; i++) __os_atomic_store_i32(&g_shard_ran[i], 0);
	__os_atomic_store_i32(&g_shard_count_seen, 0);
	munit_assert_int(xtc_exec_init(&e, n), ==, XTC_OK);
	/* Pin one probe to each shard so every shard id is observed. */
	for (i = 0; i < n; i++)
		munit_assert_int(xtc_exec_spawn_on(e, i, shard_probe, NULL, NULL),
		    ==, XTC_OK);
	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);
	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);

	/* Every shard 0..n-1 ran and reported the full count. */
	for (i = 0; i < n; i++)
		munit_assert_int(__os_atomic_load_i32(&g_shard_ran[i]), ==, 1);
	munit_assert_int(__os_atomic_load_i32(&g_shard_count_seen), ==, n);
	return MUNIT_OK;
}

/* Policy knobs: set/get round-trip and NULL-guards. */
static MunitResult
test_policy_knobs(const MunitParameter p[], void *d)
{
	xtc_exec_t *e = NULL;
	(void)p; (void)d;

	/* NULL exec: setters are no-ops, getters return 0 (never crash). */
	xtc_exec_set_service_mode(NULL, 1);
	xtc_exec_set_eager_rebalance(NULL, 1);
	xtc_exec_set_steal_backoff(NULL, 1);
	munit_assert_int(xtc_exec_get_service_mode(NULL), ==, 0);
	munit_assert_int(xtc_exec_get_eager_rebalance(NULL), ==, 0);
	munit_assert_int(xtc_exec_get_steal_backoff(NULL), ==, 0);

	munit_assert_int(xtc_exec_init(&e, 2), ==, XTC_OK);

	/* Each knob toggles and reads back both states. */
	munit_assert_int(xtc_exec_get_service_mode(e), ==, 0);
	xtc_exec_set_service_mode(e, 1);
	munit_assert_int(xtc_exec_get_service_mode(e), ==, 1);
	xtc_exec_set_service_mode(e, 0);
	munit_assert_int(xtc_exec_get_service_mode(e), ==, 0);

	munit_assert_int(xtc_exec_get_eager_rebalance(e), ==, 0);
	xtc_exec_set_eager_rebalance(e, 1);
	munit_assert_int(xtc_exec_get_eager_rebalance(e), ==, 1);
	xtc_exec_set_eager_rebalance(e, 0);
	munit_assert_int(xtc_exec_get_eager_rebalance(e), ==, 0);

	munit_assert_int(xtc_exec_get_steal_backoff(e), ==, 0);
	xtc_exec_set_steal_backoff(e, 1);
	munit_assert_int(xtc_exec_get_steal_backoff(e), ==, 1);
	xtc_exec_set_steal_backoff(e, 0);
	munit_assert_int(xtc_exec_get_steal_backoff(e), ==, 0);

	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);
	return MUNIT_OK;
}

/* Accessors and error paths: loop, loop_stats, n_loops, spawn/async NULL
 * and out-of-range branches. */
static int noop_task(xtc_task_t *self, void *u)
{ (void)self; (void)u; return XTC_TASK_DONE; }
static intptr_t noop_coro(void *arg) { (void)arg; return 0; }

static MunitResult
test_accessors_errors(const MunitParameter p[], void *d)
{
	xtc_exec_t *e = NULL;
	xtc_loop_stats_t st = XTC_LOOP_STATS_INIT;
	(void)p; (void)d;

	/* NULL exec on every accessor. */
	munit_assert_int(xtc_exec_n_loops(NULL), ==, 0);
	munit_assert_null(xtc_exec_loop(NULL, 0));
	munit_assert_int(xtc_exec_loop_stats(NULL, 0, &st), ==, XTC_E_INVAL);
	munit_assert_int(xtc_exec_stop(NULL), ==, XTC_E_INVAL);
	munit_assert_int(xtc_exec_spawn(NULL, noop_task, NULL, NULL),
	    ==, XTC_E_INVAL);
	munit_assert_int(xtc_exec_spawn_on(NULL, 0, noop_task, NULL, NULL),
	    ==, XTC_E_INVAL);
	munit_assert_int(xtc_exec_async(NULL, noop_coro, NULL, NULL),
	    ==, XTC_E_INVAL);
	munit_assert_int(xtc_exec_async_on(NULL, 0, noop_coro, NULL, NULL),
	    ==, XTC_E_INVAL);

	munit_assert_int(xtc_exec_init(&e, 3), ==, XTC_OK);

	/* loop: valid idx returns non-NULL, out-of-range returns NULL. */
	munit_assert_not_null(xtc_exec_loop(e, 0));
	munit_assert_not_null(xtc_exec_loop(e, 2));
	munit_assert_null(xtc_exec_loop(e, -1));
	munit_assert_null(xtc_exec_loop(e, 3));

	/* loop_stats: NULL out, out-of-range idx, then a valid read. */
	munit_assert_int(xtc_exec_loop_stats(e, 0, NULL), ==, XTC_E_INVAL);
	munit_assert_int(xtc_exec_loop_stats(e, -1, &st), ==, XTC_E_INVAL);
	munit_assert_int(xtc_exec_loop_stats(e, 3, &st), ==, XTC_E_INVAL);
	munit_assert_int(xtc_exec_loop_stats(e, 1, &st), ==, XTC_OK);
	/* Fresh executor: both counters start at zero. */
	munit_assert_uint64(st.tasks_run, ==, 0);
	munit_assert_uint64(st.steals, ==, 0);

	/* spawn_on / async_on out-of-range idx rejected. */
	munit_assert_int(xtc_exec_spawn_on(e, -1, noop_task, NULL, NULL),
	    ==, XTC_E_INVAL);
	munit_assert_int(xtc_exec_spawn_on(e, 3, noop_task, NULL, NULL),
	    ==, XTC_E_INVAL);
	munit_assert_int(xtc_exec_async_on(e, -1, noop_coro, NULL, NULL),
	    ==, XTC_E_INVAL);
	munit_assert_int(xtc_exec_async_on(e, 3, noop_coro, NULL, NULL),
	    ==, XTC_E_INVAL);

	/* Valid default-placement spawn + async, then run drains them. */
	munit_assert_int(xtc_exec_spawn(e, noop_task, NULL, NULL), ==, XTC_OK);
	munit_assert_int(xtc_exec_async(e, noop_coro, NULL, NULL), ==, XTC_OK);
	munit_assert_int(xtc_exec_async_on(e, 1, noop_coro, NULL, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);

	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);
	return MUNIT_OK;
}

/* Proportional-share class accessors (v1.35.0 API): the create contract
 * (shares 1..1000, latency_ns >= 0, non-NULL loop/out) and the read-back
 * accessors incl. their NULL-returns-0 error path.  These are exercised
 * by the DST/PBT sched-shares tests but had no standard-make-check unit
 * coverage. */
static MunitResult
test_exec_class_accessors(const MunitParameter p[], void *d)
{
	xtc_exec_t *e = NULL;
	xtc_loop_t *loop;
	xtc_exec_class_t c = NULL;
	(void)p; (void)d;

	/* Accessors are NULL-safe and return 0. */
	munit_assert_int(xtc_exec_class_shares(NULL), ==, 0);
	munit_assert_int64(xtc_exec_class_latency(NULL), ==, 0);
	munit_assert_int64(xtc_exec_class_runs(NULL), ==, 0);
	munit_assert_int64(xtc_exec_class_vruntime(NULL), ==, 0);

	munit_assert_int(xtc_exec_init(&e, 1), ==, XTC_OK);
	loop = xtc_exec_loop(e, 0);
	munit_assert_not_null(loop);

	/* create validation: NULL args, shares out of 1..1000, negative
	 * latency all rejected. */
	munit_assert_int(xtc_exec_class_create(NULL, 3, 0, &c), ==, XTC_E_INVAL);
	munit_assert_int(xtc_exec_class_create(loop, 3, 0, NULL), ==, XTC_E_INVAL);
	munit_assert_int(xtc_exec_class_create(loop, 0, 0, &c), ==, XTC_E_INVAL);
	munit_assert_int(xtc_exec_class_create(loop, 1001, 0, &c), ==, XTC_E_INVAL);
	munit_assert_int(xtc_exec_class_create(loop, 3, -1, &c), ==, XTC_E_INVAL);

	/* Valid create; accessors read back what was set. */
	munit_assert_int(xtc_exec_class_create(loop, 7, 250000, &c), ==, XTC_OK);
	munit_assert_not_null(c);
	munit_assert_int(xtc_exec_class_shares(c), ==, 7);
	munit_assert_int64(xtc_exec_class_latency(c), ==, 250000);
	munit_assert_int64(xtc_exec_class_runs(c), ==, 0);      /* not run yet */
	munit_assert_int64(xtc_exec_class_vruntime(c), ==, 0);

	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);
	return MUNIT_OK;
}

/*
 * [Blk1] Regression: a fiber parked on a blocking-pool offload must be
 * RESUMED when it runs on an executor loop, not only on a single
 * xtc_loop_run.  The completion is signalled by a pool WORKER (a thread
 * foreign to the fiber's loop) writing the loop-registered completion
 * pipe; while the fiber is parked and the worker sleeps, that loop's
 * exec worker sits idle in its io_poll.  If the idle poll reaps the pipe
 * readiness but drops it (does not dispatch the event), the fiber is
 * never marked runnable and is stranded -- the fiber-resume lost-wakeup
 * reported 2026-08-29 (PostgreSQL sessions-as-fibers: a backend holding
 * WALWriteLock across xtc_aio_fdatasync never resumed, wedging the
 * server).  Each of N procs on N loops offloads a short blocking sleep;
 * the run must quiesce and every offload must have returned its result.
 */
#define BLK1_N 8
static int g_blk1_done;

static int
blk1_sleep_fn(void *arg)
{
	long ms = (long)(intptr_t)arg;
	struct timespec ts;
	ts.tv_sec = ms / 1000;
	ts.tv_nsec = (ms % 1000) * 1000000L;
	(void)nanosleep(&ts, NULL);
	return (int)(ms * 2);
}

static void
blk1_proc(void *arg)
{
	int ms = (int)(intptr_t)arg;
	int out = -1;
	/* xtc_blocking_run from a fiber offloads to the pool and parks on
	 * the completion pipe registered with the loop this fiber is
	 * RUNNING on (which, when stolen, is NOT its home loop). */
	(void)xtc_blocking_run(blk1_sleep_fn, (void *)(intptr_t)ms, &out);
	if (out == ms * 2)
		xtc_atomic_fetch_add_i32(&g_blk1_done, 1, xtc_mo_seq_cst);
}

static MunitResult
test_blocking_resume_on_exec(const MunitParameter p[], void *d)
{
	xtc_exec_t *e;
	xtc_proc_opts_t opts = XTC_PROC_OPTS_INIT;
	xtc_pid_t pid;
	int i;
	(void)p; (void)d;

	xtc_atomic_store_i32(&g_blk1_done, 0, xtc_mo_seq_cst);
	/* More procs than loops, ALL spawned on loop 0, so the idle peer
	 * loops steal them and run them stolen.  A stolen fiber parks its
	 * blocking-offload completion pipe on the STEAL loop (its current
	 * loop), while its n_alive is charged to loop 0 (home).  A steal
	 * loop that holds only stolen-then-parked fibers has n_alive == 0,
	 * so __xtc_loop_step_once returns idle and the worker's bounded
	 * idle poll -- not step's blocking poll -- is what reaps the
	 * completion pipe.  If that idle poll drops the event, the fiber is
	 * stranded: the reported fiber-resume lost-wakeup. */
	munit_assert_int(xtc_exec_init(&e, 4), ==, XTC_OK);
	xtc_exec_set_eager_rebalance(e, 1);   /* force idle peers to steal */
	for (i = 0; i < BLK1_N; i++) {
		opts.name = "blk";
		opts.migratable = 1;   /* stealable: may run+park on a peer loop */
		munit_assert_int(xtc_proc_spawn(xtc_exec_loop(e, 0),
		    blk1_proc, (void *)(intptr_t)(5 + (i % 3)), &opts, &pid),
		    ==, XTC_OK);
	}
	/* Must QUIESCE.  Before the fix this hangs: a stolen fiber parked
	 * on its completion pipe on an n_alive==0 steal loop is never
	 * resumed. */
	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);
	munit_assert_int(xtc_atomic_load_i32(&g_blk1_done, xtc_mo_seq_cst), ==,
	    BLK1_N);
	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);
	return MUNIT_OK;
}

/*
 * [Blk2] Concurrent-commit stress: many migratable fibers, each doing
 * REPEATED blocking-offload parks under high steal churn (eager
 * rebalance), several contending on a shared "flush lock" held ACROSS
 * the park -- the sessions-as-fibers WALWriteLock-holder-across-
 * xtc_aio_fdatasync shape at concurrency.  Reproduces the reported
 * 2026-08-29 concurrent-commit strand if any poll/dispatch site drops a
 * completion, or the resume races the steal/rebalance transitions.  The
 * run must QUIESCE and every fiber must complete all its iterations.
 */
#define BLK2_FIBERS 64
#define BLK2_ITERS  200
static int g_blk2_done;
static int g_blk2_lock; /* 0/1 spin "flush lock" */

static int
blk2_flush_fn(void *arg)
{
	/* A short blocking "flush" on the pool worker.  The offload path
	 * (xtc_blocking_run) is exactly what xtc_aio_fdatasync uses when the
	 * native engine is absent/declines, and is where the reported
	 * concurrent-commit strand lived; exercising it directly keeps this
	 * test portable (no raw open()/O_CREAT/unlink, which MSVC's munit
	 * subset rejects). */
	int ms = (int)(intptr_t)arg;
	struct timespec ts;
	ts.tv_sec = 0;
	ts.tv_nsec = ms * 100000L;   /* ms*0.1ms */
	(void)nanosleep(&ts, NULL);
	return ms;
}

static void
blk2_proc(void *arg)
{
	int iters = (int)(intptr_t)arg;
	int k;
	for (k = 0; k < iters; k++) {
		int held = 0;
		int out = -1;
		/* A subset of iterations grab the shared flush lock and hold
		 * it ACROSS the offload park (the WALWriteLock-holder-across-
		 * fsync shape): if the holder is stranded post-park -- e.g. a
		 * spurious wake resumes it before its completion under steal
		 * churn -- every fiber spinning for the lock wedges. */
		if ((k & 3) == 0) {
			int spins = 0;
			while (xtc_atomic_exchange_i32(&g_blk2_lock, 1,
			           xtc_mo_acquire) != 0) {
				xtc_yield();
				if (++spins > 100000000) { held = -1; break; }
			}
			if (held == 0) held = 1;
		}
		if (held < 0) break;   /* wedge guard */
		/* Offload park: xtc_blocking_run parks the fiber on its
		 * completion pipe -- the path where a spurious wake must NOT
		 * make the fiber return before its work truly completes. */
		(void)xtc_blocking_run(blk2_flush_fn,
		    (void *)(intptr_t)(1 + (k & 3)), &out);
		if (held == 1)
			xtc_atomic_store_i32(&g_blk2_lock, 0, xtc_mo_release);
	}
	xtc_atomic_fetch_add_i32(&g_blk2_done, 1, xtc_mo_seq_cst);
}

static MunitResult
test_concurrent_commit_resume(const MunitParameter p[], void *d)
{
	xtc_exec_t *e;
	xtc_proc_opts_t opts = XTC_PROC_OPTS_INIT;
	xtc_pid_t pid;
	int i;
	(void)p; (void)d;

	xtc_atomic_store_i32(&g_blk2_done, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_blk2_lock, 0, xtc_mo_seq_cst);
	munit_assert_int(xtc_exec_init(&e, 12), ==, XTC_OK);
	xtc_exec_set_eager_rebalance(e, 1);
	for (i = 0; i < BLK2_FIBERS; i++) {
		opts.name = "blk2";
		opts.migratable = 1;
		/* Spawn across a couple loops so there is both homing and
		 * heavy stealing. */
		munit_assert_int(xtc_proc_spawn(xtc_exec_loop(e, i % 3),
		    blk2_proc, (void *)(intptr_t)BLK2_ITERS, &opts, &pid),
		    ==, XTC_OK);
	}
	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);
	munit_assert_int(xtc_atomic_load_i32(&g_blk2_done, xtc_mo_seq_cst), ==,
	    BLK2_FIBERS);
	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);
	return MUNIT_OK;
}

/*
 * [Blk3] Cross-loop fd-unregister under migration.  Migratable fibers
 * park via xtc_proc_wait_fd on their own pipe fd; a FOREIGN OS thread
 * pokes them with xtc_proc_wake (a NON-fd wake -- it does not satisfy
 * the fd, so on resume park_fd is still live) while eager rebalance
 * work-steals them across loops.  A woken-then-stolen fiber therefore
 * runs its xtc_proc_wait_fd cleanup (xtc_io_del_fd) on an OS thread that
 * is NOT the owner of the io it registered on -- the cross-loop
 * io->fds/SQ-ring data race the PG team hit (TSan, 2026-08-30).  With
 * the fix the unregister is deferred to the owning loop, so the run
 * must QUIESCE and every fiber completes.  (On io_uring this exercises
 * the deferred-unregister path; on other backends the safe
 * passthrough.)
 */
#define BLK3_FIBERS 32
#define BLK3_ITERS  40
static int g_blk3_done;
static xtc_pid_t   g_blk3_pids[BLK3_FIBERS];
static int         g_blk3_rfd[BLK3_FIBERS];
static int         g_blk3_wfd[BLK3_FIBERS];
static int g_blk3_ready;
static int g_blk3_stop;

static void *
blk3_poker(void *arg)
{
	(void)arg;
	while (
	    xtc_atomic_load_i32(&g_blk3_ready, xtc_mo_seq_cst) < BLK3_FIBERS &&
	    !xtc_atomic_load_i32(&g_blk3_stop, xtc_mo_seq_cst))
		(void)__os_sleep_ns(100 * 1000LL);
	while (!xtc_atomic_load_i32(&g_blk3_stop, xtc_mo_seq_cst)) {
		int i;
		for (i = 0; i < BLK3_FIBERS; i++)
			(void)xtc_proc_wake(g_blk3_pids[i]);
		(void)__os_sleep_ns(50 * 1000LL);
	}
	return NULL;
}

static void
blk3_proc(void *arg)
{
	int idx = (int)(intptr_t)arg;
	int k;
	g_blk3_pids[idx] = xtc_self();
	xtc_atomic_fetch_add_i32(&g_blk3_ready, 1, xtc_mo_seq_cst);
	for (k = 0; k < BLK3_ITERS; k++) {
		uint32_t revents = 0;
		/* Short-timeout fd park.  A poker xtc_proc_wake resumes us
		 * without the fd being ready (park_fd stays live); under
		 * eager rebalance we may resume on a peer loop -> the
		 * cleanup del_fd is cross-loop. */
		(void)xtc_proc_wait_fd(g_blk3_rfd[idx], XTC_IO_READABLE,
		    2 * 1000 * 1000, &revents);
		if ((k & 7) == 0) {
			char b = 'x';
			char sink[8];
			(void)xtc_test_pipe_write(g_blk3_wfd[idx], &b, 1);
			revents = 0;
			(void)xtc_proc_wait_fd(g_blk3_rfd[idx],
			    XTC_IO_READABLE, -1, &revents);
			(void)xtc_test_pipe_read(g_blk3_rfd[idx], sink,
			    sizeof sink);
		}
	}
	xtc_atomic_fetch_add_i32(&g_blk3_done, 1, xtc_mo_seq_cst);
}

static MunitResult
test_cross_loop_del_fd(const MunitParameter p[], void *d)
{
	xtc_exec_t *e;
	xtc_proc_opts_t opts = XTC_PROC_OPTS_INIT;
	xtc_pid_t pid;
	__os_thread_t poker = {0};
	int i;
	(void)p; (void)d;

	xtc_atomic_store_i32(&g_blk3_done, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_blk3_ready, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_blk3_stop, 0, xtc_mo_seq_cst);
	for (i = 0; i < BLK3_FIBERS; i++) {
		g_blk3_rfd[i] = g_blk3_wfd[i] = -1;
		if (xtc_test_make_pipe(&g_blk3_rfd[i], &g_blk3_wfd[i]) != 0)
			return MUNIT_SKIP;   /* no pipes available */
	}
	munit_assert_int(xtc_exec_init(&e, 8), ==, XTC_OK);
	xtc_exec_set_eager_rebalance(e, 1);
	munit_assert_int(__os_thread_create(&poker, blk3_poker, NULL),
	    ==, XTC_OK);
	for (i = 0; i < BLK3_FIBERS; i++) {
		opts.name = "blk3";
		opts.migratable = 1;
		munit_assert_int(xtc_proc_spawn(xtc_exec_loop(e, 0),
		    blk3_proc, (void *)(intptr_t)i, &opts, &pid), ==, XTC_OK);
	}
	/* Must QUIESCE: without the fix a woken-then-stolen fiber corrupts
	 * the peer io's fd registry and a fiber is stranded. */
	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);
	xtc_atomic_store_i32(&g_blk3_stop, 1, xtc_mo_seq_cst);
	munit_assert_int(__os_thread_join(&poker, NULL), ==, XTC_OK);
	munit_assert_int(xtc_atomic_load_i32(&g_blk3_done, xtc_mo_seq_cst), ==,
	    BLK3_FIBERS);
	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);
	for (i = 0; i < BLK3_FIBERS; i++)
		xtc_test_close_pipe(g_blk3_rfd[i], g_blk3_wfd[i]);
	return MUNIT_OK;
}

/*
 * [Blk4] Cross-loop task->state + timer-heap stress.  Migratable fibers,
 * all spawned on ONE loop so peers steal them, each looping on a short
 * xtc_recv timeout (which arms a park timer) while a FOREIGN OS thread
 * hammers xtc_proc_wake on all of them.  This drives (a) a stale
 * XTC_INB_WAKE draining on a fiber's old loop while it is dispatched on
 * the loop that stole it -- the cross-loop task->state race now closed
 * by the atomic CAS in the inbox-WAKE handler; and (b)
 * xtc_task_park_on_timer arming a timer on the RUNNING loop (not the
 * home loop) -- the cross-loop timer-heap race now closed by using
 * __xtc_current_loop.  Both were TSan-found 2026-08-30.  The run must
 * QUIESCE and every fiber complete all iterations.
 */
#define BLK4_FIBERS 24
#define BLK4_ITERS  60
static int g_blk4_done;
static xtc_pid_t   g_blk4_pids[BLK4_FIBERS];
static int g_blk4_ready;
static int g_blk4_stop;

static void *
blk4_poker(void *arg)
{
	(void)arg;
	while (
	    xtc_atomic_load_i32(&g_blk4_ready, xtc_mo_seq_cst) < BLK4_FIBERS &&
	    !xtc_atomic_load_i32(&g_blk4_stop, xtc_mo_seq_cst))
		(void)__os_sleep_ns(100 * 1000LL);
	while (!xtc_atomic_load_i32(&g_blk4_stop, xtc_mo_seq_cst)) {
		int i;
		for (i = 0; i < BLK4_FIBERS; i++)
			(void)xtc_proc_wake(g_blk4_pids[i]);
		/* Throttle: a tight no-sleep wake loop livelocks the fibers
		 * (they never complete a recv-timeout iteration because they
		 * are re-woken instantly), which hangs on the coarse-timer
		 * Windows runner.  A short sleep still creates the
		 * stale-WAKE-during-dispatch window this test targets. */
		(void)__os_sleep_ns(50 * 1000LL);
	}
	return NULL;
}

static void
blk4_proc(void *arg)
{
	int idx = (int)(intptr_t)arg;
	int k;
	g_blk4_pids[idx] = xtc_self();
	xtc_atomic_fetch_add_i32(&g_blk4_ready, 1, xtc_mo_seq_cst);
	for (k = 0; k < BLK4_ITERS; k++) {
		void *msg = NULL;
		size_t len = 0;
		/* recv-timeout: arms a park timer on the running loop; a poker
		 * wake resumes us (mailbox empty -> XTC_E_AGAIN) and we re-park,
		 * re-arming on whatever loop currently runs us. */
		if (xtc_recv(&msg, &len, 1 * 1000 * 1000 /* 1ms */) == XTC_OK)
			xtc_free(msg);
	}
	xtc_atomic_fetch_add_i32(&g_blk4_done, 1, xtc_mo_seq_cst);
}

static MunitResult
test_cross_loop_state_timer(const MunitParameter p[], void *d)
{
	xtc_exec_t *e;
	xtc_proc_opts_t opts = XTC_PROC_OPTS_INIT;
	xtc_pid_t pid;
	__os_thread_t poker = {0};
	int i;
	(void)p; (void)d;

	xtc_atomic_store_i32(&g_blk4_done, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_blk4_ready, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_blk4_stop, 0, xtc_mo_seq_cst);
	munit_assert_int(xtc_exec_init(&e, 8), ==, XTC_OK);
	xtc_exec_set_eager_rebalance(e, 1);
	munit_assert_int(__os_thread_create(&poker, blk4_poker, NULL),
	    ==, XTC_OK);
	for (i = 0; i < BLK4_FIBERS; i++) {
		opts.name = "blk4";
		opts.migratable = 1;
		munit_assert_int(xtc_proc_spawn(xtc_exec_loop(e, 0),
		    blk4_proc, (void *)(intptr_t)i, &opts, &pid), ==, XTC_OK);
	}
	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);
	xtc_atomic_store_i32(&g_blk4_stop, 1, xtc_mo_seq_cst);
	munit_assert_int(__os_thread_join(&poker, NULL), ==, XTC_OK);
	munit_assert_int(xtc_atomic_load_i32(&g_blk4_done, xtc_mo_seq_cst), ==,
	    BLK4_FIBERS);
	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);
	return MUNIT_OK;
}

/*
 * [Blk5] Migratable-fiber timer-park resume across migration (the
 * fsync/WAL-commit fiber-resume strand, reported 2026-09-01).  N
 * migratable fibers, all spawned on ONE loop so peers steal them,
 * contend on a shared spin lock (PG's WALWriteLock shape); the holder
 * sleeps briefly (xtc_proc_sleep) while holding it, waiters park in
 * xtc_proc_sleep.  A migratable fiber can be resumed (work-stolen /
 * spuriously) BEFORE its park timer fires, so on the next sleep
 * iteration it would hit park_on_timer's "already parked" reject and
 * re-park trusting the stale timer -- which is armed on a loop it has
 * since left.  When that stale timer fires it wakes the fiber onto the
 * OLD (now-idle, asleep) loop and the wake is stranded, so the lock
 * holder never wakes to release: everyone piles up behind it and the
 * run never quiesces.  The fix (cancel the stale timer before every
 * re-arm, matching the sync.c/future.c/lock_mgr.c timed-wait
 * discipline) makes the fire always wake the fiber on the loop it is
 * actually on.  Without the fix this hangs ~100%%; with it, quiesces
 * and every fiber completes.
 */
#define BLK5_FIBERS 16
#define BLK5_ITERS  20
#if !defined(_WIN32)
static int g_blk5_done;
static int g_blk5_lock; /* 0/1 shared "WALWriteLock" */

static void
blk5_proc(void *arg)
{
	int idx = (int)(intptr_t)arg;
	int k;
	(void)idx;
	for (k = 0; k < BLK5_ITERS; k++) {
		int spins = 0;
		/* Acquire the shared lock, parking (not busy-yield) so waiters
		 * are NOT runnable -- this lets loops go genuinely idle, the
		 * state the stranded-wake needs. */
		while (xtc_atomic_exchange_i32(&g_blk5_lock, 1,
		           xtc_mo_acquire) != 0) {
			(void)xtc_proc_sleep(50 * 1000LL);
			if (++spins > 4000000) goto out;   /* defensive */
		}
		/* Hold the lock across a short sleep -- the holder's OWN park
		 * timer is the one that must resume it after migration. */
		(void)xtc_proc_sleep(20 * 1000LL);
		xtc_atomic_store_i32(&g_blk5_lock, 0, xtc_mo_release);
	}
out:
	xtc_atomic_fetch_add_i32(&g_blk5_done, 1, xtc_mo_seq_cst);
}
#endif /* !_WIN32 */

static MunitResult
test_migratable_timer_resume(const MunitParameter p[], void *d)
{
	(void)p; (void)d;

#if defined(_WIN32)
	/* Windows' coarse timer floor (~15 ms) turns the many serialized
	 * lock handoffs -- each gated on a sub-ms xtc_proc_sleep that rounds
	 * up -- into a multi-minute run, which stalls the MSVC munit runner.
	 * The fix path (xtc_proc_sleep's timer re-arm) is exercised on
	 * Windows by m8/test_proc; the migratable-fiber strand this case
	 * targets is platform-independent and is caught on Linux/macOS. */
	return MUNIT_SKIP;
#else
	xtc_exec_t *e;
	xtc_proc_opts_t opts = XTC_PROC_OPTS_INIT;
	xtc_pid_t pid;
	int i, nloops;
	xtc_atomic_store_i32(&g_blk5_done, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_blk5_lock, 0, xtc_mo_seq_cst);
	/*
	 * Size the executor to the CPUs actually available, capped at 8.
	 *
	 * The 16 fibers here serialize on one lock, and a waiter that loses
	 * the CAS re-parks on a 50us timer.  With 8 loops pinned onto a
	 * 1-2 core CI VM (the FreeBSD vmactions runner) those loops
	 * oversubscribe the host so badly that the handoff chain cannot
	 * drain inside the runner's budget -- measured on real FreeBSD under
	 * `cpuset -l 0,1`: hangs 2/4 on the PRE-FIX tree and 1/3 after, i.e.
	 * a pre-existing starvation flake, not a correctness regression.  It
	 * only became visible once Blk2 stopped hanging first.
	 *
	 * The strand this case targets (a migratable fiber resumed by its OWN
	 * park timer after migration) needs MIGRATION, not eight loops: two
	 * loops already give work-stealing, and on a normal 8-core box this
	 * is unchanged.  Same reasoning as the _WIN32 skip above -- match the
	 * platform rather than assume the developer's machine.
	 */
	nloops = __os_ncpus();
	if (nloops > 8) nloops = 8;
	if (nloops < 2) nloops = 2;
	munit_assert_int(xtc_exec_init(&e, nloops), ==, XTC_OK);
	xtc_exec_set_eager_rebalance(e, 1);
	for (i = 0; i < BLK5_FIBERS; i++) {
		opts.name = "blk5";
		opts.migratable = 1;
		munit_assert_int(xtc_proc_spawn(xtc_exec_loop(e, 0),
		    blk5_proc, (void *)(intptr_t)i, &opts, &pid), ==, XTC_OK);
	}
	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);
	munit_assert_int(xtc_atomic_load_i32(&g_blk5_done, xtc_mo_seq_cst), ==,
	    BLK5_FIBERS);
	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);
	return MUNIT_OK;
#endif
}

/*
 * [Blk6] Migratable-fiber xtc_proc_wait_fd(finite timeout) resume across
 * migration (the fd-park sibling of Blk5; reported 2026-09-01).  N
 * migratable fibers, all spawned on ONE loop so peers steal them, each
 * parks in xtc_proc_wait_fd on the read end of its OWN pipe (never
 * written) with a finite timeout, contending on a shared lock across the
 * wait (PG's WALWriteLock / ProcSleep shape).  xtc_proc_wait_fd arms
 * both an fd registration and a timeout timer on the loop the fiber is
 * RUNNING on (wl = __xtc_current_loop); a HOME loop that spawned the
 * work but had every fiber work-stolen keeps n_alive > 0 yet may not
 * poll its own io for a long window -- so its io->owner_set stayed 0,
 * and the inline-vs-defer decision let EVERY foreign fiber's cleanup del
 * that home io's fd INLINE.  Two such cleanups racing the same home io's
 * single-owner fds list corrupted it into a cycle (xtc_io_del_fd's
 * unlink loop spins forever -> a stranded fiber holding the lock) and
 * could double-link a uf (fini double-free).  The fix records the io's
 * owner thread eagerly at worker startup (__xtc_io_set_owner), so a
 * foreign cleanup always DEFERS the unregister to the owning thread.
 * Without the fix this hangs (or aborts on the double-free)
 * intermittently; with it, quiesces and every fiber completes.
 */
#define BLK6_FIBERS 16
#define BLK6_ITERS  20
#if !defined(_WIN32)
static int g_blk6_done;
static int g_blk6_lock;

static int
blk6_wait_timeout(int fd)
{
	uint32_t rev = 0;
	/* Never written -> must return via timeout.  Non-blocking read end
	 * (set by the caller) so a readable-drain never blocks the OS
	 * thread. */
	(void)xtc_proc_wait_fd(fd, XTC_IO_READABLE, 2 * 1000 * 1000LL, &rev);
	if (rev & XTC_IO_READABLE) {
		char drain[64];
		while (xtc_test_pipe_read(fd, drain, (int)sizeof drain) > 0) { }
	}
	return 1;
}

static void
blk6_proc(void *arg)
{
	int idx = (int)(intptr_t)arg;
	int r = -1, w = -1;
	int k;
	(void)idx;
	if (xtc_test_make_pipe(&r, &w) != 0) {
		xtc_atomic_fetch_add_i32(&g_blk6_done, 1, xtc_mo_seq_cst);
		return;
	}
	(void)fcntl(r, F_SETFL, O_NONBLOCK);
	for (k = 0; k < BLK6_ITERS; k++) {
		int held = 0;
		if ((k & 1) == 0) {
			int spins = 0;
			while (xtc_atomic_exchange_i32(&g_blk6_lock, 1,
			           xtc_mo_acquire) != 0) {
				(void)blk6_wait_timeout(r);
				if (++spins > 4000000) goto out;
			}
			held = 1;
		}
		/* The wait that MUST time out; the fiber's fd+timer are armed
		 * on whatever loop currently runs it. */
		(void)blk6_wait_timeout(r);
		if (held)
			xtc_atomic_store_i32(&g_blk6_lock, 0, xtc_mo_release);
	}
out:
	xtc_test_close_pipe(r, w);
	xtc_atomic_fetch_add_i32(&g_blk6_done, 1, xtc_mo_seq_cst);
}
#endif /* !_WIN32 */

static MunitResult
test_migratable_waitfd_resume(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
#if defined(_WIN32)
	/* Same coarse-timer rationale as Blk5; the single-owner io->fds
	 * list this targets is the io_uring backend's, not the Windows IOCP
	 * registry.  Caught on Linux/macOS. */
	return MUNIT_SKIP;
#else
	xtc_exec_t *e;
	xtc_proc_opts_t opts = XTC_PROC_OPTS_INIT;
	xtc_pid_t pid;
	int i;
	xtc_atomic_store_i32(&g_blk6_done, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_blk6_lock, 0, xtc_mo_seq_cst);
	munit_assert_int(xtc_exec_init(&e, 8), ==, XTC_OK);
	xtc_exec_set_eager_rebalance(e, 1);
	for (i = 0; i < BLK6_FIBERS; i++) {
		opts.name = "blk6";
		opts.migratable = 1;
		munit_assert_int(xtc_proc_spawn(xtc_exec_loop(e, 0),
		    blk6_proc, (void *)(intptr_t)i, &opts, &pid), ==, XTC_OK);
	}
	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);
	munit_assert_int(xtc_atomic_load_i32(&g_blk6_done, xtc_mo_seq_cst), ==,
	    BLK6_FIBERS);
	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);
	return MUNIT_OK;
#endif
}

/*
 * [Blk7] Killing a fiber parked in xtc_blocking_run must not let the pool
 * worker outlive the caller's frame.
 *
 * xtc_blocking_run queues a STACK-OWNED `struct blk_work` and parks on a
 * completion pipe via xtc_proc_wait_fd -- a cancellation point.  With that
 * wait unmasked, a kill delivered during it unwound the OWNING FRAME while
 * the worker was still running and still writing into that work item (and
 * still reading the caller's arg).  MEASURED with ASan on epoll (kill 50ms
 * into a 400ms offload): the dead fiber's stack was reused, the worker read
 * a garbage w->detached, took the fire-and-forget branch, and called
 * __os_free on a STACK address -- "attempting free on address which was
 * not malloc()-ed" in blk_worker.  That is heap corruption, not a leak.
 *
 * The property asserted here is the ordering that makes it safe: the work
 * function must have COMPLETED before the caller's at-exit hook runs (the
 * first thing that observes the frame being torn down).  Pre-fix the hook
 * ran with the work still in flight.
 *
 * This deliberately does NOT assert that the kill is instantaneous: the
 * work runs on a pool thread that cannot be interrupted, so the wait is
 * bounded by the work, and the supervisor is told so -- xtc_exit_pid_-
 * deadline reports XTC_KILL_DEFERRED rather than silently swallowing the
 * kill, which is the documented contract for a masked region.
 */
static int g_blk7_work_started, g_blk7_work_finished;
static int g_blk7_finished_at_hook, g_blk7_hook_ran;
static int g_blk7_kill_status, g_blk7_run_returned;
static xtc_pid_t g_blk7_victim;

static int
blk7_slow_fn(void *arg)
{
	(void)arg;
	xtc_atomic_store_i32(&g_blk7_work_started, 1, xtc_mo_seq_cst);
	/* Long enough that the kill provably lands mid-flight. */
	__os_sleep_ns(300LL * 1000 * 1000);
	xtc_atomic_store_i32(&g_blk7_work_finished, 1, xtc_mo_seq_cst);
	return 99;
}

static void
blk7_at_exit(void *arg)
{
	(void)arg;
	/* The frame is being torn down NOW: the worker must already be done
	 * with it.  Pre-fix this sampled 0. */
	xtc_atomic_store_i32(&g_blk7_finished_at_hook,
	    xtc_atomic_load_i32(&g_blk7_work_finished, xtc_mo_seq_cst),
	    xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_blk7_hook_ran, 1, xtc_mo_seq_cst);
}

static void
blk7_victim_proc(void *arg)
{
	int out = -1;
	(void)arg;
	munit_assert_int(xtc_proc_at_exit(blk7_at_exit, NULL), ==, XTC_OK);
	(void)xtc_blocking_run(blk7_slow_fn, NULL, &out);
	xtc_atomic_store_i32(&g_blk7_run_returned, 1, xtc_mo_seq_cst);
}

static void
blk7_killer_proc(void *arg)
{
	int st = -1, spins = 0;
	(void)arg;
	while (!xtc_atomic_load_i32(&g_blk7_work_started, xtc_mo_seq_cst) &&
	    spins++ < 20000)
		(void)xtc_proc_sleep(1000LL * 1000);
	/* Kill with the worker provably mid-flight. */
	(void)xtc_proc_sleep(40LL * 1000 * 1000);
	munit_assert_int(xtc_atomic_load_i32(&g_blk7_work_finished,
	                     xtc_mo_seq_cst),
	    ==, 0);
	munit_assert_int(xtc_exit_pid_deadline(g_blk7_victim, 9,
	    3000LL * 1000 * 1000, &st), ==, XTC_OK);
	xtc_atomic_store_i32(&g_blk7_kill_status, st, xtc_mo_seq_cst);
}

static MunitResult
test_blocking_kill_no_frame_escape(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t opts = XTC_PROC_OPTS_INIT;
	xtc_pid_t killer;
	(void)p; (void)d;

	xtc_atomic_store_i32(&g_blk7_work_started, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_blk7_work_finished, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_blk7_finished_at_hook, -1, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_blk7_hook_ran, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_blk7_kill_status, -1, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_blk7_run_returned, 0, xtc_mo_seq_cst);

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	opts.name = "blk7-victim";
	munit_assert_int(xtc_proc_spawn(loop, blk7_victim_proc, NULL, &opts,
	    &g_blk7_victim), ==, XTC_OK);
	opts.name = "blk7-killer";
	munit_assert_int(xtc_proc_spawn(loop, blk7_killer_proc, NULL, &opts,
	    &killer), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	munit_assert_int(xtc_atomic_load_i32(&g_blk7_hook_ran, xtc_mo_seq_cst),
	    ==, 1);
	/* THE REGRESSION: the offload completed BEFORE the caller's frame
	 * started being torn down.  Pre-fix: 0 (worker still running). */
	munit_assert_int(xtc_atomic_load_i32(&g_blk7_finished_at_hook,
	                     xtc_mo_seq_cst),
	    ==, 1);
	munit_assert_int(xtc_atomic_load_i32(&g_blk7_work_finished,
	                     xtc_mo_seq_cst),
	    ==, 1);
	/* The kill was still honored -- the fiber died rather than
	 * completing xtc_blocking_run normally. */
	munit_assert_int(xtc_atomic_load_i32(&g_blk7_run_returned,
	                     xtc_mo_seq_cst),
	    ==, 0);
	/* And a supervisor is TOLD the kill was deferred by the mask, so a
	 * wedged offload stays diagnosable (man xtc_proc.3). */
	munit_assert_int(xtc_atomic_load_i32(&g_blk7_kill_status,
	                     xtc_mo_seq_cst),
	    ==, XTC_KILL_DEFERRED);

	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/*
 * [Blk8] A migratable fiber's identity survives a cross-thread resume.
 *
 * clang computes a thread-local's address once (%fs:0 + offset) and may
 * keep it in a register/spill slot across an opaque call -- including
 * xtc_yield(), across which a migratable fiber can move to ANOTHER OS
 * thread.  The post-yield `__current_proc = self` in xtc_proc_sleep /
 * xtc_proc_wait_fd / xtc_recv then stored into the PRE-migration
 * thread's slot, overwriting the identity of whatever proc was running
 * there.  That victim saw xtc_self() name the migrated proc, saved that
 * value at its next yield, and on resume the kill hook read the other
 * proc's kill_pending after it had exited and been freed -- the Blk2
 * heap-use-after-free (clang ASan, ~1 in 3 runs; gcc does not cache the
 * address, which is why gcc CI never saw it).  Here every fiber checks,
 * between parks, that xtc_self() still names itself; no sanitizer is
 * needed to see the clobber.
 */
#define BLK8_FIBERS 32
#define BLK8_ITERS  200
#define BLK8_CHECKS 2000
#if !defined(_WIN32)
static int g_blk8_done, g_blk8_wrong;

static void
blk8_proc(void *arg)
{
	xtc_pid_t me = xtc_self();
	int k, j;
	(void)arg;
	for (k = 0; k < BLK8_ITERS; k++) {
		(void)xtc_proc_sleep(((k & 3) + 1) * 1000LL);
		for (j = 0; j < BLK8_CHECKS; j++) {
			if (!xtc_pid_eq(xtc_self(), me)) {
				xtc_atomic_fetch_add_i32(&g_blk8_wrong, 1,
				    xtc_mo_seq_cst);
				break;
			}
		}
	}
	xtc_atomic_fetch_add_i32(&g_blk8_done, 1, xtc_mo_seq_cst);
}
#endif /* !_WIN32 */

static MunitResult
test_migratable_self_identity(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
#if defined(_WIN32)
	/* Same coarse-timer reason as Blk5; the identity fix is in proc.c
	 * and is platform-independent. */
	return MUNIT_SKIP;
#else
	xtc_exec_t *e;
	xtc_proc_opts_t opts = XTC_PROC_OPTS_INIT;
	xtc_pid_t pid;
	int i, nloops;
	xtc_atomic_store_i32(&g_blk8_done, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_blk8_wrong, 0, xtc_mo_seq_cst);
	nloops = __os_ncpus();
	if (nloops > 8) nloops = 8;
	if (nloops < 2) nloops = 2;
	munit_assert_int(xtc_exec_init(&e, nloops), ==, XTC_OK);
	xtc_exec_set_eager_rebalance(e, 1);
	for (i = 0; i < BLK8_FIBERS; i++) {
		opts.name = "blk8";
		opts.migratable = 1;
		munit_assert_int(xtc_proc_spawn(xtc_exec_loop(e, 0),
		    blk8_proc, NULL, &opts, &pid), ==, XTC_OK);
	}
	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);
	munit_assert_int(xtc_atomic_load_i32(&g_blk8_done, xtc_mo_seq_cst), ==,
	    BLK8_FIBERS);
	munit_assert_int(xtc_atomic_load_i32(&g_blk8_wrong, xtc_mo_seq_cst), ==,
	    0);
	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);
	return MUNIT_OK;
#endif
}

/*
 * [Ex5] xtc_exec_run returns once the last task is done, even when the
 * loop's timer heap emptied in the step that then blocked.
 *
 * The supervisor's idle check reads each loop's published timer count
 * (n_timers_pub), and the owner published it only after a step
 * RETURNED.  A step that dropped the last timer (here a cancelled one,
 * popped by the next-deadline peek) and then blocked in its poll --
 * with no timeout, because a task was still alive -- left the count
 * stale while it slept.  When that task finished without waking this
 * loop (on a peer, or by an exit the poll does not see), every loop
 * read idle except for the stale count: xtc_exec_run never returned.
 * Found as a 1-in-1,000 hang of /Blk3_cross_loop_del_fd under riscv64
 * emulation, where the last fiber finishes on a peer loop.
 *
 * The hang itself needs the last fiber to finish on a peer in the few
 * instructions between this loop's n_alive read and its steal re-check,
 * so this test checks the precondition deterministically instead: one
 * loop, a proc that arms a 10 s timer and parks on a pipe; a foreign
 * thread cancels the timer (a mark only, off the owner) and wakes the
 * proc, so the owner's next step pops the cancelled node and blocks.
 * While it is blocked, the published count must be 0 -- it was 1.
 */
static int          g_ex5_rfd = -1, g_ex5_wfd = -1;
static xtc_timer_t *g_ex5_timer;
static xtc_pid_t    g_ex5_pid;
static _Atomic int  g_ex5_parked;
static _Atomic int  g_ex5_returned;
static _Atomic int  g_ex5_pub = -1;

static void
ex5_never(void *u)
{
	(void)u;
}

static void
ex5_proc(void *arg)
{
	uint32_t revents;
	(void)arg;
	g_ex5_pid = xtc_self();
	(void)xtc_timer_set(xtc_exec_loop((xtc_exec_t *)arg, 0),
	    10LL * 1000 * 1000 * 1000, ex5_never, NULL, &g_ex5_timer);
	for (;;) {
		revents = 0;
		atomic_fetch_add(&g_ex5_parked, 1);
		(void)xtc_proc_wait_fd(g_ex5_rfd, XTC_IO_READABLE, -1, &revents);
	}
}

static void *
ex5_driver(void *arg)
{
	int i;
	(void)arg;
	for (i = 0; i < 2000 && atomic_load(&g_ex5_parked) < 1; i++)
		(void)__os_sleep_ns(1000 * 1000LL);
	(void)__os_sleep_ns(20 * 1000 * 1000LL);
	(void)xtc_timer_cancel(g_ex5_timer);    /* foreign: marks only */
	/* Wake the loop, not the proc: an idle step pops the cancelled node
	 * in its next-deadline peek and blocks again, running nothing. */
	(void)xtc_io_wakeup(xtc_exec_loop((xtc_exec_t *)arg, 0)->io);
	(void)__os_sleep_ns(20 * 1000 * 1000LL);
	atomic_store(&g_ex5_pub, atomic_load_explicit(
	    &xtc_exec_loop((xtc_exec_t *)arg, 0)->n_timers_pub,
	    memory_order_relaxed));
	(void)xtc_exit_pid(g_ex5_pid, 0);
	/* Bound the test: if the run has not returned in 5 s, it is the
	 * hang; stop the executor so the assertion reports it. */
	for (i = 0; i < 5000 && !atomic_load(&g_ex5_returned); i++)
		(void)__os_sleep_ns(1000 * 1000LL);
	if (!atomic_load(&g_ex5_returned))
		xtc_exec_stop((xtc_exec_t *)arg);
	return NULL;
}

static MunitResult
test_quiesce_after_timer_drop(const MunitParameter p[], void *d)
{
	xtc_exec_t *e;
	xtc_proc_opts_t opts = XTC_PROC_OPTS_INIT;
	__os_thread_t drv = {0};
	xtc_pid_t pid;
	int64_t t0 = 0, t1 = 0;
	(void)p; (void)d;

	atomic_store(&g_ex5_parked, 0);
	atomic_store(&g_ex5_returned, 0);
	if (xtc_test_make_pipe(&g_ex5_rfd, &g_ex5_wfd) != 0)
		return MUNIT_SKIP;
	munit_assert_int(xtc_exec_init(&e, 1), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(xtc_exec_loop(e, 0), ex5_proc, e,
	    &opts, &pid), ==, XTC_OK);
	munit_assert_int(__os_thread_create(&drv, ex5_driver, e), ==, XTC_OK);
	(void)__os_clock_mono(&t0);
	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);
	(void)__os_clock_mono(&t1);
	atomic_store(&g_ex5_returned, 1);
	munit_assert_int(__os_thread_join(&drv, NULL), ==, XTC_OK);
	munit_assert_int(atomic_load(&g_ex5_parked), >=, 1);
	/* Blocked with an empty heap: the supervisor must read 0 timers. */
	munit_assert_int(atomic_load(&g_ex5_pub), ==, 0);
	if (t1 - t0 >= 4LL * 1000 * 1000 * 1000)
		munit_errorf("xtc_exec_run did not return after the last proc "
		    "exited (%lld ms; stopped by the 5 s bound)",
		    (long long)((t1 - t0) / 1000000));
	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);
	xtc_test_close_pipe(g_ex5_rfd, g_ex5_wfd);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/Ex1_Ex2_init_fini",       test_init_fini,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Ex3_run_until_done",      test_run_until_done,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Ex4_n_loops",             test_n_loops,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Ex5_quiesce_after_timer_drop", test_quiesce_after_timer_drop, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Sp1_spawn",               test_spawn,           NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Sp2_spawn_on",            test_spawn_on,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Sp3_n_spawned_sum",       test_n_spawned_sum,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/A12_shard_id",            test_shard_id,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/policy_knobs",            test_policy_knobs,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/accessors_errors",        test_accessors_errors,NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/class_accessors",         test_exec_class_accessors, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Blk1_blocking_resume",    test_blocking_resume_on_exec, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Blk2_concurrent_commit",  test_concurrent_commit_resume, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Blk3_cross_loop_del_fd",  test_cross_loop_del_fd, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Blk4_cross_loop_state_timer", test_cross_loop_state_timer, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Blk5_migratable_timer_resume", test_migratable_timer_resume, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Blk6_migratable_waitfd_resume", test_migratable_waitfd_resume, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Blk7_blocking_kill_no_frame_escape", test_blocking_kill_no_frame_escape, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Blk8_migratable_self_identity", test_migratable_self_identity, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m5/exec", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
