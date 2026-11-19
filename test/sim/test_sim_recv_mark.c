/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/sim/test_sim_recv_mark.c
 *	Deterministic Simulation Testing of the receive mark (2.3):
 *	xtc_recv_mark, then a request, then xtc_recv_match_since for its
 *	reply, across loops with the simulator's seeded cross-loop latency
 *	on, while noise senders on other loops keep the caller's save queue
 *	and mailbox full of messages no receive here matches.
 *
 *	Each caller runs ROUNDS rounds: send itself a sentinel, mark, send a
 *	request tagged with the round number to a server on another loop,
 *	optionally sleep a seeded time (so the reply can arrive between the
 *	mark and the receive), optionally take a noise message (a take that
 *	may move the mark), then wait for the reply with the mark and a
 *	predicate that matches this round's tag.  On a quarter of the rounds
 *	it marks a second time before the receive, so the receive's mark is
 *	stale.  The server echoes each request after a seeded delay; on some
 *	rounds it also sends a stray copy of an earlier round's reply.
 *
 *	Invariants (per seed):
 *	  (a) every round's reply is received: a reply that arrived after
 *	      the mark, before or during the receive, is found;
 *	  (b) a current mark skips what arrived before it: the predicate is
 *	      never shown a sentinel (sent to self, so queued at once, just
 *	      before the mark), except under a stale mark, which walks
 *	      everything;
 *	  (c) nothing is lost: after the rounds, every sentinel, noise
 *	      message and stray reply (and the reply of a round that missed
 *	      it) is still in the mailbox;
 *	  (d) the run quiesces;
 *	  (e) REPLAY: the same seed gives the same per-round trace and sim
 *	      state hash.
 *
 *	Planted bug (src/inc/xtc_dst_inject.h) this test must catch:
 *	  76 RMARK_LATE  the marked receive starts after what has arrived by
 *	                 the time of the receive, so a reply that arrived
 *	                 between the mark and the receive is skipped.
 *
 *	argv: [base [count]] -- seeds base+1 .. base+count (default 0 24).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "sim_int.h"

#define N_LOOPS 3
#define N_CALLERS 2
#define ROUNDS 24
#define MS (1000LL * 1000)

/* 'Q' request, 'P' reply, 'N' noise, 'S' sentinel. */
struct rmsg {
	uint8_t kind;
	xtc_pid_t from;
	int caller;
	uint64_t round;
};

struct caller {
	xtc_exec_t *exec;
	int id;
	xtc_pid_t server;
	int got;         /* rounds whose own reply came back */
	int wrong;       /* a reply for another round taken */
	int missed;      /* a round that timed out */
	int early_seen;  /* a sentinel shown under a current mark */
	int noise_taken; /* noise messages the rounds took */
	int left_noise;  /* noise messages queued at the end */
	int left_stray;  /* stray replies queued at the end */
	int left_sent;   /* sentinels queued at the end */
	int stale;       /* 1 while the round's mark is stale */
	uint64_t trace;
};

static struct caller g_c[N_CALLERS];
static int g_noise_sent[N_CALLERS];
static int g_stray_sent[N_CALLERS];
static int g_done;

static void
send_msg(xtc_pid_t to, uint8_t kind, int caller, uint64_t round)
{
	struct rmsg m;

	memset(&m, 0, sizeof m);
	m.kind = kind;
	m.from = xtc_self();
	m.caller = caller;
	m.round = round;
	(void)xtc_send(to, &m, sizeof m);
}

/* The reply to `*u`'s round; counts a sentinel shown under a current
 * mark, which arrived before it. */
struct want {
	struct caller *c;
	uint64_t round;
};

static int
match_reply(const void *data, size_t size, void *u)
{
	const struct want *w = u;
	struct rmsg m;

	if (size != sizeof m)
		return 0;
	memcpy(&m, data, sizeof m);
	if (m.kind == 'S' && !w->c->stale)
		w->c->early_seen++;
	return m.kind == 'P' && m.caller == w->c->id && m.round == w->round;
}

static int
match_noise(const void *data, size_t size, void *u)
{
	(void)u;
	return size == sizeof(struct rmsg) &&
	    ((const struct rmsg *)data)->kind == 'N';
}

/* Echo each request as a reply after a seeded delay; on a third of them
 * send a stray copy of the previous round's reply first. */
