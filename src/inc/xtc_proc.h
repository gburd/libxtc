/*-
 * Copyright (c) 2026, The XTC Project
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
	/* Mailbox watermark: when an accepted message brings the depth to
	 * this percent of mailbox_cap (1..100; 0 = disabled), the callback
	 * fires once on the rising edge, so the app can shed load before
	 * the hard cap rejects with XTC_E_AGAIN.  The callback runs on the
	 * sender's thread, outside the mailbox lock; keep it cheap and do
	 * not block. */
	int         mailbox_watermark_pct;
	void      (*mailbox_watermark_fn)(xtc_pid_t self, size_t depth,
	                                  size_t cap, void *user);
	void       *mailbox_watermark_user;
} xtc_proc_opts_t;

/* Mailbox statistics snapshot (see xtc_proc_mailbox_stats). */
typedef struct xtc_mailbox_stats {
	size_t   depth;        /* messages currently in the mailbox */
	size_t   saved;        /* messages held in the selective-receive
	                        * save queue (inspected, not yet matched) */
	size_t   peak;         /* high-water mailbox depth ever reached */
	size_t   cap;          /* capacity bound on depth + saved (0 = none) */
	uint64_t recv_total;   /* messages accepted over the proc's life */
	uint64_t drop_total;   /* messages rejected (full / dead) */
} xtc_mailbox_stats_t;

/*
 * PUBLIC: int       xtc_proc_spawn __P((xtc_loop_t *, xtc_proc_fn, void *, const xtc_proc_opts_t *, xtc_pid_t *));
 * PUBLIC: xtc_pid_t xtc_self __P((void));
 * PUBLIC: int       xtc_send __P((xtc_pid_t, const void *, size_t));
 * PUBLIC: int       xtc_recv __P((void **, size_t *, int64_t));
 * PUBLIC: int       xtc_recv_match __P((xtc_match_fn, void *, void **, size_t *, int64_t));
 * PUBLIC: int       xtc_recv_correlate __P((const void *, size_t, int, xtc_msg_t *, int *, int64_t));
 * PUBLIC: int       xtc_proc_wait_fd __P((int, uint32_t, int64_t, uint32_t *));
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
 *
 * BACKPRESSURE CONTRACT -- read this.  Mailboxes are bounded (the
 * cap is xtc_proc_opts_t.mailbox_cap, default 4096).  This is
 * deliberate: an unbounded mailbox is how an actor system OOMs when
 * a fast sender outruns a slow receiver (the classic BEAM failure).
 * The price is that send can fail with XTC_E_AGAIN when the target
 * is full, and a dropped XTC_E_AGAIN is a SILENT MESSAGE LOSS.
 *
 * Senders MUST check the return value and decide a policy:
 *   - retry later (re-arm on a timer, or yield and resend),
 *   - shed load (drop the message and account it),
 *   - apply end-to-end flow control (e.g. stop reading the upstream
 *     socket until the target drains -- see examples/07_kaka for a
 *     credit-based scheme), or
 *   - treat it as fatal for a must-deliver path.
 * Ignoring the return is a bug, not a shortcut.
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

/*
 * Receive `n_expected` messages whose leading `corr_size` bytes
 * match `corr_value`.  The first `n_expected` matching messages
 * are delivered as an array via `out_msgs[]`; non-matching
 * messages stay in the mailbox / save queue for subsequent
 * receives.  Returns XTC_OK on full collection; XTC_E_AGAIN if
 * the timeout fires before n_expected matches arrive (in which
 * case `*out_n` is the number actually collected; out_msgs[0..*out_n]
 * are still owned by the caller and must be freed).
 *
 * This is the canonical helper for fork-join and request-reply
 * patterns: pick a correlation id, send N children a request
 * containing that id, wait for N replies whose first corr_size
 * bytes equal the id.  Avoids manual save-queue management.
 *
 * Each delivered message conforms to the same ownership contract
 * as xtc_recv: the caller owns the buffer and must free() it.
 */
typedef struct xtc_msg {
	void   *data;
	size_t  size;
} xtc_msg_t;

