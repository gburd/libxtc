/* Bounded batching contract tests. No timing oracle or per-request allocation.
 */
#include "xtc_int.h"
#include "xtc_batch.h"
#include "xtc_proc.h"
#include "xtc_inspect.h"
#include "xtc_preempt.h"
#include "preempt_int.h"
#include "xtc_async.h"
#include "loop_int.h"
#include <assert.h>
#include <stdio.h>

#ifdef XTC_ENABLE_SCHED_POLICY
struct request {
	int left, calls, outcome, completions, block;
};
struct context {
	xtc_loop_t *loop;
	xtc_batch_t *batch;
	struct request a, b, c;
	uint64_t ha, hb, hc, last_done;
	int mode, calls, vector2, peer, in_callback, hook_entered, hook_done;
	volatile int preempt_peer;
	xtc_pid_t driver;
};

static void
parking_hook(void *arg)
{
	struct context *c = arg;
	c->hook_entered = 1;
	xtc_yield();
	c->hook_done = 1;
}

static int
step(const xtc_batch_item_t *items, xtc_batch_result_t *results, size_t n,
     void *arg)
{
	struct context *c = arg;
	xtc_batch_stats_t stats = { .size = sizeof(stats), .version = 1 };
	uint64_t h;
	size_t i;
	uint64_t k;
	volatile uint64_t acc = 0;
	assert(__xtc_unsafe_depth() == 0);
	c->calls++;
	c->driver = xtc_self();
	if (c->mode == 9) {
		assert(xtc_proc_at_exit(parking_hook, c) == XTC_OK);
		assert(xtc_batch_close(c->batch, XTC_BATCH_CANCEL)
		       == XTC_E_DEADLK);
	}
	if (c->mode == 10) {
		(void)xtc_exit_self(1);
		assert(0);
	}
	if (c->mode == 11) {
		results[0].state = XTC_BATCH_ERROR;
		results[0].error = 123; /* not an existing error */
		return XTC_OK;
	}
	if (n == 2)
		c->vector2++;
	assert(xtc_batch_close(c->batch, XTC_BATCH_DRAIN) == XTC_E_DEADLK);
	assert(xtc_batch_submit(c->batch, NULL, 1, &h) == XTC_E_DEADLK);
	assert(xtc_batch_get_stats(c->batch, &stats) == XTC_OK);
	if (c->mode == 14) {
		c->in_callback = 1;
		for (k = 0; k < 400000000 && !c->preempt_peer; k++)
			acc += (k ^ (k << 1)) + (acc >> 3);
		assert(c->preempt_peer);
		c->in_callback = 0;
	}
	if (c->mode == 4)
		return XTC_E_IO;
	if (c->mode == 5)
		return XTC_OK; /* UNSET must fail, never retry. */
	if (c->mode == 6 && c->calls == 1) {
		c->in_callback = 1;
		xtc_yield();
		assert(c->peer);
		c->in_callback = 0;
	}
	for (i = 0; i < n; i++) {
		if (i != 0)
			assert(items[i - 1].handle < items[i].handle);
		struct request *r = items[i].request;
		r->calls++;
		results[i].state =
		    r->block ?
		        XTC_BATCH_BLOCKED :
		        (--r->left == 0 ? XTC_BATCH_DONE : XTC_BATCH_CONTINUE);
	}
	return XTC_OK;
}

static void
complete(uint64_t handle, void *request, int outcome, void *arg)
{
	struct context *c = arg;
	struct request *r = request;
	assert(__xtc_unsafe_depth() == 0);
	assert(handle != 0);
	assert(xtc_batch_destroy(c->batch) == XTC_E_DEADLK);
	r->completions++;
	assert(c->mode == 20 || r->completions == 1);
	r->outcome = outcome;
	c->last_done = handle;
	if (c->mode == 12) {
		(void)xtc_exit_self(1);
		assert(0);
	}
	/* Continuous refill must finish the late short member before A. */
	if (c->mode == 1 && r == &c->c)
		assert(c->a.completions == 0);
}

