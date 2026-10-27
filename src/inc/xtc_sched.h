/*-
 * Copyright (c) 2026, The XTC Project
 * @stability experimental
 * Use of this source code is governed by the ISC License.
 */
#ifndef XTC_SCHED_H
#define XTC_SCHED_H

#include <stddef.h>
#include <stdint.h>
#include "xtc_loop.h"

#define XTC_SCHED_VERSION 1
/* Defer to the ordinary picker; lane -1 is the actual default lane. */
#define XTC_SCHED_DEFAULT (-2)

typedef struct xtc_sched_lane {
	int32_t id;
	uint32_t shares;
	uint64_t runs, bypasses;
	/* Advisory batch counts; never establish runqueue readiness. */
	uint32_t ready, active;
} xtc_sched_lane_t;

typedef struct xtc_sched_view {
	const xtc_sched_lane_t *lanes;
	size_t count;
} xtc_sched_view_t;

enum xtc_sched_verdict {
	XTC_SCHED_DONE, XTC_SCHED_YIELD, XTC_SCHED_PARK
};
typedef struct xtc_sched_run {
	int32_t lane;
	enum xtc_sched_verdict verdict;
	uint64_t units;
} xtc_sched_run_t;

typedef struct xtc_sched_policy {
	uint32_t size, version;
	uint32_t max_bypass;
	int32_t (*select)(const xtc_sched_view_t *, void *);
	void (*ran)(const xtc_sched_run_t *, void *);
	void *user;
} xtc_sched_policy_t;

typedef struct xtc_sched_stats {
	uint32_t size, version;
	uint64_t selections, fallbacks, invalid_choices, fairness_overrides;
} xtc_sched_stats_t;

#ifdef __cplusplus
extern "C" {
#endif
/* Copy descriptor; borrow callback/context through loop teardown. Setup only,
 * frozen by the first run/step (even empty). NULL removes before first run.
 * Callbacks must not block, yield, allocate, or mutate runtime state. */
XTC_API int xtc_sched_policy_set(xtc_loop_t *, const xtc_sched_policy_t *);
/* Install loop-owned continuous batch-lane preference before first run. */
XTC_API int xtc_sched_continuous_set(xtc_loop_t *, uint32_t max_bypass);
/* Owner/quiescent reads only. Initialize size and version before calling. */
XTC_API int xtc_sched_get_stats(xtc_loop_t *, xtc_sched_stats_t *);
#ifdef __cplusplus
}
#endif
#endif
