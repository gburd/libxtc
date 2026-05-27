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
 *	The point of this subsystem is the BEAM/Seastar/libumem
 *	"predictably reliable" promise: a misbehaving client cannot
 *	exhaust the host because every resource has a documented cap,
 *	a documented behaviour at the cap, and a way for the operator
 *	to observe both.
 */

#ifndef XTC_RES_H
#define XTC_RES_H

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
 */
int  xtc_res_init(xtc_res_t *r, const xtc_res_caps_t *caps);

/*
 * Try to charge `n` units of `kind` to `r`.  Returns:
 *   XTC_OK            on success
 *   XTC_E_RESOURCE    if the request would exceed the cap
 *   XTC_E_INVAL       on a bad kind / negative n / NULL r
 *
 * Atomic and lock-free.
 */
int  xtc_res_acquire(xtc_res_t *r, xtc_res_kind_t k, int64_t n);

/*
 * Release `n` units.  Never fails; clamps at zero on underflow
 * (treated as a programming error in debug builds).
 */
void xtc_res_release(xtc_res_t *r, xtc_res_kind_t k, int64_t n);

int64_t xtc_res_used(const xtc_res_t *r, xtc_res_kind_t k);
int64_t xtc_res_high(const xtc_res_t *r, xtc_res_kind_t k);
int64_t xtc_res_rejects(const xtc_res_t *r, xtc_res_kind_t k);
void xtc_res_set_cap(xtc_res_t *r, xtc_res_kind_t k, int64_t cap);

/* Configure a high-water alert.  Fires `fn(kind, used, cap, user)`
 * once when `used >= pct * cap` for the named resource; re-arms
 * when used drops below.  pct in (0.0, 1.0).  Pass fn=NULL to
 * disable.  Per-resource: alerts are independent.
 *
 * PUBLIC: int  xtc_res_set_alert __P((xtc_res_t *, xtc_res_kind_t, double));
 * PUBLIC: int  xtc_res_set_alert_fn __P((xtc_res_t *, void (*)(xtc_res_kind_t, int64_t, int64_t, void *), void *));
 */
int  xtc_res_set_alert(xtc_res_t *r, xtc_res_kind_t k, double pct);
int  xtc_res_set_alert_fn(xtc_res_t *r,
                          void (*fn)(xtc_res_kind_t, int64_t, int64_t, void *),
                          void *user);

#endif /* XTC_RES_H */
