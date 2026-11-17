/*-
 * Copyright (c) 2026, The XTC Project
 * @stability stable
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * src/inc/xtc_stats.h
 *	Runtime statistics primitives: counters, gauges, histograms.
 *	Designed to be cheap to update on the hot path (single atomic
 *	per-CPU shard for counters; one atomic word for gauges) and
 *	non-obscuring under perf inspection.
 *
 *	A counter accumulates events.  Per-CPU sharded so that
 *	xtc_counter_inc compiles to one cache-line-local atomic add.
 *	Cross-CPU summing happens only on read, which is the slow path.
 *
 *	A gauge holds a current value (depth, queue length, etc.).
 *	Single atomic int64; reads are wait-free, writes are atomic
 *	store.  For low-frequency-update gauges this is fine.
 *
 *	A histogram tracks a value distribution and answers quantile
 *	queries (p50/p99/p999) and the exact max.  Log-linear buckets
 *	from 1 ns to 2^40 ns (~18 min), 16 per octave; a quantile is the
 *	upper bound of its bucket (HdrHistogram's convention), so it is
 *	never below the true value and at most 1/16 above it.  Per-CPU
 *	shards so concurrent record() calls don't contend; quantile
 *	queries merge the shards with relaxed loads, no lock.
 *
 *	A dist is a mergeable online mean/variance aggregator.
 *	Welford's single-pass update runs on the current CPU's shard;
 *	reads merge the per-CPU shards with Chan's parallel formula.
 *	Tracks mean and population variance only (no skew/kurtosis).
 *
 *	A registry walks every metric for periodic dumps to logs or
 *	a Prometheus-style scrape endpoint.
 */

#ifndef XTC_STATS_H
#define XTC_STATS_H

#include "xtc_export.h"

#include <stddef.h>
#include <stdint.h>

#include "xtc.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct xtc_counter xtc_counter_t;
typedef struct xtc_gauge   xtc_gauge_t;
typedef struct xtc_hist    xtc_hist_t;
typedef struct xtc_dist    xtc_dist_t;

typedef enum xtc_metric_kind {
	XTC_METRIC_COUNTER = 0,
	XTC_METRIC_GAUGE   = 1,
	XTC_METRIC_HIST    = 2,
	XTC_METRIC_DIST    = 3
} xtc_metric_kind_t;

typedef int (*xtc_metric_visit_fn)(const char *name,
                                    xtc_metric_kind_t kind,
                                    const void *handle,
                                    void *user);

/*
 * One finding of the host-tuning advisor (xtc_tuning_check_ex).  The
 * library hands it out, so it carries the library's size: check
 * XTC_FIELD_OK(f, field) before reading a field newer than the oldest
 * library you support.  Every string is valid only during the callback.
 *
 *   probe        a stable name to alert on: "governor", "intel_pstate",
 *                "thp", "swappiness", "autogroup", "io_uring"
 *   current      the value the probe read ("powersave", "always", "60",
 *                "EPERM", ...)
 *   recommended  the value the advisor recommends ("performance",
 *                "madvise", "10", ...)
 *   advice       one English sentence: the line xtc_tuning_check logs,
 *                without its "[tuning] " prefix
 */
typedef struct xtc_tuning_finding {
	size_t size; /* the LIBRARY's sizeof: see XTC_FIELD_OK */
	const char *probe;
	const char *current;
	const char *recommended;
	const char *advice;
} xtc_tuning_finding_t;

/* xtc_tuning_check_ex's callback: nonzero stops the check. */
typedef int (*xtc_tuning_fn)(const xtc_tuning_finding_t *f, void *user);

