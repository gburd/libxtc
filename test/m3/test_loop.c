/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m3/test_loop.c -- verifies M3_CLAIMS.md Ev1-Ev5.
 */

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "os_alloc.h"

/* [Ev1, Ev2] */
static MunitResult
test_init_fini(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_not_null(loop);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_init(NULL), ==, XTC_E_INVAL);
	munit_assert_int(xtc_loop_fini(NULL), ==, XTC_E_INVAL);
	return MUNIT_OK;
}

/* [Ev3] */
static MunitResult
test_run_empty(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* [Ev4] */
static int hits;
static int task_done_once(xtc_task_t *self, void *u) {
	(void)self; (void)u; hits++; return XTC_TASK_DONE;
}

static MunitResult
test_run_until_done(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	(void)p; (void)d;
	hits = 0;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_task_spawn(loop, task_done_once, NULL, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(hits, ==, 1);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* [Ev5] stop */
struct stop_ctx { xtc_loop_t *loop; int counter; };
static int task_stop(xtc_task_t *self, void *u) {
	struct stop_ctx *c = u;
	(void)self;
	c->counter++;
	if (c->counter == 3)
		(void)xtc_loop_stop(c->loop);
	return XTC_TASK_RESCHED;
}

static MunitResult
test_stop(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	struct stop_ctx c;
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	c.loop = loop; c.counter = 0;
	munit_assert_int(xtc_task_spawn(loop, task_stop, &c, NULL), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(c.counter, >=, 3);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/*
 * m6: xtc_loop_fini with live procs.  Procs parked in a long sleep, each
 * holding queued messages, links and a monitor, are torn down by
 * xtc_loop_fini.  Their envelopes used to leak (the unregister freed the
 * proc structs without draining their mailboxes).  Outstanding heap
 * bytes are counted through the allocator hook; messages are larger
 * than the envelope slab's payload so each one is a heap allocation.
 */
#define FINI_PROCS  16
#define FINI_MSGS   8
#define FINI_BYTES  4096

static struct __os_alloc_hook fini_real;
static _Atomic long long      fini_live;    /* outstanding allocations */

static void *fini_malloc(size_t n) {
	void *p = fini_real.malloc(n);
	if (p) atomic_fetch_add(&fini_live, 1);
	return p;
}
static void *fini_calloc(size_t n, size_t sz) {
	void *p = fini_real.calloc(n, sz);
	if (p) atomic_fetch_add(&fini_live, 1);
	return p;
}
static void *fini_realloc(void *o, size_t n) {
	void *p = fini_real.realloc(o, n);
	if (p && o == NULL) atomic_fetch_add(&fini_live, 1);
	return p;
}
static void fini_free(void *p) {
	if (p) atomic_fetch_sub(&fini_live, 1);
	fini_real.free(p);
}
static void *fini_aligned(size_t a, size_t n) {
	void *p = fini_real.aligned(a, n);
	if (p) atomic_fetch_add(&fini_live, 1);
	return p;
}
static void fini_aligned_free(void *p) {
	if (p) atomic_fetch_sub(&fini_live, 1);
	fini_real.aligned_free(p);
}

static xtc_loop_t *fini_loop;

static void
fini_sleeper(void *arg)
{
	(void)arg;
	(void)xtc_proc_sleep((int64_t)3600 * 1000000000);
}

static void
fini_parent(void *arg)
{
	static char msg[FINI_BYTES];
	xtc_pid_t kids[FINI_PROCS];
	uint64_t ref;
	int i, j;
	(void)arg;
	(void)xtc_proc_trap_exit(1);
	for (i = 0; i < FINI_PROCS; i++) {
		munit_assert_int(xtc_proc_spawn(fini_loop, fini_sleeper, NULL,
		    NULL, &kids[i]), ==, XTC_OK);
		munit_assert_int(xtc_link(kids[i]), ==, XTC_OK);
		munit_assert_int(xtc_monitor(kids[i], &ref), ==, XTC_OK);
	}
	xtc_yield();     /* let them start and park */
	for (i = 0; i < FINI_PROCS; i++)
		for (j = 0; j < FINI_MSGS; j++)
			munit_assert_int(xtc_send(kids[i], msg, sizeof msg), ==,
			    XTC_OK);
	(void)xtc_loop_stop(fini_loop);
	(void)xtc_proc_sleep((int64_t)3600 * 1000000000);
}

static long long
fini_round(void)
{
	long long before;
	before = atomic_load(&fini_live);
	munit_assert_int(xtc_loop_init(&fini_loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(fini_loop, fini_parent, NULL, NULL,
	    NULL), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(fini_loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(fini_loop), ==, XTC_OK);
	return atomic_load(&fini_live) - before;
}

static MunitResult
test_fini_live_procs(const MunitParameter p[], void *d)
{
	struct __os_alloc_hook h;
	long long leaked;
	(void)p; (void)d;
	munit_assert_int(__os_alloc_get_hook(&fini_real), ==, XTC_OK);
	h = fini_real;
	h.malloc = fini_malloc; h.calloc = fini_calloc;
	h.realloc = fini_realloc; h.free = fini_free;
	h.aligned = fini_aligned; h.aligned_free = fini_aligned_free;
	munit_assert_int(__os_alloc_set_hook(&h), ==, XTC_OK);
	(void)fini_round();          /* warm the global slabs and caches */
	leaked = fini_round();
	(void)__os_alloc_set_hook(&fini_real);
	printf("  outstanding allocations after fini: %lld\n", leaked);
	fflush(stdout);
	/* FINI_PROCS * FINI_MSGS heap envelopes used to leak. */
	munit_assert_llong(leaked, <, FINI_PROCS * FINI_MSGS / 2);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/Ev1_Ev2_init_fini",     test_init_fini,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Ev3_run_empty",         test_run_empty,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Ev4_run_until_done",    test_run_until_done,NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/Ev5_stop",              test_stop,          NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/fini_live_procs",       test_fini_live_procs, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m3/loop", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
