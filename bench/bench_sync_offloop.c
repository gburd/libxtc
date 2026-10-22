/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * bench/bench_sync_offloop.c
 *	OFF-LOOP contention on one libxtc lock: N plain OS threads (no
 *	loop, no fiber, so every wait takes the lock's thread path, as the
 *	in-process sqlxtc clients do) hammer ONE xtc_amutex or xtc_arwlock
 *	with a short critical section and a short think time between
 *	acquires.  The same loop runs over a plain pthread_mutex_t /
 *	pthread_rwlock_t as the reference for what the OS gives.
 *
 *	Reports, per run, one CSV row:
 *	  lock,pattern,threads,secs,ops,ops_per_s,csw,csw_per_kop,
 *	  cpu_s,cpu_us_per_op,p50_ns,p99_ns,max_ns,viol
 *	ops        acquire+release pairs completed in the window
 *	csw        voluntary + involuntary context switches (getrusage)
 *	cpu_s      user + system CPU of the whole process in the window
 *	pNN/max    time to ACQUIRE (lock call entry to return), merged over
 *	           all threads (log-linear histogram, ~6% bucket width)
 *	viol       exclusion violations seen (must be 0)
 *
 *	Patterns: w (every op exclusive), r (5% exclusive, 95% shared),
 *	mix (50/50).  A mutex runs every op exclusive whatever the pattern.
 *
 *	Usage: bench_sync_offloop LOCK PATTERN THREADS [SECS] [CS] [THINK]
 *	  LOCK     amutex | arwlock | pmutex | prwlock
 *	  PATTERN  w | r | mix
 *	  SECS     measured window (default 3)
 *	  CS       critical-section work units (default 20)
 *	  THINK    work units between acquires (default 200)
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>

#include "xtc.h"
#include "xtc_sync.h"

#define MAXT     1024
#define NBUCKET  1024

enum { L_AMUTEX, L_ARWLOCK, L_PMUTEX, L_PRWLOCK };

struct tstate {
	_Alignas(64) uint64_t ops;
	uint64_t max;
	uint64_t rng;
	pthread_t th;
	uint64_t hist[NBUCKET];
};

static int g_lock, g_wpct, g_cs, g_think;
static xtc_amutex_t *g_am;
static xtc_arwlock_t *g_arw;
static pthread_mutex_t g_pm = PTHREAD_MUTEX_INITIALIZER;
static pthread_rwlock_t g_prw = PTHREAD_RWLOCK_INITIALIZER;
static _Atomic int g_go, g_stop;
static _Atomic int g_inw, g_inr, g_viol;
static volatile uint64_t g_data[8];       /* the protected state */
static struct tstate *g_ts;

/* Log-linear bucket: exact below 64, then 16 sub-buckets per power of 2. */
static int
bucket_of(uint64_t v)
{
	int b;
	if (v < 64) return (int)v;
	b = 63 - __builtin_clzll(v);
	return 64 + (b - 6) * 16 + (int)((v >> (b - 4)) & 15);
}

static uint64_t
bucket_lo(int i)
{
	int b;
	if (i < 64) return (uint64_t)i;
	b = (i - 64) / 16 + 6;
	return ((uint64_t)1 << b) | ((uint64_t)((i - 64) % 16) << (b - 4));
}

static void
spin_work(int n)
{
	volatile int k = 0;
	int i;
	for (i = 0; i < n; i++) k += i;
}

static void
lock_acquire(int excl)
{
	switch (g_lock) {
	case L_AMUTEX:  (void)xtc_amutex_lock(g_am, -1); break;
	case L_ARWLOCK:
		if (excl) (void)xtc_arwlock_wrlock(g_arw, -1);
		else (void)xtc_arwlock_rdlock(g_arw, -1);
		break;
	case L_PMUTEX:  (void)pthread_mutex_lock(&g_pm); break;
	default:
		if (excl) (void)pthread_rwlock_wrlock(&g_prw);
		else (void)pthread_rwlock_rdlock(&g_prw);
		break;
	}
}

static void
lock_release(void)
{
	switch (g_lock) {
	case L_AMUTEX:  (void)xtc_amutex_unlock(g_am); break;
	case L_ARWLOCK: (void)xtc_arwlock_unlock(g_arw); break;
	case L_PMUTEX:  (void)pthread_mutex_unlock(&g_pm); break;
	default:        (void)pthread_rwlock_unlock(&g_prw); break;
	}
}

static void *
worker(void *arg)
{
	struct tstate *t = arg;
	int excl, i;
	uint64_t r, lat;
	int64_t t0;

	while (!atomic_load_explicit(&g_go, memory_order_acquire))
		;
	while (!atomic_load_explicit(&g_stop, memory_order_relaxed)) {
		t->rng ^= t->rng << 13; t->rng ^= t->rng >> 7; t->rng ^= t->rng << 17;
		r = t->rng;
		excl = g_lock == L_AMUTEX || g_lock == L_PMUTEX ||
		    (int)(r % 100) < g_wpct;
		t0 = xtc_clock_mono();
		lock_acquire(excl);
		lat = (uint64_t)(xtc_clock_mono() - t0);
		if (excl) {
			if (atomic_fetch_add_explicit(&g_inw, 1, memory_order_relaxed) != 0 ||
			    atomic_load_explicit(&g_inr, memory_order_relaxed) != 0)
				atomic_store(&g_viol, 1);
			for (i = 0; i < 8; i++) g_data[i]++;
			spin_work(g_cs);
			atomic_fetch_sub_explicit(&g_inw, 1, memory_order_relaxed);
		} else {
			atomic_fetch_add_explicit(&g_inr, 1, memory_order_relaxed);
			if (atomic_load_explicit(&g_inw, memory_order_relaxed) != 0)
				atomic_store(&g_viol, 1);
			for (i = 1; i < 8; i++)
				if (g_data[i] != g_data[0]) atomic_store(&g_viol, 1);
			spin_work(g_cs);
			atomic_fetch_sub_explicit(&g_inr, 1, memory_order_relaxed);
		}
		lock_release();
		t->ops++;
		t->hist[bucket_of(lat)]++;
		if (lat > t->max) t->max = lat;
		spin_work(g_think);
	}
	return NULL;
}

