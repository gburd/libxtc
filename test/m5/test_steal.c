/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m5/test_steal.c
 *	Asserts the work-stealing safety claims (M5 St2/St3):
 *	  St2 -- no task is lost: every spawned task runs at least once.
 *	  St3 -- no task is double-run: every spawned task runs at most
 *	        once.
 *
 *	St1 ("steals actually happen when one loop is busy and others
 *	are idle") is timing-dependent -- when stealing works perfectly
 *	the work distributes; when stealing fails everything still
 *	completes correctly thanks to St2/St3.  The deterministic
 *	invariants St2/St3 are the real correctness claims.  St1 is
 *	exercised live by `test_cross_wake.c` (which depends on tasks
 *	actually moving between loops to fire the cross-thread waker).
 *
 *	Configuration: 4-loop executor, 200 plain (stealable) tasks
 *	pinned to loop 0.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_async.h"
#include "xtc_proc.h"
#include "xtc_int.h"
#include "loop_int.h"

#define ST_N_LOOPS   4
#define ST_N_TASKS   200

static _Atomic int  st_run_count[ST_N_TASKS];
static _Atomic int  st_done_count;

/* For the fairness test: each worker records the loop it ran on so
 * the test can count how many distinct loops participated. */
static _Atomic(void *) st_ran_on[ST_N_TASKS];

static int
worker(xtc_task_t *self, void *arg)
{
	int task_id = (int)(intptr_t)arg;
	struct timespec ts = { 0, 50 * 1000 };  /* 0.05 ms each */
	(void)self;
	(void)nanosleep(&ts, NULL);
	atomic_fetch_add_explicit(&st_run_count[task_id], 1,
	    memory_order_relaxed);
	atomic_fetch_add_explicit(&st_done_count, 1, memory_order_relaxed);
	return XTC_TASK_DONE;
}

static MunitResult
test_no_loss_no_double_run(const MunitParameter p[], void *d)
{
	xtc_exec_t *e;
	int i, total_runs = 0;
	(void)p; (void)d;

	for (i = 0; i < ST_N_TASKS; i++) atomic_store(&st_run_count[i], 0);
	atomic_store(&st_done_count, 0);

	munit_assert_int(xtc_exec_init(&e, ST_N_LOOPS), ==, XTC_OK);

	for (i = 0; i < ST_N_TASKS; i++) {
		xtc_task_t *t;
		/* Unpinned raw tasks on loop 0's stealable deque, so the
		 * no-loss / no-double-run invariants are tested under
		 * actual work stealing (xtc_exec_spawn_on now pins). */
		munit_assert_int(xtc_task_spawn(xtc_exec_loop(e, 0),
		    worker, (void *)(intptr_t)i, &t),
		    ==, XTC_OK);
	}

	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);

	/* St2: every task ran at least once. */
	for (i = 0; i < ST_N_TASKS; i++) {
		int rc = atomic_load(&st_run_count[i]);
		munit_assert_int(rc, >=, 1);
		total_runs += rc;
	}
	/* St3: every task ran AT MOST once. */
	for (i = 0; i < ST_N_TASKS; i++)
		munit_assert_int(atomic_load(&st_run_count[i]), <=, 1);

	munit_assert_int(total_runs, ==, ST_N_TASKS);
	munit_assert_int(atomic_load(&st_done_count), ==, ST_N_TASKS);

	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);
	return MUNIT_OK;
}

/* St1 -- steals actually happen.  All tasks are spawned on loop 0;
 * each does enough work (0.2 ms) that the idle loops have time to
 * steal.  We record which loop each task ran on and assert that at
 * least two distinct loops participated.  If stealing were broken,
 * loop 0 would run all of them and only one loop would appear --
 * which fails the test.  The lower bound (>= 2) is deliberately weak
 * so the test is robust to scheduler timing; it proves migration
 * occurs without asserting an exact balance. */