/*
 * PUBLIC: int       xtc_counter_create __P((const char *, xtc_counter_t **));
 * PUBLIC: void      xtc_counter_destroy __P((xtc_counter_t *));
 * PUBLIC: void      xtc_counter_inc __P((xtc_counter_t *));
 * PUBLIC: void      xtc_counter_add __P((xtc_counter_t *, int64_t));
 * PUBLIC: uint64_t  xtc_counter_read __P((const xtc_counter_t *));
 *
 * PUBLIC: int       xtc_gauge_create __P((const char *, xtc_gauge_t **));
 * PUBLIC: void      xtc_gauge_destroy __P((xtc_gauge_t *));
 * PUBLIC: void      xtc_gauge_set __P((xtc_gauge_t *, int64_t));
 * PUBLIC: void      xtc_gauge_add __P((xtc_gauge_t *, int64_t));
 * PUBLIC: int64_t   xtc_gauge_read __P((const xtc_gauge_t *));
 *
 * PUBLIC: int       xtc_hist_create __P((const char *, xtc_hist_t **));
 * PUBLIC: int       xtc_hist_create_bounds __P((const char *, const double *, int, xtc_hist_t **));
 * PUBLIC: void      xtc_hist_destroy __P((xtc_hist_t *));
 * PUBLIC: void      xtc_hist_record __P((xtc_hist_t *, int64_t));
 * PUBLIC: void      xtc_hist_record_value __P((xtc_hist_t *, double));
 * PUBLIC: int64_t   xtc_hist_quantile __P((const xtc_hist_t *, double));
 * PUBLIC: int       xtc_hist_quantiles __P((const xtc_hist_t *, const double *, int, int64_t *, uint64_t *));
 * PUBLIC: uint64_t  xtc_hist_count __P((const xtc_hist_t *));
 * PUBLIC: int64_t   xtc_hist_max __P((const xtc_hist_t *));
 * PUBLIC: double    xtc_hist_sum __P((const xtc_hist_t *));
 * PUBLIC: int       xtc_hist_iterate __P((const xtc_hist_t *, xtc_hist_bucket_fn, void *));
 * PUBLIC: int       xtc_hist_set_scale __P((xtc_hist_t *, double));
 *
 * PUBLIC: int       xtc_dist_create __P((const char *, xtc_dist_t **));
 * PUBLIC: void      xtc_dist_destroy __P((xtc_dist_t *));
 * PUBLIC: void      xtc_dist_record __P((xtc_dist_t *, double));
 * PUBLIC: uint64_t  xtc_dist_count __P((const xtc_dist_t *));
 * PUBLIC: double    xtc_dist_mean __P((const xtc_dist_t *));
 * PUBLIC: double    xtc_dist_variance __P((const xtc_dist_t *));
 * PUBLIC: double    xtc_dist_stddev __P((const xtc_dist_t *));
 *
 * PUBLIC: int       xtc_metrics_iterate __P((xtc_metric_visit_fn, void *));
 * PUBLIC: int       xtc_metrics_dump_prometheus __P((int));
 * PUBLIC: int       xtc_metrics_write_prometheus __P((xtc_metrics_write_fn, void *));
 * PUBLIC: int       xtc_metrics_set_help __P((const void *, const char *));
 *
 * PUBLIC: void      xtc_tuning_check __P((void));
 * PUBLIC: int       xtc_tuning_check_ex __P((xtc_tuning_fn, void *));
 */

XTC_API int       xtc_counter_create(const char *name, xtc_counter_t **out);
XTC_API void      xtc_counter_destroy(xtc_counter_t *c);
XTC_API void      xtc_counter_inc(xtc_counter_t *c);
XTC_API void      xtc_counter_add(xtc_counter_t *c, int64_t delta);
XTC_API uint64_t  xtc_counter_read(const xtc_counter_t *c);

XTC_API int       xtc_gauge_create(const char *name, xtc_gauge_t **out);
XTC_API void      xtc_gauge_destroy(xtc_gauge_t *g);
XTC_API void      xtc_gauge_set(xtc_gauge_t *g, int64_t v);
XTC_API void      xtc_gauge_add(xtc_gauge_t *g, int64_t delta);
XTC_API int64_t   xtc_gauge_read(const xtc_gauge_t *g);

