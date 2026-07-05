/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * bench/conformance/w5_rwratio/xtc/main.c
 *   W5: reader/writer ratio sweep benchmark -- xtc runtime.
 *
 *   N pthreads share one guarded counter protected by one of two
 *   reader/writer primitives:
 *
 *     xtc_arwlock  the fiber-yielding shared/exclusive latch from
 *                  xtc_sync.h (readers share, one writer excludes; off
 *                  a loop contended waiters block on a condvar).
 *     xtc_lrlock   the Left-Right lock from xtc_lrlock.h (wait-free
 *                  reads against two side-by-side copies, single writer
 *                  with a pointer-swap publish).
 *
 *   Each thread runs a tight loop of `ops` operations.  The reader:writer
 *   ratio decides how many of every (ratio + 1) operations are reads: a
 *   ratio of 10 means 10 reads then 1 write, repeating.  Readers take the
 *   shared/read side and observe the counter; writers take the exclusive/
 *   write side and increment it.  One in every 1000 operations is timed
 *   with CLOCK_MONOTONIC to build a latency histogram (reads and writes
 *   share the histogram, since the sweep measures aggregate throughput).
 *
 *   After all threads complete, the writer count is verified against the
 *   final counter value (mutual-exclusion correctness check).
 *
 *   Default behaviour (no arguments): sweep ratios 1, 10, 100 for both
 *   primitives with threads=8, ops=100000; emit six M17 lines on stdout.
 *
 * Usage:
 *   ./bench                                  # both prims, ratios 1:10:100
 *   ./bench --prim=arwlock                   # single primitive
 *   ./bench --threads=4 --ops=10000 --prim=lrlock --ratio=10
 *   ./bench --prim=arwlock --ratio=100       # one ratio point
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
#include "xtc_lrlock.h"

/* -------------------------------------------------------------------------
 * Defaults and primitive IDs
 * ------------------------------------------------------------------------- */

#define DEFAULT_THREADS  8
#define DEFAULT_OPS      100000L

#define PRIM_ARWLOCK  0
#define PRIM_LRLOCK   1

static const char * const prim_names[] = {
	"xtc_arwlock", "xtc_lrlock"
};

/* Default ratio sweep points (readers : 1 writer). */
static const int sweep_ratios[] = { 1, 10, 100 };
#define N_RATIOS ((int)(sizeof(sweep_ratios) / sizeof(sweep_ratios[0])))

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
 * Left-Right lock callbacks.  The protected data is a single long; a
 * write applies "add 1" and full-sync keeps both copies identical.
 * ------------------------------------------------------------------------- */

static void
lr_sync(void *dst, const void *src, size_t data_size)
{
	memcpy(dst, src, data_size);
}

/* -------------------------------------------------------------------------
 * Lock abstraction -- wraps both xtc reader/writer primitives behind a
 * uniform read/write API.  The counter lives inside the primitive for
 * lrlock (its two copies) and beside it for arwlock.
 * ------------------------------------------------------------------------- */

typedef struct {
	int            prim;
	xtc_arwlock_t *arw;      /* PRIM_ARWLOCK: heap-allocated via create() */
	long           counter;  /* PRIM_ARWLOCK: guarded by arw */
	xtc_lrlock_t  *lr;       /* PRIM_LRLOCK:  the Left-Right lock */
} lock_t;

static int
lock_init(lock_t *l, int prim)
{
	l->prim    = prim;
	l->arw     = NULL;
	l->lr      = NULL;
	l->counter = 0;

	switch (prim) {
	case PRIM_ARWLOCK:
		if (xtc_arwlock_create(&l->arw) != XTC_OK) {
			fprintf(stderr, "w5/xtc: xtc_arwlock_create failed\n");
			return -1;
		}
		return 0;

	case PRIM_LRLOCK:
		if (xtc_lrlock_create(sizeof(long), NULL, lr_sync,
		    "w5", &l->lr) != XTC_OK) {
			fprintf(stderr, "w5/xtc: xtc_lrlock_create failed\n");
			return -1;
		}
		/* Zero both copies so read_begin sees a defined value from
		 * the start, then mark ready to skip the first full-sync. */
		*(long *)xtc_lrlock_write_data(l->lr) = 0;
		xtc_lrlock_mark_ready(l->lr);
		return 0;

	default:
		return -1;
	}
}

static void
lock_fini(lock_t *l)
{
	switch (l->prim) {
	case PRIM_ARWLOCK:
		if (l->arw != NULL)
			xtc_arwlock_destroy(l->arw);
		l->arw = NULL;
		break;
	case PRIM_LRLOCK:
		if (l->lr != NULL)
			xtc_lrlock_destroy(l->lr);
		l->lr = NULL;
		break;
	default:
		break;
	}
}

/* One read op: acquire shared, observe the counter, release.
 * Returns the observed value (also serves as the correctness sink). */
