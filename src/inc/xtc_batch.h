/*-
 * Copyright (c) 2026, The XTC Project
 * @stability experimental
 * Use of this source code is governed by the ISC License.
 */
#ifndef XTC_BATCH_H
#define XTC_BATCH_H

#include <stddef.h>
#include <stdint.h>
#include "xtc_exec.h"
#include "xtc_future.h"
#include "xtc_accel.h"

#define XTC_BATCH_VERSION 1

typedef struct xtc_batch xtc_batch_t;
typedef struct xtc_batch_item {
	uint64_t handle;
	void *request;
} xtc_batch_item_t;

enum xtc_batch_state {
	XTC_BATCH_UNSET,
	XTC_BATCH_CONTINUE,
	XTC_BATCH_DONE,
	XTC_BATCH_BLOCKED,
	XTC_BATCH_ERROR
};
typedef struct xtc_batch_result {
	enum xtc_batch_state state;
	int error;
} xtc_batch_result_t;

typedef struct xtc_batch_opts {
	uint32_t size, version;
	uint32_t capacity, max_active;
	uint64_t resource_capacity;
	xtc_exec_class_t sched_class;
	int (*step)(const xtc_batch_item_t *, xtc_batch_result_t *, size_t,
	            void *);
	void (*complete)(uint64_t, void *, int, void *);
	void *user;
} xtc_batch_opts_t;

enum xtc_batch_close_mode { XTC_BATCH_DRAIN, XTC_BATCH_CANCEL };

/* Async offload (--enable-sched-policy). A group may run a split step: submit
 * launches ONE device/pool op over the ready vector, the engine awaits a
 * completion token, then collect reads outputs. See xtc_batch(3). */
enum xtc_batch_exec_kind {
	XTC_BATCH_EXEC_SYNC,    /* done inline in submit; no park */
	XTC_BATCH_EXEC_FENCE,   /* pollable fence fd (native only) */
	XTC_BATCH_EXEC_FUTURE,  /* app resolves an xtc_future (sim-clean) */
	XTC_BATCH_EXEC_BLOCKING /* synchronous closure on the blocking pool */
};
/* exec_status passed to collect: a non-negative engine sentinel below, or a
 * NEGATIVE XTC_E_* when the engine could not await the op (bad/closed fence
 * fd, no accel support, refused blocking closure, failed/dropped device
 * promise); every member then fails with that code. Only EXEC_OK means the
 * op finished: after any other status it may still be running, and no
 * member is resubmitted (see xtc_batch(3)). */
#define XTC_BATCH_EXEC_OK      0
#define XTC_BATCH_EXEC_TIMEOUT 1 /* exec_timeout_ns elapsed (FENCE/FUTURE) */
#define XTC_BATCH_EXEC_ABORTED 2 /* cancel-closed or driver killed mid-op */
typedef struct xtc_batch_exec {
	enum xtc_batch_exec_kind kind;
	int fence_fd;          /* FENCE: borrowed; app closes */
	xtc_future_t *future;  /* FUTURE: app resolves */
	int (*run)(void *);    /* BLOCKING: synchronous closure */
	void *arg;
} xtc_batch_exec_t;

typedef struct xtc_batch_async_opts {
	uint32_t size, version;
	uint32_t capacity, max_active;
	uint64_t resource_capacity;
	/* Cross-loop admission (submit_mt/cancel_mt/resume_mt). 0 disables mt
	 * submission for this group (submit_mt returns XTC_E_NOSYS). When set,
	 * it bounds BOTH the pending hand-in ring and its preallocated record
	 * pool; distinct from `capacity` (admitted live requests). */
	uint32_t mt_queue_cap;
	int64_t exec_timeout_ns; /* 0 = none; FENCE/FUTURE wait only */
	xtc_exec_class_t sched_class;
	int (*submit)(const xtc_batch_item_t *, size_t, void *,
	              xtc_batch_exec_t *);
	int (*collect)(const xtc_batch_item_t *, xtc_batch_result_t *, size_t,
	               int, void *);
	void (*complete)(uint64_t, void *, int, void *);
	/* Optional: fires once per in-flight member when its cancel lands,
	 * while the op is still outstanding. */
	void (*cancel_hint)(uint64_t, void *);
	void *user;
} xtc_batch_async_opts_t;

typedef struct xtc_batch_async_stats {
	uint32_t size, version;
	uint64_t offloads, offload_timeouts, offload_errors;
	/* mt_queued: hand-ins pushed through the admission ring over the
	 * group's life (monotonic). mt_rejections: submit_mt/cancel_mt/
	 * resume_mt rejected because the ring or record pool was full, or
	 * submit_mt refused because the group was closed. */
	uint64_t mt_queued, mt_rejections;
} xtc_batch_async_stats_t;

typedef struct xtc_batch_stats {
	uint32_t size, version;
	uint32_t queued, ready, blocked;
	uint64_t reserved_units, iterations, completed, cancelled, failed;
	uint64_t rejections, max_vector, total_members;
} xtc_batch_stats_t;

