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
 *	the MVCC data path -- docs/M_CAUSALITY.md -- is a separate, later
 *	use of the same idea.)
 *
 *	Tracing is OFF by default and costs a single relaxed atomic load
 *	on the message hot path when disabled.  When enabled it serializes
 *	ring writes under a lock; enable it to debug, as with recon/dbg
 *	in the BEAM, not as an always-on production tax.
 *
 *	See docs/M_OBSERVABILITY.md (stage 3) and docs/guide/debugging.md.
 */

#ifndef XTC_TRACE_H
#define XTC_TRACE_H

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
int xtc_trace_enable(int on);

/* Drop all buffered trace records.  Returns XTC_OK. */
int xtc_trace_reset(void);

/* Visit buffered records in causal (HLC-ascending) order.  Returns the
 * number visited, or a negative XTC_E_* on error. */
int xtc_trace_dump(xtc_trace_fn cb, void *user);

/* The current global HLC value (for tests and display). */
uint64_t xtc_hlc_now(void);

#endif /* XTC_TRACE_H */
