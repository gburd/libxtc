/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/m14/test_stack_reclaim.c
 *	Lever S1 -- stack-memory reclamation on park (M_PREEMPTION 8).
 *
 *	Proves:
 *	  (a) OFF by default: no madvise happens with reclaim disabled;
 *	  (b) ENABLED: a fiber that uses a deep stack frame then parks
 *	      returns its unused tail to the OS (reclaim count rises);
 *	  (c) CORRECTNESS: a fiber that writes a sentinel into a deep
 *	      stack buffer, parks (its tail is MADV_DONTNEED'd), resumes,
 *	      and reads/rewrites that buffer sees consistent behavior --
 *	      the reclaim never crosses the live frame, and the faulted-
 *	      back tail is usable again (zero-fill on refault).  A bug in
 *	      the reclaim boundary would corrupt the resumed fiber (caught
 *	      by the post-resume assertions here, and by ASan).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_async.h"

/* Detect AddressSanitizer portably (nested #if -- a bare
 * __has_feature(...) breaks on gcc where __has_feature is undefined). */
#if defined(__SANITIZE_ADDRESS__)
# define XTC_UNDER_ASAN 1
#elif defined(__has_feature)
# if __has_feature(address_sanitizer)
#  define XTC_UNDER_ASAN 1
# endif
#endif

#define DEEP_BYTES 16384    /* a frame well above one page, so parking it
                             * leaves a clearly-reclaimable tail */

static atomic_int g_ok;      /* fibers that round-tripped their sentinel */
static atomic_int g_done;

/* Force the compiler not to optimize the deep buffer away. */
static void
touch(volatile unsigned char *p, size_t n, unsigned char v)
{
	size_t i;
	for (i = 0; i < n; i += 512)   /* touch each page-ish */
		p[i] = v;
}

static unsigned char
sum_probe(volatile unsigned char *p, size_t n)
{
	unsigned char s = 0;
	size_t i;
	for (i = 0; i < n; i += 512)
		s = (unsigned char)(s + p[i]);
	return s;
}

/*
 * A worker: build a deep frame, stamp it, PARK (yield) -- at which point
 * S1 may reclaim the unused tail below our current SP -- then resume and
 * verify the LIVE frame survived and is still writable.  Repeats so many
 * park/reclaim/resume cycles run.
 */
static void
worker(void *arg)
{
	int rounds = (int)(intptr_t)arg;
	int r;
	int good = 1;

	for (r = 0; r < rounds; r++) {
		volatile unsigned char deep[DEEP_BYTES];
		unsigned char want = (unsigned char)(0xA0 + (r & 0x0f));

		/* Stamp the deep live frame, then park.  The live frame is
		 * BELOW our SP after the call chain, so the reclaim (which
		 * only touches the tail above SP minus a margin) must not
		 * disturb `deep`. */
		touch(deep, sizeof deep, want);
		xtc_yield();
		/* Resumed: the live frame must be intact. */
		if (sum_probe(deep, sizeof deep) !=
		    (unsigned char)(want * (unsigned char)(DEEP_BYTES / 512)))
			good = 0;
		/* Re-stamp with a new value and park again -- exercises the
		 * refaulted tail on a subsequent deeper/shallower call. */
		touch(deep, sizeof deep, (unsigned char)(want ^ 0x5a));
		xtc_yield();
		if (sum_probe(deep, sizeof deep) !=
		    (unsigned char)((unsigned char)(want ^ 0x5a) *
		        (unsigned char)(DEEP_BYTES / 512)))
			good = 0;
	}
	if (good)
		atomic_fetch_add(&g_ok, 1);
	atomic_fetch_add(&g_done, 1);
}

/* A driver that keeps the loop busy so the workers actually park+resume
 * against each other (their yields hand off). */
static void
driver(void *arg)
{
	int n = (int)(intptr_t)arg;
	while (atomic_load(&g_done) < n)
		xtc_yield();
}

static MunitResult
run_workers(int n, int rounds)
{
	xtc_exec_t *e = NULL;
	xtc_loop_t *l;
	int i;

	atomic_store(&g_ok, 0);
	atomic_store(&g_done, 0);

	munit_assert_int(xtc_exec_init(&e, 1), ==, XTC_OK);
	l = xtc_exec_loop(e, 0);
	for (i = 0; i < n; i++)
		(void)xtc_proc_spawn(l, worker, (void *)(intptr_t)rounds,
		    NULL, NULL);
	(void)xtc_proc_spawn(l, driver, (void *)(intptr_t)n, NULL, NULL);
	munit_assert_int(xtc_exec_run(e), ==, XTC_OK);

	munit_assert_int(atomic_load(&g_done), ==, n);
	munit_assert_int(atomic_load(&g_ok), ==, n);   /* all sentinels survived */
	(void)xtc_exec_fini(e);
	return MUNIT_OK;
}

/* (a) OFF by default: no reclaim happens. */
static MunitResult
test_off_by_default(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	munit_assert_int(xtc_stack_reclaim_enabled(), ==, 0);
	munit_assert_uint64(xtc_stack_reclaim_count(), ==, 0);
	/* Run a workload with reclaim off; count must stay 0. */
	if (run_workers(4, 8) != MUNIT_OK)
		return MUNIT_FAIL;
	munit_assert_uint64(xtc_stack_reclaim_count(), ==, 0);
	return MUNIT_OK;
}

/* (b)+(c) ENABLED: reclaim fires AND every fiber round-trips correctly. */
static MunitResult
test_reclaim_correct(const MunitParameter p[], void *d)
{
	uint64_t before, after;
	(void)p; (void)d;

	if (xtc_stack_reclaim_enable(0) != XTC_OK) {
		/* No MADV_DONTNEED on this platform: the API declines and the
		 * feature is a documented no-op.  Nothing to prove. */
		munit_assert_int(xtc_stack_reclaim_enabled(), ==, 0);
		return MUNIT_OK;
	}
	munit_assert_int(xtc_stack_reclaim_enabled(), ==, 1);

	before = xtc_stack_reclaim_count();
	if (run_workers(4, 16) != MUNIT_OK) {
		xtc_stack_reclaim_disable();
		return MUNIT_FAIL;
	}
	after = xtc_stack_reclaim_count();

	/* Deep frames (16 KiB) parked many times -> the tail above SP is
	 * well over a page, so reclaim must have fired at least once.
	 * EXCEPT under AddressSanitizer: ASan relocates fiber frames to a
	 * heap fake-stack (detect_stack_use_after_return), so the running
	 * SP is not inside the fiber's mmap'd stack and the reclaim
	 * correctly DECLINES (its geometry check finds SP out of range) --
	 * a safe no-op, not a bug.  There the round-trip correctness (all
	 * sentinels survived, asserted in run_workers) is the thing that
	 * matters; the fire-count is expected to stay 0. */
#ifdef XTC_UNDER_ASAN
	(void)before; (void)after;
#else
	munit_assert_uint64(after, >, before);
#endif

	xtc_stack_reclaim_disable();
	munit_assert_int(xtc_stack_reclaim_enabled(), ==, 0);
	return MUNIT_OK;
}


/* ---- B3: completed proc fibers are reclaimed before loop teardown ----
 *
 * Every proc used to keep its fiber stack (two VMAs: stack + guard) and
 * its coro until xtc_loop_fini, so a server spawning one proc per
 * connection grew without bound and, on a stock kernel
 * (vm.max_map_count 65530), failed every spawn after ~32K lifetime
 * spawns.  These cases spawn many short-lived procs with a bounded number
 * live and assert that address-space mappings and RSS track the LIVE
 * count, not the history.  Linux only (they read /proc/self). */
#if defined(__linux__)
#include <stdio.h>

#define LIFE_LIVE   256

/* ASan's allocator quarantine (256 MB by default) and its own mappings
 * show up in RSS and /proc/self/maps.  Under ASan, check mappings with a
 * bound that still catches the regression (two per HISTORICAL spawn,
 * i.e. +200000) and skip the RSS bound; the release build checks both
 * tightly. */
#ifdef XTC_UNDER_ASAN
# define LIFE_MAPS_SLACK  8192
# define LIFE_CHECK_RSS   0
#else
# define LIFE_MAPS_SLACK  64
# define LIFE_CHECK_RSS   1
#endif

static long
life_status_kb(const char *key)
{
	FILE *f = fopen("/proc/self/status", "r");
	char line[256];
	long v = -1;
	size_t kl = strlen(key);
	if (f == NULL) return -1;
	while (fgets(line, sizeof line, f) != NULL)
		if (strncmp(line, key, kl) == 0) {
			v = atol(line + kl);
			break;
		}
	fclose(f);
	return v;
}

static long
life_maps(void)
{
	FILE *f = fopen("/proc/self/maps", "r");
	long n = 0;
	int c;
	if (f == NULL) return -1;
	while ((c = fgetc(f)) != EOF)
		n += (c == '\n');
	fclose(f);
	return n;
}

static long          life_n;          /* procs to spawn in total */
static atomic_long   life_live;       /* spawned and not yet exited */
static atomic_long   life_started;
static atomic_long   life_exited;
static long          life_spawn_fail; /* first failing index, or -1 */
static int           life_mode;       /* 0 return, 1 exit_self,
                                       * 2 killed parked, 3 killed running */
static long          life_maps_peak, life_rss_peak;

/* Runs on every exit path: return, xtc_exit_self, and a kill unwind. */
static void
life_on_exit(void *arg)
{
	(void)arg;
	atomic_fetch_sub(&life_live, 1);
	atomic_fetch_add(&life_exited, 1);
}

static void
life_child(void *arg)
{
	int i;
	(void)arg;
	(void)xtc_proc_at_exit(life_on_exit, NULL);
	atomic_fetch_add(&life_started, 1);
	switch (life_mode) {
	case 1:
		xtc_yield();
		(void)xtc_exit_self(3);
		break;
	case 2:
		(void)xtc_proc_sleep((int64_t)60 * 1000000000);
		break;
	case 3:
		for (i = 0; i < 1000000; i++)
			xtc_yield();
		break;
	default:
		xtc_yield();
		break;
	}
}

static void
life_sample(void)
{
	long m = life_maps(), r = life_status_kb("VmRSS:");
	if (m > life_maps_peak) life_maps_peak = m;
	if (r > life_rss_peak) life_rss_peak = r;
}

static void
life_spawner(void *arg)
{
	xtc_loop_t *l = arg;
	xtc_pid_t pids[LIFE_LIVE];
	long i = 0;
	int k, j;

	while (i < life_n) {
		if (life_mode < 2) {
			/* Steady state: keep up to LIFE_LIVE alive. */
			while (atomic_load(&life_live) >= LIFE_LIVE)
				xtc_yield();
			atomic_fetch_add(&life_live, 1);
			if (xtc_proc_spawn(l, life_child, NULL, NULL,
			    NULL) != XTC_OK) {
				atomic_fetch_sub(&life_live, 1);
				life_spawn_fail = i;
				return;
			}
			if ((i & 1023) == 0)
				life_sample();
			i++;
			continue;
		}
		/* Kill modes: a batch of LIFE_LIVE parked (2) or runnable (3)
		 * procs, all started, then all killed. */
		for (k = 0; k < LIFE_LIVE && i < life_n; k++, i++) {
			atomic_fetch_add(&life_live, 1);
			if (xtc_proc_spawn(l, life_child, NULL, NULL,
			    &pids[k]) != XTC_OK) {
				atomic_fetch_sub(&life_live, 1);
				life_spawn_fail = i;
				return;
			}
		}
		while (atomic_load(&life_started) < i)
			xtc_yield();
		life_sample();
		for (j = 0; j < k; j++)
			(void)xtc_exit_pid(pids[j], 9);
		while (atomic_load(&life_exited) < i)
			xtc_yield();
	}
}

static MunitResult
life_run(long n, int mode)
{
	xtc_loop_t *l = NULL;
	long m0, r0, m1, r1;
	long maps_grow, rss_grow;

	life_n = n;
	life_mode = mode;
	life_spawn_fail = -1;
	atomic_store(&life_live, 0);
	atomic_store(&life_started, 0);
	atomic_store(&life_exited, 0);

	munit_assert_int(xtc_loop_init(&l), ==, XTC_OK);
	m0 = life_maps();
	r0 = life_status_kb("VmRSS:");
	life_maps_peak = m0;
	life_rss_peak = r0;
	munit_assert_int(xtc_proc_spawn(l, life_spawner, l, NULL, NULL), ==,
	    XTC_OK);
	munit_assert_int(xtc_loop_run(l), ==, XTC_OK);
	life_sample();
	m1 = life_maps();
	r1 = life_status_kb("VmRSS:");
	maps_grow = life_maps_peak - m0;
	rss_grow = life_rss_peak - r0;
	/* stdout, so the numbers show on a passing run too. */
	printf("  mode %d: %ld spawned, %ld exited, maps +%ld (peak), "
	    "rss +%ld kB (peak); at end maps +%ld rss +%ld kB\n",
	    mode, n, (long)atomic_load(&life_exited), maps_grow, rss_grow,
	    m1 - m0, r1 - r0);
	fflush(stdout);
	(void)xtc_loop_fini(l);

	munit_assert_long(life_spawn_fail, ==, -1);
	munit_assert_long(atomic_load(&life_exited), ==, n);
	/* Two VMAs per live fiber stack, plus the bounded stack pool and
	 * allocator slack -- not two per historical spawn. */
	munit_assert_long(maps_grow, <, 2 * LIFE_LIVE + LIFE_MAPS_SLACK);
	if (LIFE_CHECK_RSS)
		munit_assert_long(rss_grow, <, 32 * 1024);
	return MUNIT_OK;
}

/*
 * Migratable procs on a 4-loop executor: children are stolen and many
 * finish on a thief loop, so their task structs go back to the home loop
 * through its dead stack.  Each carrier thread keeps up to
 * XTC_STACK_POOL_MAX (64) stacks pooled, two mappings each, and the
 * pools are unmapped when the carriers exit.
 */
#define LIFE_LOOPS  4

static xtc_exec_t   *life_exec;
static atomic_long   life_mig_done;

static void
life_mig_child(void *arg)
{
	int i;
	(void)arg;
	for (i = 0; i < 4; i++)
		xtc_yield();
	atomic_fetch_sub(&life_live, 1);
	atomic_fetch_add(&life_mig_done, 1);
}

static void
life_mig_spawner(void *arg)
{
	xtc_loop_t *l = arg;
	xtc_proc_opts_t o;
	long i;

	memset(&o, 0, sizeof o);
	o.migratable = 1;
	for (i = 0; i < life_n; i++) {
		while (atomic_load(&life_live) >= LIFE_LIVE)
			xtc_yield();
		atomic_fetch_add(&life_live, 1);
		if (xtc_proc_spawn(l, life_mig_child, NULL, &o, NULL) !=
		    XTC_OK) {
			atomic_fetch_sub(&life_live, 1);
			life_spawn_fail = i;
			break;
		}
		if ((i & 1023) == 0)
			life_sample();
	}
	while (atomic_load(&life_live) > 0)
		xtc_yield();
	life_sample();
	(void)xtc_exec_stop(life_exec);
}

static MunitResult
test_reclaim_migratable(const MunitParameter p[], void *d)
{
	long m0, r0, m1, maps_grow, rss_grow;
	int64_t steals = 0;
	int i;
	(void)p; (void)d;

	life_n = 100000;
	life_spawn_fail = -1;
	atomic_store(&life_live, 0);
	atomic_store(&life_mig_done, 0);
	munit_assert_int(xtc_exec_init(&life_exec, LIFE_LOOPS), ==, XTC_OK);
	xtc_exec_set_service_mode(life_exec, 1);
	m0 = life_maps();
	r0 = life_status_kb("VmRSS:");
	life_maps_peak = m0;
	life_rss_peak = r0;
	munit_assert_int(xtc_proc_spawn(xtc_exec_loop(life_exec, 0),
	    life_mig_spawner, xtc_exec_loop(life_exec, 0), NULL, NULL), ==,
	    XTC_OK);
	munit_assert_int(xtc_exec_run(life_exec), ==, XTC_OK);
	for (i = 0; i < LIFE_LOOPS; i++) {
		xtc_loop_stats_t st;
		if (xtc_exec_loop_stats(life_exec, i, &st) == XTC_OK)
			steals += (int64_t)st.steals;
	}
	(void)xtc_exec_fini(life_exec);
	m1 = life_maps();
	maps_grow = life_maps_peak - m0;
	rss_grow = life_rss_peak - r0;
	printf("  migratable: %ld done, %lld steals, maps +%ld (peak), "
	    "rss +%ld kB (peak); after exec_fini maps %+ld\n",
	    (long)atomic_load(&life_mig_done), (long long)steals, maps_grow,
	    rss_grow, m1 - m0);
	fflush(stdout);

	munit_assert_long(life_spawn_fail, ==, -1);
	munit_assert_long(atomic_load(&life_mig_done), ==, life_n);
	munit_assert_long(maps_grow, <,
	    2 * LIFE_LIVE + LIFE_LOOPS * 2 * 64 + LIFE_MAPS_SLACK);
	if (LIFE_CHECK_RSS)
		munit_assert_long(rss_grow, <, 32 * 1024);
	/* Carrier threads have exited: their stack pools are unmapped. */
	munit_assert_long(m1 - m0, <, LIFE_MAPS_SLACK);
	return MUNIT_OK;
}

static MunitResult
test_reclaim_spawn_exit(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	return life_run(100000, 0);
}

static MunitResult
test_reclaim_exit_self(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	return life_run(100000, 1);
}

static MunitResult
test_reclaim_kill_parked(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	return life_run(100000, 2);
}

static MunitResult
test_reclaim_kill_running(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	return life_run(100000, 3);
}
#endif /* __linux__ */

static MunitTest tests[] = {
	{ "/off_by_default", test_off_by_default, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/reclaim_correct", test_reclaim_correct, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
#if defined(__linux__)
	{ "/spawn_exit_100k", test_reclaim_spawn_exit, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/exit_self_100k", test_reclaim_exit_self, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/kill_parked_100k", test_reclaim_kill_parked, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/kill_running_100k", test_reclaim_kill_running, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/migratable_100k", test_reclaim_migratable, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
#endif
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite suite = {
	"/m14/stack_reclaim", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

int
main(int argc, char *argv[])
{
	return munit_suite_main(&suite, NULL, argc, argv);
}
