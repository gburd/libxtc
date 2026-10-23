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

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_res.h"
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

/*
 * [M11] A cross-loop wake must not depend on the allocator.  The inbox
 * used to calloc a node per wake; with the allocator failing,
 * xtc_waker_wake returned XTC_E_NOMEM and the wake was gone -- the parked
 * task stayed parked forever (every caller of xtc_waker_wake ignores its
 * result).  The waker here fails every allocation for the duration of
 * the wake.  If the wake is refused it re-fires it with the allocator
 * restored so the run still quiesces, and the test fails on the
 * recorded rc.
 */
static void *m11_fail_calloc(size_t n, size_t sz) { (void)n; (void)sz; return NULL; }
static void *m11_fail_malloc(size_t sz) { (void)sz; return NULL; }
static int m11_wake_rc;

static int m11_waker_fn(xtc_task_t *self, void *u) {
	struct shared *s = u;
	struct __os_alloc_hook saved, failing;
	(void)self;
	while (__os_atomic_load_i32(&s->init_done) == 0)
		(void)__os_sleep_ns(100 * 1000LL);
	(void)__os_sleep_ns(2 * 1000 * 1000LL);   /* parker is PARKED */
	munit_assert_int(__os_alloc_get_hook(&saved), ==, XTC_OK);
	failing = saved;
	failing.malloc = m11_fail_malloc;
	failing.calloc = m11_fail_calloc;
	munit_assert_int(__os_alloc_set_hook(&failing), ==, XTC_OK);
	m11_wake_rc = xtc_waker_wake(&s->parker_waker);
	munit_assert_int(__os_alloc_set_hook(&saved), ==, XTC_OK);
	if (m11_wake_rc != XTC_OK)
		(void)xtc_waker_wake(&s->parker_waker);
	return XTC_TASK_DONE;
}

static MunitResult
test_wake_no_alloc(const MunitParameter p[], void *d)
{
	xtc_exec_t *e;
	static struct shared s;
	(void)p; (void)d;
	s.parker_runs = 0; s.init_done = 0; s.max_runs = 2; s.wake_calls = 0;
	m11_wake_rc = -12345;
	munit_assert_int(xtc_exec_init(&e, 2), ==, XTC_OK);
	munit_assert_int(xtc_exec_spawn_on(e, 0, parker_fn, &s, NULL), ==, XTC_OK);
	munit_assert_int(xtc_exec_spawn_on(e, 1, m11_waker_fn, &s, NULL), ==, XTC_OK);
	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);
	munit_assert_int(m11_wake_rc, ==, XTC_OK);
	munit_assert_int(__os_atomic_load_i32(&s.parker_runs), ==, 2);
	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);
	return MUNIT_OK;
}

/*
 * [m1] XTC_RES_INBOX_MSGS is charged only by a cross-thread spawn, so a
 * cross-loop WAKE must not release it.  The drain used to release one
 * unit per drained entry, WAKEs included: units charged by someone else
 * vanished (here, the five the test holds) and the count went negative
 * under load.  The parker re-parks after each wake, so every wake below
 * is a real cross-loop WAKE.
 */
#define M1_HELD  5
#define M1_WAKES 40

/* Waker for the m1 test.  Cross-loop wakes legitimately coalesce (a wake
 * of a task that is SCHEDULED or RUNNING is only a latch, Wk3), so firing
 * exactly n wakes and expecting n distinct resumptions hangs the parker
 * whenever two wakes merge (seen under make -j load).  Instead keep
 * waking until the parker has run the target number of times; every
 * wake that lands on a PARKED parker is still a real cross-loop WAKE
 * through the inbox, which is what the accounting check needs. */
static int m1_waker_fn(xtc_task_t *self, void *u) {
	struct waker_args *a = u;
	(void)self;
	while (__os_atomic_load_i32(&a->s->init_done) == 0)
		(void)__os_sleep_ns(100 * 1000LL);
	while (__os_atomic_load_i32(&a->s->parker_runs) < a->s->max_runs) {
		(void)xtc_waker_wake(&a->s->parker_waker);
		__os_atomic_fetch_add_i32(&a->s->wake_calls, 1);
		(void)__os_sleep_ns(200 * 1000LL);
	}
	return XTC_TASK_DONE;
}

