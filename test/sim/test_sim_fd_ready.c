/*
 * test_sim_fd_ready -- DST of scheduled fd readiness
 * (xtc_sim_fd_ready_at), driving the real xtc_accel_wait_fence and
 * xtc_proc_wait_fd paths a GPU fence takes.
 *
 * Written against the public xtc_sim.h only, the way an application's
 * own DST test would be.  Per seed, on a 3-loop executor:
 *
 *   FENCE   a simulated device proc owns a pipe per job.  It schedules
 *           job k's read end ready at a seeded virtual time, and a
 *           waiter proc on a seeded loop parks on it through
 *           xtc_accel_wait_fence with its mailbox non-empty (the fence
 *           wait is mail-blind).  Each wait must return XTC_OK no
 *           earlier than its due time, and in due order.
 *   LATE    a second waiter starts waiting on job 0's fence after the
 *           fence's due time (and after the first waiter consumed it):
 *           a level readiness wakes it at once.
 *   MOVE    the device reschedules one job's fence to a later time
 *           before it is due: the waiter wakes at the later time.
 *   CANCEL  one job's fence is withdrawn (revents 0); its waiter's
 *           deadline ends the wait with XTC_E_AGAIN.
 *
 * Every seed runs twice and must replay (the wake times, the order, the
 * scheduler state hash).  Planted bug (src/inc/xtc_dst_inject.h):
 *   51 FDREADY_EDGE  the readiness fires once and is forgotten, so the
 *                    LATE waiter is never woken.
 *
 * Usage: test_sim_fd_ready [base [count]]
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xtc.h"
#include "xtc_accel.h"
#include "xtc_exec.h"
#include "xtc_io.h"
#include "xtc_proc.h"
#include "xtc_sim.h"

#define N_LOOPS 3
#define N_JOBS 4
#define MS (1000LL * 1000)

struct job {
	int fd[2];
	int64_t due;  /* scheduled ready time (after MOVE, the moved one) */
	int64_t woke; /* virtual time the waiter returned */
	int rc;
	int order; /* position among the waiters' returns */
};

struct run {
	xtc_exec_t *e;
	struct job j[N_JOBS];
	int move_job, cancel_job;
	int returns;
	int late_rc;
	int64_t late_woke;
	int done;
};

static struct run g_r;

static void
waiter(void *arg)
{
	struct job *jb = arg;
	int64_t deadline = jb == &g_r.j[g_r.cancel_job] ? 60 * MS : -1;
	(void)xtc_send(xtc_self(), "mail", 4);
	jb->rc = xtc_accel_wait_fence(jb->fd[0], deadline);
	jb->woke = xtc_clock_mono();
	jb->order = g_r.returns++;
	g_r.done++;
}

static void
late_waiter(void *arg)
{
	struct run *r = arg;
	uint32_t rev = 0;
	/* Wait until well after job 0's fence was due and consumed. */
	(void)xtc_proc_sleep(r->j[0].due + 5 * MS);
	r->late_rc =
	    xtc_proc_wait_fd(r->j[0].fd[0], XTC_IO_READABLE, 100 * MS, &rev);
	r->late_woke = xtc_clock_mono();
	r->done++;
}

/* The simulated device: schedules every job's fence, moves one, cancels
 * one, all from inside the run. */
static void
device(void *arg)
{
	struct run *r = arg;
	int k;
	for (k = 0; k < N_JOBS; k++) {
		struct job *jb = &r->j[k];
		jb->due = (int64_t)(1 + xtc_sim_rand(40)) * MS;
		(void)xtc_sim_fd_ready_at(jb->fd[0], jb->due, XTC_IO_READABLE);
		(void)xtc_proc_spawn(xtc_exec_loop(r->e,
		                         (int)xtc_sim_rand(N_LOOPS)),
		    waiter, jb, NULL, NULL);
	}
	(void)xtc_sim_fd_ready_at(r->j[r->cancel_job].fd[0], 0, 0);
	r->j[r->cancel_job].due = -1;
	r->j[r->move_job].due += (int64_t)(1 + xtc_sim_rand(20)) * MS;
	(void)xtc_sim_fd_ready_at(r->j[r->move_job].fd[0],
	    r->j[r->move_job].due, XTC_IO_READABLE);
	(void)xtc_proc_spawn(xtc_exec_loop(r->e, (int)xtc_sim_rand(N_LOOPS)),
	    late_waiter, r, NULL, NULL);
	r->done++;
}

static void
plan(void *arg)
{
	struct run *r = arg;
	r->move_job = 1 + (int)xtc_sim_rand(N_JOBS - 1);
	r->cancel_job = 1 + (int)xtc_sim_rand(N_JOBS - 1);
	if (r->cancel_job == r->move_job)
		r->cancel_job = 1 + r->move_job % (N_JOBS - 1);
	(void)xtc_proc_spawn(xtc_exec_loop(r->e, 0), device, r, NULL, NULL);
}

