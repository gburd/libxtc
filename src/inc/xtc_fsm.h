/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_fsm.h
 *	The L4 gen_statem: a finite state machine that runs as an
 *	xtc_proc.  It is a disciplined loop over xtc_recv: each event
 *	is dispatched to the user's event() callback for the current
 *	state, and the callback's result drives the machine.
 *
 *	Modeled on Erlang's gen_statem.  The three subtle features
 *	gen_statem exists to standardize -- and that hand-rolled C
 *	switch machines almost always botch -- are all provided here:
 *
 *	  - state_enter: enter() is called on every state change
 *	    (old_state -> new_state), so entry actions live in one
 *	    place instead of being scattered across every transition.
 *
 *	  - postponed events: an event that cannot be handled in the
 *	    current state is stashed (XTC_FSM_POSTPONE) and REPLAYED,
 *	    oldest first, after the next successful state transition,
 *	    ahead of any fresh event.  This is the feature most
 *	    hand-rolled FSMs get wrong.
 *
 *	  - state timeouts: a result may arm a per-state timeout
 *	    (state_timeout_ns > 0).  If it fires before any event
 *	    arrives, a synthetic state-timeout event is delivered to
 *	    event() with msg == NULL and len == 0.  Any real event
 *	    cancels a pending state timeout (gen_statem semantics).
 *
 *	The machine is driven by two client entry points: xtc_fsm_send
 *	(asynchronous event, fire-and-forget) and xtc_fsm_call
 *	(synchronous request/reply, reusing the same reply machinery as
 *	xtc_svr).  A call reply is sent by the event() callback via
 *	xtc_fsm_reply (from the reply handle it is given).
 */

#ifndef XTC_FSM_H
#define XTC_FSM_H

#include "xtc_export.h"

#include <stddef.h>
#include <stdint.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"

/*
 * What the event() callback asks the machine to do next:
 *
 *   XTC_FSM_KEEP     stay in the current state (state data may have
 *                    changed).  A state_timeout_ns > 0 (re)arms the
 *                    state timeout in the current state.
 *   XTC_FSM_NEXT     transition to result.next_state.  enter() runs
 *                    for the new state, then any postponed events are
 *                    replayed oldest first.  state_timeout_ns > 0 arms
 *                    a timeout for the new state.
 *   XTC_FSM_POSTPONE stash this event and re-deliver it after the next
 *                    NEXT transition, ahead of fresh events.  The
 *                    machine stays in the current state.
 *   XTC_FSM_STOP     run terminate() and exit the proc.
 */
typedef enum xtc_fsm_action {
	XTC_FSM_KEEP     = 0,
	XTC_FSM_NEXT     = 1,
	XTC_FSM_POSTPONE = 2,
	XTC_FSM_STOP     = 3
} xtc_fsm_action_t;

typedef struct xtc_fsm_result {
	xtc_fsm_action_t action;
	int              next_state;       /* used iff action == XTC_FSM_NEXT */
	int64_t          state_timeout_ns; /* 0 = none; > 0 arms a state
	                                    * timeout for the resulting state */
} xtc_fsm_result_t;

typedef struct xtc_fsm      xtc_fsm_t;
typedef struct xtc_fsm_call xtc_fsm_call_t;

/*
 * A synthetic state-timeout event is delivered to event() with
 * msg == NULL and len == 0.  A machine that uses state timeouts must
 * treat that shape as its timeout signal.
 */
#define XTC_FSM_IS_STATE_TIMEOUT(msg, len) ((msg) == NULL && (len) == 0)

/*
 * Reason codes passed to terminate().  A clean XTC_FSM_STOP uses
 * XTC_FSM_REASON_NORMAL; the machine may pass any nonzero reason of
 * its own by storing it before returning XTC_FSM_STOP (the reason
 * argument is currently always XTC_FSM_REASON_NORMAL for a graceful
 * stop and nonzero if the loop is torn down under it).
 */
#define XTC_FSM_REASON_NORMAL 0

