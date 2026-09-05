/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * examples/03_supervised_app.c -- OTP-style app: a root supervisor
 * starts two children (a counter-server and a stats-printer); the
 * supervisor's one_for_all strategy ensures they are restarted
 * together if either crashes.
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "xtc.h"
#include "xtc_app.h"
#include "xtc_proc.h"
#include "xtc_orc.h"
#include "xtc_trace.h"

static int g_iterations;

/*
 * L1 (proportional-share scheduling, inspired by Glommio -- Glauber
 * Costa / ScyllaDB): two scheduling classes share the app's loop.  The
 * counter is the latency-sensitive foreground worker (more shares AND a
 * latency bound, so it is scheduled promptly); the stats printer is
 * best-effort background (fewer shares).  Under the default FIFO the
 * two interleave ~1:1 regardless of intent; with classes the counter
 * gets the weighted CPU + latency it needs.  Created once in main,
 * tagged onto each proc at entry via xtc_proc_set_class.  See
 * docs/guide/08-scheduling.md. */
static xtc_exec_class_t g_cls_fg;   /* counter: latency-sensitive foreground */
static xtc_exec_class_t g_cls_bg;   /* stats:   best-effort background */

/* A1 (resource scope): a per-worker scratch resource whose release MUST
 * run even if the supervisor kills this worker mid-loop.  Freed by
 * counter_release, registered on an xtc_scope so it runs on every exit
 * path -- normal return AND an async restart-kill. */
struct counter_res { unsigned char *scratch; };

static void
counter_release(void *arg)
{
	struct counter_res *r = arg;
	if (r == NULL)
		return;
	/* Our own allocations, so either allocator would work -- but the
	 * public xtc_* family is what a consumer should reach for, and it
	 * routes through the same hook an embedder installs for accounting. */
	xtc_free(r->scratch);
	xtc_free(r);
}

/* A2 (cancellation masking): the body of a critical two-step update the
 * worker must complete atomically w.r.t. an async kill -- if the
 * supervisor restarts us BETWEEN the two steps we would leave the
 * counter half-updated.  xtc_uncancelable defers any kill until the
 * body returns.  (Contrived here for illustration; in real code this is
 * a durable-write-then-index-update, a lock acquire+register, etc.) */
static int g_committed;
static int
critical_commit(void *arg)
{
	int *count = arg;
	/* step 1 + step 2 must both land, or neither -- no kill in between. */
	int next = *count + 1;
	g_committed = next;   /* step 1: the durable/shared side */
	*count = next;        /* step 2: the local side */
	return 0;
}

static void
counter_proc(void *arg)
{
	int count = 0;
	xtc_scope_t *scope;
	struct counter_res *res;
	(void)arg;

	if (g_cls_fg != NULL)
		(void)xtc_proc_set_class(g_cls_fg);   /* L1: foreground class */

	/* Acquire the worker's scratch resource under a scope: the release
	 * is now a MECHANISM (runs on every exit), not a manner we must
	 * remember to repeat on each early return / kill path. */
	res = xtc_calloc(1, sizeof *res);
	scope = xtc_scope_open();
	if (res != NULL && scope != NULL) {
		res->scratch = xtc_calloc(1, 4096);
		(void)xtc_scope_defer(scope, counter_release, res);
	} else {
		xtc_free(res);
		res = NULL;
	}

	for (count = 0; count < g_iterations; ) {
		void *m; size_t sz;
		(void)xtc_recv(&m, &sz, 50 * 1000 * 1000);   /* 50 ms tick */
		/* xtc_free, NOT free: an xtc_recv buffer comes from libxtc's
		 * allocator (replaceable via the alloc hook), so plain free()
		 * would be a mismatched free.  xtc_free(NULL) is a no-op. */
		xtc_free(m);
		/* Advance the counter through the masked critical section so a
		 * restart-kill can never split the two-step update. */
		(void)xtc_uncancelable(critical_commit, &count);
	}
	printf("counter: completed %d iterations\n", count);

	/* Closing the scope runs counter_release LIFO; an async kill while
	 * parked in xtc_recv above would run the same finalizer on the
	 * proc's exit path -- either way the scratch buffer is freed. */
	if (scope != NULL)
		xtc_scope_close(scope);
	else
		counter_release(res);
}

