/* Runtime policy contracts; internal task seam makes steal races deterministic. */
#include "xtc_int.h"
#include "xtc_sched.h"
#include "loop_int.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_sim.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern int __xtc_loop_step_once(xtc_loop_t *);
#ifdef XTC_ENABLE_SCHED_POLICY
struct context {
	xtc_loop_t *loop;
	int calls, ran, done, choice, steal, changed;
	int last[4], count[4], step;
	xtc_task_t *stolen;
};
struct worker { struct context *c; int lane, left; };

static int32_t
select_lane(const xtc_sched_view_t *v, void *arg)
{
	struct context *c = arg;
	size_t i;
	assert(v->count > 0 && v->count <= 17);
	for (i = 1; i < v->count; i++)
		assert(v->lanes[i - 1].id < v->lanes[i].id);
	assert(xtc_sched_policy_set(c->loop, NULL) == XTC_E_AGAIN);
	c->calls++;
	if (c->steal) {
		c->stolen = xtc_deque_steal(&c->loop->deque);
		c->steal = 0;
	}
	return c->choice;
}

static void
ran(const xtc_sched_run_t *r, void *arg)
{
	struct context *c = arg;
	assert(r->lane >= -1 && r->lane <= 2);
	assert(r->units == 1);
	assert(r->verdict == XTC_SCHED_DONE || r->verdict == XTC_SCHED_YIELD);
	if (c->changed)
		assert(r->lane == (c->ran == 0 ? 0 : 1));
	c->ran++;
}

static int
work(xtc_task_t *t, void *arg)
{
	struct worker *w = arg;
	struct context *c = w->c;
	int idx = w->lane + 1;
	assert(xtc_sched_policy_set(c->loop, NULL) == XTC_E_AGAIN);
	if (c->choice == 0 && !c->changed) {
		if (c->step - c->last[idx] > 7) {
			fprintf(stderr, "FAIL: scheduler fairness bound: gap=%d bound=7\n",
			    c->step - c->last[idx]);
			exit(1);
		}
		c->last[idx] = c->step;
	}
	c->step++;
	c->count[idx]++;
	if (c->changed)
		t->sched_class = 1;
	if (--w->left == 0) {
		c->done++;
		return XTC_TASK_DONE;
	}
	return XTC_TASK_RESCHED;
}

