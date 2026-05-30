/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * examples/07_kaka/metrics.h -- broker observability and memory budget.
 *
 *	Observability is built on xtc_stats: per-request counters and
 *	latency histograms the partition proc updates on every PRODUCE
 *	and FETCH.  The memory budget is an xtc_res cap on stored record
 *	payload; PRODUCE that would exceed it is rejected rather than
 *	letting an unbounded producer grow the broker without limit --
 *	the bounded-resources property the runtime is built to provide.
 */

#ifndef KAKA_METRICS_H
#define KAKA_METRICS_H

#include <stddef.h>
#include <stdint.h>

/* Create the counters/histograms and the budget.  Idempotent. */
void kaka_metrics_init(void);

/* Set the broker memory budget (bytes of stored record payload).
 * 0 = unbounded.  Safe to call at startup before serving. */
void kaka_metrics_set_mem_cap(int64_t bytes);

/* Release the whole budget back to zero (test helper). */
void kaka_metrics_mem_reset(void);

/* Budget hooks used by the partition proc.  kaka_mem_acquire returns 0
 * on success, -1 if the cap would be exceeded. */
int  kaka_mem_acquire(int64_t bytes);
void kaka_mem_release(int64_t bytes);

/* Record one handled request. */
void kaka_metrics_produce(uint32_t records, size_t bytes, int64_t lat_ns,
    uint32_t rejected);
void kaka_metrics_fetch(uint32_t records, size_t bytes, int64_t lat_ns);

/* Snapshot for the metrics line / tests. */
typedef struct kaka_metrics {
	uint64_t produce_requests;
	uint64_t produce_records;
	uint64_t produce_bytes;
	uint64_t produce_rejected;
	uint64_t fetch_requests;
	uint64_t fetch_records;
	uint64_t fetch_bytes;
	double   produce_p50_us;
	double   produce_p99_us;
	double   fetch_p50_us;
	double   fetch_p99_us;
	int64_t  mem_used;
	int64_t  mem_cap;
} kaka_metrics_t;

void kaka_metrics_get(kaka_metrics_t *out);

#endif /* KAKA_METRICS_H */
