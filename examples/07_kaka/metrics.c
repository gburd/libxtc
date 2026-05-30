/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * examples/07_kaka/metrics.c -- broker observability and memory budget.
 */

#include "metrics.h"

#include <string.h>

#include "xtc_int.h"
#include "xtc_res.h"
#include "xtc_stats.h"

static xtc_counter_t *g_produce_reqs;
static xtc_counter_t *g_produce_recs;
static xtc_counter_t *g_produce_bytes;
static xtc_counter_t *g_produce_rejected;
static xtc_counter_t *g_fetch_reqs;
static xtc_counter_t *g_fetch_recs;
static xtc_counter_t *g_fetch_bytes;
static xtc_hist_t    *g_produce_lat;
static xtc_hist_t    *g_fetch_lat;

static xtc_res_t      g_budget;
static int            g_inited;

void
kaka_metrics_init(void)
{
	xtc_res_caps_t caps = XTC_RES_CAPS_DEFAULT;

	if (g_inited)
		return;
	(void)xtc_counter_create("kaka.produce.requests", &g_produce_reqs);
	(void)xtc_counter_create("kaka.produce.records", &g_produce_recs);
	(void)xtc_counter_create("kaka.produce.bytes", &g_produce_bytes);
	(void)xtc_counter_create("kaka.produce.rejected", &g_produce_rejected);
	(void)xtc_counter_create("kaka.fetch.requests", &g_fetch_reqs);
	(void)xtc_counter_create("kaka.fetch.records", &g_fetch_recs);
	(void)xtc_counter_create("kaka.fetch.bytes", &g_fetch_bytes);
	(void)xtc_hist_create("kaka.produce.latency_ns", &g_produce_lat);
	(void)xtc_hist_create("kaka.fetch.latency_ns", &g_fetch_lat);

	/* Budget tracks stored record payload via XTC_RES_MEM_BYTES;
	 * leave the other resource caps at their generous defaults. */
	(void)xtc_res_init(&g_budget, &caps);
	g_inited = 1;
}

void
kaka_metrics_set_mem_cap(int64_t bytes)
{
	kaka_metrics_init();
	xtc_res_set_cap(&g_budget, XTC_RES_MEM_BYTES, bytes);
}

void
kaka_metrics_mem_reset(void)
{
	int64_t used;
	kaka_metrics_init();
	used = xtc_res_used(&g_budget, XTC_RES_MEM_BYTES);
	if (used > 0)
		xtc_res_release(&g_budget, XTC_RES_MEM_BYTES, used);
}

int
kaka_mem_acquire(int64_t bytes)
{
	kaka_metrics_init();
	if (bytes <= 0)
		return 0;
	return xtc_res_acquire(&g_budget, XTC_RES_MEM_BYTES, bytes) == XTC_OK
	    ? 0 : -1;
}

void
kaka_mem_release(int64_t bytes)
{
	if (bytes > 0)
		xtc_res_release(&g_budget, XTC_RES_MEM_BYTES, bytes);
}

void
kaka_metrics_produce(uint32_t records, size_t bytes, int64_t lat_ns,
    uint32_t rejected)
{
	if (!g_inited)
		return;
	xtc_counter_inc(g_produce_reqs);
	if (records) xtc_counter_add(g_produce_recs, (int64_t)records);
	if (bytes) xtc_counter_add(g_produce_bytes, (int64_t)bytes);
	if (rejected) xtc_counter_add(g_produce_rejected, (int64_t)rejected);
	if (lat_ns >= 0) xtc_hist_record(g_produce_lat, lat_ns);
}

void
kaka_metrics_fetch(uint32_t records, size_t bytes, int64_t lat_ns)
{
	if (!g_inited)
		return;
	xtc_counter_inc(g_fetch_reqs);
	if (records) xtc_counter_add(g_fetch_recs, (int64_t)records);
	if (bytes) xtc_counter_add(g_fetch_bytes, (int64_t)bytes);
	if (lat_ns >= 0) xtc_hist_record(g_fetch_lat, lat_ns);
}

void
kaka_metrics_get(kaka_metrics_t *out)
{
	if (out == NULL)
		return;
	memset(out, 0, sizeof *out);
	if (!g_inited)
		return;
	out->produce_requests = xtc_counter_read(g_produce_reqs);
	out->produce_records = xtc_counter_read(g_produce_recs);
	out->produce_bytes = xtc_counter_read(g_produce_bytes);
	out->produce_rejected = xtc_counter_read(g_produce_rejected);
	out->fetch_requests = xtc_counter_read(g_fetch_reqs);
	out->fetch_records = xtc_counter_read(g_fetch_recs);
	out->fetch_bytes = xtc_counter_read(g_fetch_bytes);
	out->produce_p50_us = xtc_hist_quantile(g_produce_lat, 0.50) / 1000.0;
	out->produce_p99_us = xtc_hist_quantile(g_produce_lat, 0.99) / 1000.0;
	out->fetch_p50_us = xtc_hist_quantile(g_fetch_lat, 0.50) / 1000.0;
	out->fetch_p99_us = xtc_hist_quantile(g_fetch_lat, 0.99) / 1000.0;
	out->mem_used = xtc_res_used(&g_budget, XTC_RES_MEM_BYTES);
	out->mem_cap = g_budget.caps.mem_bytes;
}