static xtc_batch_opts_t
options(struct context *c)
{
	xtc_batch_opts_t o = { .size = sizeof(o),
		               .version = 1,
		               .capacity = 4,
		               .max_active = 2,
		               .resource_capacity = 5,
		               .step = step,
		               .complete = complete,
		               .user = c };
	return o;
}

static void
peer(void *arg)
{
	struct context *c = arg;
	xtc_batch_stats_t s = { .size = sizeof(s), .version = 1 };
	uint64_t h;
	if (c->mode == 6 || c->mode == 14) {
		assert(c->in_callback);
		assert(xtc_batch_cancel(c->batch, c->ha) == XTC_E_DEADLK);
		assert(xtc_batch_close(c->batch, XTC_BATCH_CANCEL)
		       == XTC_E_DEADLK);
		assert(xtc_batch_destroy(c->batch) == XTC_E_DEADLK);
		c->peer = 1;
		c->preempt_peer = 1;
		xtc_yield();
	}
	if (c->mode == 1 || c->mode == 13) {
		assert((c->mode == 1 ? c->b.completions : c->a.completions)
		       == 1);
		assert(xtc_batch_submit(c->batch, &c->c, 2, &c->hc) == XTC_OK);
		assert(xtc_batch_cancel(c->batch, c->mode == 1 ? c->hb : c->ha)
		       == XTC_E_INVAL);
		assert(c->hc > c->hb);
	}
	if (c->mode == 2) {
		assert(xtc_batch_get_stats(c->batch, &s) == XTC_OK);
		assert(s.queued == 2 && s.blocked == 1
		       && s.reserved_units == 3);
		assert(c->b.calls == 0 && c->c.calls == 0); /* no FCFS bypass */
		assert(xtc_batch_resume(c->batch, c->hb) == XTC_E_INVAL);
		c->a.block = 0;
		assert(xtc_batch_resume(c->batch, c->ha) == XTC_OK);
		assert(xtc_batch_resume(c->batch, c->ha) == XTC_OK);
	}
	if (c->mode == 3) {
		assert(c->a.calls == 1);
		assert(xtc_batch_cancel(c->batch, c->ha) == XTC_OK);
	}
	if (c->mode == 7) { /* first submission after empty driver parked */
		assert(xtc_batch_submit(c->batch, &c->a, 1, &h) == XTC_OK);
	}
	assert(xtc_batch_close(c->batch, c->mode == 8 ? XTC_BATCH_CANCEL :
	                                                XTC_BATCH_DRAIN)
	       == XTC_OK);
	assert(xtc_batch_close(c->batch, XTC_BATCH_DRAIN) == XTC_OK);
	if (c->mode == 10 || c->mode == 12)
		return; /* unexpected driver exit already completed cleanup */
	assert(xtc_batch_destroy(c->batch) == XTC_E_AGAIN);
	if (c->mode == 9) {
		while (!c->hook_entered)
			xtc_yield();
		assert(!c->hook_done);
		assert(xtc_batch_destroy(c->batch) == XTC_E_AGAIN);
	}
}