static void
stats_proc(void *arg)
{
	int rounds = 0;
	(void)arg;
	if (g_cls_bg != NULL)
		(void)xtc_proc_set_class(g_cls_bg);   /* L1: background class */
	for (rounds = 0; rounds < g_iterations / 5; rounds++) {
		void *m; size_t sz;
		(void)xtc_recv(&m, &sz, 250 * 1000 * 1000);  /* 250 ms */
		xtc_free(m);   /* see the note in the foreground worker */
		printf("stats: round %d\n", rounds);
	}
}

static xtc_app_t *g_app;

static void
shutdown_watcher(void *arg)
{
	void *m; size_t sz;
	(void)arg;
	(void)xtc_recv(&m, &sz, 1500 * 1000 * 1000);  /* 1.5 s */
	xtc_free(m);   /* see the note in the foreground worker */
	printf("watcher: stopping app\n");
	(void)xtc_app_stop(g_app);
}

int
main(int argc, char **argv)
{
	xtc_app_t *app;
	xtc_app_opts_t opts = XTC_APP_OPTS_DEFAULT;
	xtc_child_spec_t kids[2];
	xtc_pid_t watcher_pid;

	g_iterations = argc > 1 ? atoi(argv[1]) : 10;

	/* A3 (async causal trace): turn on the per-fiber park/resume ring so
	 * that if a worker faults, xtc_dump shows not just its current state
	 * but HOW it got there (its recent xtc_recv / wait boundaries).  Off
	 * by default and zero-cost when off; a debugging aid you flip on. */
	(void)xtc_trace_causal_enable(1);

	opts.name = "demo_app";
	opts.sup.strategy     = XTC_SUP_ONE_FOR_ALL;
	opts.sup.max_restarts = 5;
	opts.sup.period_ns    = 1000LL * 1000 * 1000;

	memset(kids, 0, sizeof kids);
	kids[0].name   = "counter";
	kids[0].fn     = counter_proc;
	kids[0].policy = XTC_RESTART_TRANSIENT;
	kids[1].name   = "stats";
	kids[1].fn     = stats_proc;
	kids[1].policy = XTC_RESTART_TRANSIENT;

	if (xtc_app_create(&opts, &app) != XTC_OK) return 1;
	g_app = app;

	/* L1: create the two scheduling classes on the app's loop before the
	 * children run.  Foreground (counter) gets 3x the shares of the
	 * background (stats) plus a 1ms latency bound; background gets 1
	 * share, no bound.  (Glommio-inspired weighted-fair scheduling.) */
	/* Check these: if class creation fails, g_cls_* stay unset, every
	 * xtc_proc_set_class below is skipped, and the program prints the
	 * same output while demonstrating NOTHING about proportional-share
	 * scheduling.  A silent demo-defeating failure is worse than an
	 * error. */
	if (xtc_exec_class_create(xtc_app_loop(app), 3, 1000LL * 1000,
	    &g_cls_fg) != XTC_OK ||
	    xtc_exec_class_create(xtc_app_loop(app), 1, 0,
	    &g_cls_bg) != XTC_OK) {
		fprintf(stderr, "could not create scheduling classes; the "
		    "L1 proportional-share part of this demo would be a "
		    "no-op\n");
		return 1;
	}

	/* L3 (over-budget stall watchdog, inspired by Glommio's stall
	 * detector): warn (with a backtrace) if any single task runs longer
	 * than 50ms on this loop -- catches a runaway that would starve the
	 * others.  Off by default; opt in with a budget. */
	xtc_loop_set_stall_budget(xtc_app_loop(app), 50LL * 1000 * 1000);

	if (xtc_app_start(app, kids, 2) != XTC_OK) return 1;
	if (xtc_proc_spawn(xtc_app_loop(app), shutdown_watcher, NULL,
	    NULL, &watcher_pid) != XTC_OK) return 1;
	if (xtc_app_run(app) != XTC_OK) return 1;

	xtc_app_destroy(app);
	printf("done\n");
	return 0;
}
