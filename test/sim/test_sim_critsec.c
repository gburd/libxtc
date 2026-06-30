#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <stdint.h>
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_sim.h"
/*
 * DST critical-section fault-point coverage.  Runs a multi-loop
 * workload (cross-loop ping/pong, which drives the scheduler's steal,
 * inbox-push, and run-queue-enqueue critical sections) under the
 * deterministic scheduler with fault points ENABLED, and asserts:
 *
 *   - the critical-section fault points planted in the runtime
 *     (sched.steal.pre_cas, sched.inbox.pre_push,
 *     sched.enqueue.post_deque_push) are actually REACHED (coverage:
 *     fault_points_seen > 0) -- the workload exercises those sections;
 *   - the fire pattern is DETERMINISTIC: the same seed fires the same
 *     points the same number of times across two runs;
 *   - enabling fault points does NOT break determinism: the run still
 *     reaches quiescence and the application result replays (the FAULT
 *     stream is isolated from the scheduling streams).
 *
 * This is the "fault injection at every critical section under DST"
 * coverage: every planted XTC_SIM_FAULT_POINT becomes a seeded,
 * reproducible probe, and the test proves the sections are reached and
 * the timing is replayable.
 */

#define N_LOOPS 4
#define N_PAIRS 16
#define N_HOPS  3

static atomic_int  g_replies;
static atomic_long g_app_hash;

static void
pong(void *arg)
{
	(void)arg;
	int hops = N_HOPS;
	while (hops-- > 0) {
		void *m = NULL;
		size_t n = 0;
		xtc_pid_t from;
		if (xtc_recv(&m, &n, -1) != XTC_OK || m == NULL)
			return;
		memcpy(&from, m, sizeof from);
		free(m);
		int r = 1;
		(void)xtc_send(from, &r, sizeof r);
	}
}

struct ping_arg { xtc_pid_t peer; long id; };
static struct ping_arg g_args[N_PAIRS];

static void
ping(void *arg)
{
	struct ping_arg *pa = arg;
	xtc_pid_t self = xtc_self();
	int hops = N_HOPS;
	while (hops-- > 0) {
		void *m = NULL;
		size_t n = 0;
		long h;
		(void)xtc_send(pa->peer, &self, sizeof self);
		if (xtc_recv(&m, &n, -1) != XTC_OK || m == NULL)
			return;
		free(m);
		atomic_fetch_add_explicit(&g_replies, 1, memory_order_relaxed);
		h = atomic_load_explicit(&g_app_hash, memory_order_relaxed);
		h = h * 1000003L + (pa->id + 1);
		atomic_store_explicit(&g_app_hash, h, memory_order_relaxed);
	}
}

/* Run once with fault points enabled at `pct`/1000; capture the per-point
 * fire counts + the application result + the breadth. */
struct run_result {
	int      replies;
	long     app_hash;
	uint64_t steal_fires;
	uint64_t inbox_fires;
	uint64_t enqueue_fires;
	int      points_seen;
	int      rc;
};

static void
run_once(uint64_t seed, unsigned pct, struct run_result *r)
{
	xtc_exec_t *e = NULL;
	int i;
	atomic_store(&g_replies, 0);
	atomic_store(&g_app_hash, 0);
	memset(r, 0, sizeof *r);

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK) { r->rc = -1; return; }

	/* Enable fault points BEFORE the run; xtc_sim_exec_run activates
	 * sim (which the points also require). */
	xtc_sim_fault_points_enable(pct);

	for (i = 0; i < N_PAIRS; i++) {
		xtc_loop_t *lp = xtc_exec_loop(e, (unsigned)(i % N_LOOPS));
		xtc_loop_t *li = xtc_exec_loop(e, (unsigned)((i + 1) % N_LOOPS));
		xtc_pid_t pong_pid;
		(void)xtc_proc_spawn(lp, pong, NULL, NULL, &pong_pid);
		g_args[i].peer = pong_pid;
		g_args[i].id = i;
		(void)xtc_proc_spawn(li, ping, &g_args[i], NULL, NULL);
	}

	r->rc = xtc_sim_exec_run(e, seed, 5000000);
	r->replies = atomic_load(&g_replies);
	r->app_hash = atomic_load(&g_app_hash);
	r->steal_fires = xtc_sim_fault_point_fires("sched.steal.pre_cas");
	r->inbox_fires = xtc_sim_fault_point_fires("sched.inbox.pre_push");
	r->enqueue_fires =
	    xtc_sim_fault_point_fires("sched.enqueue.post_deque_push");
	r->points_seen = xtc_sim_fault_points_seen();

	xtc_sim_fault_points_disable();
	(void)xtc_exec_fini(e);
}

/* A steal-inducing workload: spawn many unpinned TASKS (xtc_task_spawn,
 * which ARE stealable -- unlike procs/fibers, which are pinned because
 * a stackful coroutine cannot migrate its stack across loops) all on
 * ONE loop, so idle peer loops work-steal them.  This drives the
 * sched.enqueue.post_deque_push point (deque push) and the
 * sched.steal.pre_cas point (a thief stealing from the loaded loop). */
#define N_STEAL_WORKERS 64
static atomic_int g_steal_done;

static int
steal_task(xtc_task_t *self, void *u)
{
	(void)self; (void)u;
	atomic_fetch_add_explicit(&g_steal_done, 1, memory_order_relaxed);
	return XTC_TASK_DONE;
}

