/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * docs/_includes/snippets/08_causal_trace.c -- the async causal trace:
 * "how did this fiber get here" spliced onto a fiber's current state.
 *
 * The per-fiber ring records each suspend/resume boundary (park reason
 * and resume) with a static site label.  It is OFF by default and
 * zero-cost when off; enable it to debug a stuck or mis-scheduled
 * fiber.  xtc_dump splices each proc's recent chain onto its state line;
 * xtc_trace_causal_dump reads one proc's chain programmatically.
 */

/* !region full */
#include <stdio.h>
#include <stdlib.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_trace.h"

static xtc_pid_t g_worker;

/* !region cb */
/* One record of the fiber's park/resume chain, oldest-first. */
static const char *
kind_name(int k)
{
	switch (k) {
	case XTC_CAUSAL_PARK_MAILBOX: return "park:mailbox";
	case XTC_CAUSAL_PARK_TIMER:   return "park:timer";
	case XTC_CAUSAL_PARK_FD:      return "park:fd";
	case XTC_CAUSAL_RESUME:       return "resume";
	default:                      return "?";
	}
}

static int
on_causal(const xtc_causal_rec_t *rec, void *user)
{
	(void)user;
	printf("  %s @ %s\n", kind_name(rec->kind),
	    rec->site != NULL ? rec->site : "?");
	return 0;                       /* 0 = keep going */
}
/* !endregion cb */

/* The worker parks on a series of timer sleeps, then reads its OWN
 * causal chain while still alive -- the point where you would splice it
 * onto a fault. */
static void
worker(void *arg)
{
	int i;
	(void)arg;
	for (i = 0; i < 3; i++)
		(void)xtc_proc_sleep(1000000LL);   /* 1ms timer park */

	printf("worker causal chain (how it got here):\n");
	(void)xtc_trace_causal_dump(xtc_self(), on_causal, NULL);
}

/* !region enable */
int
main(void)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t o = { 0 };

	xtc_trace_causal_enable(1);         /* opt in (off by default) */

	if (xtc_loop_init(&loop) != XTC_OK)
		return 1;
	if (xtc_proc_spawn(loop, worker, NULL, &o, &g_worker) != XTC_OK)
		return 1;
	if (xtc_loop_run(loop) != XTC_OK)
		return 1;
	(void)xtc_loop_fini(loop);

	xtc_trace_causal_enable(0);         /* stop paying for it */
	return 0;
}
/* !endregion enable */
/* !endregion full */
