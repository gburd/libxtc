/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * src/ptc/stats.c -- xtc_stats implementation.
 *
 * Counter design:
 *   per-CPU shards, each on its own cache line.  xtc_counter_inc
 *   reads sched_getcpu() (cached in TLS), atomic-adds 1 to the
 *   corresponding shard.  Read sums all shards under no lock
 *   (relaxed atomic loads).  No cross-CPU coherence traffic on the
 *   hot path.
 *
 * Gauge design:
 *   single _Atomic int64.  Trivial.  For higher write rates we'd
 *   shard like the counter; today this is fine.
 *
 * Histogram design:
 *   we vendor the HDR-style histogram from bench/conformance/include/
 *   hist.h.  Per-CPU sharded for record(); merged on quantile query.
 *
 * Dist design:
 *   mergeable online mean/variance.  Each per-CPU shard holds a
 *   Welford triple (n, mean, m2); xtc_dist_record does the Welford
 *   single-pass update on the calling CPU's shard.  Welford is a
 *   read-modify-write of three fields, not a single atomic add, and
 *   two threads can migrate onto the same shard index, so each shard
 *   carries a tiny spinlock held only for the handful of arithmetic
 *   ops (this is NOT lock-free).  Reads take the same per-shard lock
 *   briefly to snapshot the triple (double is not naturally atomic),
 *   then merge all shards pairwise with Chan's parallel formula.
 *   Mean + population variance only -- no M3/M4.  Reads no clock,
 *   allocates nothing on the record path; NOT sim-reachable.
 *
 * Registry:
 *   global linked list, mutex for register/unregister, lock-free
 *   walk for the (rare) iteration.  Reading the linked list under
 *   no lock relies on append-only insert + atomic next pointers.
 */

#define _GNU_SOURCE

#include "xtc_int.h"
#include "preempt_int.h"   /* __xtc_unsafe_* / __xtc_mtx_*: internal preemption brackets */
#include "xtc_stats.h"
#include "os_tuning.h"   /* __os_tuning_check: xtc_tuning_check wraps it */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#if defined(__linux__)
#include <sched.h>
#endif

/* ---- per-CPU shard helpers ---- */

#ifndef XTC_CACHE_LINE
#define XTC_CACHE_LINE 64
#endif

#define XTC_STATS_NAME_MAX 64

static int
__ncpus(void)
{
	int n;
#if defined(__linux__)
	n = (int)sysconf(_SC_NPROCESSORS_ONLN);
#else
	n = 4;     /* sensible default for non-Linux */
#endif
	if (n < 1) n = 1;
	if (n > 256) n = 256;     /* clamp; oversharding hurts read */
	return n;
}

static XTC_THREAD_LOCAL int __cached_cpu = -1;

static int
__current_cpu(int n_cpus)
{
	if (__cached_cpu >= 0 && __cached_cpu < n_cpus) return __cached_cpu;
#if defined(__linux__)
	__cached_cpu = sched_getcpu();
	if (__cached_cpu < 0) __cached_cpu = 0;
	if (__cached_cpu >= n_cpus) __cached_cpu = __cached_cpu % n_cpus;
#else
	__cached_cpu = 0;
#endif
	return __cached_cpu;
}

/* ---- registry ---- */

struct stats_registry_entry {
	const void          *handle;
	xtc_metric_kind_t    kind;
	char                 name[XTC_STATS_NAME_MAX];
	struct stats_registry_entry *next;
};

static pthread_mutex_t __stats_reg_lock = PTHREAD_MUTEX_INITIALIZER;
static struct stats_registry_entry *__reg_head;

static void
__reg_add(const void *handle, xtc_metric_kind_t kind, const char *name)
{
	struct stats_registry_entry *e;
	if (__os_calloc(1, sizeof *e, (void **)&e) != XTC_OK) return;
	e->handle = handle;
	e->kind = kind;
	strncpy(e->name, name, XTC_STATS_NAME_MAX - 1);
	(void)__xtc_mtx_lock(&__stats_reg_lock);
	e->next = __reg_head;
	__reg_head = e;
	(void)__xtc_mtx_unlock(&__stats_reg_lock);
}

