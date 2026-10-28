/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m4/test_async.c -- verifies M4_CLAIMS.md C1-C9.
 */

#include <stdint.h>
#include <stdio.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_async.h"
#include "os_time.h"

/* [C1, C2] basic spawn-and-await. */
static intptr_t add_one(void *arg) {
	int v = *(int *)arg;
	return (intptr_t)(v + 1);
}

static MunitResult
test_basic(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_task_t *t;
	int v = 41;
	intptr_t r = 0;
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_async(loop, add_one, &v, &t), ==, XTC_OK);
	munit_assert_int(xtc_await(t, &r), ==, XTC_OK);
	munit_assert_int((int)r, ==, 42);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* [C4] yield round-trip. */
static int yield_count;
static intptr_t yielding_coro(void *arg) {
	int n = *(int *)arg;
	int i;
	for (i = 0; i < n; i++) {
		yield_count++;
		xtc_yield();
	}
	return (intptr_t)yield_count;
}

static MunitResult
test_yield(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_task_t *t;
	int n = 5;
	intptr_t r = 0;
	(void)p; (void)d;
	yield_count = 0;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_async(loop, yielding_coro, &n, &t), ==, XTC_OK);
	munit_assert_int(xtc_await(t, &r), ==, XTC_OK);
	munit_assert_int((int)r, ==, 5);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* [C5] locals survive yield.  This is the headline contract that
 * distinguishes fibers from protothreads. */
static intptr_t locals_test(void *arg) {
	(void)arg;
	int a = 100, b = 200, c = 300;
	xtc_yield();
	munit_assert_int(a, ==, 100);
	xtc_yield();
	munit_assert_int(b, ==, 200);
	xtc_yield();
	munit_assert_int(c, ==, 300);
	return (intptr_t)(a + b + c);
}

static MunitResult
test_locals_survive(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_task_t *t;
	intptr_t r = 0;
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_async(loop, locals_test, NULL, &t), ==, XTC_OK);
	munit_assert_int(xtc_await(t, &r), ==, XTC_OK);
	munit_assert_int((int)r, ==, 600);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* [C9] many coroutines coexist. */
#define MANY 64
struct mctx { int seed; int sum; };
static intptr_t many_coro(void *arg) {
	struct mctx *c = arg;
	int i, s = 0;
	for (i = 0; i < 10; i++) { s += c->seed + i; xtc_yield(); }
	c->sum = s;
	return s;
}