static double
cpu_s(const struct rusage *u)
{
	return (double)u->ru_utime.tv_sec + u->ru_utime.tv_usec / 1e6 +
	    (double)u->ru_stime.tv_sec + u->ru_stime.tv_usec / 1e6;
}

int
main(int argc, char **argv)
{
	const char *lk, *pat;
	int n, secs, i, j;
	struct rusage u0, u1;
	int64_t t0, t1;
	uint64_t ops = 0, max = 0, acc, p50 = 0, p99 = 0, *hist;
	long csw;
	double el, cpu;

	if (argc < 4) {
		fprintf(stderr, "usage: %s amutex|arwlock|pmutex|prwlock w|r|mix "
		    "THREADS [SECS] [CS] [THINK]\n", argv[0]);
		return 2;
	}
	lk = argv[1]; pat = argv[2]; n = atoi(argv[3]);
	secs = argc > 4 ? atoi(argv[4]) : 3;
	g_cs = argc > 5 ? atoi(argv[5]) : 20;
	g_think = argc > 6 ? atoi(argv[6]) : 200;
	if (n < 1 || n > MAXT || secs < 1) return 2;
	if (!strcmp(lk, "amutex")) g_lock = L_AMUTEX;
	else if (!strcmp(lk, "arwlock")) g_lock = L_ARWLOCK;
	else if (!strcmp(lk, "pmutex")) g_lock = L_PMUTEX;
	else if (!strcmp(lk, "prwlock")) g_lock = L_PRWLOCK;
	else return 2;
	if (!strcmp(pat, "w")) g_wpct = 100;
	else if (!strcmp(pat, "r")) g_wpct = 5;
	else if (!strcmp(pat, "mix")) g_wpct = 50;
	else return 2;

	if (xtc_amutex_create(&g_am) != XTC_OK ||
	    xtc_arwlock_create(&g_arw) != XTC_OK)
		return 1;
	g_ts = xtc_aligned_alloc(64, sizeof *g_ts * (size_t)n);
	hist = xtc_calloc(NBUCKET, sizeof *hist);
	if (g_ts == NULL || hist == NULL) return 1;
	memset(g_ts, 0, sizeof *g_ts * (size_t)n);
	for (i = 0; i < n; i++) {
		g_ts[i].rng = 0x9E3779B97F4A7C15ull * (uint64_t)(i + 1);
		if (pthread_create(&g_ts[i].th, NULL, worker, &g_ts[i]) != 0)
			return 1;
	}
	(void)getrusage(RUSAGE_SELF, &u0);
	t0 = xtc_clock_mono();
	atomic_store_explicit(&g_go, 1, memory_order_release);
	(void)xtc_sleep_ns((int64_t)secs * 1000000000LL);
	atomic_store(&g_stop, 1);
	t1 = xtc_clock_mono();
	(void)getrusage(RUSAGE_SELF, &u1);
	for (i = 0; i < n; i++) {
		(void)pthread_join(g_ts[i].th, NULL);
		ops += g_ts[i].ops;
		if (g_ts[i].max > max) max = g_ts[i].max;
		for (j = 0; j < NBUCKET; j++) hist[j] += g_ts[i].hist[j];
	}
	for (acc = 0, j = 0; j < NBUCKET; j++) {
		acc += hist[j];
		if (p50 == 0 && acc * 2 >= ops) p50 = bucket_lo(j);
		if (acc * 100 >= ops * 99) { p99 = bucket_lo(j); break; }
	}
	el = (double)(t1 - t0) / 1e9;
	csw = (u1.ru_nvcsw - u0.ru_nvcsw) + (u1.ru_nivcsw - u0.ru_nivcsw);
	cpu = cpu_s(&u1) - cpu_s(&u0);
	printf("%s,%s,%d,%.2f,%llu,%.0f,%ld,%.1f,%.2f,%.3f,%llu,%llu,%llu,%d\n",
	    lk, pat, n, el, (unsigned long long)ops, (double)ops / el, csw,
	    ops ? (double)csw * 1000.0 / (double)ops : 0.0, cpu,
	    ops ? cpu * 1e6 / (double)ops : 0.0, (unsigned long long)p50,
	    (unsigned long long)p99, (unsigned long long)max,
	    atomic_load(&g_viol));
	xtc_amutex_destroy(g_am);
	xtc_arwlock_destroy(g_arw);
	xtc_free(hist);
	xtc_aligned_free(g_ts);
	return atomic_load(&g_viol) ? 1 : 0;
}