static void
server(void *arg)
{
	struct rmsg m;
	void *raw;
	size_t sz;
	int64_t d;

	(void)arg;
	while (xtc_recv(&raw, &sz, 50 * MS) == XTC_OK) {
		if (sz != sizeof m) {
			xtc_free(raw);
			continue;
		}
		memcpy(&m, raw, sizeof m);
		xtc_free(raw);
		d = (int64_t)__xtc_sim_rng_range(XTC_SIM_RNG_APP, 3) * MS / 2;
		if (d > 0)
			(void)xtc_proc_sleep(d);
		if (m.round > 0 &&
		    __xtc_sim_rng_range(XTC_SIM_RNG_APP, 3) == 0) {
			send_msg(m.from, 'P', m.caller, m.round - 1);
			g_stray_sent[m.caller]++;
		}
		send_msg(m.from, 'P', m.caller, m.round);
	}
}

/* Send `n` noise messages to `*arg`'s caller, spaced by seeded gaps. */
struct noise_arg {
	xtc_pid_t to;
	int caller;
	int n;
};

static void
noise(void *arg)
{
	struct noise_arg *na = arg;
	int i;

	for (i = 0; i < na->n; i++) {
		send_msg(na->to, 'N', na->caller, 0);
		g_noise_sent[na->caller]++;
		(void)xtc_proc_sleep(
		    (int64_t)__xtc_sim_rng_range(XTC_SIM_RNG_APP, 4) * MS / 4);
	}
}

/* Count what is left: noise, and stray replies of earlier rounds. */
static void
count_left(struct caller *c)
{
	struct rmsg m;
	void *raw;
	size_t sz;

	while (xtc_recv(&raw, &sz, 0) == XTC_OK) {
		if (sz == sizeof m) {
			memcpy(&m, raw, sizeof m);
			if (m.kind == 'N')
				c->left_noise++;
			else if (m.kind == 'P')
				c->left_stray++;
			else if (m.kind == 'S')
				c->left_sent++;
		}
		xtc_free(raw);
	}
}

static void
caller(void *arg)
{
	struct caller *c = arg;
	struct noise_arg na;
	struct want want;
	struct rmsg got;
	xtc_loop_t *nl = xtc_exec_loop(c->exec, (c->id + 2) % N_LOOPS);
	uint64_t mark, m2, round;
	void *raw;
	size_t sz;
	int64_t gap;
	int r, rc, extra;

	na.to = xtc_self();
	na.caller = c->id;
	na.n = 16 + (int)__xtc_sim_rng_range(XTC_SIM_RNG_APP, 32);
	(void)xtc_proc_spawn(nl, noise, &na, NULL, NULL);
	(void)xtc_proc_sleep(MS); /* some noise is queued before round 0 */
	for (r = 0; r < ROUNDS; r++) {
		round = (uint64_t)r;
		gap = (int64_t)__xtc_sim_rng_range(XTC_SIM_RNG_APP, 4) * MS / 2;
		extra = (int)__xtc_sim_rng_range(XTC_SIM_RNG_APP, 4);
		send_msg(xtc_self(), 'S', c->id, round);
		(void)xtc_recv_mark(&mark);
		c->stale = 0;
		send_msg(c->server, 'Q', c->id, round);
		if (gap > 0)
			(void)xtc_proc_sleep(gap); /* the reply may land now */
		if (extra == 1 &&
		    xtc_recv_match(match_noise, NULL, &raw, &sz, 0) == XTC_OK) {
			xtc_free(raw);
			c->noise_taken++;
		}
		if (extra == 2) {
			(void)xtc_recv_mark(&m2); /* `mark` is stale now */
			c->stale = 1;
		}
		want.c = c;
		want.round = round;
		rc = xtc_recv_match_since(mark, match_reply, &want, &raw, &sz,
		    20 * MS);
		if (rc != XTC_OK) {
			c->missed++;
		} else {
			memcpy(&got, raw, sizeof got);
			xtc_free(raw);
			if (got.kind == 'P' && got.round == round)
				c->got++;
			else
				c->wrong++;
		}
		c->stale = 0;
		c->trace = c->trace * 1000003u + (uint64_t)rc * 8u +
		    (uint64_t)extra * 2u + (uint64_t)(gap / MS);
	}
	(void)xtc_proc_sleep(60 * MS); /* the noise and the server finish */
	count_left(c);
	xtc_atomic_fetch_add_i32(&g_done, 1, xtc_mo_seq_cst);
}

