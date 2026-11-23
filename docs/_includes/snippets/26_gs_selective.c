/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * docs/_includes/snippets/26_gs_selective.c --
 * the "Selective receive" program of docs/getting-started.md:
 * xtc_recv_match takes the priority message ahead of the FIFO head.
 *
 * docs/getting-started.md has no Jekyll front matter, so the site copies
 * it as a plain file and it cannot include this one.  It shows the region
 * below verbatim instead, and test/docs/test_doc_snippets.sh fails when
 * the two differ.
 */

/* !region full */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <xtc.h>
#include <xtc_loop.h>
#include <xtc_proc.h>

static int
is_priority(const void *data, size_t sz, void *u)
{
    (void)u;
    return sz >= 9 && memcmp(data, "PRIORITY:", 9) == 0;
}

static void
worker(void *arg)
{
    void *m1, *m2; size_t s1, s2;
    (void)arg;

    /* Pick the priority message first, even if not the FIFO head. */
    if (xtc_recv_match(is_priority, NULL, &m1, &s1, -1) == XTC_OK) {
        printf("priority: %.*s\n", (int)s1, (char *)m1);
        xtc_free(m1);
    }
    /* Then drain the rest in arrival order. */
    while (xtc_recv(&m2, &s2, 100LL * 1000 * 1000) == XTC_OK) {
        printf("normal:   %.*s\n", (int)s2, (char *)m2);
        xtc_free(m2);
    }
}

int
main(void)
{
    xtc_loop_t *loop;
    xtc_pid_t pid;
    const char *msgs[] = {
        "first", "second", "PRIORITY:urgent!", "fourth", "fifth"
    };
    int i;

    xtc_loop_create(&loop);
    xtc_proc_spawn(loop, worker, NULL, NULL, &pid);
    for (i = 0; i < 5; i++)
        xtc_send(pid, msgs[i], strlen(msgs[i]));
    xtc_loop_run(loop);
    xtc_loop_destroy(loop);
    return 0;
}
/* !endregion full */
