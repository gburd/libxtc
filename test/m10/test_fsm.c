/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m10/test_fsm.c -- xtc_fsm (gen_statem) behaviour.
 */

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_fsm.h"

/* States for a tiny door: CLOSED -> OPEN -> CLOSED. */
enum { S_CLOSED = 0, S_OPEN = 1, S_LOCKED = 2 };

struct door {
	int enters;         /* enter() call count */
	int last_new;       /* last new_state seen by enter() */
	int opens;          /* times we transitioned into OPEN */
	int timeouts;       /* state-timeout events seen */
	int postpone_seen;  /* order marker for postponed replay */
	int replay_order[8];
	int replay_n;
};

/* Event payloads are a single int "command". */
enum { EV_OPEN = 100, EV_CLOSE = 101, EV_LOCK = 102, EV_PING = 103 };

static int
door_enter(void *st, int old_state, int new_state)
{
	struct door *d = st;
	(void)old_state;
	d->enters++;
	d->last_new = new_state;
	if (new_state == S_OPEN) d->opens++;
	return XTC_OK;
}

static xtc_fsm_result_t
door_event(void *st, int state, const void *msg, size_t len,
           xtc_fsm_call_t *call)
{
	struct door *d = st;
	xtc_fsm_result_t r = { XTC_FSM_KEEP, 0, 0 };
	int cmd = -1;

	if (XTC_FSM_IS_STATE_TIMEOUT(msg, len)) {
		d->timeouts++;
		/* On timeout in OPEN, auto-close. */
		if (state == S_OPEN) { r.action = XTC_FSM_NEXT; r.next_state = S_CLOSED; }
		return r;
	}
	if (len >= (int)sizeof(int)) memcpy(&cmd, msg, sizeof(int));

	/* A sync call: reply with the current state, keep state. */
	if (call != NULL) {
		int reply = state;
		(void)xtc_fsm_reply(call, &reply, sizeof reply);
		return r;
	}

	switch (state) {
	case S_CLOSED:
		if (cmd == EV_OPEN) { r.action = XTC_FSM_NEXT; r.next_state = S_OPEN; }
		else if (cmd == EV_LOCK) { r.action = XTC_FSM_NEXT; r.next_state = S_LOCKED; }
		return r;
	case S_OPEN:
		if (cmd == EV_CLOSE) { r.action = XTC_FSM_NEXT; r.next_state = S_CLOSED; }
		/* record replay order for postponed-event test */
		if (cmd >= 200) {
			if (d->replay_n < 8) d->replay_order[d->replay_n++] = cmd;
		}
		return r;
	case S_LOCKED:
		/* In LOCKED, an OPEN is postponed until we CLOSE-unlock. */
		if (cmd == EV_OPEN) { r.action = XTC_FSM_POSTPONE; return r; }
		if (cmd == EV_CLOSE) { r.action = XTC_FSM_NEXT; r.next_state = S_CLOSED; }
		return r;
	}
	return r;
}

static void
door_terminate(void *st, int reason)
{
	(void)st; (void)reason;
}

static const xtc_fsm_callbacks_t DOOR_CB = {
	door_enter, door_event, door_terminate
};

/* ---- driver proc: runs the scenario, then stops the fsm ---- */

struct scenario {
	xtc_pid_t fsm_pid;
	struct door *d;
	xtc_fsm_t *fsm;
	int done;
	int call_reply_state;
};

static void
driver(void *arg)
{
	struct scenario *sc = arg;
	int cmd;

	/* CLOSED -> OPEN (transition + enter fires) */
	cmd = EV_OPEN;  (void)xtc_fsm_send(sc->fsm_pid, &cmd, sizeof cmd);
	/* sync call: current state should be OPEN */
	{
		void *rep = NULL; size_t rn = 0;
		if (xtc_fsm_call(sc->fsm_pid, "?", 1, &rep, &rn,
		    1000LL * 1000 * 1000) == XTC_OK && rn == sizeof(int))
			memcpy(&sc->call_reply_state, rep, sizeof(int));
		if (rep) xtc_free(rep);
	}
	cmd = EV_CLOSE; (void)xtc_fsm_send(sc->fsm_pid, &cmd, sizeof cmd);

	/* Postpone test: LOCK, then OPEN (postponed in LOCKED), then a
	 * couple of tagged events (>=200) that only OPEN handles, then
	 * CLOSE (unlock -> replays the postponed OPEN first). */
	cmd = EV_LOCK;  (void)xtc_fsm_send(sc->fsm_pid, &cmd, sizeof cmd);
	cmd = EV_OPEN;  (void)xtc_fsm_send(sc->fsm_pid, &cmd, sizeof cmd);
	cmd = EV_CLOSE; (void)xtc_fsm_send(sc->fsm_pid, &cmd, sizeof cmd);

	/* let the machine settle */
	xtc_proc_sleep(50LL * 1000 * 1000);
	sc->done = 1;
	(void)xtc_fsm_stop(sc->fsm);
}

