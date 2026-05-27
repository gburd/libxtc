/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * bench/conformance/w7_timer/xtc/main.c
 *   M17 W7 -- timer wheel benchmark, xtc runtime.
 *
 *   Three phases:
 *     1. Schedule N timers at random offsets in [1 ms, 10 s].
 *        Record per-call latency of xtc_timer_set.
 *     2. Cancel N/2 timers chosen at random (Fisher-Yates shuffle).
 *        Record per-call latency of xtc_timer_cancel.
 *     3. Drive the event loop until all remaining N/2 timers fire.
 *        Record fire-accuracy: how many nanoseconds late each timer
 *        fired relative to its scheduled deadline.
 *
 *   Emits three M17 result lines on stdout:
 *     workload=W7 runtime=xtc_schedule ...
 *     workload=W7 runtime=xtc_cancel  ...
 *     workload=W7 runtime=xtc_fire    ...
 *
 * Build:
 *   cd bench/conformance/w7_timer/xtc && make
 *
 * Usage:
 *   ./bench                   # N=100000 (default)
 *   ./bench --N=10000
 *   ./bench --params=N=10000
 */

#define HIST_IMPLEMENTATION
#include "hist.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>

#include "xtc.h"
#include "xtc_loop.h"

/* ---------------------------------------------------------------------- */
/* Global state shared with timer callbacks                                 */
/* ---------------------------------------------------------------------- */

static xtc_loop_t  *g_loop;
static int64_t     *g_deadline;   /* [N] absolute fire deadline, ns        */
static hist_t       g_fire_hist;  /* fire-accuracy histogram               */
static atomic_int   g_remaining;  /* non-cancelled timers not yet fired    */

/* ---------------------------------------------------------------------- */
/* Timing helpers                                                           */
/* ---------------------------------------------------------------------- */

static int64_t
mono_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * INT64_C(1000000000)
	     + (int64_t)ts.tv_nsec;
}

/* ---------------------------------------------------------------------- */
/* Pseudo-random helpers (no external deps)                                 */
/* ---------------------------------------------------------------------- */

/*
 * rand64 -- XOR-shift64 PRNG seeded from the global rand() state.
 * Three rand() calls guarantee at least 45 bits of entropy (POSIX
 * RAND_MAX >= 32767) and typically 93 bits on Linux (RAND_MAX = 2^31-1).
 */
static uint64_t
rand64(void)
{
	return ((uint64_t)(unsigned int)rand() << 33)
	     ^ ((uint64_t)(unsigned int)rand() << 15)
	     ^  (uint64_t)(unsigned int)rand();
}

/*
 * rand_delay_ns -- uniform random delay in [lo_ns, hi_ns).
 */
static int64_t
rand_delay_ns(int64_t lo_ns, int64_t hi_ns)
{
	uint64_t range = (uint64_t)(hi_ns - lo_ns);
	return lo_ns + (int64_t)(rand64() % range);
}

/* ---------------------------------------------------------------------- */
/* Fisher-Yates in-place shuffle on int array                               */
/* ---------------------------------------------------------------------- */

static void
shuffle(int *arr, long n)
{
	long i;
	for (i = n - 1; i > 0; i--) {
		long j   = (long)(rand64() % (uint64_t)(i + 1));
		int  tmp = arr[i];
		arr[i]   = arr[j];
		arr[j]   = tmp;
	}
}

/* ---------------------------------------------------------------------- */
/* Timer callback                                                           */
/* ---------------------------------------------------------------------- */

static void
timer_cb(void *u)
{
	int     idx  = (int)(intptr_t)u;
	int64_t now  = mono_ns();
	int64_t late = now - g_deadline[idx];

	/* Clamp tiny negative jitter (clock-read races) to zero. */
	if (late < 0)
		late = 0;

	hist_record(&g_fire_hist, (uint64_t)late);

	/*
	 * When the last non-cancelled timer fires, stop the loop.
	 * atomic_fetch_sub returns the value *before* decrement, so
	 * a return value of 1 means we just decremented to 0.
	 */
	if (atomic_fetch_sub_explicit(&g_remaining, 1,
	                              memory_order_acq_rel) == 1)
		xtc_loop_stop(g_loop);
}

