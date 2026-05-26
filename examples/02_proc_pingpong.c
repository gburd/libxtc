/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the PostgreSQL License.
 *
 * examples/02_proc_pingpong.c — Erlang-style processes: ping sends
 * a counter to pong, pong replies with counter+1; bounce 100 times.
 *
 * Demonstrates xtc_proc_spawn / xtc_send / xtc_recv with sender-pid
 * encoded in the message payload (xtc_recv doesn't surface a sender
 * directly; user-space encodes it).
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"

#define ROUNDS 100

struct rpc_msg {
	xtc_pid_t from;
	int       n;
};

static void
pong(void *arg)
{
	void  *m;
	size_t sz;
	(void)arg;
	for (;;) {
		struct rpc_msg req;
		struct rpc_msg reply;
		if (xtc_recv(&m, &sz, 1000LL * 1000 * 1000) != XTC_OK) return;
		if (sz != sizeof req) { if (m) free(m); continue; }
		memcpy(&req, m, sizeof req);
		free(m);
		if (req.n >= ROUNDS) {
			printf("pong: reached %d, exiting\n", req.n);
			return;
		}
		reply.from = xtc_self();
		reply.n    = req.n + 1;
		(void)xtc_send(req.from, &reply, sizeof reply);
	}
}

struct ping_state { xtc_pid_t pong; };

static void
ping(void *arg)
{
	struct ping_state *st = arg;
	int n = 0;
	void  *m;
	size_t sz;
	while (n < ROUNDS) {
		struct rpc_msg req = { .from = xtc_self(), .n = n };
		if (xtc_send(st->pong, &req, sizeof req) != XTC_OK) break;
		if (xtc_recv(&m, &sz, 1000LL * 1000 * 1000) != XTC_OK) break;
		if (sz != sizeof req) { if (m) free(m); break; }
		{
			struct rpc_msg reply;
			memcpy(&reply, m, sizeof reply);
			free(m);
			n = reply.n;
		}
	}
	printf("ping: completed %d rounds\n", n);
}

int
main(void)
{
	xtc_loop_t *loop;
	struct ping_state st;
	xtc_pid_t pong_pid, ping_pid;

	if (xtc_loop_init(&loop) != XTC_OK) return 1;
	if (xtc_proc_spawn(loop, pong, NULL, NULL, &pong_pid) != XTC_OK) return 1;
	st.pong = pong_pid;
	if (xtc_proc_spawn(loop, ping, &st, NULL, &ping_pid) != XTC_OK) return 1;
	if (xtc_loop_run(loop) != XTC_OK) return 1;
	(void)xtc_loop_fini(loop);
	return 0;
}
