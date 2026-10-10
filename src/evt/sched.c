/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 */
#include "xtc_int.h"
#include "loop_int.h"
#include "xtc_sched.h"

#ifdef XTC_ENABLE_SCHED_POLICY
#include "xtc_dst_inject.h" /* DST bug-injection harness (no-op in prod) */
/* XTC_NOALLOC_BEGIN: continuous batch lane preference and hints */
static int32_t
__sched_continuous_select(const xtc_sched_view_t *view, void *arg)
{
	struct xtc_sched_state *s = arg;
	const xtc_sched_lane_t *lane;
	uint64_t oldest = 0;
	unsigned distance, nearest = 16;
	int choice = XTC_SCHED_DEFAULT;
	size_t i;

	for (i = 0; i < view->count; i++) {
		lane = &view->lanes[i];
		if (lane->id < 0 || lane->id >= 16 ||
		    !s->batch_attached[lane->id] || lane->ready == 0)
			continue;
		distance = ((unsigned)lane->id + 16 - s->batch_next) % 16;
		if (choice == XTC_SCHED_DEFAULT || lane->bypasses > oldest ||
		    (lane->bypasses == oldest && distance < nearest)) {
			choice = lane->id;
			oldest = lane->bypasses;
			nearest = distance;
		}
	}
	return choice;
}

int
__xtc_sched_batch_attach(xtc_loop_t *loop, int lane)
{
	if (loop == NULL || lane < 0 || lane >= 16 ||
	    lane >= loop->n_classes || !loop->classes[lane].in_use ||
	    loop->sched.batch_attached[lane])
		return XTC_E_INVAL;
	loop->sched.batch_attached[lane] = 1;
	loop->sched.batch_ready[lane] = 0;
	loop->sched.batch_active[lane] = 0;
	return XTC_OK;
}

void
__xtc_sched_batch_detach(xtc_loop_t *loop, int lane)
{
	if (loop == NULL || lane < 0 || lane >= 16)
		return;
	loop->sched.batch_attached[lane] = 0;
	loop->sched.batch_ready[lane] = 0;
	loop->sched.batch_active[lane] = 0;
}

void
__xtc_sched_batch_hint(xtc_loop_t *loop, int lane, uint32_t ready,
    uint32_t active)
{
	if (loop == NULL || lane < 0 || lane >= 16 ||
	    !loop->sched.batch_attached[lane])
		return;
	loop->sched.batch_ready[lane] = ready;
	loop->sched.batch_active[lane] = active;
}
/* XTC_NOALLOC_END */
#endif

/* PUBLIC: int xtc_sched_continuous_set __P((xtc_loop_t *, uint32_t)); */
int
xtc_sched_continuous_set(xtc_loop_t *loop, uint32_t max_bypass)
{
#ifdef XTC_ENABLE_SCHED_POLICY
	xtc_sched_policy_t policy = {0};

	if (loop == NULL)
		return XTC_E_INVAL;
	policy.size = sizeof(policy);
	policy.version = XTC_SCHED_VERSION;
	policy.max_bypass = max_bypass;
	policy.select = __sched_continuous_select;
	policy.user = &loop->sched;
	return xtc_sched_policy_set(loop, &policy);
#else
	(void)loop;
	(void)max_bypass;
	return XTC_E_NOSYS;
#endif
}

/* PUBLIC: int xtc_sched_policy_set __P((xtc_loop_t *, const xtc_sched_policy_t *)); */
int
xtc_sched_policy_set(xtc_loop_t *loop, const xtc_sched_policy_t *policy)
{
#ifdef XTC_ENABLE_SCHED_POLICY
	int expected = 0, rc = XTC_OK;
	if (loop == NULL)
		return XTC_E_INVAL;
	if (!atomic_compare_exchange_strong_explicit(&loop->sched.phase,
	    &expected, 1, memory_order_acquire, memory_order_relaxed))
		return XTC_E_AGAIN;
	if (policy != NULL) {
		if (policy->size != sizeof(*policy))
			rc = XTC_E_INVAL;
		else if (policy->version != XTC_SCHED_VERSION)
			rc = XTC_E_VERSION;
		else if (policy->select == NULL || policy->max_bypass == 0)
			rc = XTC_E_INVAL;
	}
	if (rc == XTC_OK) {
		if (policy != NULL)
			loop->sched.policy = *policy;
		else
			memset(&loop->sched.policy, 0, sizeof(loop->sched.policy));
	}
	atomic_store_explicit(&loop->sched.phase, 0, memory_order_release);
	return rc;
#else
	(void)loop;
	(void)policy;
	return XTC_E_NOSYS;
#endif
}

/* PUBLIC: int xtc_sched_get_stats __P((xtc_loop_t *, xtc_sched_stats_t *)); */
int
xtc_sched_get_stats(xtc_loop_t *loop, xtc_sched_stats_t *out)
{
#ifdef XTC_ENABLE_SCHED_POLICY
	if (loop == NULL || out == NULL || out->size != sizeof(*out))
		return XTC_E_INVAL;
	if (out->version != XTC_SCHED_VERSION)
		return XTC_E_VERSION;
	*out = loop->sched.stats;
	out->size = sizeof(*out);
	out->version = XTC_SCHED_VERSION;
	return XTC_OK;
#else
	(void)loop;
	(void)out;
	return XTC_E_NOSYS;
#endif
}

