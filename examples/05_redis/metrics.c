/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * examples/05_redis/metrics.c
 *	Periodic resource stats logging.
 */

#include <stdio.h>
#include <stdatomic.h>

#include "db.h"
#include "xtc_proc.h"
#include "xtc_res.h"
#include "xtc_log.h"
#include "xtc_int.h"

#define METRICS_INTERVAL_NS  (5000LL * 1000 * 1000)  /* 5 seconds */

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
		/* Yield for interval */
		(void)xtc_recv(&msg, &msg_len, METRICS_INTERVAL_NS);
		if (msg)
			__os_free(msg);

		/* Collect and log stats */
		{
			size_t keys = db_key_count(st->db);
			size_t mem = db_mem_used(st->db);
			int conns = atomic_load(st->conn_count);
			int64_t res_mem = 0, res_high = 0;

			if (st->res) {
				res_mem = xtc_res_used(st->res, XTC_RES_MEM_BYTES);
				res_high = xtc_res_high(st->res, XTC_RES_MEM_BYTES);
			}

			XTC_LOG_INFO_F(
			    "metrics: keys=%zu db_mem=%zu conns=%d "
			    "res_mem=%lld res_high=%lld",
			    keys, mem, conns,
			    (long long)res_mem, (long long)res_high);
		}
	}
}

int
metrics_spawn(xtc_loop_t *loop, db_t *db, xtc_res_t *res,
              _Atomic int *conn_count, xtc_pid_t *out_pid)
{
	metrics_state_t *st;
	xtc_proc_opts_t opts = { 0 };

	if (__os_malloc(sizeof(*st), (void **)&st) != XTC_OK || !st)
		return XTC_E_NOMEM;

	st->db = db;
	st->res = res;
	st->conn_count = conn_count;
	opts.name = "redis-metrics";

	return xtc_proc_spawn(loop, metrics_proc, st, &opts, out_pid);
}
