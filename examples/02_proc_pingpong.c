/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * examples/02_proc_pingpong.c -- Erlang-style processes: ping sends
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
		int rc;
		if (xtc_recv(&m, &sz, 1000LL * 1000 * 1000) != XTC_OK) return;
		/* xtc_free, NOT free: the buffer xtc_recv hands us comes from
		 * libxtc's allocator, which an embedder can replace
		 * (xtc_alloc_set_hook).  Releasing it with the C library's
		 * free() is a mismatched free -- heap corruption under any
		 * custom allocator.  Same rule for every buffer the docs say
		 * to "free with xtc_free". */
		if (sz != sizeof req) { xtc_free(m); continue; }
		memcpy(&req, m, sizeof req);
		xtc_free(m);
		if (req.n >= ROUNDS) {
			printf("pong: reached %d, exiting\n", req.n);
			return;
		}
		reply.from = xtc_self();
		reply.n    = req.n + 1;
		/* Always check xtc_send: a full mailbox returns XTC_E_AGAIN,
		 * and dropping that return silently loses the reply -- the
		 * peer then waits forever for an answer that was discarded.
		 * Backpressure is the caller's to handle, not the library's. */
		rc = xtc_send(req.from, &reply, sizeof reply);
		if (rc != XTC_OK) {
			fprintf(stderr, "pong: reply to round %d dropped: %s\n",
			    req.n + 1, xtc_strerror(rc));
			return;
		}
	}
}

static int g_ping_rounds;   /* observed outcome, checked in main */

struct ping_state { xtc_pid_t pong; };

static void
ping(void *arg)
{
	struct ping_state *st = arg;
	int n = 0;
	void  *m;
	size_t sz;
	struct rpc_msg reply;
	while (n < ROUNDS) {
		struct rpc_msg req = { .from = xtc_self(), .n = n };
		if (xtc_send(st->pong, &req, sizeof req) != XTC_OK) break;
		if (xtc_recv(&m, &sz, 1000LL * 1000 * 1000) != XTC_OK) break;
		if (sz != sizeof req) { xtc_free(m); break; }
		memcpy(&reply, m, sizeof reply);
		xtc_free(m);   /* see the note in pong() */
		n = reply.n;
	}
	printf("ping: completed %d rounds\n", n);
	/* Report the outcome so a failed run is visible to the caller (and
	 * to CI) instead of looking like success. */
	g_ping_rounds = n;
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

	/* Assert the observable outcome.  Without this the program prints
	 * "completed 0 rounds" and still exits 0, so CI (and a reader) could
	 * not tell a working build from a broken one. */
	if (g_ping_rounds < ROUNDS) {
		fprintf(stderr, "FAIL: only %d of %d rounds completed\n",
		    g_ping_rounds, ROUNDS);
		return 1;
	}
	return 0;
}