typedef struct xtc_fsm_callbacks {
	/* Called on every state change (old_state -> new_state), including
	 * the initial entry (old_state == new_state == initial_state).
	 * Returns XTC_OK to proceed; any XTC_E_* stops the machine.
	 * OK to be NULL. */
	int (*enter)(void *state, int old_state, int new_state);

	/* Called for each event in the current state.  For a synchronous
	 * xtc_fsm_call, `call` is non-NULL and the callback must satisfy
	 * it exactly once with xtc_fsm_reply; for an asynchronous
	 * xtc_fsm_send (or a synthetic state timeout), `call` is NULL.
	 * Required. */
	xtc_fsm_result_t (*event)(void *state, int cur_state,
	                          const void *msg, size_t len,
	                          xtc_fsm_call_t *call);

	/* Called once when the machine stops.  OK to be NULL. */
	void (*terminate)(void *state, int reason);
} xtc_fsm_callbacks_t;

typedef struct xtc_fsm_opts {
	const char *name;         /* optional, for logs */
	size_t      mailbox_cap;  /* 0 = default */
} xtc_fsm_opts_t;

/*
 * PUBLIC: int       xtc_fsm_start __P((xtc_loop_t *, const xtc_fsm_callbacks_t *, void *, int, const xtc_fsm_opts_t *, xtc_fsm_t **));
 * PUBLIC: int       xtc_fsm_stop __P((xtc_fsm_t *));
 * PUBLIC: int       xtc_fsm_join __P((xtc_fsm_t *, int64_t));
 * PUBLIC: xtc_pid_t xtc_fsm_pid __P((const xtc_fsm_t *));
 * PUBLIC: int       xtc_fsm_send __P((xtc_pid_t, const void *, size_t));
 * PUBLIC: int       xtc_fsm_call __P((xtc_pid_t, const void *, size_t, void **, size_t *, int64_t));
 * PUBLIC: int       xtc_fsm_reply __P((xtc_fsm_call_t *, const void *, size_t));
 */

/*
 * Start a state machine on `loop`.  It runs as its own xtc_proc,
 * beginning in `initial_state` (for which enter() is called before the
 * first event).  The returned handle lets a caller query/stop/join it
 * from outside.
 */
XTC_API int       xtc_fsm_start(xtc_loop_t *loop,
                                const xtc_fsm_callbacks_t *cb,
                                void *state,
                                int initial_state,
                                const xtc_fsm_opts_t *opts,
                                xtc_fsm_t **out);

/* Ask the machine to stop.  Non-blocking; sets a flag and kicks the
 * proc's recv.  terminate() runs, then the proc exits.  Safe from any
 * thread, multiple times. */
XTC_API int       xtc_fsm_stop(xtc_fsm_t *fsm);

/* Wait for the machine to exit, then free its handle.  timeout_ns < 0
 * waits forever, 0 polls once.  On XTC_OK the handle is invalid; on
 * XTC_E_AGAIN (timeout) it is left intact so the caller may join
 * again. */
XTC_API int       xtc_fsm_join(xtc_fsm_t *fsm, int64_t timeout_ns);

/* The machine's pid, for use with xtc_fsm_send / xtc_fsm_call. */
XTC_API xtc_pid_t xtc_fsm_pid(const xtc_fsm_t *fsm);

/* Asynchronous event: deliver `ev` to the machine and return without
 * waiting.  The event lands in event() with call == NULL. */
XTC_API int       xtc_fsm_send(xtc_pid_t target, const void *ev, size_t len);

/* Synchronous event: deliver `req`, block until the machine replies or
 * timeout_ns elapses.  On XTC_OK, *out_reply / *out_size receive a
 * heap buffer the caller must xtc_free.  Returns XTC_E_AGAIN on
 * timeout, XTC_E_INVAL on bad arguments. */
XTC_API int       xtc_fsm_call(xtc_pid_t target,
                               const void *req, size_t req_size,
                               void **out_reply, size_t *out_size,
                               int64_t timeout_ns);

/* From inside event() when `call` is non-NULL, send the reply and
 * release the call handle.  Each call must be replied exactly once. */
XTC_API int       xtc_fsm_reply(xtc_fsm_call_t *call,
                                const void *reply, size_t size);

#endif /* XTC_FSM_H */
