/* Bounded batching contract tests. No timing oracle or per-request allocation.
 */
#include "xtc_int.h"
#include "xtc_batch.h"
#include "xtc_proc.h"
#include "xtc_exec.h"
#include "xtc_accel.h"
#include "xtc_inspect.h"
#include "xtc_preempt.h"
#include "preempt_int.h"
#include "xtc_async.h"
#include "xtc_blocking.h"
#include "xtc_future.h"
#include "loop_int.h"
#include "proc_int.h" /* XTC_HAVE_PROC_KILL_WAKE */
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#if defined(__linux__)
#include <sys/eventfd.h>
#include <unistd.h>
#endif

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

/* --------------------------------------------------------------------------
 * Async-offload group tests (S1): submit/collect split, SYNC/FUTURE/BLOCKING
 * exec kinds, cancel-in-flight, teardown-while-parked, timeout, exactly-once
 * conservation. FENCE is native-only and awaited through xtc_accel_wait_fence;
 * it has no device on CI so the behavioral coverage uses FUTURE/SYNC/BLOCKING.
 * -------------------------------------------------------------------------- */
struct areq {
	int id, left, done_result, completions, outcome;
};
struct actx {
	xtc_loop_t *loop;
	xtc_batch_t *batch;
	struct areq req[4];
	enum xtc_batch_exec_kind kind;
	int submit_calls, collect_calls, cancel_hints;
	int64_t timeout_ns;
	xtc_pid_t driver;
	/* FUTURE path: submit stashes the promise so a peer resolves it on
	 * command (controls completion timing). */
	xtc_promise_t *pending_prom;
	xtc_future_t *pending_fut;
	int want_cancel; /* peer cancels this handle index while in flight */
	uint64_t handles[4];
	int accepted, completed, cancelled, failed, last_exec_status;
	int fence_fd; /* FENCE kind: eventfd signalled by a peer */
};

static int
blocking_kernel(void *arg)
{
	struct actx *c = arg;
	assert(xtc_pid_is_none(xtc_self())); /* a pool thread, not the driver loop */
	c->req[0].done_result = 1;
	return 0;
}

static int
asub(const xtc_batch_item_t *items, size_t n, void *user,
     xtc_batch_exec_t *exec)
{
	struct actx *c = user;
	struct areq *r;
	size_t i;
	c->submit_calls++;
	c->driver = xtc_self();
	exec->kind = c->kind;
	switch (c->kind) {
	case XTC_BATCH_EXEC_SYNC:
		for (i = 0; i < n; ++i) {
			r = items[i].request;
			r->done_result = (--r->left == 0);
		}
		break;
	case XTC_BATCH_EXEC_BLOCKING:
		exec->run = blocking_kernel;
		exec->arg = c;
		break;
	case XTC_BATCH_EXEC_FUTURE:
		if (xtc_future_new_pair(&c->pending_prom, &c->pending_fut)
		    != XTC_OK)
			return XTC_E_NOMEM;
		exec->future = c->pending_fut;
		for (i = 0; i < n; ++i) {
			r = items[i].request;
			r->done_result = (--r->left == 0);
		}
		break;
#if defined(__linux__)
	case XTC_BATCH_EXEC_FENCE:
		exec->fence_fd = c->fence_fd; /* app owns it; engine never closes */
		for (i = 0; i < n; ++i) {
			r = items[i].request;
			r->done_result = (--r->left == 0);
		}
		break;
#endif
	default:
		return XTC_E_INVAL;
	}
	return XTC_OK;
}

static int
acol(const xtc_batch_item_t *items, xtc_batch_result_t *results, size_t n,
     int exec_status, void *user)
{
	struct actx *c = user;
	struct areq *r;
	size_t i;
	c->collect_calls++;
	c->last_exec_status = exec_status;
	for (i = 0; i < n; ++i) {
		r = items[i].request;
		if (exec_status == XTC_BATCH_EXEC_TIMEOUT) {
			results[i].state = XTC_BATCH_ERROR;
			results[i].error = XTC_E_IO;
		} else if (r->done_result)
			results[i].state = XTC_BATCH_DONE;
		else
			results[i].state = XTC_BATCH_CONTINUE;
	}
	return XTC_OK;
}

static void
acomplete(uint64_t handle, void *request, int outcome, void *user)
{
	struct actx *c = user;
	struct areq *r = request;
	assert(handle != 0);
	r->completions++;
	assert(r->completions == 1); /* EXACTLY ONCE */
	r->outcome = outcome;
	if (outcome == XTC_OK)
		c->completed++;
	else if (outcome == XTC_E_ABORTED)
		c->cancelled++;
	else
		c->failed++;
}

static void
acancel_hint(uint64_t handle, void *user)
{
	struct actx *c = user;
	assert(handle != 0);
	c->cancel_hints++;
}

static xtc_batch_async_opts_t
aopts(struct actx *c)
{
	xtc_batch_async_opts_t o = { .size = sizeof(o),
		                     .version = XTC_BATCH_VERSION,
		                     .capacity = 4,
		                     .max_active = 2,
		                     .resource_capacity = 5,
		                     .exec_timeout_ns = c->timeout_ns,
		                     .submit = asub,
		                     .collect = acol,
		                     .complete = acomplete,
		                     .cancel_hint = acancel_hint,
		                     .user = c };
	return o;
}

/* FUTURE peer: resolve each outstanding promise as it appears, optionally
 * cancelling a member first; then close drain once all have terminated. */
static void
future_peer(void *arg)
{
	struct actx *c = arg;
	int guard = 0;
	while (c->completed + c->cancelled + c->failed < c->accepted) {
		assert(++guard < 100000);
		if (c->pending_fut != NULL && c->pending_prom != NULL) {
			if (c->want_cancel >= 0) {
				assert(xtc_batch_cancel(c->batch,
				    c->handles[c->want_cancel]) == XTC_OK);
				c->want_cancel = -1;
			}
			xtc_promise_set(c->pending_prom, 0, XTC_OK);
			c->pending_prom = NULL;
			c->pending_fut = NULL;
		}
		xtc_yield();
	}
	assert(xtc_batch_close(c->batch, XTC_BATCH_DRAIN) == XTC_OK);
}

static void
drain_peer(void *arg)
{
	struct actx *c = arg;
	assert(xtc_batch_close(c->batch, XTC_BATCH_DRAIN) == XTC_OK);
}

static void
async_submit(struct actx *c, int id, uint64_t charge)
{
	assert(xtc_batch_submit(c->batch, &c->req[id], charge, &c->handles[id])
	       == XTC_OK);
	c->accepted++;
}

static void
async_finish(struct actx *c, int accepted)
{
	xtc_batch_stats_t s = { .size = sizeof(s), .version = XTC_BATCH_VERSION };
	xtc_batch_async_stats_t as = { .size = sizeof(as),
		                       .version = XTC_BATCH_VERSION };
	assert(xtc_loop_run(c->loop) == XTC_OK);
	assert(c->completed + c->cancelled + c->failed == accepted);
	assert(xtc_batch_get_stats(c->batch, &s) == XTC_OK);
	assert(s.reserved_units == 0);
	assert(s.queued == 0 && s.ready == 0 && s.blocked == 0);
	assert(xtc_batch_get_async_stats(c->batch, &as) == XTC_OK);
	/* Every successful submit is one offload; timeouts/errors are a subset. */
	assert(as.offloads == (uint64_t)c->submit_calls);
	assert(as.offload_timeouts + as.offload_errors <= as.offloads);
	assert(xtc_batch_destroy(c->batch) == XTC_OK);
	assert(xtc_loop_fini(c->loop) == XTC_OK);
}

static void
async_sync_kind(void)
{
	struct actx c = { 0 };
	xtc_batch_async_opts_t o;
	xtc_pid_t pid;
	int i;
	c.kind = XTC_BATCH_EXEC_SYNC;
	c.want_cancel = -1;
	for (i = 0; i < 3; ++i)
		c.req[i].left = i + 1; /* variable length: 1,2,3 iterations */
	assert(xtc_loop_init(&c.loop) == XTC_OK);
	o = aopts(&c);
	/* version 2 is unknown, max_active 0 is malformed, submit NULL rejected. */
	o.version = XTC_BATCH_VERSION + 1;
	assert(xtc_batch_create_async(c.loop, &o, &c.batch) == XTC_E_VERSION);
	o.version = XTC_BATCH_VERSION;
	o.max_active = 0;
	assert(xtc_batch_create_async(c.loop, &o, &c.batch) == XTC_E_INVAL);
	o.max_active = 2;
	o.submit = NULL;
	assert(xtc_batch_create_async(c.loop, &o, &c.batch) == XTC_E_INVAL);
	o = aopts(&c);
	assert(xtc_batch_create_async(c.loop, &o, &c.batch) == XTC_OK);
	{
		xtc_batch_async_stats_t as = { .size = sizeof(as),
			                       .version = XTC_BATCH_VERSION + 1 };
		assert(xtc_batch_get_async_stats(c.batch, &as) == XTC_E_VERSION);
		as.version = XTC_BATCH_VERSION;
		as.size -= 1;
		assert(xtc_batch_get_async_stats(c.batch, &as) == XTC_E_INVAL);
	}
	for (i = 0; i < 3; ++i)
		async_submit(&c, i, 1);
	assert(xtc_proc_spawn(c.loop, drain_peer, &c, NULL, &pid) == XTC_OK);
	async_finish(&c, 3);
	for (i = 0; i < 3; ++i)
		assert(c.req[i].completions == 1 && c.req[i].outcome == XTC_OK);
	assert(c.submit_calls == c.collect_calls && c.submit_calls > 0);
	puts("batch-async: SYNC variable-length exactly-once OK");
}

