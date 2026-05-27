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

static int g_iterations;

static void
counter_proc(void *arg)
{
	int count = 0;
	(void)arg;
	for (count = 0; count < g_iterations; count++) {
		void *m; size_t sz;
		(void)xtc_recv(&m, &sz, 50 * 1000 * 1000);   /* 50 ms tick */
		if (m) free(m);
	}
	printf("counter: completed %d iterations\n", count);
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
