/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * docs/_includes/snippets/25_gs_pingpong.c --
 * the "Erlang-style processes" program of docs/getting-started.md: one
 * process sends five messages, another receives them in order.
 *
 * docs/getting-started.md has no Jekyll front matter, so the site copies
 * it as a plain file and it cannot include this one.  It shows the region
 * below verbatim instead, and test/docs/test_doc_snippets.sh fails when
 * the two differ.
 */

/* !region full */
#include <stdio.h>
#include <stdlib.h>
#include <xtc.h>
#include <xtc_loop.h>
#include <xtc_proc.h>

#define N_ROUNDS 5

static void
ponger(void *arg)
{
    int round;
    (void)arg;
    for (round = 0; round < N_ROUNDS; round++) {
        void *msg; size_t sz;
        if (xtc_recv(&msg, &sz, -1) != XTC_OK) return;
        printf("ponger got: %.*s\n", (int)sz, (char *)msg);
        xtc_free(msg);
    }
}

static void
pinger(void *arg)
{
    xtc_pid_t target = *(xtc_pid_t *)arg;
    int i;
    for (i = 0; i < N_ROUNDS; i++) {
        char buf[32];
        int n = snprintf(buf, sizeof buf, "ping #%d", i);
        xtc_send(target, buf, (size_t)n);
    }
}

int
main(void)
{
    xtc_loop_t *loop;
    xtc_pid_t pong_pid, ping_pid;

    xtc_loop_create(&loop);
    xtc_proc_spawn(loop, ponger, NULL, NULL, &pong_pid);
    xtc_proc_spawn(loop, pinger, &pong_pid, NULL, &ping_pid);
    xtc_loop_run(loop);
    xtc_loop_destroy(loop);
    return 0;
}
/* !endregion full */
