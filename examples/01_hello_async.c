/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * examples/01_hello_async.c -- minimal coroutine: spawn an async
 * task, await its return value.
 *
 * Build:
 *   cc -I../src/inc 01_hello_async.c ../build_unix/libxtc.a -lpthread -o 01_hello
 */

#include <stdio.h>
#include <stdint.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_async.h"

static intptr_t
hello(void *arg)
{
	int n = (int)(intptr_t)arg;
	printf("hello from coroutine, computing %d * %d\n", n, n);
	xtc_yield();           /* round-trip through the loop */
	return n * n;
}

int
main(void)
{
	xtc_loop_t *loop;
	xtc_task_t *task;
	intptr_t result = 0;

	if (xtc_loop_init(&loop) != XTC_OK) return 1;
	if (xtc_async(loop, hello, (void *)(intptr_t)7, &task) != XTC_OK) return 1;
	if (xtc_loop_run(loop) != XTC_OK) return 1;
	(void)xtc_await(task, &result);
	printf("result = %lld\n", (long long)result);
	(void)xtc_loop_fini(loop);
	return 0;
}
