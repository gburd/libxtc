/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_res.h
 *	Resource governance.  An xtc_res_t is a per-executor (or
 *	per-loop) accountant that tracks bounded resources: tasks,
 *	channels, channel slots, file descriptors, memory.  Acquire is
 *	atomic and either succeeds (counter <= cap) or returns
 *	XTC_E_RESOURCE; release is unconditional.
 *
 *	WHAT IS METERED -- exactly, nothing else is charged by libxtc:
 *	  TASKS, INBOX_MSGS  every task spawned on a loop (xtc_async,
 *	      xtc_proc_spawn, ...) against that loop's own accountant
 *	      (xtc_loop_res); cross-thread spawns also charge INBOX_MSGS.
 *	  CHANNELS, CHAN_SLOTS  channels created with a non-NULL res.
 *	  MEM_BYTES  xtc_slab chunks when xtc_slab_opts_t.res is set, and
 *	      xtc_mctx chunks (header + payload) of a context attached
 *	      with xtc_res_attach_mctx.  NOT xtc_malloc, fiber stacks,
 *	      proc/mailbox structures, or any other internal allocation.
 *	  FDS  descriptors returned by xtc_net_listen, _dial,
 *	      _unix_listen, _unix_dial and _udp_socket while an
 *	      accountant is attached with xtc_res_attach_net, released by
 *	      xtc_net_close.  NOT fds from a raw accept(2), files, pipes,
 *	      the I/O backend's own fds, or xproc control sockets.
 *	A cap on a kind nothing charges bounds nothing.  Metering that
 *	needs an attach is off by default (no cost, no behavior change).
 */

#ifndef XTC_RES_H
#define XTC_RES_H

#include "xtc_export.h"

#include <stdint.h>
#include <stdatomic.h>

#include "xtc.h"

typedef enum xtc_res_kind {
	XTC_RES_TASKS = 0,        /* live xtc_task_t allocations */
	XTC_RES_CHANNELS = 1,     /* live xtc_chan_* objects */
	XTC_RES_CHAN_SLOTS = 2,   /* in-flight messages across all chans */
	XTC_RES_FDS = 3,          /* open fds attributable to xtc */
	XTC_RES_MEM_BYTES = 4,    /* bytes in xtc-tracked allocations */
	XTC_RES_INBOX_MSGS = 5,   /* cross-loop inbox messages in flight */

	XTC_RES__COUNT
} xtc_res_kind_t;

/*
 * Caps.  Zero means "no cap" (unbounded; debug only).  Defaults
 * pick reasonable values for a 4-loop M5 executor on a workstation.
 */
typedef struct xtc_res_caps {
	int64_t tasks;            /* default 100000 */
	int64_t channels;         /* default 4096 */
	int64_t chan_slots;       /* default 1000000 */
	int64_t fds;              /* default 65536 */
	int64_t mem_bytes;        /* default 1 GiB */
	int64_t inbox_msgs;       /* default 65536 (per loop, not global) */
} xtc_res_caps_t;

#define XTC_RES_CAPS_DEFAULT { \
	.tasks       = 100000,            \
	.channels    = 4096,              \
	.chan_slots  = 1000000,           \
	.fds         = 65536,             \
	.mem_bytes   = 1024L * 1024 * 1024, \
	.inbox_msgs  = 65536              \
}

typedef struct xtc_res {
	xtc_res_caps_t   caps;
	_Atomic int64_t  used[XTC_RES__COUNT];
	_Atomic int64_t  high[XTC_RES__COUNT];   /* high-water mark for stats */
	_Atomic int64_t  rejects[XTC_RES__COUNT];/* count of XTC_E_RESOURCE returns */

	/* High-water alert callback: fires once when used / cap crosses
	 * the threshold percent (e.g. 0.8 = 80%).  Re-arms when used
	 * drops below the threshold so a second crossing fires again.
	 * Set via xtc_res_set_alert. */
	double           alert_pct[XTC_RES__COUNT];
	_Atomic int      alert_armed[XTC_RES__COUNT];   /* 1 = ready to fire */
	void           (*alert_fn)(xtc_res_kind_t k, int64_t used,
	                          int64_t cap, void *user);
	void            *alert_user;
} xtc_res_t;

/*
 * PUBLIC: int  xtc_res_init __P((xtc_res_t *, const xtc_res_caps_t *));
 * PUBLIC: int  xtc_res_acquire __P((xtc_res_t *, xtc_res_kind_t, int64_t));
 * PUBLIC: void xtc_res_release __P((xtc_res_t *, xtc_res_kind_t, int64_t));
 * PUBLIC: int64_t xtc_res_used __P((const xtc_res_t *, xtc_res_kind_t));
 * PUBLIC: int64_t xtc_res_high __P((const xtc_res_t *, xtc_res_kind_t));
 * PUBLIC: int64_t xtc_res_rejects __P((const xtc_res_t *, xtc_res_kind_t));
 * PUBLIC: void xtc_res_set_cap __P((xtc_res_t *, xtc_res_kind_t, int64_t));
 * PUBLIC: int  xtc_res_attach_mctx __P((xtc_res_t *, struct xtc_mctx *));
 * PUBLIC: int  xtc_res_attach_net __P((xtc_res_t *));
 */
