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
 *   single atomic int64.  Trivial.  For higher write rates we'd
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
#include "xtc_runtime.h"
#include "os_tuning.h"   /* __os_tuning_check: xtc_runtime_tuning_check wraps it */

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h> /* HUGE_VAL (a constant: no -lm) */
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
	char *help; /* xtc_metric_set_help; NULL = none */
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
			if (e->help != NULL)
				__os_free(e->help);
			__os_free(e);
			break;
		}
	}
	(void)__xtc_mtx_unlock(&__stats_reg_lock);
}

/* ---- counter ---- */

struct counter_shard {
	_Alignas(XTC_CACHE_LINE) uint64_t value;
	uint8_t pad[XTC_CACHE_LINE - sizeof(uint64_t)];
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
	xtc_atomic_fetch_add_u64(&c->shards[__current_cpu(c->n_cpus)].value, 1,
	    xtc_mo_relaxed);
}

void
xtc_counter_add(xtc_counter_t *c, int64_t delta)
{
	if (XTC_UNLIKELY(c == NULL)) return;
	xtc_atomic_fetch_add_u64(&c->shards[__current_cpu(c->n_cpus)].value,
	    (uint64_t)delta, xtc_mo_relaxed);
}

uint64_t
xtc_counter_read(const xtc_counter_t *c)
{
	uint64_t total = 0;
	int i;
	if (c == NULL) return 0;
	for (i = 0; i < c->n_cpus; i++)
		total +=
		    xtc_atomic_load_u64(&c->shards[i].value, xtc_mo_relaxed);
	return total;
}

/* ---- gauge ---- */

struct xtc_gauge {
	char              name[XTC_STATS_NAME_MAX];
	int64_t value;
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
	xtc_atomic_store_i64(&g->value, v, xtc_mo_relaxed);
}

void
xtc_gauge_add(xtc_gauge_t *g, int64_t delta)
{
	if (XTC_UNLIKELY(g == NULL)) return;
	xtc_atomic_fetch_add_i64(&g->value, delta, xtc_mo_relaxed);
}

int64_t
xtc_gauge_read(const xtc_gauge_t *g)
{
	if (g == NULL) return 0;
	return xtc_atomic_load_i64(&g->value, xtc_mo_relaxed);
}

/* ---- histogram ----
 * Log-linear histogram for nanosecond latencies: 40 powers of two
 * (1 ns .. 2^40 ns, about 18 minutes) with 16 sub-buckets each = 640
 * buckets, so a bucket is at most 1/16 of its value wide (values below
 * 16 ns are exact).  Values >= 2^40 are counted in the top bucket.
 * Each shard also keeps the exact largest value it recorded.  Per-CPU
 * sharded for record; merged on query.  A record is ONE atomic add (its
 * bucket) plus a relaxed load of the shard's max; the count is the sum
 * of the buckets, computed by the reader.
 */

#define XTC_HIST_BASE_BITS  40
#define XTC_HIST_SUB_BITS   4
#define XTC_HIST_SUBS       (1 << XTC_HIST_SUB_BITS)
#define XTC_HIST_BUCKETS    (XTC_HIST_BASE_BITS * XTC_HIST_SUBS)

struct hist_shard {
	_Alignas(XTC_CACHE_LINE) uint64_t buckets[XTC_HIST_BUCKETS];
	int64_t max;
	uint64_t sum; /* of the recorded values; a wrap reads as a reset */
};

/*
 * A fixed-bound histogram (xtc_hist_create_bounds): the caller's upper
 * bounds, ascending, and one count per bound plus the +Inf bucket.  A
 * value lands in the first bucket whose bound is >= it (Prometheus's
 * `le`).  The sum is a double, stored as its bits and added with a CAS
 * (uncontended unless two threads share a shard).
 */
#define XTC_HIST_BOUNDS_MAX 64

struct bhist_shard {
	_Alignas(XTC_CACHE_LINE) uint64_t buckets[XTC_HIST_BOUNDS_MAX + 1];
	uint64_t sum_bits; /* a double */
};

