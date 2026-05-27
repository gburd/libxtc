/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * bench/conformance/w4_mutex/xtc/main.c
 *   W4: mutex contention benchmark -- xtc runtime.
 *
 *   N pthreads contend for a shared counter protected by one of three
 *   primitives: xtc_amutex (async-parking mutex from xtc_sync),
 *   xtc_lwlock (the PG-style lightweight lock from M13b), or
 *   __os_mutex (raw pthread_mutex via xtc's OS layer).
 *
 *   Each thread executes a tight loop: acquire the lock, increment
 *   the shared counter, release the lock; one in every 1000 iterations
 *   is timed with CLOCK_MONOTONIC to build a latency histogram.
 *   After all threads complete, the counter is verified to equal the
 *   total number of operations (mutual exclusion check).
 *
 *   Default behaviour (no arguments): run all three primitives with
 *   threads=8, ops=100000; emit three M17 lines on stdout.
 *
 * Usage:
 *   ./bench                              # all three prims, threads=8, ops=100000
 *   ./bench --prim=lwlock                # single primitive, one output line
 *   ./bench --threads=4 --ops=10000 --prim=lwlock
 *   ./bench --sweep=1                    # threads=1,2,4,8,16,32 x all prims
 *   ./bench --sweep=1 --prim=osmutex     # sweep for one primitive only
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>
#include <sys/resource.h>

#include "hist.h"

#include "xtc.h"
#include "xtc_sync.h"
#include "xtc_lwlock.h"
#include "os_thread.h"

/* -------------------------------------------------------------------------
 * Defaults and primitive IDs
 * ------------------------------------------------------------------------- */

#define DEFAULT_THREADS  8
#define DEFAULT_OPS      100000L

#define PRIM_AMUTEX   0
#define PRIM_LWLOCK   1
#define PRIM_OSMUTEX  2

static const char * const prim_names[] = {
	"xtc_amutex", "xtc_lwlock", "xtc_osmutex"
};

/* -------------------------------------------------------------------------
 * Timing and resource helpers
 * ------------------------------------------------------------------------- */

static uint64_t
now_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * UINT64_C(1000000000)
	     + (uint64_t)ts.tv_nsec;
}

static uint64_t
ru_cpu_us(const struct rusage *ru)
{
	return (uint64_t)(ru->ru_utime.tv_sec  + ru->ru_stime.tv_sec)
	     * UINT64_C(1000000)
	     + (uint64_t)(ru->ru_utime.tv_usec + ru->ru_stime.tv_usec);
}

/* -------------------------------------------------------------------------
 * In-place histogram merge (bucket-level add).
 * Requires both histograms to have been initialised with the same sub_bits.
 * ------------------------------------------------------------------------- */

static void
hist_merge(hist_t *dst, const hist_t *src)
{
	uint32_t i, n;

	n = dst->n_buckets < src->n_buckets ? dst->n_buckets : src->n_buckets;
	for (i = 0; i < n; i++)
		dst->buckets[i] += src->buckets[i];

	dst->total += src->total;

	if (src->total > 0) {
		if (src->min_ns < dst->min_ns)
			dst->min_ns = src->min_ns;
		if (src->max_ns > dst->max_ns)
			dst->max_ns = src->max_ns;
	}
}

/* -------------------------------------------------------------------------
 * Lock abstraction -- wraps the three xtc primitives behind a uniform API
 * ------------------------------------------------------------------------- */

typedef struct {
	int            prim;
	xtc_amutex_t  *amutex;   /* PRIM_AMUTEX: heap-allocated via create() */
	xtc_lwlock_t   lwlock;   /* PRIM_LWLOCK: stack / embed */
	__os_mutex_t   osmutex;  /* PRIM_OSMUTEX: stack / embed */
} lock_t;

static int
lock_init(lock_t *l, int prim)
{
	l->prim   = prim;
	l->amutex = NULL;

	switch (prim) {
	case PRIM_AMUTEX:
		if (xtc_amutex_create(&l->amutex) != XTC_OK) {
			fprintf(stderr, "w4/xtc: xtc_amutex_create failed\n");
			return -1;
		}
		return 0;

	case PRIM_LWLOCK:
		if (xtc_lwlock_init(&l->lwlock, 0) != XTC_OK) {
			fprintf(stderr, "w4/xtc: xtc_lwlock_init failed\n");
			return -1;
		}
		return 0;

	case PRIM_OSMUTEX:
		if (__os_mutex_init(&l->osmutex) != XTC_OK) {
			fprintf(stderr, "w4/xtc: __os_mutex_init failed\n");
			return -1;
		}
		return 0;

	default:
		return -1;
	}
}

