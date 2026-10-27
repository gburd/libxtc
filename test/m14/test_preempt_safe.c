/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/m14/test_preempt_safe.c
 *	Involuntary preemption yields a fiber only at a SAFE POINT: a PC in
 *	a preemptible range (the main executable's code, XTC_PREEMPTIBLE
 *	functions, xtc_preempt_region_add ranges), never in libc, another
 *	shared library or libxtc (src/ptc/preempt.c, SAFE POINTS).
 *
 *	A kicker thread pthread_kill()s the preemption signal into the loop
 *	thread every 5 us (the CPU-time timer only fires at the kernel's
 *	scheduler tick, far too rarely to hit a window), while fibers on the
 *	loop work inside libc:
 *	  /libc_malloc  4 fibers malloc/free tcache-sized blocks.  Before the
 *	                check, a fiber preempted inside malloc let a
 *	                loop-mate into the same per-thread tcache: "malloc():
 *	                unaligned tcache chunk detected" or SIGSEGV within a
 *	                second, every run.
 *	  /libc_stdio   4 fibers fprintf to one FILE.  The FILE lock is
 *	                recursive per THREAD, so a loop-mate of a preempted
 *	                writer walked in: torn lines (~1000 of 750k).
 *	  /fairness     a runaway that spends most of its time in memset
 *	                (libc) and the rest in this file's own loop is
 *	                still sliced: loop-mates progress.
 *	  /classify     the classification itself: this file's code is
 *	                preemptible, a libc function and a libxtc function
 *	                are not, a region added at run time is.
 *	  /tick_cost    the cost of the check per tick (reported, plus a
 *	                loose bound).
 *	Linux x86-64 / aarch64 only (where the redirect exists); skips
 *	elsewhere and under sanitizers (see test_preempt_p2).
 */

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <signal.h>
#include <time.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_async.h"
#include "xtc_preempt.h"
#if !defined(PS_BEFORE)
#include "xtc_preempt_involuntary.h"
#endif

#if !defined(__has_feature)
#  define __has_feature(x) 0
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__) || \
    __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#  define PS_SANITIZER 1
#endif

#if defined(__linux__) && (defined(__x86_64__) || defined(__aarch64__)) && \
    (defined(__GNUC__) || defined(__clang__)) && !defined(PS_SANITIZER)
#define PS_ON 1

extern int __xtc_preempt_pc_ok(uintptr_t pc);

/* PS_BEFORE: build the libc cases against a library from before the
 * safe-point check (void set_involuntary, no involuntary header) to show
 * them failing there: cc -DPS_BEFORE ... */
#if defined(PS_BEFORE)
#define XTC_PS_ENABLE() xtc_preempt_set_involuntary(1)
#else
#define XTC_PS_ENABLE() \
	munit_assert_int(xtc_preempt_set_involuntary(1), ==, XTC_OK)
#endif

static _Atomic int g_ready, g_done;
static pthread_t   g_loop_thread;
static _Atomic long g_ops, g_bad;
static long        g_kick_us = 5;

/* The kicker: SIGVTALRM into the loop thread every g_kick_us while the
 * workers run.  The handler is the library's (xtc_preempt_arm installed
 * it); a long timer keeps the CPU-time timer out of the way. */
static void *
ps_kicker(void *arg)
{
	struct timespec ts;
	(void)arg;
	ts.tv_sec = 0;
	ts.tv_nsec = g_kick_us * 1000L;
	while (!atomic_load(&g_done)) {
		if (atomic_load(&g_ready))
			(void)pthread_kill(g_loop_thread, SIGVTALRM);
		(void)nanosleep(&ts, NULL);
	}
	return NULL;
}