static void
async_blocking_kind(void)
{
	struct actx c = { 0 };
	xtc_batch_async_opts_t o;
	xtc_pid_t pid;
	c.kind = XTC_BATCH_EXEC_BLOCKING;
	c.want_cancel = -1;
	c.req[0].left = 1;
	assert(xtc_loop_init(&c.loop) == XTC_OK);
	o = aopts(&c);
	assert(xtc_batch_create_async(c.loop, &o, &c.batch) == XTC_OK);
	async_submit(&c, 0, 1);
	assert(xtc_proc_spawn(c.loop, drain_peer, &c, NULL, &pid) == XTC_OK);
	async_finish(&c, 1);
	assert(c.req[0].completions == 1 && c.req[0].outcome == XTC_OK);
	assert(c.req[0].done_result); /* the pool closure ran off-loop */
	xtc_blocking_shutdown();
	puts("batch-async: BLOCKING ran off-loop, fiber resumed OK");
}

static void
async_future_kind(void)
{
	struct actx c = { 0 };
	xtc_batch_async_opts_t o;
	xtc_pid_t pid;
	int i;
	c.kind = XTC_BATCH_EXEC_FUTURE;
	c.want_cancel = -1;
	for (i = 0; i < 2; ++i)
		c.req[i].left = i + 1;
	assert(xtc_loop_init(&c.loop) == XTC_OK);
	o = aopts(&c);
	assert(xtc_batch_create_async(c.loop, &o, &c.batch) == XTC_OK);
	for (i = 0; i < 2; ++i)
		async_submit(&c, i, 1);
	assert(xtc_proc_spawn(c.loop, future_peer, &c, NULL, &pid) == XTC_OK);
	async_finish(&c, 2);
	for (i = 0; i < 2; ++i)
		assert(c.req[i].completions == 1 && c.req[i].outcome == XTC_OK);
	puts("batch-async: FUTURE await+collect OK");
}

static void
async_cancel_inflight(void)
{
	struct actx c = { 0 };
	xtc_batch_async_opts_t o;
	xtc_pid_t pid;
	c.kind = XTC_BATCH_EXEC_FUTURE;
	c.want_cancel = 0; /* cancel member 0 while its future is outstanding */
	c.req[0].left = 3; /* multi-iteration so it is still live when cancelled */
	assert(xtc_loop_init(&c.loop) == XTC_OK);
	o = aopts(&c);
	assert(xtc_batch_create_async(c.loop, &o, &c.batch) == XTC_OK);
	async_submit(&c, 0, 1);
	assert(xtc_proc_spawn(c.loop, future_peer, &c, NULL, &pid) == XTC_OK);
	async_finish(&c, 1);
	assert(c.req[0].completions == 1 && c.req[0].outcome == XTC_E_ABORTED);
	assert(c.collect_calls >= 1 && c.cancel_hints == 1);
	puts("batch-async: cancel-in-flight -> collect+ABORTED once, hint OK");
}

static void
async_timeout(void)
{
	struct actx c = { 0 };
	xtc_batch_async_opts_t o;
	c.kind = XTC_BATCH_EXEC_FUTURE;
	c.want_cancel = -1;
	c.timeout_ns = 1000000; /* 1ms; the future is never resolved */
	c.req[0].left = 1;
	assert(xtc_loop_init(&c.loop) == XTC_OK);
	o = aopts(&c);
	assert(xtc_batch_create_async(c.loop, &o, &c.batch) == XTC_OK);
	async_submit(&c, 0, 1);
	assert(xtc_batch_close(c.batch, XTC_BATCH_DRAIN) == XTC_OK);
	async_finish(&c, 1);
	assert(c.req[0].completions == 1 && c.req[0].outcome == XTC_E_IO);
	assert(c.last_exec_status == XTC_BATCH_EXEC_TIMEOUT);
	if (c.pending_prom != NULL)
		xtc_promise_drop(c.pending_prom);
	puts("batch-async: timeout -> collect sees TIMEOUT, member failed OK");
}

/* Teardown-while-parked (the B1 regression): the driver parks on a FUTURE that
 * never resolves; xtc_batch_close(CANCEL) signals the group notify, which the
 * engine's FUTURE await also waits on, so the await wakes, observes the cancel,
 * and batch_collect_inflight finalizes the in-flight member exactly once
 * (collect still runs so the app can free its device buffer) with no leak/UAF.
 * The engine owns and discards the future read half; the test owns the
 * promise. */
static void
teardown_peer(void *arg)
{
	struct actx *c = arg;
	int i;
	for (i = 0; i < 8; ++i)
		xtc_yield();
	assert(c->submit_calls == 1 && c->collect_calls == 0);
	assert(xtc_batch_close(c->batch, XTC_BATCH_CANCEL) == XTC_OK);
}

static void
async_teardown_while_parked(void)
{
	struct actx c = { 0 };
	xtc_batch_async_opts_t o;
	xtc_pid_t pid;
	c.kind = XTC_BATCH_EXEC_FUTURE;
	c.want_cancel = -1;
	c.req[0].left = 5; /* never resolved; stays in flight until the cancel */
	assert(xtc_loop_init(&c.loop) == XTC_OK);
	o = aopts(&c);
	assert(xtc_batch_create_async(c.loop, &o, &c.batch) == XTC_OK);
	async_submit(&c, 0, 1);
	assert(xtc_proc_spawn(c.loop, teardown_peer, &c, NULL, &pid) == XTC_OK);
	assert(xtc_loop_run(c.loop) == XTC_OK);
	/* exactly-once ABORTED, collect ran, hint fired, no leak. */
	assert(c.req[0].completions == 1 && c.req[0].outcome == XTC_E_ABORTED);
	assert(c.collect_calls == 1 && c.submit_calls == 1);
	assert(c.cancel_hints == 1);
	assert(xtc_batch_destroy(c.batch) == XTC_OK);
	/* The engine consumed (discarded) the future; the test keeps the
	 * promise and drops it. Dropping after discard is a safe refcount
	 * no-op on the cell (ASan proves no UAF/leak either ordering). */
	if (c.pending_prom != NULL)
		xtc_promise_drop(c.pending_prom);
	assert(xtc_loop_fini(c.loop) == XTC_OK);
	puts("batch-async: teardown-while-parked finalized exactly-once OK");
}

/* The other ordering: promise ALREADY set (future resolved) but the group is
 * cancel-closed in the same turn. Exactly-once holds; the engine discards the
 * resolved future exactly once; the promise handle was consumed by set. */
static void
teardown_resolved_peer(void *arg)
{
	struct actx *c = arg;
	int i;
	for (i = 0; i < 8; ++i)
		xtc_yield();
	if (c->pending_prom != NULL) {
		xtc_promise_set(c->pending_prom, 0, XTC_OK);
		c->pending_prom = NULL;
		c->pending_fut = NULL;
	}
	assert(xtc_batch_close(c->batch, XTC_BATCH_CANCEL) == XTC_OK);
}

static void
async_teardown_resolved(void)
{
	struct actx c = { 0 };
	xtc_batch_async_opts_t o;
	xtc_pid_t pid;
	c.kind = XTC_BATCH_EXEC_FUTURE;
	c.want_cancel = -1;
	c.req[0].left = 5;
	assert(xtc_loop_init(&c.loop) == XTC_OK);
	o = aopts(&c);
	assert(xtc_batch_create_async(c.loop, &o, &c.batch) == XTC_OK);
	async_submit(&c, 0, 1);
	assert(xtc_proc_spawn(c.loop, teardown_resolved_peer, &c, NULL, &pid)
	       == XTC_OK);
	assert(xtc_loop_run(c.loop) == XTC_OK);
	/* Exactly once; cancel overrides a ready result to ABORTED, or the
	 * await observed ready first (OK) -- either way once, no leak. */
	assert(c.req[0].completions == 1);
	assert(c.req[0].outcome == XTC_E_ABORTED || c.req[0].outcome == XTC_OK);
	assert(xtc_batch_destroy(c.batch) == XTC_OK);
	assert(xtc_loop_fini(c.loop) == XTC_OK);
	puts("batch-async: teardown with resolved future finalized once OK");
}

/* Edge-triggered cancel: the driver parks on a FUTURE that is NEVER resolved
 * (no device completion, no timeout). close(CANCEL) resolves the engine-owned
 * cancel promise, which wakes the when_any wait so the member finalizes as
 * ABORTED exactly once -- collect still runs, the hint fires, no leak. The
 * DETERMINISTIC proof that the wake is edge-triggered (not the old 1ms slice)
 * lives in the sim test (test_sim_batch.c aproducer): there the virtual clock
 * only advances on xtc_proc_sleep, so a slice timer could never fire under the
 * bare-yield cancel path, yet the member still terminates. Here we assert the
 * finalize is correct and prompt (bounded by loop turns, no timeout). */
static void
cancel_wake_peer(void *arg)
{
	struct actx *c = arg;
	int i;
	for (i = 0; i < 8; ++i)
		xtc_yield();
	assert(c->submit_calls == 1 && c->collect_calls == 0);
	/* No sleep, no promise resolution: the cancel promise is the only wake
	 * source for the never-resolving device future. */
	assert(xtc_batch_close(c->batch, XTC_BATCH_CANCEL) == XTC_OK);
}

static void
async_cancel_wakes_immediately(void)
{
	struct actx c = { 0 };
	xtc_batch_async_opts_t o;
	xtc_pid_t pid;
	c.kind = XTC_BATCH_EXEC_FUTURE;
	c.want_cancel = -1;
	c.req[0].left = 5; /* never resolved; only close(CANCEL) ends it */
	assert(xtc_loop_init(&c.loop) == XTC_OK);
	o = aopts(&c);
	assert(xtc_batch_create_async(c.loop, &o, &c.batch) == XTC_OK);
	async_submit(&c, 0, 1);
	assert(xtc_proc_spawn(c.loop, cancel_wake_peer, &c, NULL, &pid) == XTC_OK);
	/* The loop returns only once the group drains; with no completion and no
	 * timeout, the ONLY path to drain is the cancel-promise edge wake. */
	assert(xtc_loop_run(c.loop) == XTC_OK);
	assert(c.req[0].completions == 1 && c.req[0].outcome == XTC_E_ABORTED);
	assert(c.collect_calls == 1 && c.cancel_hints == 1);
	assert(xtc_batch_destroy(c.batch) == XTC_OK);
	if (c.pending_prom != NULL)
		xtc_promise_drop(c.pending_prom);
	assert(xtc_loop_fini(c.loop) == XTC_OK);
	puts("batch-async: cancel-close edge-wakes the await (no slice) OK");
}