struct xtc_hist {
	char                name[XTC_STATS_NAME_MAX];
	int                 n_cpus;
	struct hist_shard *shards; /* log-linear; NULL for a bounds hist */
	int n_bounds;              /* bounds hist only */
	double bounds[XTC_HIST_BOUNDS_MAX];
	struct bhist_shard *bshards; /* bounds hist only */
	uint64_t scale_bits;         /* a double: the exposition factor
	                              * (xtc_hist_set_scale) */
};

static int
__hist_bucket(int64_t value_ns)
{
	int base, sub;
	uint64_t v;
	if (value_ns <= 0) return 0;
	v = (uint64_t)value_ns;
	if (v >= (1ULL << XTC_HIST_BASE_BITS)) v = (1ULL << XTC_HIST_BASE_BITS) - 1;
	base = 63 - XTC_CLZLL(v);   /* highest set bit; v >= 1 */
	/* Sub-bucket: the XTC_HIST_SUB_BITS bits below the base.  Below
	 * 2^SUB_BITS there are fewer bits than sub-buckets, so the value
	 * is shifted up and each bucket holds exactly one value. */
	if (base >= XTC_HIST_SUB_BITS)
		sub = (int)((v >> (base - XTC_HIST_SUB_BITS)) & (XTC_HIST_SUBS - 1));
	else
		sub = (int)((v << (XTC_HIST_SUB_BITS - base)) & (XTC_HIST_SUBS - 1));
	return base * XTC_HIST_SUBS + sub;
}

/* The smallest value bucket idx holds. */
static int64_t
__hist_bucket_lower_bound(int idx)
{
	int base = idx / XTC_HIST_SUBS;
	int sub = idx % XTC_HIST_SUBS;
	if (base >= XTC_HIST_SUB_BITS)
		return ((int64_t)1 << base) |
		    ((int64_t)sub << (base - XTC_HIST_SUB_BITS));
	return ((int64_t)1 << base) | (sub >> (XTC_HIST_SUB_BITS - base));
}

/* The largest value bucket idx holds (inclusive). */
static int64_t
__hist_bucket_upper_bound(int idx)
{
	int base = idx / XTC_HIST_SUBS;
	int sub = idx % XTC_HIST_SUBS;
	if (base >= XTC_HIST_SUB_BITS)
		return (((int64_t)1 << base) |
		    ((int64_t)sub << (base - XTC_HIST_SUB_BITS))) +
		    ((int64_t)1 << (base - XTC_HIST_SUB_BITS)) - 1;
	return ((int64_t)1 << base) | (sub >> (XTC_HIST_SUB_BITS - base));
}

