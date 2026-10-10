/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 */
#ifndef XTC_BATCH_H
#define XTC_BATCH_H

#include <stddef.h>
#include <stdint.h>
#include "xtc_exec.h"

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
#ifdef __cplusplus
}
#endif
#endif
