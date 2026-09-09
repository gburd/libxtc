/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/aio_int.h
 *	Internal xtc_aio helpers.  __xtc_aio_force_offload forces
 *	the blocking-pool offload path even on a host with a native
 *	completion engine (io_uring / IOCP), so the portable fallback can
 *	be exercised and proven identical to the native path.  Also reads
 *	XTC_AIO_FORCE_OFFLOAD=1 on first use.  __xtc_aio_done_set/_get are
 *	the atomic accessors for the cross-thread xtc_aio_t.done completion
 *	flag (see the contract comment below).  Library-internal (the __
 *	prefix) and not part of the stable API -- split out of xtc_aio.h
 *	so no __-prefixed symbol leaks into an installed public header.
 */

#ifndef XTC_AIO_INT_H
#define XTC_AIO_INT_H

#include "xtc_io.h"       /* xtc_aio_t */

#include <stdatomic.h>

void __xtc_aio_force_offload(int on);

/*
 * Cross-thread access to xtc_aio_t.done.
 *
 * On a native completion backend the REAPING thread (whichever loop's poll
 * thread drains the ring) publishes the result and the PARKED FIBER reads it
 * in its wake-recheck loop.  Two threads, one flag: the accesses must be
 * atomic, and the flag must ORDER the result field it guards.
 *
 * The field itself stays a plain `int` -- see the contract comment on
 * xtc_aio_t.done in xtc_io.h for why (public header, C++ consumers, and a
 * measured-identical layout either way).  These helpers apply C11 atomics to
 * its ADDRESS, the pattern src/inc/os_atomic.h is built around.
 *
 * Ordering is release/acquire, not seq_cst, and that is the point: the
 * reaper writes a->res and THEN releases a->done, so a fiber that acquires a
 * set done is guaranteed to see the matching res.  Before this, res was read
 * after a PLAIN done read with nothing ordering the two -- the scheduler
 * handoff (the wake CAS / inbox mutex) does supply an edge, but the recheck
 * loop can observe done through its own read WITHOUT going through that
 * edge, so the handoff did not actually cover this pair.  Same publish-
 * result-then-flag shape, and same ordering, as w->result / w->done in
 * src/ptc/blocking.c.
 *
 * The clearing store on the submit path is deliberately NOT routed here: it
 * runs on the submitting fiber's own thread before the op is visible to any
 * reaper, which is exactly the single-threaded pre-publication case plain
 * storage exists to allow.
 */
static inline void
__xtc_aio_done_set(xtc_aio_t *a)
{
	atomic_store_explicit((_Atomic int *)&a->done, 1,
	    memory_order_release);
}

static inline int
__xtc_aio_done_get(const xtc_aio_t *a)
{
	return atomic_load_explicit(
	    (_Atomic int *)(uintptr_t)&a->done, memory_order_acquire);
}

#endif /* XTC_AIO_INT_H */
