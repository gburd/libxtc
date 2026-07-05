/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * src/inc/xtc_osproc.h
 *	OS-process spawn + control socket + lifecycle (R3).
 *
 *	This is the "default-to-fork extension tier" backbone: a way to
 *	run work in a separate OS process -- either an exec'd program or
 *	a forked callback -- with a control channel back to the spawning
 *	loop and non-blocking lifecycle management (signal, reap, wait).
 *	Distinct from xtc_proc_spawn, which spawns an in-process fiber.
 *	Use this tier for un-cooperative or untrusted work that must not
 *	run in a loop thread (no in-thread forcible unwind exists), and
 *	for the classic one-backend-per-connection isolation model.
 *
 *	PLATFORM: POSIX only (fork/exec + pidfd).  On Windows every entry
 *	point is present but returns XTC_E_NOSYS (CreateProcess + a
 *	control pipe is a future port); callers must handle the decline.
 *
 *	Control socket
 *	  When opts.ctrl_socket is set, spawn creates an AF_UNIX
 *	  SOCK_STREAM socketpair.  The PARENT end is returned by
 *	  xtc_osproc_ctrl_fd(): it is O_NONBLOCK and O_CLOEXEC and is
 *	  pollable with xtc_proc_wait_fd / wrappable with xtc_net, so the
 *	  spawning fiber can exchange control messages (framed via
 *	  xtc_net_send_frame/recv_frame) without blocking its loop.  The
 *	  CHILD end is NOT close-on-exec: a forked callback receives it as
 *	  the fn(ctrl_fd, ...) argument; an exec'd program inherits it and
 *	  finds its number in the environment variable XTC_CTRL_FD.
 *
 *	Exit notification
 *	  On Linux (>= 5.3) spawn opens a pidfd, so xtc_osproc_wait parks
 *	  the caller on a readable fd and reaps in O(1) with no SIGCHLD
 *	  handler.  On platforms without pidfd, wait falls back to a
 *	  cooperative waitpid(WNOHANG) poll (it yields/sleeps between
 *	  probes, so it never blocks the loop) -- a kqueue EVFILT_PROC
 *	  fast path is a future optimisation.  Either way the library
 *	  installs no process-wide SIGCHLD handler, so it never collides
 *	  with an embedder's own signal handling.
 *
 *	fork() in a threaded process
 *	  An xtc executor has multiple loop + offload-pool threads.  fork
 *	  duplicates only the calling thread; locks held by other threads
 *	  are frozen in the child.  The exec path is therefore safe (the
 *	  child only does async-signal-safe work, then execs a fresh
 *	  image).  The fn (fork-only) path runs the callback in that
 *	  half-initialised child: the callback must restrict itself to
 *	  async-signal-safe work until it re-initialises its own runtime.
 */

#ifndef XTC_OSPROC_H
#define XTC_OSPROC_H

#include "xtc.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct xtc_osproc xtc_osproc_t;

typedef struct xtc_osproc_opts {
	const char  *name;            /* label for logs/debug; may be NULL */

	/* Exactly one of argv / fn must be set.
	 *
	 * argv != NULL: the child execvp(argv[0], argv)s a program.
	 * fn   != NULL: the child runs fn(ctrl_fd, arg) and _exit()s with
	 *               its return value (0..255).  ctrl_fd is the child's
	 *               control-socket end, or -1 if ctrl_socket == 0. */
	char *const *argv;
	int        (*fn)(int ctrl_fd, void *arg);
	void        *arg;

	int          ctrl_socket;     /* 1: create the control socketpair */
} xtc_osproc_opts_t;

/*
 * Spawn an OS process.  Callable from a loop fiber; never blocks the
 * loop.  Returns XTC_OK and *out on success; XTC_E_INVAL (bad opts:
 * neither or both of argv/fn), XTC_E_NOMEM, or XTC_E_INTERNAL
 * (fork/socketpair/exec setup failed).
 *
 * PUBLIC: int  xtc_osproc_spawn __P((const xtc_osproc_opts_t *, xtc_osproc_t **));
 */
int  xtc_osproc_spawn(const xtc_osproc_opts_t *opts, xtc_osproc_t **out);

/* The child's OS process id, or -1.
 * PUBLIC: long xtc_osproc_pid __P((const xtc_osproc_t *)); */
long xtc_osproc_pid(const xtc_osproc_t *p);

/* The parent end of the control socket (O_NONBLOCK, O_CLOEXEC), or -1
 * if no control socket was requested.  Owned by the handle; do not
 * close it directly -- xtc_osproc_destroy does.
 * PUBLIC: int  xtc_osproc_ctrl_fd __P((const xtc_osproc_t *)); */
