/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/sim/test_sim_send_wait.c
 *	Deterministic Simulation Testing of waiting for mailbox room (2.2):
 *	xtc_send_wait from procs and xtc_send_notify, against receivers
 *	with a small cap that drain at seeded speeds, half of the time with
 *	xtc_recv_match (so takes come from the selective-receive save queue
 *	as well as the mailbox), across loops.
 *
 *	Invariants (per seed):
 *	  (a) every message a sender sent with xtc_send_wait arrives exactly
 *	      once, and no xtc_send_wait fails (receivers live through the
 *	      run);
 *	  (b) no sender is parked in xtc_send_wait while its receiver has
 *	      room: a watchdog samples each sender's park start and each
 *	      receiver's depth (xtc_proc_mailbox_stats); 200 ms of virtual
 *	      time with room is a lost wakeup (planted as ROOM_SAVEQ: a
 *	      save-queue take does not wake the waiters -- "a sender waited
 *	      with room in the mailbox");
 *	  (c) every xtc_send_notify is answered by one ROOM message;
 *	  (d) the run quiesces and REPLAYS.
 *	The workload runs under the simulator's cross-loop latency, so
 *	messages are in flight while mailboxes fill and drain.  A second,
 *	smaller run per seed aims at the in-flight window itself:
 *	  (e) a send told XTC_OK arrives, even when the mailbox filled
 *	      while it was in flight (planted as SIM_DROP_LATE: "a message
 *	      sent with XTC_OK was lost"), and the room it held comes back
 *	      once it lands;
 *	  (f) a message in flight to a proc that dies, or still in flight
 *	      when a run stops at its step budget, is freed (the allocation
 *	      auditor sees no growth over the run).
 *
 *	argv: [base [count]] -- seeds base+1 .. base+count (default 0 16).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xtc.h"
#include "xtc_alloc_audit.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "sim_int.h"

#define N_LOOPS 3
#define N_RX    2
#define N_TX    4             /* senders per receiver */
#define N_MSG   24            /* messages per sender */
#define CAP     4
#define MS      (1000LL * 1000)

struct rx {
	xtc_pid_t pid;
	int       selective;      /* drain odd ones first: save queue */
	int64_t   pace;           /* virtual ns per message */
	unsigned char seen[N_TX][N_MSG];
	int       got, dup;
};

struct tx {
	struct rx *rx;
	int        id;
	int        fails;
	int64_t    park_since;    /* virtual time an xtc_send_wait began */
	int        in_wait;
};

static struct rx g_rx[N_RX];
static struct tx g_tx[N_RX][N_TX];
static struct tx g_nt[N_RX];  /* one xtc_send_notify user per receiver */
static int g_notified, g_notes, g_stuck, g_tx_done, g_sent;
static uint64_t g_trace;

static int
match_odd(const void *d, size_t sz, void *u)
{
	(void)u;
	return sz == sizeof(int) && (*(const int *)d & 1) == 1;
}

static void
receiver(void *arg)
{
	struct rx *r = arg;
	void *m;
	size_t sz;
	int v;
	/* A seeded pace, drawn inside the run (the run seeds the RNG). */
	r->pace = (int64_t)__xtc_sim_rng_range(XTC_SIM_RNG_APP, 4) * MS;
	while (r->got < N_TX * N_MSG + 1) {
		/* Selective: an odd value if one is queued now (from the save
		 * queue when an even one is ahead of it), else the next. */
		if ((!r->selective ||
		        xtc_recv_match(match_odd, NULL, &m, &sz, 0) !=
		            XTC_OK) &&
		    xtc_recv(&m, &sz, -1) != XTC_OK)
			continue;
		if (sz == sizeof(int)) {
			memcpy(&v, m, sizeof v);
			if (v >= 0 && v / N_MSG < N_TX &&
			    r->seen[v / N_MSG][v % N_MSG]++ != 0)
				r->dup++;
			g_trace = g_trace * 1000003u + (uint64_t)v;
		}
		free(m);
		r->got++;
		if (r->pace > 0)
			(void)xtc_proc_sleep(r->pace);
	}
}

static void
sender(void *arg)
{
	struct tx *t = arg;
	int i, v;
	for (i = 0; i < N_MSG; i++) {
		v = t->id * N_MSG + i;
		t->park_since = xtc_clock_mono();
		t->in_wait = 1;
		if (xtc_send_wait(t->rx->pid, &v, sizeof v, 30000 * MS) !=
		    XTC_OK)
			t->fails++;
		else
			xtc_atomic_fetch_add_i32(&g_sent, 1, xtc_mo_seq_cst);
		t->in_wait = 0;
	}
	xtc_atomic_fetch_add_i32(&g_tx_done, 1, xtc_mo_seq_cst);
}

/* xtc_send_notify: one request mid-run, answered by one ROOM message,
 * then one message (the receiver counts it in its total). */
static void
notifier(void *arg)
{
	struct tx *t = arg;
	xtc_room_info_t ri = XTC_ROOM_INFO_INIT;
	uint64_t ref;
	void *m;
	size_t sz;
	int v = -1;
	(void)xtc_proc_sleep((int64_t)(1 + __xtc_sim_rng_range(
	                         XTC_SIM_RNG_APP, 20)) * MS);
	if (xtc_send_notify(t->rx->pid, &ref) != XTC_OK)
		return;
	xtc_atomic_fetch_add_i32(&g_notes, 1, xtc_mo_seq_cst);
	while (xtc_recv(&m, &sz, 30000 * MS) == XTC_OK) {
		int ok = xtc_room_decode(m, sz, &ri) == XTC_OK &&
		    ri.ref == ref;
		free(m);
		if (ok) {
			xtc_atomic_fetch_add_i32(&g_notified, 1,
			    xtc_mo_seq_cst);
			break;
		}
	}
	if (xtc_send_wait(t->rx->pid, &v, sizeof v, 30000 * MS) == XTC_OK)
		xtc_atomic_fetch_add_i32(&g_sent, 1, xtc_mo_seq_cst);
}

/* Every 20 ms of virtual time: a sender parked for 200 ms on a receiver
 * whose mailbox has room is a lost wakeup. */
static void
watchdog(void *arg)
{
	xtc_mailbox_stats_t st = XTC_MAILBOX_STATS_INIT;
	int i, j;
	(void)arg;
	while (xtc_atomic_load_i32(&g_tx_done, xtc_mo_seq_cst) <
	    N_RX * N_TX) {
		(void)xtc_proc_sleep(20 * MS);
		for (i = 0; i < N_RX; i++) {
			if (xtc_proc_mailbox_stats(g_rx[i].pid, &st) != XTC_OK)
				continue;
			if (st.depth + st.saved > CAP / 2)
				continue;
			for (j = 0; j < N_TX; j++)
				if (g_tx[i][j].in_wait &&
				    xtc_clock_mono() - g_tx[i][j].park_since >
				        200 * MS)
					g_stuck++;
		}
	}
}

static int
run_one(uint64_t seed, uint64_t *out_trace, uint64_t *out_state)
{
	xtc_exec_t *e;
	xtc_proc_opts_t o = XTC_PROC_OPTS_INIT;
	int i, j, rc;

	memset(g_rx, 0, sizeof g_rx);
	memset(g_tx, 0, sizeof g_tx);
	memset(g_nt, 0, sizeof g_nt);
	g_notified = g_notes = g_stuck = g_tx_done = g_sent = 0;
	g_trace = 0;
	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK)
		return -1;
	o.mailbox_cap = CAP;
	for (i = 0; i < N_RX; i++) {
		g_rx[i].selective = i & 1;
		(void)xtc_proc_spawn(xtc_exec_loop(e, i), receiver, &g_rx[i],
		    &o, &g_rx[i].pid);
	}
	for (i = 0; i < N_RX; i++) {
		for (j = 0; j < N_TX; j++) {
			g_tx[i][j].rx = &g_rx[i];
			g_tx[i][j].id = j;
			(void)xtc_proc_spawn(xtc_exec_loop(e, (i + j + 1) %
			    N_LOOPS), sender, &g_tx[i][j], NULL, NULL);
		}
		g_nt[i].rx = &g_rx[i];
		(void)xtc_proc_spawn(xtc_exec_loop(e, (i + 2) % N_LOOPS),
		    notifier, &g_nt[i], NULL, NULL);
	}
	(void)xtc_proc_spawn(xtc_exec_loop(e, 0), watchdog, NULL, NULL, NULL);
	xtc_sim_net_latency(10 * 1000LL, 300 * 1000LL);
	rc = xtc_sim_exec_run(e, seed, 50000000);
	xtc_sim_net_latency(0, 0);
	*out_trace = g_trace;
	*out_state = xtc_sim_state_hash(e);
	(void)xtc_exec_fini(e);
	return rc;
}

/*
 * The in-flight window, (e) and (f).  A cap-2 receiver on loop 0 sleeps
 * while a sender on loop 1 sends a burst of 10: only what the mailbox
 * can hold may come back XTC_OK, and all of that must arrive.  Once the
 * receiver has drained, a second burst must be admitted again (the room
 * the first burst held came back).  A message sent to a proc that exits
 * inside the latency window is dropped, and so is one still in flight
 * when the run is cut short; neither may leak.
 */
#define IF_BURST 10
#define IF_CAP 2

static xtc_pid_t g_if_rx;
static int g_if_ok[2], g_if_got, g_if_late;

static void
if_receiver(void *arg)
{
	void *m;
	size_t sz;
	(void)arg;
	(void)xtc_proc_sleep(5 * MS);
	while (xtc_recv(&m, &sz, 30 * MS) == XTC_OK) {
		xtc_free(m); /* through the hook the auditor counts */
		g_if_got++;
	}
}

static void
if_sender(void *arg)
{
	int i, round;
	char big[1024]; /* above the inline size: its own block */
	(void)arg;
	memset(big, 'x', sizeof big);
	for (round = 0; round < 2; round++) {
		for (i = 0; i < IF_BURST; i++)
			if (xtc_send(g_if_rx, big, sizeof big) == XTC_OK)
				g_if_ok[round]++;
		(void)xtc_proc_sleep(20 * MS);
	}
}

/* Exits after 5 us, before the latency floor (10 us): a message sent
 * to it at time 0 lands after it is gone. */
static void
if_short(void *arg)
{
	(void)arg;
	(void)xtc_proc_sleep(5 * 1000LL);
}

static void
if_late_sender(void *arg)
{
	char big[1024];
	xtc_pid_t *to = arg;
	memset(big, 'y', sizeof big);
	if (xtc_send(*to, big, sizeof big) == XTC_OK)
		g_if_late++;
}

/* One run of the in-flight cases; *grew = live allocations the run left
 * behind (the auditor's count after the exec is finalized, minus before). */
static int
if_run(uint64_t seed, long max_steps, int *grew)
{
	xtc_exec_t *e;
	xtc_proc_opts_t o = XTC_PROC_OPTS_INIT;
	xtc_pid_t short_pid;
	size_t c0 = 0, c1 = 0;
	int rc;

	g_if_ok[0] = g_if_ok[1] = g_if_got = g_if_late = 0;
	xtc_alloc_audit_stats(&c0, NULL);
	if (xtc_exec_init(&e, 2) != XTC_OK)
		return -1;
	o.mailbox_cap = IF_CAP;
	(void)xtc_proc_spawn(xtc_exec_loop(e, 0), if_receiver, NULL, &o,
	    &g_if_rx);
	(void)xtc_proc_spawn(xtc_exec_loop(e, 1), if_sender, NULL, NULL, NULL);
	(void)xtc_proc_spawn(xtc_exec_loop(e, 0), if_short, NULL, NULL,
	    &short_pid);
	(void)xtc_proc_spawn(xtc_exec_loop(e, 1), if_late_sender, &short_pid,
	    NULL, NULL);
	xtc_sim_net_latency(10 * 1000LL, 200 * 1000LL);
	rc = xtc_sim_exec_run(e, seed, max_steps);
	xtc_sim_net_latency(0, 0);
	(void)xtc_exec_fini(e);
	xtc_alloc_audit_stats(&c1, NULL);
	*grew = c1 > c0 ? (int)(c1 - c0) : 0;
	return rc;
}

static int
if_check(uint64_t k, uint64_t seed)
{
	int grew = 0, rc, pass = 1;

	rc = if_run(seed, 50000000, &grew);
	if (g_if_got != g_if_ok[0] + g_if_ok[1] || g_if_ok[0] > IF_CAP) {
		printf("  seed %llu: a message sent with XTC_OK was lost "
		       "(sent %d+%d, received %d, cap %d)\n",
		    (unsigned long long)k, g_if_ok[0], g_if_ok[1], g_if_got,
		    IF_CAP);
		pass = 0;
	}
	if (rc != XTC_OK || g_if_ok[0] == 0 || g_if_ok[1] == 0 ||
	    g_if_late != 1) {
		printf("  seed %llu: in-flight run rc=%d sent %d+%d late %d "
		       "(the room did not come back)\n",
		    (unsigned long long)k, rc, g_if_ok[0], g_if_ok[1],
		    g_if_late);
		pass = 0;
	}
	if (grew != 0) {
		printf("  seed %llu: a message dropped in flight leaked "
		       "(%d allocations)\n",
		    (unsigned long long)k, grew);
		pass = 0;
	}
	/* Cut the run at its first steps: the burst is still in flight
	 * when the exec is finalized. */
	(void)if_run(seed, 3, &grew);
	if (grew != 0) {
		printf("  seed %llu: a message in flight at the end of a run "
		       "leaked (%d allocations)\n",
		    (unsigned long long)k, grew);
		pass = 0;
	}
	return pass;
}

int
main(int argc, char **argv)
{
	uint64_t base = 0, seed, tr, tr2, st, st2;
	int n = 16, k, i, j, fails = 0, rc, rc2;

	if (argc > 1) base = strtoull(argv[1], NULL, 0);
	if (argc > 2) n = atoi(argv[2]);
	printf("== send-wait DST: %d seeds from base %llu ==\n", n,
	    (unsigned long long)base);
	/* The auditor counts live allocations for (f).  One warm-up run
	 * first: process-wide caches (slabs, the proc-table free list) are
	 * made once and kept. */
	if (xtc_alloc_audit_enable(1) != XTC_OK) {
		printf("FAIL: xtc_alloc_audit_enable\n");
		return 1;
	}
	(void)if_run(0x9E3779B97F4A7C15ull, 50000000, &i);
	for (k = 1; k <= n; k++) {
		int pass = 1, got = 0, dup = 0, sfail = 0;
		seed = 0x9E3779B97F4A7C15ull * (base + (uint64_t)k);
		rc = run_one(seed, &tr, &st);
		for (i = 0; i < N_RX; i++) {
			got += g_rx[i].got;
			dup += g_rx[i].dup;
			for (j = 0; j < N_TX; j++)
				sfail += g_tx[i][j].fails;
		}
		if (got < g_sent) {
			printf(
			    "  seed %llu: a message sent with XTC_OK was lost "
			    "(sent %d, received %d)\n",
			    (unsigned long long)(base + (uint64_t)k), g_sent,
			    got);
			pass = 0;
		}
		if (g_stuck > 0) {
			printf("  seed %llu: a sender waited with room in the "
			       "mailbox (%d samples)\n",
			    (unsigned long long)(base + (uint64_t)k), g_stuck);
			pass = 0;
		}
		if (rc != XTC_OK || dup != 0 || sfail != 0 ||
		    got != N_RX * (N_TX * N_MSG + 1) ||
		    g_notified != g_notes || g_notes != N_RX) {
			printf("  seed %llu: rc=%d got=%d/%d dup=%d fails=%d "
			       "notified=%d/%d\n",
			    (unsigned long long)(base + (uint64_t)k), rc, got,
			    N_RX * (N_TX * N_MSG + 1), dup, sfail, g_notified,
			    g_notes);
			pass = 0;
		}
		if (!if_check(base + (uint64_t)k, seed))
			pass = 0;
		if (pass) {
			rc2 = run_one(seed, &tr2, &st2);
			if (rc2 != rc || tr2 != tr || st2 != st) {
				printf("  seed %llu: replay diverged\n",
				    (unsigned long long)(base + (uint64_t)k));
				pass = 0;
			}
		}
		if (!pass)
			fails++;
	}
	if (fails != 0) {
		printf("FAIL: %d/%d send-wait seeds failed\n", fails, n);
		return 1;
	}
	(void)xtc_alloc_audit_enable(0);
	printf("OK: send-wait DST -- %d seeds, %d senders x %d messages into "
	       "cap-%d mailboxes (mailbox and save-queue takes) under net "
	       "latency, every message once, no lost room wakeup, every "
	       "notify answered, nothing sent with XTC_OK lost or leaked in "
	       "flight, all replay\n",
	    n, N_RX * N_TX, N_MSG, CAP);
	return 0;
}
