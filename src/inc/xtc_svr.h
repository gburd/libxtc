/*-
 * Copyright (c) 2026, The XTC Project
 * @stability stable
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
 *	Modeled on Erlang's gen_server.  The xtc_svr_call_t handle ties
 *	a synchronous reply back to its caller (a tagged message to a
 *	calling proc, a reply slot for a plain-thread caller), so the
 *	caller can wait for the reply with a timeout.
 */

#ifndef XTC_SVR_H
#define XTC_SVR_H

#include "xtc_export.h"

#include <stddef.h>
#include <stdint.h>

#include "xtc.h"
#include "xtc_chan.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_sync.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Result codes for handle_call / handle_cast / handle_info: the
 * callback may either keep running (XTC_SVR_CONTINUE) or request
 * the server to stop (XTC_SVR_STOP).  handle_call may also defer its
 * reply (XTC_SVR_NOREPLY): it saved the call with xtc_svr_call_save
 * and will xtc_svr_reply later. */
#define XTC_SVR_CONTINUE   0
#define XTC_SVR_STOP       1
#define XTC_SVR_NOREPLY    2

typedef struct xtc_svr     xtc_svr_t;
typedef struct xtc_svr_call xtc_svr_call_t;

typedef struct xtc_svr_callbacks {
	size_t size;        /* sizeof(xtc_svr_callbacks_t): XTC_SVR_CALLBACKS_INIT */
	int  (*init)        (void *state);                              /* OK to be NULL */
	int  (*handle_call) (void *state, const void *req, size_t req_size,
	                     xtc_svr_call_t *call);                     /* required if calls used */
	int  (*handle_cast) (void *state, const void *msg, size_t size); /* OK to be NULL */
	int  (*handle_info) (void *state, const void *msg, size_t size); /* OK to be NULL */
	/* Runs before the next recv when a callback armed a continuation via
	 * xtc_svr_continue(cont); `cont` is that argument.  Lets init (or a
	 * handler) return fast -- unblocking the caller / the supervisor --
	 * and finish expensive work off the critical path, race-free (no
	 * self-send that could race an incoming message).  A handle_continue
	 * may itself arm another continuation.  OK to be NULL (an armed
	 * continuation with no handler is dropped). */
	void (*terminate)   (void *state, int reason);                  /* OK to be NULL */
	int  (*handle_continue)(void *state, void *cont);               /* OK to be NULL */
} xtc_svr_callbacks_t;

/* Set the callbacks with designated initializers after it:
 * { .size = sizeof(xtc_svr_callbacks_t), .handle_call = f }. */
#define XTC_SVR_CALLBACKS_INIT { .size = sizeof(xtc_svr_callbacks_t) }

typedef struct xtc_svr_opts {
	size_t      size;          /* sizeof(xtc_svr_opts_t): XTC_SVR_OPTS_INIT */
	const char *name;          /* optional, for logs */
	size_t      mailbox_cap;   /* 0 = default */
	/*
	 * 2.2: the xtc_proc_opts_t knobs, for the server's proc (see
	 * xtc_proc_opts_t for their meaning), so a server need not set them
	 * on itself in init.  sched_class must have been created on the
	 * server's loop.
	 */
	void (*mailbox_watermark_fn)(xtc_pid_t self, size_t depth, size_t cap,
	    void *user);
	void *mailbox_watermark_user;
	xtc_exec_class_t sched_class; /* NULL = the plain FIFO class */
	int mailbox_watermark_pct;    /* 0 = off, 1..100 */
	/*
	 * 2.2: 1 = xtc_svr_start returns only after init has run, with
	 * init's code: on failure the server is gone, *out is untouched, and
	 * no join is needed.  Legal from a proc (it parks) or from a thread
	 * while another thread runs the server's loop; anywhere else (a
	 * thread that runs the loop itself, a loop nobody runs yet)
	 * XTC_E_STATE, since init could never run.  0 = asynchronous.
	 */
	int wait_init;
} xtc_svr_opts_t;

#define XTC_SVR_OPTS_INIT { .size = sizeof(xtc_svr_opts_t) }

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
 * PUBLIC: int       xtc_svr_continue __P((void *));
 * PUBLIC: xtc_svr_call_t *xtc_svr_call_save __P((const xtc_svr_call_t *));
 */

/* Start is asynchronous unless opts->wait_init: xtc_svr_start spawns
 * the server and returns before init runs.  A failed init runs terminate
 * and ends the server with xtc_exit_self(init's code): a monitor sees
 * DOWN kind EXIT with that code (2.2; before, the DOWN was CLEAN). */
XTC_API int       xtc_svr_start(xtc_loop_t *loop,
                                const xtc_svr_callbacks_t *cb,
                                void *state,
                                const xtc_svr_opts_t *opts,
                                xtc_svr_t **out);

XTC_API int       xtc_svr_stop(xtc_svr_t *svr);
XTC_API int       xtc_svr_join(xtc_svr_t *svr, int64_t timeout_ns);
XTC_API xtc_pid_t xtc_svr_pid(const xtc_svr_t *svr);