/* FENCE kind (native only): submit hands the engine an eventfd; a peer signals
 * it to complete the op. Proves the FENCE await (xtc_accel_wait_fence) wires
 * end-to-end. The app owns the fd; the engine never closes it. */
#if defined(__linux__)
static void
fence_peer(void *arg)
{
	struct actx *c = arg;
	uint64_t one = 1;
	int i;
	for (i = 0; i < 6; ++i)
		xtc_yield();
	/* Signal the fence: the eventfd becomes readable, waking the await. */
	assert(write(c->fence_fd, &one, sizeof(one)) == (ssize_t)sizeof(one));
	assert(xtc_batch_close(c->batch, XTC_BATCH_DRAIN) == XTC_OK);
}

static void
async_fence_kind(void)
{
	struct actx c = { 0 };
	xtc_batch_async_opts_t o;
	xtc_pid_t pid;
	c.kind = XTC_BATCH_EXEC_FENCE;
	c.want_cancel = -1;
	c.req[0].left = 1;
	c.fence_fd = eventfd(0, EFD_NONBLOCK);
	if (c.fence_fd < 0) {
		puts("batch-async: FENCE test skipped (no eventfd)");
		return;
	}
	assert(xtc_loop_init(&c.loop) == XTC_OK);
	o = aopts(&c);
	assert(xtc_batch_create_async(c.loop, &o, &c.batch) == XTC_OK);
	async_submit(&c, 0, 1);
	assert(xtc_proc_spawn(c.loop, fence_peer, &c, NULL, &pid) == XTC_OK);
	async_finish(&c, 1);
	assert(c.req[0].completions == 1 && c.req[0].outcome == XTC_OK);
	(void)close(c.fence_fd); /* the engine never closed it (app owns it) */
	puts("batch-async: FENCE await via eventfd OK");
}
#else
static void
async_fence_kind(void)
{
	puts("batch-async: FENCE test skipped (non-Linux)");
}
#endif

/* --------------------------------------------------------------------------
 * Cross-loop admission tests (S2): submit_mt/cancel_mt/resume_mt from producer
 * fibers on sibling loops into a group on the model loop. The group uses the
 * SYNC exec kind (step runs inline in submit) so correctness is deterministic
 * and no device model is needed. Each request carries a reply future the
 * complete callback resolves, demonstrating the cross-loop result pattern the
 * engine does NOT provide itself.
 * -------------------------------------------------------------------------- */
#include <stdatomic.h>

struct mt_req {
	int id, left, outcome, completions, ran;
	xtc_promise_t *reply;
	xtc_future_t *reply_fut;
};
struct mt_ctx {
	xtc_exec_t *exec;
	xtc_batch_t *batch;
	struct mt_req req[8];
	int n_req;
	_Atomic int completed; /* complete cb fired (driver loop) */
	_Atomic int producers_done;
	int n_producers;
	int submit_rejections; /* producer-observed XTC_E_AGAIN */
};

/* SYNC step: do the member's work inline, mark DONE after `left` iterations. */
static int
mt_submit(const xtc_batch_item_t *items, size_t n, void *user,
          xtc_batch_exec_t *exec)
{
	struct mt_req *r;
	size_t i;
	(void)user;
	exec->kind = XTC_BATCH_EXEC_SYNC;
	for (i = 0; i < n; ++i) {
		r = items[i].request;
		r->ran++;
		if (r->left > 0)
			r->left--;
	}
	return XTC_OK;
}

static int
mt_collect(const xtc_batch_item_t *items, xtc_batch_result_t *results, size_t n,
           int exec_status, void *user)
{
	struct mt_req *r;
	size_t i;
	(void)exec_status;
	(void)user;
	for (i = 0; i < n; ++i) {
		r = items[i].request;
		results[i].state = r->left == 0 ? XTC_BATCH_DONE :
		                                 XTC_BATCH_CONTINUE;
	}
	return XTC_OK;
}

static void
mt_complete(uint64_t handle, void *request, int outcome, void *user)
{
	struct mt_ctx *c = user;
	struct mt_req *r = request;
	assert(handle != 0);
	r->completions++;
	assert(r->completions == 1); /* EXACTLY ONCE */
	r->outcome = outcome;
	(void)atomic_fetch_add_explicit(&c->completed, 1, memory_order_relaxed);
	/* Resolve the submitter's reply future (cross-loop result routing). */
	if (r->reply != NULL) {
		(void)xtc_promise_set(r->reply, (intptr_t)outcome, XTC_OK);
		r->reply = NULL;
	}
}

static xtc_batch_async_opts_t
mt_opts(struct mt_ctx *c, uint32_t cap, uint32_t mt_queue_cap)
{
	xtc_batch_async_opts_t o = { .size = sizeof(o),
		                     .version = XTC_BATCH_VERSION,
		                     .capacity = cap,
		                     .max_active = cap,
		                     .resource_capacity = cap,
		                     .mt_queue_cap = mt_queue_cap,
		                     .submit = mt_submit,
		                     .collect = mt_collect,
		                     .complete = mt_complete,
		                     .user = c };
	return o;
}

/* Producer fiber on a sibling loop: submit each assigned request via submit_mt,
 * then await its reply future -- proving admission AND completion cross loops. */
struct mt_producer {
	struct mt_ctx *c;
	int first, count;
};

static void
mt_producer_fn(void *arg)
{
	struct mt_producer *p = arg;
	struct mt_ctx *c = p->c;
	int i;
	for (i = p->first; i < p->first + p->count; ++i) {
		uint64_t h = 0;
		intptr_t val = 0;
		int rc;
		/* The reply pair is created on the producer; the driver's
		 * complete cb resolves it. */
		assert(xtc_future_new_pair(&c->req[i].reply, &c->req[i].reply_fut)
		       == XTC_OK);
		for (;;) {
			rc = xtc_batch_submit_mt(c->batch, &c->req[i], 1, &h);
			if (rc == XTC_OK)
				break;
			assert(rc == XTC_E_AGAIN); /* ring briefly full */
			xtc_yield();
		}
		assert(h != 0);
		/* Await the result on THIS loop. */
		assert(xtc_future_wait(c->req[i].reply_fut, &val, -1) == XTC_OK);
		c->req[i].reply_fut = NULL;
		assert(val == XTC_OK);
	}
	(void)atomic_fetch_add_explicit(&c->producers_done, 1,
	    memory_order_relaxed);
}

/* Controller on the model loop: once every member has completed and every
 * producer has finished awaiting, close the group (owner-loop call) so the
 * driver exits and the executor idle-stops. */
static void
mt_controller_fn(void *arg)
{
	struct mt_ctx *c = arg;
	while (atomic_load_explicit(&c->completed, memory_order_relaxed)
	           < c->n_req
	       || atomic_load_explicit(&c->producers_done, memory_order_relaxed)
	              < c->n_producers)
		xtc_yield();
	assert(xtc_batch_close(c->batch, XTC_BATCH_DRAIN) == XTC_OK);
}

static void
mt_conservation(struct mt_ctx *c)
{
	xtc_batch_stats_t s = { .size = sizeof(s), .version = XTC_BATCH_VERSION };
	xtc_batch_async_stats_t as = { .size = sizeof(as),
		                       .version = XTC_BATCH_VERSION };
	assert(xtc_batch_get_stats(c->batch, &s) == XTC_OK);
	/* accepted == completed+cancelled+failed, reserved back to 0. */
	assert(s.reserved_units == 0);
	assert(s.queued == 0 && s.ready == 0 && s.blocked == 0);
	assert(s.completed + s.cancelled + s.failed
	       == (uint64_t)atomic_load_explicit(&c->completed,
	              memory_order_relaxed));
	assert(xtc_batch_get_async_stats(c->batch, &as) == XTC_OK);
	(void)as;
}

static void
mt_multiproducer(void)
{
	struct mt_ctx c = { 0 };
	struct mt_producer p[2];
	xtc_batch_async_opts_t o;
	xtc_pid_t pid;
	int i;
	c.n_req = 6;
	c.n_producers = 2;
	for (i = 0; i < c.n_req; ++i)
		c.req[i].left = (i % 3) + 1; /* variable length 1..3 */
	assert(xtc_exec_init(&c.exec, 3) == XTC_OK);
	o = mt_opts(&c, 8, 8);
	assert(xtc_batch_create_async(xtc_exec_loop(c.exec, 0), &o, &c.batch)
	       == XTC_OK);
	p[0] = (struct mt_producer){ &c, 0, 3 };
	p[1] = (struct mt_producer){ &c, 3, 3 };
	assert(xtc_proc_spawn(xtc_exec_loop(c.exec, 1), mt_producer_fn, &p[0],
	    NULL, &pid) == XTC_OK);
	assert(xtc_proc_spawn(xtc_exec_loop(c.exec, 2), mt_producer_fn, &p[1],
	    NULL, &pid) == XTC_OK);
	assert(xtc_proc_spawn(xtc_exec_loop(c.exec, 0), mt_controller_fn, &c,
	    NULL, &pid) == XTC_OK);
	assert(xtc_exec_run(c.exec) == XTC_OK);
	for (i = 0; i < c.n_req; ++i) {
		assert(c.req[i].completions == 1 && c.req[i].outcome == XTC_OK);
		assert(c.req[i].ran >= 1);
	}
	mt_conservation(&c);
	{
		xtc_batch_async_stats_t as = { .size = sizeof(as),
			                       .version = XTC_BATCH_VERSION };
		assert(xtc_batch_get_async_stats(c.batch, &as) == XTC_OK);
		assert(as.mt_queued >= (uint64_t)c.n_req);
		assert(as.mt_rejections == 0);
	}
	assert(xtc_batch_destroy(c.batch) == XTC_OK);
	assert(xtc_exec_fini(c.exec) == XTC_OK);
	puts("batch-mt: 2 producer loops, reply-future completion, exactly-once OK");
}