static void
scenario(int mode)
{
	struct context c = { 0 };
	xtc_batch_opts_t o;
	xtc_batch_stats_t s = { .size = sizeof(s), .version = 1 };
	xtc_pid_t pid;
	uint64_t h = 99;
	c.mode = mode;
	c.a.left = mode == 1 ? 4 : 1;
	c.b.left = mode == 13 ? 4 : 1;
	c.c.left = 1;
	c.a.block = mode == 2 || mode == 3 || mode == 8;
	assert(xtc_loop_init(&c.loop) == XTC_OK);
	o = options(&c);
	o.version = 2;
	assert(xtc_batch_create(c.loop, &o, &c.batch) == XTC_E_VERSION);
	o.version = 1;
	o.max_active = 0;
	assert(xtc_batch_create(c.loop, &o, &c.batch) == XTC_E_INVAL);
	o.max_active = 2;
	assert(xtc_batch_create(c.loop, &o, &c.batch) == XTC_OK);
	assert(xtc_batch_submit(c.batch, &c.a, 6, &h) == XTC_E_RANGE
	       && h == 99);
	assert(xtc_batch_submit(c.batch, &c.a, 1, NULL) == XTC_E_INVAL);
	if (mode != 7)
		assert(xtc_batch_submit(c.batch, &c.a, 3, &c.ha) == XTC_OK);
	if (mode == 1 || mode == 2 || mode == 13) {
		assert(xtc_batch_submit(c.batch, &c.b, mode == 2 ? 3 : 2, &c.hb)
		       == XTC_OK);
		if (mode == 2)
			assert(xtc_batch_submit(c.batch, &c.c, 2, &c.hc)
			       == XTC_OK);
	}
	assert(xtc_batch_destroy(c.batch) == XTC_E_AGAIN);
	assert(xtc_proc_spawn(c.loop, peer, &c, NULL, &pid) == XTC_OK);
	assert(xtc_loop_run(c.loop) == XTC_OK);
	assert(c.a.completions == 1);
	assert(c.a.outcome
	       == (mode == 4                             ? XTC_E_IO :
	           mode == 5 || mode == 10 || mode == 11 ? XTC_E_INTERNAL :
	           mode == 3 || mode == 8                ? XTC_E_ABORTED :
	                                                   XTC_OK));
	assert(xtc_batch_get_stats(c.batch, &s) == XTC_OK);
	assert(s.queued == 0 && s.ready == 0 && s.blocked == 0
	       && s.reserved_units == 0);
	if (mode == 1)
		assert(c.vector2 >= 2 && c.c.completions == 1);
	assert(xtc_batch_destroy(c.batch) == XTC_OK);
	xtc_loop_fini(c.loop);
}
/* These detect missing size/version checks, wrong-loop permission and treating
 * close/stop as reaping. All cleanup is completed before loop finalization. */
static void
lifecycle(void)
{
	struct context c = { 0 };
	xtc_loop_t *other;
	xtc_batch_t *unused = NULL;
	xtc_batch_opts_t o;
	xtc_batch_stats_t stats = { .size = sizeof(stats), .version = 2 };
	uint64_t h[5];
	int i;
	assert(xtc_loop_init(&c.loop) == XTC_OK);
	assert(xtc_loop_init(&other) == XTC_OK);
	o = options(&c);
	c.mode = 20;
	assert(xtc_exec_class_create(other, 100, 0, &o.sched_class) == XTC_OK);
	assert(xtc_batch_create(c.loop, &o, &unused) == XTC_E_INVAL);
	assert(xtc_exec_class_create(c.loop, 100, 0, &o.sched_class) == XTC_OK);
	assert(xtc_batch_create(c.loop, &o, &c.batch) == XTC_OK);
	assert(xtc_batch_create(c.loop, &o, &unused) == XTC_E_INVAL);
	assert(xtc_batch_get_stats(c.batch, &stats) == XTC_E_VERSION);
	stats.version = 1;
	stats.size--;
	assert(xtc_batch_get_stats(c.batch, &stats) == XTC_E_INVAL);
	__xtc_current_loop = other;
	assert(xtc_batch_submit(c.batch, &c.a, 1, &h[0]) == XTC_E_INVAL);
	assert(xtc_batch_destroy(c.batch) == XTC_E_INVAL);
	__xtc_current_loop = NULL;
	for (i = 0; i < 4; i++) {
		assert(xtc_batch_submit(c.batch, &c.a, 1, &h[i]) == XTC_OK);
		assert(xtc_batch_cancel(c.batch, h[i]) == XTC_OK);
	}
	assert(xtc_batch_submit(c.batch, &c.a, 1, &h[4]) == XTC_E_AGAIN);
	/* Distinct requests for the exactly-once callback assertion. */
	assert(xtc_batch_close(c.batch, XTC_BATCH_CANCEL) == XTC_OK);
	assert(xtc_batch_close(c.batch, XTC_BATCH_DRAIN) == XTC_OK);
	assert(xtc_batch_destroy(c.batch) == XTC_E_AGAIN);
	/* Complete tracks per request, so this fixture uses another callback.
	 */
	assert(xtc_loop_run(c.loop) == XTC_OK);
	assert(c.a.calls == 0 && c.a.completions == 4);
	assert(xtc_batch_create(c.loop, &o, &unused) == XTC_E_AGAIN);
	assert(xtc_batch_destroy(c.batch) == XTC_OK);
	assert(xtc_loop_fini(c.loop) == XTC_OK);
	assert(xtc_loop_fini(other) == XTC_OK);
	c = (struct context) { 0 };
	assert(xtc_loop_init(&c.loop) == XTC_OK);
	o = options(&c);
	assert(xtc_batch_create(c.loop, &o, &c.batch) == XTC_OK);
	assert(xtc_batch_close(c.batch, XTC_BATCH_DRAIN) == XTC_OK);
	assert(xtc_batch_destroy(c.batch) == XTC_E_AGAIN);
	assert(xtc_loop_run(c.loop) == XTC_OK);
	assert(xtc_batch_destroy(c.batch) == XTC_OK);
	assert(xtc_loop_fini(c.loop) == XTC_OK);
}

