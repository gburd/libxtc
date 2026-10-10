/* Internal lane policy state. No queue links or allocation. */
#ifndef XTC_SCHED_INT_H
#define XTC_SCHED_INT_H
#include "xtc_sched.h"
#include <stdatomic.h>
#ifdef XTC_ENABLE_SCHED_POLICY
struct xtc_sched_state {
	/* 0: setup, 1: descriptor being copied, 2: permanently frozen. */
	_Atomic int phase;
	xtc_sched_policy_t policy;
	xtc_sched_stats_t stats;
	uint64_t bypasses[17], runs[17];
	uint32_t batch_ready[16], batch_active[16];
	unsigned char batch_attached[16];
	unsigned batch_next;
};
/* Caller serializes pre-run/owner access and brackets unsafe bookkeeping.
 * One group per explicit class; default-lane groups do not register. */
int __xtc_sched_batch_attach(xtc_loop_t *, int);
void __xtc_sched_batch_detach(xtc_loop_t *, int);
void __xtc_sched_batch_hint(xtc_loop_t *, int, uint32_t, uint32_t);
void __xtc_sched_freeze(xtc_loop_t *);
int __xtc_sched_select(xtc_loop_t *);
void __xtc_sched_service(xtc_loop_t *, int);
void __xtc_sched_ran(xtc_loop_t *, int, int);
#endif
#endif