static void
lock_fini(lock_t *l)
{
	switch (l->prim) {
	case PRIM_AMUTEX:
		xtc_amutex_destroy(l->amutex);
		l->amutex = NULL;
		break;
	case PRIM_LWLOCK:
		xtc_lwlock_destroy(&l->lwlock);
		break;
	case PRIM_OSMUTEX:
		(void)__os_mutex_destroy(&l->osmutex);
		break;
	default:
		break;
	}
}

static void
lock_acquire(lock_t *l)
{
	switch (l->prim) {
	case PRIM_AMUTEX:
		(void)xtc_amutex_lock(l->amutex, (int64_t)-1);
		break;
	case PRIM_LWLOCK:
		(void)xtc_lwlock_acquire(&l->lwlock, XTC_LW_EXCLUSIVE);
		break;
	case PRIM_OSMUTEX:
		(void)__os_mutex_lock(&l->osmutex);
		break;
	default:
		break;
	}
}

static void
lock_release(lock_t *l)
{
	switch (l->prim) {
	case PRIM_AMUTEX:
		(void)xtc_amutex_unlock(l->amutex);
		break;
	case PRIM_LWLOCK:
		xtc_lwlock_release(&l->lwlock);
		break;
	case PRIM_OSMUTEX:
		(void)__os_mutex_unlock(&l->osmutex);
		break;
	default:
		break;
	}
}

/* -------------------------------------------------------------------------
 * Per-thread worker
 *
 * Runs `ops` acquire-increment-release iterations.  Every 1000th
 * iteration is bracketed with CLOCK_MONOTONIC calls and the resulting
 * latency is recorded in the per-thread histogram.
 * ------------------------------------------------------------------------- */

struct worker_arg {
	lock_t   *lock;
	long     *counter;      /* shared; access only while holding lock */
	long      ops;
	int       thread_idx;   /* used to stagger per-thread sample windows */
	hist_t   *hist;         /* per-thread histogram; merged by main after join */
};

static void *
worker_fn(void *arg_)
{
	struct worker_arg *a = arg_;
	long     i;
	uint64_t sample_n;

	sample_n = (uint64_t)(unsigned)a->thread_idx * 97u + 1u;

	for (i = 0; i < a->ops; i++) {
		int      do_sample;
		uint64_t t0, t1;

		sample_n++;
		do_sample = ((sample_n % 1000u) == 0u);

		t0 = do_sample ? now_ns() : 0;

		lock_acquire(a->lock);
		(*a->counter)++;
		lock_release(a->lock);

		if (do_sample) {
			t1 = now_ns();
			hist_record(a->hist, t1 - t0);
		}
	}

	return NULL;
}

/* -------------------------------------------------------------------------
 * run_bench -- run one (prim, threads, ops) tuple and emit one M17 line
 * ------------------------------------------------------------------------- */

static void
run_bench(int prim, int n_threads, long total_ops)
{
	lock_t             lock;
	long               counter;
	long               per_thread;
	long               actual_ops;
	pthread_t         *tids     = NULL;
	struct worker_arg *args     = NULL;
	hist_t            *hists    = NULL;
	hist_t             merged;
	struct rusage      ru0, ru1;
	uint64_t           t_start, t_end;
	uint64_t           elapsed_ns, cpu_us, rss_kb;
	int                j;

	if (lock_init(&lock, prim) != 0)
		return;

	per_thread = total_ops / n_threads;
	actual_ops = per_thread * n_threads;
	counter    = 0;

	tids  = calloc((size_t)n_threads, sizeof(pthread_t));
	args  = calloc((size_t)n_threads, sizeof(struct worker_arg));
	hists = calloc((size_t)n_threads, sizeof(hist_t));
	if (tids == NULL || args == NULL || hists == NULL) {
		fprintf(stderr, "w4/xtc: calloc failed\n");
		goto cleanup_lock;
	}

	for (j = 0; j < n_threads; j++) {
		if (hist_init(&hists[j], HIST_SUB_BITS_DEFAULT) != 0) {
			fprintf(stderr, "w4/xtc: hist_init failed (thread %d)\n", j);
			while (j > 0)
				hist_fini(&hists[--j]);
			goto cleanup_lock;
		}
		args[j].lock       = &lock;
		args[j].counter    = &counter;
		args[j].ops        = per_thread;
		args[j].thread_idx = j;
		args[j].hist       = &hists[j];
	}

	getrusage(RUSAGE_SELF, &ru0);
	t_start = now_ns();

	for (j = 0; j < n_threads; j++)
		(void)pthread_create(&tids[j], NULL, worker_fn, &args[j]);

	for (j = 0; j < n_threads; j++)
		(void)pthread_join(tids[j], NULL);

	t_end = now_ns();
	getrusage(RUSAGE_SELF, &ru1);

	/* Mutual exclusion correctness check */
	if (counter != actual_ops) {
		fprintf(stderr,
		    "w4/xtc: FAILED mutual exclusion check: "
		    "counter=%ld expected=%ld prim=%s threads=%d\n",
		    counter, actual_ops, prim_names[prim], n_threads);
	}

	/* Merge per-thread histograms into one */
	(void)hist_init(&merged, HIST_SUB_BITS_DEFAULT);
	for (j = 0; j < n_threads; j++) {
		hist_merge(&merged, &hists[j]);
		hist_fini(&hists[j]);
	}

	elapsed_ns = t_end - t_start;
	cpu_us     = ru_cpu_us(&ru1) - ru_cpu_us(&ru0);
	rss_kb     = (uint64_t)ru1.ru_maxrss; /* Linux: already KiB */

	printf("workload=W4 runtime=%s params=threads=%d:ops=%ld"
	       " elapsed_ns=%llu"
	       " cpu_us=%llu"
	       " rss_kb=%llu"
	       " p50_ns=%llu"
	       " p95_ns=%llu"
	       " p99_ns=%llu"
	       " p999_ns=%llu\n",
	    prim_names[prim],
	    n_threads,
	    actual_ops,
	    (unsigned long long)elapsed_ns,
	    (unsigned long long)cpu_us,
	    (unsigned long long)rss_kb,
	    (unsigned long long)hist_percentile(&merged,  50.0),
	    (unsigned long long)hist_percentile(&merged,  95.0),
	    (unsigned long long)hist_percentile(&merged,  99.0),
	    (unsigned long long)hist_percentile(&merged,  99.9));

	hist_fini(&merged);
	free(tids);
	free(args);
	free(hists);
	lock_fini(&lock);
	return;

cleanup_lock:
	free(tids);
	free(args);
	free(hists);
	lock_fini(&lock);
}

