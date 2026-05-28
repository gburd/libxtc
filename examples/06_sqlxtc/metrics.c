/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/metrics.c
 *	Periodic xtc_res snapshot logger.
 */

#include <stdatomic.h>
#include <stdio.h>

#include "xtc_int.h"
#include "xtc_log.h"
#include "xtc_proc.h"
#include "xtc_res.h"

#define METRICS_INTERVAL_NS  (5LL * 1000 * 1000 * 1000)

typedef struct metrics_state {
	xtc_res_t   *res;
	_Atomic int *conn_count;
	_Atomic int64_t *queries_total;
} metrics_state_t;

static void
metrics_proc(void *arg)
{
	metrics_state_t *st = arg;
	void *msg; size_t msg_len;
	for (;;) {
		(void)xtc_recv(&msg, &msg_len, METRICS_INTERVAL_NS);
		if (msg) __os_free(msg);

		{
			int conns = atomic_load(st->conn_count);
			int64_t qs = atomic_load(st->queries_total);
			int64_t mem = xtc_res_used(st->res, XTC_RES_MEM_BYTES);
			int64_t high = xtc_res_high(st->res, XTC_RES_MEM_BYTES);
			int64_t rej = xtc_res_rejects(st->res, XTC_RES_MEM_BYTES);
			XTC_LOG_INFO_F(
			    "metrics: conns=%d queries=%lld mem=%lld high=%lld rejects=%lld",
			    conns, (long long)qs, (long long)mem,
			    (long long)high, (long long)rej);
		}
	}
}

int
metrics_spawn(xtc_loop_t *loop, xtc_res_t *res, _Atomic int *conn_count,
              _Atomic int64_t *queries_total, xtc_pid_t *out_pid)
{
	metrics_state_t *st;
	xtc_proc_opts_t opts = { 0 };
	st = (metrics_state_t *)calloc(1, sizeof *st);
	if (!st) return XTC_E_NOMEM;
	st->res = res;
	st->conn_count = conn_count;
	st->queries_total = queries_total;
	opts.name = "sqlxtc-metrics";
	return xtc_proc_spawn(loop, metrics_proc, st, &opts, out_pid);
}
