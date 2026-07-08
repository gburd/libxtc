/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * docs/_includes/snippets/03_ping_pong.c -- two Erlang-style processes
 * exchanging messages.  ping sends a counter to pong; pong replies with
 * counter + 1; they bounce it a few times and stop.
 *
 * Shows xtc_proc_spawn / xtc_self / xtc_send / xtc_recv, and the rule
 * that a message received from xtc_recv is caller-owned memory freed
 * with xtc_free (NOT plain free -- libxtc may use its own allocator).
 */

/* !region full */
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"

#define ROUNDS 4

/* Messages are plain bytes.  xtc_recv gives no sender, so we carry the
 * reply address in the payload -- the usual libxtc idiom. */
struct msg {
	xtc_pid_t from;
	int       n;
};

static void
pong(void *arg)
{
	void  *raw;
	size_t sz;

	(void)arg;
	for (;;) {
		struct msg req, reply;

		if (xtc_recv(&raw, &sz, 1000LL * 1000 * 1000) != XTC_OK)
			return;                 /* timed out: no partner left */
		if (sz != sizeof req) {
			xtc_free(raw);
			continue;
		}
		memcpy(&req, raw, sizeof req);
		xtc_free(raw);              /* received buffers are ours to free */

		if (req.n >= ROUNDS) {
			printf("pong: reached %d, done\n", req.n);
			return;
		}
		reply.from = xtc_self();
		reply.n    = req.n + 1;
		(void)xtc_send(req.from, &reply, sizeof reply);
	}
}

static void
ping(void *arg)
{
	xtc_pid_t peer = *(xtc_pid_t *)arg;
	struct msg first = { xtc_self(), 0 };
	void  *raw;
	size_t sz;

	(void)xtc_send(peer, &first, sizeof first);
	for (;;) {
		struct msg r;

		if (xtc_recv(&raw, &sz, 1000LL * 1000 * 1000) != XTC_OK)
			return;
		memcpy(&r, raw, sizeof r);
		xtc_free(raw);
		printf("ping: got %d\n", r.n);
		if (r.n >= ROUNDS)
			return;
		r.from = xtc_self();
		(void)xtc_send(peer, &r, sizeof r);
	}
}

int
main(void)
{
	xtc_loop_t *loop;
	xtc_pid_t   pong_pid;

	if (xtc_loop_init(&loop) != XTC_OK)
		return 1;
	if (xtc_proc_spawn(loop, pong, NULL, NULL, &pong_pid) != XTC_OK)
		return 1;
	if (xtc_proc_spawn(loop, ping, &pong_pid, NULL, NULL) != XTC_OK)
		return 1;
	(void)xtc_loop_run(loop);
	(void)xtc_loop_fini(loop);
	return 0;
}
/* !endregion full */
