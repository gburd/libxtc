/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * docs/_includes/snippets/24_gs_hello.c --
 * the "Hello, async" program of docs/getting-started.md: one task that
 * runs once, prints a line, and is done.
 *
 * docs/getting-started.md has no Jekyll front matter, so the site copies
 * it as a plain file and it cannot include this one.  It shows the region
 * below verbatim instead, and test/docs/test_doc_snippets.sh fails when
 * the two differ.
 */

/* !region full */
#include <stdio.h>
#include <xtc.h>
#include <xtc_loop.h>

static int
greeter(xtc_task_t *self, void *user)
{
    (void)self; (void)user;
    printf("hello from a task\n");
    return XTC_TASK_DONE;
}

int
main(void)
{
    xtc_loop_t *loop;
    xtc_loop_create(&loop);
    xtc_task_spawn(loop, greeter, NULL, NULL);
    xtc_loop_run(loop);
    xtc_loop_destroy(loop);
    return 0;
}
/* !endregion full */