XTC_API int  xtc_res_init(xtc_res_t *r, const xtc_res_caps_t *caps);

/*
 * Try to charge `n` units of `kind` to `r`.  Returns:
 *   XTC_OK            on success
 *   XTC_E_RESOURCE    if the request would exceed the cap
 *   XTC_E_INVAL       on a bad kind / negative n / NULL r
 *
 * Atomic and lock-free.
 */
XTC_API int  xtc_res_acquire(xtc_res_t *r, xtc_res_kind_t k, int64_t n);

/*
 * Release `n` units.  Never fails; clamps at zero on underflow
 * (treated as a programming error in debug builds).
 */
XTC_API void xtc_res_release(xtc_res_t *r, xtc_res_kind_t k, int64_t n);

XTC_API int64_t xtc_res_used(const xtc_res_t *r, xtc_res_kind_t k);
XTC_API int64_t xtc_res_high(const xtc_res_t *r, xtc_res_kind_t k);
XTC_API int64_t xtc_res_rejects(const xtc_res_t *r, xtc_res_kind_t k);

/* Set one kind's cap at runtime; 0 = no cap.  Returns nothing, so an
 * invalid argument (NULL r, bad kind, NEGATIVE cap) is ignored and the
 * existing cap is kept.  Releases before 1.50 stored a negative cap,
 * which then read as "no cap" -- silently unbounded.  (A negative cap
 * passed in the caps struct to xtc_res_init still means no cap.) */
XTC_API void xtc_res_set_cap(xtc_res_t *r, xtc_res_kind_t k, int64_t cap);

/* Configure a high-water alert.  Fires `fn(kind, used, cap, user)`
 * once when `used >= pct * cap` for the named resource; re-arms
 * when used drops below.  pct must lie in the OPEN interval
 * (0.0, 1.0); 0, 1.0, anything outside, or NaN is XTC_E_INVAL
 * (releases before 1.50 accepted 0 and 1.0).  Pass fn=NULL to
 * disable.  Per-resource: alerts are independent.
 *
 * PUBLIC: int  xtc_res_set_alert __P((xtc_res_t *, xtc_res_kind_t, double));
 * PUBLIC: int  xtc_res_set_alert_fn __P((xtc_res_t *, void (*)(xtc_res_kind_t, int64_t, int64_t, void *), void *));
 */
XTC_API int  xtc_res_set_alert(xtc_res_t *r, xtc_res_kind_t k, double pct);
XTC_API int  xtc_res_set_alert_fn(xtc_res_t *r,
                                  void (*fn)(xtc_res_kind_t, int64_t, int64_t, void *),
                                  void *user);

/*
 * Metering attach points (since 1.50).  Both are opt-in; until called,
 * nothing is charged.  Pass r == NULL to detach.
 *
 * xtc_res_attach_mctx: charge every chunk of memory context `m`
 *   (payload plus its fixed per-chunk header) to r's XTC_RES_MEM_BYTES.
 *   The context's CURRENT footprint is charged at attach time; if that
 *   alone exceeds the cap the call fails with XTC_E_RESOURCE and
 *   changes nothing.  Afterwards an allocation that would pass the cap
 *   is REFUSED: xtc_mctx_alloc/_calloc/_strdup return NULL without
 *   touching the heap.  xtc_mctx_free / _reset / _destroy give the
 *   bytes back; detaching (or re-attaching elsewhere) moves the current
 *   footprint.  Children created while attached inherit r (a child
 *   created earlier is not affected).  Call at setup time: attach must
 *   not race an alloc/free on the same context.  XTC_E_INVAL if m is
 *   NULL.
 *
 * xtc_res_attach_net: PROCESS-WIDE.  Every fd a creating xtc_net call
 *   hands out is charged one XTC_RES_FDS unit to r BEFORE the socket is
 *   made; past the cap the call returns XTC_E_RESOURCE and creates
 *   nothing (a refused dial sends no SYN).  xtc_net_close releases the
 *   unit to whichever accountant the fd was charged to, so a later
 *   detach does not unbalance fds already out.  Close metered fds with
 *   xtc_net_close; a raw close(2) leaks the unit until the kernel
 *   reuses that fd number for another metered socket.  Always XTC_OK.
 */
struct xtc_mctx;
XTC_API int  xtc_res_attach_mctx(xtc_res_t *r, struct xtc_mctx *m);
XTC_API int  xtc_res_attach_net(xtc_res_t *r);

#endif /* XTC_RES_H */
