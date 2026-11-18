/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * docs/_includes/snippets/23_recv_join.c -- wait for two messages that
 * belong together: an A and a B with the same key from the same sender,
 * in whichever order they arrive, or the DOWN of a sender that will not
 * finish.
 *
 * join_nested() is the obvious code, and it is wrong: it commits to the
 * first A it sees, so an A whose B never comes blocks a complete pair
 * queued behind it, and when it gives up it has consumed that A.
 * join_pair() is the correct form: one selective receive whose predicate
 * remembers the parts it has been shown and accepts the first message
 * that completes a pair, then a masked zero-timeout take of the partner.
 * main() runs both on the same mailbox and checks every outcome.
 */

/* !region full */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"

#define MS (1000LL * 1000)

/* One part of a pair.  A predicate sees only the payload, so the sender
 * rides in it.  kind is never 'D', the first byte of a DOWN. */
struct part {
	uint8_t   kind;   /* 'A' or 'B' */
	xtc_pid_t from;
	uint64_t  key;
};

/* 1 iff (data, size) is a part; copies it to *p. */
static int
part_get(const void *data, size_t size, struct part *p)
{
	if (size != sizeof *p)
		return 0;
	memcpy(p, data, sizeof *p);
	return p->kind == 'A' || p->kind == 'B';
}

/* The part *user names exactly: kind, sender and key. */
static int
match_part(const void *data, size_t size, void *user)
{
	const struct part *want = user;
	struct part p;

	return part_get(data, size, &p) && p.kind == want->kind &&
	    xtc_pid_eq(p.from, want->from) && p.key == want->key;
}

/* !region wrong */
/* Any A. */
static int
match_any_a(const void *data, size_t size, void *user)
{
	struct part p;

	(void)user;
	return part_get(data, size, &p) && p.kind == 'A';
}

/* WRONG: take the first A, then wait for the B that goes with it. */
static int
join_nested(int64_t timeout_ns, struct part *a, struct part *b)
{
	struct part want;
	void *m;
	size_t n;
	int rc;

	rc = xtc_recv_match(match_any_a, NULL, &m, &n, timeout_ns);
	if (rc != XTC_OK)
		return rc;
	memcpy(a, m, sizeof *a);
	xtc_free(m);
	want = *a;
	want.kind = 'B';
	/* The A is consumed.  If its B never comes this times out holding
	 * it, and a kill delivered at this receive unwinds holding it. */
	rc = xtc_recv_match(match_part, &want, &m, &n, timeout_ns);
	if (rc != XTC_OK)
		return rc;
	memcpy(b, m, sizeof *b);
	xtc_free(m);
	return XTC_OK;
}
/* !endregion wrong */

/* !region join */
/*
 * The partial matches one receive call has seen.  The joiner runs with
 * mailbox_cap SEEN_MAX: its mailbox and save queue together never hold
 * more than SEEN_MAX ordinary messages, so the table cannot fill.  (The
 * cap also means partial matches left queued can block the senders.)
 */
#define SEEN_MAX 64

struct join {
	uint64_t    ref;              /* a DOWN with this ref ends the wait */
	int         n_seen;
	struct part seen[SEEN_MAX];   /* parts shown so far, oldest first */
};

/*
 * One call of xtc_recv_match shows its predicate the save queue and then
 * each new arrival, in arrival order.  Remember every part; accept the
 * first one that completes a pair with a part remembered earlier, or the
 * DOWN of j->ref.  When a part is accepted, its partner is in the save
 * queue.
 */
static int
match_pair(const void *data, size_t size, void *user)
{
	struct join *j = user;
	xtc_down_info_t di = XTC_DOWN_INFO_INIT;
	struct part p;
	int i, dup = 0;

	if (size > 0 && *(const uint8_t *)data == 'D' &&
	    xtc_down_decode_ex(data, size, &di) == XTC_OK)
		return di.ref == j->ref;
	if (!part_get(data, size, &p))
		return 0;
	for (i = 0; i < j->n_seen; i++) {
		if (!xtc_pid_eq(j->seen[i].from, p.from) ||
		    j->seen[i].key != p.key)
			continue;
		if (j->seen[i].kind != p.kind)
			return 1;   /* p completes a pair */
		dup = 1;        /* a second A (or B) waits for a later join */
	}
	if (!dup && j->n_seen < SEEN_MAX)
		j->seen[j->n_seen++] = p;
	return 0;
}