static void
__reg_remove(const void *handle)
{
	struct stats_registry_entry **link, *e;
	(void)__xtc_mtx_lock(&__stats_reg_lock);
	for (link = &__reg_head; (e = *link) != NULL; link = &e->next) {
		if (e->handle == handle) {
			*link = e->next;
			__os_free(e);
			break;
		}
	}
	(void)__xtc_mtx_unlock(&__stats_reg_lock);
}

/* ---- counter ---- */

struct counter_shard {
	_Alignas(XTC_CACHE_LINE) _Atomic uint64_t value;
	uint8_t pad[XTC_CACHE_LINE - sizeof(_Atomic uint64_t)];
};

struct xtc_counter {
	char                  name[XTC_STATS_NAME_MAX];
	int                   n_cpus;
	struct counter_shard *shards;
};

int
xtc_counter_create(const char *name, xtc_counter_t **out)
{
	xtc_counter_t *c;
	int rc;
	if (name == NULL || out == NULL) return XTC_E_INVAL;
	if ((rc = __os_calloc(1, sizeof *c, (void **)&c)) != XTC_OK) return rc;
	strncpy(c->name, name, XTC_STATS_NAME_MAX - 1);
	c->n_cpus = __ncpus();
	/* counter_shard is _Alignas(XTC_CACHE_LINE); the array base must
	 * be cache-line aligned or shard[0] is misaligned.  calloc only
	 * guarantees max_align_t, so allocate aligned and zero. */
	if ((rc = __os_aligned_alloc(XTC_CACHE_LINE,
	    (size_t)c->n_cpus * sizeof *c->shards,
	    (void **)&c->shards)) != XTC_OK) {
		__os_free(c);
		return rc;
	}
	memset(c->shards, 0, (size_t)c->n_cpus * sizeof *c->shards);
	__reg_add(c, XTC_METRIC_COUNTER, c->name);
	*out = c;
	return XTC_OK;
}

void
xtc_counter_destroy(xtc_counter_t *c)
{
	if (c == NULL) return;
	__reg_remove(c);
	__os_aligned_free(c->shards);
	__os_free(c);
}

void
xtc_counter_inc(xtc_counter_t *c)
{
	if (XTC_UNLIKELY(c == NULL)) return;
	atomic_fetch_add_explicit(
	    &c->shards[__current_cpu(c->n_cpus)].value,
	    1, memory_order_relaxed);
}

void
xtc_counter_add(xtc_counter_t *c, int64_t delta)
{
	if (XTC_UNLIKELY(c == NULL)) return;
	atomic_fetch_add_explicit(
	    &c->shards[__current_cpu(c->n_cpus)].value,
	    (uint64_t)delta, memory_order_relaxed);
}

uint64_t
xtc_counter_read(const xtc_counter_t *c)
{
	uint64_t total = 0;
	int i;
	if (c == NULL) return 0;
	for (i = 0; i < c->n_cpus; i++)
		total += atomic_load_explicit(&c->shards[i].value,
		    memory_order_relaxed);
	return total;
}

/* ---- gauge ---- */

struct xtc_gauge {
	char              name[XTC_STATS_NAME_MAX];
	_Atomic int64_t   value;
};

int
xtc_gauge_create(const char *name, xtc_gauge_t **out)
{
	xtc_gauge_t *g;
	int rc;
	if (name == NULL || out == NULL) return XTC_E_INVAL;
	if ((rc = __os_calloc(1, sizeof *g, (void **)&g)) != XTC_OK) return rc;
	strncpy(g->name, name, XTC_STATS_NAME_MAX - 1);
	__reg_add(g, XTC_METRIC_GAUGE, g->name);
	*out = g;
	return XTC_OK;
}

void
xtc_gauge_destroy(xtc_gauge_t *g)
{
	if (g == NULL) return;
	__reg_remove(g);
	__os_free(g);
}

void
xtc_gauge_set(xtc_gauge_t *g, int64_t v)
{
	if (XTC_UNLIKELY(g == NULL)) return;
	atomic_store_explicit(&g->value, v, memory_order_relaxed);
}

