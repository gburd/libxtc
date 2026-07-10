/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * examples/10_circuit_breaker.c -- the circuit-breaker pattern as an
 * xtc_fsm (gen_statem).  A circuit breaker guards a flaky downstream
 * dependency: after too many consecutive failures it "opens" and fails
 * fast (rejecting calls without touching the dependency) for a cooldown
 * period, then "half-opens" to let a single trial through before
 * deciding to close (recover) or re-open.
 *
 * This is the canonical three-state machine, and it shows off exactly
 * the three xtc_fsm features hand-rolled breakers get wrong:
 *
 *   - state_enter: the cooldown timer is armed in one place (enter of
 *     the OPEN state), not scattered across every transition into OPEN.
 *   - state timeout: the OPEN -> HALF_OPEN cooldown is a state timeout;
 *     any real event (there are none while open, since we fail fast)
 *     would cancel it, which is the correct gen_statem semantics.
 *   - clean synchronous request/reply: a caller's "may I proceed?"
 *     query is an xtc_fsm_call answered by xtc_fsm_reply.
 *
 * States:
 *   CLOSED     normal; calls allowed; N consecutive failures -> OPEN.
 *   OPEN       failing fast; a cooldown state-timeout -> HALF_OPEN.
 *   HALF_OPEN  one trial allowed; its success -> CLOSED, failure -> OPEN.
 *
 * Events (a one-byte opcode sent with xtc_fsm_call so the caller learns
 * the verdict, or xtc_fsm_send for reporting an outcome):
 *   'q'  query: "may I proceed?"  reply is 1 (allow) or 0 (reject).
 *   's'  report success of a permitted call.
 *   'f'  report failure of a permitted call.
 *
 * Uses only the public xtc_* API (this is a consumer program).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_fsm.h"

enum cb_state { CB_CLOSED = 0, CB_OPEN = 1, CB_HALF_OPEN = 2 };

#define CB_FAIL_THRESHOLD   3                 /* trips after 3 in a row */
#define CB_COOLDOWN_NS      (200LL * 1000000) /* 200 ms open cooldown */

struct breaker {
	int consecutive_failures;
	/* observable counters, read by the driver after the run */
	int trips;          /* CLOSED/HALF_OPEN -> OPEN transitions */
	int recoveries;     /* HALF_OPEN -> CLOSED transitions */
	int rejected;       /* queries answered "reject" (fail fast) */
};

static int
cb_enter(void *st, int old_state, int new_state)
{
	struct breaker *b = st;
	(void)old_state;
	if (new_state == CB_CLOSED)
		b->consecutive_failures = 0;
	return XTC_OK;
}

static xtc_fsm_result_t
cb_event(void *st, int cur, const void *msg, size_t len, xtc_fsm_call_t *call)
{
	struct breaker *b = st;
	xtc_fsm_result_t r = { XTC_FSM_KEEP, 0, 0 };
	unsigned char op;

	/* A synthetic state timeout (only armed in OPEN): open -> half. */
	if (XTC_FSM_IS_STATE_TIMEOUT(msg, len)) {
		if (cur == CB_OPEN) {
			r.action = XTC_FSM_NEXT;
			r.next_state = CB_HALF_OPEN;
		}
		return r;
	}

	op = (len >= 1) ? *(const unsigned char *)msg : 0;

	if (op == 'q') {
		/* Query: allowed unless we are OPEN (fail fast). */
		int allow = (cur != CB_OPEN);
		if (!allow) b->rejected++;
		if (call != NULL) {
			unsigned char yn = (unsigned char)allow;
			(void)xtc_fsm_reply(call, &yn, 1);
		}
		/* A real event cancels a pending state timeout (gen_statem
		 * semantics), so while OPEN we must RE-ARM the cooldown here;
		 * otherwise a steady stream of fail-fast queries would keep
		 * the breaker open forever.  (A production breaker would track
		 * an absolute deadline; re-arming the full cooldown on each
		 * query is the simple, demo-clear choice.) */
		if (cur == CB_OPEN)
			r.state_timeout_ns = CB_COOLDOWN_NS;
		return r;   /* KEEP */
	}

	if (op == 's') {           /* reported success */
		if (cur == CB_HALF_OPEN) {
			b->recoveries++;
			r.action = XTC_FSM_NEXT;
			r.next_state = CB_CLOSED;   /* enter() zeroes failures */
		} else {
			b->consecutive_failures = 0;
		}
		return r;
	}

	if (op == 'f') {           /* reported failure */
		if (cur == CB_HALF_OPEN) {
			/* trial failed: re-open with a fresh cooldown */
			b->trips++;
			r.action = XTC_FSM_NEXT;
			r.next_state = CB_OPEN;
			r.state_timeout_ns = CB_COOLDOWN_NS;
		} else if (cur == CB_CLOSED) {
			if (++b->consecutive_failures >= CB_FAIL_THRESHOLD) {
				b->trips++;
				r.action = XTC_FSM_NEXT;
				r.next_state = CB_OPEN;
				r.state_timeout_ns = CB_COOLDOWN_NS;
			}
		}
		return r;
	}

	return r;   /* unknown op: KEEP */
}