/* Overflow: a tiny ring/pool with no draining (driver never runs the boundary
 * because we hold off the controller) forces submit_mt to return XTC_E_AGAIN
 * and count mt_rejections, with no handle issued and no leak. Done single-
 * threaded, pre-run, so the driver has not yet drained anything. */
static void
mt_overflow(void)
{
	struct mt_ctx c = { 0 };
	xtc_batch_async_opts_t o;
	uint64_t h;
	int i, rejects = 0;
	xtc_batch_async_stats_t as = { .size = sizeof(as),
		                       .version = XTC_BATCH_VERSION };
	c.n_req = 0;
	assert(xtc_exec_init(&c.exec, 1) == XTC_OK);
	/* mt_queue_cap 2 (rounds to 2); the free-pool holds 2 records. The
	 * driver proc exists but the executor is NOT running, so nothing drains
	 * the ring: the 3rd claim finds the pool empty -> AGAIN. */
	o = mt_opts(&c, 4, 2);
	assert(xtc_batch_create_async(xtc_exec_loop(c.exec, 0), &o, &c.batch)
	       == XTC_OK);
	for (i = 0; i < 5; ++i) {
		int rc = xtc_batch_submit_mt(c.batch, &c.req[i], 1, &h);
		if (rc == XTC_E_AGAIN)
			rejects++;
		else
			assert(rc == XTC_OK && h != 0);
	}
	assert(rejects >= 1); /* at least one over-cap submit was rejected */
	assert(xtc_batch_get_async_stats(c.batch, &as) == XTC_OK);
	assert(as.mt_rejections == (uint64_t)rejects);
	/* charge out of range and mt-disabled paths. */
	assert(xtc_batch_submit_mt(c.batch, &c.req[0], 0, &h) == XTC_E_RANGE);
	assert(xtc_batch_submit_mt(c.batch, &c.req[0], 999, &h) == XTC_E_RANGE);
	/* Close + run: every accepted hand-in still completes exactly once
	 * (ABORTED by the cancel-close), then the driver exits. */
	assert(xtc_batch_close(c.batch, XTC_BATCH_CANCEL) == XTC_OK);
	assert(xtc_exec_run(c.exec) == XTC_OK);
	assert(atomic_load(&c.completed) == 5 - rejects);
	for (i = 0; i < 5 - rejects; ++i)
		assert(c.req[i].completions == 1
		       && c.req[i].outcome == XTC_E_ABORTED);
	assert(xtc_batch_destroy(c.batch) == XTC_OK);
	assert(xtc_exec_fini(c.exec) == XTC_OK);
	puts("batch-mt: ring overflow -> AGAIN, rejections counted, no leak OK");
}

/* mt-disabled group (mt_queue_cap == 0): submit_mt returns XTC_E_NOSYS. */
static void
mt_disabled(void)
{
	struct mt_ctx c = { 0 };
	xtc_batch_async_opts_t o;
	uint64_t h;
	assert(xtc_exec_init(&c.exec, 1) == XTC_OK);
	o = mt_opts(&c, 4, 0); /* mt_queue_cap 0 */
	assert(xtc_batch_create_async(xtc_exec_loop(c.exec, 0), &o, &c.batch)
	       == XTC_OK);
	assert(xtc_batch_submit_mt(c.batch, &c.req[0], 1, &h) == XTC_E_NOSYS);
	assert(xtc_batch_cancel_mt(c.batch, 1) == XTC_E_NOSYS);
	assert(xtc_batch_resume_mt(c.batch, 1) == XTC_E_NOSYS);
	assert(xtc_batch_close(c.batch, XTC_BATCH_CANCEL) == XTC_OK);
	assert(xtc_exec_run(c.exec) == XTC_OK);
	assert(xtc_batch_destroy(c.batch) == XTC_OK);
	assert(xtc_exec_fini(c.exec) == XTC_OK);
	puts("batch-mt: mt_queue_cap 0 -> submit_mt NOSYS OK");
}

extern int __xtc_batch_mt_inject(xtc_batch_t *, int, uint64_t, void *, uint64_t);

/* MAJOR 3 regression: "a cancel is never lost" decayed after mt_queue_cap
 * stale cancels. A cancel_mt for a handle that already completed (the
 * client-disconnect race) used to be parked forever in a bounded early-
 * control set; once mt_queue_cap of them had accumulated, a later cancel
 * that needed remembering was silently dropped. Here 3 * mt_queue_cap
 * stale cancels are issued, then a cancel_mt from a DIFFERENT producer
 * loop races its target while that target is HELD behind a full group: the
 * target must still end ABORTED without ever running, and nothing may have
 * been retained for the stale cancels (they are discarded on drain). */
struct stale_ctx {
	struct mt_ctx *c;
	uint64_t victim;
	_Atomic int phase;
};
static struct stale_ctx g_stale;

static int
mt_block_submit(const xtc_batch_item_t *items, size_t n, void *user,
                xtc_batch_exec_t *exec)
{
	size_t i;
	(void)user;
	exec->kind = XTC_BATCH_EXEC_SYNC;
	for (i = 0; i < n; ++i)
		((struct mt_req *)items[i].request)->ran++;
	return XTC_OK;
}

/* req[0] blocks (holding the only slot) until phase 2; every other
 * request finishes in one step. */
static int
mt_block_collect(const xtc_batch_item_t *items, xtc_batch_result_t *results,
                 size_t n, int exec_status, void *user)
{
	struct mt_ctx *c = user;
	size_t i;
	(void)exec_status;
	for (i = 0; i < n; ++i)
		results[i].state = items[i].request == &c->req[0]
		        && atomic_load(&g_stale.phase) < 2 ?
		    XTC_BATCH_BLOCKED : XTC_BATCH_DONE;
	return XTC_OK;
}

static void
mt_stale_canceller(void *arg)
{
	struct mt_ctx *c = arg;
	int i;
	while (atomic_load(&g_stale.phase) < 1)
		xtc_yield();
	/* Different producer than the submitter: cancel the held victim. */
	while (xtc_batch_cancel_mt(c->batch, g_stale.victim) != XTC_OK)
		xtc_yield();
	for (i = 0; i < 8; ++i)
		xtc_yield();
	atomic_store(&g_stale.phase, 2);
}

static void
mt_stale_owner(void *arg)
{
	struct mt_ctx *c = arg;
	uint64_t h;
	int i, k;
	/* 3 * mt_queue_cap stale cancels: each names a request that has
	 * already completed. */
	for (k = 0; k < 12; ++k) {
		while (xtc_batch_submit_mt(c->batch, &c->req[7], 1, &h)
		       != XTC_OK)
			xtc_yield();
		while (c->req[7].completions == 0)
			xtc_yield();
		c->req[7].completions = 0;
		while (xtc_batch_cancel_mt(c->batch, h) != XTC_OK)
			xtc_yield();
		for (i = 0; i < 4; ++i)
			xtc_yield();
	}
	/* req[0] takes the only slot and blocks; the victim (req[1]) is then
	 * held behind it. */
	while (xtc_batch_submit_mt(c->batch, &c->req[0], 1, &h) != XTC_OK)
		xtc_yield();
	while (c->req[0].ran == 0)
		xtc_yield();
	while (xtc_batch_submit_mt(c->batch, &c->req[1], 1, &g_stale.victim)
	       != XTC_OK)
		xtc_yield();
	atomic_store(&g_stale.phase, 1);
	while (atomic_load(&g_stale.phase) < 2)
		xtc_yield();
	assert(xtc_batch_resume_mt(c->batch, h) == XTC_OK);
	while (c->req[0].completions == 0 || c->req[1].completions == 0)
		xtc_yield();
	assert(xtc_batch_close(c->batch, XTC_BATCH_DRAIN) == XTC_OK);
}

static void
mt_stale_cancels(void)
{
	struct mt_ctx c = { 0 };
	xtc_batch_async_opts_t o;
	xtc_batch_stats_t s = { .size = sizeof(s), .version = XTC_BATCH_VERSION };
	xtc_pid_t pid;
	memset(&g_stale, 0, sizeof g_stale);
	g_stale.c = &c;
	assert(xtc_exec_init(&c.exec, 2) == XTC_OK);
	o = mt_opts(&c, 1, 4); /* one slot, mt_queue_cap 4 */
	o.submit = mt_block_submit;
	o.collect = mt_block_collect;
	assert(xtc_batch_create_async(xtc_exec_loop(c.exec, 0), &o, &c.batch)
	       == XTC_OK);
	assert(xtc_proc_spawn(xtc_exec_loop(c.exec, 0), mt_stale_owner, &c,
	    NULL, &pid) == XTC_OK);
	assert(xtc_proc_spawn(xtc_exec_loop(c.exec, 1), mt_stale_canceller,
	    &c, NULL, &pid) == XTC_OK);
	assert(xtc_exec_run(c.exec) == XTC_OK);
	assert(c.req[0].completions == 1 && c.req[0].outcome == XTC_OK);
	assert(c.req[1].completions == 1);
	assert(c.req[1].outcome == XTC_E_ABORTED); /* the cancel was kept */
	assert(c.req[1].ran == 0);                 /* and it never ran */
	assert(xtc_batch_get_stats(c.batch, &s) == XTC_OK);
	assert(s.completed == 13 && s.cancelled == 1 && s.rejections == 0);
	assert(xtc_batch_destroy(c.batch) == XTC_OK);
	assert(xtc_exec_fini(c.exec) == XTC_OK);
	puts("batch-mt: stale cancels discarded; a later cancel is never lost OK");
}

/* cancel_mt/resume_mt for a handle STILL IN THE RING (not yet drained) is
 * applied correctly at the boundary. Inject SUBMIT(H) then CANCEL(H) in ring
 * order (same producer order); both drain in the same boundary and the member
 * ends ABORTED, never having run. */
