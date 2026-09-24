/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/m99/rexis_harness.h
 *	Start / stop a real rexis-server-xtc for the m99 tests.  Header-only
 *	(static functions); each test binary includes it once.
 *
 *	Port: never a fixed number.  The harness asks the kernel for a free
 *	loopback port (bind port 0, read it back, close) and starts the
 *	server on it.  That leaves a window in which another process could
 *	take the port, so a start whose server exits before it says it is
 *	listening is RETRIED on a fresh port (rexis binds exclusively -- no
 *	SO_REUSEPORT -- so a collision is a clean bind failure, never two
 *	servers splitting one port's traffic).
 *
 *	Readiness: rexis prints "rexis: listening on HOST:PORT" to stdout
 *	once its socket is listening; the harness waits (bounded) for that
 *	line instead of sleeping and hoping.
 *
 *	Stop: SIGTERM, then a BOUNDED wait.  rexis_stop() returns the
 *	server's exit status, or -1 if it had to be SIGKILLed: a server that
 *	ignores SIGTERM is a failure the tests assert on, not a hang.
 *
 *	Orphans: the child sets PR_SET_PDEATHSIG (Linux), so a test runner
 *	killed by a timeout cannot leave servers holding ports behind.
 *
 *	The binary is $REXIS_SERVER, else ../../examples/05_rexis/rexis-server-xtc
 *	relative to the current directory (test/m99).
 */

#ifndef REXIS_HARNESS_H
#define REXIS_HARNESS_H

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

#define REXIS_DEFAULT_BIN  "../../examples/05_rexis/rexis-server-xtc"
#define REXIS_READY_MS     5000   /* max wait for the "listening" line */
#define REXIS_STOP_MS      5000   /* max wait for exit after SIGTERM */
#define REXIS_START_TRIES  5

typedef struct rexis_srv {
	pid_t pid;
	int   port;
} rexis_srv_t;

static int64_t
rexis_now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* A loopback port that was free a moment ago. */
static int
rexis_free_port(void)
{
	struct sockaddr_in a;
	socklen_t len = sizeof a;
	int fd, port = -1;

	if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
		return -1;
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (bind(fd, (struct sockaddr *)&a, sizeof a) == 0 &&
	    getsockname(fd, (struct sockaddr *)&a, &len) == 0)
		port = ntohs(a.sin_port);
	close(fd);
	return port;
}

/* One attempt.  0 = listening, -1 = the server exited / never said so. */
static int
rexis_try_start(rexis_srv_t *s, int port, const char *const *extra,
    int n_extra)
{
	const char *bin = getenv("REXIS_SERVER");
	char port_s[16], line[256];
	char *argv[40];
	int pfd[2], argc = 0, i;
	size_t used = 0;
	int64_t deadline;
	pid_t pid;

	if (bin == NULL || *bin == '\0')
		bin = REXIS_DEFAULT_BIN;
	snprintf(port_s, sizeof port_s, "%d", port);
	argv[argc++] = (char *)bin;
	argv[argc++] = "--host=127.0.0.1";
	argv[argc++] = "-p";
	argv[argc++] = port_s;
	for (i = 0; i < n_extra && argc < 38; i++)
		argv[argc++] = (char *)extra[i];
	argv[argc] = NULL;

	if (pipe(pfd) != 0)
		return -1;
	fflush(NULL);
	if ((pid = fork()) < 0) {
		close(pfd[0]);
		close(pfd[1]);
		return -1;
	}
	if (pid == 0) {
#ifdef __linux__
		(void)prctl(PR_SET_PDEATHSIG, SIGKILL);
#endif
		dup2(pfd[1], STDOUT_FILENO);
		close(pfd[0]);
		close(pfd[1]);
		execv(bin, argv);
		fprintf(stderr, "rexis_harness: exec %s: %s\n", bin,
		    strerror(errno));
		_exit(127);
	}
	close(pfd[1]);

	/* Read until the ready line, EOF (the server exited: e.g. the port
	 * was taken) or the deadline. */
	deadline = rexis_now_ms() + REXIS_READY_MS;
	for (;;) {
		struct pollfd p = { pfd[0], POLLIN, 0 };
		int64_t left = deadline - rexis_now_ms();
		ssize_t n;

		if (left <= 0 || poll(&p, 1, (int)left) <= 0)
			break;
		n = read(pfd[0], line + used, sizeof line - 1 - used);
		if (n <= 0)
			break;
		used += (size_t)n;
		line[used] = '\0';
		if (strstr(line, "rexis: listening on") != NULL &&
		    strchr(line, '\n') != NULL) {
			/* rexis writes nothing else to stdout (logs go to
			 * stderr) and ignores SIGPIPE, so closing our end is
			 * safe. */
			close(pfd[0]);
			s->pid = pid;
			s->port = port;
			return 0;
		}
		if (used >= sizeof line - 1)
			break;
	}
	close(pfd[0]);
	kill(pid, SIGKILL);
	waitpid(pid, NULL, 0);
	return -1;
}

/* Start rexis with `extra` args on a free port.  0 on success. */
static int
rexis_start(rexis_srv_t *s, const char *const *extra, int n_extra)
{
	int t, port;

	s->pid = -1;
	s->port = -1;
	for (t = 0; t < REXIS_START_TRIES; t++) {
		if ((port = rexis_free_port()) <= 0)
			continue;
		if (rexis_try_start(s, port, extra, n_extra) == 0)
			return 0;
	}
	fprintf(stderr, "rexis_harness: server did not start after %d tries\n",
	    REXIS_START_TRIES);
	return -1;
}

/*
 * SIGTERM, then wait up to REXIS_STOP_MS.  Returns the exit status
 * (0 = clean shutdown), 128+sig if it died on a signal, or -1 if it did
 * not exit in time (it is then SIGKILLed and reaped, so nothing leaks).
 */
static int
rexis_stop(rexis_srv_t *s)
{
	int64_t deadline;
	int status, rc = -1;

	if (s->pid <= 0)
		return -1;
	kill(s->pid, SIGTERM);
	deadline = rexis_now_ms() + REXIS_STOP_MS;
	for (;;) {
		pid_t r = waitpid(s->pid, &status, WNOHANG);
		if (r == s->pid) {
			rc = WIFEXITED(status) ? WEXITSTATUS(status) :
			    128 + WTERMSIG(status);
			break;
		}
		if (r < 0 || rexis_now_ms() >= deadline) {
			kill(s->pid, SIGKILL);
			waitpid(s->pid, NULL, 0);
			break;
		}
		usleep(10 * 1000);
	}
	s->pid = -1;
	return rc;
}

/* Connect to the server with a receive timeout (so a wedged server
 * fails a test instead of hanging it). */
static int
rexis_connect(const rexis_srv_t *s, int rcv_timeout_ms)
{
	struct sockaddr_in a;
	struct timeval tv;
	int fd;

	if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
		return -1;
	tv.tv_sec = rcv_timeout_ms / 1000;
	tv.tv_usec = (rcv_timeout_ms % 1000) * 1000;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
	memset(&a, 0, sizeof a);
	a.sin_family = AF_INET;
	a.sin_port = htons((uint16_t)s->port);
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect(fd, (struct sockaddr *)&a, sizeof a) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/* Build one RESP command array into buf; returns its length. */
static int
rexis_build(char *buf, size_t cap, int argc, ...)
{
	va_list ap;
	int i, len;
	const char *arg;

	len = snprintf(buf, cap, "*%d\r\n", argc);
	va_start(ap, argc);
	for (i = 0; i < argc; i++) {
		arg = va_arg(ap, const char *);
		len += snprintf(buf + len, cap - (size_t)len, "$%zu\r\n%s\r\n",
		    strlen(arg), arg);
	}
	va_end(ap);
	return len;
}

#endif /* REXIS_HARNESS_H */
