/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * SPDX-License-Identifier: ISC
 *
 * A/B eviction-sweep benchmark. theta=0 selects the legacy sweep.
 * Dataset must exceed the pool. Zipf ranks are shuffled across page ids.
 * No provider/trickler is spawned. Use a disposable path on local NVMe.
 * CSV quantiles are histogram bucket lower bounds, sampled in us and
 * reported in ns; max_ns is exact. tail_adv includes warm-up and covers
 * successful evict_one calls only, not whole bm_fix_pid operations.
 */
#include <errno.h>
#include <math.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bufmgr.h"
#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_stats.h"

static bm_t *g_bm;
static uint32_t g_n_pages;
static int64_t g_deadline_ns;
static double *g_cdf;
static uint32_t *g_rank_pid;
static _Atomic uint64_t g_ops, g_errs, g_max_ns;
static xtc_hist_t *g_lat;
static uint64_t *g_rng;

static uint64_t
xs64(uint64_t *s)
{
	uint64_t x = *s;
	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	return *s = x;
}

static uint32_t
zipf_rank(uint64_t *s)
{
	double u = (double)(xs64(s) >> 11) * (1.0 / 9007199254740992.0);
	uint32_t lo = 0, hi = g_n_pages - 1;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;
		if (g_cdf[mid] < u)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

static void
worker(void *arg)
{
	uint64_t *rng = arg;
	bm_frame_t *f;
	uint64_t n = 0;
	while (xtc_clock_mono() < g_deadline_ns) {
		uint32_t pid = g_rank_pid[zipf_rank(rng)];
		int64_t t0 = xtc_clock_mono(), dt;
		uint64_t cur;
		if (bm_fix_pid(g_bm, (bm_pid_t)pid, &f) != XTC_OK) {
			atomic_fetch_add(&g_errs, 1);
			return;
		}
		if (((const uint32_t *)bm_page(f))[0] != pid)
			atomic_fetch_add(&g_errs, 1);
		bm_unfix(g_bm, f, 0);
		dt = xtc_clock_mono() - t0;
		/* Histogram clamps at 2^24 units: us spans 16 s, ns only 16 ms. */
		xtc_hist_record(g_lat, dt / 1000);
		cur = atomic_load_explicit(&g_max_ns, memory_order_relaxed);
		while ((uint64_t)dt > cur && !atomic_compare_exchange_weak_explicit(
		    &g_max_ns, &cur, (uint64_t)dt, memory_order_relaxed,
		    memory_order_relaxed))
			;
		if ((++n & 63) == 0)
			xtc_yield();
	}
	atomic_fetch_add(&g_ops, n);
}

static int
window(xtc_exec_t *e, uint32_t loops, uint32_t fibers, uint32_t secs)
{
	uint32_t i;
	g_deadline_ns = xtc_clock_mono() + (int64_t)secs * 1000000000LL;
	for (i = 0; i < fibers; i++)
		if (xtc_proc_spawn(xtc_exec_loop(e, (int)(i % loops)), worker,
		    &g_rng[i], NULL, NULL) != XTC_OK)
			return -1;
	if (xtc_exec_run(e) != XTC_OK || atomic_load(&g_errs) != 0)
		return -1;
	return 0;
}

static int
build_store(const char *path, uint32_t n_pages)
{
	bm_opts_t bo = BM_OPTS_DEFAULT;
	bm_t *bm = NULL;
	bm_frame_t *f;
	bm_pid_t pid;
	uint32_t i;
	int rc = -1;
	bo.path = path;
	bo.n_frames = 1024;
	bo.claim_threshold = 0; /* identical setup independent of measured arm */
	if (bm_create(&bo, &bm) != XTC_OK)
		return -1;
	for (i = 0; i < n_pages; i++) {
		if (bm_alloc_pid(bm, &f, &pid) != XTC_OK)
			goto out;
		memset(bm_page(f), 0, bo.page_size);
		((uint32_t *)bm_page(f))[0] = (uint32_t)pid;
		bm_unfix(bm, f, 1);
	}
	if (bm_checkpoint(bm) == XTC_OK)
		rc = 0;
out:
	bm_destroy(bm);
	return rc;
}

static uint32_t
number(const char *s)
{
	char *end;
	unsigned long long v;
	errno = 0;
	v = strtoull(s, &end, 10);
	if (errno != 0 || s == end || *end != '\0' || *s == '-' || v > UINT32_MAX) {
		fprintf(stderr, "invalid unsigned argument: %s\n", s);
		exit(2);
	}
	return (uint32_t)v;
}

int
main(int argc, char **argv)
{
	uint32_t theta, frames, pages, i, secs, loops, fibers, direct, zs, seed;
	uint64_t allocs, adv, hits, loads, ops, shuf = 0xC0FFEEull;
	int steady = 0;
	int64_t start, elapsed;
	const char *path;
	double z, sum;
	struct stat st;
	xtc_exec_t *e = NULL;
	bm_opts_t bo = BM_OPTS_DEFAULT;
	bm_stats_t s0, s1;

	if (argc < 4 || argc > 11) {
		fprintf(stderr, "usage: %s theta seconds path [frames] [pages] "
		    "[loops] [fibers] [zipf_s_x100] [direct] [seed]\n", argv[0]);
		return 2;
	}
	theta = number(argv[1]);
	secs = number(argv[2]);
	path = argv[3];
	frames = argc > 4 ? number(argv[4]) : 65536;
	pages = argc > 5 ? number(argv[5]) : frames + frames / 8;
	loops = argc > 6 ? number(argv[6]) : 8;
	fibers = argc > 7 ? number(argv[7]) : 64;
	zs = argc > 8 ? number(argv[8]) : 110;
	direct = argc > 9 ? number(argv[9]) : 1;
	seed = argc > 10 ? number(argv[10]) : 42;
	if (secs == 0 || frames == 0 || pages <= frames || loops == 0 ||
	    loops > 1024 || fibers == 0 || fibers > 65536 || zs > 300 || direct > 1)
		return 2;
	g_n_pages = pages;
	if (stat(path, &st) != 0) {
		if (errno != ENOENT || build_store(path, pages) != 0)
			return 1;
	} else if (st.st_size != ((int64_t)pages + 1) * 4096) {
		fprintf(stderr, "existing store size does not match pages\n");
		return 1;
	}
	g_cdf = xtc_calloc(pages, sizeof *g_cdf);
	g_rank_pid = xtc_calloc(pages, sizeof *g_rank_pid);
	g_rng = xtc_calloc(fibers, sizeof *g_rng);
	if (g_cdf == NULL || g_rank_pid == NULL || g_rng == NULL)
		return 1;
	z = (double)zs / 100.0;
	for (sum = 0, i = 0; i < pages; i++)
		sum += 1.0 / pow((double)(i + 1), z);
	for (i = 0; i < pages; i++) {
		double prev = i ? g_cdf[i - 1] : 0.0;
		g_cdf[i] = prev + (1.0 / pow((double)(i + 1), z)) / sum;
		g_rank_pid[i] = i + 1;
	}
	for (i = pages - 1; i > 0; i--) {
		uint32_t j = (uint32_t)(xs64(&shuf) % (i + 1)), t;
		t = g_rank_pid[i];
		g_rank_pid[i] = g_rank_pid[j];
		g_rank_pid[j] = t;
	}
	for (i = 0; i < fibers; i++) {
		g_rng[i] = seed ^ (0x9E3779B97F4A7C15ull * ((uint64_t)i + 1));
		if (g_rng[i] == 0)
			g_rng[i] = 1;
	}
	bo.path = path;
	bo.n_frames = frames;
	bo.reopen = 1;
	bo.direct = (uint8_t)direct;
	bo.claim_threshold = theta;
	if (bm_create(&bo, &g_bm) != XTC_OK ||
	    xtc_hist_create("bm_sweep_fix_us", &g_lat) != XTC_OK ||
	    xtc_exec_init(&e, (int)loops) != XTC_OK)
		return 1;

	/* No fixed cold-start window: require eviction in three consecutive
	 * one-second windows. Each worker continues its RNG across windows. */
	bm_get_stats(g_bm, &s0);
	for (i = 0; i < 120 && steady < 3; i++) {
		if (window(e, loops, fibers, 1) != 0)
			return 1;
		bm_get_stats(g_bm, &s1);
		steady = s1.allocs > s0.allocs ? steady + 1 : 0;
		s0 = s1;
	}
	if (steady < 3) {
		fprintf(stderr, "warm-up never reached sustained eviction\n");
		return 1;
	}
	xtc_hist_destroy(g_lat);
	if (xtc_hist_create("bm_sweep_fix_us", &g_lat) != XTC_OK)
		return 1;
	atomic_store(&g_ops, 0);
	atomic_store(&g_max_ns, 0);
	start = xtc_clock_mono();
	if (window(e, loops, fibers, secs) != 0)
		return 1;
	elapsed = xtc_clock_mono() - start;
	bm_get_stats(g_bm, &s1);
	ops = atomic_load(&g_ops);
	allocs = s1.allocs - s0.allocs;
	adv = s1.advances - s0.advances;
	hits = s1.hits - s0.hits;
	loads = s1.loads - s0.loads;
	if (ops == 0 || allocs == 0)
		return 1;
	printf("%u,%u,%u,%u,%u,%u,%llu,%.0f,%.4f,%llu,%.3f,%llu,%llu,%lld,%lld,%lld,%llu\n",
	    theta, frames, pages, loops, fibers, secs, (unsigned long long)ops,
	    (double)ops * 1e9 / (double)elapsed,
	    hits + loads ? 100.0 * (double)hits / (double)(hits + loads) : 0.0,
	    (unsigned long long)allocs, (double)adv / (double)allocs,
	    (unsigned long long)s1.adv_max,
	    (unsigned long long)(s1.forced_claims - s0.forced_claims),
	    (long long)xtc_hist_quantile(g_lat, 0.50) * 1000,
	    (long long)xtc_hist_quantile(g_lat, 0.99) * 1000,
	    (long long)xtc_hist_quantile(g_lat, 0.999) * 1000,
	    (unsigned long long)atomic_load(&g_max_ns));
	xtc_exec_fini(e);
	bm_destroy(g_bm);
	xtc_hist_destroy(g_lat);
	xtc_free(g_cdf);
	xtc_free(g_rank_pid);
	xtc_free(g_rng);
	return 0;
}