XTC_API int       xtc_hist_create(const char *name, xtc_hist_t **out);
XTC_API void      xtc_hist_destroy(xtc_hist_t *h);
XTC_API void      xtc_hist_record(xtc_hist_t *h, int64_t value_ns);
XTC_API int64_t   xtc_hist_quantile(const xtc_hist_t *h, double q);
XTC_API uint64_t  xtc_hist_count(const xtc_hist_t *h);
XTC_API int64_t   xtc_hist_max(const xtc_hist_t *h);

/*
 * A histogram with the caller's bucket upper bounds instead of the
 * log-linear ones: n_bounds (1..64) finite values, >= 0 and strictly
 * ascending, copied at creation; a +Inf bucket is implicit.  A value v
 * counts in the first bucket whose bound is >= v (Prometheus's `le`),
 * so the exposition carries exactly these `le` labels and a dashboard
 * written against them (vLLM's TTFT bounds, say) works unchanged.
 * Values are unit-free doubles: record them with xtc_hist_record_value
 * (xtc_hist_record converts its integer).  It keeps counts and a sum
 * only: xtc_hist_quantile, _quantiles and _max answer 0.
 * XTC_E_INVAL for a NULL argument or bad bounds.
 */
XTC_API int       xtc_hist_create_bounds(const char *name,
                      const double *upper_bounds, int n_bounds,
                      xtc_hist_t **out);

/* Record a unit-free value: into a fixed-bound histogram as is, into a
 * log-linear one rounded to an integer.  Negative and NaN count as 0. */
XTC_API void      xtc_hist_record_value(xtc_hist_t *h, double v);

/*
 * Several quantiles, and the count, from one merge of the shards:
 * xtc_hist_quantile merges every shard on each call, so a reader that
 * wants p50, p99 and the count pays one merge here instead of three.
 * out[i] answers qs[i] (n >= 0; out_count may be NULL).  XTC_OK, or
 * XTC_E_INVAL for a NULL h, or NULL qs/out with n > 0.
 */
XTC_API int       xtc_hist_quantiles(const xtc_hist_t *h, const double *qs,
                      int n, int64_t *out, uint64_t *out_count);

/* The sum of every recorded value (a negative xtc_hist_record value
 * counts as 0, as in its bucket).  A log-linear sum is exact below
 * 2^53 and wraps at 2^64, which a scrape reads as a counter reset. */
XTC_API double    xtc_hist_sum(const xtc_hist_t *h);

/*
 * Visit the buckets lowest first: fn(lower, upper, count, user) for the
 * bucket (lower, upper] (the first also holds 0; upper is HUGE_VAL for
 * the last one), count merged over the shards and not cumulative.  A
 * log-linear histogram visits only its non-empty buckets, a fixed-bound
 * one every bucket.
 * Bounds are in the recorded unit (the scale does not apply).  Returns
 * the number of buckets visited (a nonzero fn return stops after that
 * one), or XTC_E_INVAL.
 */
typedef int (*xtc_hist_bucket_fn)(double lower, double upper,
                                  uint64_t count, void *user);
XTC_API int       xtc_hist_iterate(const xtc_hist_t *h, xtc_hist_bucket_fn fn,
                      void *user);

/*
 * The factor the Prometheus exposition multiplies this histogram's
 * bucket bounds and sum by (default 1.0): 1e-9 exposes a nanosecond
 * histogram in seconds, the Prometheus base unit (name it *_seconds).
 * The recorded values and every other accessor are unaffected.
 * XTC_E_INVAL for a NULL h or a scale that is not a positive number.
 */
XTC_API int       xtc_hist_set_scale(xtc_hist_t *h, double scale);

XTC_API int       xtc_dist_create(const char *name, xtc_dist_t **out);
XTC_API void      xtc_dist_destroy(xtc_dist_t *d);
XTC_API void      xtc_dist_record(xtc_dist_t *d, double value);
XTC_API uint64_t  xtc_dist_count(const xtc_dist_t *d);
XTC_API double    xtc_dist_mean(const xtc_dist_t *d);
XTC_API double    xtc_dist_variance(const xtc_dist_t *d);
XTC_API double    xtc_dist_stddev(const xtc_dist_t *d);

