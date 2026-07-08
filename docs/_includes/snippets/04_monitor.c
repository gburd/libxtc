/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * docs/_includes/snippets/04_monitor.c -- watch a child process and
 * learn, as a message, exactly how it ended.
 *
 * The parent spawns a child with xtc_proc_spawn_monitor.  When the
 * child returns (or crashes, or is killed), libxtc delivers a DOWN
 * message to the parent's mailbox.  The parent decodes it with
 * xtc_down_decode and prints the target pid and reason.
 */

/* !region full */
#include <stdio.h>
#include <stdint.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"

static void
worker(void *arg)
{
	int code = (int)(intptr_t)arg;

	printf("worker: exiting with reason %d\n", code);
	xtc_exit_self(code);          /* end deliberately with a reason */
}

static void
parent(void *arg)
{
	xtc_loop_t *loop = arg;      /* our loop, handed in at spawn */
	xtc_pid_t   child;
	uint64_t    ref;
	void       *raw;
	size_t      sz;

	/* Spawn AND monitor atomically: there is no window in which the
	 * child could exit before the monitor is armed (which would race a
	 * plain spawn-then-monitor). */
	if (xtc_proc_spawn_monitor(loop, worker,
	    (void *)(intptr_t)42, NULL, &child, &ref) != XTC_OK)
		return;

	/* Wait for the DOWN.  It arrives as an ordinary mailbox message. */
	if (xtc_recv(&raw, &sz, 1000LL * 1000 * 1000) != XTC_OK)
		return;

	{
		xtc_pid_t who;
		int       reason;

		if (xtc_down_decode(raw, sz, &who, &reason) == XTC_OK)
			printf("parent: child ended, reason %d\n", reason);
		else
			printf("parent: unexpected message\n");
	}
	xtc_free(raw);
}

int
main(void)
{
	xtc_loop_t *loop;

	if (xtc_loop_init(&loop) != XTC_OK)
		return 1;
	if (xtc_proc_spawn(loop, parent, loop, NULL, NULL) != XTC_OK)
		return 1;
	(void)xtc_loop_run(loop);
	(void)xtc_loop_fini(loop);
	return 0;
}
/* !endregion full */
