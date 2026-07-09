/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * docs/_includes/snippets/06_qsort_yield.c -- suspend from inside a
 * foreign C call chain.  qsort() (a libc function that knows nothing
 * about libxtc) calls our comparator, and the comparator yields the
 * fiber back to the loop mid-sort.  Because libxtc fibers are STACKFUL,
 * qsort's frame -- deep in the C call stack -- is frozen and restored
 * intact; qsort never notices.  A callback/stackless runtime cannot do
 * this: there would be no stack to suspend.
 */

/* !region full */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_async.h"

static int g_yields;   /* how many times the comparator suspended */

/* An ordinary qsort comparator -- except it cooperatively yields.  It
 * could just as well await an async read or a message here; the point
 * is that the suspension happens INSIDE qsort's call frame. */
static int
cmp_yielding(const void *a, const void *b)
{
	int x = *(const int *)a;
	int y = *(const int *)b;

	xtc_yield();     /* hand the loop control from deep inside qsort() */
	g_yields++;
	return (x > y) - (x < y);
}

static intptr_t
sorter(void *arg)
{
	int *v = arg;
	int n = 8, i;

	/* qsort is plain libc: it has no idea it is running on a fiber.
	 * Our comparator suspends the whole fiber (qsort frame included)
	 * on each compare, and qsort resumes correctly every time. */
	qsort(v, (size_t)n, sizeof v[0], cmp_yielding);

	for (i = 1; i < n; i++)
		if (v[i - 1] > v[i])
			return -1;       /* not sorted -> the pattern broke */
	return 0;                    /* sorted correctly across all the yields */
}

int
main(void)
{
	static int data[8] = { 5, 2, 8, 1, 9, 3, 7, 4 };
	xtc_loop_t *loop;
	xtc_task_t *task;
	intptr_t    ok = -1;

	if (xtc_loop_init(&loop) != XTC_OK)
		return 1;
	if (xtc_async(loop, sorter, data, &task) != XTC_OK)
		return 1;
	(void)xtc_loop_run(loop);
	(void)xtc_await(task, &ok);

	printf("sorted correctly through %d in-qsort yields: %s\n",
	    g_yields, ok == 0 ? "yes" : "no");
	(void)xtc_loop_fini(loop);
	return ok == 0 ? 0 : 1;
}
/* !endregion full */