XTC_API int       xtc_metrics_iterate(xtc_metric_visit_fn fn, void *user);

/*
 * The registry in the Prometheus text exposition format (0.0.4), which
 * `promtool check metrics` accepts.  Each metric is one family named
 * after it with every character outside [a-zA-Z0-9_:] made '_'
 * ("rexis.requests" -> rexis_requests), led by its help text if it has
 * one (xtc_metrics_set_help):
 *   counter    # TYPE counter, one sample
 *   gauge      # TYPE gauge, one sample
 *   hist       # TYPE histogram: cumulative _bucket{le="..."} through
 *              le="+Inf", _sum, _count.  A fixed-bound histogram writes
 *              every bound; a log-linear one the upper bound of each
 *              bucket that ever held a value.  Bounds and _sum are
 *              multiplied by xtc_hist_set_scale's factor.
 *   dist       # TYPE summary with _sum and _count (mean = _sum /
 *              _count) and no quantiles, then a <name>_stddev gauge.
 * The counter suffix (_total) and the unit (_seconds) are the
 * registered name's job.  Through 2.1 a hist was _count/_p50/_p99/
 * _p999/_max and a dist _count/_mean/_variance/_stddev, which Prometheus
 * rejects; read quantiles with xtc_hist_quantiles instead.
 *
 * xtc_metrics_write_prometheus renders the whole page into one buffer
 * and calls fn(buf, len, user) once, with no lock held (fn may block or
 * park); xtc_metrics_dump_prometheus writes it to fd.  Both return the
 * number of metrics written, or XTC_E_INVAL, XTC_E_NOMEM, or XTC_E_IO
 * (fn returned nonzero, or a write(2) failed).
 */
typedef int (*xtc_metrics_write_fn)(const char *buf, size_t len, void *user);
XTC_API int       xtc_metrics_write_prometheus(xtc_metrics_write_fn fn,
                      void *user);
XTC_API int       xtc_metrics_dump_prometheus(int fd);

/*
 * Attach help text (copied) to a registered counter, gauge, hist or dist
 * handle: the # HELP line of its family.  NULL removes it.  XTC_OK,
 * XTC_E_NOTFOUND if the handle is not registered, XTC_E_INVAL, or
 * XTC_E_NOMEM.
 */
XTC_API int       xtc_metrics_set_help(const void *metric, const char *help);

/*
 * Run libxtc's host-tuning advisor: a battery of cheap, read-only
 * probes (CPU governor, intel_pstate, transparent hugepages, vm
 * swappiness, sched autogroup, io_uring-under-seccomp) that each log
 * one XTC_LOG_INFO line to the xtc_log_default() sink when the host is
 * NOT in the recommended state for a low-latency runtime.  Silent on a
 * well-tuned host; Linux-only (a no-op elsewhere); never writes /proc
 * or /sys.
 *
 * This is the intended way to get those lines into an application's log
 * sink on the xtc_exec / xtc_loop bring-up path (xtc_app_start already
 * calls it by default; a consumer that stands up carriers via
 * xtc_exec_create should call xtc_tuning_check() once at startup, after
 * installing its xtc_log_set_default sink, to receive the advisories).
 * Safe to call more than once.  With no default logger installed the
 * lines go nowhere: an application that wants the findings without a
 * process-wide logger uses xtc_tuning_check_ex.
 */
XTC_API void      xtc_tuning_check(void);

/*
 * The same probes, with each finding handed to fn(f, user) on the
 * calling thread instead of logged; no global state is read or written,
 * so concurrent callers and other threads' logging do not interfere.
 * Returns the number of findings (0 on a well-tuned host and on every
 * non-Linux host), counting one fn stopped at.  fn may be NULL to only
 * count.  The probes read /proc and /sys: call it off a loop thread (the
 * blocking pool) in a server.
 */
XTC_API int xtc_tuning_check_ex(xtc_tuning_fn fn, void *user);

#ifdef __cplusplus
}
#endif

#endif /* XTC_STATS_H */