static void
mt_cancel_in_ring(void)
{
	struct mt_ctx c = { 0 };
	xtc_batch_async_opts_t o;
	xtc_pid_t pid;
	uint64_t H = 1;
	c.n_req = 1;
	c.n_producers = 0;
	c.req[0].left = 2;
	assert(xtc_exec_init(&c.exec, 1) == XTC_OK);
	o = mt_opts(&c, 4, 4);
	assert(xtc_batch_create_async(xtc_exec_loop(c.exec, 0), &o, &c.batch)
	       == XTC_OK);
	assert(__xtc_batch_mt_inject(c.batch, 0 /*MT_SUBMIT*/, H, &c.req[0], 1)
	       == XTC_OK);
	assert(__xtc_batch_mt_inject(c.batch, 1 /*MT_CANCEL*/, H, NULL, 0)
	       == XTC_OK);
	assert(xtc_proc_spawn(xtc_exec_loop(c.exec, 0), mt_controller_fn, &c,
	    NULL, &pid) == XTC_OK);
	assert(xtc_exec_run(c.exec) == XTC_OK);
	assert(c.req[0].completions == 1 && c.req[0].outcome == XTC_E_ABORTED);
	assert(c.req[0].ran == 0);
	mt_conservation(&c);
	assert(xtc_batch_destroy(c.batch) == XTC_OK);
	assert(xtc_exec_fini(c.exec) == XTC_OK);
	puts("batch-mt: cancel of a handle still in the ring applied at boundary OK");
}

/* B2 CONTENTION: force genuinely concurrent pool claims. Several producer
 * fibers on sibling loops (one OS thread per loop) hammer submit_mt back to
 * back -- no await between calls -- so their batch_mt_claim / batch_mt_release
 * on the shared free-pool overlap in real time. A single-consumer pool would
 * hand the same record to two racers here (double-claim/double-release); the
 * mpmc pool serialises consumers so each claim pops a distinct record. The
 * driver drains and completes every admitted hand-in; conservation at the end
 * (completed+cancelled+failed == admitted, reserved back to 0) is the invariant
 * a double-claim would violate, and TSan sees the concurrent try_recv/try_send.
 */
struct mt_hammer {
	struct mt_ctx *c;
	int count;
};

static void
mt_hammer_fn(void *arg)
{
	struct mt_hammer *h = arg;
	struct mt_ctx *c = h->c;
	int i;
	for (i = 0; i < h->count; ++i) {
		uint64_t handle = 0;
		int rc;
		for (;;) {
			/* request==NULL: the complete cb only counts here. */
			rc = xtc_batch_submit_mt(c->batch, NULL, 1, &handle);
			if (rc == XTC_OK)
				break;
			assert(rc == XTC_E_AGAIN);
			xtc_yield();
		}
		assert(handle != 0);
	}
	(void)atomic_fetch_add_explicit(&c->producers_done, 1,
	    memory_order_relaxed);
}

/* Count-only complete cb (no per-request reply future). */
static void
mt_count_complete(uint64_t handle, void *request, int outcome, void *user)
{
	struct mt_ctx *c = user;
	(void)request;
	(void)outcome;
	assert(handle != 0);
	(void)atomic_fetch_add_explicit(&c->completed, 1, memory_order_relaxed);
}

/* SYNC step that marks every member DONE in one pass (NULL requests). */
static int
mt_count_submit(const xtc_batch_item_t *items, size_t n, void *user,
                xtc_batch_exec_t *exec)
{
	(void)items;
	(void)n;
	(void)user;
	exec->kind = XTC_BATCH_EXEC_SYNC;
	return XTC_OK;
}

static int
mt_count_collect(const xtc_batch_item_t *items, xtc_batch_result_t *results,
                 size_t n, int exec_status, void *user)
{
	size_t i;
	(void)items;
	(void)exec_status;
	(void)user;
	for (i = 0; i < n; ++i)
		results[i].state = XTC_BATCH_DONE;
	return XTC_OK;
}

static void
mt_contended_pool(void)
{
	struct mt_ctx c = { 0 };
	struct mt_hammer hm[3];
	xtc_batch_async_opts_t o;
	xtc_pid_t pid;
	int i, admitted;
	const int per = 64, nprod = 3;
	c.n_req = per * nprod;
	c.n_producers = nprod;
	assert(xtc_exec_init(&c.exec, 4) == XTC_OK);
	/* Small pool/ring (4) relative to the hand-in count forces repeated
	 * claim/release recycling, so claims from the 3 producer threads
	 * overlap on the SAME few records -- the exact window B2 lives in. */
	o = mt_opts(&c, 16, 4);
	o.submit = mt_count_submit;
	o.collect = mt_count_collect;
	o.complete = mt_count_complete;
	assert(xtc_batch_create_async(xtc_exec_loop(c.exec, 0), &o, &c.batch)
	       == XTC_OK);
	for (i = 0; i < nprod; ++i) {
		hm[i] = (struct mt_hammer){ &c, per };
		assert(xtc_proc_spawn(xtc_exec_loop(c.exec, i + 1), mt_hammer_fn,
		    &hm[i], NULL, &pid) == XTC_OK);
	}
	assert(xtc_proc_spawn(xtc_exec_loop(c.exec, 0), mt_controller_fn, &c,
	    NULL, &pid) == XTC_OK);
	assert(xtc_exec_run(c.exec) == XTC_OK);
	/* Every admitted hand-in completes exactly once. mt_queued counts the
	 * ring pushes (== admitted + any that lost a full ring); completed is
	 * the driver's exactly-once count. A double-claim would corrupt the
	 * pool and either strand a record (completed < admitted) or complete
	 * one twice (completed > admitted). */
	admitted = atomic_load_explicit(&c.completed, memory_order_relaxed);
	assert(admitted == c.n_req);
	mt_conservation(&c);
	assert(xtc_batch_destroy(c.batch) == XTC_OK);
	assert(xtc_exec_fini(c.exec) == XTC_OK);
	puts("batch-mt: contended free-pool, concurrent claims, exactly-once OK");
}

/* BLOCKER 1 regression: a hand-in drained while every slot is taken was
 * dropped (no completion, counted as a rejection) although submit_mt had
 * already returned XTC_OK and its handle. Six hand-ins into a 2-slot group,
 * queued before the first boundary, must all complete exactly once, in
 * submission order, with zero rejections. The bounded controller turns the
 * old hang into an assertion. */
struct hold_ctx {
	struct mt_ctx *c;
	uint64_t order[8];
	_Atomic int n_order;
};
static struct hold_ctx *g_hold;

static void
mt_hold_complete(uint64_t handle, void *request, int outcome, void *user)
{
	int i;
	(void)request;
	assert(outcome == XTC_OK);
	i = atomic_fetch_add_explicit(&g_hold->n_order, 1,
	    memory_order_relaxed);
	if (i < 8)
		g_hold->order[i] = handle;
	mt_count_complete(handle, request, outcome, user);
}

static void
mt_bounded_controller_fn(void *arg)
{
	struct mt_ctx *c = arg;
	long guard = 0;
	while (atomic_load_explicit(&c->completed, memory_order_relaxed)
	    < c->n_req && ++guard < 200000)
		xtc_yield();
	assert(xtc_batch_close(c->batch, XTC_BATCH_DRAIN) == XTC_OK);
}

static void
mt_full_hold(void)
{
	struct mt_ctx c = { 0 };
	struct hold_ctx hc = { 0 };
	xtc_batch_async_opts_t o;
	xtc_batch_stats_t s = { .size = sizeof(s), .version = XTC_BATCH_VERSION };
	xtc_pid_t pid;
	uint64_t h[6];
	int i;
	hc.c = &c;
	g_hold = &hc;
	c.n_req = 6;
	assert(xtc_exec_init(&c.exec, 1) == XTC_OK);
	o = mt_opts(&c, 2, 8);
	o.submit = mt_count_submit;
	o.collect = mt_count_collect;
	o.complete = mt_hold_complete;
	assert(xtc_batch_create_async(xtc_exec_loop(c.exec, 0), &o, &c.batch)
	       == XTC_OK);
	for (i = 0; i < 6; ++i)
		assert(xtc_batch_submit_mt(c.batch, NULL, 1, &h[i]) == XTC_OK);
	assert(xtc_proc_spawn(xtc_exec_loop(c.exec, 0),
	    mt_bounded_controller_fn, &c, NULL, &pid) == XTC_OK);
	assert(xtc_exec_run(c.exec) == XTC_OK);
	assert(atomic_load(&c.completed) == 6);
	for (i = 0; i < 6; ++i)
		assert(hc.order[i] == h[i]); /* admission order preserved */
	assert(xtc_batch_get_stats(c.batch, &s) == XTC_OK);
	assert(s.rejections == 0 && s.completed == 6 && s.queued == 0);
	assert(xtc_batch_destroy(c.batch) == XTC_OK);
	assert(xtc_exec_fini(c.exec) == XTC_OK);
	puts("batch-mt: hand-ins beyond capacity held, all completed in order OK");
}

/* MAJOR 2 regression: submit_mt into a closed or closing group returned
 * XTC_OK and a handle that never completed. Now (a) after close every
 * submit_mt returns XTC_E_AGAIN without touching *out, before and after the
 * driver has gone; (b) with plain-thread producers racing close(DRAIN),
 * every XTC_OK handle completes exactly once. */
struct close_ctx {
	struct mt_ctx *c;
	_Atomic int closed, ok;
	_Atomic int done[4096];
};
static struct close_ctx g_close;

static void
mt_close_complete(uint64_t handle, void *request, int outcome, void *user)
{
	(void)request;
	(void)user;
	assert(outcome == XTC_OK);
	assert(handle < 4096);
	assert(atomic_fetch_add(&g_close.done[handle], 1) == 0);
	(void)atomic_fetch_add_explicit(&g_close.c->completed, 1,
	    memory_order_relaxed);
}

