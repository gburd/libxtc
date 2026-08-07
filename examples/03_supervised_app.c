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
	free(r->scratch);
	free(r);
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

	/* Acquire the worker's scratch resource under a scope: the release
	 * is now a MECHANISM (runs on every exit), not a manner we must
	 * remember to repeat on each early return / kill path. */
	res = calloc(1, sizeof *res);
	scope = xtc_scope_open();
	if (res != NULL && scope != NULL) {
		res->scratch = calloc(1, 4096);
		(void)xtc_scope_defer(scope, counter_release, res);
	} else {
		free(res);
		res = NULL;
	}

	for (count = 0; count < g_iterations; ) {
		void *m; size_t sz;
		(void)xtc_recv(&m, &sz, 50 * 1000 * 1000);   /* 50 ms tick */
		if (m) free(m);
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
	for (rounds = 0; rounds < g_iterations / 5; rounds++) {
		void *m; size_t sz;
		(void)xtc_recv(&m, &sz, 250 * 1000 * 1000);  /* 250 ms */
		if (m) free(m);
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
	if (m) free(m);
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

	if (xtc_app_start(app, kids, 2) != XTC_OK) return 1;
	if (xtc_proc_spawn(xtc_app_loop(app), shutdown_watcher, NULL,
	    NULL, &watcher_pid) != XTC_OK) return 1;
	if (xtc_app_run(app) != XTC_OK) return 1;

	xtc_app_destroy(app);
	printf("done\n");
	return 0;
}
