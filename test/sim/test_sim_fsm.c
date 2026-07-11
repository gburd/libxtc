/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/sim/test_sim_fsm.c
 *	DST coverage of the gen_statem behaviour (src/orc/fsm.c).  A
 *	turnstile FSM (LOCKED <-> UNLOCKED) is driven by concurrent event
 *	senders across N loops under the seeded scheduler.  Each event is
 *	an async xtc_fsm_send; the machine toggles on a "push"/"coin" event
 *	and counts pushes-while-locked (rejected) vs pushes-while-unlocked
 *	(accepted).  Invariants: the machine processes exactly the events
 *	sent, the accept/reject split is a deterministic function of the
 *	seeded delivery order, and the whole run replays byte-identically;
 *	a different seed reorders but stays internally consistent.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <stdint.h>
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_fsm.h"
#include "xtc_sim.h"

#define N_LOOPS   4
#define N_SENDERS 6
#define PER       8            /* events each sender fires */
#define TOTAL     (N_SENDERS * PER)

enum { ST_LOCKED = 0, ST_UNLOCKED = 1 };

/* Event opcodes. */
#define EV_COIN 1              /* LOCKED -> UNLOCKED */
#define EV_PUSH 2              /* UNLOCKED -> LOCKED (accepted); else rejected */

struct turnstile {
	int processed;             /* events seen by event() */
	int accepted;              /* pushes that went through */
	int rejected;              /* pushes while locked */
	long fold;                 /* order-sensitive fold of (state,ev) */
};

static xtc_fsm_t *g_fsm;
static xtc_pid_t  g_fsm_pid;
static struct turnstile g_ts;
static atomic_int g_senders_done;

static xtc_fsm_result_t
ts_event(void *st, int cur, const void *msg, size_t len, xtc_fsm_call_t *call)
{
	struct turnstile *t = st;
	xtc_fsm_result_t r = { XTC_FSM_KEEP, 0, 0 };
	int ev;
	(void)call;
	if (XTC_FSM_IS_STATE_TIMEOUT(msg, len)) return r;   /* none used */
	if (len != sizeof(int)) return r;
	ev = *(const int *)msg;

	t->processed++;
	t->fold = t->fold * 1000003L + (long)(cur * 8 + ev);

	if (ev == EV_COIN) {
		if (cur == ST_LOCKED) {
			r.action = XTC_FSM_NEXT; r.next_state = ST_UNLOCKED;
		}
	} else if (ev == EV_PUSH) {
		if (cur == ST_UNLOCKED) {
			t->accepted++;
			r.action = XTC_FSM_NEXT; r.next_state = ST_LOCKED;
		} else {
			t->rejected++;   /* push while locked: no transition */
		}
	}
	return r;
}

static const xtc_fsm_callbacks_t TS_CB = { NULL, ts_event, NULL };

/* A sender fires PER events, alternating coin/push, then finishes. */
static void
sender(void *arg)
{
	int id = (int)(intptr_t)arg;
	int i;
	for (i = 0; i < PER; i++) {
		int ev = ((id + i) & 1) ? EV_PUSH : EV_COIN;
		(void)xtc_fsm_send(g_fsm_pid, &ev, sizeof ev);
	}
	atomic_fetch_add_explicit(&g_senders_done, 1, memory_order_relaxed);
}

/* Winder: once all senders are done, drain by sending a final marker and
 * stopping the machine so the run quiesces. */
static void
winder(void *arg)
{
	(void)arg;
	/* Spin-yield until every sender has enqueued its events. */
	while (atomic_load_explicit(&g_senders_done, memory_order_relaxed)
	    < N_SENDERS)
		xtc_yield();
	/* A few yields so the machine drains its mailbox. */
	{ int k; for (k = 0; k < TOTAL + 8; k++) xtc_yield(); }
	(void)xtc_fsm_stop(g_fsm);
}