/* Run n copies of fn on one loop with the kicker going. */
static void
ps_run(void (*fn)(void *), int n)
{
	xtc_exec_t *e = NULL;
	pthread_t kt;
	intptr_t i;
	atomic_store(&g_ready, 0);
	atomic_store(&g_done, 0);
	atomic_store(&g_ops, 0);
	atomic_store(&g_bad, 0);
	munit_assert_int(xtc_exec_init(&e, 1), ==, XTC_OK);
	munit_assert_int(xtc_exec_set_preempt(e, 10LL * 1000 * 1000 * 1000),
	    ==, XTC_OK);
	XTC_PS_ENABLE();
	for (i = 0; i < n; i++)
		munit_assert_int(xtc_proc_spawn(xtc_exec_loop(e, 0), fn,
		    (void *)i, NULL, NULL), ==, XTC_OK);
	munit_assert_int(pthread_create(&kt, NULL, ps_kicker, NULL), ==, 0);
	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);
	atomic_store(&g_done, 1);
	(void)pthread_join(kt, NULL);
	(void)xtc_preempt_set_involuntary(0);
	(void)xtc_exec_fini(e);
}

static MunitResult
ps_skip(void)
{
#if defined(PS_BEFORE)
	if (!xtc_preempt_supported())
#else
	if (!xtc_preempt_supported() || !xtc_preempt_involuntary_supported())
#endif
		return MUNIT_SKIP;
	return MUNIT_OK;
}

XTC_PREEMPTIBLE static void
ps_note_loop(intptr_t id)
{
	if (id == 0) {
		g_loop_thread = pthread_self();
		atomic_store(&g_ready, 1);
	}
}

/* ---- /libc_malloc ---- */
XTC_PREEMPTIBLE static void
ps_malloc_worker(void *arg)
{
	intptr_t id = (intptr_t)arg;
	unsigned s = (unsigned)id * 2654435761u + 1;
	unsigned char *p[16];
	int64_t end;
	int i;
	ps_note_loop(id);
	memset(p, 0, sizeof p);
	end = xtc_clock_mono() + 500LL * 1000 * 1000;
	while (xtc_clock_mono() < end) {
		for (i = 0; i < 16; i++) {
			s = s * 1103515245u + 12345u;
			if (p[i] != NULL) {
				if (p[i][0] != (unsigned char)i)
					atomic_fetch_add(&g_bad, 1);
				free(p[i]);   /* XTC_RAW_OK: the libc allocator under test */
			}
			p[i] = malloc(16 + (s >> 24) % 200);   /* XTC_RAW_OK: as above */
			munit_assert_not_null(p[i]);
			p[i][0] = (unsigned char)i;
		}
		atomic_fetch_add(&g_ops, 16);
		if ((s & 255) == 0)
			xtc_yield();
	}
	for (i = 0; i < 16; i++)
		free(p[i]);   /* XTC_RAW_OK: as above */
	if (id == 0)
		atomic_store(&g_ready, 0);
}

static MunitResult
test_ps_libc_malloc(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	if (ps_skip() != MUNIT_OK)
		return MUNIT_SKIP;
	ps_run(ps_malloc_worker, 4);
	munit_logf(MUNIT_LOG_INFO, "%ld malloc/free pairs, %ld bad",
	    atomic_load(&g_ops), atomic_load(&g_bad));
	munit_assert_long(atomic_load(&g_ops), >, 0);
	munit_assert_long(atomic_load(&g_bad), ==, 0);
	return MUNIT_OK;
}

/* ---- /libc_stdio ---- */
static FILE  *g_f;
static char  *g_buf;
#define PS_BUFSZ (64u << 20)
static const char ps_tail[] = "abcdefghijklmnopqrstuvwxyz";

XTC_PREEMPTIBLE static void
ps_stdio_worker(void *arg)
{
	intptr_t id = (intptr_t)arg;
	int64_t end;
	long seq = 0;
	ps_note_loop(id);
	end = xtc_clock_mono() + 500LL * 1000 * 1000;
	while (xtc_clock_mono() < end && seq < 150000) {
		(void)fprintf(g_f, "fiber %d seq %ld %s\n", (int)id, seq++,
		    ps_tail);
		if ((seq & 1023) == 0)
			xtc_yield();
	}
	atomic_fetch_add(&g_ops, seq);
	if (id == 0)
		atomic_store(&g_ready, 0);
}

