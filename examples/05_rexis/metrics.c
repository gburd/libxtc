/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * examples/05_rexis/metrics.c
 *
 *	Periodic stats reporter.  Owns the rexis-side xtc_stats
 *	objects and dumps them on an interval to stderr in
 *	Prometheus text-exposition format.  cmd.c records command
 *	counters and a latency histogram against these objects.
 */

#include <stdio.h>
#include <stdatomic.h>
#include <unistd.h>

#include "db.h"
#include "xtc_proc.h"
#include "xtc_res.h"
#include "xtc_log.h"
#include "xtc_int.h"
#include "xtc_stats.h"

#define METRICS_INTERVAL_NS  (5000LL * 1000 * 1000)  /* 5 seconds */

xtc_counter_t *rexis_stat_cmd_total      = NULL;
xtc_counter_t *rexis_stat_unknown_cmd    = NULL;
xtc_hist_t    *rexis_stat_cmd_latency    = NULL;
xtc_gauge_t   *rexis_stat_db_keys        = NULL;
xtc_gauge_t   *rexis_stat_db_mem         = NULL;
xtc_gauge_t   *rexis_stat_connections    = NULL;
xtc_gauge_t   *rexis_stat_res_mem        = NULL;

static void
metrics_register(void)
{
	(void)xtc_counter_create("rexis_cmd_total",        &rexis_stat_cmd_total);
	(void)xtc_counter_create("rexis_cmd_unknown_total",&rexis_stat_unknown_cmd);
	(void)xtc_hist_create   ("rexis_cmd_latency_ns",   &rexis_stat_cmd_latency);
	(void)xtc_gauge_create  ("rexis_db_keys",          &rexis_stat_db_keys);
	(void)xtc_gauge_create  ("rexis_db_memory_bytes",  &rexis_stat_db_mem);
	(void)xtc_gauge_create  ("rexis_connections",     &rexis_stat_connections);
	(void)xtc_gauge_create  ("rexis_res_memory_bytes", &rexis_stat_res_mem);
}

typedef struct metrics_state {
	db_t        *db;
	xtc_res_t   *res;
	_Atomic int *conn_count;
} metrics_state_t;

static void
metrics_proc(void *arg)
{
	metrics_state_t *st = arg;
	void *msg;
	size_t msg_len;

	for (;;) {
		(void)xtc_recv(&msg, &msg_len, METRICS_INTERVAL_NS);
		if (msg)
			__os_free(msg);

		/* Update gauges from authoritative state. */
		if (rexis_stat_db_keys != NULL)
			xtc_gauge_set(rexis_stat_db_keys,
			    (int64_t)db_key_count(st->db));
		if (rexis_stat_db_mem != NULL)
			xtc_gauge_set(rexis_stat_db_mem,
			    (int64_t)db_mem_used(st->db));
		if (rexis_stat_connections != NULL)
			xtc_gauge_set(rexis_stat_connections,
			    (int64_t)atomic_load(st->conn_count));
		if (st->res != NULL && rexis_stat_res_mem != NULL)
			xtc_gauge_set(rexis_stat_res_mem,
			    xtc_res_used(st->res, XTC_RES_MEM_BYTES));

		/* Log a one-line summary plus the current p50 / p99
		 * for command latency.  Operators who want full
		 * Prometheus exposition can wire xtc_metrics_dump_prometheus
		 * to a /metrics HTTP endpoint or a periodic file. */
		{
			int64_t p50 = 0, p99 = 0;
			if (rexis_stat_cmd_latency != NULL) {
				p50 = xtc_hist_quantile(rexis_stat_cmd_latency, 0.50);
				p99 = xtc_hist_quantile(rexis_stat_cmd_latency, 0.99);
			}
			XTC_LOG_INFO_F(
			    "metrics: keys=%zu db_mem=%zu conns=%d "
			    "cmd_p50_ns=%lld cmd_p99_ns=%lld",
			    db_key_count(st->db),
			    db_mem_used(st->db),
			    atomic_load(st->conn_count),
			    (long long)p50, (long long)p99);
		}
	}
}

int
metrics_spawn(xtc_loop_t *loop, db_t *db, xtc_res_t *res,
              _Atomic int *conn_count, xtc_pid_t *out_pid)
{
	metrics_state_t *st;
	xtc_proc_opts_t opts = { 0 };

	metrics_register();

	if (__os_malloc(sizeof(*st), (void **)&st) != XTC_OK || !st)
		return XTC_E_NOMEM;

	st->db = db;
	st->res = res;
	st->conn_count = conn_count;
	opts.name = "rexis-metrics";

	return xtc_proc_spawn(loop, metrics_proc, st, &opts, out_pid);
}

/*
 * Dump all registered metrics in Prometheus text-exposition format
 * to the given fd.  cmd.c calls this from the INFO command path so
 * a client can scrape the server through normal RESP.
 */
int
metrics_dump_prometheus(int fd)
{
	return xtc_metrics_dump_prometheus(fd);
}