void
xtc_gauge_add(xtc_gauge_t *g, int64_t delta)
{
	if (XTC_UNLIKELY(g == NULL)) return;
	atomic_fetch_add_explicit(&g->value, delta, memory_order_relaxed);
}

int64_t
xtc_gauge_read(const xtc_gauge_t *g)
{
	if (g == NULL) return 0;
	return atomic_load_explicit(&g->value, memory_order_relaxed);
}

/* ---- histogram ----
 * Simple log-linear histogram for nanosecond latencies: 24 powers
 * of two from 1ns to ~16ms, with 16 sub-buckets each = 384 buckets.
 * Per-CPU sharded for record; merged on quantile query.
 */

#define XTC_HIST_BASE_BITS  24
#define XTC_HIST_SUB_BITS   4
#define XTC_HIST_SUBS       (1 << XTC_HIST_SUB_BITS)
#define XTC_HIST_BUCKETS    (XTC_HIST_BASE_BITS * XTC_HIST_SUBS)

struct hist_shard {
	_Alignas(XTC_CACHE_LINE) _Atomic uint64_t buckets[XTC_HIST_BUCKETS];
	_Atomic uint64_t count;
};

struct xtc_hist {
	char                name[XTC_STATS_NAME_MAX];
	int                 n_cpus;
	struct hist_shard  *shards;
};

static int
__hist_bucket(int64_t value_ns)
{
	int base, sub;
	uint64_t v;
	if (value_ns <= 0) return 0;
	v = (uint64_t)value_ns;
	if (v >= (1ULL << XTC_HIST_BASE_BITS)) v = (1ULL << XTC_HIST_BASE_BITS) - 1;
	/* Find highest set bit. */
#if defined(__GNUC__) || defined(__clang__)
	base = 63 - XTC_CLZLL(v);
#else
	base = 0; { uint64_t x = v; while (x >>= 1) base++; }
#endif
	if (base < 0) base = 0;
	if (base >= XTC_HIST_BASE_BITS) base = XTC_HIST_BASE_BITS - 1;
	/* Sub-bucket: top XTC_HIST_SUB_BITS bits below the base. */
	if (base >= XTC_HIST_SUB_BITS) {
		sub = (int)((v >> (base - XTC_HIST_SUB_BITS)) & (XTC_HIST_SUBS - 1));
	} else {
		sub = 0;
	}
	return base * XTC_HIST_SUBS + sub;
}

static int64_t
__hist_bucket_lower_bound(int idx)
{
	int base = idx / XTC_HIST_SUBS;
	int sub = idx % XTC_HIST_SUBS;
	int64_t v;
	if (base >= XTC_HIST_SUB_BITS)
		v = ((int64_t)1 << base) | ((int64_t)sub << (base - XTC_HIST_SUB_BITS));
	else
		v = (int64_t)1 << base;
	return v;
}

int
xtc_hist_create(const char *name, xtc_hist_t **out)
{
	xtc_hist_t *h;
	int rc;
	if (name == NULL || out == NULL) return XTC_E_INVAL;
	if ((rc = __os_calloc(1, sizeof *h, (void **)&h)) != XTC_OK) return rc;
	strncpy(h->name, name, XTC_STATS_NAME_MAX - 1);
	h->n_cpus = __ncpus();
	/* hist_shard is _Alignas(XTC_CACHE_LINE); same as the counter
	 * shards, the array base must be cache-line aligned. */
	if ((rc = __os_aligned_alloc(XTC_CACHE_LINE,
	    (size_t)h->n_cpus * sizeof *h->shards,
	    (void **)&h->shards)) != XTC_OK) {
		__os_free(h);
		return rc;
	}
	memset(h->shards, 0, (size_t)h->n_cpus * sizeof *h->shards);
	__reg_add(h, XTC_METRIC_HIST, h->name);
	*out = h;
	return XTC_OK;
}

void
xtc_hist_destroy(xtc_hist_t *h)
{
	if (h == NULL) return;
	__reg_remove(h);
	__os_aligned_free(h->shards);
	__os_free(h);
}