static void *
mt_close_producer(void *arg)
{
	struct mt_ctx *c = arg;
	uint64_t h;
	int rc;
	for (;;) {
		h = 0;
		rc = xtc_batch_submit_mt(c->batch, NULL, 1, &h);
		if (rc == XTC_OK) {
			assert(h != 0 && h < 4096);
			(void)atomic_fetch_add(&g_close.ok, 1);
		} else {
			assert(rc == XTC_E_AGAIN && h == 0);
			if (atomic_load(&g_close.closed))
				break;
		}
		if (atomic_load(&g_close.ok) >= 3000)
			break;
	}
	return NULL;
}

static void
mt_close_owner(void *arg)
{
	struct mt_ctx *c = arg;
	while (atomic_load(&g_close.ok) < 200)
		xtc_yield();
	assert(xtc_batch_close(c->batch, XTC_BATCH_DRAIN) == XTC_OK);
	atomic_store(&g_close.closed, 1);
}

static void
mt_closed(void)
{
	struct mt_ctx c = { 0 };
	xtc_batch_async_opts_t o;
	xtc_pid_t pid;
	pthread_t th[2];
	uint64_t h = 77;
	int i;
	/* (a) closed pre-run, then after the driver has exited. */
	assert(xtc_exec_init(&c.exec, 1) == XTC_OK);
	o = mt_opts(&c, 4, 4);
	o.submit = mt_count_submit;
	o.collect = mt_count_collect;
	o.complete = mt_count_complete;
	assert(xtc_batch_create_async(xtc_exec_loop(c.exec, 0), &o, &c.batch)
	       == XTC_OK);
	assert(xtc_batch_close(c.batch, XTC_BATCH_DRAIN) == XTC_OK);
	assert(xtc_batch_submit_mt(c.batch, NULL, 1, &h) == XTC_E_AGAIN);
	assert(h == 77);
	assert(xtc_exec_run(c.exec) == XTC_OK);
	for (i = 0; i < 8; ++i)
		assert(xtc_batch_submit_mt(c.batch, NULL, 1, &h) == XTC_E_AGAIN);
	assert(h == 77 && atomic_load(&c.completed) == 0);
	assert(xtc_batch_destroy(c.batch) == XTC_OK);
	assert(xtc_exec_fini(c.exec) == XTC_OK);
	/* (b) producers racing close(DRAIN). */
	memset(&c, 0, sizeof c);
	memset(&g_close, 0, sizeof g_close);
	g_close.c = &c;
	assert(xtc_exec_init(&c.exec, 1) == XTC_OK);
	o = mt_opts(&c, 4, 8);
	o.submit = mt_count_submit;
	o.collect = mt_count_collect;
	o.complete = mt_close_complete;
	assert(xtc_batch_create_async(xtc_exec_loop(c.exec, 0), &o, &c.batch)
	       == XTC_OK);
	assert(xtc_proc_spawn(xtc_exec_loop(c.exec, 0), mt_close_owner, &c,
	    NULL, &pid) == XTC_OK);
	for (i = 0; i < 2; ++i)
		assert(pthread_create(&th[i], NULL, mt_close_producer, &c) == 0);
	assert(xtc_exec_run(c.exec) == XTC_OK);
	for (i = 0; i < 2; ++i)
		assert(pthread_join(th[i], NULL) == 0);
	assert(atomic_load(&c.completed) == atomic_load(&g_close.ok));
	assert(xtc_batch_destroy(c.batch) == XTC_OK);
	assert(xtc_exec_fini(c.exec) == XTC_OK);
	printf("batch-mt: closed group refuses submit_mt; %d hand-ins racing "
	       "close all completed once OK\n", atomic_load(&g_close.ok));
}

static void
mt_tests(void)
{
	mt_disabled();
	mt_overflow();
	mt_multiproducer();
	mt_full_hold();
	mt_closed();
	mt_contended_pool();
	mt_stale_cancels();
	mt_cancel_in_ring();
}

/* ----- placement helpers (S3) ---------------------------------------- */

/* loop_for_device: a probed device (or a synthesized renderD128 when the
 * host reports none) must map to a VALID loop index and XTC_OK, never a
 * garbage/out-of-range index. On every single-NUMA local host the device
 * numa_node reads -1 (or the host is single-node), so the documented
 * loop-0 fallback is the correct answer -- not a failure. NULL args give
 * XTC_E_INVAL. */
static void
placement_loop_for_device(void)
{
	xtc_exec_t *exec;
	xtc_accel_dev_t devs[8];
	xtc_accel_dev_t synth;
	int n_found = 0, n_loops, idx, i, rc;
	assert(xtc_exec_init(&exec, 3) == XTC_OK);
	n_loops = xtc_exec_n_loops(exec);
	assert(n_loops == 3);
	/* NULL-arg validation. */
	assert(xtc_batch_loop_for_device(NULL, &synth, &idx) == XTC_E_INVAL);
	assert(xtc_batch_loop_for_device(exec, NULL, &idx) == XTC_E_INVAL);
	assert(xtc_accel_probe(NULL, 0, &n_found) == XTC_OK);
	memset(devs, 0, sizeof devs);
	if (n_found > 0) {
		int got = n_found > 8 ? 8 : n_found;
		assert(xtc_accel_probe(devs, 8, &n_found) == XTC_OK);
		for (i = 0; i < got; ++i) {
			idx = -42;
			rc = xtc_batch_loop_for_device(exec, &devs[i], &idx);
			assert(rc == XTC_OK);
			assert(idx >= 0 && idx < n_loops);
		}
	}
	/* Always exercise the sysfs read + fallback with a synthesized
	 * device, so the path is covered even on a host with no GPU/NPU. */
	memset(&synth, 0, sizeof synth);
	synth.kind = XTC_ACCEL_KIND_GPU;
	snprintf(synth.name, sizeof synth.name, "renderD128");
	snprintf(synth.node, sizeof synth.node, "/dev/dri/renderD128");
	idx = -42;
	rc = xtc_batch_loop_for_device(exec, &synth, &idx);
	assert(rc == XTC_OK);
	assert(idx >= 0 && idx < n_loops); /* valid index, never garbage */
	/* A bogus device name is unknown topology -> loop 0 fallback, XTC_OK. */
	memset(&synth, 0, sizeof synth);
	snprintf(synth.name, sizeof synth.name, "nonsense");
	idx = -42;
	assert(xtc_batch_loop_for_device(exec, &synth, &idx) == XTC_OK);
	assert(idx == 0);
	assert(xtc_exec_fini(exec) == XTC_OK);
	puts("batch-place: loop_for_device valid index + loop-0 fallback OK");
}

/* Controller that closes 4 real groups once each has drained, so the
 * executor stops and the groups can be destroyed. Used by the router
 * test to prove the groups stay usable after the router is destroyed. */
struct place_ctx {
	xtc_exec_t *exec;
	xtc_batch_t *g[4];
	struct mt_req req[4];
	struct mt_ctx *mc; /* shared completion counter (mt_complete user) */
	int n;
};

/* One closer fiber per shard loop: wait until every member has completed
 * (the shared atomic, bumped by mt_complete on each driver loop) then close
 * THIS loop's group. xtc_batch_close is owner-loop-only, so each group must
 * be closed from a fiber on its own loop -- there is no cross-loop close_mt;
 * this is the documented create-on-chosen-loop discipline for teardown too. */
static void
place_closer_fn(void *arg)
{
	struct place_ctx *pc = arg;
	int id = xtc_exec_loop_id();
	while (atomic_load_explicit(&pc->mc->completed, memory_order_acquire)
	    < pc->n)
		xtc_yield();
	assert(xtc_batch_close(pc->g[id], XTC_BATCH_DRAIN) == XTC_OK);
}

/* router: 4 real groups (one per loop), stable key->shard, destroy frees
 * ONLY the router (groups still usable), then destroy the groups normally.
 * NULL/n<=0 give XTC_E_INVAL. */
static void
placement_router(void)
{
	struct place_ctx pc = { 0 };
	struct mt_ctx mc = { 0 }; /* only to borrow mt_opts callbacks/user */
	xtc_batch_router_t *r = NULL;
	xtc_batch_async_opts_t o;
	xtc_pid_t pid;
	uint64_t key, h;
	int i, rc;
	pc.n = 4;
	pc.mc = &mc;
	mc.n_req = 0;
	assert(xtc_exec_init(&pc.exec, 4) == XTC_OK);
	for (i = 0; i < pc.n; ++i) {
		o = mt_opts(&mc, 4, 0);
		assert(xtc_batch_create_async(xtc_exec_loop(pc.exec, i), &o,
		    &pc.g[i]) == XTC_OK);
	}
	/* NULL / n<=0 validation. */
	assert(xtc_batch_router_create(NULL, 4, &r) == XTC_E_INVAL);
	assert(xtc_batch_router_create(pc.g, 0, &r) == XTC_E_INVAL);
	assert(xtc_batch_router_create(pc.g, -1, &r) == XTC_E_INVAL);
	assert(xtc_batch_router_create(pc.g, 4, NULL) == XTC_E_INVAL);
	assert(xtc_batch_route(NULL, 0) == NULL);
	/* Create the real router over the 4 groups. */
	assert(xtc_batch_router_create(pc.g, pc.n, &r) == XTC_OK);
	/* Stable key->shard: groups[key % n], and the same key always the
	 * same group. */
	for (key = 0; key < 40; ++key) {
		xtc_batch_t *b = xtc_batch_route(r, key);
		assert(b == pc.g[key % (uint64_t)pc.n]);
		assert(xtc_batch_route(r, key) == b); /* stable */
	}
	/* Destroy the router: frees ONLY the router. The groups must still be
	 * usable afterward -- submit one request to each via submit_mt-free
	 * owner path is not available pre-run here, so prove usability by
	 * submitting through the live executor below instead. */
	xtc_batch_router_destroy(r);
	r = NULL;
	/* Groups still usable: submit one request to each (owner-loop submit
	 * is only valid pre-run or on the owner loop; do it pre-run here). */
	for (i = 0; i < pc.n; ++i) {
		pc.req[i].left = 1;
		rc = xtc_batch_submit(pc.g[i], &pc.req[i], 1, &h);
		assert(rc == XTC_OK && h != 0);
	}
	/* A closer on EACH shard loop closes its own group (owner-loop rule). */
	for (i = 0; i < pc.n; ++i)
		assert(xtc_proc_spawn(xtc_exec_loop(pc.exec, i), place_closer_fn,
		    &pc, NULL, &pid) == XTC_OK);
	assert(xtc_exec_run(pc.exec) == XTC_OK);
	for (i = 0; i < pc.n; ++i) {
		assert(pc.req[i].completions == 1);
		assert(pc.req[i].outcome == XTC_OK);
		assert(xtc_batch_destroy(pc.g[i]) == XTC_OK);
	}
	assert(xtc_exec_fini(pc.exec) == XTC_OK);
	puts("batch-place: router N groups + hash, destroy frees only router OK");
}