/* Synchronous call: send `req`, wait for reply, copy reply into a
 * heap-allocated buffer that the caller must xtc_free.  Callable from a
 * proc or from a plain thread.  Returns:
 *   XTC_OK          -- *out_reply / *out_size populated
 *   XTC_E_AGAIN     -- timeout
 *   XTC_E_NOTFOUND  -- the server exited before replying (noticed at
 *                      once, any timeout including -1: an in-proc caller
 *                      monitors the server for the call and demonitors
 *                      it after, a plain thread is woken by the server's
 *                      exit; 2.2 -- through 2.1 a 25 ms liveness poll)
 *   XTC_E_INVAL     -- bad pid / not a server / server already gone
 *   XTC_E_STATE     -- a waiting call off a proc on the simulator's own
 *                      thread (an xtc_sim_external_complete delivery fn),
 *                      where the server cannot run while it waits
 * A timed-out, aborted or server-gone call only stops the CALLER
 * waiting.  A reply the server sends afterwards to an off-proc caller is
 * discarded (that xtc_svr_reply returns XTC_E_NOTFOUND); to an in-proc
 * caller it is delivered to the caller's mailbox as an internally framed
 * reply envelope (never confused with an application message), which the
 * caller's next xtc_svr_call discards; a proc that makes no further call
 * can still see it with a plain xtc_recv. */
XTC_API int xtc_svr_call(xtc_pid_t target,
                         const void *req, size_t req_size,
                         void **out_reply, size_t *out_size,
                         int64_t timeout_ns);

/* Like xtc_svr_call, but cancellable: while waiting for the reply the
 * abort token is polled, and the call returns XTC_E_ABORTED if it
 * fires first.  Fire the token's source (xtc_abort_source_fire) from
 * a timeout or a cancel-request path -- the cooperative cancellation
 * primitive (e.g. a statement timeout delivering a cancel at the next
 * wait point).  Cancellation stops only the caller's wait; the
 * server keeps processing, and a late reply is handled as for a
 * timed-out xtc_svr_call. */
XTC_API int xtc_svr_call_abortable(xtc_pid_t target,
                                   const void *req, size_t req_size,
                                   void **out_reply, size_t *out_size,
                                   int64_t timeout_ns, xtc_abort_token_t *tok);

/* Fire-and-forget: send `msg` to the server.  Server's handle_cast
 * (if non-NULL) will see it; if NULL, falls through to handle_info. */
XTC_API int xtc_svr_cast(xtc_pid_t target, const void *msg, size_t size);

/* From inside handle_call, send the reply and release the call.
 * Each call must be replied exactly once. */
XTC_API int xtc_svr_reply(xtc_svr_call_t *call,
                          const void *reply, size_t size);

/*
 * Arm a continuation from within a server callback (init or a handle_*):
 * handle_continue(state, cont) runs before the server's next recv.  Use
 * it to return from init quickly (unblocking the supervisor / the
 * xtc_svr_start caller) and finish expensive setup before the first
 * message, without the self-send that races an incoming message.
 * Returns XTC_E_INVAL if called outside a server callback.
 */
XTC_API int xtc_svr_continue(void *cont);

/*
 * A server as a supervised child (2.2).  Name xtc_svr_child_main as a
 * child spec's fn and a const xtc_svr_child_t * as its arg: the child
 * proc IS the server, so a crash restarts it, a drain reaches it, and
 * there is no xtc_svr_t to join (the proc frees its own state on every
 * exit).  Each incarnation runs init on the same `state`.
 *
 *   - XTC_APP_SHUTDOWN_MSG (an xtc_app drain) is handed to handle_info
 *     like any other message, so the server can finish and return
 *     XTC_SVR_STOP; a server with no handle_info stops at once.
 *   - init's failure ends the incarnation with xtc_exit_self(init's code),
 *     a failure a TRANSIENT spec restarts.  A supervisor started with
 *     wait_children (xtc_orc.h) waits for init and fails its start with
 *     that code.
 *   - Clients find the server by the spec's reg_name: xtc_svr_call_name,
 *     or xtc_reg_whereis_wait across a restart.
 *
 * The xtc_svr_child_t and its callbacks must outlive the supervisor.
 *
 * PUBLIC: void      xtc_svr_child_main __P((void *));
 */
typedef struct xtc_svr_child {
	size_t size; /* sizeof(xtc_svr_child_t) */
	const xtc_svr_callbacks_t *cb;
	void *state;
} xtc_svr_child_t;

#define XTC_SVR_CHILD_INIT { .size = sizeof(xtc_svr_child_t) }

XTC_API void xtc_svr_child_main(void *arg);

/*
 * Deferred reply (gen_server:reply/2).  The xtc_svr_call_t passed to
 * handle_call is valid only for the duration of that callback.  To
 * reply LATER -- after a batch fills, after other shards answer, after
 * a timer fires -- call xtc_svr_call_save() inside handle_call to get
 * a heap-allocated handle that outlives the callback, stash it in the
 * server state, return XTC_SVR_NOREPLY, and call xtc_svr_reply() on
 * the saved handle from any later callback.  xtc_svr_reply frees a
 * saved handle after sending.  Each saved handle must be replied
 * exactly once.
 *
 * Safe for in-proc callers (the reply routes to the caller's mailbox
 * by tag, so it is harmless even if the caller has gone).  For an
 * off-proc caller the reply targets the caller's stack-resident reply
 * slot, so the caller must remain blocked in xtc_svr_call (not time
 * out) until the deferred reply is sent.
 */
XTC_API xtc_svr_call_t *xtc_svr_call_save(const xtc_svr_call_t *call);

#ifdef __cplusplus
}
#endif

#endif /* XTC_SVR_H */