static uint64_t
digest(const struct run *r, uint64_t state)
{
	uint64_t h = 0xcbf29ce484222325ull ^ state;
	int k;
	for (k = 0; k < N_JOBS; k++) {
		h = (h ^ (uint64_t)r->j[k].woke) * 0x100000001B3ull;
		h = (h ^ (uint64_t)r->j[k].order) * 0x100000001B3ull;
		h = (h ^ (uint64_t)(uint32_t)r->j[k].rc) * 0x100000001B3ull;
	}
	h = (h ^ (uint64_t)r->late_woke) * 0x100000001B3ull;
	return (h ^ (uint64_t)(uint32_t)r->late_rc) * 0x100000001B3ull;
}

/* The first violated property, or NULL. */
static const char *
check(const struct run *r)
{
	int a, b;
	if (r->late_rc != XTC_OK || r->late_woke != r->j[0].due + 5 * MS)
		return "a waiter after the due time never saw the readiness";
	for (a = 0; a < N_JOBS; a++) {
		const struct job *ja = &r->j[a];
		if (a == r->cancel_job) {
			if (ja->rc != XTC_E_AGAIN || ja->woke != 60 * MS)
				return "a withdrawn fence still fired";
			continue;
		}
		if (ja->rc != XTC_OK || ja->woke != ja->due)
			return "a fence waiter did not wake at its due time";
		for (b = 0; b < N_JOBS; b++) {
			const struct job *jb = &r->j[b];
			if (b != r->cancel_job && ja->due < jb->due &&
			    ja->order > jb->order)
				return "fences were delivered out of due order";
		}
	}
	return r->done == N_JOBS + 2 ? NULL : "a proc did not finish";
}

static int
run_one(uint64_t seed, uint64_t *out)
{
	struct run *r = &g_r;
	const char *why;
	int k, rc;

	memset(r, 0, sizeof *r);
	for (k = 0; k < N_JOBS; k++)
		if (pipe(r->j[k].fd) != 0)
			return -1;
	if (xtc_exec_create(&(xtc_exec_opts_t){
	    .size = sizeof(xtc_exec_opts_t), .n_loops = N_LOOPS }, &r->e) != XTC_OK)
		return -1;
	(void)xtc_proc_spawn(xtc_exec_loop(r->e, 0), plan, r, NULL, NULL);
	rc = xtc_sim_exec_run(r->e, seed, 2000000);
	*out = digest(r, xtc_sim_state_hash(r->e));
	xtc_exec_destroy(r->e);
	for (k = 0; k < N_JOBS; k++) {
		(void)close(r->j[k].fd[0]);
		(void)close(r->j[k].fd[1]);
	}
	why = check(r);
	if (why == NULL && rc != XTC_OK)
		why = "the run did not quiesce";
	if (why == NULL)
		return 0;
	printf("  seed 0x%016llx: FAIL %s (rc=%d late=%d@%lld due0=%lld)\n",
	    (unsigned long long)seed, why, rc, r->late_rc,
	    (long long)r->late_woke, (long long)r->j[0].due);
	return 1;
}

int
main(int argc, char **argv)
{
	uint64_t base = 0x666472; /* "fdr" */
	int n = 32, i, fails = 0;

	if (argc > 1)
		base = strtoull(argv[1], NULL, 0);
	if (argc > 2)
		n = atoi(argv[2]);
	if (xtc_accel_wait_fence(-1, 0) == XTC_E_NOSYS) {
		printf("SKIP: no accel support in this build\n");
		return 0;
	}
	printf("== scheduled fd readiness DST: %d seeds from base 0x%llx ==\n",
	    n, (unsigned long long)base);
	for (i = 0; i < n; i++) {
		uint64_t seed = base + (uint64_t)i * 0x9E3779B97F4A7C15ull;
		uint64_t d1 = 0, d2 = 0;
		if (run_one(seed, &d1) != 0) {
			fails++;
			continue;
		}
		if (run_one(seed, &d2) != 0 || d1 != d2) {
			printf("  seed 0x%016llx: FAIL replay diverged\n",
			    (unsigned long long)seed);
			fails++;
		}
	}
	if (fails == 0) {
		printf("OK: scheduled fd readiness DST -- %d seeds x %d fences "
		       "through xtc_accel_wait_fence: each wakes at its due "
		       "time in due order with mail queued, a late waiter sees "
		       "the level, a moved fence wakes late, a withdrawn one "
		       "times out, all replay\n",
		    n, N_JOBS);
		return 0;
	}
	printf("FAIL: %d/%d fd-readiness seeds failed\n", fails, n);
	return 1;
}