#ifdef __cplusplus
extern "C" {
#endif
/* Setup is externally serialized, before the loop's first run/step. */
XTC_API int
xtc_batch_create(xtc_loop_t *, const xtc_batch_opts_t *, xtc_batch_t **);
/* Mutations: pre-run or actual owner loop only; callbacks in flight => DEADLK.
 * Requests are borrowed through completion. Handles belong to one group. */
XTC_API int xtc_batch_submit(xtc_batch_t *, void *, uint64_t, uint64_t *);
XTC_API int xtc_batch_cancel(xtc_batch_t *, uint64_t);
XTC_API int xtc_batch_resume(xtc_batch_t *, uint64_t);
XTC_API int xtc_batch_close(xtc_batch_t *, enum xtc_batch_close_mode);
/* Destroy/stats also permit externally synchronized quiescent post-run calls.
 * Close -> run until driver ends -> destroy -> loop/exec fini. Stop is not
 * drain. Initialize output size/version; unknown version => VERSION, wrong size
 * => INVAL. Feature-disabled builds return NOSYS for every batch operation. */
XTC_API int xtc_batch_destroy(xtc_batch_t *);
XTC_API int xtc_batch_get_stats(xtc_batch_t *, xtc_batch_stats_t *);
/* Async-offload group. Same lifecycle as xtc_batch_create; the step is the
 * submit/collect pair in opts. submit_mt/placement are NOT part of this.
 * Disabled builds return NOSYS. */
XTC_API int
xtc_batch_create_async(xtc_loop_t *, const xtc_batch_async_opts_t *,
    xtc_batch_t **);
XTC_API int xtc_batch_get_async_stats(xtc_batch_t *, xtc_batch_async_stats_t *);
/* Cross-loop admission (--enable-sched-policy, mt_queue_cap > 0 in
 * xtc_batch_create_async opts). submit_mt/cancel_mt/resume_mt are callable
 * from ANY loop or a plain OS thread: they touch only a lock-free admission
 * ring + record pool + the group's monotonic handle counter, never the
 * group's slots. The driver (owner loop) drains the ring at each iteration
 * boundary. submit_mt stamps and returns a handle immediately; admission into
 * the active set happens at the next boundary (a hand-in that finds the group
 * full waits, in order, for a slot). Every XTC_OK is followed by exactly one
 * complete callback. A cancel_mt for a handle the caller was given is always
 * applied; one for a completed handle is discarded. Returns XTC_E_NOSYS if
 * mt_queue_cap == 0 (mt not enabled) or the build lacks
 * --enable-sched-policy; XTC_E_AGAIN if the ring/pool is full or the group is
 * closed (no handle issued; retry with a yield/sleep, there is no blocking
 * variant); XTC_E_RANGE on an impossible charge or handle exhaustion.
 * cancel_mt/resume_mt return XTC_E_INVAL once the driver has finished.
 * See xtc_batch(3). */
XTC_API int xtc_batch_submit_mt(xtc_batch_t *, void *, uint64_t, uint64_t *);
XTC_API int xtc_batch_cancel_mt(xtc_batch_t *, uint64_t);
XTC_API int xtc_batch_resume_mt(xtc_batch_t *, uint64_t);

/* ----- Placement helpers (--enable-sched-policy) -----------------------
 * Advisory glue, NOT a scheduler: there is no automatic migration of a
 * live group. A group stays on its creation loop; these only help a
 * consumer CHOOSE a loop at create time and shard across a fixed set of
 * groups it created. Disabled builds return XTC_E_NOSYS. */

/* Map a probed accelerator device to the executor loop index whose
 * carrier thread is on the same NUMA node as the device (best-effort).
 * Reads the device's NUMA node from sysfs (static topology, no sim-
 * nondeterministic source): for a renderD* node from
 * /sys/class/drm/<name>/device/numa_node, for an accel* node from
 * /sys/class/accel/<name>/device/numa_node. On success writes a loop
 * index in [0, xtc_exec_n_loops) to *out_loop_idx and returns XTC_OK.
 * Falls back to loop 0 (still XTC_OK -- absence is not an error, matching
 * xtc_accel's convention) when the device numa_node is -1/unknown, the
 * executor's loops record no known node, or numa_nodes <= 1. A carrier
 * records its node when it starts, so before the executor has run (when a
 * group can be created) this currently always returns loop 0. Returns
 * XTC_E_INVAL on a NULL argument. See xtc_batch(3). */
XTC_API int xtc_batch_loop_for_device(xtc_exec_t *,
    const xtc_accel_dev_t *, int *);

/* Keyed-shard router: N groups + a modulo, nothing more. The caller
 * creates one group per shard loop (xtc_batch_create[_async] per loop,
 * typically placed with xtc_batch_loop_for_device); the router is pure
 * glue over those N independent groups and adds NO cross-group runtime
 * coupling. It does not submit -- the caller does
 * xtc_batch_submit[_mt](xtc_batch_route(r, key), ...). _create BORROWS
 * the groups array by copy (does not own the groups); _destroy frees
 * ONLY the router, never the groups (caller owns their lifetime).
 * _route returns groups[key % n] (stable key -> shard). _create returns
 * XTC_E_INVAL on NULL/n<=0, XTC_E_NOMEM on allocation failure,
 * XTC_E_NOSYS in disabled builds. */
typedef struct xtc_batch_router xtc_batch_router_t;
XTC_API int xtc_batch_router_create(xtc_batch_t **groups, int n,
    xtc_batch_router_t **out);
XTC_API void xtc_batch_router_destroy(xtc_batch_router_t *);
XTC_API xtc_batch_t *xtc_batch_route(xtc_batch_router_t *, uint64_t key);
#ifdef __cplusplus
}
#endif
#endif