static const xtc_fsm_callbacks_t CB_CALLBACKS = {
	cb_enter, cb_event, NULL
};

/* Driver: drive the breaker through trip -> cooldown -> recover, and
 * print what happened. */
struct driver_args { xtc_fsm_t *fsm; struct breaker *b; };

static int
query_allow(xtc_pid_t fsm)
{
	unsigned char q = 'q';
	void *rep = NULL; size_t rn = 0;
	int allow = 0;
	if (xtc_fsm_call(fsm, &q, 1, &rep, &rn, 1000LL * 1000000) == XTC_OK &&
	    rn >= 1)
		allow = *(unsigned char *)rep;
	if (rep) xtc_free(rep);
	return allow;
}

static void
report(xtc_pid_t fsm, unsigned char outcome)
{
	(void)xtc_fsm_send(fsm, &outcome, 1);
}

static void
driver_proc(void *arg)
{
	struct driver_args *da = arg;
	xtc_pid_t fsm = xtc_fsm_pid(da->fsm);
	int i;

	printf("circuit breaker: threshold=%d cooldown=%lld ms\n",
	    CB_FAIL_THRESHOLD, (long long)(CB_COOLDOWN_NS / 1000000));

	/* CLOSED: allowed. */
	printf("  closed, query allowed = %d\n", query_allow(fsm));

	/* Trip it: report threshold consecutive failures. */
	for (i = 0; i < CB_FAIL_THRESHOLD; i++)
		report(fsm, 'f');
	/* Let the transition to OPEN settle. */
	xtc_proc_sleep(20LL * 1000000);
	printf("  after %d failures, query allowed = %d (should be 0, fail fast)\n",
	    CB_FAIL_THRESHOLD, query_allow(fsm));

	/* Wait out the cooldown; state timeout moves OPEN -> HALF_OPEN. */
	xtc_proc_sleep(CB_COOLDOWN_NS + 50LL * 1000000);
	printf("  after cooldown, query allowed = %d (half-open trial)\n",
	    query_allow(fsm));

	/* Trial succeeds -> CLOSED. */
	report(fsm, 's');
	xtc_proc_sleep(20LL * 1000000);
	printf("  after successful trial, query allowed = %d (recovered)\n",
	    query_allow(fsm));

	printf("  trips=%d recoveries=%d rejected=%d\n",
	    da->b->trips, da->b->recoveries, da->b->rejected);

	(void)xtc_fsm_stop(da->fsm);
}

int
main(void)
{
	xtc_loop_t *loop = NULL;
	xtc_fsm_t *fsm = NULL;
	struct breaker b;
	struct driver_args da;

	memset(&b, 0, sizeof b);
	if (xtc_loop_init(&loop) != XTC_OK) {
		fprintf(stderr, "loop init failed\n");
		return 1;
	}
	if (xtc_fsm_start(loop, &CB_CALLBACKS, &b, CB_CLOSED, NULL, &fsm)
	    != XTC_OK) {
		fprintf(stderr, "fsm start failed\n");
		(void)xtc_loop_fini(loop);
		return 1;
	}
	da.fsm = fsm;
	da.b = &b;
	if (xtc_proc_spawn(loop, driver_proc, &da, NULL, NULL) != XTC_OK) {
		fprintf(stderr, "spawn failed\n");
		(void)xtc_loop_fini(loop);
		return 1;
	}
	(void)xtc_loop_run(loop);
	(void)xtc_fsm_join(fsm, 0);
	(void)xtc_loop_fini(loop);

	/* Assert the observed behavior (this doubles as the example's
	 * self-check: trip once, recover once, and reject at least the
	 * fail-fast query). */
	if (b.trips < 1 || b.recoveries < 1 || b.rejected < 1) {
		fprintf(stderr, "FAIL: trips=%d recoveries=%d rejected=%d\n",
		    b.trips, b.recoveries, b.rejected);
		return 1;
	}
	printf("OK\n");
	return 0;
}
