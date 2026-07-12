/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/orc/fsm.c
 *	L4 gen_statem: a finite state machine running as an xtc_proc.
 *	See src/inc/xtc_fsm.h for the contract.  The machine is a
 *	disciplined loop over xtc_recv that dispatches each event to the
 *	user event() callback for the current state and acts on the
 *	returned xtc_fsm_result_t: KEEP / NEXT (with enter() + postponed
 *	replay) / POSTPONE (stash + replay after the next NEXT) / STOP.
 *
 *	Wire format (byte 0 = kind), mirroring xtc_svr's minimal scheme:
 *	  'E' <payload>                       async event  (xtc_fsm_send)
 *	  'C' <reply_pid:8><tag:4><payload>   sync call     (xtc_fsm_call)
 *	  'S'                                 stop kick     (xtc_fsm_stop)
 *	A call reply is a plain message to reply_pid: <tag:4><payload>,
 *	which xtc_fsm_call correlates.
 */

#include "xtc_int.h"
#include "xtc_fsm.h"
#include "xtc_orc.h"
#include "xtc_sync.h"
#include "xtc_inspect.h"   /* xtc_proc_info: used by xtc_fsm_join */

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

/* A stashed (postponed) event: the raw decoded payload, replayed after
 * the next state transition.  Sync calls are NOT postponable (a caller
 * is blocked waiting) -- POSTPONE of a call replies XTC_E_AGAIN and is
 * not stashed; only async events postpone. */
struct fsm_postponed {
	struct fsm_postponed *next;
	size_t                len;
	/* payload bytes follow inline */
};

struct xtc_fsm {
	xtc_pid_t             pid;
	_Atomic int           stop_requested;
	int                   joined;
};

/* Per-call reply handle handed to event(); the caller of xtc_fsm_call
 * blocks on a tagged recv, so reply just sends <tag><payload> back. */
struct xtc_fsm_call {
	xtc_pid_t reply_pid;
	uint32_t  reply_tag;
	int       replied;
};

/* The running machine's private state, owned by its proc fiber. */
struct fsm_run {
	const xtc_fsm_callbacks_t *cb;
	void                      *state;
	int                        cur_state;
	struct xtc_fsm            *handle;   /* shared stop flag */
	struct fsm_postponed      *pq_head;
	struct fsm_postponed      *pq_tail;
	/* a pending state timeout deadline, or -1 if none */
	int64_t                    timeout_ns;   /* relative, armed per result */
};

/* ---- postponed-event queue -------------------------------------- */

static int
fsm_postpone(struct fsm_run *r, const void *payload, size_t len)
{
	struct fsm_postponed *p;
	if (__os_malloc(sizeof(*p) + len, (void **)&p) != XTC_OK)
		return XTC_E_NOMEM;
	p->next = NULL;
	p->len = len;
	if (len > 0)
		memcpy((uint8_t *)p + sizeof(*p), payload, len);
	if (r->pq_tail != NULL)
		r->pq_tail->next = p;
	else
		r->pq_head = p;
	r->pq_tail = p;
	return XTC_OK;
}

static void
fsm_pq_free(struct fsm_run *r)
{
	struct fsm_postponed *p, *n;
	for (p = r->pq_head; p != NULL; p = n) {
		n = p->next;
		__os_free(p);
	}
	r->pq_head = r->pq_tail = NULL;
}

/* ---- dispatch one decoded event to the callback ----------------- */

/*
 * Apply the result of an event() call.  Returns 1 to keep running,
 * 0 to stop.  On a NEXT transition runs enter() then replays the
 * postponed queue (oldest first) in the new state until it drains or
 * a replay itself transitions (which re-queues the remainder).
 */
static int
fsm_apply(struct fsm_run *r, xtc_fsm_result_t res, int *out_stop_reason);

static int
fsm_dispatch(struct fsm_run *r, const void *msg, size_t len,
             struct xtc_fsm_call *call, int *out_stop_reason)
{
	xtc_fsm_result_t res;

	res = r->cb->event(r->state, r->cur_state, msg, len, call);

