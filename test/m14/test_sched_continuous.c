/* Continuous policy contracts, independent of the batch driver engine. */
#include "xtc_int.h"
#include "loop_int.h"
#include "xtc_sched.h"
#include <assert.h>
#include <stdio.h>

extern int __xtc_loop_step_once(xtc_loop_t *);

#ifdef XTC_ENABLE_SCHED_POLICY
struct progress {
	xtc_loop_t *loop;
	int step, done, continuous, last[3], count[3];
};
struct worker {
	struct progress *p;
	int lane;
};

static int
work(xtc_task_t *task, void *arg)
{
	struct worker *w = arg;
	struct progress *p = w->p;
	int i = w->lane + 1;

	(void)task;
	assert(xtc_sched_continuous_set(p->loop, 3) == XTC_E_AGAIN);
	if (p->continuous)
		assert(p->step - p->last[i] <= 6);
	p->last[i] = p->step++;
	if (++p->count[i] == 40) {
		p->done++;
		return XTC_TASK_DONE;
	}
	return XTC_TASK_RESCHED;
}

static int32_t
inspect(const xtc_sched_view_t *view, void *arg)
{
	size_t i;

	(void)arg;
	for (i = 0; i < view->count; i++) {
		if (view->lanes[i].id == 0) {
			assert(view->lanes[i].ready == 7);
			assert(view->lanes[i].active == 9);
		} else {
			assert(view->lanes[i].ready == 0);
			assert(view->lanes[i].active == 0);
		}
	}
	return XTC_SCHED_DEFAULT;
}

static void
contracts(void)
{
	xtc_loop_t *loop;
	xtc_task_t dummy = {0};
	xtc_sched_policy_t policy = {0};
	xtc_sched_lane_t lanes[3] = {{0}};
	xtc_sched_view_t view = {lanes, 3};
	int i, id;

	assert(xtc_sched_continuous_set(NULL, 3) == XTC_E_INVAL);
	assert(xtc_loop_init(&loop) == XTC_OK);
	assert(xtc_sched_continuous_set(loop, 0) == XTC_E_INVAL);
	assert(__xtc_sched_batch_attach(loop, 0) == XTC_E_INVAL);
	for (i = 0; i < 16; i++) {
		assert(__xtc_loop_class_create(loop, 100, 0, &id) == XTC_OK);
		assert(id == i);
	}
	assert(__xtc_sched_batch_attach(NULL, 0) == XTC_E_INVAL);
	assert(__xtc_sched_batch_attach(loop, -1) == XTC_E_INVAL);
	assert(__xtc_sched_batch_attach(loop, 16) == XTC_E_INVAL);
	__xtc_sched_batch_hint(NULL, 0, 1, 1);
	__xtc_sched_batch_hint(loop, -1, 1, 1);
	__xtc_sched_batch_hint(loop, 16, 1, 1);
	__xtc_sched_batch_detach(NULL, 0);
	__xtc_sched_batch_detach(loop, -1);
	__xtc_sched_batch_detach(loop, 16);
	for (i = 0; i < 16; i++) {
		assert(__xtc_sched_batch_attach(loop, i) == XTC_OK);
		assert(__xtc_sched_batch_attach(loop, i) == XTC_E_INVAL);
		__xtc_sched_batch_detach(loop, i);
	}
	assert(__xtc_sched_batch_attach(loop, 0) == XTC_OK);
	assert(__xtc_sched_batch_attach(loop, 1) == XTC_OK);
	__xtc_sched_batch_hint(loop, 0, 7, 9);
	__xtc_sched_batch_hint(loop, 1, 0, 8);
	policy.size = sizeof(policy);
	policy.version = XTC_SCHED_VERSION;
	policy.max_bypass = 3;
	policy.select = inspect;
	assert(xtc_sched_policy_set(loop, &policy) == XTC_OK);
	loop->q_head = &dummy;
	loop->classes[0].q_head = &dummy;
	loop->classes[2].q_head = &dummy;
	assert(__xtc_sched_select(loop) == XTC_SCHED_DEFAULT);
	loop->q_head = NULL;
	loop->classes[0].q_head = loop->classes[2].q_head = NULL;
	assert(xtc_sched_continuous_set(loop, 3) == XTC_OK);
	/* Hints alone never manufacture runtime readiness. */
	assert(__xtc_sched_select(loop) == XTC_SCHED_DEFAULT);
	loop->classes[1].q_head = &dummy;
	assert(__xtc_sched_select(loop) == XTC_SCHED_DEFAULT);
	/* DEFAULT preference cannot disable the mandatory runtime guard. */
	loop->sched.bypasses[2] = 3;
	assert(__xtc_sched_select(loop) == 1);
	loop->sched.bypasses[2] = 0;
	loop->classes[0].q_head = &dummy;
	assert(__xtc_sched_select(loop) == 0);
	__xtc_sched_batch_hint(loop, 1, 1, 8);
	__xtc_sched_service(loop, 0);
	assert(__xtc_sched_select(loop) == 1);
	__xtc_sched_service(loop, 1);
	assert(__xtc_sched_select(loop) == 0);
	/* Saturated ages tie: rotate after actual service, without wrapping. */
	loop->sched.bypasses[1] = loop->sched.bypasses[2] = UINT64_MAX;
	for (i = 0; i < 3; i++) {
		lanes[i].id = i;
		lanes[i].ready = 1;
		lanes[i].bypasses = UINT64_MAX;
	}
	assert(loop->sched.policy.select(&view, loop->sched.policy.user) == 0);
	__xtc_sched_service(loop, 0);
	assert(loop->sched.policy.select(&view, loop->sched.policy.user) == 1);
	/* An older eligible group beats the rotating tie break. */
	lanes[0].bypasses = UINT64_MAX;
	lanes[1].bypasses = 1;
	assert(loop->sched.policy.select(&view, loop->sched.policy.user) == 0);
	__xtc_sched_batch_detach(loop, 0);
	__xtc_sched_batch_detach(loop, 1);
	assert(loop->sched.policy.select(&view, loop->sched.policy.user) == XTC_SCHED_DEFAULT);
	assert(__xtc_sched_batch_attach(loop, 0) == XTC_OK);
	loop->sched.bypasses[1] = loop->sched.bypasses[2] = 0;
	/* Detach cleared the previous group's hints. */
	assert(__xtc_sched_select(loop) == XTC_SCHED_DEFAULT);
	loop->classes[0].q_head = loop->classes[1].q_head = NULL;
	assert(xtc_sched_policy_set(loop, NULL) == XTC_OK);
	assert(loop->sched.policy.select == NULL);
	atomic_store(&loop->sched.phase, 1);
	assert(xtc_sched_continuous_set(loop, 3) == XTC_E_AGAIN);
	atomic_store(&loop->sched.phase, 0);
	assert(xtc_sched_continuous_set(loop, UINT32_MAX) == XTC_OK);
	(void)__xtc_loop_step_once(loop);
	assert(xtc_sched_continuous_set(loop, 3) == XTC_E_AGAIN);
	assert(xtc_sched_policy_set(loop, NULL) == XTC_E_AGAIN);
	assert(xtc_loop_fini(loop) == XTC_OK);
}

