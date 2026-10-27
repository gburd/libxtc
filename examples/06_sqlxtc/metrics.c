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

#include <stdlib.h>
#include <stdatomic.h>
#include <stdio.h>

#include "xtc_blocking.h"
#include "xtc_log.h"
#include "xtc_proc.h"
#include "xtc_res.h"
#include "xtc_stats.h"
#include "sql_parse.h"
#include "vexec.h"
#include "engine.h"
#include "xstore.h"

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

static _Atomic int g_metrics_stop, g_metrics_alive;
static xtc_pid_t   g_metrics_pid;

/*
 * Write the log ring to its sink.  Nothing else drains it while the
 * server runs (main drains once before and once after xtc_app_run), so
 * the metrics line, the loop stall watchdog's WARN and every other
 * record stayed invisible until exit, and the ring (4096 records) then
 * dropped the oldest.  The sink is a write(2), so it runs on the
 * blocking pool, never on loop 0.
 */
static int
metrics_drain_log(void *arg)
{
	(void)arg;
	return xtc_log_drain(xtc_log_default());
}

/* At-exit hook: free the state and clear the alive flag on every exit
 * path (a killed metrics proc left the flag set, so metrics_stop
 * polled 2 s for it). */
static void
metrics_dead(void *arg)
{
	free(arg);
	atomic_store(&g_metrics_alive, 0);
}

static void
metrics_proc(void *arg)
{
	metrics_state_t *st = arg;
	void *msg; size_t msg_len;
	int drained;
	if (xtc_proc_at_exit(metrics_dead, st) != XTC_OK) {
		metrics_dead(st);
		return;
	}
	while (!atomic_load(&g_metrics_stop)) {
		(void)xtc_recv(&msg, &msg_len, METRICS_INTERVAL_NS);
		if (msg) xtc_free(msg);
		if (atomic_load(&g_metrics_stop))
			break;

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
			int64_t qmax = sqlxtc_stat_query_latency ?
			    xtc_hist_max(sqlxtc_stat_query_latency) : 0;
			int64_t mem = st->res ?
			    xtc_res_used(st->res, XTC_RES_MEM_BYTES) : 0;
			uint64_t wc = 0, wf = 0;
			sx_storage_wal_counts(&wc, &wf);
			XTC_LOG_INFO_F(
			    "metrics: conns=%d queries=%lld errors=%lld "
			    "q_p50_ns=%lld q_p99_ns=%lld q_max_ns=%lld mem=%lld "
			    "parses=%llu "
			    "plans=%llu wal_commits=%llu wal_fsyncs=%llu "
			    "row_waits=%llu row_wait_ms=%llu row_conflicts=%llu "
			    "row_deadlocks=%llu",
			    atomic_load(st->conn_count),
			    (long long)qs, (long long)errs,
			    (long long)p50, (long long)p99, (long long)qmax,
			    (long long)mem, sql_parse_count(),
			    (unsigned long long)vx_plan_build_count(),
			    (unsigned long long)wc, (unsigned long long)wf,
			    (unsigned long long)xstore_intent_waits(),
			    (unsigned long long)(xstore_intent_wait_ns() / 1000000u),
			    (unsigned long long)xstore_intent_conflicts(),
			    (unsigned long long)xstore_intent_deadlocks());
		}
		/* Not queued (pool unavailable): the next tick retries; never
		 * write on the loop. */
		(void)xtc_blocking_run(metrics_drain_log, NULL, &drained);
	}
}

/* Stop the metrics proc and, on a proc, wait until it has exited, so no
 * log drain is still running on the blocking pool when the caller goes
 * on to stop the loops and free their fibers.  Idempotent. */
void
metrics_stop(void)
{
	int i;
	if (!atomic_load(&g_metrics_alive))
		return;
	atomic_store(&g_metrics_stop, 1);
	(void)xtc_send(g_metrics_pid, "s", 1);
	if (xtc_pid_is_none(xtc_self()))
		return;
	for (i = 0; i < 2000 && atomic_load(&g_metrics_alive); i++)
		(void)xtc_proc_sleep(1LL * 1000 * 1000);
}

int
metrics_spawn(xtc_loop_t *loop, xtc_res_t *res, _Atomic int *conn_count,
              _Atomic int64_t *queries_total, xtc_pid_t *out_pid)
{
	metrics_state_t *st;
	xtc_proc_opts_t opts = XTC_PROC_OPTS_INIT;
	int rc;

	(void)queries_total;   /* superseded by sqlxtc_stat_query_total */
	metrics_register();

	st = (metrics_state_t *)calloc(1, sizeof *st);
	if (!st) return XTC_E_NOMEM;
	st->res = res;
	st->conn_count = conn_count;
	opts.name = "sqlxtc-metrics";
	atomic_store(&g_metrics_stop, 0);
	atomic_store(&g_metrics_alive, 1);
	if ((rc = xtc_proc_spawn(loop, metrics_proc, st, &opts,
	    &g_metrics_pid)) != XTC_OK) {
		atomic_store(&g_metrics_alive, 0);
		free(st);
		return rc;
	}
	if (out_pid != NULL)
		*out_pid = g_metrics_pid;
	return XTC_OK;
}
