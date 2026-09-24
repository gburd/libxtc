/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m3/test_timer.c -- verifies M3_CLAIMS.md Tm5-Tm10.
 */

#include <stdint.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "os_time.h"
#include "os_alloc.h"

#include <stdatomic.h>

/* [Tm5] basic */
static int basic_fired;
static int64_t basic_fire_time;
static void basic_cb(void *u) {
	(void)u; basic_fired++;
	(void)__os_clock_mono(&basic_fire_time);
}

static MunitResult
test_timer_basic(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_timer_t *t;
	int64_t before;
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	basic_fired = 0;
	munit_assert_int(__os_clock_mono(&before), ==, XTC_OK);
	munit_assert_int(xtc_timer_set(loop, 5 * XTC_NS_PER_MS,
	    basic_cb, NULL, &t), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(basic_fired, ==, 1);
	munit_assert_int64(basic_fire_time - before, >=, 5 * XTC_NS_PER_MS);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* [Tm6] order */
static int order_log[5];
static int order_idx;
static void order_cb(void *u) { order_log[order_idx++] = *(int *)u; }

static MunitResult
test_order(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	int ids[5] = { 1, 2, 3, 4, 5 };
	int delays[5] = { 30, 10, 50, 20, 40 };  /* in ms */
	int sorted[5] = { 2, 4, 1, 5, 3 };       /* expected fire order */
	int i;
	(void)p; (void)d;

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	order_idx = 0;
	for (i = 0; i < 5; i++)
		munit_assert_int(xtc_timer_set(loop,
		    (int64_t)delays[i] * XTC_NS_PER_MS,
		    order_cb, &ids[i], NULL), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(order_idx, ==, 5);
	for (i = 0; i < 5; i++)
		munit_assert_int(order_log[i], ==, sorted[i]);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* [Tm7] cancel before fire */
static int cancel_fired;
static void cancel_cb(void *u) { (void)u; cancel_fired++; }

static MunitResult
test_cancel_before_fire(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_timer_t *t;
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	cancel_fired = 0;
	munit_assert_int(xtc_timer_set(loop, 5 * XTC_NS_PER_MS,
	    cancel_cb, NULL, &t), ==, XTC_OK);
	munit_assert_int(xtc_timer_cancel(t), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(cancel_fired, ==, 0);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* [Tm8] cancel after fire is a no-op */
static MunitResult
test_cancel_after_fire(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_timer_t *t;
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_timer_set(loop, 1 * XTC_NS_PER_MS,
	    basic_cb, NULL, &t), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	/* Now t has fired; cancelling is a safe no-op. */
	munit_assert_int(xtc_timer_cancel(t), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* [Tm9] many timers */
#define MANY 200
static int many_fired;
static int64_t many_last;
static int many_in_order;
static void many_cb(void *u) {
	int64_t now;
	int idx = *(int *)u;
	(void)idx;
	(void)__os_clock_mono(&now);
	if (many_fired > 0 && now < many_last) many_in_order = 0;
	many_last = now;
	many_fired++;
}

static MunitResult
test_many_timers(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	int ids[MANY];
	int i;
	(void)p; (void)d;

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	many_fired = 0; many_last = 0; many_in_order = 1;
	for (i = 0; i < MANY; i++) {
		ids[i] = i;
		/* Random-ish delays in [0, 20] ms */
		int delay_ms = (int)((unsigned)(i * 2654435761u) % 20);
		munit_assert_int(xtc_timer_set(loop,
		    (int64_t)delay_ms * XTC_NS_PER_MS,
		    many_cb, &ids[i], NULL), ==, XTC_OK);
	}
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(many_fired, ==, MANY);
	munit_assert_int(many_in_order, ==, 1);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* [Tm10] park_on_timer */
static int park_runs;
struct pt { int phase; };

static int park_task(xtc_task_t *self, void *u) {
	struct pt *c = u;
	park_runs++;
	if (c->phase == 0) {
		c->phase = 1;
		munit_assert_int(xtc_task_park_on_timer(self, 5 * XTC_NS_PER_MS),
		    ==, XTC_OK);
		return XTC_TASK_PENDING;
	}
	return XTC_TASK_DONE;
}

static MunitResult
test_park_on_timer(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	struct pt c = { 0 };
	int64_t before, after;
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	park_runs = 0;
	munit_assert_int(__os_clock_mono(&before), ==, XTC_OK);
	munit_assert_int(xtc_task_spawn(loop, park_task, &c, NULL), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(__os_clock_mono(&after), ==, XTC_OK);
	munit_assert_int(park_runs, ==, 2);
	munit_assert_int64(after - before, >=, 5 * XTC_NS_PER_MS);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* Tm11: a timer must fire even when the run queue NEVER empties.
 * A busy task that keeps returning XTC_TASK_RESCHED (the yield/spin
 * shape) used to starve the timer drain, because the loop only expired
 * timers once the run queue drained.  The scheduler now drains due
 * timers on the IO-fairness quantum under load too.
 *
 * The busy task stops ONLY when the timer fires -- there is no
 * self-draining escape (a spin cap that returns DONE would let the run
 * queue empty and the timer fire late, masking the bug).  A wall-clock
 * safety bound records failure + stops the loop if the timer is starved
 * far past its deadline, so the test FAILS (loudly) instead of hanging
 * forever when the fix is absent. */
static int busy_timer_fired;
static int busy_starved;             /* set if the timer was starved past the bound */
static int64_t busy_start_ns;
static void busy_timer_cb(void *u) { (void)u; busy_timer_fired = 1; }
static int busy_task(xtc_task_t *self, void *u) {
	xtc_loop_t *loop = u;   /* the loop, passed via user arg */
	int64_t now = 0;
	(void)self;
	if (busy_timer_fired)
		return XTC_TASK_DONE;              /* timer fired promptly -> stop */
	(void)__os_clock_mono(&now);
	/* The timer deadline is 5ms; if 2s of pure spinning has elapsed and
	 * it STILL has not fired, it is starved -- the bug.  Record it and
	 * stop the loop so the test fails cleanly rather than hangs. */
	if (now - busy_start_ns > 2LL * XTC_NS_PER_SEC) {
		busy_starved = 1;
		xtc_loop_stop(loop);
		return XTC_TASK_DONE;
	}
	return XTC_TASK_RESCHED;                  /* keep the run queue busy */
}

static MunitResult
test_timer_under_busy_runqueue(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_timer_t *t;
	int64_t before, after;
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	busy_timer_fired = 0;
	busy_starved = 0;
	munit_assert_int(__os_clock_mono(&before), ==, XTC_OK);
	busy_start_ns = before;
	munit_assert_int(xtc_timer_set(loop, 5 * XTC_NS_PER_MS,
	    busy_timer_cb, NULL, &t), ==, XTC_OK);
	munit_assert_int(xtc_task_spawn(loop, busy_task, loop, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(__os_clock_mono(&after), ==, XTC_OK);
	/* The timer fired despite the never-empty run queue, and was NOT
	 * starved to the 2s bound (before the fix it would spin the full 2s
	 * and set busy_starved). */
	munit_assert_int(busy_starved, ==, 0);
	munit_assert_int(busy_timer_fired, ==, 1);
	/* And it fired reasonably promptly -- well under the starvation
	 * bound (a loose 500ms ceiling; the deadline is 5ms). */
	munit_assert_int64(after - before, <, 500 * XTC_NS_PER_MS);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* ---- Tm12: park timers are freed by loop_fini even when a timer slab
 * exists.
 *
 * xtc_timer_set allocates from the loop's timer slab; the park timers
 * behind xtc_task_park_on_timer come from __os_calloc.  Both sit on the
 * loop's all_timers list, and loop_fini used to free EVERY node with
 * xtc_slab_free once the slab existed -- so each calloc'd park timer was
 * pushed onto the slab's free list as a foreign pointer and lost when the
 * slab was destroyed.  Counted here through the allocator hook (no
 * sanitizer needed): every calloc/malloc the loop makes must be matched
 * by a free by the time xtc_loop_fini returns.  Fails on the unfixed code
 * (one live allocation per park); passes with it. */
static _Atomic long g_tm12_live;
static struct __os_alloc_hook g_tm12_saved;

static void *
tm12_malloc(size_t n)
{
	void *p = g_tm12_saved.malloc(n);
	if (p != NULL) atomic_fetch_add(&g_tm12_live, 1);
	return p;
}
static void *
tm12_calloc(size_t n, size_t sz)
{
	void *p = g_tm12_saved.calloc(n, sz);
	if (p != NULL) atomic_fetch_add(&g_tm12_live, 1);
	return p;
}
static void *
tm12_realloc(void *o, size_t sz)
{
	void *p = g_tm12_saved.realloc(o, sz);
	if (o == NULL && p != NULL) atomic_fetch_add(&g_tm12_live, 1);
	return p;
}
static void
tm12_free(void *p)
{
	if (p != NULL) atomic_fetch_sub(&g_tm12_live, 1);
	g_tm12_saved.free(p);
}
static void *
tm12_aligned(size_t a, size_t sz)
{
	void *p = g_tm12_saved.aligned(a, sz);
	if (p != NULL) atomic_fetch_add(&g_tm12_live, 1);
	return p;
}
static void
tm12_aligned_free(void *p)
{
	if (p != NULL) atomic_fetch_sub(&g_tm12_live, 1);
	g_tm12_saved.aligned_free(p);
}

static void tm12_noop(void *u) { (void)u; }

#define TM12_PARKS 20

/* Parks on a 1us timer TM12_PARKS times (each park is a fresh calloc'd
 * node that stays on all_timers after it fires), then finishes. */
static int
tm12_task(xtc_task_t *self, void *u)
{
	int *n = u;
	if ((*n)++ < TM12_PARKS) {
		munit_assert_int(xtc_task_park_on_timer(self, 1000), ==, XTC_OK);
		return XTC_TASK_PENDING;
	}
	return XTC_TASK_DONE;
}

static MunitResult
test_park_timers_freed_with_slab(const MunitParameter p[], void *d)
{
	struct __os_alloc_hook h;
	xtc_loop_t *loop = NULL;
	long leaked;
	int parks = 0;
	(void)p; (void)d;

	munit_assert_int(__os_alloc_get_hook(&g_tm12_saved), ==, XTC_OK);
	h.malloc = tm12_malloc;   h.calloc = tm12_calloc;
	h.realloc = tm12_realloc; h.free = tm12_free;
	h.aligned = tm12_aligned; h.aligned_free = tm12_aligned_free;
	atomic_store(&g_tm12_live, 0);
	munit_assert_int(__os_alloc_set_hook(&h), ==, XTC_OK);

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	/* Creates the loop's timer slab -- the trigger.  1us, so the loop
	 * does not wait on it. */
	munit_assert_int(xtc_timer_set(loop, 1000, tm12_noop, NULL, NULL),
	    ==, XTC_OK);
	/* Park timers from __os_calloc, left on all_timers for loop_fini. */
	munit_assert_int(xtc_task_spawn(loop, tm12_task, &parks, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(parks, ==, TM12_PARKS + 1);   /* really parked */
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);

	munit_assert_int(__os_alloc_set_hook(&g_tm12_saved), ==, XTC_OK);
	leaked = atomic_load(&g_tm12_live);
	munit_logf(MUNIT_LOG_INFO, "live allocations after loop_fini: %ld",
	    leaked);
	munit_assert_long(leaked, ==, 0);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/Tm5_basic",            test_timer_basic,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Tm6_order",            test_order,              NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Tm7_cancel_before",    test_cancel_before_fire, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Tm8_cancel_after",     test_cancel_after_fire,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Tm9_many",             test_many_timers,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Tm10_park_on_timer",   test_park_on_timer,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Tm11_timer_under_busy_runqueue", test_timer_under_busy_runqueue, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Tm12_park_timers_freed_with_slab", test_park_timers_freed_with_slab, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m3/timer", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