static void
scenario(int choice, int steal, int changed)
{
	struct context c = {0};
	struct worker w[4];
	xtc_sched_policy_t p = {0};
	xtc_sched_stats_t s = {0};
	xtc_task_t *t;
	int i, id, n = changed ? 1 : 4;
	assert(xtc_loop_init(&c.loop) == XTC_OK);
	__xtc_current_loop = c.loop;
	c.choice = choice; c.steal = steal; c.changed = changed;
	p.size = sizeof p; p.version = 1; p.max_bypass = 3;
	p.select = select_lane; p.ran = ran; p.user = &c;
	p.version = 2;
	assert(xtc_sched_policy_set(c.loop, &p) == XTC_E_VERSION);
	p.version = 1; p.size--;
	assert(xtc_sched_policy_set(c.loop, &p) == XTC_E_INVAL);
	p.size++; p.max_bypass = 0;
	assert(xtc_sched_policy_set(c.loop, &p) == XTC_E_INVAL);
	p.max_bypass = 3;
	assert(xtc_sched_policy_set(c.loop, &p) == XTC_OK);
	/* Descriptor is copied, not borrowed. */
	p.select = NULL;
	for (i = 0; i < 3; i++)
		assert(__xtc_loop_class_create(c.loop, 100, 0, &id) == XTC_OK);
	for (i = 0; i < n; i++) {
		w[i].c = &c; w[i].lane = changed ? 0 : i - 1; w[i].left = 40;
		assert(__xtc_task_spawn_ex(c.loop, work, &w[i], 0, &t) == XTC_OK);
		/* Standalone spawn uses the default FIFO. Move to the lane. */
		assert(c.loop->q_head == t && c.loop->q_tail == t);
		c.loop->q_head = c.loop->q_tail = NULL;
		t->sched_class = w[i].lane;
		if (w[i].lane == -1)
			assert(xtc_deque_push(&c.loop->deque, t) == XTC_OK);
		else
			assert(__xtc_loop_enqueue(c.loop, t) == XTC_OK);
	}
	if (steal) {
		assert(__xtc_loop_step_once(c.loop) == 1);
		assert(c.stolen != NULL && c.step == 1 && c.calls == 1);
		assert(__xtc_loop_enqueue(c.loop, c.stolen) == XTC_OK);
	}
	while (c.done != n) {
		assert(c.step < n * 40);
		assert(__xtc_loop_step_once(c.loop) == 1);
	}
	assert(c.ran == n * 40);
	assert(xtc_sched_policy_set(c.loop, NULL) == XTC_E_AGAIN);
	s.size = sizeof s; s.version = 1;
	assert(xtc_sched_get_stats(c.loop, &s) == XTC_OK);
	assert(s.selections == (uint64_t)c.calls);
	if (choice == 99) assert(s.invalid_choices > 0 && s.fallbacks > 0);
	if (choice == 0 && !changed) assert(s.fairness_overrides > 0);
	assert(xtc_loop_fini(c.loop) == XTC_OK);
	__xtc_current_loop = NULL;
}
#ifdef XTC_IO_BACKEND_SIM
/* Re-popping a policy-forced lane through min-vruntime breaks the
 * max_bypass + four-ready-lanes bound with unequal shares (seed 1). */
static void
buggify_fairness(uint64_t seed)
{
	struct context c = {0};
	struct worker w[4];
	xtc_sched_policy_t p = {0};
	xtc_exec_t *exec;
	xtc_task_t *t;
	int i, id, faults;

	for (faults = 0; faults <= 1; faults++) {
		memset(&c, 0, sizeof c);
		assert(xtc_exec_init(&exec, 1) == XTC_OK);
		c.loop = xtc_exec_loop(exec, 0);
		__xtc_current_loop = c.loop;
		p.size = sizeof p;
		p.version = 1;
		p.max_bypass = 3;
		p.select = select_lane;
		p.ran = ran;
		p.user = &c;
		assert(xtc_sched_policy_set(c.loop, &p) == XTC_OK);
		for (i = 0; i < 3; i++)
			assert(__xtc_loop_class_create(c.loop,
			    i == 2 ? 1 : 1000, 0, &id) == XTC_OK);
		/* Default FIFO task last: each spawn can be detached alone. */
		for (i = 3; i >= 0; i--) {
			w[i].c = &c;
			w[i].lane = i - 1;
			w[i].left = 1000;
			assert(__xtc_task_spawn_ex(c.loop, work, &w[i], 1,
			    &t) == XTC_OK);
			assert(c.loop->q_head == t && c.loop->q_tail == t);
			c.loop->q_head = c.loop->q_tail = NULL;
			t->sched_class = w[i].lane;
			assert(__xtc_loop_enqueue(c.loop, t) == XTC_OK);
		}
		__xtc_current_loop = NULL;
		if (faults)
			xtc_sim_buggify_enable(1000);
		else
			xtc_sim_buggify_disable();
		assert(xtc_sim_exec_run(exec, seed, 100000) == XTC_OK);
		assert(c.done == 4 && c.step == 4000 && c.ran == 4000);
		for (i = 0; i < 4; i++)
			assert(c.count[i] == 1000);
		assert(xtc_exec_fini(exec) == XTC_OK);
	}
	xtc_sim_buggify_disable();
}

struct replay {
	uint64_t hash;
	unsigned runs, done;
};

static int32_t
replay_select(const xtc_sched_view_t *view, void *arg)
{
	(void)arg;
	return view->lanes[0].id;
}

