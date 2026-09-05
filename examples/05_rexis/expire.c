/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * examples/05_rexis/expire.c
 *	Timer-driven key expiration proc.
 */

#include <stdio.h>

#include "db.h"
#include "xtc_proc.h"
#include "xtc_log.h"

/* Local helper wrapping xtc_clock_mono(). */
static inline int64_t xtc_now_ns(void) {
	int64_t t; t = xtc_clock_mono(); return t;
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
			xtc_free(msg);

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

	if ((st = xtc_malloc(sizeof(*st))) == NULL)
		return XTC_E_NOMEM;

	st->db = db;
	opts.name = "rexis-expire";

	return xtc_proc_spawn(loop, expire_proc, st, &opts, out_pid);
}
