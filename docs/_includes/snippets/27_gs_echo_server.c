/* SNIPPET: compile-only */
/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * docs/_includes/snippets/27_gs_echo_server.c --
 * the "A small TCP server" program of docs/getting-started.md: one process
 * per connection.  Compile-only, because it listens on 127.0.0.1:7777
 * until it is killed; 13_tcp_echo.c is the echo server the gate runs.
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
#include <unistd.h>
#include <xtc.h>
#include <xtc_loop.h>
#include <xtc_proc.h>
#include <xtc_net.h>
#include <xtc_io.h>

struct conn_args {
    int fd;
};

static void
conn_proc(void *arg)
{
    struct conn_args *a = arg;
    char buf[1024];
    for (;;) {
        uint32_t revents;
        ssize_t n;
        /* Wait for the socket to become readable, with a 30s idle
         * timeout to keep mostly-idle connections from holding
         * the proc forever. */
        if (xtc_proc_wait_fd(a->fd, XTC_IO_READABLE | XTC_IO_HUP,
                             30LL * 1000 * 1000 * 1000, &revents) != XTC_OK)
            break;
        if (revents & (XTC_IO_HUP | XTC_IO_ERR)) break;

        n = read(a->fd, buf, sizeof buf);
        if (n <= 0) break;
        if (write(a->fd, buf, (size_t)n) != n) break;
    }
    close(a->fd);
    free(a);
}

static xtc_loop_t *loop;
static int listen_fd;

static void
listener(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t revents;
        int conn_fd;
        struct conn_args *a;
        xtc_pid_t pid;

        if (xtc_proc_wait_fd(listen_fd, XTC_IO_READABLE, -1, &revents)
            != XTC_OK)
            break;
        /* Non-blocking accept: XTC_E_AGAIN when another wakeup took
         * the connection first. */
        if (xtc_net_accept(listen_fd, &conn_fd) != XTC_OK) continue;

        a = malloc(sizeof *a);
        a->fd = conn_fd;
        xtc_proc_spawn(loop, conn_proc, a, NULL, &pid);
    }
}

int
main(void)
{
    xtc_tcp_opts_t opts = XTC_TCP_OPTS_INIT;
    xtc_pid_t pid;

    if (xtc_net_listen(XTC_NET_INET, "127.0.0.1", 7777, &opts, &listen_fd)
        != XTC_OK) {
        perror("listen");
        return 1;
    }
    xtc_loop_create(&loop);
    xtc_proc_spawn(loop, listener, NULL, NULL, &pid);
    printf("echo server listening on 127.0.0.1:7777\n");
    xtc_loop_run(loop);
    xtc_loop_destroy(loop);
    close(listen_fd);
    return 0;
}
/* !endregion full */