/*
 * Wait up to timeout_ns for an A and a B with the same key from the same
 * sender.  Returns:
 *   XTC_OK          both parts consumed, in *a and *b;
 *   XTC_E_NOTFOUND  the DOWN of j->ref came first: it is consumed, no
 *                   part is;
 *   XTC_E_AGAIN     the time ran out: nothing was consumed;
 *   XTC_E_NOMEM     copying the partner out failed: the first part is in
 *                   *a or *b and the partner is still queued.
 */
static int
join_pair(struct join *j, int64_t timeout_ns, struct part *a, struct part *b)
{
	struct part first, want;
	void *m;
	size_t n;
	int rc;

	j->n_seen = 0;
	rc = xtc_recv_match(match_pair, j, &m, &n, timeout_ns);
	if (rc != XTC_OK)
		return rc;
	if (!part_get(m, n, &first)) {
		xtc_free(m);   /* the DOWN */
		return XTC_E_NOTFOUND;
	}
	xtc_free(m);
	*(first.kind == 'A' ? a : b) = first;
	want = first;
	want.kind = first.kind == 'A' ? 'B' : 'A';
	/*
	 * Every receive delivers a pending kill at entry.  Masked, a kill
	 * that lands now waits for xtc_mask_leave, which unwinds with both
	 * parts in *a and *b: cleanup on the unwind sees the whole pair or
	 * none of it.  The partner is in the save queue, so a 0 timeout
	 * finds it without parking.
	 */
	(void)xtc_mask_enter();
	rc = xtc_recv_match(match_part, &want, &m, &n, 0);
	if (rc == XTC_OK) {
		memcpy(first.kind == 'A' ? b : a, m, sizeof *a);
		xtc_free(m);
	}
	(void)xtc_mask_leave();
	return rc;
}
/* !endregion join */

/* ---- the run ------------------------------------------------------- */

static xtc_pid_t g_joiner, g_s1, g_s2;
static int g_fail;

static void
expect(int ok, const char *what)
{
	if (!ok) {
		printf("FAIL: %s\n", what);
		g_fail = 1;
	}
}

static void
send_part(xtc_pid_t to, char kind, uint64_t key)
{
	struct part p;

	memset(&p, 0, sizeof p);
	p.kind = (uint8_t)kind;
	p.from = xtc_self();
	p.key = key;
	(void)xtc_send(to, &p, sizeof p);
}

/* s1 sends an A and never its B; s2 sends an A, a message of another
 * kind, and its B; s3 sends an A and exits. */
static void
s1(void *arg)
{
	(void)arg;
	send_part(g_joiner, 'A', 1);
}

static void
s2(void *arg)
{
	(void)arg;
	send_part(g_joiner, 'A', 2);
	send_part(g_joiner, 'N', 0);
	send_part(g_joiner, 'B', 2);
}

static void
s3(void *arg)
{
	(void)arg;
	send_part(g_joiner, 'A', 3);
}

/* Messages in the calling proc's mailbox and save queue. */
static size_t
queued(void)
{
	xtc_mailbox_stats_t st = XTC_MAILBOX_STATS_INIT;

	(void)xtc_proc_mailbox_stats(xtc_self(), &st);
	return st.depth + st.saved;
}

/*
 * A proc that queues itself an A and waits, without a deadline, for the
 * B; it is killed in that wait.  Its at-exit hook counts what is still
 * queued: what a hook that hands pending mail on would see.
 */
static size_t g_left_at_exit;

static void
victim_exit(void *arg)
{
	(void)arg;
	g_left_at_exit = queued();
}

static void
victim(void *arg)
{
	struct join j;
	struct part a, b;

	(void)xtc_proc_at_exit(victim_exit, NULL);
	send_part(xtc_self(), 'A', 4);
	j.ref = 0;
	if (arg != NULL)
		(void)join_nested(-1, &a, &b);
	else
		(void)join_pair(&j, -1, &a, &b);
}

