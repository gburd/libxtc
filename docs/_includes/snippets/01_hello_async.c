/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * docs/snippets/01_hello_async.c -- the first snippet in the Getting
 * Started chapter.  Spawn one asynchronous task on an event loop, run
 * the loop, and read the task's return value back on the main thread.
 *
 * This is the whole program a newcomer can copy, compile, and run.
 */

/* !region full */
#include <stdio.h>
#include <stdint.h>

#include "xtc.h"        /* error codes, XTC_OK */
#include "xtc_loop.h"   /* the event loop */
#include "xtc_async.h"  /* xtc_async / xtc_await / xtc_yield */

/* A coroutine body.  It takes a void* and returns an intptr_t; libxtc
 * runs it on a fiber, so it may cooperatively yield the CPU back to the
 * loop with xtc_yield() and later be resumed exactly where it left off. */
static intptr_t
square(void *arg)
{
	int n = (int)(intptr_t)arg;

	printf("coroutine: computing %d * %d\n", n, n);
	xtc_yield();              /* hand control back to the loop, then resume */
	return (intptr_t)n * n;
}

int
main(void)
{
	xtc_loop_t *loop;
	xtc_task_t *task;
	intptr_t    result = 0;

	if (xtc_loop_init(&loop) != XTC_OK)
		return 1;

	/* Spawn the coroutine.  It does not run yet -- it is queued on the
	 * loop and will run when we call xtc_loop_run(). */
	if (xtc_async(loop, square, (void *)(intptr_t)7, &task) != XTC_OK)
		return 1;

	/* Drive the loop until every task has finished. */
	if (xtc_loop_run(loop) != XTC_OK)
		return 1;

	/* The task is done; collect its return value. */
	(void)xtc_await(task, &result);
	printf("result = %lld\n", (long long)result);

	(void)xtc_loop_fini(loop);
	return 0;
}
/* !endregion full */
