/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * examples/05_redis/expire.c
 *	Timer-driven key expiration proc.
 */

#include <stdio.h>

#include "db.h"
#include "xtc_proc.h"
#include "xtc_log.h"
#include "xtc_int.h"

/* Local helper: xtc __os_clock_mono uses out-param style. */
static inline int64_t xtc_now_ns(void) {
	int64_t t; (void)__os_clock_mono(&t); return t;
}

#define EXPIRE_INTERVAL_NS  (100 * 1000 * 1000)  /* 100 ms */
#define EXPIRE_SCAN_LIMIT   100                  /* keys per scan */

typedef struct expire_state {
	db_t *db;
} expire_state_t;

static void
expire_proc(void *arg)
{
	expire_state_t *st = arg;
	void *msg;
	size_t msg_len;

	for (;;) {
		int64_t now = xtc_now_ns();
		int removed;

		/* Yield for interval */
		(void)xtc_recv(&msg, &msg_len, EXPIRE_INTERVAL_NS);
		if (msg)
			__os_free(msg);

		/* Scan and expire */
		db_write_begin(st->db);
		removed = db_expire_stale(st->db, now, EXPIRE_SCAN_LIMIT);
		db_write_end(st->db);

		if (removed > 0) {
			XTC_LOG_DEBUG_F("expire: removed %d keys", removed);
		}
	}
}

int
expire_spawn(xtc_loop_t *loop, db_t *db, xtc_pid_t *out_pid)
{
	expire_state_t *st;
	xtc_proc_opts_t opts = { 0 };

	if (__os_malloc(sizeof(*st), (void **)&st) != XTC_OK || !st)
		return XTC_E_NOMEM;

	st->db = db;
	opts.name = "redis-expire";

	return xtc_proc_spawn(loop, expire_proc, st, &opts, out_pid);
}