static MunitResult
test_ps_libc_stdio(const MunitParameter p[], void *d)
{
	char *q, *nl, tail[64];
	long lines = 0, bad = 0, seq;
	int id;
	(void)p; (void)d;
	if (ps_skip() != MUNIT_OK)
		return MUNIT_SKIP;
	g_buf = calloc(1, PS_BUFSZ);   /* XTC_RAW_OK: test scratch */
	munit_assert_not_null(g_buf);
	g_f = fmemopen(g_buf, PS_BUFSZ, "w");
	munit_assert_not_null(g_f);
	ps_run(ps_stdio_worker, 4);
	(void)fflush(g_f);
	for (q = g_buf; *q != '\0'; q = nl + 1) {
		nl = strchr(q, '\n');
		if (nl == NULL) {
			bad++;
			break;
		}
		*nl = '\0';
		lines++;
		if (sscanf(q, "fiber %d seq %ld %63s", &id, &seq, tail) != 3 ||
		    strcmp(tail, ps_tail) != 0)
			bad++;
	}
	(void)fclose(g_f);
	free(g_buf);   /* XTC_RAW_OK: as above */
	munit_logf(MUNIT_LOG_INFO, "%ld lines written, %ld parsed, %ld torn",
	    atomic_load(&g_ops), lines, bad);
	munit_assert_long(lines, ==, atomic_load(&g_ops));
	munit_assert_long(bad, ==, 0);
	return MUNIT_OK;
}

/* ---- /fairness ---- */
static _Atomic int  g_runaway_done;
static _Atomic long g_peer_runs;
static volatile unsigned char g_scratch[1 << 16];

XTC_PREEMPTIBLE static void
ps_runaway(void *arg)
{
	uint64_t acc = 0, k;
	int64_t end = xtc_clock_mono() + 300LL * 1000 * 1000;
	(void)arg;
	ps_note_loop(0);
	while (xtc_clock_mono() < end) {
		memset((void *)g_scratch, (int)(acc & 0xff), sizeof g_scratch);
		for (k = 0; k < 2000; k++)   /* this file's own code */
			acc += (k ^ (acc << 1)) + (acc >> 3);
	}
	g_scratch[0] = (unsigned char)acc;
	atomic_store(&g_runaway_done, 1);
	atomic_store(&g_ready, 0);
}

XTC_PREEMPTIBLE static void
ps_peer(void *arg)
{
	(void)arg;
	while (!atomic_load(&g_runaway_done)) {
		atomic_fetch_add(&g_peer_runs, 1);
		xtc_yield();
	}
}

XTC_PREEMPTIBLE static void
ps_fair_spawn(void *arg)
{
	intptr_t i = (intptr_t)arg;
	if (i == 0)
		ps_runaway(arg);
	else
		ps_peer(arg);
}

static MunitResult
test_ps_fairness(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	if (ps_skip() != MUNIT_OK)
		return MUNIT_SKIP;
	atomic_store(&g_runaway_done, 0);
	atomic_store(&g_peer_runs, 0);
	ps_run(ps_fair_spawn, 3);
	munit_logf(MUNIT_LOG_INFO, "peers ran %ld times while the runaway "
	    "(memset + own loop) held the loop", atomic_load(&g_peer_runs));
	/* Each slice of the runaway lets both peers run once. */
	munit_assert_long(atomic_load(&g_peer_runs), >, 100);
	return MUNIT_OK;
}

#if !defined(PS_BEFORE)
/* ---- /classify ---- */
static volatile int g_cls_sink;
XTC_PREEMPTIBLE static void ps_classify_dummy(void) { g_cls_sink = 1; }
static void ps_unmarked(void) { g_cls_sink = 2; }   /* distinct body: no ICF */
static unsigned char g_jit[64];
static uintptr_t g_libc_pc;   /* a return address inside libc's qsort */

/* A PC inside libc: the comparator's return address points into qsort.
 * (Not &memcpy: in a non-PIE executable that is a PLT stub in the main
 * program.) */
static int
ps_cmp(const void *a, const void *b)
{
	g_libc_pc = (uintptr_t)__builtin_return_address(0);
	return *(const int *)a - *(const int *)b;
}