/* -------------------------------------------------------------------------
 * Argument parsing helpers
 * ------------------------------------------------------------------------- */

static int
parse_prim(const char *s)
{
	if (strcmp(s, "amutex")  == 0) return PRIM_AMUTEX;
	if (strcmp(s, "lwlock")  == 0) return PRIM_LWLOCK;
	if (strcmp(s, "osmutex") == 0) return PRIM_OSMUTEX;
	fprintf(stderr,
	    "w4/xtc: unknown --prim='%s'; use amutex|lwlock|osmutex\n", s);
	return -2;   /* distinct from -1 (all) */
}

/* -------------------------------------------------------------------------
 * main
 * ------------------------------------------------------------------------- */

static const int sweep_threads[] = { 1, 2, 4, 8, 16, 32 };
#define N_SWEEP ((int)(sizeof(sweep_threads) / sizeof(sweep_threads[0])))

int
main(int argc, char **argv)
{
	int    prim_arg  = -1;            /* -1 = all three */
	int    n_threads = DEFAULT_THREADS;
	long   ops       = DEFAULT_OPS;
	int    sweep     = 0;
	int    i;

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];

		if (strncmp(a, "--threads=", 10) == 0) {
			n_threads = (int)strtol(a + 10, NULL, 10);
		} else if (strncmp(a, "--ops=", 6) == 0) {
			ops = strtol(a + 6, NULL, 10);
		} else if (strncmp(a, "--prim=", 7) == 0) {
			prim_arg = parse_prim(a + 7);
			if (prim_arg == -2)
				return 1;
		} else if (strncmp(a, "--sweep=", 8) == 0) {
			sweep = (int)strtol(a + 8, NULL, 10);
		} else if (strncmp(a, "--params=", 9) == 0) {
			/* accept --params=threads=N:ops=M for run.sh compat */
			const char *p = a + 9;
			do {
				if (strncmp(p, "threads=", 8) == 0)
					n_threads = (int)strtol(p + 8, NULL, 10);
				else if (strncmp(p, "ops=", 4) == 0)
					ops = strtol(p + 4, NULL, 10);
				p = strchr(p, ':');
				if (p != NULL) p++;
			} while (p != NULL);
		}
	}

	if (n_threads < 1) n_threads = 1;
	if (ops < 1)       ops = 1;

	if (sweep) {
		int ti, p;

		for (ti = 0; ti < N_SWEEP; ti++) {
			int t = sweep_threads[ti];

			if (prim_arg < 0) {
				for (p = PRIM_AMUTEX; p <= PRIM_OSMUTEX; p++)
					run_bench(p, t, ops);
			} else {
				run_bench(prim_arg, t, ops);
			}
		}
	} else {
		if (prim_arg < 0) {
			int p;

			for (p = PRIM_AMUTEX; p <= PRIM_OSMUTEX; p++)
				run_bench(p, n_threads, ops);
		} else {
			run_bench(prim_arg, n_threads, ops);
		}
	}

	return 0;
}