static MunitResult
test_many_coros(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	struct mctx ctxs[MANY];
	xtc_task_t *tasks[MANY];
	int i, expected;
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	for (i = 0; i < MANY; i++) {
		ctxs[i].seed = i;
		ctxs[i].sum = 0;
		munit_assert_int(xtc_async(loop, many_coro, &ctxs[i], &tasks[i]),
		    ==, XTC_OK);
	}
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	for (i = 0; i < MANY; i++) {
		int j;
		expected = 0;
		for (j = 0; j < 10; j++) expected += i + j;
		munit_assert_int(ctxs[i].sum, ==, expected);
	}
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* [C7] coroutine that returns normally is reaped. */
static intptr_t trivial(void *arg) { (void)arg; return 77; }

static MunitResult
test_coro_done(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_task_t *t;
	intptr_t r = 0;
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_async(loop, trivial, NULL, &t), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	/* After loop_run, the coro is done.  Awaiting yields its result. */
	munit_assert_int(xtc_await(t, &r), ==, XTC_OK);
	munit_assert_int((int)r, ==, 77);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* [C3] nested await: a coroutine can spawn and await another. */
static int inner_arg;
static intptr_t inner(void *arg) {
	inner_arg = *(int *)arg;
	return (intptr_t)(inner_arg * 2);
}
struct outer_ctx { xtc_loop_t *loop; int n; };
static intptr_t outer(void *arg) {
	struct outer_ctx *o = arg;
	xtc_task_t *child = NULL;
	intptr_t r = 0;
	(void)xtc_async(o->loop, inner, &o->n, &child);
	(void)xtc_await(child, &r);
	return r + 1;
}

static MunitResult
test_nested(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_task_t *t;
	struct outer_ctx ctx;
	intptr_t r = 0;
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	ctx.loop = loop; ctx.n = 21;
	munit_assert_int(xtc_async(loop, outer, &ctx, &t), ==, XTC_OK);
	munit_assert_int(xtc_await(t, &r), ==, XTC_OK);
	munit_assert_int((int)r, ==, 21 * 2 + 1);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* [P1, P2, P3] protothread basics. */
static int pt_log[8];
static int pt_log_n;
static XTC_PT_THREAD(pt_simple(xtc_pt_t *self, int *runs)) {
	XTC_PT_BEGIN(self);
	pt_log[pt_log_n++] = 1;
	XTC_PT_YIELD(self);
	pt_log[pt_log_n++] = 2;
	XTC_PT_YIELD(self);
	pt_log[pt_log_n++] = 3;
	(*runs)++;
	XTC_PT_END(self);
}

static MunitResult
test_pt_basic(const MunitParameter p[], void *d)
{
	xtc_pt_t pt;
	int runs = 0;
	char rc;
	(void)p; (void)d;
	XTC_PT_INIT(&pt);
	pt_log_n = 0;
	rc = pt_simple(&pt, &runs);   munit_assert_int(rc, ==, XTC_PT_YIELDED);
	rc = pt_simple(&pt, &runs);   munit_assert_int(rc, ==, XTC_PT_YIELDED);
	rc = pt_simple(&pt, &runs);   munit_assert_int(rc, ==, XTC_PT_DONE);
	munit_assert_int(runs, ==, 1);
	munit_assert_int(pt_log_n, ==, 3);
	munit_assert_int(pt_log[0], ==, 1);
	munit_assert_int(pt_log[1], ==, 2);
	munit_assert_int(pt_log[2], ==, 3);
	return MUNIT_OK;
}

/* yield watchdog: a coro over its per-loop time budget is reported by
 * xtc_yield_check, and xtc_yield_if_due lets it cooperate so a peer
 * coro on the same loop makes progress. */
static int g_yc_over;          /* coro saw itself over budget */
static int g_yc_ticks;         /* peer ran while the hog computed */
static int g_yc_hog_done;

static void
busy_ns(int64_t ns)
{
	int64_t start = 0, now = 0;
	(void)__os_clock_mono(&start);
	do { (void)__os_clock_mono(&now); } while (now - start < ns);
}

static intptr_t
yc_hog(void *arg)
{
	int i;
	(void)arg;
	/* ~24ms of compute in ~2ms slices, cooperating each slice. */
	for (i = 0; i < 12; i++) {
		busy_ns(3LL * 1000 * 1000);    /* 3ms > 2ms budget */
		if (xtc_yield_check())
			g_yc_over = 1;
		(void)xtc_yield_if_due();
	}
	g_yc_hog_done = 1;
	return 0;
}

static intptr_t
yc_ticker(void *arg)
{
	(void)arg;
	while (!g_yc_hog_done && g_yc_ticks < 1000) {
		g_yc_ticks++;
		xtc_yield();
	}
	return 0;
}

static MunitResult
test_yield_watchdog(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_task_t *hog = NULL, *tick = NULL;
	(void)p; (void)d;

	g_yc_over = g_yc_ticks = g_yc_hog_done = 0;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	/* Budget set before the first dispatch so the hog's quanta are
	 * timed from the start. */
	xtc_yield_set_budget(loop, 2LL * 1000 * 1000);   /* 2ms */
	munit_assert_int(xtc_async(loop, yc_hog, NULL, &hog), ==, XTC_OK);
	munit_assert_int(xtc_async(loop, yc_ticker, NULL, &tick), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	munit_assert_int(g_yc_over, ==, 1);                 /* saw over-budget */
	munit_assert_uint64(xtc_yield_due_count(loop), >, 0);
	munit_assert_int(g_yc_ticks, >=, 2);                /* peer interleaved */
	munit_assert_int(g_yc_hog_done, ==, 1);

	/* Off a loop / no budget: never due. */
	munit_assert_int(xtc_yield_check(), ==, 0);

	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/*
 * [C10] floating-point control and status are per fiber, as they are per
 * thread: a fiber that changes the rounding mode / flush-to-zero, or
 * raises an exception flag, and yields must not hand that to the next
 * fiber on the thread, and must get its own back on resume.  ucontext
 * (glibc saves MXCSR + x87 env / FPCR + FPSR) always did; the fcontext
 * switch did not until it saved them too.  Registers are read directly
 * (no libm): x86-64 MXCSR + x87 control word, aarch64 FPCR + FPSR.
 */
#if defined(__GNUC__) && (defined(__x86_64__) || defined(__aarch64__))
#define FPE_TEST 1
#if defined(__x86_64__)
static uint32_t fpe_mx(void) { uint32_t m; __asm__ volatile("stmxcsr %0" : "=m"(m)); return m; }
static void fpe_set_mx(uint32_t m) { __asm__ volatile("ldmxcsr %0" :: "m"(m)); }
static uint64_t
fpe_ctl(void)
{
	uint16_t cw;
	__asm__ volatile("fnstcw %0" : "=m"(cw));
	return ((uint64_t)cw << 32) | (fpe_mx() & ~0x3fu);
}
static void
fpe_set_ctl(uint64_t v)
{
	uint16_t cw = (uint16_t)(v >> 32);
	fpe_set_mx(((uint32_t)v & ~0x3fu) | (fpe_mx() & 0x3fu));
	__asm__ volatile("fldcw %0" :: "m"(cw));
}
static uint64_t fpe_flags(void) { return fpe_mx() & 0x3fu; }
static void fpe_set_flags(uint64_t f) { fpe_set_mx((fpe_mx() & ~0x3fu) | (uint32_t)f); }
/* MXCSR RC (13-14) and x87 CW RC (10-11): up = 10b, down = 01b; FTZ 15. */
#define FPE_UP		((0x0800ull << 32) | 0x4000u | 0x8000u)
#define FPE_DOWN	((0x0400ull << 32) | 0x2000u)
#else
static uint64_t fpe_ctl(void) { uint64_t v; __asm__ volatile("mrs %0, fpcr" : "=r"(v)); return v; }
static void fpe_set_ctl(uint64_t v) { __asm__ volatile("msr fpcr, %0" :: "r"(v)); }
static uint64_t fpe_flags(void) { uint64_t v; __asm__ volatile("mrs %0, fpsr" : "=r"(v)); return v & 0x9fu; }
static void fpe_set_flags(uint64_t f) { __asm__ volatile("msr fpsr, %0" :: "r"(f)); }
/* FPCR RMode (22-23): up = 01b, down = 10b; FZ 24, DN 25. */
#define FPE_UP		((1ull << 22) | (1ull << 24))
#define FPE_DOWN	((2ull << 22) | (1ull << 25))
#endif

static uint64_t g_fpe_def, g_fpe_b_start, g_fpe_b_flags, g_fpe_a_end,
    g_fpe_b_end;

static intptr_t
fpe_a(void *arg)
{
	(void)arg;
	fpe_set_ctl(g_fpe_def | FPE_UP);
	xtc_yield();			/* B starts: must not inherit FPE_UP */
	fpe_set_flags(1);		/* invalid-operation flag */
	xtc_yield();			/* B resumes: must not see the flag */
	g_fpe_a_end = fpe_ctl();
	fpe_set_ctl(g_fpe_def);
	return 0;
}

static intptr_t
fpe_b(void *arg)
{
	(void)arg;
	g_fpe_b_start = fpe_ctl();
	fpe_set_ctl(g_fpe_def | FPE_DOWN);
	fpe_set_flags(0);
	xtc_yield();
	g_fpe_b_flags = fpe_flags();
	g_fpe_b_end = fpe_ctl();
	fpe_set_ctl(g_fpe_def);
	return 0;
}

static MunitResult
test_fp_env_per_fiber(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	(void)p; (void)d;
	g_fpe_def = fpe_ctl();
	if ((g_fpe_def & (FPE_UP | FPE_DOWN)) != 0)
		return MUNIT_SKIP;   /* not started in the default modes */
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_async(loop, fpe_a, NULL, NULL), ==, XTC_OK);
	munit_assert_int(xtc_async(loop, fpe_b, NULL, NULL), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	munit_logf(MUNIT_LOG_INFO, "default %#llx: B start %#llx end %#llx "
	    "flags %#llx, A end %#llx", (unsigned long long)g_fpe_def,
	    (unsigned long long)g_fpe_b_start, (unsigned long long)g_fpe_b_end,
	    (unsigned long long)g_fpe_b_flags, (unsigned long long)g_fpe_a_end);
	munit_assert_uint64(g_fpe_b_start, ==, g_fpe_def);   /* fresh: default */
	munit_assert_uint64(g_fpe_b_end, ==, g_fpe_def | FPE_DOWN);
	munit_assert_uint64(g_fpe_b_flags, ==, 0);           /* not A's flag */
	munit_assert_uint64(g_fpe_a_end, ==, g_fpe_def | FPE_UP);
	munit_assert_uint64(fpe_ctl(), ==, g_fpe_def);       /* scheduler's */
	return MUNIT_OK;
}
#endif

#if defined(__linux__)
/*
 * C11: a finished xtc_async coroutine gives its fiber stack back at once.
 * Its task and coro stay until xtc_loop_fini (xtc_await reads the result
 * through them), but the stack did too: 20,000 async/await pairs held
 * 20,000 stacks -- two mappings each, 48 -> 40,048 lines of
 * /proc/self/maps -- so a long-lived loop using xtc_async ran into the
 * kernel's mapping limit after ~32 K of them.  Asserted: 4,000 pairs grow
 * the mapping count by less than 100 (the stacks are pooled and reused),
 * and every result still reads back after the coroutines finished.
 */
static long
c11_maps(void)
{
	FILE *f = fopen("/proc/self/maps", "r");
	char buf[512];
	long n = 0;
	if (f == NULL) return -1;
	while (fgets(buf, sizeof buf, f) != NULL)
		n++;
	fclose(f);
	return n;
}

static intptr_t
c11_child(void *arg)
{
	return (intptr_t)arg * 3;
}

static long g_c11_before, g_c11_after, g_c11_bad;

static intptr_t
c11_parent(void *arg)
{
	xtc_loop_t *loop = arg;
	long i;
	g_c11_before = c11_maps();
	for (i = 0; i < 4000; i++) {
		xtc_task_t *t = NULL;
		intptr_t r = -1;
		if (xtc_async(loop, c11_child, (void *)(intptr_t)i, &t) != XTC_OK ||
		    xtc_await(t, &r) != XTC_OK || r != i * 3)
			g_c11_bad++;
		if (xtc_await(t, &r) != XTC_OK || r != i * 3)
			g_c11_bad++;   /* a second await reads the kept result */
	}
	g_c11_after = c11_maps();
	return 0;
}

static MunitResult
test_async_releases_stack(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_async(loop, c11_parent, loop, NULL), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	munit_logf(MUNIT_LOG_INFO, "/proc/self/maps lines: %ld -> %ld over "
	    "4000 async/await", g_c11_before, g_c11_after);
	munit_assert_long(g_c11_bad, ==, 0);
	munit_assert_long(g_c11_before, >, 0);
	munit_assert_long(g_c11_after - g_c11_before, <, 100);
	return MUNIT_OK;
}
#endif

static MunitTest tests[] = {
#if defined(__linux__)
	{ "/C11_async_releases_stack", test_async_releases_stack, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
#endif
#if defined(FPE_TEST)
	{ "/C10_fp_env_per_fiber", test_fp_env_per_fiber, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
#endif
	{ "/C1_C2_basic",       test_basic,           NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/C3_nested",         test_nested,          NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/C4_yield",          test_yield,           NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/C5_locals_survive", test_locals_survive,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/C9_many_coros",     test_many_coros,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/C7_coro_done",      test_coro_done,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/P1_pt_basic",       test_pt_basic,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/yield_watchdog",    test_yield_watchdog,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m4/async", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
