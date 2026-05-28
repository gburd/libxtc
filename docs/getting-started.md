# Getting started with xtc

This guide takes you from "just cloned the repo" to "I have an
async TCP server in C running" in about thirty minutes.  No prior
xtc knowledge assumed; some familiarity with C and pthreads is.

## What xtc is

xtc is a runtime library for writing asynchronous, fault-tolerant,
predictable-latency programs in C.  Think Tokio (Rust's async
runtime), the BEAM (Erlang's process model), and Seastar (C++'s
thread-per-core reactor) -- xtc takes the parts of each that work
well in C.

You write your program against xtc primitives:

* `xtc_loop` -- an event loop that owns a thread.
* `xtc_proc` -- a BEAM-style process with a mailbox.
* `xtc_io` -- backend-pluggable async I/O (epoll, io_uring, kqueue,
  IOCP, poll, select).
* `xtc_lwlock`, `xtc_lrlock`, `xtc_lockmgr` -- synchronisation
  primitives from "fast lightweight" up to "transactional with
  deadlock detection."
* `xtc_slab`, `xtc_mctx` -- memory allocators and contexts.
* `xtc_supervisor` -- crash-restart trees a la OTP.
* `xtc_res` -- resource accounting with hard caps.
* `xtc_log`, `xtc_cfg`, `xtc_stats` -- observability.

You link against `libxtc.a`; the runtime is in-process, no daemon.

## Prerequisites

* A C11 compiler (gcc 11+, clang 13+, or MSVC 2019+).
* pthreads (POSIX) or Windows threads (MinGW).
* About 50 MB of disk for the build tree.
* Optionally: `liburing` for the io_uring backend on Linux,
  OpenSSL for TLS support.

xtc itself is BSD KNF C, no external runtime dependencies beyond
libc + pthreads.

## Build

Clone, configure, build:

```sh
git clone https://codeberg.org/gregburd/libxtc.git
cd libxtc/dist && autoreconf -i && cd ..
mkdir -p build_unix && cd build_unix
../dist/configure
make -j$(nproc)
```

Run the test suite to confirm nothing is broken on your platform:

```sh
make check
```

You should see a line like `269 of 269 tests successful`.  If
something fails, check `docs/KNOWN_ISSUES.md` -- some platforms
have intentional skips that look like failures at a glance.

Install (optional):

```sh
sudo make install
```

This drops `libxtc.a` into `/usr/local/lib`, headers into
`/usr/local/include`, and man pages into `/usr/local/share/man`.

## Hello, async

The simplest possible xtc program: spawn a task, run the loop,
exit.  Save as `hello.c`:

```c
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
    xtc_loop_init(&loop);
    xtc_task_spawn(loop, greeter, NULL, NULL);
    xtc_loop_run(loop);
    xtc_loop_fini(loop);
    return 0;
}
```

Compile:

```sh
cc hello.c -lxtc -lpthread -o hello
./hello
```

Output: `hello from a task`.

This isn't much yet -- a task that runs once and exits.  Tasks are
the lowest level of "something the loop runs"; for real programs
you'll usually use processes (the next section) which add identity
and a mailbox.

## Erlang-style processes

A process (`xtc_proc`) is a fiber with a mailbox.  Two processes
communicate by sending messages.  Save as `pingpong.c`:

```c
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
        free(msg);
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

    xtc_loop_init(&loop);
    xtc_proc_spawn(loop, ponger, NULL, NULL, &pong_pid);
    xtc_proc_spawn(loop, pinger, &pong_pid, NULL, &ping_pid);
    xtc_loop_run(loop);
    xtc_loop_fini(loop);
    return 0;
}
```

Compile and run:

```sh
cc pingpong.c -lxtc -lpthread -o pingpong
./pingpong
```

Output:

```
ponger got: ping #0
ponger got: ping #1
ponger got: ping #2
ponger got: ping #3
ponger got: ping #4
```

The pinger sent 5 messages.  The ponger received them in FIFO
order via `xtc_recv` and printed each.  Both processes ran
co-operatively in a single xtc_loop on a single OS thread.

## Selective receive

A process can receive messages selectively -- skip messages that
don't match a predicate, leaving them in the mailbox for later.
This is the BEAM's `receive` semantics.  Save as `selective.c`:

```c
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
        free(m1);
    }
    /* Then drain the rest in arrival order. */
    while (xtc_recv(&m2, &s2, 100LL * 1000 * 1000) == XTC_OK) {
        printf("normal:   %.*s\n", (int)s2, (char *)m2);
        free(m2);
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

    xtc_loop_init(&loop);
    xtc_proc_spawn(loop, worker, NULL, NULL, &pid);
    for (i = 0; i < 5; i++)
        xtc_send(pid, msgs[i], strlen(msgs[i]));
    xtc_loop_run(loop);
    xtc_loop_fini(loop);
    return 0;
}
```

Output:

```
priority: PRIORITY:urgent!
normal:   first
normal:   second
normal:   fourth
normal:   fifth
```

The priority message comes out first, then the others in arrival
order.  Same model as Erlang's `receive` clauses.

## A small TCP server

This is the meat of why you'd use xtc: writing a server that handles
many concurrent connections with predictable latency.  Save as
`echo_server.c`:

```c
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

static void
listener(void *arg)
{
    int listen_fd = *(int *)arg;
    for (;;) {
        uint32_t revents;
        int conn_fd;
        struct conn_args *a;
        xtc_pid_t pid;

        if (xtc_proc_wait_fd(listen_fd, XTC_IO_READABLE, -1, &revents)
            != XTC_OK)
            break;
        conn_fd = accept(listen_fd, NULL, NULL);
        if (conn_fd < 0) continue;
        xtc_net_setnonblock(conn_fd);

        a = malloc(sizeof *a);
        a->fd = conn_fd;
        xtc_proc_spawn(__xtc_current_loop, conn_proc, a, NULL, &pid);
    }
}

int
main(void)
{
    xtc_loop_t *loop;
    xtc_tcp_opts_t opts = XTC_TCP_OPTS_DEFAULT;
    int listen_fd;
    xtc_pid_t pid;

    if (xtc_net_listen(XTC_NET_INET, "127.0.0.1", 7777, &opts, &listen_fd)
        != XTC_OK) {
        perror("listen");
        return 1;
    }
    xtc_loop_init(&loop);
    xtc_proc_spawn(loop, listener, &listen_fd, NULL, &pid);
    printf("echo server listening on 127.0.0.1:7777\n");
    xtc_loop_run(loop);
    xtc_loop_fini(loop);
    close(listen_fd);
    return 0;
}
```

Compile and run:

```sh
cc echo_server.c -lxtc -lpthread -o echo_server
./echo_server &
nc 127.0.0.1 7777
hello
hello                # echoed back
```

What's happening:

* `xtc_net_listen` opens a non-blocking TCP listening socket.
* The `listener` process loops on `xtc_proc_wait_fd(listen_fd,
  XTC_IO_READABLE, -1)` -- this sleeps the process until the listen
  fd has a connection waiting, no busy-polling.
* Each `accept` spawns a new `xtc_proc` for that connection.
* The conn_proc loops on `xtc_proc_wait_fd(client_fd, ...)` and
  echoes bytes back.

A thousand idle connections cost a thousand sleeping fibers -- each
proc is ~512 bytes, no thread per connection.  When a connection
becomes active, only its proc wakes; others stay asleep.  This is
the xtc model in one screen of code.

## Where to go next

By layer of complexity:

1. `examples/01_hello_async.c` -- timer-driven task.
2. `examples/02_proc_pingpong.c` -- the ping-pong above, fleshed
   out with reasoning about ownership and lifetime.
3. `examples/03_supervised_app.c` -- crash a worker, watch the
   supervisor restart it.  OTP-style fault tolerance.
4. `examples/04_lockmgr_demo.c` -- transactional locking with a
   deadlock detector.  Demonstrates a custom victim picker
   driven by a coroutine.
5. `examples/05_rexis/` -- Redis-protocol-compatible server with
   hard resource budgets.  ~2000 LOC; uses every major xtc layer.
6. `examples/06_sqlxtc/` -- networked SQLite via the Quack
   protocol.  Multi-client, embedded SQLite executor, all I/O
   through xtc.

For deep architecture: read `PLAN.md`.  For specific topics:

* `docs/M_TLS.md` -- TLS support strategy.
* `docs/M_BEAM_LESSONS.md` -- 12 BEAM/OTP production issues and
  how libxtc handles each.
* `docs/M_SQLXTC_HARDFORK.md` -- the plan for breaking SQLite into
  concurrent xtc procs.
* `docs/M_MULTI_HEAD_RECV.md` -- when not to add a feature, with
  research from the literature.

For API reference:

* Per-primitive man pages live in `man/man3/` after `make install`.
* The headers in `src/inc/xtc_*.h` are the source of truth; each
  is heavily commented.
* `dist/s_include` regenerates the per-subsystem prototype headers
  from `PUBLIC:` markers.

## When you hit something missing

xtc is pre-1.0.  Some things aren't there yet, some things are
there but rough.  `docs/KNOWN_ISSUES.md` lists the known gaps;
the rexis and sqlxtc examples each have a "Gaps in xtc" section
documenting what they wished xtc had.

Patches welcome.  Open issues at
<https://codeberg.org/gregburd/libxtc/issues>.

## Style discipline

If you contribute back: BSD KNF as encoded in `.clang-format`.
ASCII-only in source / docs / comments / commit messages.  Run
`dist/s_async` and `dist/s_cfg` lints before committing.  Property
tests via hegel-c are encouraged for any new primitive.  See
`AGENTS.md` for the full list of project conventions.