void
xtc_hist_record(xtc_hist_t *h, int64_t value_ns)
{
	int b;
	struct hist_shard *s;
	if (XTC_UNLIKELY(h == NULL)) return;
	b = __hist_bucket(value_ns);
	s = &h->shards[__current_cpu(h->n_cpus)];
	atomic_fetch_add_explicit(&s->buckets[b], 1, memory_order_relaxed);
	atomic_fetch_add_explicit(&s->count, 1, memory_order_relaxed);
}

uint64_t
xtc_hist_count(const xtc_hist_t *h)
{
	uint64_t total = 0;
	int i;
	if (h == NULL) return 0;
	for (i = 0; i < h->n_cpus; i++)
		total += atomic_load_explicit(&h->shards[i].count,
		    memory_order_relaxed);
	return total;
}

int64_t
xtc_hist_quantile(const xtc_hist_t *h, double q)
{
	uint64_t total, target, running = 0;
	int b, i;
	if (h == NULL) return 0;
	total = xtc_hist_count(h);
	if (total == 0) return 0;
	if (q < 0.0) q = 0.0; else if (q > 1.0) q = 1.0;
	target = (uint64_t)((double)total * q);
	if (target < 1) target = 1;
	for (b = 0; b < XTC_HIST_BUCKETS; b++) {
		uint64_t bucket_total = 0;
		for (i = 0; i < h->n_cpus; i++)
			bucket_total += atomic_load_explicit(
			    &h->shards[i].buckets[b],
			    memory_order_relaxed);
		running += bucket_total;
		if (running >= target)
			return __hist_bucket_lower_bound(b);
	}
	return __hist_bucket_lower_bound(XTC_HIST_BUCKETS - 1);
}

/* ---- dist (mergeable online mean/variance) ----
 * Welford's single-pass update per per-CPU shard; Chan et al. (1983)
 * parallel merge of the (n, mean, m2) triples on read.  Mean and
 * population variance only; no higher moments.
 */

struct dist_shard {
	/* Align the whole struct to a cache line and order the fields
	 * naturally (n, mean, m2 are 8-byte; the 1-byte lock last) so the
	 * shard is EXACTLY one cache line.  Putting the over-aligned
	 * attribute on a leading 1-byte member instead would leave a
	 * 7-byte gap and round the struct up to two lines. */
	_Alignas(XTC_CACHE_LINE) uint64_t n;
	double      mean;
	double      m2;
	atomic_flag lock;
	uint8_t     pad[XTC_CACHE_LINE - sizeof(uint64_t)
	    - 2 * sizeof(double) - sizeof(atomic_flag)];
};

struct xtc_dist {
	char               name[XTC_STATS_NAME_MAX];
	int                n_cpus;
	struct dist_shard *shards;
};

/*
 * Per-shard spinlock.  The critical section is a few arithmetic ops
 * (Welford update on write, a 3-field snapshot on read), never a
 * blocking call, so a bare test-and-set spin is the right weight;
 * bracket it with the preemption-defer helpers so a fiber that owns
 * the lock is not involuntarily preempted mid-update.
 */
static void
__dist_shard_lock(struct dist_shard *s)
{
	__xtc_unsafe_enter();
	while (atomic_flag_test_and_set_explicit(&s->lock,
	    memory_order_acquire))
		;
}

static void
__dist_shard_unlock(struct dist_shard *s)
{
	atomic_flag_clear_explicit(&s->lock, memory_order_release);
	__xtc_unsafe_leave();
}