static long
lock_read(lock_t *l)
{
	long v;

	switch (l->prim) {
	case PRIM_ARWLOCK:
		(void)xtc_arwlock_rdlock(l->arw, (int64_t)-1);
		v = l->counter;
		(void)xtc_arwlock_unlock(l->arw);
		return v;

	case PRIM_LRLOCK: {
		const long *p = xtc_lrlock_read_begin(l->lr);
		/* read_begin can return NULL if reader slots are exhausted;
		 * treat as "no observation" (should not happen with the
		 * default 64-slot pool for our thread counts). */
		v = (p != NULL) ? *p : 0;
		if (p != NULL)
			xtc_lrlock_read_end(l->lr);
		return v;
	}

	default:
		return 0;
	}
}

/* One write op: acquire exclusive/write side, increment, publish/release. */
static void
lock_write(lock_t *l)
{
	switch (l->prim) {
	case PRIM_ARWLOCK:
		(void)xtc_arwlock_wrlock(l->arw, (int64_t)-1);
		l->counter++;
		(void)xtc_arwlock_unlock(l->arw);
		break;

	case PRIM_LRLOCK: {
		long *p = xtc_lrlock_write_begin(l->lr);
		(*p)++;
		/* We mutated the copy directly, so full-sync keeps both
		 * copies identical on publish. */
		xtc_lrlock_publish_full_sync(l->lr);
		xtc_lrlock_write_end(l->lr);
		break;
	}

	default:
		break;
	}
}

/* Read the final counter value (called after all threads join). */
static long
lock_final(lock_t *l)
{
	switch (l->prim) {
	case PRIM_ARWLOCK:
		return l->counter;
	case PRIM_LRLOCK: {
		const long *p = xtc_lrlock_read_begin(l->lr);
		long v = (p != NULL) ? *p : 0;
		if (p != NULL)
			xtc_lrlock_read_end(l->lr);
		return v;
	}
	default:
		return 0;
	}
}

/* -------------------------------------------------------------------------
 * Per-thread worker
 *
 * Runs `ops` operations.  For every (ratio + 1) operations, `ratio` are
 * reads and 1 is a write.  Every 1000th operation is timed.
 * ------------------------------------------------------------------------- */

struct worker_arg {
	lock_t   *lock;
	long      ops;
	int       ratio;        /* readers per writer */
	int       thread_idx;   /* staggers per-thread sample windows */
	hist_t   *hist;
	long      read_sink;    /* accumulates observed values (out) */
	long      writes;       /* number of writes this thread issued (out) */
};

static void *
worker_fn(void *arg_)
{
	struct worker_arg *a = arg_;
	long     i;
	long     phase;         /* 0..ratio; == ratio -> this op is a write */
	long     writes = 0;
	long     sink = 0;
	uint64_t sample_n;

	sample_n = (uint64_t)(unsigned)a->thread_idx * 97u + 1u;
	phase    = 0;

	for (i = 0; i < a->ops; i++) {
		int      do_sample;
		int      is_write;
		uint64_t t0, t1;

		sample_n++;
		do_sample = ((sample_n % 1000u) == 0u);
		is_write  = (phase >= a->ratio);

		t0 = do_sample ? now_ns() : 0;

		if (is_write) {
			lock_write(a->lock);
			writes++;
			phase = 0;
		} else {
			sink += lock_read(a->lock);
			phase++;
		}

		if (do_sample) {
			t1 = now_ns();
			hist_record(a->hist, t1 - t0);
		}
	}

	a->read_sink = sink;
	a->writes    = writes;
	return NULL;
}

/* -------------------------------------------------------------------------
 * run_bench -- run one (prim, threads, ops, ratio) tuple; emit one M17 line
 * ------------------------------------------------------------------------- */