static void
run_steal(uint64_t seed, unsigned pct, struct run_result *r)
{
	xtc_exec_t *e = NULL;
	xtc_loop_t *l0;
	int i;
	atomic_store(&g_steal_done, 0);
	memset(r, 0, sizeof *r);
	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK) { r->rc = -1; return; }
	xtc_sim_fault_points_enable(pct);
	l0 = xtc_exec_loop(e, 0);
	for (i = 0; i < N_STEAL_WORKERS; i++)
		(void)xtc_task_spawn(l0, steal_task, NULL, NULL);
	r->rc = xtc_sim_exec_run(e, seed, 5000000);
	r->replies = atomic_load(&g_steal_done);
	r->steal_fires = xtc_sim_fault_point_fires("sched.steal.pre_cas");
	r->enqueue_fires =
	    xtc_sim_fault_point_fires("sched.enqueue.post_deque_push");
	r->points_seen = xtc_sim_fault_points_seen();
	xtc_sim_fault_points_disable();
	(void)xtc_exec_fini(e);
}

int
main(void)
{
	struct run_result a, b, s1, s2;
	int expect_replies = N_PAIRS * N_HOPS;

	/* 30% fire probability at every critical-section point. */
	run_once(0xD1CE, 300, &a);
	run_once(0xD1CE, 300, &b);

	printf("run1: rc=%d replies=%d app=%ld points_seen=%d "
	    "fires(steal=%llu inbox=%llu enqueue=%llu)\n",
	    a.rc, a.replies, a.app_hash, a.points_seen,
	    (unsigned long long)a.steal_fires,
	    (unsigned long long)a.inbox_fires,
	    (unsigned long long)a.enqueue_fires);
	printf("run2: rc=%d replies=%d app=%ld points_seen=%d "
	    "fires(steal=%llu inbox=%llu enqueue=%llu)\n",
	    b.rc, b.replies, b.app_hash, b.points_seen,
	    (unsigned long long)b.steal_fires,
	    (unsigned long long)b.inbox_fires,
	    (unsigned long long)b.enqueue_fires);

	if (a.rc != XTC_OK || b.rc != XTC_OK) {
		printf("FAIL: run did not reach quiescence (rc %d/%d)\n",
		    a.rc, b.rc);
		return 1;
	}
	if (a.replies != expect_replies || b.replies != expect_replies) {
		printf("FAIL: not all replies (got %d/%d, want %d)\n",
		    a.replies, b.replies, expect_replies);
		return 1;
	}
	/* Coverage: the critical sections were reached. */
	if (a.points_seen < 1) {
		printf("FAIL: no critical-section fault points were reached\n");
		return 1;
	}
	/* The inbox push happens on every cross-loop send, so it MUST be
	 * reached in this cross-loop workload. */
	if (a.inbox_fires == 0 && b.inbox_fires == 0) {
		printf("FAIL: the inbox critical section never fired -- "
		    "expected on cross-loop sends\n");
		return 1;
	}
	/* Determinism: same seed -> identical fire counts + result. */
	if (a.steal_fires != b.steal_fires ||
	    a.inbox_fires != b.inbox_fires ||
	    a.enqueue_fires != b.enqueue_fires ||
	    a.app_hash != b.app_hash) {
		printf("FAIL: fault-point fires / result not deterministic\n");
		return 1;
	}

	/* Phase 2: a steal-inducing workload (many unpinned tasks on one
	 * loop -> idle peers steal them).  The steal critical section
	 * (sched.steal.pre_cas) is reached when a thief takes a task; we
	 * assert it is reached and fires deterministically. */
	run_steal(0x57EA1, 300, &s1);
	run_steal(0x57EA1, 300, &s2);
	printf("steal1: rc=%d done=%d points_seen=%d "
	    "fires(steal=%llu enqueue=%llu)\n",
	    s1.rc, s1.replies, s1.points_seen,
	    (unsigned long long)s1.steal_fires,
	    (unsigned long long)s1.enqueue_fires);
	printf("steal2: rc=%d done=%d points_seen=%d "
	    "fires(steal=%llu enqueue=%llu)\n",
	    s2.rc, s2.replies, s2.points_seen,
	    (unsigned long long)s2.steal_fires,
	    (unsigned long long)s2.enqueue_fires);
	if (s1.rc != XTC_OK || s2.rc != XTC_OK ||
	    s1.replies != N_STEAL_WORKERS || s2.replies != N_STEAL_WORKERS) {
		printf("FAIL: steal workload did not complete\n");
		return 1;
	}
	/* The steal critical section MUST be reached: the unpinned tasks are
	 * all on loop 0, and the deterministic scheduler lets idle peers
	 * steal them. */
	if (s1.steal_fires == 0 && s2.steal_fires == 0) {
		printf("FAIL: the steal critical section never fired -- "
		    "expected idle loops to steal unpinned tasks\n");
		return 1;
	}
	if (s1.steal_fires != s2.steal_fires ||
	    s1.enqueue_fires != s2.enqueue_fires) {
		printf("FAIL: steal-workload fires not deterministic\n");
		return 1;
	}

	printf("OK: critical-section fault points fire deterministically "
	    "under DST (ping/pong inbox=%llu; steal workload steal=%llu "
	    "enqueue=%llu; all replay identically)\n",
	    (unsigned long long)a.inbox_fires,
	    (unsigned long long)s1.steal_fires,
	    (unsigned long long)s1.enqueue_fires);
	return 0;
}
