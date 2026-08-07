/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * docs/_includes/snippets/09_dispatch.c -- the callback -> fiber
 * bridge: submit an effect to the runtime from a FOREIGN OS thread and
 * await its result.
 *
 * The executor runs on its own worker threads; main is foreign to them.
 * main calls xtc_dispatch to run compute() on a loop, then awaits the
 * future for the result -- the ergonomic front door for bridging a C
 * library's callback, a signal follow-up, or an embedder's I/O thread
 * into the fiber world.
 */

/* !region full */
#include <stdio.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_dispatch.h"
#include "xtc_future.h"

/* !region effect */
/* The effect to run on the runtime.  Returns a result the future
 * carries back to the caller. */
static int
compute(void *arg)
{
	return (int)(intptr_t)arg * 2;
}
/* !endregion effect */

/* Drive the executor on its own thread so main is a genuinely foreign
 * caller (service mode: runs until xtc_exec_stop). */
static xtc_exec_t *g_exec;
static void *
run_exec(void *arg)
{
	(void)arg;
	(void)xtc_exec_run(g_exec);
	return NULL;
}

/* !region submit */
int
main(void)
{
	pthread_t th;
	xtc_future_t *fut = NULL;
	intptr_t out = 0;

	if (xtc_exec_init(&g_exec, 2) != XTC_OK)
		return 1;
	xtc_exec_set_service_mode(g_exec, 1);
	if (pthread_create(&th, NULL, run_exec, NULL) != 0)
		return 1;
	usleep(5000);                       /* let the workers spin up */

	/* From this foreign thread, submit compute(21) to run on a loop
	 * and await the doubled result. */
	if (xtc_dispatch(xtc_exec_loop(g_exec, 0), compute,
	    (void *)(intptr_t)21, &fut, NULL) != XTC_OK)
		return 1;
	if (xtc_future_wait(fut, &out, -1) != XTC_OK)
		return 1;

	printf("dispatch result: %ld\n", (long)out);   /* 42 */

	(void)xtc_exec_stop(g_exec);
	(void)pthread_join(th, NULL);
	(void)xtc_exec_fini(g_exec);
	return out == 42 ? 0 : 1;
}
/* !endregion submit */
/* !endregion full */