static void
independent_loop_teardown(void)
{
	struct context a = {0}, b = {0};
	xtc_batch_opts_t o;

	assert(xtc_loop_init(&a.loop) == XTC_OK);
	assert(xtc_loop_init(&b.loop) == XTC_OK);
	o = options(&a);
	assert(xtc_batch_create(a.loop, &o, &a.batch) == XTC_OK);
	o = options(&b);
	assert(xtc_batch_create(b.loop, &o, &b.batch) == XTC_OK);
	assert(xtc_batch_close(a.batch, XTC_BATCH_DRAIN) == XTC_OK);
	assert(xtc_loop_run(a.loop) == XTC_OK);
	/* Equal PID values on standalone loops must not join another driver. */
	assert(xtc_batch_destroy(a.batch) == XTC_OK);
	assert(xtc_batch_destroy(b.batch) == XTC_E_AGAIN);
	assert(xtc_batch_close(b.batch, XTC_BATCH_DRAIN) == XTC_OK);
	assert(xtc_loop_run(b.loop) == XTC_OK);
	assert(xtc_batch_destroy(b.batch) == XTC_OK);
	assert(xtc_loop_fini(a.loop) == XTC_OK);
	assert(xtc_loop_fini(b.loop) == XTC_OK);
}

#ifndef __has_feature
#define __has_feature(x) 0
#endif
static void
involuntary_callback(void)
{
#if !defined(__SANITIZE_ADDRESS__) && !defined(__SANITIZE_THREAD__)            \
    && !__has_feature(address_sanitizer) && !__has_feature(thread_sanitizer)
	extern int __xtc_coro_preempt_effective(void);
	if (__xtc_coro_preempt_effective() && xtc_preempt_supported()) {
		xtc_preempt_set_involuntary(1);
		assert(xtc_preempt_arm(1000000) == XTC_OK);
		scenario(14);
		assert(xtc_preempt_disarm() == XTC_OK);
		xtc_preempt_set_involuntary(0);
	} else
		puts("batch: involuntary callback test skipped (substrate)");
#else
	puts("batch: involuntary callback test skipped (sanitizer)");
#endif
}