	/* A sync call that is postponed cannot block the caller forever:
	 * reply XTC_E_AGAIN and drop it (do not stash). */
	if (res.action == XTC_FSM_POSTPONE && call != NULL) {
		if (!call->replied)
			(void)xtc_fsm_reply(call, NULL, 0);
		return 1;
	}
	if (res.action == XTC_FSM_POSTPONE) {
		(void)fsm_postpone(r, msg, len);
		r->timeout_ns = -1;   /* postpone does not arm a timeout */
		return 1;
	}
	return fsm_apply(r, res, out_stop_reason);
}

static int
fsm_apply(struct fsm_run *r, xtc_fsm_result_t res, int *out_stop_reason)
{
	switch (res.action) {
	case XTC_FSM_KEEP:
		r->timeout_ns = res.state_timeout_ns > 0 ? res.state_timeout_ns : -1;
		return 1;

	case XTC_FSM_STOP:
		*out_stop_reason = XTC_FSM_REASON_NORMAL;
		return 0;

	case XTC_FSM_NEXT: {
		int old = r->cur_state;
		struct fsm_postponed *replay, *p, *n;
		r->cur_state = res.next_state;
		if (r->cb->enter != NULL &&
		    r->cb->enter(r->state, old, r->cur_state) != XTC_OK) {
			*out_stop_reason = XTC_FSM_REASON_NORMAL;
			return 0;
		}
		r->timeout_ns = res.state_timeout_ns > 0 ? res.state_timeout_ns : -1;

		/* Replay postponed events (oldest first) in the new state.
		 * Detach the queue first: a replayed event may itself postpone
		 * (re-appending to the now-empty queue) or transition (which
		 * leaves the remaining detached events to be re-queued). */
		replay = r->pq_head;
		r->pq_head = r->pq_tail = NULL;
		for (p = replay; p != NULL; p = n) {
			n = p->next;
			{
				const void *pl = p->len > 0 ?
				    (const uint8_t *)p + sizeof(*p) : NULL;
				int keep = fsm_dispatch(r, pl, p->len, NULL,
				    out_stop_reason);
				__os_free(p);
				if (!keep) {
					/* stopped mid-replay: free the rest */
					for (p = n; p != NULL; p = n) {
						n = p->next;
						__os_free(p);
					}
					return 0;
				}
			}
		}
		return 1;
	}

	default:
		return 1;
	}
}

/* ---- the machine proc ------------------------------------------- */

static void
fsm_entry(void *arg)
{
	struct fsm_run *r = arg;
	int stop_reason = XTC_FSM_REASON_NORMAL;
	int running = 1;

	r->handle->pid = xtc_self();

	/* Initial state entry: enter(initial, initial). */
	if (r->cb->enter != NULL &&
	    r->cb->enter(r->state, r->cur_state, r->cur_state) != XTC_OK)
		running = 0;

	while (running &&
	    !atomic_load_explicit(&r->handle->stop_requested,
	        memory_order_acquire)) {
		void  *msg = NULL;
		size_t size = 0;
		int64_t to;
		int rc;
		uint8_t kind;

		/* Wait for an event.  If a state timeout is armed, bound the
		 * recv by it; on timeout deliver a synthetic state-timeout
		 * event (msg == NULL, len == 0). */
		to = r->timeout_ns > 0 ? r->timeout_ns : 100LL * 1000 * 1000;
		rc = xtc_recv(&msg, &size, to);
		if (rc == XTC_E_AGAIN) {
			if (r->timeout_ns > 0) {
				/* state timeout fired */
				r->timeout_ns = -1;
				running = fsm_dispatch(r, NULL, 0, NULL, &stop_reason);
			}
			continue;
		}
		if (rc != XTC_OK)
			break;
		if (size == 0) { __os_free(msg); continue; }

		kind = ((uint8_t *)msg)[0];

		if (size == 1 && kind == 'S') {   /* stop kick */
			__os_free(msg);
			break;
		}

		if (kind == 'E') {
			const void *pl = size > 1 ? (uint8_t *)msg + 1 : NULL;
			running = fsm_dispatch(r, pl, size - 1, NULL, &stop_reason);
			__os_free(msg);
		} else if (kind == 'C' && size >= 13) {
			struct xtc_fsm_call call;
			const void *pl;
			call.replied = 0;
			memcpy(&call.reply_pid, (uint8_t *)msg + 1, 8);
			memcpy(&call.reply_tag, (uint8_t *)msg + 9, 4);
			pl = size > 13 ? (uint8_t *)msg + 13 : NULL;
			running = fsm_dispatch(r, pl, size - 13, &call, &stop_reason);
			/* A call that the callback neither replied nor postponed
			 * gets an empty reply so the caller does not hang. */
			if (!call.replied)
				(void)xtc_fsm_reply(&call, NULL, 0);
			__os_free(msg);
		} else {
			__os_free(msg);   /* malformed */
		}
	}

	if (r->cb->terminate != NULL)
		r->cb->terminate(r->state, stop_reason);
	fsm_pq_free(r);
	__os_free(r);
}

