/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m5/test_exec.c -- verifies M5_CLAIMS.md Ex1-Ex4, Sp1-Sp3.
 */

#include <stdint.h>
#include <stdatomic.h>
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
#include "io_pipe_compat.h"
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
	xtc_loop_stats_t st;
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
static _Atomic int g_blk1_done;

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
		atomic_fetch_add(&g_blk1_done, 1);
}

static MunitResult
test_blocking_resume_on_exec(const MunitParameter p[], void *d)
{
	xtc_exec_t *e;
	xtc_proc_opts_t opts = { 0 };
	xtc_pid_t pid;
	int i;
	(void)p; (void)d;

	atomic_store(&g_blk1_done, 0);
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
	munit_assert_int(atomic_load(&g_blk1_done), ==, BLK1_N);
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
static _Atomic int g_blk2_done;
static _Atomic int g_blk2_lock;   /* 0/1 spin "flush lock" */

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
			while (atomic_exchange_explicit(&g_blk2_lock, 1,
			    memory_order_acquire) != 0) {
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
			atomic_store_explicit(&g_blk2_lock, 0,
			    memory_order_release);
	}
	atomic_fetch_add(&g_blk2_done, 1);
}

static MunitResult
test_concurrent_commit_resume(const MunitParameter p[], void *d)
{
	xtc_exec_t *e;
	xtc_proc_opts_t opts = { 0 };
	xtc_pid_t pid;
	int i;
	(void)p; (void)d;

	atomic_store(&g_blk2_done, 0);
	atomic_store(&g_blk2_lock, 0);
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
	munit_assert_int(atomic_load(&g_blk2_done), ==, BLK2_FIBERS);
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
static _Atomic int g_blk3_done;
static xtc_pid_t   g_blk3_pids[BLK3_FIBERS];
static int         g_blk3_rfd[BLK3_FIBERS];
static int         g_blk3_wfd[BLK3_FIBERS];
static _Atomic int g_blk3_ready;
static _Atomic int g_blk3_stop;

static void *
blk3_poker(void *arg)
{
	(void)arg;
	while (atomic_load(&g_blk3_ready) < BLK3_FIBERS &&
	    !atomic_load(&g_blk3_stop))
		(void)__os_sleep_ns(100 * 1000LL);
	while (!atomic_load(&g_blk3_stop)) {
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
	atomic_fetch_add(&g_blk3_ready, 1);
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
	atomic_fetch_add(&g_blk3_done, 1);
}

static MunitResult
test_cross_loop_del_fd(const MunitParameter p[], void *d)
{
	xtc_exec_t *e;
	xtc_proc_opts_t opts = { 0 };
	xtc_pid_t pid;
	__os_thread_t poker = {0};
	int i;
	(void)p; (void)d;

	atomic_store(&g_blk3_done, 0);
	atomic_store(&g_blk3_ready, 0);
	atomic_store(&g_blk3_stop, 0);
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
	atomic_store(&g_blk3_stop, 1);
	munit_assert_int(__os_thread_join(&poker, NULL), ==, XTC_OK);
	munit_assert_int(atomic_load(&g_blk3_done), ==, BLK3_FIBERS);
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
static _Atomic int g_blk4_done;
static xtc_pid_t   g_blk4_pids[BLK4_FIBERS];
static _Atomic int g_blk4_ready;
static _Atomic int g_blk4_stop;

static void *
blk4_poker(void *arg)
{
	(void)arg;
	while (atomic_load(&g_blk4_ready) < BLK4_FIBERS &&
	    !atomic_load(&g_blk4_stop))
		(void)__os_sleep_ns(100 * 1000LL);
	while (!atomic_load(&g_blk4_stop)) {
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
	atomic_fetch_add(&g_blk4_ready, 1);
	for (k = 0; k < BLK4_ITERS; k++) {
		void *msg = NULL;
		size_t len = 0;
		/* recv-timeout: arms a park timer on the running loop; a poker
		 * wake resumes us (mailbox empty -> XTC_E_AGAIN) and we re-park,
		 * re-arming on whatever loop currently runs us. */
		if (xtc_recv(&msg, &len, 1 * 1000 * 1000 /* 1ms */) == XTC_OK)
			xtc_free(msg);
	}
	atomic_fetch_add(&g_blk4_done, 1);
}

static MunitResult
test_cross_loop_state_timer(const MunitParameter p[], void *d)
{
	xtc_exec_t *e;
	xtc_proc_opts_t opts = { 0 };
	xtc_pid_t pid;
	__os_thread_t poker = {0};
	int i;
	(void)p; (void)d;

	atomic_store(&g_blk4_done, 0);
	atomic_store(&g_blk4_ready, 0);
	atomic_store(&g_blk4_stop, 0);
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
	atomic_store(&g_blk4_stop, 1);
	munit_assert_int(__os_thread_join(&poker, NULL), ==, XTC_OK);
	munit_assert_int(atomic_load(&g_blk4_done), ==, BLK4_FIBERS);
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
static _Atomic int g_blk5_done;
static _Atomic int g_blk5_lock;   /* 0/1 shared "WALWriteLock" */

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
		while (atomic_exchange_explicit(&g_blk5_lock, 1,
		    memory_order_acquire) != 0) {
			(void)xtc_proc_sleep(50 * 1000LL);
			if (++spins > 4000000) goto out;   /* defensive */
		}
		/* Hold the lock across a short sleep -- the holder's OWN park
		 * timer is the one that must resume it after migration. */
		(void)xtc_proc_sleep(20 * 1000LL);
		atomic_store_explicit(&g_blk5_lock, 0, memory_order_release);
	}
out:
	atomic_fetch_add(&g_blk5_done, 1);
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
	xtc_proc_opts_t opts = { 0 };
	xtc_pid_t pid;
	int i;
	atomic_store(&g_blk5_done, 0);
	atomic_store(&g_blk5_lock, 0);
	munit_assert_int(xtc_exec_init(&e, 8), ==, XTC_OK);
	xtc_exec_set_eager_rebalance(e, 1);
	for (i = 0; i < BLK5_FIBERS; i++) {
		opts.name = "blk5";
		opts.migratable = 1;
		munit_assert_int(xtc_proc_spawn(xtc_exec_loop(e, 0),
		    blk5_proc, (void *)(intptr_t)i, &opts, &pid), ==, XTC_OK);
	}
	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);
	munit_assert_int(atomic_load(&g_blk5_done), ==, BLK5_FIBERS);
	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);
	return MUNIT_OK;
#endif
}

static MunitTest tests[] = {
	{ "/Ex1_Ex2_init_fini",       test_init_fini,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Ex3_run_until_done",      test_run_until_done,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Ex4_n_loops",             test_n_loops,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
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
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m5/exec", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