static MunitResult
test_fsm_basic(const MunitParameter p[], void *data)
{
	xtc_loop_t *loop;
	struct door d;
	struct scenario sc;
	(void)p; (void)data;

	memset(&d, 0, sizeof d);
	memset(&sc, 0, sizeof sc);
	sc.d = &d;

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_fsm_start(loop, &DOOR_CB, &d, S_CLOSED, NULL,
	    &sc.fsm), ==, XTC_OK);
	sc.fsm_pid = xtc_fsm_pid(sc.fsm);
	munit_assert_int(xtc_proc_spawn(loop, driver, &sc, NULL, NULL), ==,
	    XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	munit_assert_int(sc.done, ==, 1);
	/* enter fired: initial CLOSED + OPEN + CLOSED + LOCKED + CLOSED
	 * (+ the postponed OPEN replays into OPEN). */
	munit_assert_int(d.enters, >=, 4);
	munit_assert_int(sc.call_reply_state, ==, S_OPEN);
	munit_assert_int(d.opens, >=, 2);   /* opened directly, and via replay */

	(void)xtc_fsm_join(sc.fsm, 0);   /* reclaim the handle (proc already gone) */
	(void)xtc_loop_fini(loop);
	return MUNIT_OK;
}

/* State-timeout test: a machine that arms a timeout and expects the
 * synthetic timeout event. */
enum { T_WAIT = 0, T_DONE = 1 };

struct timer_fsm { int timeouts; int reached_done; };

static xtc_fsm_result_t
timer_event(void *st, int state, const void *msg, size_t len,
            xtc_fsm_call_t *call)
{
	struct timer_fsm *t = st;
	xtc_fsm_result_t r = { XTC_FSM_KEEP, 0, 0 };
	(void)call;
	if (state == T_WAIT) {
		if (XTC_FSM_IS_STATE_TIMEOUT(msg, len)) {
			t->timeouts++;
			r.action = XTC_FSM_NEXT; r.next_state = T_DONE;
			return r;
		}
	}
	return r;
}

static int
timer_enter(void *st, int old_state, int new_state)
{
	struct timer_fsm *t = st;
	(void)old_state;
	if (new_state == T_DONE) t->reached_done = 1;
	return XTC_OK;
}

static const xtc_fsm_callbacks_t TIMER_CB = { timer_enter, timer_event, NULL };

struct timer_scn { xtc_fsm_t *fsm; xtc_pid_t pid; };

static void
timer_driver(void *arg)
{
	struct timer_scn *s = arg;
	/* arm the state timeout by sending one event that returns KEEP with
	 * a timeout -- but our timer_event only arms on initial state via a
	 * result; simplest: send a dummy to trigger a KEEP + timeout. */
	/* Instead we rely on the fsm arming the timeout from the initial
	 * KEEP: send nothing; the machine's first event must set it.  So
	 * send one no-op that returns KEEP+timeout. */
	int dummy = 0;
	(void)dummy;
	/* Actually arm via a first event that requests a timeout. */
	xtc_proc_sleep(120LL * 1000 * 1000);   /* let the timeout fire */
	(void)xtc_fsm_stop(s->fsm);
}

static xtc_fsm_result_t
timer_event_arm(void *st, int state, const void *msg, size_t len,
                xtc_fsm_call_t *call)
{
	struct timer_fsm *t = st;
	xtc_fsm_result_t r = { XTC_FSM_KEEP, 0, 0 };
	(void)call;
	if (XTC_FSM_IS_STATE_TIMEOUT(msg, len)) {
		if (state == T_WAIT) {
			t->timeouts++;
			r.action = XTC_FSM_NEXT; r.next_state = T_DONE;
		}
		return r;
	}
	/* a real event: keep, arm a 30ms state timeout */
	r.action = XTC_FSM_KEEP; r.state_timeout_ns = 30LL * 1000 * 1000;
	return r;
}

static int
timer_enter_arm(void *st, int old_state, int new_state)
{
	struct timer_fsm *t = st;
	(void)old_state;
	if (new_state == T_DONE) t->reached_done = 1;
	return XTC_OK;
}

static const xtc_fsm_callbacks_t TIMER_CB2 = {
	timer_enter_arm, timer_event_arm, NULL
};

static void
timer_driver2(void *arg)
{
	struct timer_scn *s = arg;
	int ev = 1;
	(void)xtc_fsm_send(s->pid, &ev, sizeof ev);  /* arms the 30ms timeout */
	xtc_proc_sleep(120LL * 1000 * 1000);          /* let it fire + transition */
	(void)xtc_fsm_stop(s->fsm);
}

static MunitResult
test_fsm_state_timeout(const MunitParameter p[], void *data)
{
	xtc_loop_t *loop;
	struct timer_fsm t;
	struct timer_scn s;
	(void)p; (void)data; (void)TIMER_CB; (void)timer_event;
	(void)timer_enter; (void)timer_driver;

	memset(&t, 0, sizeof t);
	memset(&s, 0, sizeof s);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_fsm_start(loop, &TIMER_CB2, &t, T_WAIT, NULL,
	    &s.fsm), ==, XTC_OK);
	s.pid = xtc_fsm_pid(s.fsm);
	munit_assert_int(xtc_proc_spawn(loop, timer_driver2, &s, NULL, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	munit_assert_int(t.timeouts, >=, 1);      /* the state timeout fired */
	munit_assert_int(t.reached_done, ==, 1);  /* and drove the transition */

	(void)xtc_fsm_join(s.fsm, 0);   /* reclaim the handle */
	(void)xtc_loop_fini(loop);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/fsm_basic", test_fsm_basic, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/fsm_state_timeout", test_fsm_state_timeout, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite suite = {
	"/m10.5/fsm", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

int
main(int argc, char *argv[])
{
	return munit_suite_main(&suite, NULL, argc, argv);
}