static int
fair_worker(xtc_task_t *self, void *arg)
{
	int task_id = (int)(intptr_t)arg;
	struct timespec ts = { 0, 200 * 1000 };  /* 0.2 ms each */
	(void)self;
	(void)nanosleep(&ts, NULL);
	atomic_store_explicit(&st_ran_on[task_id],
	    (void *)__xtc_current_loop, memory_order_relaxed);
	atomic_fetch_add_explicit(&st_done_count, 1, memory_order_relaxed);
	return XTC_TASK_DONE;
}

static MunitResult
test_steals_happen(const MunitParameter p[], void *d)
{
	xtc_exec_t *e;
	int i, n_distinct = 0;
	void *seen[ST_N_LOOPS];
	(void)p; (void)d;

	for (i = 0; i < ST_N_TASKS; i++)
		atomic_store(&st_ran_on[i], NULL);
	atomic_store(&st_done_count, 0);

	munit_assert_int(xtc_exec_init(&e, ST_N_LOOPS), ==, XTC_OK);
	for (i = 0; i < ST_N_TASKS; i++) {
		xtc_task_t *t;
		/* Unpinned raw tasks piled on loop 0 so idle peers must
		 * steal to distribute them. */
		munit_assert_int(xtc_task_spawn(xtc_exec_loop(e, 0),
		    fair_worker, (void *)(intptr_t)i, &t), ==, XTC_OK);
	}
	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);
	munit_assert_int(atomic_load(&st_done_count), ==, ST_N_TASKS);

	/* Count distinct loops that ran a task. */
	for (i = 0; i < ST_N_TASKS; i++) {
		void *l = atomic_load(&st_ran_on[i]);
		int j, found = 0;
		if (l == NULL) continue;
		for (j = 0; j < n_distinct; j++)
			if (seen[j] == l) { found = 1; break; }
		if (!found && n_distinct < ST_N_LOOPS)
			seen[n_distinct++] = l;
	}
	/* Broken stealing -> everything runs on loop 0 -> n_distinct==1. */
	munit_assert_int(n_distinct, >=, 2);

	/* The per-loop work stats should corroborate: at least one loop
	 * other than 0 recorded steals, and the tasks_run counts cover
	 * the whole batch. */
	{
		xtc_loop_stats_t ls;
		uint64_t total_run = 0, total_steals = 0;
		int k;
		for (k = 0; k < ST_N_LOOPS; k++) {
			munit_assert_int(xtc_exec_loop_stats(e, k, &ls),
			    ==, XTC_OK);
			total_run += ls.tasks_run;
			total_steals += ls.steals;
		}
		munit_assert_uint64(total_run, >=, (uint64_t)ST_N_TASKS);
		munit_assert_uint64(total_steals, >=, 1);
	}

	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);
	return MUNIT_OK;
}

/*
 * [M8] Yield fairness on one loop.  XTC_TASK_RESCHED promises the back of
 * the run queue (xtc_loop.3).  The owner used to pop its stealable deque
 * LIFO and always drain its owner-only FIFO first, so a migratable fiber
 * that yielded was re-popped at once (two migratable yielders: counts
 * [0, 1655348]) and a yielding pinned fiber starved every migratable one.
 * Each fiber here records the other's progress when it finishes; with a
 * FIFO run queue the two interleave and each sees the other at least half
 * way.
 */
#define M8_ITERS 2000
static _Atomic int m8_count[2];
static int m8_seen_at_finish[2];

static void
m8_yielder(void *arg)
{
	int me = (int)(intptr_t)arg, k;
	for (k = 0; k < M8_ITERS; k++) {
		atomic_fetch_add(&m8_count[me], 1);
		xtc_yield();
	}
	m8_seen_at_finish[me] = atomic_load(&m8_count[!me]);
}