static void
placement_tests(void)
{
	placement_loop_for_device();
	placement_router();
}

/* --------------------------------------------------------------------------
 * Async-offload contract (review 1.55 MAJOR 1/4/5/6 + the BLOCKING rc).
 * One harness: a 1-member async group whose submit hands the engine a
 * token chosen per case, a peer on the same loop that acts at a set time,
 * and a watchdog thread that turns a hang into a failure. Times are wall
 * clock (native build).
 * -------------------------------------------------------------------------- */
struct oc {
	xtc_loop_t *loop;
	xtc_batch_t *batch;
	int kind, fence_fd, blocking_rc, cont_on_timeout;
	int64_t timeout_ns;
	xtc_promise_t *prom;
	_Atomic int op_running, overlap, submits, collects, hints, completions;
	_Atomic int hint_while_running, done, stage;
	int statuses[8], outcome;
	double t0, t_collect, t_hint;
	xtc_pid_t driver;
	const char *name;
};
static struct oc g_oc;

static double
oc_now(void)
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

static int
oc_blocking_fn(void *arg)
{
	(void)arg;
	return 0;
}

static int
oc_submit(const xtc_batch_item_t *items, size_t n, void *user,
          xtc_batch_exec_t *exec)
{
	struct oc *c = user;
	xtc_future_t *f;
	(void)items;
	(void)n;
	if (atomic_load(&c->op_running))
		atomic_fetch_add(&c->overlap, 1); /* resubmitted over a live op */
	atomic_fetch_add(&c->submits, 1);
	c->driver = xtc_self();
	exec->kind = c->kind;
	switch (c->kind) {
	case XTC_BATCH_EXEC_FUTURE:
		if (xtc_future_new_pair(&c->prom, &f) != XTC_OK)
			return XTC_E_NOMEM;
		exec->future = f;
		atomic_store(&c->op_running, 1);
		break;
	case XTC_BATCH_EXEC_FENCE:
		exec->fence_fd = c->fence_fd;
		atomic_store(&c->op_running, 1);
		break;
	case XTC_BATCH_EXEC_BLOCKING:
		exec->run = oc_blocking_fn;
		break;
	default:
		break;
	}
	return XTC_OK;
}

static int
oc_collect(const xtc_batch_item_t *items, xtc_batch_result_t *results,
           size_t n, int exec_status, void *user)
{
	struct oc *c = user;
	size_t i;
	int k = atomic_fetch_add(&c->collects, 1);
	(void)items;
	if (k < 8)
		c->statuses[k] = exec_status;
	if (k == 0)
		c->t_collect = oc_now() - c->t0;
	for (i = 0; i < n; ++i)
		results[i].state = exec_status == XTC_BATCH_EXEC_OK ?
		    XTC_BATCH_DONE :
		    (c->cont_on_timeout ? XTC_BATCH_CONTINUE : XTC_BATCH_DONE);
	return XTC_OK;
}

static void
oc_hint(uint64_t handle, void *user)
{
	struct oc *c = user;
	(void)handle;
	if (atomic_fetch_add(&c->hints, 1) == 0)
		c->t_hint = oc_now() - c->t0;
	if (atomic_load(&c->op_running))
		atomic_fetch_add(&c->hint_while_running, 1);
}

static void
oc_complete(uint64_t handle, void *request, int outcome, void *user)
{
	struct oc *c = user;
	(void)handle;
	(void)request;
	assert(atomic_fetch_add(&c->completions, 1) == 0); /* exactly once */
	c->outcome = outcome;
}

static void *
oc_watchdog(void *arg)
{
	struct oc *c = arg;
	int i;
	for (i = 0; i < 500 && !atomic_load(&c->done); i++)
		usleep(10000);
	if (!atomic_load(&c->done)) {
		fprintf(stderr, "%s: hung (submits=%d collects=%d "
		    "completions=%d hints=%d)\n", c->name, atomic_load(&c->submits),
		    atomic_load(&c->collects), atomic_load(&c->completions),
		    atomic_load(&c->hints));
		_exit(3);
	}
	return NULL;
}

/* The "device": finishes the op at `ms`, from a plain thread. */
static int g_dev_ms;
static void *
oc_device(void *arg)
{
	struct oc *c = arg;
	uint64_t one = 1;
	usleep((useconds_t)g_dev_ms * 1000);
	atomic_store(&c->op_running, 0);
	if (c->kind == XTC_BATCH_EXEC_FENCE && c->fence_fd >= 0)
		assert(write(c->fence_fd, &one, sizeof one) == sizeof one);
	else if (c->prom != NULL)
		(void)xtc_promise_set(c->prom, 0, XTC_OK);
	return NULL;
}

static void
oc_setup(struct oc *c, const char *name, int kind, int64_t timeout_ns)
{
	xtc_batch_async_opts_t o = { .size = sizeof(o),
		                     .version = XTC_BATCH_VERSION,
		                     .capacity = 2,
		                     .max_active = 2,
		                     .resource_capacity = 2,
		                     .mt_queue_cap = 4,
		                     .submit = oc_submit,
		                     .collect = oc_collect,
		                     .complete = oc_complete,
		                     .cancel_hint = oc_hint };
	uint64_t h;
	memset(c, 0, sizeof *c);
	c->name = name;
	c->kind = kind;
	c->fence_fd = -1;
	c->timeout_ns = timeout_ns;
	o.exec_timeout_ns = timeout_ns;
	o.user = c;
	assert(xtc_loop_init(&c->loop) == XTC_OK);
	assert(xtc_batch_create_async(c->loop, &o, &c->batch) == XTC_OK);
	assert(xtc_batch_submit(c->batch, NULL, 1, &h) == XTC_OK);
	c->t0 = oc_now();
}

static void
oc_run(struct oc *c, void (*peer)(void *), int device_ms)
{
	pthread_t wd, dev;
	xtc_pid_t pid;
	g_dev_ms = device_ms;
	assert(pthread_create(&wd, NULL, oc_watchdog, c) == 0);
	if (device_ms >= 0)
		assert(pthread_create(&dev, NULL, oc_device, c) == 0);
	assert(xtc_proc_spawn(c->loop, peer, c, NULL, &pid) == XTC_OK);
	assert(xtc_loop_run(c->loop) == XTC_OK);
	if (device_ms >= 0)
		assert(pthread_join(dev, NULL) == 0);
	atomic_store(&c->done, 1);
	assert(pthread_join(wd, NULL) == 0);
	assert(xtc_batch_destroy(c->batch) == XTC_OK);
	assert(xtc_loop_fini(c->loop) == XTC_OK);
	printf("batch-async: %s: collects=%d status0=%d outcome=%d hints=%d "
	       "t_collect=%.3f t_hint=%.3f\n", c->name, atomic_load(&c->collects),
	       c->statuses[0], c->outcome, atomic_load(&c->hints), c->t_collect,
	       c->t_hint);
}

static void
oc_peer_drain(void *arg)
{
	struct oc *c = arg;
	assert(xtc_batch_close(c->batch, XTC_BATCH_DRAIN) == XTC_OK);
}

/* MAJOR 1: FENCE wait errors used to reach collect as EXEC_OK. */
static void
async_fence_errors(void)
{
#if defined(__linux__) && defined(XTC_HAVE_ACCEL)
	struct oc *c = &g_oc;
	xtc_batch_async_stats_t as = { .size = sizeof(as),
		                       .version = XTC_BATCH_VERSION };
	oc_setup(c, "fence fd=-1", XTC_BATCH_EXEC_FENCE, 0);
	c->fence_fd = -1;
	assert(xtc_batch_get_async_stats(c->batch, &as) == XTC_OK);
	oc_run(c, oc_peer_drain, -1);
	assert(c->statuses[0] == XTC_E_INVAL && c->outcome == XTC_E_INVAL);
	oc_setup(c, "fence closed fd", XTC_BATCH_EXEC_FENCE, 0);
	c->fence_fd = eventfd(0, 0);
	(void)close(c->fence_fd);
	oc_run(c, oc_peer_drain, -1);
	assert(c->statuses[0] == XTC_E_INVAL && c->outcome == XTC_E_INVAL);
#else
	puts("batch-async: fence error test skipped (no accel support)");
#endif
}

/* lead item: a blocking closure the pool refused (or never ran) must not
 * reach collect as EXEC_OK. exec->run == NULL makes xtc_blocking_run
 * return XTC_E_INVAL without running anything -- the same channel as the
 * pool's XTC_E_AGAIN / XTC_E_RESOURCE on the io branch. */
static int
oc_submit_norun(const xtc_batch_item_t *items, size_t n, void *user,
                xtc_batch_exec_t *exec)
{
	int rc = oc_submit(items, n, user, exec);
	exec->run = NULL;
	return rc;
}