static void
stop_peer(void *arg)
{
	struct context *c = arg;
	assert(xtc_loop_stop(c->loop) == XTC_OK);
}
static void
close_peer(void *arg)
{
	struct context *c = arg;
	assert(xtc_batch_destroy(c->batch) == XTC_E_AGAIN);
	assert(xtc_batch_close(c->batch, XTC_BATCH_CANCEL) == XTC_OK);
}
static void
stop_is_not_drain(void)
{
	struct context c = { 0 };
	xtc_batch_opts_t o;
	xtc_pid_t pid;
	assert(xtc_loop_init(&c.loop) == XTC_OK);
	o = options(&c);
	assert(xtc_batch_create(c.loop, &o, &c.batch) == XTC_OK);
	assert(xtc_proc_spawn(c.loop, stop_peer, &c, NULL, &pid) == XTC_OK);
	assert(xtc_loop_run(c.loop) == XTC_OK);
	assert(xtc_batch_destroy(c.batch) == XTC_E_AGAIN);
	assert(xtc_proc_spawn(c.loop, close_peer, &c, NULL, &pid) == XTC_OK);
	assert(xtc_loop_run(c.loop) == XTC_OK);
	assert(xtc_batch_destroy(c.batch) == XTC_OK);
	assert(xtc_loop_fini(c.loop) == XTC_OK);
}

static void
kill_peer(void *arg)
{
	struct context *c = arg;
	int i;
	/* The first vector blocked, and two driver turns consume the stored
	 * submission signal then park on the indefinite notify waiter. */
	for (i = 0; i < 4; i++)
		xtc_yield();
	assert(c->a.calls == 1 && c->a.completions == 0);
	assert(xtc_exit_pid(c->driver, 1) == XTC_OK);
	for (i = 0; i < 4; i++)
		xtc_yield();
	assert(c->a.completions == 0); /* kill alone does not wake notify */
	assert(xtc_batch_close(c->batch, XTC_BATCH_CANCEL) == XTC_OK);
}
static void
masked_wait_kill(void)
{
	struct context c = { 0 };
	xtc_batch_opts_t o;
	xtc_pid_t pid;
	assert(xtc_loop_init(&c.loop) == XTC_OK);
	o = options(&c);
	c.a.block = 1;
	assert(xtc_batch_create(c.loop, &o, &c.batch) == XTC_OK);
	assert(xtc_batch_submit(c.batch, &c.a, 1, &c.ha) == XTC_OK);
	assert(xtc_proc_spawn(c.loop, kill_peer, &c, NULL, &pid) == XTC_OK);
	assert(xtc_loop_run(c.loop) == XTC_OK);
	assert(c.a.completions == 1 && c.a.outcome == XTC_E_ABORTED);
	assert(xtc_batch_destroy(c.batch) == XTC_OK);
	assert(xtc_loop_fini(c.loop) == XTC_OK);
}

extern void __xtc_batch_set_last_handle(xtc_batch_t *, uint64_t);

/* The last identity is UINT64_MAX; exhaustion never wraps onto a live one. */
static void
exhaust_peer(void *arg)
{
	struct context *c = arg;
	xtc_batch_stats_t s = { .size = sizeof(s), .version = 1 };
	uint64_t h = 99;
	while (c->a.completions == 0)
		xtc_yield();
	/* Open group, free slots, retired last handle: still exhausted. */
	assert(xtc_batch_submit(c->batch, &c->b, 1, &h) == XTC_E_RANGE
	       && h == 99);
	assert(xtc_batch_cancel(c->batch, UINT64_MAX) == XTC_E_INVAL);
	assert(xtc_batch_cancel(c->batch, 1) == XTC_E_INVAL);
	assert(xtc_batch_get_stats(c->batch, &s) == XTC_OK);
	assert(s.completed == 1 && s.rejections == 2 && s.queued == 0);
	assert(xtc_batch_close(c->batch, XTC_BATCH_DRAIN) == XTC_OK);
}

