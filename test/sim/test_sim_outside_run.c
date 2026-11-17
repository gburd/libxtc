/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/sim/test_sim_outside_run.c
 *	A sim build refuses to drive a loop outside a simulation run.
 *
 *	The sim I/O backend has no kernel poller: its poll never blocks and
 *	never looks at a real fd.  A program that called xtc_loop_run (or
 *	xtc_exec_run) on such a build outside xtc_sim_exec_run therefore spun
 *	a core forever on a pipe or socket it could not see become ready (the
 *	TCP echo snippet hung until killed).  Both calls now return
 *	XTC_E_STATE at once there.  This test parks a proc on a pipe nobody
 *	writes and checks, in order:
 *	  - xtc_loop_run on a plain loop returns XTC_E_STATE, and the proc it
 *	    did not run is still waiting to;
 *	  - xtc_exec_run returns XTC_E_STATE likewise;
 *	  - the same kind of program under xtc_sim_exec_run still runs (a
 *	    proc that sleeps on the virtual clock finishes, XTC_OK).
 *	Each step is bounded by an alarm, so the old spin fails the test
 *	instead of hanging it.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_io.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_sim.h"

static int g_pipe[2];
static int g_ran;

/* Park on a pipe nobody writes: only a real poller could end the wait. */
static void
reader(void *arg)
{
	uint32_t rev = 0;
	(void)arg;
	g_ran = 1;
	(void)xtc_proc_wait_fd(g_pipe[0], XTC_IO_READABLE, -1, &rev);
}

/* Sleep on the virtual clock and finish: the work a sim run drives. */
static void
sleeper(void *arg)
{
	(void)arg;
	(void)xtc_proc_sleep(5LL * 1000 * 1000);
	g_ran = 2;
}

static void
on_alarm(int sig)
{
	static const char msg[] =
	    "FAIL: a loop outside a sim run did not return\n";
	ssize_t n;
	(void)sig;
	n = write(1, msg, sizeof msg - 1);
	_exit(n < 0 ? 2 : 1);
}

int
main(void)
{
	xtc_loop_t *loop = NULL;
	xtc_exec_t *e = NULL;
	int rc;

	if (pipe(g_pipe) != 0) {
		printf("FAIL: pipe\n");
		return 1;
	}
	(void)signal(SIGALRM, on_alarm);

	/* A plain loop, outside any run. */
	if (xtc_loop_create(&loop) != XTC_OK ||
	    xtc_proc_spawn(loop, reader, NULL, NULL, NULL) != XTC_OK) {
		printf("FAIL: setup of the plain loop\n");
		return 1;
	}
	(void)alarm(10);
	rc = xtc_loop_run(loop);
	(void)alarm(0);
	if (rc != XTC_E_STATE || g_ran != 0) {
		printf("FAIL: xtc_loop_run outside a run: %s, proc ran %d\n",
		    xtc_strerror(rc), g_ran);
		return 1;
	}

	/* An executor, outside any run. */
	if (xtc_exec_init(&e, 2) != XTC_OK ||
	    xtc_proc_spawn(xtc_exec_loop(e, 0), reader, NULL, NULL, NULL) !=
	        XTC_OK) {
		printf("FAIL: setup of the executor\n");
		return 1;
	}
	(void)alarm(10);
	rc = xtc_exec_run(e);
	(void)alarm(0);
	if (rc != XTC_E_STATE || g_ran != 0) {
		printf("FAIL: xtc_exec_run outside a run: %s, proc ran %d\n",
		    xtc_strerror(rc), g_ran);
		return 1;
	}
	(void)xtc_exec_fini(e);

	/* The same program under a run is driven as before. */
	if (xtc_exec_init(&e, 2) != XTC_OK ||
	    xtc_proc_spawn(xtc_exec_loop(e, 1), sleeper, NULL, NULL, NULL) !=
	        XTC_OK) {
		printf("FAIL: setup of the sim run\n");
		return 1;
	}
	rc = xtc_sim_exec_run(e, 0x5175, 100000);
	if (rc != XTC_OK || g_ran != 2) {
		printf("FAIL: xtc_sim_exec_run: %s, proc ran %d\n",
		    xtc_strerror(rc), g_ran);
		return 1;
	}
	(void)xtc_exec_fini(e);

	printf("OK: outside a sim run xtc_loop_run and xtc_exec_run return "
	       "XTC_E_STATE at once (a proc parked on a real pipe would spin "
	       "the sim backend forever); xtc_sim_exec_run still drives the "
	       "procs\n");
	return 0;
}
