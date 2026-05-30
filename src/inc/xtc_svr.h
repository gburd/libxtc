/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_svr.h
 *	The L4 gen_server: a structured pattern for a long-running
 *	process that handles three kinds of incoming traffic:
 *
 *	  - call:  a synchronous request expecting a reply.
 *	  - cast:  a fire-and-forget command.
 *	  - info:  any other message that lands in the mailbox
 *	           (timer ticks, monitor DOWNs, raw sends, etc.)
 *
 *	The server runs as an xtc_proc.  Behaviour is supplied via a
 *	vtable of callbacks.  All callbacks run in the server's
 *	process; they may use any xtc_proc / xtc_sync API.
 *
 *	Modeled on Erlang's gen_server.  The xtc_call_t handle ties
 *	a synchronous reply back to its caller via xtc_chan_oneshot
 *	so the caller can park on the reply with a timeout.
 */

#ifndef XTC_SVR_H
#define XTC_SVR_H

#include <stddef.h>
#include <stdint.h>

#include "xtc.h"
#include "xtc_chan.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_sync.h"

/* Result codes for handle_call / handle_cast / handle_info: the
 * callback may either keep running (XTC_SVR_CONTINUE) or request
 * the server to stop (XTC_SVR_STOP). */
#define XTC_SVR_CONTINUE   0
#define XTC_SVR_STOP       1

typedef struct xtc_svr     xtc_svr_t;
typedef struct xtc_svr_call xtc_svr_call_t;

typedef struct xtc_svr_callbacks {
	int  (*init)        (void *state);                              /* OK to be NULL */
	int  (*handle_call) (void *state, const void *req, size_t req_size,
	                     xtc_svr_call_t *call);                     /* required if calls used */
	int  (*handle_cast) (void *state, const void *msg, size_t size); /* OK to be NULL */
	int  (*handle_info) (void *state, const void *msg, size_t size); /* OK to be NULL */
	void (*terminate)   (void *state, int reason);                  /* OK to be NULL */
} xtc_svr_callbacks_t;

typedef struct xtc_svr_opts {
	const char *name;          /* optional, for logs */
	size_t      mailbox_cap;   /* 0 = default */
} xtc_svr_opts_t;

/*
 * PUBLIC: int       xtc_svr_start __P((xtc_loop_t *, const xtc_svr_callbacks_t *, void *, const xtc_svr_opts_t *, xtc_svr_t **));
 * PUBLIC: int       xtc_svr_stop __P((xtc_svr_t *));
 * PUBLIC: int       xtc_svr_join __P((xtc_svr_t *, int64_t));
 * PUBLIC: xtc_pid_t xtc_svr_pid __P((const xtc_svr_t *));
 *
 * PUBLIC: int       xtc_svr_call __P((xtc_pid_t, const void *, size_t, void **, size_t *, int64_t));
 * PUBLIC: int       xtc_svr_call_abortable __P((xtc_pid_t, const void *, size_t, void **, size_t *, int64_t, xtc_abort_token_t *));
 * PUBLIC: int       xtc_svr_cast __P((xtc_pid_t, const void *, size_t));
 * PUBLIC: int       xtc_svr_reply __P((xtc_svr_call_t *, const void *, size_t));
 */

int       xtc_svr_start(xtc_loop_t *loop,
                        const xtc_svr_callbacks_t *cb,
                        void *state,
                        const xtc_svr_opts_t *opts,
                        xtc_svr_t **out);

int       xtc_svr_stop(xtc_svr_t *svr);
int       xtc_svr_join(xtc_svr_t *svr, int64_t timeout_ns);
xtc_pid_t xtc_svr_pid(const xtc_svr_t *svr);

/* Synchronous call: send `req`, wait for reply, copy reply into a
 * heap-allocated buffer that the caller must xtc_free.  Returns:
 *   XTC_OK          -- *out_reply / *out_size populated
 *   XTC_E_AGAIN     -- timeout
 *   XTC_E_INVAL     -- bad pid / not a server
 */
int xtc_svr_call(xtc_pid_t target,
                 const void *req, size_t req_size,
                 void **out_reply, size_t *out_size,
                 int64_t timeout_ns);

/* Like xtc_svr_call, but cancellable: while waiting for the reply the
 * abort token is polled, and the call returns XTC_E_ABORTED if it
 * fires first.  Fire the token's source (xtc_abort_source_fire) from
 * a timeout or a cancel-request path -- the cooperative cancellation
 * primitive (e.g. a statement timeout delivering a cancel at the next
 * wait point).  Cancellation stops only the caller's wait; the
 * server keeps processing and a late reply is discarded. */
int xtc_svr_call_abortable(xtc_pid_t target,
                           const void *req, size_t req_size,
                           void **out_reply, size_t *out_size,
                           int64_t timeout_ns, xtc_abort_token_t *tok);

/* Fire-and-forget: send `msg` to the server.  Server's handle_cast
 * (if non-NULL) will see it; if NULL, falls through to handle_info. */
int xtc_svr_cast(xtc_pid_t target, const void *msg, size_t size);

/* From inside handle_call, send the reply and release the call.
 * Each call must be replied exactly once. */
int xtc_svr_reply(xtc_svr_call_t *call,
                  const void *reply, size_t size);

#endif /* XTC_SVR_H */