static MunitResult
test_ps_classify(const MunitParameter p[], void *d)
{
	int v[4] = { 3, 1, 4, 2 };
	(void)p; (void)d;
	if (ps_skip() != MUNIT_OK)
		return MUNIT_SKIP;
	munit_assert_int(xtc_preempt_set_involuntary(1), ==, XTC_OK);
	munit_assert_int(__xtc_preempt_pc_ok(
	    (uintptr_t)&ps_classify_dummy), ==, 1);   /* XTC_PREEMPTIBLE */
	/* This binary links libxtc.a, so the rest of the executable is not
	 * preemptible by default (static libxtc: mark or add). */
	munit_assert_int(__xtc_preempt_pc_ok((uintptr_t)&ps_unmarked), ==, 0);
	qsort(v, 4, sizeof v[0], ps_cmp);
	munit_assert_int(__xtc_preempt_pc_ok(g_libc_pc), ==, 0);   /* libc */
	munit_assert_int(__xtc_preempt_pc_ok((uintptr_t)&xtc_yield), ==, 0);
	munit_assert_int(__xtc_preempt_pc_ok(
	    (uintptr_t)&xtc_preempt_set_involuntary), ==, 0);   /* libxtc */
	/* A data range (a stand-in for a JIT buffer) is not preemptible
	 * until added; libxtc's own text can never be added. */
	munit_assert_int(__xtc_preempt_pc_ok((uintptr_t)&g_jit[8]), ==, 0);
	munit_assert_int(xtc_preempt_region_add(g_jit, g_jit + sizeof g_jit),
	    ==, XTC_OK);
	munit_assert_int(__xtc_preempt_pc_ok((uintptr_t)&g_jit[8]), ==, 1);
	munit_assert_int(__xtc_preempt_pc_ok((uintptr_t)&g_jit[64]), ==, 0);
	munit_assert_int(xtc_preempt_region_add(g_jit, g_jit), ==,
	    XTC_E_INVAL);
	munit_assert_int(xtc_preempt_region_add(NULL, g_jit), ==,
	    XTC_E_INVAL);
	/* A range over libc (around the PC qsort returned to) is refused. */
	munit_assert_int(xtc_preempt_region_add((const void *)(g_libc_pc - 64),
	    (const void *)(g_libc_pc + 64)), ==, XTC_E_INVAL);

	(void)xtc_preempt_set_involuntary(0);
	return MUNIT_OK;
}

/* ---- /tick_cost ---- */
static MunitResult
test_ps_tick_cost(const MunitParameter p[], void *d)
{
	int64_t t0, t1;
	volatile int sink = 0;
	long k, n = 2000000;
	uintptr_t pcs[3];
	(void)p; (void)d;
	if (ps_skip() != MUNIT_OK)
		return MUNIT_SKIP;
	munit_assert_int(xtc_preempt_set_involuntary(1), ==, XTC_OK);
	pcs[0] = (uintptr_t)&ps_classify_dummy;
	pcs[1] = (uintptr_t)&g_jit[0];
	pcs[2] = (uintptr_t)&xtc_yield;
	t0 = xtc_clock_mono();
	for (k = 0; k < n; k++)
		sink += __xtc_preempt_pc_ok(pcs[k % 3]);
	t1 = xtc_clock_mono();
	(void)xtc_preempt_set_involuntary(0);
	munit_logf(MUNIT_LOG_INFO, "safe-point check: %.1f ns per tick",
	    (double)(t1 - t0) / (double)n);
	munit_assert_int(sink, >, 0);
	/* A tick costs microseconds (signal delivery and sigreturn); the
	 * check must stay a small fraction of that even under valgrind. */
	munit_assert_long((t1 - t0) / n, <, 2000);
	return MUNIT_OK;
}
#endif /* !PS_BEFORE */
#endif /* PS_ON */

static MunitTest tests[] = {
#if defined(PS_ON)
	{ "/libc_malloc", test_ps_libc_malloc, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/libc_stdio", test_ps_libc_stdio, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/fairness", test_ps_fairness, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
#if !defined(PS_BEFORE)
	{ "/classify", test_ps_classify, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/tick_cost", test_ps_tick_cost, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
#endif
#endif
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite suite = {
	"/m14/preempt_safe", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

int
main(int argc, char *argv[])
{
	return munit_suite_main(&suite, NULL, argc, argv);
}