/* ---- public API ------------------------------------------------- */

int
xtc_fsm_start(xtc_loop_t *loop, const xtc_fsm_callbacks_t *cb, void *state,
              int initial_state, const xtc_fsm_opts_t *opts,
              xtc_fsm_t **out)
{
	struct xtc_fsm *h;
	struct fsm_run *r;
	xtc_proc_opts_t pop;
	xtc_pid_t pid;
	int rc;

	if (loop == NULL || cb == NULL || cb->event == NULL || out == NULL)
		return XTC_E_INVAL;

	if ((rc = __os_calloc(1, sizeof(*h), (void **)&h)) != XTC_OK)
		return rc;
	if ((rc = __os_calloc(1, sizeof(*r), (void **)&r)) != XTC_OK) {
		__os_free(h);
		return rc;
	}
	atomic_store_explicit(&h->stop_requested, 0, memory_order_relaxed);
	h->joined = 0;
	r->cb = cb;
	r->state = state;
	r->cur_state = initial_state;
	r->handle = h;
	r->pq_head = r->pq_tail = NULL;
	r->timeout_ns = -1;

	memset(&pop, 0, sizeof pop);
	if (opts != NULL) {
		pop.name = opts->name;
		pop.mailbox_cap = opts->mailbox_cap;
	}
	rc = xtc_proc_spawn(loop, fsm_entry, r, &pop, &pid);
	if (rc != XTC_OK) {
		__os_free(r);
		__os_free(h);
		return rc;
	}
	/* h->pid is set by fsm_entry via xtc_self(); the spawn out-pid is
	 * the same value and is available synchronously here. */
	h->pid = pid;
	*out = h;
	return XTC_OK;
}

int
xtc_fsm_stop(xtc_fsm_t *fsm)
{
	uint8_t kick = 'S';
	if (fsm == NULL) return XTC_E_INVAL;
	atomic_store_explicit(&fsm->stop_requested, 1, memory_order_release);
	if (!xtc_pid_is_none(fsm->pid))
		(void)xtc_send(fsm->pid, &kick, 1);
	return XTC_OK;
}

int
xtc_fsm_join(xtc_fsm_t *fsm, int64_t timeout_ns)
{
	if (fsm == NULL) return XTC_E_INVAL;
	/* The proc frees `r` on exit; here we just wait until the proc is
	 * gone (its pid no longer resolves to a live proc), then free the
	 * handle.  Reuse xtc_exit_pid semantics via a bounded poll. */
	{
		int64_t waited = 0;
		const int64_t step = 1000LL * 1000;   /* 1ms */
		for (;;) {
			xtc_proc_info_t info;
			if (xtc_proc_info(fsm->pid, &info) != XTC_OK)
				break;   /* gone */
			if (timeout_ns == 0)
				return XTC_E_AGAIN;
			if (timeout_ns > 0 && waited >= timeout_ns)
				return XTC_E_AGAIN;
			xtc_proc_sleep(step);
			waited += step;
		}
	}
	__os_free(fsm);
	return XTC_OK;
}

xtc_pid_t
xtc_fsm_pid(const xtc_fsm_t *fsm)
{
	if (fsm == NULL) return XTC_PID_NONE;
	return fsm->pid;
}