static void
replay_ran(const xtc_sched_run_t *run, void *arg)
{
	struct replay *r = arg;
	r->hash = r->hash * 33 + (uint64_t)(run->lane + 2) * 3 + run->verdict;
	r->runs++;
}

static void
replay_work(void *arg)
{
	struct replay *r = arg;
	int i;
	for (i = 0; i < 20; i++)
		xtc_yield();
	r->done++;
}

static void
deterministic_policy(void)
{
	xtc_exec_t *exec;
	xtc_loop_t *loop;
	xtc_exec_class_t lane;
	xtc_proc_opts_t opts;
	xtc_sched_policy_t p = {0};
	struct replay r[2];
	uint64_t seed;
	int repeat, i;
	p.size = sizeof p; p.version = 1; p.max_bypass = 3;
	p.select = replay_select; p.ran = replay_ran;
	for (seed = 1; seed <= 32; seed++) {
		memset(r, 0, sizeof r);
		for (repeat = 0; repeat < 2; repeat++) {
			assert(xtc_exec_init(&exec, 1) == XTC_OK);
			loop = xtc_exec_loop(exec, 0);
			p.user = &r[repeat];
			assert(xtc_sched_policy_set(loop, &p) == XTC_OK);
			for (i = 0; i < 4; i++) {
				memset(&opts, 0, sizeof opts);
				opts.size = sizeof opts;
				if (i != 0) {
					assert(xtc_exec_class_create(loop, 100, 0, &lane) == XTC_OK);
					opts.sched_class = lane;
				}
				assert(xtc_proc_spawn(loop, replay_work, &r[repeat], &opts, NULL) == XTC_OK);
			}
			assert(xtc_sim_exec_run(exec, seed, 10000) == XTC_OK);
			assert(r[repeat].done == 4 && r[repeat].runs == 84);
			assert(xtc_sched_policy_set(loop, NULL) == XTC_E_AGAIN);
			assert(xtc_exec_fini(exec) == XTC_OK);
		}
		assert(r[0].hash == r[1].hash);
	}
}
#endif

#endif

int
main(int argc, char **argv)
{
	xtc_loop_t *loop;
#if defined(XTC_ENABLE_SCHED_POLICY) && defined(XTC_IO_BACKEND_SIM)
	uint64_t seed, base = 1, count = 64;

	/* Corpus argv is the literal first seed, then seed count. */
	if (argc > 1)
		base = strtoull(argv[1], NULL, 0);
	if (argc > 2)
		count = strtoull(argv[2], NULL, 0);
	assert(base > 0 && count > 0 && count <= UINT64_MAX - base);
#else
	(void)argc;
	(void)argv;
#endif
	assert(xtc_loop_init(&loop) == XTC_OK);
#ifdef XTC_ENABLE_SCHED_POLICY
	assert(xtc_sched_policy_set(loop, NULL) == XTC_OK);
	(void)__xtc_loop_step_once(loop); /* Empty first step still freezes. */
	assert(xtc_sched_policy_set(loop, NULL) == XTC_E_AGAIN);
#ifdef XTC_IO_BACKEND_SIM
	/* Walk the requested seeds before the fixed native scenarios, so the
	 * planted fairness bug reports an actual first-catching seed. */
	for (seed = base; seed < base + count; seed++)
		buggify_fairness(seed);
	deterministic_policy();
#endif
	scenario(0, 0, 0);
	scenario(99, 0, 0);
	scenario(-1, 1, 0);
	scenario(0, 0, 1);
#else
	assert(xtc_sched_policy_set(loop, NULL) == XTC_E_NOSYS);
	assert(xtc_sched_get_stats(loop, NULL) == XTC_E_NOSYS);
#endif
	assert(xtc_loop_fini(loop) == XTC_OK);
	puts("sched policy OK");
	return 0;
}