static void
handle_exhaustion(void)
{
	struct context c = { 0 };
	xtc_batch_opts_t o;
	xtc_batch_stats_t s = { .size = sizeof(s), .version = 1 };
	xtc_pid_t pid;
	uint64_t h = 99;
	assert(xtc_loop_init(&c.loop) == XTC_OK);
	o = options(&c);
	c.a.left = 1;
	assert(xtc_batch_create(c.loop, &o, &c.batch) == XTC_OK);
	__xtc_batch_set_last_handle(c.batch, UINT64_MAX - 1);
	assert(xtc_batch_submit(c.batch, &c.a, 1, &c.ha) == XTC_OK
	       && c.ha == UINT64_MAX);
	assert(xtc_batch_submit(c.batch, &c.b, 1, &h) == XTC_E_RANGE
	       && h == 99);
	assert(xtc_batch_get_stats(c.batch, &s) == XTC_OK);
	assert(s.rejections == 1 && s.queued == 1); /* no slot taken */
	assert(xtc_batch_cancel(c.batch, 1) == XTC_E_INVAL); /* no alias */
	assert(xtc_proc_spawn(c.loop, exhaust_peer, &c, NULL, &pid) == XTC_OK);
	assert(xtc_loop_run(c.loop) == XTC_OK);
	assert(c.a.completions == 1 && c.a.outcome == XTC_OK
	       && c.last_done == UINT64_MAX);
	assert(c.b.calls == 0 && c.b.completions == 0);
	assert(xtc_batch_destroy(c.batch) == XTC_OK);
	assert(xtc_loop_fini(c.loop) == XTC_OK);
}

/* resource_capacity == UINT64_MAX: a sum that would wrap is a refusal, and
 * the refused head blocks a younger member that would fit (strict FCFS). */
static int
charge_step(const xtc_batch_item_t *items, xtc_batch_result_t *results,
            size_t n, void *arg)
{
	struct context *c = arg;
	xtc_batch_stats_t s = { .size = sizeof(s), .version = 1 };
	size_t i;
	assert(xtc_batch_get_stats(c->batch, &s) == XTC_OK);
	switch (c->calls++) {
	case 0:
		assert(n == 1 && items[0].handle == c->ha);
		assert(s.reserved_units == UINT64_MAX - 1 && s.queued == 3);
		break;
	case 1:
		assert(n == 2 && items[0].handle == c->hb
		       && items[1].handle == c->hc);
		assert(s.reserved_units == 3 && s.queued == 1);
		break;
	default:
		assert(n == 1 && items[0].handle > c->hc);
		assert(s.reserved_units == UINT64_MAX && s.queued == 0);
	}
	for (i = 0; i < n; i++)
		results[i].state = XTC_BATCH_DONE;
	return XTC_OK;
}

static void
charge_limits(void)
{
	struct context c = { 0 };
	struct request d = { 0 };
	xtc_batch_opts_t o;
	xtc_batch_stats_t s = { .size = sizeof(s), .version = 1 };
	uint64_t h;
	assert(xtc_loop_init(&c.loop) == XTC_OK);
	o = options(&c);
	o.max_active = 3;
	o.resource_capacity = UINT64_MAX;
	o.step = charge_step;
	assert(xtc_batch_create(c.loop, &o, &c.batch) == XTC_OK);
	assert(xtc_batch_submit(c.batch, &c.a, UINT64_MAX - 1, &c.ha)
	       == XTC_OK);
	assert(xtc_batch_submit(c.batch, &c.b, 2, &c.hb) == XTC_OK);
	assert(xtc_batch_submit(c.batch, &c.c, 1, &c.hc) == XTC_OK);
	assert(xtc_batch_submit(c.batch, &d, UINT64_MAX, &h) == XTC_OK);
	assert(xtc_batch_close(c.batch, XTC_BATCH_DRAIN) == XTC_OK);
	assert(xtc_loop_run(c.loop) == XTC_OK);
	assert(c.calls == 3 && c.a.completions == 1 && c.b.completions == 1
	       && c.c.completions == 1 && d.completions == 1);
	assert(xtc_batch_get_stats(c.batch, &s) == XTC_OK);
	assert(s.reserved_units == 0 && s.completed == 4 && s.rejections == 0);
	assert(xtc_batch_destroy(c.batch) == XTC_OK);
	assert(xtc_loop_fini(c.loop) == XTC_OK);
}