/* Run victim (nested or not), kill it in its wait, and wait for it. */
static size_t
kill_in_wait(xtc_loop_t *loop, int nested)
{
	xtc_pid_t v;
	uint64_t ref;
	void *m;
	size_t n;

	g_left_at_exit = 99;
	(void)xtc_proc_spawn_monitor(loop, victim, nested ? loop : NULL, NULL,
	    &v, &ref);
	(void)xtc_proc_sleep(5 * MS);
	(void)xtc_exit_pid(v, 1);
	(void)xtc_recv(&m, &n, -1);   /* its DOWN: nothing else is queued */
	xtc_free(m);
	return g_left_at_exit;
}

/* Empty the mailbox, then fill it with [A(s1) A(s2) N B(s2)]. */
static void
refill(xtc_loop_t *loop)
{
	void *m;
	size_t n;

	while (xtc_recv(&m, &n, 0) == XTC_OK)
		xtc_free(m);
	(void)xtc_proc_spawn(loop, s1, NULL, NULL, &g_s1);
	(void)xtc_proc_spawn(loop, s2, NULL, NULL, &g_s2);
	(void)xtc_proc_sleep(10 * MS);   /* both run and exit */
}

static void
joiner(void *arg)
{
	xtc_loop_t *loop = arg;
	struct join j;
	struct part a, b;
	xtc_pid_t s3_pid;
	void *m;
	size_t n, left;
	int rc;

	refill(loop);
	rc = join_nested(20 * MS, &a, &b);
	printf("nested: %s holding the A of %s, %zu left queued\n",
	    xtc_strerror(rc), xtc_pid_eq(a.from, g_s1) ? "s1" : "?",
	    queued());
	expect(rc == XTC_E_AGAIN && xtc_pid_eq(a.from, g_s1) &&
	    queued() == 3, "nested: times out holding s1's A");

	refill(loop);
	j.ref = 0;   /* monitor refs are never 0: no DOWN ends this one */
	rc = join_pair(&j, 20 * MS, &a, &b);
	printf("join:   %s, the pair of %s, %zu left queued\n",
	    xtc_strerror(rc), xtc_pid_eq(a.from, g_s2) ? "s2" : "?",
	    queued());
	expect(rc == XTC_OK && xtc_pid_eq(a.from, g_s2) &&
	    xtc_pid_eq(b.from, g_s2) && a.key == 2 && b.key == 2 &&
	    queued() == 2, "join: s2's pair, A(s1) and N left");

	rc = join_pair(&j, 5 * MS, &a, &b);
	printf("join:   %s, %zu left queued\n", xtc_strerror(rc), queued());
	expect(rc == XTC_E_AGAIN && queued() == 2,
	    "join: a timeout takes nothing");

	(void)xtc_proc_spawn_monitor(loop, s3, NULL, NULL, &s3_pid, &j.ref);
	rc = join_pair(&j, -1, &a, &b);
	printf("join:   %s after s3's DOWN, %zu left queued\n",
	    xtc_strerror(rc), queued());
	expect(rc == XTC_E_NOTFOUND && queued() == 3,
	    "join: the DOWN ends an unbounded wait");

	while (xtc_recv(&m, &n, 0) == XTC_OK)
		xtc_free(m);
	left = kill_in_wait(loop, 1);
	printf("nested: killed waiting for its B, %zu left queued\n", left);
	expect(left == 0, "nested: a kill takes the A with it");
	left = kill_in_wait(loop, 0);
	printf("join:   killed waiting for its B, %zu left queued\n", left);
	expect(left == 1, "join: a kill leaves the A queued");
}

int
main(void)
{
	xtc_proc_opts_t o = XTC_PROC_OPTS_INIT;
	xtc_loop_t *loop;

	o.mailbox_cap = SEEN_MAX;
	if (xtc_loop_create(&loop) != XTC_OK ||
	    xtc_proc_spawn(loop, joiner, loop, &o, &g_joiner) != XTC_OK)
		return 1;
	(void)xtc_loop_run(loop);
	xtc_loop_destroy(loop);
	return g_fail;
}
/* !endregion full */