static int
run_one(uint64_t seed, uint64_t *out_trace, uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	int i, rc;
	uint64_t tr = 0;

	memset(g_c, 0, sizeof g_c);
	memset(g_noise_sent, 0, sizeof g_noise_sent);
	memset(g_stray_sent, 0, sizeof g_stray_sent);
	xtc_atomic_store_i32(&g_done, 0, xtc_mo_seq_cst);
	if (xtc_exec_create(&(xtc_exec_opts_t) { .size =
	                                             sizeof(xtc_exec_opts_t),
	                        .n_loops = N_LOOPS },
	        &e) != XTC_OK)
		return -1;
	xtc_sim_net_latency(10 * 1000LL, 800 * 1000LL);
	for (i = 0; i < N_CALLERS; i++) {
		g_c[i].exec = e;
		g_c[i].id = i;
		(void)xtc_proc_spawn(xtc_exec_loop(e, (i + 1) % N_LOOPS),
		    server, NULL, NULL, &g_c[i].server);
		(void)xtc_proc_spawn(xtc_exec_loop(e, i % N_LOOPS), caller,
		    &g_c[i], NULL, NULL);
	}
	rc = xtc_sim_exec_run(e, seed, 20000000);
	xtc_sim_net_latency(0, 0);
	for (i = 0; i < N_CALLERS; i++)
		tr = tr * 31u + g_c[i].trace;
	*out_trace = tr;
	*out_state = xtc_sim_state_hash(e);
	xtc_exec_destroy(e);
	return rc;
}

/* 1 iff seed k's run met (a)-(d); prints what failed. */
static int
check_one(uint64_t k, int rc, int *rounds_ok)
{
	struct caller *c;
	int i, pass = 1;

	for (i = 0; i < N_CALLERS; i++) {
		c = &g_c[i];
		*rounds_ok += c->got;
		if (c->missed > 0 || c->wrong > 0 || c->got != ROUNDS) {
			printf("  seed %llu: a marked receive skipped a reply "
			       "(caller %d: got %d/%d, missed %d, wrong %d)\n",
			    (unsigned long long)k, i, c->got, ROUNDS, c->missed,
			    c->wrong);
			pass = 0;
		}
		if (c->early_seen > 0) {
			printf("  seed %llu: a marked receive tested a message "
			       "from before the mark (caller %d, %d times)\n",
			    (unsigned long long)k, i, c->early_seen);
			pass = 0;
		}
		if (c->left_noise + c->noise_taken != g_noise_sent[i] ||
		    c->left_stray != g_stray_sent[i] + c->missed ||
		    c->left_sent != ROUNDS) {
			printf("  seed %llu: caller %d lost messages (noise "
			       "%d+%d of %d, reply %d of %d, sentinel %d of "
			       "%d)\n",
			    (unsigned long long)k, i, c->left_noise,
			    c->noise_taken, g_noise_sent[i], c->left_stray,
			    g_stray_sent[i] + c->missed, c->left_sent, ROUNDS);
			pass = 0;
		}
	}
	if (rc != XTC_OK ||
	    xtc_atomic_load_i32(&g_done, xtc_mo_seq_cst) != N_CALLERS) {
		printf("  seed %llu: rc=%d done=%d/%d\n", (unsigned long long)k,
		    rc, xtc_atomic_load_i32(&g_done, xtc_mo_seq_cst),
		    N_CALLERS);
		pass = 0;
	}
	return pass;
}

int
main(int argc, char **argv)
{
	uint64_t base = 0, seed, tr, tr2, st, st2;
	int n = 24, k, fails = 0, rc, rc2, rounds = 0;

	if (argc > 1)
		base = strtoull(argv[1], NULL, 0);
	if (argc > 2)
		n = atoi(argv[2]);
	printf("== recv mark DST: %d seeds from base %llu ==\n", n,
	    (unsigned long long)base);
	for (k = 1; k <= n; k++) {
		seed = 0x9E3779B97F4A7C15ull * (base + (uint64_t)k);
		rc = run_one(seed, &tr, &st);
		if (!check_one(base + (uint64_t)k, rc, &rounds)) {
			fails++;
			continue;
		}
		rc2 = run_one(seed, &tr2, &st2);
		if (rc2 != rc || tr2 != tr || st2 != st) {
			printf("  seed %llu: replay diverged\n",
			    (unsigned long long)(base + (uint64_t)k));
			fails++;
		}
	}
	if (fails != 0) {
		printf("FAIL: %d/%d recv mark seeds failed\n", fails, n);
		return 1;
	}
	printf("OK: recv mark DST -- %d seeds, %d marked request-reply rounds, "
	       "cross-loop latency and noise on, nothing skipped or lost, all "
	       "replay\n",
	    n, rounds);
	return 0;
}