int  xtc_osproc_ctrl_fd(const xtc_osproc_t *p);

/* Send signal `sig` to the child (e.g. SIGTERM graceful, SIGKILL hard,
 * SIGINT cancel).  Returns XTC_OK, XTC_E_INVAL, or XTC_E_INTERNAL.
 * PUBLIC: int  xtc_osproc_signal __P((const xtc_osproc_t *, int)); */
int  xtc_osproc_signal(const xtc_osproc_t *p, int sig);

/* Non-blocking reap.  If the child has exited, returns XTC_OK and (when
 * status != NULL) stores the raw waitpid status; if still running,
 * returns XTC_E_AGAIN.  After XTC_OK the child is reaped (no zombie).
 * PUBLIC: int  xtc_osproc_try_wait __P((xtc_osproc_t *, int *)); */
int  xtc_osproc_try_wait(xtc_osproc_t *p, int *status);

/* Cooperative wait: park the calling fiber until the child exits (or
 * timeout_ns elapses; < 0 = forever), reap it, and store the raw
 * waitpid status in *status.  Never blocks the loop thread.  Returns
 * XTC_OK (exited), XTC_E_AGAIN (timeout), or XTC_E_*.  Must be called
 * from a loop fiber (it parks); off a fiber it polls-and-sleeps.
 * PUBLIC: int  xtc_osproc_wait __P((xtc_osproc_t *, int *, int64_t)); */
int  xtc_osproc_wait(xtc_osproc_t *p, int *status, int64_t timeout_ns);

/* Close the control socket + pidfd and free the handle.  Does NOT kill
 * or reap a still-running child (signal + wait first for a clean
 * shutdown); if the child already exited it is reaped here so it does
 * not linger as a zombie.
 * PUBLIC: void xtc_osproc_destroy __P((xtc_osproc_t *)); */
void xtc_osproc_destroy(xtc_osproc_t *p);

/* ---- isolated-worker ergonomics ------------------------------------
 *
 * The request/reply convenience over the control socket: real MMU
 * isolation (a separate OS process) with a simple framed call.  The
 * parent spawns a worker, then xtc_osproc_call() ships a request frame
 * and parks for the reply (never blocking the loop).  The child runs
 * xtc_osproc_serve(), which loops handling framed requests until the
 * socket closes.
 */

/* Spawn a forked isolated worker running fn(ctrl_fd, arg), with a
 * control socket always created.  Equivalent to xtc_osproc_spawn with
 * opts{.name, .fn, .arg, .ctrl_socket=1}.
 *
 * PUBLIC: int xtc_osproc_isolated_spawn __P((const char *, int (*)(int, void *), void *, xtc_osproc_t **));
 */
int xtc_osproc_isolated_spawn(const char *name,
                              int (*fn)(int ctrl_fd, void *arg), void *arg,
                              xtc_osproc_t **out);

/* Parent side: send a request frame to the worker and park for its
 * reply frame (allocated with the xtc allocator; free with xtc_free).
 * max_reply caps the reply (0 = uncapped).  Returns XTC_OK, XTC_E_AGAIN
 * on timeout, or XTC_E_*.  Requires a control socket.
 *
 * PUBLIC: int xtc_osproc_call __P((xtc_osproc_t *, const void *, size_t, void **, size_t *, size_t, int64_t));
 */
int xtc_osproc_call(xtc_osproc_t *p, const void *req, size_t req_len,
                    void **reply, size_t *reply_len, size_t max_reply,
                    int64_t timeout_ns);

/* Child-side handler: given a request, produce a reply.  Set *reply
 * (malloc'd, or NULL for an empty reply) and *reply_len; return XTC_OK
 * to send it and keep serving, or non-XTC_OK to stop.  xtc_osproc_serve
 * frees *reply after sending. */
typedef int (*xtc_osproc_handler_fn)(const void *req, size_t req_len,
                                     void **reply, size_t *reply_len,
                                     void *arg);

/* Child side: serve framed requests on ctrl_fd until the peer closes
 * (or a handler stops it).  Returns the handler's stop code, or XTC_OK
 * on clean EOF.
 *
 * PUBLIC: int xtc_osproc_serve __P((int, xtc_osproc_handler_fn, void *));
 */
int xtc_osproc_serve(int ctrl_fd, xtc_osproc_handler_fn handler, void *arg);

#ifdef __cplusplus
}
#endif

#endif /* XTC_OSPROC_H */
