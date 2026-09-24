/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * docs/_includes/snippets/12_graceful_drain.c -- drain a supervised app
 * on SIGTERM.
 *
 * Three workers run under the app's root supervisor.  The program opts
 * in to signal-driven draining with xtc_app_drain_on_signal, then sends
 * ITSELF SIGTERM.  The handler only writes a byte to a pipe; a watcher
 * proc runs xtc_app_shutdown: every worker gets XTC_APP_SHUTDOWN_MSG,
 * finishes its current job and returns, and xtc_app_run comes back with
 * a report.  One worker ignores the request, to show the force phase:
 * it is cancelled after the drain deadline and still reported.  Exits 0
 * when the report says every worker is gone.
 */

/* !region full */
#include <signal.h>
#include <stdint.h>
#include <stdio.h>

#include "xtc.h"
#include "xtc_app.h"
#include "xtc_proc.h"

#define MS (1000LL * 1000)

/* A polite worker: does "jobs" until it is asked to drain. */
static void
worker(void *arg)
{
	int   id = (int)(intptr_t)arg;
	void *msg;
	size_t len;

	for (;;) {
		if (xtc_recv(&msg, &len, 10 * MS) != XTC_OK)
			continue;                 /* no mail: keep working */
		if (xtc_app_is_shutdown_msg(msg, len)) {
			xtc_free(msg);
			printf("worker %d: drained\n", id);
			return;                   /* finish cleanly */
		}
		xtc_free(msg);
	}
}

/* A worker that ignores the drain request: it gets force-cancelled. */
static void
stubborn(void *arg)
{
	void *msg;
	size_t len;

	(void)arg;
	for (;;)
		if (xtc_recv(&msg, &len, -1) == XTC_OK)
			xtc_free(msg);            /* throws the request away */
}

/* Plays the operator: waits a moment, then SIGTERMs this process. */
static void
operator(void *arg)
{
	(void)arg;
	(void)xtc_proc_sleep(50 * MS);
	printf("operator: raise(SIGTERM)\n");
	(void)raise(SIGTERM);
}

int
main(void)
{
	xtc_app_opts_t         opts = XTC_APP_OPTS_DEFAULT;
	xtc_app_drain_report_t rep = XTC_APP_DRAIN_REPORT_INIT;
	xtc_child_spec_t       kids[4] = {
		{ .name = "w0", .fn = worker, .arg = (void *)0,
		  .policy = XTC_RESTART_PERMANENT },
		{ .name = "w1", .fn = worker, .arg = (void *)1,
		  .policy = XTC_RESTART_PERMANENT },
		{ .name = "w2", .fn = worker, .arg = (void *)2,
		  .policy = XTC_RESTART_PERMANENT },
		{ .name = "stubborn", .fn = stubborn,
		  .policy = XTC_RESTART_PERMANENT },
	};
	xtc_app_t *app;
	xtc_pid_t  op;

	opts.name = "drain_demo";
	opts.no_tuning_check = 1;
	if (xtc_app_create(&opts, &app) != XTC_OK ||
	    xtc_app_start(app, kids, 4) != XTC_OK)
		return 1;

	/* Opt in: SIGTERM/SIGINT -> 200 ms to drain, then 1 s to force. */
	if (xtc_app_drain_on_signal(app, 200 * MS, 1000 * MS, &rep) != XTC_OK)
		return 1;
	if (xtc_proc_spawn(xtc_app_loop(app), operator, NULL, NULL, &op)
	    != XTC_OK)
		return 1;

	(void)xtc_app_run(app);           /* returns after the drain */
	printf("report: %d children, %d drained, %d forced, %d survivors\n",
	    rep.n_children, rep.n_drained, rep.n_forced, rep.n_survivors);
	xtc_app_destroy(app);
	return (rep.n_drained == 3 && rep.n_forced == 1 &&
	    rep.n_survivors == 0) ? 0 : 1;
}
/* !endregion full */
