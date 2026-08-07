/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_trace.h
 *	Causal message tracing -- libxtc's seq_trace.  An opt-in,
 *	bounded ring of trace events (message sends and receives,
 *	process spawns and exits), each stamped with a hybrid logical
 *	clock (HLC) so the true happens-before of a request can be
 *	reconstructed across procs and cores even when wall-clock
 *	arrival order lies.
 *
 *	The HLC is a 64-bit stamp: the high 48 bits are a monotonic
 *	physical-time component (microseconds) and the low 16 bits a
 *	logical counter.  Every traced event ticks one global clock, so
 *	a send's stamp is always less than the stamp of the receive it
 *	causes; a RECV record also carries the originating SEND's stamp
 *	in `cause`, so the causal edge is explicit.  (A per-shard HLC for
 *	the MVCC data path is a separate, later
 *	use of the same idea.)
 *
 *	Tracing is OFF by default and costs a single relaxed atomic load
 *	on the message hot path when disabled.  When enabled it serializes
 *	ring writes under a lock; enable it to debug, as with recon/dbg
 *	in the BEAM, not as an always-on production tax.
 *
 *	See docs/guide/debugging.md.
 */

#ifndef XTC_TRACE_H
#define XTC_TRACE_H

#include "xtc_export.h"

#include <stddef.h>
#include <stdint.h>

#include "xtc.h"
#include "xtc_proc.h"

enum xtc_trace_kind {
	XTC_TRACE_SEND  = 0,   /* self sent a message to peer */
	XTC_TRACE_RECV  = 1,   /* self received a message from peer */
	XTC_TRACE_SPAWN = 2,   /* self (parent) spawned peer (child) */
	XTC_TRACE_EXIT  = 3    /* self exited; detail = reason */
};

typedef struct xtc_trace_rec {
	uint64_t  hlc;     /* this event's HLC stamp */
	uint64_t  cause;   /* RECV: the originating send's HLC; else 0 */
	int       kind;    /* enum xtc_trace_kind */
	xtc_pid_t self;    /* the proc the event happened in */
	xtc_pid_t peer;    /* the other proc (dest / source / child) */
	uint32_t  detail;  /* SEND/RECV: payload bytes; EXIT: reason */
} xtc_trace_rec_t;

/* Visit callback: return 0 to continue, nonzero to stop early. */
typedef int (*xtc_trace_fn)(const xtc_trace_rec_t *rec, void *user);

/*
 * PUBLIC: int      xtc_trace_enable __P((int));
 * PUBLIC: int      xtc_trace_reset __P((void));
 * PUBLIC: int      xtc_trace_dump __P((xtc_trace_fn, void *));
 * PUBLIC: uint64_t xtc_hlc_now __P((void));
 */

/* Turn tracing on (on != 0) or off.  Returns the previous state. */
XTC_API int xtc_trace_enable(int on);

/* Drop all buffered trace records.  Returns XTC_OK. */
XTC_API int xtc_trace_reset(void);

/* Visit buffered records in causal (HLC-ascending) order.  Returns the
 * number visited, or a negative XTC_E_* on error. */
XTC_API int xtc_trace_dump(xtc_trace_fn cb, void *user);

/* The current global HLC value (for tests and display). */
XTC_API uint64_t xtc_hlc_now(void);

/* ---- A3: async causal trace (per-fiber suspend/resume ring) ----
 *
 * Cats Effect keeps a small per-fiber ring of the await/resume call
 * sites and splices that causal chain onto a fault or fiber dump, so a
 * dump answers not just WHAT a fiber's state is now but HOW it got here.
 * This is libxtc's C analog.  Where xtc_trace above records the causal
 * chain of MESSAGES between procs (seq_trace), the causal trace here
 * records the suspend/resume chain WITHIN one proc: the ordered sites at
 * which the fiber parked and resumed (mailbox recv, timer sleep, fd
 * wait, ...).  xtc_dump splices each proc's recent chain onto its state
 * line when the trace is enabled.
 *
 * It is OFF by default and ZERO-COST when off: the per-proc ring is
 * written only when the trace is enabled, so a disabled trace is a
 * single relaxed-atomic load + branch on the suspend/resume boundary and
 * touches no memory.  The ring is per-proc and core-private -- each
 * write is one index bump and a store on the owning fiber, no lock, no
 * atomic on the record, no allocation after spawn.  Enable it to debug a
 * stuck or mis-scheduled fiber, not as an always-on tax.
 *
 * See docs/guide/debugging.md.
 */

/* The kind of suspend/resume boundary a causal record marks.  A PARK_*
 * value names WHY the fiber suspended; RESUME marks the matching wake. */
enum xtc_causal_kind {
	XTC_CAUSAL_PARK_MAILBOX = 0,   /* parked in xtc_recv* (await a message) */
	XTC_CAUSAL_PARK_TIMER   = 1,   /* parked in xtc_proc_sleep (a delay) */
	XTC_CAUSAL_PARK_FD      = 2,   /* parked in xtc_proc_wait_fd (I/O) */
	XTC_CAUSAL_RESUME       = 3    /* the fiber resumed after a park */
};

/* One per-fiber causal record: the boundary kind + a static site label
 * (the __func__ of the park site, or a caller-supplied string literal --
 * always a static string, so the ring stores the pointer, never a copy). */
typedef struct xtc_causal_rec {
	int          kind;    /* enum xtc_causal_kind */
	const char  *site;    /* static label (e.g. __func__); never freed */
} xtc_causal_rec_t;

/* Visit callback: return 0 to continue, nonzero to stop early. */
typedef int (*xtc_causal_fn)(const xtc_causal_rec_t *rec, void *user);

/*
 * PUBLIC: int xtc_trace_causal_enable __P((int));
 * PUBLIC: int xtc_trace_causal_dump __P((xtc_pid_t, xtc_causal_fn, void *));
 */

/* Turn the per-fiber causal trace on (on != 0) or off.  Returns the
 * previous state (1 on, 0 off).  Off by default; when off the
 * suspend/resume boundary pays one relaxed load + branch. */
XTC_API int xtc_trace_causal_enable(int on);

/* Visit the causal records of proc `pid` oldest-first (the fiber's
 * recent park/resume chain).  Returns the number visited, XTC_E_NOTFOUND
 * if no live proc has that pid, or XTC_E_INVAL on a NULL callback.  When
 * the trace is disabled a live proc simply has an empty ring (0). */
XTC_API int xtc_trace_causal_dump(xtc_pid_t pid, xtc_causal_fn cb, void *user);

#endif /* XTC_TRACE_H */