static int
m8_run_pair(int migratable0, int migratable1)
{
	xtc_exec_t *e;
	xtc_proc_opts_t o0 = XTC_PROC_OPTS_INIT, o1 = XTC_PROC_OPTS_INIT;
	xtc_pid_t pid;
	o0.migratable = migratable0;
	o1.migratable = migratable1;
	atomic_store(&m8_count[0], 0);
	atomic_store(&m8_count[1], 0);
	m8_seen_at_finish[0] = m8_seen_at_finish[1] = -1;
	munit_assert_int(xtc_exec_init(&e, 1), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(xtc_exec_loop(e, 0), m8_yielder,
	    (void *)(intptr_t)0, &o0, &pid), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(xtc_exec_loop(e, 0), m8_yielder,
	    (void *)(intptr_t)1, &o1, &pid), ==, XTC_OK);
	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);
	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);
	munit_logf(MUNIT_LOG_INFO, "mig=(%d,%d) seen_at_finish=[%d, %d]",
	    migratable0, migratable1, m8_seen_at_finish[0],
	    m8_seen_at_finish[1]);
	return m8_seen_at_finish[0] < m8_seen_at_finish[1] ?
	    m8_seen_at_finish[0] : m8_seen_at_finish[1];
}

static MunitResult
test_yield_fairness(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	/* two migratable, then pinned + migratable */
	munit_assert_int(m8_run_pair(1, 1), >=, M8_ITERS / 2);
	munit_assert_int(m8_run_pair(0, 1), >=, M8_ITERS / 2);
	return MUNIT_OK;
}

/*
 * [M9] An idle loop steals even while it has a pending timer.  Loop 0
 * holds M9_BUSY runnable migratable fibers; loop 1's only task is a proc
 * asleep for 300 ms.  Stealing used to need a loop with no pending timer
 * (or eager rebalance), so loop 1 sat out the whole sleep: steals [0, 0].
 */
#define M9_BUSY 32
static _Atomic int m9_stop;

static void
m9_busy(void *arg)
{
	volatile int k;
	(void)arg;
	while (!atomic_load(&m9_stop)) {
		for (k = 0; k < 2000; k++)
			;
		xtc_yield();
	}
}

static void
m9_sleeper(void *arg)
{
	(void)arg;
	(void)xtc_proc_sleep(300 * 1000 * 1000LL);
	atomic_store(&m9_stop, 1);
}

static MunitResult
test_steal_with_pending_timer(const MunitParameter p[], void *d)
{
	xtc_exec_t *e;
	xtc_proc_opts_t mo = XTC_PROC_OPTS_INIT;
	xtc_loop_stats_t s1;
	xtc_pid_t pid;
	int i;
	(void)p; (void)d;
	atomic_store(&m9_stop, 0);
	mo.migratable = 1;
	munit_assert_int(xtc_exec_init(&e, 2), ==, XTC_OK);
	for (i = 0; i < M9_BUSY; i++)
		munit_assert_int(xtc_proc_spawn(xtc_exec_loop(e, 0), m9_busy,
		    NULL, &mo, &pid), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(xtc_exec_loop(e, 1), m9_sleeper,
	    NULL, NULL, &pid), ==, XTC_OK);
	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);
	munit_assert_int(xtc_exec_loop_stats(e, 1, &s1), ==, XTC_OK);
	munit_logf(MUNIT_LOG_INFO, "loop1 steals=%llu tasks_run=%llu",
	    (unsigned long long)s1.steals, (unsigned long long)s1.tasks_run);
	munit_assert_uint64(s1.steals, >=, 1);
	munit_assert_uint64(s1.tasks_run, >, 100);
	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/St2_St3_no_loss_no_double_run", test_no_loss_no_double_run,
	  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/St1_steals_happen_fairness", test_steals_happen,
	  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/M8_yield_fairness", test_yield_fairness,
	  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/M9_steal_with_pending_timer", test_steal_with_pending_timer,
	  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = {
	"/m5/steal", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