int       xtc_recv_correlate(const void *corr_value, size_t corr_size,
                              int n_expected,
                              xtc_msg_t *out_msgs,
                              int *out_n,
                              int64_t timeout_ns);

/*
 * Wait until ANY of the following becomes true:
 *   - the given fd has any of `interest` bits set
 *     (XTC_IO_READABLE / WRITABLE / ERR / HUP),
 *   - a message arrives in the calling proc's mailbox,
 *   - the timeout elapses (only if `timeout_ns >= 0`),
 *   - the proc is killed (xtc_exit_pid raises the exit as usual).
 *
 * Returns:
 *   XTC_OK       on a non-timeout wakeup.  *out_revents has the
 *                XTC_IO_* bits that fired plus XTC_WAIT_MAILBOX if
 *                a message is queued.  Multiple bits can be set if
 *                more than one source raced to wake.
 *   XTC_E_AGAIN  timeout fired with nothing else.  *out_revents has
 *                XTC_WAIT_TIMEOUT.
 *   XTC_E_INVAL  bad args (NULL out_revents, fd<0, etc.) or called
 *                from outside a process.
 *
 * The fd is auto-unregistered before return; the mailbox is left
 * untouched (caller still calls xtc_recv to actually drain).
 */
#define XTC_WAIT_MAILBOX  0x10000u   /* in out_revents only */
#define XTC_WAIT_TIMEOUT  0x20000u   /* in out_revents only */

int       xtc_proc_wait_fd(int fd, uint32_t interest, int64_t timeout_ns,
                            uint32_t *out_revents);

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

/* Snapshot a process's mailbox statistics into *out.  Returns XTC_OK,
 * or XTC_E_INVAL if the pid is dead / unknown.  Safe to call from any
 * thread. */
int       xtc_proc_mailbox_stats(xtc_pid_t pid, xtc_mailbox_stats_t *out);

/* ---- R1: per-fiber fault containment ----
 *
 * Turns a real synchronous fault (SIGSEGV / SIGBUS / SIGFPE / SIGILL)
 * inside one coroutine into an unwind of only that process, leaving
 * siblings on the same loop untouched -- the runtime support PG's
 * "let it crash" session containment needs.
 *
 * POSIX only (sigaltstack + sigaction + siglongjmp).  On Windows the
 * API is present and compiles, but containment is INACTIVE -- a fault
 * keeps its process-wide disposition (the status quo); structured
 * exception handling is the Windows mechanism and is not yet wired.
 */
#include <setjmp.h>

#if defined(_WIN32)
#include <windows.h>
/*
 * Windows recovery uses CONTEXT capture/restore rather than
 * setjmp/longjmp.  The fault is caught by a Vectored Exception Handler
 * that restores this saved CONTEXT via EXCEPTION_CONTINUE_EXECUTION --
 * the OS reloads the thread's registers and resumes at the capture
 * point.  longjmp out of (or via) a VEH is unsafe: on a fiber stack it
 * walks unwind tables that no longer match and corrupts the CRT; a
 * context restore does no unwinding at all.
 */
typedef struct xtc_recovery_buf { CONTEXT ctx; } xtc_recovery_buf_t;
#else
typedef sigjmp_buf xtc_recovery_buf_t;
#endif

/* Install the process-wide fault handler.  On POSIX it registers a
 * SIGSEGV/SIGBUS/SIGFPE/SIGILL handler on an alternate signal stack
 * (call once per loop thread for the alt stack; the handler is
 * installed once).  On Windows it registers a Vectored Exception
 * Handler.  Returns XTC_OK on success. */
int       xtc_fault_guard_install(void);

/* Internal arm-slot for the xtc_proc_recovery_arm() macro (POSIX). */
xtc_recovery_buf_t *__xtc_proc_recovery_slot(void);

#if defined(_WIN32)
/* Windows recovery-arm helpers (used by the macro below).
 * __xtc_recovery_prep arms the frame and clears the fired flag;
 * __xtc_recovery_ctx returns the CONTEXT to capture into;
 * __xtc_recovery_result returns 0 on the arming pass and the fault
 * code when the VEH has restored the context. */
void      __xtc_recovery_prep(void);
CONTEXT  *__xtc_recovery_ctx(void);
int       __xtc_recovery_result(void);
#endif