static struct __os_alloc_hook saved_hook;
static size_t alloc_attempt, fail_at, alloc_live, alloc_cap;
static int alloc_deny;

static int
alloc_fail(void)
{
	alloc_attempt++;
	return alloc_deny || (fail_at != 0 && alloc_attempt == fail_at);
}
static void *
test_malloc(size_t size)
{
	void *p;
	if (alloc_fail())
		return NULL;
	p = saved_hook.malloc(size);
	if (p != NULL)
		alloc_live++;
	return p;
}
static void *
test_calloc(size_t n, size_t size)
{
	void *p;
	if (alloc_fail()
	    || (alloc_cap != 0 && size != 0 && n > alloc_cap / size))
		return NULL;
	p = saved_hook.calloc(n, size);
	if (p != NULL)
		alloc_live++;
	return p;
}
static void *
test_realloc(void *old, size_t size)
{
	void *p;
	if (alloc_fail())
		return NULL;
	p = saved_hook.realloc(old, size);
	if (p != NULL && old == NULL)
		alloc_live++;
	return p;
}
static void
test_free(void *p)
{
	if (p != NULL) {
		assert(alloc_live > 0);
		alloc_live--;
	}
	saved_hook.free(p);
}
static void *
test_aligned(size_t align, size_t size)
{
	void *p;
	if (alloc_fail())
		return NULL;
	p = saved_hook.aligned(align, size);
	if (p != NULL)
		alloc_live++;
	return p;
}
static void
test_aligned_free(void *p)
{
	if (p != NULL) {
		assert(alloc_live > 0);
		alloc_live--;
	}
	saved_hook.aligned_free(p);
}

static void
allocation_failures(void)
{
	struct __os_alloc_hook h = { test_malloc,  test_calloc,
		                     test_realloc, test_free,
		                     test_aligned, test_aligned_free };
	struct context c;
	xtc_batch_opts_t o;
	size_t nth, attempts;
	int rc;
	assert(__os_alloc_get_hook(&saved_hook) == XTC_OK);
	assert(__os_alloc_set_hook(&h) == XTC_OK);
	for (nth = 1; nth < 100; nth++) {
		c = (struct context) { 0 };
		assert(xtc_loop_init(&c.loop) == XTC_OK);
		o = options(&c);
		alloc_attempt = 0;
		fail_at = nth;
		rc = xtc_batch_create(c.loop, &o, &c.batch);
		attempts = alloc_attempt;
		fail_at = 0;
		if (rc == XTC_OK) {
			assert(xtc_batch_close(c.batch, XTC_BATCH_CANCEL)
			       == XTC_OK);
			assert(xtc_loop_run(c.loop) == XTC_OK);
			assert(xtc_batch_destroy(c.batch) == XTC_OK);
		} else {
			assert(rc == XTC_E_NOMEM);
			assert(c.batch == NULL);
		}
		/* Lazy proc-table registration belongs to the loop, not the
		 * group. */
		assert(xtc_loop_fini(c.loop) == XTC_OK);
		assert(alloc_live == 0);
		if (nth > attempts)
			break;
	}
	assert(nth < 100 && nth > 6);
	assert(__os_alloc_set_hook(&saved_hook) == XTC_OK);
}

/* UINT32_MAX slots overflow size_t on ILP32 => RANGE before any allocation.
 * LP64 cannot overflow; the hook stands in for a refusing allocator. */
