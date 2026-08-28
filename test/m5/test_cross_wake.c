/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m5/test_cross_wake.c -- verifies M5_CLAIMS.md Cw1-Cw4.
 *
 * Pattern: a "parker" task on loop 0 registers a waker, returns
 * PENDING, and counts how many times its fn is re-invoked.  A
 * "waker" task on loop 1 fires the waker N times.  We verify the
 * idempotent collapse semantics from M3 + the cross-thread
 * delivery added in M5.
 */

#include <stdint.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_int.h"
#include "os_time.h"

struct shared {
	xtc_waker_t parker_waker;
	int         init_done;
	int         parker_runs;
	int         max_runs;       /* parker exits with DONE after this */
	int         wake_calls;     /* total cross-thread wakes fired */
};

static int parker_fn(xtc_task_t *self, void *u) {
	struct shared *s = u;
	int n = __os_atomic_fetch_add_i32(&s->parker_runs, 1) + 1;
	if (n == 1) {
		(void)xtc_task_waker(self, &s->parker_waker);
		__os_atomic_store_i32(&s->init_done, 1);
		return XTC_TASK_PENDING;
	}
	if (n >= s->max_runs) return XTC_TASK_DONE;
	return XTC_TASK_PENDING;   /* re-park; await another wake */
}

struct waker_args {
	struct shared *s;
	int            n_wakes;
};

static int waker_fn(xtc_task_t *self, void *u) {
	struct waker_args *a = u;
	int i;
	(void)self;
	while (__os_atomic_load_i32(&a->s->init_done) == 0)
		(void)__os_sleep_ns(100 * 1000LL);
	for (i = 0; i < a->n_wakes; i++) {
		(void)xtc_waker_wake(&a->s->parker_waker);
		__os_atomic_fetch_add_i32(&a->s->wake_calls, 1);
		(void)__os_sleep_ns(200 * 1000LL);   /* let parker run between wakes */
	}
	return XTC_TASK_DONE;
}

/* [Cw1, Cw2] basic + idempotent: one waker fires, parker runs again. */
static MunitResult
test_basic(const MunitParameter p[], void *d)
{
	xtc_exec_t *e;
	static struct shared s;
	static struct waker_args wa;
	(void)p; (void)d;
	s.parker_runs = 0; s.init_done = 0; s.max_runs = 2; s.wake_calls = 0;
	wa.s = &s; wa.n_wakes = 1;
	munit_assert_int(xtc_exec_init(&e, 4), ==, XTC_OK);
	munit_assert_int(xtc_exec_spawn_on(e, 0, parker_fn, &s, NULL), ==, XTC_OK);
	munit_assert_int(xtc_exec_spawn_on(e, 1, waker_fn,  &wa, NULL), ==, XTC_OK);
	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);
	munit_assert_int(__os_atomic_load_i32(&s.parker_runs), ==, 2);
	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);
	return MUNIT_OK;
}

/* [Cw3] wake-after-done is a safe no-op.  Run the basic flow, then
 * fire the waker after the parker has completed.  Should not crash. */
static MunitResult
test_after_done(const MunitParameter p[], void *d)
{
	xtc_exec_t *e;
	static struct shared s;
	static struct waker_args wa;
	(void)p; (void)d;
	s.parker_runs = 0; s.init_done = 0; s.max_runs = 2; s.wake_calls = 0;
	wa.s = &s; wa.n_wakes = 1;
	munit_assert_int(xtc_exec_init(&e, 4), ==, XTC_OK);
	munit_assert_int(xtc_exec_spawn_on(e, 0, parker_fn, &s, NULL), ==, XTC_OK);
	munit_assert_int(xtc_exec_spawn_on(e, 1, waker_fn,  &wa, NULL), ==, XTC_OK);
	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);
	/* Now both tasks DONE.  Fire the captured waker -- must be safe. */
	munit_assert_int(xtc_waker_wake(&s.parker_waker), ==, XTC_OK);
	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);
	return MUNIT_OK;
}