#ifdef XTC_ENABLE_SCHED_POLICY
/* XTC_NOALLOC_BEGIN: lane policy selection and notifications */
void
__xtc_sched_freeze(xtc_loop_t *loop)
{
	int expected;
	/* Serialize with a finite pre-run descriptor copy, never user code. */
	for (;;) {
		expected = atomic_load_explicit(&loop->sched.phase, memory_order_acquire);
		if (expected == 2)
			return;
		if (expected == 0 && atomic_compare_exchange_weak_explicit(
		    &loop->sched.phase, &expected, 2, memory_order_acquire,
		    memory_order_relaxed))
			return;
	}
}

static int
__sched_ready(xtc_loop_t *loop, int lane)
{
	if (lane == -1)
		return loop->q_head != NULL || xtc_deque_len(&loop->deque) > 0;
	return lane < loop->n_classes && loop->classes[lane].in_use &&
	    loop->classes[lane].q_head != NULL;
}

static void
__sched_inc(uint64_t *value)
{
	if (*value != UINT64_MAX)
		++*value;
}

int
__xtc_sched_select(xtc_loop_t *loop)
{
	struct xtc_sched_state *s = &loop->sched;
	xtc_sched_lane_t lanes[17];
	xtc_sched_view_t view = { lanes, 0 };
	int lane, choice, forced = XTC_SCHED_DEFAULT, valid = 0;
	uint64_t oldest = 0;
	size_t i;

	for (lane = -1; lane < loop->n_classes; lane++) {
		if (!__sched_ready(loop, lane)) {
			s->bypasses[lane + 1] = 0;
			continue;
		}
		lanes[view.count].id = lane;
		lanes[view.count].shares = lane == -1 ? XTC_DEFAULT_CLASS_SHARES :
		    (uint32_t)loop->classes[lane].shares;
		lanes[view.count].runs = s->runs[lane + 1];
		lanes[view.count].bypasses = s->bypasses[lane + 1];
		lanes[view.count].ready = lane < 0 ? 0 : s->batch_ready[lane];
		lanes[view.count++].active = lane < 0 ? 0 : s->batch_active[lane];
		if (s->bypasses[lane + 1] >= s->policy.max_bypass &&
		    (forced == XTC_SCHED_DEFAULT || s->bypasses[lane + 1] > oldest)) {
			forced = lane;
			oldest = s->bypasses[lane + 1];
		}
	}
	if (view.count == 0)
		return XTC_SCHED_DEFAULT;
	__sched_inc(&s->stats.selections);
	choice = s->policy.select(&view, s->policy.user);
	for (i = 0; i < view.count; i++)
		if (lanes[i].id == choice)
			valid = 1;
	if (choice != XTC_SCHED_DEFAULT && !valid)
		__sched_inc(&s->stats.invalid_choices);
	if (forced != XTC_SCHED_DEFAULT
#if XTC_DST_BUG(XTC_DST_BUG_SCHEDFAIR)
	    && 0
#endif
	    ) {
		if (choice != forced)
			__sched_inc(&s->stats.fairness_overrides);
		return forced;
	}
	if (!valid)
		return XTC_SCHED_DEFAULT;
	return choice;
}

void
__xtc_sched_service(xtc_loop_t *loop, int lane)
{
	int i;
	/* Rotate on actual service, not a preference overridden by the guard. */
	if (lane >= 0 && loop->sched.batch_attached[lane])
		loop->sched.batch_next = ((unsigned)lane + 1) % 16;
	for (i = -1; i < loop->n_classes; i++) {
		if (i == lane || !__sched_ready(loop, i))
			loop->sched.bypasses[i + 1] = 0;
		else
			__sched_inc(&loop->sched.bypasses[i + 1]);
	}
	__sched_inc(&loop->sched.runs[lane + 1]);
}

void
__xtc_sched_ran(xtc_loop_t *loop, int lane, int verdict)
{
	xtc_sched_run_t run;
	int i;
	/* Reset empty lanes after verdict processing, so a newcomer never
	 * inherits the age of a lane that drained or parked. */
	for (i = -1; i < loop->n_classes; i++)
		if (!__sched_ready(loop, i))
			loop->sched.bypasses[i + 1] = 0;
	if (loop->sched.policy.ran == NULL)
		return;
	run.lane = lane;
	run.units = 1;
	switch (verdict) {
	case XTC_TASK_DONE: run.verdict = XTC_SCHED_DONE; break;
	case XTC_TASK_RESCHED: run.verdict = XTC_SCHED_YIELD; break;
	case XTC_TASK_PENDING: run.verdict = XTC_SCHED_PARK; break;
	default: return;
	}
	loop->sched.policy.ran(&run, loop->sched.policy.user);
}
/* XTC_NOALLOC_END */
#endif