static void
storage_overflow(void)
{
	struct __os_alloc_hook h = { test_malloc,  test_calloc,
	                             test_realloc, test_free,
	                             test_aligned, test_aligned_free };
	struct context c = { 0 };
	xtc_batch_opts_t o;
	int want = SIZE_MAX <= UINT32_MAX ? XTC_E_RANGE : XTC_E_NOMEM;
	assert(__os_alloc_get_hook(&saved_hook) == XTC_OK);
	assert(__os_alloc_set_hook(&h) == XTC_OK);
	assert(xtc_loop_init(&c.loop) == XTC_OK);
	o = options(&c);
	o.capacity = UINT32_MAX;
	o.max_active = 1;
	alloc_cap = (size_t)1 << 30;
	assert(xtc_batch_create(c.loop, &o, &c.batch) == want);
	alloc_cap = 0;
	assert(c.batch == NULL);
	assert(xtc_loop_fini(c.loop) == XTC_OK);
	assert(alloc_live == 0);
	assert(__os_alloc_set_hook(&saved_hook) == XTC_OK);
	printf("batch: storage overflow => %s\n",
	       want == XTC_E_RANGE ? "RANGE" : "NOMEM (LP64)");
}

static void
noalloc_peer(void *arg)
{
	struct context *c = arg;
	/* Driver ran empty and parked. Owner submission/wake must not allocate.
	 */
	alloc_deny = 1;
	alloc_attempt = 0;
	assert(xtc_batch_submit(c->batch, &c->a, 1, &c->ha) == XTC_OK);
	xtc_yield();
	assert(c->a.calls == 1);
	assert(xtc_batch_resume(c->batch, c->ha) == XTC_OK);
	assert(xtc_batch_cancel(c->batch, c->ha) == XTC_OK);
	assert(xtc_batch_close(c->batch, XTC_BATCH_CANCEL) == XTC_OK);
}

static void
noalloc(void)
{
	struct __os_alloc_hook h = { test_malloc,  test_calloc,
		                     test_realloc, test_free,
		                     test_aligned, test_aligned_free };
	struct context c = { 0 };
	xtc_batch_opts_t o;
	xtc_pid_t pid;
	assert(__os_alloc_get_hook(&saved_hook) == XTC_OK);
	assert(__os_alloc_set_hook(&h) == XTC_OK);
	assert(xtc_loop_init(&c.loop) == XTC_OK);
	o = options(&c);
	c.a.block = 1;
	assert(xtc_batch_create(c.loop, &o, &c.batch) == XTC_OK);
	assert(xtc_proc_spawn(c.loop, noalloc_peer, &c, NULL, &pid) == XTC_OK);
	assert(xtc_loop_run(c.loop) == XTC_OK);
	assert(alloc_attempt == 0);
	alloc_deny = 0;
	assert(c.a.completions == 1 && c.a.outcome == XTC_E_ABORTED);
	assert(xtc_batch_destroy(c.batch) == XTC_OK);
	assert(xtc_loop_fini(c.loop) == XTC_OK);
	assert(alloc_live == 0);
	assert(__os_alloc_set_hook(&saved_hook) == XTC_OK);
}

#endif

int
main(void)
{
#ifdef XTC_ENABLE_SCHED_POLICY
	int i;
	handle_exhaustion();
	charge_limits();
	storage_overflow();
	for (i = 1; i <= 13; i++)
		scenario(i);
	lifecycle();
	independent_loop_teardown();
	allocation_failures();
	noalloc();
	stop_is_not_drain();
	masked_wait_kill();
	involuntary_callback();
#else
	assert(xtc_batch_create(NULL, NULL, NULL) == XTC_E_NOSYS);
	assert(xtc_batch_submit(NULL, NULL, 0, NULL) == XTC_E_NOSYS);
	assert(xtc_batch_cancel(NULL, 0) == XTC_E_NOSYS);
	assert(xtc_batch_resume(NULL, 0) == XTC_E_NOSYS);
	assert(xtc_batch_close(NULL, XTC_BATCH_CANCEL) == XTC_E_NOSYS);
	assert(xtc_batch_destroy(NULL) == XTC_E_NOSYS);
	assert(xtc_batch_get_stats(NULL, NULL) == XTC_E_NOSYS);
#endif
	puts("batch: OK");
	return 0;
}
