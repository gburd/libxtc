/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * docs/_includes/snippets/00_readme_taste.c -- the README's "30-second
 * taste": a one-process actor system.  main() spawns a worker proc and a
 * parent proc; the worker sends a message to the parent; the parent
 * receives it.
 *
 * THE RULE THIS SNIPPET EXISTS TO ENFORCE: xtc_self() and xtc_recv() are
 * PROC-CONTEXT calls.  Off a proc, xtc_self() returns XTC_PID_NONE and
 * xtc_recv() rejects with XTC_E_INVAL (both measured).  An earlier README
 * called both from main(), so it compiled, exited 0, and NEVER printed the
 * line the prose promised.  The receive has to happen INSIDE a proc, and
 * the loop has to run for either proc to execute at all.
 */

/* !region full */
#include <stdio.h>
#include <string.h>

#include "xtc.h"        /* XTC_OK, xtc_free */
#include "xtc_loop.h"   /* the event loop */
#include "xtc_proc.h"   /* procs, mailboxes, send/recv */

/* The worker runs on a fiber.  It is handed the pid to reply to. */
static void
worker(void *arg)
{
	xtc_pid_t parent = *(xtc_pid_t *)arg;

	(void)xtc_send(parent, "hello", 5);
}

/* The parent spawns the worker from INSIDE a proc, so it has a pid to be
 * replied to and a mailbox to receive in. */
static void
parent(void *arg)
{
	xtc_loop_t *loop = arg;
	xtc_pid_t   self = xtc_self();   /* a real pid: we are on a proc */
	void       *msg;
	size_t      sz;

	if (xtc_proc_spawn(loop, worker, &self, NULL, NULL) != XTC_OK)
		return;

	/* Wait up to one second for the worker's message. */
	if (xtc_recv(&msg, &sz, 1000LL * 1000 * 1000) == XTC_OK) {
		printf("got %zu bytes from worker\n", sz);
		xtc_free(msg);           /* received buffers are ours to free --
		                          * with xtc_free, not free(3): libxtc
		                          * may use its own allocator */
	}
}

int
main(void)
{
	xtc_loop_t *loop;

	if (xtc_loop_init(&loop) != XTC_OK)
		return 1;
	if (xtc_proc_spawn(loop, parent, loop, NULL, NULL) != XTC_OK)
		return 1;
	(void)xtc_loop_run(loop);        /* runs both procs to completion */
	(void)xtc_loop_fini(loop);
	return 0;
}
/* !endregion full */
