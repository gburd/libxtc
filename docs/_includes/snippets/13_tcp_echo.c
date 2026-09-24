/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * docs/_includes/snippets/13_tcp_echo.c -- a TCP echo server in one
 * file: xtc_net_listen + one process per connection + xtc_proc_wait_fd.
 *
 * The shape of most first real programs.  An acceptor process parks on
 * the listening socket; each accepted connection gets its own process
 * that parks on ITS socket, echoes whatever arrives, and exits on EOF.
 * No callbacks, no hand-written state machine: each connection is
 * straight-line code, and a parked process costs a fiber stack, not an
 * OS thread.
 *
 * So that this file can run as a test, it is also its own client: a
 * third process dials the server, sends a few lines, checks each echo,
 * and then asks the acceptor to stop.  A real server would drop the
 * client and run until signalled.  Exits 0 when every echo matched.
 */

/* !region full */
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>

#include "xtc.h"
#include "xtc_io.h"
#include "xtc_loop.h"
#include "xtc_net.h"
#include "xtc_proc.h"

#define MS (1000LL * 1000)

static int g_listen_fd = -1;
static int g_port;
static int g_stop;            /* set by the client when it is done */
static int g_echoed;          /* lines the client saw echoed back */

/* One connection: read what is there, write it back, repeat. */
static void
echo_conn(void *arg)
{
	int     fd = (int)(intptr_t)arg;
	char    buf[512];
	ssize_t n;
	uint32_t rev;

	for (;;) {
		n = recv(fd, buf, sizeof buf, 0);          /* fd is non-blocking */
		if (n == 0)
			break;                             /* peer closed */
		if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK)
			break;                             /* a real error */
		if (n > 0) {
			/* Small replies to a local peer: one send is enough
			 * here.  A production server loops on a short write,
			 * parking for XTC_IO_WRITABLE. */
			if (send(fd, buf, (size_t)n, 0) != n)
				break;
			continue;                          /* maybe more queued */
		}
		/* Nothing to read: park THIS process until the socket is
		 * readable.  The loop runs every other process meanwhile. */
		if (xtc_proc_wait_fd(fd, XTC_IO_READABLE | XTC_IO_HUP, -1,
		    &rev) != XTC_OK)
			break;
	}
	xtc_net_close(fd);
}

/* Accept connections and spawn a process for each, until g_stop. */
static void
acceptor(void *arg)
{
	xtc_loop_t *loop = arg;
	uint32_t    rev;
	xtc_pid_t   pid;
	int         fd;

	while (!g_stop) {
		fd = accept(g_listen_fd, NULL, NULL);
		if (fd >= 0) {
			if (xtc_net_setnonblock(fd) != XTC_OK ||
			    xtc_proc_spawn(loop, echo_conn, (void *)(intptr_t)fd,
			    NULL, &pid) != XTC_OK)
				xtc_net_close(fd);
			continue;
		}
		if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
			break;
		/* No pending connection: park on the listening socket.  The
		 * timeout lets us notice g_stop; a mailbox message (the
		 * client's "stop" below) wakes us at once. */
		(void)xtc_proc_wait_fd(g_listen_fd, XTC_IO_READABLE, 100 * MS,
		    &rev);
	}
	xtc_net_close(g_listen_fd);
}
/* !endregion full */

/* !region client */
/* The self-test client: a process on the same loop, so it too must
 * park rather than block while it waits for each echo. */
struct client_args { xtc_pid_t acceptor; };

static void
client(void *arg)
{
	struct client_args *ca = arg;
	const char *lines[] = { "hello\n", "from a fiber\n", "bye\n" };
	char     buf[64];
	uint32_t rev;
	size_t   i, got;
	ssize_t  n;
	int      fd;

	if (xtc_net_dial(XTC_NET_INET, "127.0.0.1", g_port, NULL, &fd) != XTC_OK)
		goto done;
	/* The dial is non-blocking: wait until the connect completes. */
	if (xtc_proc_wait_fd(fd, XTC_IO_WRITABLE, 2000 * MS, &rev) != XTC_OK)
		goto close;
	for (i = 0; i < sizeof lines / sizeof lines[0]; i++) {
		size_t len = strlen(lines[i]);
		if (send(fd, lines[i], len, 0) != (ssize_t)len)
			goto close;
		for (got = 0; got < len; ) {
			n = recv(fd, buf + got, sizeof buf - got, 0);
			if (n > 0) {
				got += (size_t)n;
				continue;
			}
			if (n == 0 || (errno != EAGAIN && errno != EWOULDBLOCK))
				goto close;
			if (xtc_proc_wait_fd(fd, XTC_IO_READABLE, 2000 * MS,
			    &rev) != XTC_OK)
				goto close;                /* no echo in 2 s */
		}
		if (got != len || memcmp(buf, lines[i], len) != 0)
			goto close;
		printf("echoed: %.*s", (int)len, buf);
		g_echoed++;
	}
close:
	xtc_net_close(fd);          /* EOF: the connection's process exits */
done:
	g_stop = 1;
	(void)xtc_send(ca->acceptor, "stop", 4);   /* wake it now */
}
/* !endregion client */

int
main(void)
{
	xtc_tcp_opts_t     topts = XTC_TCP_OPTS_DEFAULT;
	struct client_args ca;
	xtc_loop_t        *loop;
	xtc_pid_t          cpid;
	int                rc, status = 1;

	/* A write to a peer that already closed raises SIGPIPE, whose
	 * default action kills the process.  Servers ignore it and take
	 * the EPIPE error from send() instead. */
	(void)signal(SIGPIPE, SIG_IGN);

	if (xtc_loop_init(&loop) != XTC_OK)
		return 1;

	/* A fixed port can be taken by something else on a shared host, so
	 * try a few.  (A real server takes its port from its config.) */
	for (g_port = 47311; g_port < 47331; g_port++)
		if (xtc_net_listen(XTC_NET_INET, "127.0.0.1", g_port, &topts,
		    &g_listen_fd) == XTC_OK)
			break;
	if (g_listen_fd < 0) {
		fprintf(stderr, "no free port in 47311..47330\n");
		goto out;
	}
	printf("listening on 127.0.0.1:%d\n", g_port);

	if ((rc = xtc_proc_spawn(loop, acceptor, loop, NULL, &ca.acceptor))
	    != XTC_OK ||
	    (rc = xtc_proc_spawn(loop, client, &ca, NULL, &cpid)) != XTC_OK) {
		fprintf(stderr, "spawn: %s\n", xtc_strerror(rc));
		g_stop = 1;
		goto out;
	}
	/* Returns when every process has exited: the client finishes, the
	 * acceptor sees g_stop, each connection sees EOF. */
	if ((rc = xtc_loop_run(loop)) != XTC_OK)
		fprintf(stderr, "xtc_loop_run: %s\n", xtc_strerror(rc));
	else if (g_echoed == 3)
		status = 0;
	printf("%d of 3 lines echoed\n", g_echoed);
out:
	(void)xtc_loop_fini(loop);
	return status;
}