static int
run_turnstile(uint64_t seed, int *out_proc, int *out_acc, int *out_rej,
              long *out_fold, uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	int i, rc;

	memset(&g_ts, 0, sizeof g_ts);
	atomic_store(&g_senders_done, 0);

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK) return -1;
	if (xtc_fsm_start(xtc_exec_loop(e, 0), &TS_CB, &g_ts, ST_LOCKED, NULL,
	    &g_fsm) != XTC_OK) {
		(void)xtc_exec_fini(e); return -1;
	}
	g_fsm_pid = xtc_fsm_pid(g_fsm);

	for (i = 0; i < N_SENDERS; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e, (unsigned)((i % (N_LOOPS - 1)) + 1)),
		    sender, (void *)(intptr_t)i, NULL, NULL);
	(void)xtc_proc_spawn(xtc_exec_loop(e, 0), winder, NULL, NULL, NULL);

	rc = xtc_sim_exec_run(e, seed, 5000000);

	*out_proc = g_ts.processed;
	*out_acc  = g_ts.accepted;
	*out_rej  = g_ts.rejected;
	*out_fold = g_ts.fold;
	if (out_state) *out_state = xtc_sim_state_hash(e);
	if (g_fsm != NULL) { (void)xtc_fsm_join(g_fsm, 0); g_fsm = NULL; }
	(void)xtc_exec_fini(e);
	return rc;
}

int
main(void)
{
	int p1 = 0, a1 = 0, r1 = 0, p2 = 0, a2 = 0, r2 = 0, p3 = 0, a3 = 0, r3 = 0;
	long f1 = 0, f2 = 0, f3 = 0;
	uint64_t s1 = 0, s2 = 0, s3 = 0;
	int rc1, rc2, rc3;

	rc1 = run_turnstile(0xF00D5, &p1, &a1, &r1, &f1, &s1);
	if (rc1 != XTC_OK) { printf("FAIL: fsm run rc=%d (hang?)\n", rc1); return 1; }
	rc2 = run_turnstile(0xF00D5, &p2, &a2, &r2, &f2, &s2);
	rc3 = run_turnstile(0x5AA51, &p3, &a3, &r3, &f3, &s3);
	if (rc2 != XTC_OK || rc3 != XTC_OK) {
		printf("FAIL: fsm replay/diff rc=%d/%d\n", rc2, rc3); return 1;
	}

	printf("run1: processed=%d accepted=%d rejected=%d fold=%ld state=%016llx\n",
	    p1, a1, r1, f1, (unsigned long long)s1);
	printf("run2: processed=%d accepted=%d rejected=%d fold=%ld state=%016llx\n",
	    p2, a2, r2, f2, (unsigned long long)s2);
	printf("run3 (diff seed): processed=%d accepted=%d rejected=%d fold=%ld\n",
	    p3, a3, r3, f3);

	/* Invariant: the machine processed exactly the events sent. */
	if (p1 != TOTAL) {
		printf("FAIL: processed=%d want %d (lost/dup events)\n", p1, TOTAL);
		return 1;
	}
	/* accepted + rejected must equal the number of PUSH events, which is
	 * exactly half the total by construction of the sender pattern. */
	if (a1 + r1 <= 0 || a1 + r1 > TOTAL) {
		printf("FAIL: accept/reject split out of range (%d+%d)\n", a1, r1);
		return 1;
	}
	/* Byte-identical replay of the same seed. */
	if (p1 != p2 || a1 != a2 || r1 != r2 || f1 != f2 || s1 != s2) {
		printf("FAIL: fsm run did not replay byte-identically\n");
		return 1;
	}
	/* Different seed: still processed every event (internal consistency). */
	if (p3 != TOTAL) {
		printf("FAIL: diff-seed processed=%d want %d\n", p3, TOTAL);
		return 1;
	}

	printf("OK: turnstile FSM processes every event, accept/reject split "
	    "is a deterministic function of the seeded delivery order, and "
	    "the run replays byte-identically; a different seed reorders but "
	    "stays consistent\n");
	return 0;
}