/* ---------------------------------------------------------------------- */
/* Argument parsing                                                         */
/* ---------------------------------------------------------------------- */

static long
parse_n(int argc, char *argv[])
{
	int i;
	for (i = 1; i < argc; i++) {
		const char *a = argv[i];
		if (strncmp(a, "--N=", 4) == 0)
			return atol(a + 4);
		if (strncmp(a, "--params=N=", 11) == 0)
			return atol(a + 11);
		if (strncmp(a, "--params=", 9) == 0) {
			const char *p = strstr(a + 9, "N=");
			if (p != NULL)
				return atol(p + 2);
		}
	}
	return 100000L;
}

/* ---------------------------------------------------------------------- */
/* main                                                                     */
/* ---------------------------------------------------------------------- */

int
main(int argc, char *argv[])
{
	long          N, n_cancel, n_fire;
	long          i;
	xtc_timer_t **timers    = NULL;
	int          *order     = NULL;
	int64_t       t0, t1;
	hist_t        sched_hist, cancel_hist;
	uint64_t      elapsed_sched, elapsed_cancel, elapsed_fire;
	struct rusage ru;
	uint64_t      cpu_us, rss_kb;

	/* ---- parse args ---- */
	N = parse_n(argc, argv);
	if (N <= 0)
		N = 100000L;
	n_cancel = N / 2;
	n_fire   = N - n_cancel;

	srand((unsigned int)(uint64_t)mono_ns());

	/* ---- allocate per-timer arrays ---- */
	timers     = calloc((size_t)N, sizeof(*timers));
	g_deadline = calloc((size_t)N, sizeof(*g_deadline));
	order      = malloc((size_t)N  * sizeof(*order));
	if (timers == NULL || g_deadline == NULL || order == NULL) {
		fprintf(stderr, "w7_timer/xtc: out of memory (N=%ld)\n", N);
		return 1;
	}

	/* ---- init histograms ---- */
	if (hist_init(&sched_hist,  HIST_SUB_BITS_DEFAULT) != 0 ||
	    hist_init(&cancel_hist, HIST_SUB_BITS_DEFAULT) != 0 ||
	    hist_init(&g_fire_hist, HIST_SUB_BITS_DEFAULT) != 0) {
		fprintf(stderr, "w7_timer/xtc: hist_init failed\n");
		return 1;
	}

	/* ---- init loop ---- */
	if (xtc_loop_init(&g_loop) != XTC_OK) {
		fprintf(stderr, "w7_timer/xtc: xtc_loop_init failed\n");
		return 1;
	}

	/* ================================================================== */
	/* Phase 1 -- Schedule N timers                                        */
	/* ================================================================== */

	t0 = mono_ns();

	for (i = 0; i < N; i++) {
		int64_t delay_ns, call_t0, call_t1;

		/*
		 * Uniform delay in [1 ms, 10 s].  Upper bound is
		 * 10_001_000_000 (exclusive) so the max delay is
		 * exactly 10_000_999_999 ns ~= 10 s.
		 */
		delay_ns = rand_delay_ns(INT64_C(1000000),
		                         INT64_C(10001000000));

		call_t0       = mono_ns();
		g_deadline[i] = call_t0 + delay_ns;

		if (xtc_timer_set(g_loop, delay_ns, timer_cb,
		                  (void *)(intptr_t)i,
		                  &timers[i]) != XTC_OK) {
			fprintf(stderr,
			        "w7_timer/xtc: xtc_timer_set failed i=%ld\n",
			        i);
			(void)xtc_loop_fini(g_loop);
			return 1;
		}

		call_t1 = mono_ns();
		hist_record(&sched_hist, (uint64_t)(call_t1 - call_t0));
	}

	t1            = mono_ns();
	elapsed_sched = (uint64_t)(t1 - t0);

	/* ================================================================== */
	/* Phase 2 -- Cancel N/2 timers chosen at random                      */
	/* ================================================================== */

	for (i = 0; i < N; i++)
		order[i] = (int)i;
	shuffle(order, N);

	t0 = mono_ns();

	for (i = 0; i < n_cancel; i++) {
		int     idx    = order[i];
		int64_t call_t0, call_t1;

		call_t0 = mono_ns();
		if (xtc_timer_cancel(timers[idx]) != XTC_OK) {
			fprintf(stderr,
			        "w7_timer/xtc: xtc_timer_cancel failed\n");
			(void)xtc_loop_fini(g_loop);
			return 1;
		}
		call_t1 = mono_ns();

		hist_record(&cancel_hist, (uint64_t)(call_t1 - call_t0));
		timers[idx] = NULL;  /* mark as cancelled */
	}

	t1             = mono_ns();
	elapsed_cancel = (uint64_t)(t1 - t0);

	/* ================================================================== */
	/* Phase 3 -- Run loop until all remaining timers fire                 */
	/* ================================================================== */

	/*
	 * Set the atomic counter before calling xtc_loop_run.  The last
	 * callback to fire decrements it to 0 and calls xtc_loop_stop.
	 * The loop also exits naturally if no timers remain; both are safe.
	 *
	 * Maximum wall time: ~10 s (largest possible delay).
	 */
	atomic_store_explicit(&g_remaining, (int)n_fire, memory_order_release);

	t0 = mono_ns();

	if (xtc_loop_run(g_loop) != XTC_OK) {
		fprintf(stderr, "w7_timer/xtc: xtc_loop_run failed\n");
		return 1;
	}

	t1           = mono_ns();
	elapsed_fire = (uint64_t)(t1 - t0);

	(void)xtc_loop_fini(g_loop);

	/* ---- resource usage ---- */
	if (getrusage(RUSAGE_SELF, &ru) != 0) {
		fprintf(stderr, "w7_timer/xtc: getrusage failed\n");
		return 1;
	}

	cpu_us = (uint64_t)(ru.ru_utime.tv_sec  + ru.ru_stime.tv_sec)
	         * UINT64_C(1000000)
	       + (uint64_t)(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec);
	rss_kb = (uint64_t)ru.ru_maxrss;   /* Linux: ru_maxrss is KiB */

	/* ================================================================== */
	/* Emit three M17 result lines                                        */
	/* ================================================================== */

	printf("workload=W7 runtime=xtc_schedule params=N=%ld"
	       " elapsed_ns=%llu cpu_us=0 rss_kb=0"
	       " p50_ns=%llu p95_ns=%llu p99_ns=%llu p999_ns=%llu\n",
	       N,
	       (unsigned long long)elapsed_sched,
	       (unsigned long long)hist_percentile(&sched_hist,  50.0),
	       (unsigned long long)hist_percentile(&sched_hist,  95.0),
	       (unsigned long long)hist_percentile(&sched_hist,  99.0),
	       (unsigned long long)hist_percentile(&sched_hist,  99.9));

	printf("workload=W7 runtime=xtc_cancel params=N=%ld"
	       " elapsed_ns=%llu cpu_us=0 rss_kb=0"
	       " p50_ns=%llu p95_ns=%llu p99_ns=%llu p999_ns=%llu\n",
	       n_cancel,
	       (unsigned long long)elapsed_cancel,
	       (unsigned long long)hist_percentile(&cancel_hist,  50.0),
	       (unsigned long long)hist_percentile(&cancel_hist,  95.0),
	       (unsigned long long)hist_percentile(&cancel_hist,  99.0),
	       (unsigned long long)hist_percentile(&cancel_hist,  99.9));

	printf("workload=W7 runtime=xtc_fire params=N=%ld"
	       " elapsed_ns=%llu cpu_us=%llu rss_kb=%llu"
	       " p50_ns=%llu p95_ns=%llu p99_ns=%llu p999_ns=%llu\n",
	       n_fire,
	       (unsigned long long)elapsed_fire,
	       (unsigned long long)cpu_us,
	       (unsigned long long)rss_kb,
	       (unsigned long long)hist_percentile(&g_fire_hist,  50.0),
	       (unsigned long long)hist_percentile(&g_fire_hist,  95.0),
	       (unsigned long long)hist_percentile(&g_fire_hist,  99.0),
	       (unsigned long long)hist_percentile(&g_fire_hist,  99.9));

	/* ---- cleanup ---- */
	hist_fini(&sched_hist);
	hist_fini(&cancel_hist);
	hist_fini(&g_fire_hist);
	free(timers);
	free(g_deadline);
	free(order);

	return 0;
}