static void
run_bench(int prim, int n_threads, long total_ops, int ratio)
{
	lock_t             lock;
	long               per_thread;
	long               actual_ops;
	long               total_writes;
	long               final_count;
	pthread_t         *tids  = NULL;
	struct worker_arg *args  = NULL;
	hist_t            *hists = NULL;
	hist_t             merged;
	struct rusage      ru0, ru1;
	uint64_t           t_start, t_end;
	uint64_t           elapsed_ns, cpu_us, rss_kb;
	int                j;

	if (lock_init(&lock, prim) != 0)
		return;

	per_thread = total_ops / n_threads;
	actual_ops = per_thread * n_threads;

	tids  = calloc((size_t)n_threads, sizeof(pthread_t));
	args  = calloc((size_t)n_threads, sizeof(struct worker_arg));
	hists = calloc((size_t)n_threads, sizeof(hist_t));
	if (tids == NULL || args == NULL || hists == NULL) {
		fprintf(stderr, "w5/xtc: calloc failed\n");
		goto cleanup_lock;
	}

	for (j = 0; j < n_threads; j++) {
		if (hist_init(&hists[j], HIST_SUB_BITS_DEFAULT) != 0) {
			fprintf(stderr, "w5/xtc: hist_init failed (thread %d)\n", j);
			while (j > 0)
				hist_fini(&hists[--j]);
			goto cleanup_lock;
		}
		args[j].lock       = &lock;
		args[j].ops        = per_thread;
		args[j].ratio      = ratio;
		args[j].thread_idx = j;
		args[j].hist       = &hists[j];
		args[j].read_sink  = 0;
		args[j].writes     = 0;
	}

	getrusage(RUSAGE_SELF, &ru0);
	t_start = now_ns();

	for (j = 0; j < n_threads; j++)
		(void)pthread_create(&tids[j], NULL, worker_fn, &args[j]);

	for (j = 0; j < n_threads; j++)
		(void)pthread_join(tids[j], NULL);

	t_end = now_ns();
	getrusage(RUSAGE_SELF, &ru1);

	/* Sum writes across threads; the final counter must match. */
	total_writes = 0;
	for (j = 0; j < n_threads; j++)
		total_writes += args[j].writes;

	final_count = lock_final(&lock);
	if (final_count != total_writes) {
		fprintf(stderr,
		    "w5/xtc: FAILED mutual exclusion check: "
		    "counter=%ld expected=%ld prim=%s threads=%d ratio=%d\n",
		    final_count, total_writes, prim_names[prim],
		    n_threads, ratio);
	}

	/* Merge per-thread histograms into one. */
	(void)hist_init(&merged, HIST_SUB_BITS_DEFAULT);
	for (j = 0; j < n_threads; j++) {
		hist_merge(&merged, &hists[j]);
		hist_fini(&hists[j]);
	}

	elapsed_ns = t_end - t_start;
	cpu_us     = ru_cpu_us(&ru1) - ru_cpu_us(&ru0);
	rss_kb     = (uint64_t)ru1.ru_maxrss; /* Linux: already KiB */

	printf("workload=W5 runtime=%s params=threads=%d:ops=%ld:ratio=%d"
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
	    ratio,
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
 * Argument parsing
 * ------------------------------------------------------------------------- */

static int
parse_prim(const char *s)
{
	if (strcmp(s, "arwlock") == 0) return PRIM_ARWLOCK;
	if (strcmp(s, "lrlock")  == 0) return PRIM_LRLOCK;
	fprintf(stderr,
	    "w5/xtc: unknown --prim='%s'; use arwlock|lrlock\n", s);
	return -2;   /* distinct from -1 (all) */
}

int
main(int argc, char **argv)
{
	int    prim_arg  = -1;            /* -1 = both */
	int    n_threads = DEFAULT_THREADS;
	long   ops       = DEFAULT_OPS;
	int    ratio_arg = -1;            /* -1 = default sweep */
	int    i;

	for (i = 1; i < argc; i++) {
		const char *a = argv[i];

		if (strncmp(a, "--threads=", 10) == 0) {
			n_threads = (int)strtol(a + 10, NULL, 10);
		} else if (strncmp(a, "--ops=", 6) == 0) {
			ops = strtol(a + 6, NULL, 10);
		} else if (strncmp(a, "--ratio=", 8) == 0) {
			ratio_arg = (int)strtol(a + 8, NULL, 10);
		} else if (strncmp(a, "--prim=", 7) == 0) {
			prim_arg = parse_prim(a + 7);
			if (prim_arg == -2)
				return 1;
		} else if (strncmp(a, "--params=", 9) == 0) {
			/* accept --params=threads=N:ops=M:ratio=R for run.sh */
			const char *p = a + 9;
			do {
				if (strncmp(p, "threads=", 8) == 0)
					n_threads = (int)strtol(p + 8, NULL, 10);
				else if (strncmp(p, "ops=", 4) == 0)
					ops = strtol(p + 4, NULL, 10);
				else if (strncmp(p, "ratio=", 6) == 0)
					ratio_arg = (int)strtol(p + 6, NULL, 10);
				p = strchr(p, ':');
				if (p != NULL) p++;
			} while (p != NULL);
		}
	}

	if (n_threads < 1) n_threads = 1;
	if (ops < 1)       ops = 1;
	if (ratio_arg == 0) ratio_arg = 1;    /* 0:1 is meaningless; clamp */

	{
		int p_lo = (prim_arg < 0) ? PRIM_ARWLOCK : prim_arg;
		int p_hi = (prim_arg < 0) ? PRIM_LRLOCK  : prim_arg;
		int p;

		for (p = p_lo; p <= p_hi; p++) {
			if (ratio_arg > 0) {
				run_bench(p, n_threads, ops, ratio_arg);
			} else {
				int ri;

				for (ri = 0; ri < N_RATIOS; ri++)
					run_bench(p, n_threads, ops,
					    sweep_ratios[ri]);
			}
		}
	}

	return 0;
}
