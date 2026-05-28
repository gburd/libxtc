/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * src/inc/xtc_stats.h
 *	Runtime statistics primitives: counters, gauges, histograms.
 *	Designed to be cheap to update on the hot path (single atomic
 *	per-CPU shard for counters; one _Atomic for gauges) and
 *	non-obscuring under perf inspection.
 *
 *	A counter accumulates events.  Per-CPU sharded so that
 *	xtc_counter_inc compiles to one cache-line-local atomic add.
 *	Cross-CPU summing happens only on read, which is the slow path.
 *
 *	A gauge holds a current value (depth, queue length, etc.).
 *	Single _Atomic int64; reads are wait-free, writes are atomic
 *	store.  For low-frequency-update gauges this is fine.
 *
 *	A histogram tracks a value distribution and answers quantile
 *	queries (p50/p99/p999).  The current implementation wraps the
 *	HDR-style hist used by the conformance bench, with per-CPU
 *	shards so concurrent record() calls don't contend.  Quantile
 *	queries merge shards under a read lock.
 *
 *	A registry walks every metric for periodic dumps to logs or
 *	a Prometheus-style scrape endpoint.
 */

#ifndef XTC_STATS_H
#define XTC_STATS_H

#include <stddef.h>
#include <stdint.h>

#include "xtc.h"

typedef struct xtc_counter xtc_counter_t;
typedef struct xtc_gauge   xtc_gauge_t;
typedef struct xtc_hist    xtc_hist_t;

typedef enum xtc_metric_kind {
	XTC_METRIC_COUNTER = 0,
	XTC_METRIC_GAUGE   = 1,
	XTC_METRIC_HIST    = 2
} xtc_metric_kind_t;

typedef int (*xtc_metric_visit_fn)(const char *name,
                                    xtc_metric_kind_t kind,
                                    const void *handle,
                                    void *user);

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
 * PUBLIC: void      xtc_hist_destroy __P((xtc_hist_t *));
 * PUBLIC: void      xtc_hist_record __P((xtc_hist_t *, int64_t));
 * PUBLIC: int64_t   xtc_hist_quantile __P((const xtc_hist_t *, double));
 * PUBLIC: uint64_t  xtc_hist_count __P((const xtc_hist_t *));
 *
 * PUBLIC: int       xtc_metrics_iterate __P((xtc_metric_visit_fn, void *));
 * PUBLIC: int       xtc_metrics_dump_prometheus __P((int));
 */

int       xtc_counter_create(const char *name, xtc_counter_t **out);
void      xtc_counter_destroy(xtc_counter_t *c);
void      xtc_counter_inc(xtc_counter_t *c);
void      xtc_counter_add(xtc_counter_t *c, int64_t delta);
uint64_t  xtc_counter_read(const xtc_counter_t *c);

int       xtc_gauge_create(const char *name, xtc_gauge_t **out);
void      xtc_gauge_destroy(xtc_gauge_t *g);
void      xtc_gauge_set(xtc_gauge_t *g, int64_t v);
void      xtc_gauge_add(xtc_gauge_t *g, int64_t delta);
int64_t   xtc_gauge_read(const xtc_gauge_t *g);

int       xtc_hist_create(const char *name, xtc_hist_t **out);
void      xtc_hist_destroy(xtc_hist_t *h);
void      xtc_hist_record(xtc_hist_t *h, int64_t value_ns);
int64_t   xtc_hist_quantile(const xtc_hist_t *h, double q);
uint64_t  xtc_hist_count(const xtc_hist_t *h);

int       xtc_metrics_iterate(xtc_metric_visit_fn fn, void *user);
int       xtc_metrics_dump_prometheus(int fd);

#endif /* XTC_STATS_H */