static uint64_t
__hist_one_bits(void)
{
	double one = 1.0;
	uint64_t bits;
	memcpy(&bits, &one, sizeof bits);
	return bits;
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
	h->scale_bits = __hist_one_bits();
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

/* PUBLIC: int xtc_hist_create_bounds __P((const char *, const double *, int,
 * xtc_hist_t **)); */
int
xtc_hist_create_bounds(const char *name, const double *upper_bounds,
    int n_bounds, xtc_hist_t **out)
{
	xtc_hist_t *h;
	int rc, i;

	if (name == NULL || out == NULL || upper_bounds == NULL ||
	    n_bounds < 1 || n_bounds > XTC_HIST_BOUNDS_MAX)
		return XTC_E_INVAL;
	/* Finite, >= 0 and strictly ascending (a NaN fails each test). */
	for (i = 0; i < n_bounds; i++)
		if (!(upper_bounds[i] >= 0.0 && upper_bounds[i] < 1e300) ||
		    (i > 0 && !(upper_bounds[i] > upper_bounds[i - 1])))
			return XTC_E_INVAL;
	if ((rc = __os_calloc(1, sizeof *h, (void **)&h)) != XTC_OK)
		return rc;
	strncpy(h->name, name, XTC_STATS_NAME_MAX - 1);
	h->n_cpus = __ncpus();
	h->scale_bits = __hist_one_bits();
	h->n_bounds = n_bounds;
	memcpy(h->bounds, upper_bounds, (size_t)n_bounds * sizeof h->bounds[0]);
	/* bhist_shard is over-aligned: allocate the array aligned. */
	if ((rc = __os_aligned_alloc(XTC_CACHE_LINE,
	         (size_t)h->n_cpus * sizeof *h->bshards,
	         (void **)&h->bshards)) != XTC_OK) {
		__os_free(h);
		return rc;
	}
	memset(h->bshards, 0, (size_t)h->n_cpus * sizeof *h->bshards);
	__reg_add(h, XTC_METRIC_HIST, h->name);
	*out = h;
	return XTC_OK;
}

void
xtc_hist_destroy(xtc_hist_t *h)
{
	if (h == NULL) return;
	__reg_remove(h);
	if (h->shards != NULL)
		__os_aligned_free(h->shards);
	if (h->bshards != NULL)
		__os_aligned_free(h->bshards);
	__os_free(h);
}

/* Record into a fixed-bound histogram: a binary search for the first
 * bound >= v, one atomic add to that bucket and one to the sum. */
static void
__bhist_record(xtc_hist_t *h, double v)
{
	struct bhist_shard *s;
	int lo = 0, hi = h->n_bounds, mid;
	uint64_t old, bits;
	double sum;

	if (!(v >= 0.0))
		v = 0.0; /* negative and NaN count as 0 */
	while (lo < hi) {
		mid = (lo + hi) / 2;
		if (v <= h->bounds[mid])
			hi = mid;
		else
			lo = mid + 1;
	}
	s = &h->bshards[__current_cpu(h->n_cpus)];
	xtc_atomic_fetch_add_u64(&s->buckets[lo], 1, xtc_mo_relaxed);
	old = xtc_atomic_load_u64(&s->sum_bits, xtc_mo_relaxed);
	do {
		memcpy(&sum, &old, sizeof sum);
		sum += v;
		memcpy(&bits, &sum, sizeof bits);
	} while (!xtc_atomic_cas_weak_u64(&s->sum_bits, &old, bits,
	    xtc_mo_relaxed, xtc_mo_relaxed));
}

/* PUBLIC: void xtc_hist_record_value __P((xtc_hist_t *, double)); */
void
xtc_hist_record_value(xtc_hist_t *h, double v)
{
	if (XTC_UNLIKELY(h == NULL))
		return;
	if (h->bshards != NULL) {
		__bhist_record(h, v);
		return;
	}
	if (!(v >= 0.0))
		v = 0.0;
	xtc_hist_record(h, v < 9.2e18 ? (int64_t)(v + 0.5) : INT64_MAX);
}

void
xtc_hist_record(xtc_hist_t *h, int64_t value_ns)
{
	int b;
	struct hist_shard *s;
	int64_t cur;
	if (XTC_UNLIKELY(h == NULL)) return;
	if (XTC_UNLIKELY(h->bshards != NULL)) {
		__bhist_record(h, (double)value_ns);
		return;
	}
	b = __hist_bucket(value_ns);
	s = &h->shards[__current_cpu(h->n_cpus)];
	xtc_atomic_fetch_add_u64(&s->buckets[b], 1, xtc_mo_relaxed);
	xtc_atomic_fetch_add_u64(&s->sum, value_ns > 0 ? (uint64_t)value_ns : 0,
	    xtc_mo_relaxed);
	/* Exact max: a plain load; a CAS only for a new max. */
	cur = xtc_atomic_load_i64(&s->max, xtc_mo_relaxed);
	while (XTC_UNLIKELY(value_ns > cur) &&
	    !xtc_atomic_cas_weak_i64(&s->max, &cur, value_ns, xtc_mo_relaxed,
	        xtc_mo_relaxed))
		;
}

static uint64_t
__hist_bucket_total(const xtc_hist_t *h, int b)
{
	uint64_t total = 0;
	int i;
	for (i = 0; i < h->n_cpus; i++)
		total += xtc_atomic_load_u64(&h->shards[i].buckets[b],
		    xtc_mo_relaxed);
	return total;
}

static uint64_t
__bhist_bucket_total(const xtc_hist_t *h, int b)
{
	uint64_t total = 0;
	int i;
	for (i = 0; i < h->n_cpus; i++)
		total += xtc_atomic_load_u64(&h->bshards[i].buckets[b],
		    xtc_mo_relaxed);
	return total;
}

uint64_t
xtc_hist_count(const xtc_hist_t *h)
{
	uint64_t total = 0;
	int b;
	if (h == NULL) return 0;
	if (h->bshards != NULL) {
		for (b = 0; b <= h->n_bounds; b++)
			total += __bhist_bucket_total(h, b);
		return total;
	}
	for (b = 0; b < XTC_HIST_BUCKETS; b++)
		total += __hist_bucket_total(h, b);
	return total;
}

int64_t
xtc_hist_max(const xtc_hist_t *h)
{
	int64_t m = 0, v;
	int i;
	if (h == NULL || h->bshards != NULL)
		return 0;
	for (i = 0; i < h->n_cpus; i++) {
		v = xtc_atomic_load_i64(&h->shards[i].max, xtc_mo_relaxed);
		if (v > m) m = v;
	}
	return m;
}

/* PUBLIC: double xtc_hist_sum __P((const xtc_hist_t *)); */
double
xtc_hist_sum(const xtc_hist_t *h)
{
	double sum = 0.0, v;
	uint64_t bits;
	int i;
	if (h == NULL)
		return 0.0;
	for (i = 0; i < h->n_cpus; i++) {
		if (h->bshards != NULL) {
			bits = xtc_atomic_load_u64(&h->bshards[i].sum_bits,
			    xtc_mo_relaxed);
			memcpy(&v, &bits, sizeof v);
			sum += v;
		} else {
			sum += (double)xtc_atomic_load_u64(&h->shards[i].sum,
			    xtc_mo_relaxed);
		}
	}
	return sum;
}

/* The rank a quantile q of `total` values asks for (nearest rank). */
static uint64_t
__hist_rank(uint64_t total, double q)
{
	uint64_t target;
	double want;

	if (!(q > 0.0)) q = 0.0; else if (q > 1.0) q = 1.0;   /* NaN -> 0 */
	want = (double)total * q;
	target = (uint64_t)want;
	/* ceil, without turning a rounding error (0.07 * 100 =
	 * 7.000000000000001) into the next rank. */
	if (want - (double)target > want * 1e-12) target++;
	if (target < 1) target = 1;
	if (target > total) target = total;
	return target;
}

/*
 * The answer for a quantile whose rank landed in bucket b: the bucket's
 * upper bound (as HdrHistogram does), never above the exact max.  The
 * max is read after the buckets, so it covers every value they counted,
 * except a record still between its add and its max update: a max below
 * this bucket is that one, and is ignored.  The top bucket also holds
 * every value >= 2^40: it answers the max.
 */
static int64_t
__hist_answer(int b, int64_t max)
{
	int64_t lb, ub;

	if (b == 0) return max > 0 ? 1 : 0;   /* values <= 1; max is never < 0 */
	lb = __hist_bucket_lower_bound(b);
	ub = __hist_bucket_upper_bound(b);
	if (max >= lb && (max < ub || b == XTC_HIST_BUCKETS - 1))
		return max;
	return ub;
}

/*
 * Nearest rank: for each q, the smallest recorded value v with at least
 * ceil(q * count) values <= v, reported as described above, so >= the
 * true quantile and within one sub-bucket (1/16) of it.  Two passes over
 * the shards whatever the number of quantiles: one for the count, one
 * walk that settles every rank.  The buckets only grow between them, so
 * the walk reaches every target.  A fixed-bound histogram has no
 * quantiles: every answer is 0.
 */
/* PUBLIC: int xtc_hist_quantiles __P((const xtc_hist_t *, const double *, int,
 * int64_t *, uint64_t *)); */
int
xtc_hist_quantiles(const xtc_hist_t *h, const double *qs, int n, int64_t *out,
    uint64_t *out_count)
{
	uint64_t total, running = 0, lo;
	int64_t max;
	int b, i, left;

	if (h == NULL || n < 0 || (n > 0 && (qs == NULL || out == NULL)))
		return XTC_E_INVAL;
	total = xtc_hist_count(h);
	if (out_count != NULL)
		*out_count = total;
	for (i = 0; i < n; i++)
		out[i] = 0;
	if (h->bshards != NULL || total == 0 || n == 0)
		return XTC_OK;

	/* Settle the ranks in one walk: each bucket decides every
	 * quantile whose rank it reaches.  out[i] holds the bucket index
	 * + 1 until the max is known. */
	left = n;
	for (b = 0; b < XTC_HIST_BUCKETS && left > 0; b++) {
		lo = running;
		running += __hist_bucket_total(h, b);
		if (running == lo && b < XTC_HIST_BUCKETS - 1)
			continue;
		for (i = 0; i < n; i++)
			if (out[i] == 0 &&
			    (running >= __hist_rank(total, qs[i]) ||
			        b == XTC_HIST_BUCKETS - 1)) {
				out[i] = b + 1;
				left--;
			}
	}
	max = xtc_hist_max(h);
	for (i = 0; i < n; i++)
		out[i] = __hist_answer(out[i] > 0 ? (int)out[i] - 1 :
		                                    XTC_HIST_BUCKETS - 1,
		    max);
	return XTC_OK;
}

int64_t
xtc_hist_quantile(const xtc_hist_t *h, double q)
{
	int64_t v = 0;
	if (xtc_hist_quantiles(h, &q, 1, &v, NULL) != XTC_OK)
		return 0;
	return v;
}

/*
 * One bucket at a time, lowest first: (lower, upper] in the recorded
 * unit, and the bucket's count summed over the shards.  A log-linear
 * histogram visits only its non-empty buckets; a fixed-bound one visits
 * every bucket, since its bounds are the schema a dashboard keys on.
 */
/* PUBLIC: int xtc_hist_iterate __P((const xtc_hist_t *, xtc_hist_bucket_fn,
 * void *)); */
int
xtc_hist_iterate(const xtc_hist_t *h, xtc_hist_bucket_fn fn, void *user)
{
	double lower, upper;
	uint64_t c;
	int b, n = 0;

	if (h == NULL || fn == NULL)
		return XTC_E_INVAL;
	if (h->bshards != NULL) {
		for (b = 0; b <= h->n_bounds; b++) {
			lower = b == 0 ? 0.0 : h->bounds[b - 1];
			upper = b < h->n_bounds ? h->bounds[b] : HUGE_VAL;
			n++;
			if (fn(lower, upper, __bhist_bucket_total(h, b),
			        user) != 0)
				break;
		}
		return n;
	}
	for (b = 0; b < XTC_HIST_BUCKETS; b++) {
		if ((c = __hist_bucket_total(h, b)) == 0)
			continue;
		lower =
		    b == 0 ? 0.0 : (double)(__hist_bucket_lower_bound(b) - 1);
		upper = b == XTC_HIST_BUCKETS - 1 ?
		    HUGE_VAL :
		    (double)__hist_bucket_upper_bound(b);
		n++;
		if (fn(lower, upper, c, user) != 0)
			break;
	}
	return n;
}

/* PUBLIC: int xtc_hist_set_scale __P((xtc_hist_t *, double)); */
int
xtc_hist_set_scale(xtc_hist_t *h, double scale)
{
	uint64_t bits;
	if (h == NULL || !(scale > 0.0 && scale < 1e300))
		return XTC_E_INVAL;
	memcpy(&bits, &scale, sizeof bits);
	xtc_atomic_store_u64(&h->scale_bits, bits, xtc_mo_relaxed);
	return XTC_OK;
}

static double
__hist_scale(const xtc_hist_t *h)
{
	uint64_t bits = xtc_atomic_load_u64(&h->scale_bits, xtc_mo_relaxed);
	double scale;
	memcpy(&scale, &bits, sizeof scale);
	return scale;
}

/* v * scale, computed as v / round(1 / scale) when scale is the
 * reciprocal of a whole number (1e-9, 1e-6, ...): one correctly rounded
 * division, so 3071 ns prints as 3.071e-06, where 3071 * 1e-9 prints
 * 3.0710000000000003e-06. */
static double
__hist_scaled(double v, double scale)
{
	double inv = 1.0 / scale, r;
	if (scale < 1.0 && inv < 9e15) {
		r = (double)(int64_t)(inv + 0.5);
		if (1.0 / r == scale)
			return v / r;
	}
	return v * scale;
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
	uint8_t lock; /* test-and-set spinlock: 0 clear, 1 set */
	uint8_t pad[XTC_CACHE_LINE - sizeof(uint64_t) - 2 * sizeof(double) -
	    sizeof(uint8_t)];
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
	while (xtc_atomic_exchange_u8(&s->lock, 1, xtc_mo_acquire))
		;
}

static void
__dist_shard_unlock(struct dist_shard *s)
{
	xtc_atomic_store_u8(&s->lock, 0, xtc_mo_release);
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
	/* memset-0 already cleared the locks; the explicit clear is kept. */
	for (i = 0; i < d->n_cpus; i++)
		xtc_atomic_store_u8(&d->shards[i].lock, 0, xtc_mo_relaxed);
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
 * Neumaier compensation is deferred unless a consumer proves drift.
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
	 * precision.  Could swap for sqrt() if libxtc ever grows a
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

/* PUBLIC: int xtc_metrics_set_help __P((const void *, const char *)); */
int
xtc_metrics_set_help(const void *metric, const char *help)
{
	struct stats_registry_entry *e;
	char *copy = NULL, *old = NULL;
	int rc;

	if (metric == NULL)
		return XTC_E_INVAL;
	if (help != NULL && (rc = __os_strdup(help, &copy)) != XTC_OK)
		return rc;
	rc = XTC_E_NOTFOUND;
	(void)__xtc_mtx_lock(&__stats_reg_lock);
	for (e = __reg_head; e != NULL; e = e->next) {
		if (e->handle == metric) {
			old = e->help;
			e->help = copy;
			copy = NULL;
			rc = XTC_OK;
			break;
		}
	}
	(void)__xtc_mtx_unlock(&__stats_reg_lock);
	if (old != NULL)
		__os_free(old);
	if (copy != NULL)
		__os_free(copy);
	return rc;
}

/*
 * A growable text buffer for the exposition.  On an allocation failure
 * it latches `nomem` and drops the rest; the caller reports XTC_E_NOMEM.
 */
struct prom_buf {
	char *p;
	size_t len, cap;
	int nomem;
};

static void __pb_printf(struct prom_buf *b, const char *fmt, ...)
    XTC_PRINTF_FMT(2, 3);

static void
__pb_printf(struct prom_buf *b, const char *fmt, ...)
{
	va_list ap;
	size_t ncap;
	char *np;
	int n;

	if (b->nomem)
		return;
	for (;;) {
		va_start(ap, fmt);
		n = vsnprintf(b->p + b->len, b->cap - b->len, fmt, ap);
		va_end(ap);
		if (n < 0) {
			b->nomem = 1;
			return;
		}
		if ((size_t)n < b->cap - b->len) {
			b->len += (size_t)n;
			return;
		}
		ncap = b->cap * 2 + (size_t)n;
		if (__os_realloc(b->p, ncap, (void **)&np) != XTC_OK) {
			b->nomem = 1;
			return;
		}
		b->p = np;
		b->cap = ncap;
	}
}

/* A sample value: the shortest form that reads back as the same
 * double, with ".0" on an integral value (as the Prometheus Python
 * client writes `le`, so vLLM-style bucket labels come out identical);
 * +Inf, -Inf and NaN as the text format spells them. */
static void
__pb_float(struct prom_buf *b, double v)
{
	char t[40];
	int prec;

	if (v != v) {
		__pb_printf(b, "NaN");
		return;
	}
	if (v > 1.7976931348623157e308) {
		__pb_printf(b, "+Inf");
		return;
	}
	if (v < -1.7976931348623157e308) {
		__pb_printf(b, "-Inf");
		return;
	}
	for (prec = 15; prec <= 17; prec++) {
		(void)snprintf(t, sizeof t, "%.*g", prec, v);
		if (strtod(t, NULL) == v)
			break;
	}
	if (strpbrk(t, ".en") == NULL)
		__pb_printf(b, "%s.0", t);
	else
		__pb_printf(b, "%s", t);
}

/* The family name: every character outside [a-zA-Z0-9_:] becomes '_',
 * and a leading digit gets a '_' in front ("rexis.requests" ->
 * "rexis_requests"). */
static void
__prom_name(char *out, size_t outsz, const char *name)
{
	size_t i = 0, o = 0;
	char c;

	if (name[0] >= '0' && name[0] <= '9' && o + 1 < outsz)
		out[o++] = '_';
	for (; name[i] != '\0' && o + 1 < outsz; i++) {
		c = name[i];
		out[o++] = ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		               (c >= '0' && c <= '9') || c == '_' || c == ':') ?
		    c :
		    '_';
	}
	if (o == 0 && o + 1 < outsz)
		out[o++] = '_';
	out[o] = '\0';
}

/* # HELP (escaped: backslash and newline) and # TYPE lines. */
static void
__prom_header(struct prom_buf *b, const char *n, const char *help,
    const char *type)
{
	const char *c;

	if (help != NULL) {
		__pb_printf(b, "# HELP %s ", n);
		for (c = help; *c != '\0'; c++) {
			if (*c == '\\')
				__pb_printf(b, "\\\\");
			else if (*c == '\n')
				__pb_printf(b, "\\n");
			else
				__pb_printf(b, "%c", *c);
		}
		__pb_printf(b, "\n");
	}
	__pb_printf(b, "# TYPE %s %s\n", n, type);
}

static void
__prom_bucket(struct prom_buf *b, const char *n, double le, uint64_t cum)
{
	__pb_printf(b, "%s_bucket{le=\"", n);
	__pb_float(b, le);
	__pb_printf(b, "\"} %llu\n", (unsigned long long)cum);
}

/*
 * A histogram family: cumulative _bucket{le=...} series ending in +Inf,
 * then _sum and _count.  A fixed-bound histogram writes every bound, so
 * the series a dashboard keys on are always present; a log-linear one
 * writes the buckets that have ever held a value (they never empty).
 * Bounds and the sum are multiplied by the histogram's scale.
 */
static void
__prom_hist(struct prom_buf *b, const char *n, const xtc_hist_t *h)
{
	uint64_t cum = 0, c;
	double scale = __hist_scale(h);
	int i;

	/* One pass, so _count is exactly the +Inf bucket even while other
	 * threads record. */
	if (h->bshards != NULL) {
		for (i = 0; i <= h->n_bounds; i++) {
			cum += __bhist_bucket_total(h, i);
			if (i < h->n_bounds)
				__prom_bucket(b, n,
				    __hist_scaled(h->bounds[i], scale), cum);
		}
	} else {
		for (i = 0; i < XTC_HIST_BUCKETS; i++) {
			if ((c = __hist_bucket_total(h, i)) == 0)
				continue;
			cum += c;
			if (i < XTC_HIST_BUCKETS - 1)
				__prom_bucket(b, n,
				    __hist_scaled((double)
				                      __hist_bucket_upper_bound(
				                          i),
				        scale),
				    cum);
		}
	}
	__prom_bucket(b, n, HUGE_VAL, cum);
	__pb_printf(b, "%s_sum ", n);
	__pb_float(b, __hist_scaled(xtc_hist_sum(h), scale));
	__pb_printf(b, "\n%s_count %llu\n", n, (unsigned long long)cum);
}

/* One registry entry as one family (two for a dist). */
static void
__prom_entry(struct prom_buf *b, const struct stats_registry_entry *e)
{
	char n[XTC_STATS_NAME_MAX + 2];
	uint64_t cnt;
	double mean;

	__prom_name(n, sizeof n, e->name);
	switch (e->kind) {
	case XTC_METRIC_COUNTER:
		__prom_header(b, n, e->help, "counter");
		__pb_printf(b, "%s %llu\n", n,
		    (unsigned long long)xtc_counter_read(
		        (const xtc_counter_t *)e->handle));
		break;
	case XTC_METRIC_GAUGE:
		__prom_header(b, n, e->help, "gauge");
		__pb_printf(b, "%s %lld\n", n,
		    (long long)xtc_gauge_read((const xtc_gauge_t *)e->handle));
		break;
	case XTC_METRIC_HIST:
		__prom_header(b, n, e->help, "histogram");
		__prom_hist(b, n, (const xtc_hist_t *)e->handle);
		break;
	case XTC_METRIC_DIST:
		/* A summary with no quantiles: mean = _sum / _count.  The
		 * standard deviation, which a summary cannot carry, is its
		 * own gauge family. */
		cnt = xtc_dist_count((const xtc_dist_t *)e->handle);
		mean = xtc_dist_mean((const xtc_dist_t *)e->handle);
		__prom_header(b, n, e->help, "summary");
		__pb_printf(b, "%s_sum ", n);
		__pb_float(b, mean * (double)cnt);
		__pb_printf(b, "\n%s_count %llu\n", n, (unsigned long long)cnt);
		if (e->help != NULL)
			__pb_printf(b,
			    "# HELP %s_stddev standard deviation of "
			    "%s\n",
			    n, n);
		__pb_printf(b, "# TYPE %s_stddev gauge\n%s_stddev ", n, n);
		__pb_float(b, xtc_dist_stddev((const xtc_dist_t *)e->handle));
		__pb_printf(b, "\n");
		break;
	}
}

/*
 * Render every registered metric in the Prometheus text format (0.0.4)
 * into one buffer under the registry lock, then hand the page to fn
 * with no lock held, so fn may block or park.
 */
/* PUBLIC: int xtc_metrics_write_prometheus __P((xtc_metrics_write_fn, void *));
 */
int
xtc_metrics_write_prometheus(xtc_metrics_write_fn fn, void *user)
{
	struct stats_registry_entry *e;
	struct prom_buf b;
	int n = 0, rc;

	if (fn == NULL)
		return XTC_E_INVAL;
	memset(&b, 0, sizeof b);
	b.cap = 4096;
	if ((rc = __os_malloc(b.cap, (void **)&b.p)) != XTC_OK)
		return rc;
	b.p[0] = '\0';
	(void)__xtc_mtx_lock(&__stats_reg_lock);
	for (e = __reg_head; e != NULL && !b.nomem; e = e->next) {
		__prom_entry(&b, e);
		n++;
	}
	(void)__xtc_mtx_unlock(&__stats_reg_lock);
	if (b.nomem)
		rc = XTC_E_NOMEM;
	else if (fn(b.p, b.len, user) != 0)
		rc = XTC_E_IO;
	else
		rc = n;
	__os_free(b.p);
	return rc;
}

/* Write all of buf to the fd, retrying short writes and EINTR. */
static int
__prom_fd_sink(const char *buf, size_t len, void *user)
{
	int fd = (int)(intptr_t)user;
	ssize_t w;

	while (len > 0) {
		w = write(fd, buf, len); /* XTC_BLOCKING_OK: dump path */
		if (w < 0 && errno == EINTR)
			continue;
		if (w <= 0)
			return -1;
		buf += w;
		len -= (size_t)w;
	}
	return 0;
}

int
xtc_metrics_dump_prometheus(int fd)
{
	if (fd < 0) return XTC_E_INVAL;
	return xtc_metrics_write_prometheus(__prom_fd_sink,
	    (void *)(intptr_t)fd);
}

/*
 * PUBLIC: void xtc_runtime_tuning_check __P((void));
 *
 * Public entry to the host-tuning advisor.  Thin wrapper over the
 * internal __os_tuning_check so a consumer on the xtc_exec / xtc_loop
 * bring-up path can trigger the advisory probes (and route their
 * XTC_LOG_INFO output to its installed xtc_log_default sink) without
 * going through xtc_app_start.  See xtc_stats.h.
 */
void
xtc_runtime_tuning_check(void)
{
	__os_tuning_check();
}

/* PUBLIC: int xtc_runtime_tuning_check_ex __P((xtc_tuning_fn, void *)); */
int
xtc_runtime_tuning_check_ex(xtc_tuning_fn fn, void *user)
{
	return __os_tuning_check_ex(fn, user);
}