/*
 * Arm a recovery frame for the calling process, exactly like
 * sigsetjmp: returns 0 on the normal path and the fault signal number
 * (POSIX) or exception code (Windows) when control returns here via a
 * contained fault.  Use it as:
 *
 *     int sig = xtc_proc_recovery_arm();
 *     if (sig != 0) {                 // recovered from a contained fault
 *         ... release locks, reset the memory context, close fds ...
 *         xtc_exit_self(reason);       // delivers DOWN to the supervisor
 *     }
 *     ... session work ...
 *
 * The frame is disarmed automatically when a fault fires it (so a
 * fault during recovery escalates to process abort); re-arm or
 * xtc_proc_recovery_disarm() as needed.
 *
 * IMPORTANT: containment only unwinds the fiber's CALL STACK.  Any
 * resources the proc held at fault time -- locks, fds, allocations,
 * buffer pins -- are the recovery block's responsibility to release,
 * exactly as a PostgreSQL backend's sigsetjmp handler aborts the
 * transaction and releases LWLocks.  Hold those under an xtc_mctx you
 * can reset, and release lock-manager locks with the lock manager's
 * release-all; otherwise a contained fault leaks or, worse, leaves a
 * lock held and wedges peers.
 */
#if defined(_WIN32)
/* Capture the proc fn's own frame inline (like setjmp), so the VEH can
 * restore it; the comma expression returns 0 while arming and the
 * fault code after a contained fault resumes execution here. */
#define xtc_proc_recovery_arm() \
	(__xtc_recovery_prep(), \
	 RtlCaptureContext(__xtc_recovery_ctx()), \
	 __xtc_recovery_result())
#else
#define xtc_proc_recovery_arm() (sigsetjmp(*__xtc_proc_recovery_slot(), 1))
#endif

/* Disarm the calling process's recovery frame. */
void      xtc_proc_recovery_disarm(void);

/* Critical section: while crit_depth > 0, a fault is NOT contained --
 * it escalates to process abort, because shared state may be torn.
 * Nestable.  Mirrors PG's START_CRIT_SECTION / END_CRIT_SECTION. */
void      xtc_proc_critical_enter(void);
void      xtc_proc_critical_leave(void);

/* Register a callback to run when the calling process exits -- on a
 * normal return OR a contained-fault recovery (after
 * xtc_proc_recovery_arm -> xtc_exit_self).  Callbacks run LIFO,
 * outside signal context, with the proc still current, BEFORE its
 * monitors observe DOWN.  This is where an embedder guarantees a
 * faulted session releases what it held -- e.g. register
 * xtc_lock_release_all so no lock-manager lock outlives the proc, or
 * a memory-context reset.  Up to a small fixed number per proc;
 * returns XTC_E_RESOURCE past the limit, XTC_E_INVAL off a proc. */
int       xtc_proc_at_exit(void (*fn)(void *), void *arg);

/* A memory context scoped to the calling process: created lazily on
 * first call, destroyed automatically on proc exit (a backstop so a
 * faulted session's allocations are reclaimed even if its recovery
 * block itself faults).  Returns NULL off a proc.  See xtc_mctx.h. */
struct xtc_mctx *xtc_proc_mctx(void);

/* Decode a DOWN signal (delivered to a monitor when its target exits)
 * into the target pid and exit reason, without hand-rolling the
 * on-wire layout.  The DOWN/EXIT signals are sent packed; a mismatched
 * (unpacked) mirror struct misreads `reason`.  Returns XTC_OK if msg
 * is a DOWN, XTC_E_INVAL otherwise.  out_pid / out_reason may be NULL. */
int       xtc_down_decode(const void *msg, size_t len,
                          xtc_pid_t *out_pid, int *out_reason);

/* Internal: save / restore the current-proc context across a yield
 * done by a lower-level primitive (e.g. xtc_amutex parking the
 * fiber), so the proc still sees itself on resume.  Opaque to the
 * caller. */
void     *__xtc_proc_ctx_save(void);
void      __xtc_proc_ctx_restore(void *ctx);

#endif /* XTC_PROC_H */