int
xtc_dist_create(const char *name, xtc_dist_t **out)
{
	xtc_dist_t *d;
	int rc, i;
	if (name == NULL || out == NULL) return XTC_E_INVAL;
	if ((rc = __os_calloc(1, sizeof *d, (void **)&d)) != XTC_OK) return rc;
	strncpy(d->name, name, XTC_STATS_NAME_MAX - 1);
	d->n_cpus = __ncpus();
	/* dist_shard is _Alignas(XTC_CACHE_LINE); like the counter and
	 * hist shards the array base must be cache-line aligned or
	 * shard[0] is misaligned.  calloc only guarantees max_align_t. */
	if ((rc = __os_aligned_alloc(XTC_CACHE_LINE,
	    (size_t)d->n_cpus * sizeof *d->shards,
	    (void **)&d->shards)) != XTC_OK) {
		__os_free(d);
		return rc;
	}
	memset(d->shards, 0, (size_t)d->n_cpus * sizeof *d->shards);
	/* memset-0 is a cleared atomic_flag on every supported target. */
	for (i = 0; i < d->n_cpus; i++)
		atomic_flag_clear_explicit(&d->shards[i].lock,
		    memory_order_relaxed);
	__reg_add(d, XTC_METRIC_DIST, d->name);
	*out = d;
	return XTC_OK;
}

void
xtc_dist_destroy(xtc_dist_t *d)
{
	if (d == NULL) return;
	__reg_remove(d);
	__os_aligned_free(d->shards);
	__os_free(d);
}

void
xtc_dist_record(xtc_dist_t *d, double value)
{
	struct dist_shard *s;
	double delta, delta2;
	if (XTC_UNLIKELY(d == NULL)) return;
	s = &d->shards[__current_cpu(d->n_cpus)];
	__dist_shard_lock(s);
	/* Welford single-pass update of this shard's (n, mean, m2). */
	s->n++;
	delta = value - s->mean;
	s->mean += delta / (double)s->n;
	delta2 = value - s->mean;
	s->m2 += delta * delta2;
	__dist_shard_unlock(s);
}

/*
 * Snapshot + Chan merge of all shards into (out_n, out_mean, out_m2),
 * starting from the empty accumulator (0, 0, 0).  Plain pairwise Chan
 * merge; no compensated summation.
 * ponytail: defer Neumaier compensation unless a consumer proves drift.
 */
static void
__dist_merge(const xtc_dist_t *d, uint64_t *out_n, double *out_mean,
    double *out_m2)
{
	uint64_t n_acc = 0;
	double mean_acc = 0.0, m2_acc = 0.0;
	int i;
	for (i = 0; i < d->n_cpus; i++) {
		struct dist_shard *s = &d->shards[i];
		uint64_t n_b, n_tot;
		double mean_b, m2_b, delta;
		/* Snapshot the shard triple under its lock; double is not
		 * naturally atomic and the writer updates all three. */
		__dist_shard_lock(s);
		n_b = s->n;
		mean_b = s->mean;
		m2_b = s->m2;
		__dist_shard_unlock(s);
		if (n_b == 0) continue;
		n_tot = n_acc + n_b;
		delta = mean_b - mean_acc;
		mean_acc += delta * (double)n_b / (double)n_tot;
		m2_acc += m2_b + delta * delta
		    * (double)n_acc * (double)n_b / (double)n_tot;
		n_acc = n_tot;
	}
	*out_n = n_acc;
	*out_mean = n_acc == 0 ? 0.0 : mean_acc;
	*out_m2 = m2_acc;
}

uint64_t
xtc_dist_count(const xtc_dist_t *d)
{
	uint64_t n;
	double mean, m2;
	if (d == NULL) return 0;
	__dist_merge(d, &n, &mean, &m2);
	return n;
}

double
xtc_dist_mean(const xtc_dist_t *d)
{
	uint64_t n;
	double mean, m2;
	if (d == NULL) return 0.0;
	__dist_merge(d, &n, &mean, &m2);
	return n == 0 ? 0.0 : mean;
}

double
xtc_dist_variance(const xtc_dist_t *d)
{
	uint64_t n;
	double mean, m2;
	if (d == NULL) return 0.0;
	__dist_merge(d, &n, &mean, &m2);
	if (n < 2) return 0.0;
	return m2 / (double)n;   /* population variance */
}

