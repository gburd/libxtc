/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * examples/06_sqlxtc/metrics.c
 *
 *	Periodic stats reporter.  Owns the sqlxtc-side xtc_stats
 *	objects; conn.c records query counts and latency against
 *	them.  Dumps a p50/p99 summary on an interval.
 */

#include <stdatomic.h>
#include <stdio.h>

#include "xtc_int.h"
#include "xtc_log.h"
#include "xtc_proc.h"
#include "xtc_res.h"
#include "xtc_stats.h"
#include "vfs.h"
#include "pcache.h"

#define METRICS_INTERVAL_NS  (5LL * 1000 * 1000 * 1000)

xtc_counter_t *sqlxtc_stat_query_total   = NULL;
xtc_counter_t *sqlxtc_stat_query_errors  = NULL;
xtc_hist_t    *sqlxtc_stat_query_latency = NULL;
xtc_gauge_t   *sqlxtc_stat_connections   = NULL;
xtc_gauge_t   *sqlxtc_stat_res_mem       = NULL;

static void
metrics_register(void)
{
	(void)xtc_counter_create("sqlxtc_query_total",      &sqlxtc_stat_query_total);
	(void)xtc_counter_create("sqlxtc_query_errors_total",&sqlxtc_stat_query_errors);
	(void)xtc_hist_create   ("sqlxtc_query_latency_ns", &sqlxtc_stat_query_latency);
	(void)xtc_gauge_create  ("sqlxtc_connections",      &sqlxtc_stat_connections);
	(void)xtc_gauge_create  ("sqlxtc_res_memory_bytes", &sqlxtc_stat_res_mem);
}

typedef struct metrics_state {
	xtc_res_t   *res;
	_Atomic int *conn_count;
} metrics_state_t;

static void
metrics_proc(void *arg)
{
	metrics_state_t *st = arg;
	void *msg; size_t msg_len;
	for (;;) {
		(void)xtc_recv(&msg, &msg_len, METRICS_INTERVAL_NS);
		if (msg) __os_free(msg);

		if (sqlxtc_stat_connections != NULL)
			xtc_gauge_set(sqlxtc_stat_connections,
			    (int64_t)atomic_load(st->conn_count));
		if (st->res != NULL && sqlxtc_stat_res_mem != NULL)
			xtc_gauge_set(sqlxtc_stat_res_mem,
			    xtc_res_used(st->res, XTC_RES_MEM_BYTES));

		{
			int64_t qs = sqlxtc_stat_query_total ?
			    (int64_t)xtc_counter_read(sqlxtc_stat_query_total) : 0;
			int64_t errs = sqlxtc_stat_query_errors ?
			    (int64_t)xtc_counter_read(sqlxtc_stat_query_errors) : 0;
			int64_t p50 = sqlxtc_stat_query_latency ?
			    xtc_hist_quantile(sqlxtc_stat_query_latency, 0.50) : 0;
			int64_t p99 = sqlxtc_stat_query_latency ?
			    xtc_hist_quantile(sqlxtc_stat_query_latency, 0.99) : 0;
			int64_t mem = st->res ?
			    xtc_res_used(st->res, XTC_RES_MEM_BYTES) : 0;
			vfs_stats_t vfs;
			pcache_stats_t pc;
			vfs_get_stats(&vfs);
			pcache_get_stats(&pc);
			XTC_LOG_INFO_F(
			    "metrics: conns=%d queries=%lld errors=%lld "
			    "q_p50_ns=%lld q_p99_ns=%lld mem=%lld "
			    "vfs_r=%llu vfs_w=%llu vfs_rb=%llu vfs_wb=%llu "
			    "pc_hit=%llu pc_miss=%llu pc_pages=%llu",
			    atomic_load(st->conn_count),
			    (long long)qs, (long long)errs,
			    (long long)p50, (long long)p99,
			    (long long)mem,
			    (unsigned long long)vfs.reads,
			    (unsigned long long)vfs.writes,
			    (unsigned long long)vfs.bytes_read,
			    (unsigned long long)vfs.bytes_written,
			    (unsigned long long)pc.fetch_hit,
			    (unsigned long long)pc.fetch_miss,
			    (unsigned long long)pc.live_pages);
		}
	}
}

int
metrics_spawn(xtc_loop_t *loop, xtc_res_t *res, _Atomic int *conn_count,
              _Atomic int64_t *queries_total, xtc_pid_t *out_pid)
{
	metrics_state_t *st;
	xtc_proc_opts_t opts = { 0 };

	(void)queries_total;   /* superseded by sqlxtc_stat_query_total */
	metrics_register();

	st = (metrics_state_t *)calloc(1, sizeof *st);
	if (!st) return XTC_E_NOMEM;
	st->res = res;
	st->conn_count = conn_count;
	opts.name = "sqlxtc-metrics";
	return xtc_proc_spawn(loop, metrics_proc, st, &opts, out_pid);
}
