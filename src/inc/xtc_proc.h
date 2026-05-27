/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_proc.h
 *	BEAM-style processes with mailboxes, selective receive,
 *	links, monitors, and explicit exit.  M8 ships the core; the
 *	`xtc_orc` supervisor (M10) sits on top of these primitives.
 *
 *	A process is a coroutine with identity (xtc_pid_t) plus a
 *	mailbox.  Send is fire-and-forget; the message is copied into
 *	an envelope owned by the mailbox.  Receive is selective: the
 *	caller supplies a match function, and envelopes that don't
 *	match are kept in arrival order in a save queue and re-tested
 *	on the next receive.
 */

#ifndef XTC_PROC_H
#define XTC_PROC_H

#include <stddef.h>
#include <stdint.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_async.h"

/*
 * Process identifier.  Encodes the loop ID, a per-loop slot index,
 * and a generation counter so a stale pid (after a process exits and
 * its slot is reused) is recognisably stale on lookup.
 */
typedef struct xtc_pid {
	uint16_t loop_id;
	uint16_t local_id;
	uint32_t gen;
} xtc_pid_t;

#define XTC_PID_NONE ((xtc_pid_t){0, 0, 0})

static inline int
xtc_pid_eq(xtc_pid_t a, xtc_pid_t b)
{
	return a.loop_id == b.loop_id &&
	       a.local_id == b.local_id &&
	       a.gen == b.gen;
}

static inline int
xtc_pid_is_none(xtc_pid_t p)
{
	return p.loop_id == 0 && p.local_id == 0 && p.gen == 0;
}

/*
 * Match callback for selective receive.  Inspects an envelope's
 * data + size and returns:
 *   1  -> consume this envelope (the receive call returns it)
 *   0  -> skip; envelope stays in the save queue for the next receive
 */
typedef int (*xtc_match_fn)(const void *data, size_t size, void *user_data);

/*
 * Process entry function.  The proc runs as a coroutine and the
 * function returns nothing; exit happens by returning from the entry,
 * by calling xtc_exit, or by being killed via a link.
 */
typedef void (*xtc_proc_fn)(void *arg);

typedef struct xtc_proc_opts {
	const char *name;          /* optional, for debug */
	size_t      mailbox_cap;   /* 0 = default */
	int         link_to;       /* if != 0, this is a pid index to link to */
} xtc_proc_opts_t;

/*
 * PUBLIC: int       xtc_proc_spawn __P((xtc_loop_t *, xtc_proc_fn, void *, const xtc_proc_opts_t *, xtc_pid_t *));
 * PUBLIC: xtc_pid_t xtc_self __P((void));
 * PUBLIC: int       xtc_send __P((xtc_pid_t, const void *, size_t));
 * PUBLIC: int       xtc_recv __P((void **, size_t *, int64_t));
 * PUBLIC: int       xtc_recv_match __P((xtc_match_fn, void *, void **, size_t *, int64_t));
 * PUBLIC: int       xtc_exit_self __P((int));
 * PUBLIC: int       xtc_exit_pid __P((xtc_pid_t, int));
 * PUBLIC: int       xtc_link __P((xtc_pid_t));
 * PUBLIC: int       xtc_unlink __P((xtc_pid_t));
 * PUBLIC: int       xtc_monitor __P((xtc_pid_t, uint64_t *));
 */

int       xtc_proc_spawn(xtc_loop_t *loop, xtc_proc_fn fn, void *arg,
                          const xtc_proc_opts_t *opts, xtc_pid_t *out_pid);

/* From inside a process, return its pid; from outside, returns NONE. */
/*
 * Asynchronous cross-process exit signal.  Sets a kill flag on the
 * target proc; the target raises the exit at its next yield/recv
 * point with the supplied reason.  Idempotent (first call wins).
 * Returns XTC_E_INVAL if the target is unknown or already dead.
 */
int xtc_exit_pid(xtc_pid_t target, int reason);

xtc_pid_t xtc_self(void);

/*
 * Send a message.  Copies `size` bytes from `data` into a mailbox
 * envelope; the caller retains ownership of `data`.  Returns:
 *   XTC_OK            queued successfully
 *   XTC_E_INVAL       NULL data with non-zero size, or stale/unknown pid
 *   XTC_E_AGAIN       target mailbox at capacity
 *   XTC_E_RESOURCE    global slot cap (XTC_RES_CHAN_SLOTS) hit
 */
int       xtc_send(xtc_pid_t to, const void *data, size_t size);

/*
 * Receive the next envelope from this process's mailbox.  Allocates
 * a new buffer for the caller via __os_malloc; the caller frees with
 * __os_free.  Blocks (yields the coroutine) up to timeout_ns; -1 is
 * indefinite, 0 is non-blocking.
 *
 * Returns:
 *   XTC_OK            *out / *size set
 *   XTC_E_AGAIN       timeout fired with no message
 *   XTC_E_INVAL       called outside a process
 */
int       xtc_recv(void **out, size_t *out_size, int64_t timeout_ns);

/*
 * Selective receive.  match_fn is called for each envelope in
 * arrival order; the first one for which match_fn returns 1 is
 * delivered.  Non-matching envelopes are kept in the save queue.
 */
int       xtc_recv_match(xtc_match_fn match_fn, void *user_data,
                          void **out, size_t *out_size,
                          int64_t timeout_ns);

/* Explicit exit from inside a process; reason is delivered via
 * EXIT/DOWN signals to linked / monitoring procs. */
int       xtc_exit_self(int reason);

/* Link / unlink: bidirectional fate. */
int       xtc_link(xtc_pid_t other);
int       xtc_unlink(xtc_pid_t other);

/* Monitor: unidirectional notification.  out_ref is filled with the
 * monitor reference; the watcher receives a DOWN message of shape
 * { uint8_t kind = 'D'; uint64_t ref; xtc_pid_t pid; int reason; }
 * when the monitored process exits. */
int       xtc_monitor(xtc_pid_t target, uint64_t *out_ref);

#endif /* XTC_PROC_H */