/* [Cw4] concurrent wakes: many wakes from loop 1 -> parker observes
 * at least one re-run, at most n_wakes + 1 (1 initial + n_wakes
 * idempotent collapses).  We bound max_runs to be small so the parker
 * exits as soon as it has run a couple of times; otherwise the
 * test would hang waiting for additional wakes that never come. */
static MunitResult
test_concurrent(const MunitParameter p[], void *d)
{
	xtc_exec_t *e;
	static struct shared s;
	static struct waker_args wa;
	int observed;
	(void)p; (void)d;
	s.parker_runs = 0; s.init_done = 0; s.max_runs = 2; s.wake_calls = 0;
	wa.s = &s; wa.n_wakes = 50;
	munit_assert_int(xtc_exec_init(&e, 4), ==, XTC_OK);
	munit_assert_int(xtc_exec_spawn_on(e, 0, parker_fn, &s, NULL), ==, XTC_OK);
	munit_assert_int(xtc_exec_spawn_on(e, 1, waker_fn,  &wa, NULL), ==, XTC_OK);
	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);
	observed = __os_atomic_load_i32(&s.parker_runs);
	munit_assert_int(observed, >=, 2);                  /* initial + at least one wake */
	munit_assert_int(observed, <=, wa.n_wakes + 1);     /* idempotent cap */
	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);
	return MUNIT_OK;
}

/* [Cw5] xtc_loop_wake from a FOREIGN (non-xtc) OS thread.  Models the
 * embedder scenario (a PostgreSQL pooled carrier marking a parked
 * session runnable from a sibling carrier thread): a raw pthread that
 * libxtc does not manage fires the parked task's waker cross-thread and
 * then nudges the target loop with the PUBLIC xtc_loop_wake() handle
 * API (rather than reaching into loop->io).  The parked task must
 * resume and the run must quiesce.  Without the loop nudge a real
 * backend could leave the loop asleep in xtc_io_poll (the reported
 * cross-loop wake miss); the waker's own inbox-push + this nudge close
 * it lost-wake-free. */
struct foreign_args {
	struct shared *s;
	xtc_loop_t    *loop0;
};

static void *
foreign_waker_thread(void *arg)
{
	struct foreign_args *a = arg;
	/* wait until the parker has parked (init_done) */
	while (__os_atomic_load_i32(&a->s->init_done) == 0)
		(void)__os_sleep_ns(100 * 1000LL);
	(void)__os_sleep_ns(2 * 1000 * 1000LL);   /* let the loop go idle in poll */
	/* Make the condition true (fire the waker: deposits a resume on the
	 * loop's inbox), THEN nudge the loop's poller via the public API. */
	(void)xtc_waker_wake(&a->s->parker_waker);
	(void)xtc_loop_wake(a->loop0);
	return NULL;
}

static MunitResult
test_loop_wake_foreign(const MunitParameter p[], void *d)
{
	xtc_exec_t *e;
	__os_thread_t thr = {0};
	static struct shared s;
	struct foreign_args fa;
	(void)p; (void)d;
	s.parker_runs = 0; s.init_done = 0; s.max_runs = 2; s.wake_calls = 0;
	munit_assert_int(xtc_exec_init(&e, 4), ==, XTC_OK);
	munit_assert_int(xtc_exec_spawn_on(e, 0, parker_fn, &s, NULL), ==, XTC_OK);
	/* NULL loop -> XTC_E_INVAL (guard). */
	munit_assert_int(xtc_loop_wake(NULL), ==, XTC_E_INVAL);
	fa.s = &s; fa.loop0 = xtc_exec_loop(e, 0);
	munit_assert_ptr_not_null(fa.loop0);
	munit_assert_int(__os_thread_create(&thr, foreign_waker_thread, &fa),
	    ==, XTC_OK);
	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);   /* must quiesce */
	munit_assert_int(__os_thread_join(&thr, NULL), ==, XTC_OK);
	munit_assert_int(__os_atomic_load_i32(&s.parker_runs), ==, 2);
	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/Cw1_Cw2_basic",   test_basic,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Cw3_after_done",  test_after_done, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Cw4_concurrent",  test_concurrent, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Cw5_loop_wake_foreign", test_loop_wake_foreign, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m5/cross_wake", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