static void
async_blocking_refused(void)
{
	struct oc *c = &g_oc;
	xtc_batch_async_opts_t o = { .size = sizeof(o),
		                     .version = XTC_BATCH_VERSION,
		                     .capacity = 1,
		                     .max_active = 1,
		                     .resource_capacity = 1,
		                     .submit = oc_submit_norun,
		                     .collect = oc_collect,
		                     .complete = oc_complete };
	xtc_batch_async_stats_t as = { .size = sizeof(as),
		                       .version = XTC_BATCH_VERSION };
	uint64_t h;
	memset(c, 0, sizeof *c);
	c->name = "blocking refused";
	c->kind = XTC_BATCH_EXEC_BLOCKING;
	o.user = c;
	assert(xtc_loop_init(&c->loop) == XTC_OK);
	assert(xtc_batch_create_async(c->loop, &o, &c->batch) == XTC_OK);
	assert(xtc_batch_submit(c->batch, NULL, 1, &h) == XTC_OK);
	assert(xtc_batch_close(c->batch, XTC_BATCH_DRAIN) == XTC_OK);
	assert(xtc_loop_run(c->loop) == XTC_OK);
	assert(xtc_batch_get_async_stats(c->batch, &as) == XTC_OK);
	assert(c->statuses[0] == XTC_E_INVAL && c->outcome == XTC_E_INVAL);
	assert(as.offload_errors == 1);
	assert(xtc_batch_destroy(c->batch) == XTC_OK);
	assert(xtc_loop_fini(c->loop) == XTC_OK);
	puts("batch-async: blocking closure refused -> negative exec_status OK");
}

/* MAJOR 4: after TIMEOUT, collect's CONTINUE must not resubmit over the
 * live op; the member fails with XTC_E_AGAIN. FUTURE, 50 ms timeout, op
 * finishes at 300 ms. */
static void
async_timeout_no_resubmit(void)
{
	struct oc *c = &g_oc;
	oc_setup(c, "timeout+CONTINUE", XTC_BATCH_EXEC_FUTURE, 50000000);
	c->cont_on_timeout = 1;
	oc_run(c, oc_peer_drain, 300);
	assert(c->statuses[0] == XTC_BATCH_EXEC_TIMEOUT);
	assert(atomic_load(&c->overlap) == 0 && atomic_load(&c->submits) == 1);
	assert(c->outcome == XTC_E_AGAIN);
}

/* MAJOR 4: a stray xtc_proc_wake must not cut a FENCE timeout. 2 s
 * budget, stray wake at 20 ms, fence at 300 ms: collect sees OK ~0.3 s. */
static void
oc_peer_stray(void *arg)
{
	struct oc *c = arg;
	while (atomic_load(&c->submits) == 0)
		xtc_yield();
	(void)xtc_proc_sleep(20000000);
	(void)xtc_proc_wake(c->driver);
	assert(xtc_batch_close(c->batch, XTC_BATCH_DRAIN) == XTC_OK);
}

static void
async_fence_stray_wake(void)
{
#if defined(__linux__) && defined(XTC_HAVE_ACCEL)
	struct oc *c = &g_oc;
	oc_setup(c, "fence stray wake", XTC_BATCH_EXEC_FENCE, 2000000000LL);
	c->fence_fd = eventfd(0, EFD_NONBLOCK);
	oc_run(c, oc_peer_stray, 300);
	(void)close(c->fence_fd);
	assert(c->statuses[0] == XTC_BATCH_EXEC_OK && c->outcome == XTC_OK);
	assert(c->t_collect >= 0.29);
#endif
}

/* MAJOR 5: close(CANCEL) must end a FENCE that never fires. */
static void
oc_peer_cancel(void *arg)
{
	struct oc *c = arg;
	while (atomic_load(&c->submits) == 0)
		xtc_yield();
	(void)xtc_proc_sleep(20000000);
	assert(xtc_batch_close(c->batch, XTC_BATCH_CANCEL) == XTC_OK);
}

static void
async_fence_close_cancel(void)
{
#if defined(__linux__) && defined(XTC_HAVE_ACCEL)
	struct oc *c = &g_oc;
	oc_setup(c, "fence close(CANCEL)", XTC_BATCH_EXEC_FENCE, 0);
	c->fence_fd = eventfd(0, EFD_NONBLOCK);
	oc_run(c, oc_peer_cancel, -1);
	(void)close(c->fence_fd);
	assert(c->statuses[0] == XTC_BATCH_EXEC_ABORTED);
	assert(c->outcome == XTC_E_ABORTED && c->t_collect < 1.0);
	assert(atomic_load(&c->hints) == 1);
#endif
}

/* MAJOR 5: a kill must end a FUTURE that never resolves. Needs the
 * kill-wake proc layer (fix155/proc): before it, a kill cannot wake a
 * parked fiber at all. */
#ifdef XTC_HAVE_PROC_KILL_WAKE
static void
oc_peer_kill(void *arg)
{
	struct oc *c = arg;
	while (atomic_load(&c->submits) == 0)
		xtc_yield();
	(void)xtc_proc_sleep(20000000);
	assert(xtc_exit_pid(c->driver, 1) == XTC_OK);
}
#endif

static void
async_future_kill(void)
{
#ifdef XTC_HAVE_PROC_KILL_WAKE
	struct oc *c = &g_oc;
	oc_setup(c, "future kill", XTC_BATCH_EXEC_FUTURE, 0);
	oc_run(c, oc_peer_kill, -1);
	assert(c->statuses[0] == XTC_BATCH_EXEC_ABORTED);
	assert(c->outcome == XTC_E_ABORTED && c->t_collect < 1.0);
	if (c->prom != NULL)
		xtc_promise_drop(c->prom);
#else
	puts("batch-async: future kill test skipped (needs proc kill-wake)");
#endif
}

/* MAJOR 6: cancel_hint fires when the cancel lands on an in-flight member
 * (op still running), for owner cancel and for cancel_mt from a plain
 * thread. Op finishes at 300 ms; the cancel lands at ~20 ms. */
static void
oc_peer_owner_cancel(void *arg)
{
	struct oc *c = arg;
	while (atomic_load(&c->submits) == 0)
		xtc_yield();
	(void)xtc_proc_sleep(20000000);
	assert(xtc_batch_cancel(c->batch, 1) == XTC_OK);
	assert(xtc_batch_close(c->batch, XTC_BATCH_DRAIN) == XTC_OK);
}

static void *
oc_mt_canceller(void *arg)
{
	struct oc *c = arg;
	while (atomic_load(&c->submits) == 0)
		usleep(1000);
	usleep(20000);
	assert(xtc_batch_cancel_mt(c->batch, 1) == XTC_OK);
	return NULL;
}

static void
async_hint_on_landing(void)
{
	struct oc *c = &g_oc;
	pthread_t t;
	int kind;
	for (kind = 0; kind < 2; ++kind) {
		oc_setup(c, kind ? "hint (FENCE, cancel_mt)" :
		                   "hint (FUTURE, owner cancel)",
		    kind ? XTC_BATCH_EXEC_FENCE : XTC_BATCH_EXEC_FUTURE, 0);
#if !defined(__linux__) || !defined(XTC_HAVE_ACCEL)
		if (kind) {
			assert(xtc_batch_close(c->batch, XTC_BATCH_CANCEL)
			       == XTC_OK);
			assert(xtc_loop_run(c->loop) == XTC_OK);
			assert(xtc_batch_destroy(c->batch) == XTC_OK);
			assert(xtc_loop_fini(c->loop) == XTC_OK);
			break;
		}
#else
		if (kind)
			c->fence_fd = eventfd(0, EFD_NONBLOCK);
#endif
		if (kind)
			assert(pthread_create(&t, NULL, oc_mt_canceller, c) == 0);
		oc_run(c, kind ? oc_peer_drain : oc_peer_owner_cancel, 300);
		if (kind) {
			assert(pthread_join(t, NULL) == 0);
			(void)close(c->fence_fd);
		}
		assert(atomic_load(&c->hints) == 1);
		assert(atomic_load(&c->hint_while_running) == 1);
		assert(c->t_hint < 0.2);
		assert(c->outcome == XTC_E_ABORTED);
	}
}

static void
async_contract_tests(void)
{
	async_fence_errors();
	async_blocking_refused();
	async_timeout_no_resubmit();
	async_fence_stray_wake();
	async_fence_close_cancel();
	async_future_kill();
	async_hint_on_landing();
}

static void
async_tests(void)
{
	async_sync_kind();
	async_future_kind();
	async_blocking_kind();
	async_cancel_inflight();
	async_timeout();
	async_teardown_while_parked();
	async_teardown_resolved();
	async_cancel_wakes_immediately();
	async_fence_kind();
	async_contract_tests();
	mt_tests();
	placement_tests();
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
	async_tests();
#else
	assert(xtc_batch_create(NULL, NULL, NULL) == XTC_E_NOSYS);
	assert(xtc_batch_submit(NULL, NULL, 0, NULL) == XTC_E_NOSYS);
	assert(xtc_batch_submit_mt(NULL, NULL, 0, NULL) == XTC_E_NOSYS);
	assert(xtc_batch_cancel(NULL, 0) == XTC_E_NOSYS);
	assert(xtc_batch_cancel_mt(NULL, 0) == XTC_E_NOSYS);
	assert(xtc_batch_resume(NULL, 0) == XTC_E_NOSYS);
	assert(xtc_batch_resume_mt(NULL, 0) == XTC_E_NOSYS);
	assert(xtc_batch_close(NULL, XTC_BATCH_CANCEL) == XTC_E_NOSYS);
	assert(xtc_batch_destroy(NULL) == XTC_E_NOSYS);
	assert(xtc_batch_get_stats(NULL, NULL) == XTC_E_NOSYS);
	{
		int idx = -42;
		xtc_accel_dev_t dev;
		memset(&dev, 0, sizeof dev);
		assert(xtc_batch_loop_for_device(NULL, &dev, &idx)
		    == XTC_E_NOSYS);
		assert(idx == 0); /* stub still writes the loop-0 fallback */
		assert(xtc_batch_router_create(NULL, 0, NULL) == XTC_E_NOSYS);
		xtc_batch_router_destroy(NULL); /* no-op, must not crash */
		assert(xtc_batch_route(NULL, 0) == NULL);
	}
#endif
	puts("batch: OK");
	return 0;
}