int
xtc_fsm_send(xtc_pid_t target, const void *ev, size_t len)
{
	uint8_t *buf;
	int rc;
	if (len > 0 && ev == NULL) return XTC_E_INVAL;
	if (len > SIZE_MAX - 1) return XTC_E_INVAL;
	if (__os_malloc(len + 1, (void **)&buf) != XTC_OK) return XTC_E_NOMEM;
	buf[0] = 'E';
	if (len > 0) memcpy(buf + 1, ev, len);
	rc = xtc_send(target, buf, len + 1);
	__os_free(buf);
	return rc;
}

int
xtc_fsm_call(xtc_pid_t target, const void *req, size_t req_size,
             void **out_reply, size_t *out_size, int64_t timeout_ns)
{
	static _Atomic uint32_t g_tag = 1;
	uint8_t *buf;
	uint32_t tag;
	xtc_pid_t self;
	size_t msg_size;
	int rc;

	if (req_size > 0 && req == NULL) return XTC_E_INVAL;
	if (req_size > SIZE_MAX - 13) return XTC_E_INVAL;
	self = xtc_self();
	if (xtc_pid_is_none(self)) return XTC_E_INVAL;   /* must call from a proc */

	tag = atomic_fetch_add_explicit(&g_tag, 1, memory_order_relaxed);
	msg_size = 13 + req_size;
	if (__os_malloc(msg_size, (void **)&buf) != XTC_OK) return XTC_E_NOMEM;
	buf[0] = 'C';
	memcpy(buf + 1, &self, 8);
	memcpy(buf + 9, &tag, 4);
	if (req_size > 0) memcpy(buf + 13, req, req_size);
	rc = xtc_send(target, buf, msg_size);
	__os_free(buf);
	if (rc != XTC_OK) return rc;

	/* Wait for a reply tagged with our tag: <tag:4><payload>. */
	{
		int64_t waited = 0;
		const int64_t step = 5LL * 1000 * 1000;
		for (;;) {
			void  *rmsg = NULL;
			size_t rsize = 0;
			int64_t to = timeout_ns < 0 ? step :
			    (timeout_ns - waited < step ? timeout_ns - waited : step);
			if (to <= 0 && timeout_ns >= 0) return XTC_E_AGAIN;
			rc = xtc_recv(&rmsg, &rsize, to);
			if (rc == XTC_E_AGAIN) {
				waited += step;
				if (timeout_ns >= 0 && waited >= timeout_ns)
					return XTC_E_AGAIN;
				continue;
			}
			if (rc != XTC_OK) return rc;
			if (rsize >= 4) {
				uint32_t rtag;
				memcpy(&rtag, rmsg, 4);
				if (rtag == tag) {
					size_t plen = rsize - 4;
					if (out_reply != NULL && plen > 0) {
						void *copy;
						if (__os_malloc(plen, &copy) != XTC_OK) {
							__os_free(rmsg);
							return XTC_E_NOMEM;
						}
						memcpy(copy, (uint8_t *)rmsg + 4, plen);
						*out_reply = copy;
					} else if (out_reply != NULL) {
						*out_reply = NULL;
					}
					if (out_size != NULL) *out_size = plen;
					__os_free(rmsg);
					return XTC_OK;
				}
			}
			/* Not our reply -- drop it (fsm callers do not multiplex
			 * unrelated traffic on the calling proc). */
			__os_free(rmsg);
		}
	}
}

int
xtc_fsm_reply(xtc_fsm_call_t *call, const void *reply, size_t size)
{
	uint8_t *buf;
	size_t msg_size;
	int rc;

	if (call == NULL) return XTC_E_INVAL;
	if (call->replied) return XTC_E_INVAL;
	if (xtc_pid_is_none(call->reply_pid)) { call->replied = 1; return XTC_OK; }
	if (size > SIZE_MAX - 4) return XTC_E_INVAL;

	msg_size = 4 + size;
	if (__os_malloc(msg_size, (void **)&buf) != XTC_OK) return XTC_E_NOMEM;
	memcpy(buf, &call->reply_tag, 4);
	if (size > 0) memcpy(buf + 4, reply, size);
	rc = xtc_send(call->reply_pid, buf, msg_size);
	__os_free(buf);
	call->replied = 1;
	return rc;
}
