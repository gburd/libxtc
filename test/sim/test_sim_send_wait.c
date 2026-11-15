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
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_sim.h"

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
static int g_notified, g_notes, g_stuck, g_tx_done;
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
	(void)xtc_send_wait(t->rx->pid, &v, sizeof v, 30000 * MS);
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
	g_notified = g_notes = g_stuck = g_tx_done = 0;
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
	rc = xtc_sim_exec_run(e, seed, 50000000);
	*out_trace = g_trace;
	*out_state = xtc_sim_state_hash(e);
	(void)xtc_exec_fini(e);
	return rc;
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
	printf("OK: send-wait DST -- %d seeds, %d senders x %d messages into "
	       "cap-%d mailboxes (mailbox and save-queue takes), every "
	       "message once, no lost room wakeup, every notify answered, "
	       "all replay\n", n, N_RX * N_TX, N_MSG, CAP);
	return 0;
}