double
xtc_dist_stddev(const xtc_dist_t *d)
{
	double v = xtc_dist_variance(d);
	/* sqrt via Newton-Raphson rather than libm: libxtc links no -lm
	 * and adding that dependency library-wide (onto every consumer's
	 * link line) for one square root is not worth it.  The argument
	 * is a variance -- always finite and >= 0 -- so a handful of
	 * iterations from a decent seed converges to full double
	 * precision.  ponytail: swap for sqrt() if libxtc ever grows a
	 * real libm dependency. */
	double x, prev;
	int i;
	if (!(v > 0.0)) return 0.0;   /* 0, -0, and NaN-safe */
	x = v;                        /* seed; monotone-converges downward */
	for (i = 0; i < 64; i++) {
		prev = x;
		x = 0.5 * (x + v / x);
		if (x >= prev) break;     /* no further improvement */
	}
	return x;
}

/* ---- iteration / dump ---- */

int
xtc_metrics_iterate(xtc_metric_visit_fn fn, void *user)
{
	struct stats_registry_entry *e;
	int n = 0;
	if (fn == NULL) return XTC_E_INVAL;
	(void)__xtc_mtx_lock(&__stats_reg_lock);
	for (e = __reg_head; e != NULL; e = e->next) {
		if (fn(e->name, e->kind, e->handle, user) != 0) break;
		n++;
	}
	(void)__xtc_mtx_unlock(&__stats_reg_lock);
	return n;
}

static int
__dump_prom_visit(const char *name, xtc_metric_kind_t kind,
    const void *handle, void *user)
{
	int fd = (int)(intptr_t)user;
	char buf[256];
	int n = 0;
	switch (kind) {
	case XTC_METRIC_COUNTER:
		n = snprintf(buf, sizeof buf,
		    "# TYPE %s counter\n%s %llu\n", name, name,
		    (unsigned long long)xtc_counter_read(
		        (const xtc_counter_t *)handle));
		break;
	case XTC_METRIC_GAUGE:
		n = snprintf(buf, sizeof buf,
		    "# TYPE %s gauge\n%s %lld\n", name, name,
		    (long long)xtc_gauge_read(
		        (const xtc_gauge_t *)handle));
		break;
	case XTC_METRIC_HIST: {
		const xtc_hist_t *h = (const xtc_hist_t *)handle;
		uint64_t cnt = xtc_hist_count(h);
		int64_t p50 = xtc_hist_quantile(h, 0.50);
		int64_t p99 = xtc_hist_quantile(h, 0.99);
		int64_t p999 = xtc_hist_quantile(h, 0.999);
		n = snprintf(buf, sizeof buf,
		    "# TYPE %s histogram\n"
		    "%s_count %llu\n"
		    "%s_p50 %lld\n"
		    "%s_p99 %lld\n"
		    "%s_p999 %lld\n",
		    name, name, (unsigned long long)cnt,
		    name, (long long)p50,
		    name, (long long)p99,
		    name, (long long)p999);
		break;
	}
	case XTC_METRIC_DIST: {
		const xtc_dist_t *dd = (const xtc_dist_t *)handle;
		n = snprintf(buf, sizeof buf,
		    "# TYPE %s summary\n"
		    "%s_count %llu\n"
		    "%s_mean %.17g\n"
		    "%s_variance %.17g\n"
		    "%s_stddev %.17g\n",
		    name, name,
		    (unsigned long long)xtc_dist_count(dd),
		    name, xtc_dist_mean(dd),
		    name, xtc_dist_variance(dd),
		    name, xtc_dist_stddev(dd));
		break;
	}
	}
	if (n > 0 && fd >= 0) {
		ssize_t w = write(fd, buf, (size_t)n);  /* XTC_BLOCKING_OK: dump path */
		(void)w;
	}
	return 0;
}

int
xtc_metrics_dump_prometheus(int fd)
{
	if (fd < 0) return XTC_E_INVAL;
	return xtc_metrics_iterate(__dump_prom_visit, (void *)(intptr_t)fd);
}

/*
 * PUBLIC: void xtc_tuning_check __P((void));
 *
 * Public entry to the host-tuning advisor.  Thin wrapper over the
 * internal __os_tuning_check so a consumer on the xtc_exec / xtc_loop
 * bring-up path can trigger the advisory probes (and route their
 * XTC_LOG_INFO output to its installed xtc_log_default sink) without
 * going through xtc_app_start.  See xtc_stats.h.
 */
void
xtc_tuning_check(void)
{
	__os_tuning_check();
}