static void
two_groups_and_heartbeat(int continuous)
{
	struct progress p = {0};
	struct worker w[3];
	xtc_sched_stats_t stats = {0};
	xtc_task_t *task;
	int i, id;

	assert(xtc_loop_init(&p.loop) == XTC_OK);
	__xtc_current_loop = p.loop;
	p.continuous = continuous;
	if (continuous)
		assert(xtc_sched_continuous_set(p.loop, 3) == XTC_OK);
	for (i = 0; i < 2; i++) {
		assert(__xtc_loop_class_create(p.loop, 100, 0, &id) == XTC_OK);
		assert(__xtc_sched_batch_attach(p.loop, id) == XTC_OK);
		__xtc_sched_batch_hint(p.loop, id, 1, 1);
	}
	for (i = 0; i < 3; i++) {
		w[i].p = &p;
		w[i].lane = i - 1;
		assert(__xtc_task_spawn_ex(p.loop, work, &w[i], 0, &task) == XTC_OK);
		assert(p.loop->q_head == task && p.loop->q_tail == task);
		p.loop->q_head = p.loop->q_tail = NULL;
		task->sched_class = w[i].lane;
		if (w[i].lane == -1)
			assert(xtc_deque_push(&p.loop->deque, task) == XTC_OK);
		else
			assert(__xtc_loop_enqueue(p.loop, task) == XTC_OK);
	}
	while (p.done != 3) {
		assert(p.step < 120);
		assert(__xtc_loop_step_once(p.loop) == 1);
	}
	assert(p.step == 120);
	stats.size = sizeof(stats);
	stats.version = XTC_SCHED_VERSION;
	assert(xtc_sched_get_stats(p.loop, &stats) == XTC_OK);
	assert(stats.invalid_choices == 0);
	if (continuous)
		assert(stats.fairness_overrides > 0);
	else
		assert(stats.selections == 0 && stats.fallbacks == 0);
	__xtc_sched_batch_detach(p.loop, 0);
	__xtc_sched_batch_detach(p.loop, 1);
	assert(xtc_loop_fini(p.loop) == XTC_OK);
	__xtc_current_loop = NULL;
}
#endif

int
main(void)
{
#ifdef XTC_ENABLE_SCHED_POLICY
	contracts();
	two_groups_and_heartbeat(0);
	two_groups_and_heartbeat(1);
#else
	assert(xtc_sched_continuous_set(NULL, 0) == XTC_E_NOSYS);
#endif
	puts("sched continuous OK");
	return 0;
}
