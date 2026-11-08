/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * test/sim/test_sim_pid_xexec.c
 *	DST: a pid reaches the proc that owns it, across executors, and a
 *	pid of a finalized loop reaches nothing.
 *
 *	Two executors A and B of NL loops each.  Every loop of A holds a
 *	receiver in slot 0; every loop of B holds a receiver in slot 0 (the
 *	same (local_id, gen) as A's) and a sender in slot 1.  B runs under
 *	the simulator first: each sender sends to every receiver of A and
 *	of B, a message naming the receiver it is for.  A message queued for
 *	a proc that has not run yet waits in its mailbox, so A then runs and
 *	its receivers drain theirs.  Then A is finalized and an executor C,
 *	laid out like B, sends to A's dead pids.
 *
 *	INVARIANTS: no receiver gets a message addressed to another
 *	(MISDELIVERED); every receiver gets exactly NL; every send to a dead
 *	pid fails and C's receivers get nothing; both runs quiesce; the same
 *	seed replays byte-identically (an order-sensitive fold of every pid
 *	and every delivery, plus the sim state hashes) -- which also proves
 *	the loop ids of a run are a function of the seed, not of what ran
 *	before it in the process; a second seed is consistent too.
 *
 *	Planted bug 30 (XTC_DST_BUG_PIDEXEC, scripts/dst-bug-inject.sh)
 *	restores the 2.1 executor-relative loop id: B's senders reach B's
 *	own slot-0 receivers instead of A's, and C reaches its own.
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
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_sim.h"

#define NL     2
#define RECV_T (1000LL * 1000)          /* virtual ns */

struct msg {
	int side, idx, from;            /* side 0 = A, 1 = B, 2 = C */
};

static xtc_pid_t g_rx[3][NL];           /* receivers, by side */
static xtc_pid_t g_dead[NL];
static int g_got[3][NL], g_wrong, g_dead_ok;
static long g_fold;

static void
fold(long v)
{
	long h = xtc_atomic_load_long(&g_fold, xtc_mo_relaxed);
	h = (long)((unsigned long)h * 1000003UL + (unsigned long)v);
	xtc_atomic_store_long(&g_fold, h, xtc_mo_relaxed);
}

static void
fold_pid(xtc_pid_t p)
{
	fold((long)p.loop_id * 131 + (long)p.local_id * 7 + (long)p.gen);
}

static void
receiver(void *arg)
{
	int side = (int)(intptr_t)arg >> 8, me = (int)(intptr_t)arg & 0xff;
	struct msg m;
	void *buf;
	size_t sz;

	while (xtc_recv(&buf, &sz, RECV_T) == XTC_OK) {
		memcpy(&m, buf, sizeof m);
		xtc_free(buf);
		fold(side * 1000 + me * 100 + m.side * 10 + m.from);
		if (m.side == side && m.idx == me)
			xtc_atomic_fetch_add_i32(&g_got[side][me], 1,
			    xtc_mo_seq_cst);
		else {
			xtc_atomic_fetch_add_i32(&g_wrong, 1, xtc_mo_seq_cst);
			printf("  MISDELIVERED: receiver %c%d got a message "
			    "for %c%d\n", 'A' + side, me, 'A' + m.side, m.idx);
		}
	}
}

static void
sender(void *arg)
{
	int me = (int)(intptr_t)arg, side, j;
	struct msg m;

	for (side = 0; side < 2; side++)
		for (j = 0; j < NL; j++) {
			m.side = side;
			m.idx = j;
			m.from = me;
			if (xtc_send(g_rx[side][j], &m, sizeof m) != XTC_OK)
				xtc_atomic_fetch_add_i32(&g_wrong, 1,
				    xtc_mo_seq_cst);
		}
}

static void
dead_sender(void *arg)
{
	struct msg m;
	int j;

	for (j = 0; j < NL; j++) {
		m.side = 0;
		m.idx = j;
		m.from = (int)(intptr_t)arg;
		if (xtc_send(g_dead[j], &m, sizeof m) == XTC_OK)
			xtc_atomic_fetch_add_i32(&g_dead_ok, 1, xtc_mo_seq_cst);
	}
}

static int
spawn_side(xtc_exec_t *e, int side, int with_sender, xtc_proc_fn snd)
{
	int i;

	for (i = 0; i < NL; i++)
		if (xtc_proc_spawn(xtc_exec_loop(e, i), receiver,
		    (void *)(intptr_t)(side << 8 | i), NULL, &g_rx[side][i]) !=
		    XTC_OK)
			return -1;
	for (i = 0; with_sender && i < NL; i++)
		if (xtc_proc_spawn(xtc_exec_loop(e, i), snd,
		    (void *)(intptr_t)i, NULL, NULL) != XTC_OK)
			return -1;
	return 0;
}

struct out {
	int rc[3], wrong, dead_ok, got[3][NL];
	long fold;
	uint64_t st[3];
	uint32_t ids[3][NL];
};

static void
run(uint64_t seed, struct out *o)
{
	xtc_exec_t *a = NULL, *b = NULL, *c = NULL;
	int s, i;

	memset(o, 0, sizeof *o);
	memset(g_rx, 0, sizeof g_rx);
	memset(g_got, 0, sizeof g_got);
	xtc_atomic_store_i32(&g_wrong, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_dead_ok, 0, xtc_mo_seq_cst);
	xtc_atomic_store_long(&g_fold, 0, xtc_mo_seq_cst);
	if (xtc_exec_init(&a, NL) != XTC_OK || xtc_exec_init(&b, NL) !=
	    XTC_OK || spawn_side(a, 0, 0, NULL) != 0 ||
	    spawn_side(b, 1, 1, sender) != 0) {
		o->rc[0] = XTC_E_NOMEM;
		return;
	}
	o->rc[1] = xtc_sim_exec_run(b, seed, 1000000);
	o->st[1] = xtc_sim_state_hash(b);
	o->rc[0] = xtc_sim_exec_run(a, seed ^ 0xA, 1000000);
	o->st[0] = xtc_sim_state_hash(a);
	for (i = 0; i < NL; i++)
		g_dead[i] = g_rx[0][i];
	(void)xtc_exec_fini(a);
	if (xtc_exec_init(&c, NL) != XTC_OK ||
	    spawn_side(c, 2, 1, dead_sender) != 0) {
		o->rc[2] = XTC_E_NOMEM;
		return;
	}
	o->rc[2] = xtc_sim_exec_run(c, seed ^ 0xC, 1000000);
	o->st[2] = xtc_sim_state_hash(c);
	(void)xtc_exec_fini(c);
	(void)xtc_exec_fini(b);
	for (s = 0; s < 3; s++)
		for (i = 0; i < NL; i++) {
			fold_pid(g_rx[s][i]);
			o->ids[s][i] = g_rx[s][i].loop_id;
			o->got[s][i] =
			    xtc_atomic_load_i32(&g_got[s][i], xtc_mo_seq_cst);
		}
	o->wrong = xtc_atomic_load_i32(&g_wrong, xtc_mo_seq_cst);
	o->dead_ok = xtc_atomic_load_i32(&g_dead_ok, xtc_mo_seq_cst);
	o->fold = xtc_atomic_load_long(&g_fold, xtc_mo_seq_cst);
}

static int
check(const char *what, const struct out *o)
{
	int s, i, ok = 1;

	for (s = 0; s < 3; s++)
		if (o->rc[s] != XTC_OK) {
			printf("FAIL: %s: run %c rc=%d (hang?)\n", what,
			    'A' + s, o->rc[s]);
			ok = 0;
		}
	if (o->wrong != 0) {
		printf("FAIL: %s: MISDELIVERED %d message(s)\n", what,
		    o->wrong);
		ok = 0;
	}
	for (s = 0; s < 2; s++)
		for (i = 0; i < NL; i++)
			if (o->got[s][i] != NL) {
				printf("FAIL: %s: receiver %c%d got %d of %d\n",
				    what, 'A' + s, i, o->got[s][i], NL);
				ok = 0;
			}
	if (o->dead_ok != 0 || o->got[2][0] != 0 || o->got[2][1] != 0) {
		printf("FAIL: %s: MISDELIVERED to a dead loop's pid: %d send(s) "
		    "accepted\n", what, o->dead_ok);
		ok = 0;
	}
	return ok;
}

int
main(void)
{
	struct out r1, r2, r3;

	run(0x9D1E, &r1);
	run(0x9D1E, &r2);
	run(0x5EED7, &r3);
	printf("run1: loop ids A %u,%u B %u,%u C %u,%u fold=%ld "
	    "state=%016llx/%016llx/%016llx\n", r1.ids[0][0], r1.ids[0][1],
	    r1.ids[1][0], r1.ids[1][1], r1.ids[2][0], r1.ids[2][1], r1.fold,
	    (unsigned long long)r1.st[0], (unsigned long long)r1.st[1],
	    (unsigned long long)r1.st[2]);
	if (!check("run1", &r1) || !check("run3", &r3))
		return 1;
	if (r1.fold != r2.fold || memcmp(r1.st, r2.st, sizeof r1.st) != 0 ||
	    memcmp(r1.ids, r2.ids, sizeof r1.ids) != 0) {
		printf("FAIL: did not replay (fold %ld/%ld, loop id A0 %u/%u)\n",
		    r1.fold, r2.fold, r1.ids[0][0], r2.ids[0][0]);
		return 1;
	}
	printf("OK: pids cross executors exactly, a dead loop's pid reaches "
	    "nothing, replay byte-identical\n");
	return 0;
}