static MunitResult
test_inbox_res_wake_symmetric(const MunitParameter p[], void *d)
{
	xtc_exec_t *e;
	static struct shared s;
	static struct waker_args wa;
	xtc_res_t *r0;
	(void)p; (void)d;
	s.parker_runs = 0; s.init_done = 0; s.max_runs = M1_WAKES + 1;
	s.wake_calls = 0;
	wa.s = &s; wa.n_wakes = M1_WAKES;
	munit_assert_int(xtc_exec_init(&e, 2), ==, XTC_OK);
	r0 = xtc_loop_res(xtc_exec_loop(e, 0));
	munit_assert_ptr_not_null(r0);
	munit_assert_int(xtc_res_acquire(r0, XTC_RES_INBOX_MSGS, M1_HELD),
	    ==, XTC_OK);
	munit_assert_int(xtc_exec_spawn_on(e, 0, parker_fn, &s, NULL), ==, XTC_OK);
	munit_assert_int(xtc_exec_spawn_on(e, 1, m1_waker_fn, &wa, NULL), ==, XTC_OK);
	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);
	munit_assert_int(__os_atomic_load_i32(&s.parker_runs), ==, M1_WAKES + 1);
	munit_assert_int64(xtc_res_used(r0, XTC_RES_INBOX_MSGS), ==, M1_HELD);
	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);
	return MUNIT_OK;
}

/*
 * [Cw6] A cross-loop wake of a task on a loop that is RUNNING must not
 * write the loop's wakeup fd: the owner drains its inbox at the top of
 * every step and re-checks it before it blocks, so the nudge is only
 * needed when the owner is asleep in its poller.  Loop 0 here never
 * sleeps (a spinner keeps its run queue non-empty) and loop 1 wakes a
 * parked task on it CW6_WAKES times.  Before the elision every wake was
 * a write(2) (>= CW6_WAKES in /proc/self/io syscw over the window); now
 * none is needed.  The bound leaves room for unrelated writes.  Linux
 * only (the counter is /proc/self/io); a work count, not a timing.
 */
#define CW6_WAKES 200
static _Atomic int  g_cw6_done;
static long         g_cw6_writes = -1;

static long
cw6_syscw(void)
{
#if defined(__linux__)
	char buf[512];
	FILE *f = fopen("/proc/self/io", "r");
	long v = -1;
	if (f == NULL)
		return -1;
	while (fgets(buf, sizeof buf, f) != NULL)
		if (strncmp(buf, "syscw:", 6) == 0)
			v = atol(buf + 6);
	fclose(f);
	return v;
#else
	return -1;
#endif
}

static int cw6_spinner_fn(xtc_task_t *self, void *u) {
	(void)self; (void)u;
	return atomic_load(&g_cw6_done) ? XTC_TASK_DONE : XTC_TASK_RESCHED;
}

static int cw6_waker_fn(xtc_task_t *self, void *u) {
	struct shared *s = u;
	long w0;
	int i;
	(void)self;
	while (__os_atomic_load_i32(&s->init_done) == 0)
		;
	w0 = cw6_syscw();
	for (i = 0; i < CW6_WAKES; i++) {
		(void)xtc_waker_wake(&s->parker_waker);
		/* Wait for the parker to run and re-park, so the next wake is
		 * a real inbox push, not a latch on a SCHEDULED task. */
		while (__os_atomic_load_i32(&s->parker_runs) < i + 2)
			;
	}
	if (w0 >= 0)
		g_cw6_writes = cw6_syscw() - w0;
	atomic_store(&g_cw6_done, 1);
	return XTC_TASK_DONE;
}

static MunitResult
test_wake_running_loop_no_write(const MunitParameter p[], void *d)
{
	xtc_exec_t *e;
	static struct shared s;
	(void)p; (void)d;
	if (cw6_syscw() < 0)
		return MUNIT_SKIP;
	s.parker_runs = 0; s.init_done = 0; s.max_runs = CW6_WAKES + 1;
	s.wake_calls = 0;
	atomic_store(&g_cw6_done, 0);
	g_cw6_writes = -1;
	munit_assert_int(xtc_exec_init(&e, 2), ==, XTC_OK);
	munit_assert_int(xtc_exec_spawn_on(e, 0, cw6_spinner_fn, NULL, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_exec_spawn_on(e, 0, parker_fn, &s, NULL), ==, XTC_OK);
	munit_assert_int(xtc_exec_spawn_on(e, 1, cw6_waker_fn, &s, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);
	munit_assert_int(__os_atomic_load_i32(&s.parker_runs), ==, CW6_WAKES + 1);
	munit_assert_int(xtc_exec_fini(e), ==, XTC_OK);
	munit_logf(MUNIT_LOG_INFO, "%d wakes to a running loop: %ld write(2)s",
	    CW6_WAKES, g_cw6_writes);
	munit_assert_long(g_cw6_writes, >=, 0);
	munit_assert_long(g_cw6_writes, <, CW6_WAKES / 4);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/Cw1_Cw2_basic",   test_basic,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Cw3_after_done",  test_after_done, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Cw4_concurrent",  test_concurrent, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Cw5_loop_wake_foreign", test_loop_wake_foreign, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/M11_wake_no_alloc", test_wake_no_alloc, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/m1_inbox_res_wake_symmetric", test_inbox_res_wake_symmetric, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Cw6_wake_running_loop_no_write", test_wake_running_loop_no_write, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m5/cross_wake", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
