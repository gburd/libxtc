/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * src/orc/osproc.c
 *	OS-process spawn + control socket + lifecycle (R3).
 *	See src/inc/xtc_osproc.h for the contract.
 */

#include "xtc_int.h"
#include "xtc_osproc.h"

#if !defined(_WIN32)

#include "xtc_proc.h"
#include "xtc_io.h"
#include "xtc_net.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>

#if defined(__linux__)
#include <sys/syscall.h>
#endif

struct xtc_osproc {
	pid_t pid;
	int   ctrl_fd;     /* parent end of the control socket, or -1 */
	int   pidfd;       /* Linux pidfd for pollable exit, or -1 */
	int   reaped;      /* 1 once waitpid has collected the child */
	int   status;      /* raw waitpid status, valid when reaped */
};

static int
__pidfd_open(pid_t pid)
{
#if defined(__linux__) && defined(SYS_pidfd_open)
	long fd = syscall(SYS_pidfd_open, pid, 0);
	if (fd < 0) return -1;
	return (int)fd;
#else
	(void)pid;
	return -1;
#endif
}

int
xtc_osproc_spawn(const xtc_osproc_opts_t *opts, xtc_osproc_t **out)
{
	struct xtc_osproc *p;
	int sv[2] = { -1, -1 };
	int rc;
	pid_t pid;

	if (out == NULL || opts == NULL)
		return XTC_E_INVAL;
	*out = NULL;
	/* Exactly one of argv / fn. */
	if ((opts->argv == NULL) == (opts->fn == NULL))
		return XTC_E_INVAL;

	if ((rc = __os_calloc(1, sizeof *p, (void **)&p)) != XTC_OK)
		return rc;
	p->pid = -1;
	p->ctrl_fd = -1;
	p->pidfd = -1;

	if (opts->ctrl_socket) {
		if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
			__os_free(p);
			return XTC_E_INTERNAL;
		}
	}

	pid = fork();
	if (pid < 0) {
		if (sv[0] >= 0) { (void)close(sv[0]); (void)close(sv[1]); }
		__os_free(p);
		return XTC_E_INTERNAL;
	}

	if (pid == 0) {
		/* ---- child ---- async-signal-safe only until exec/fn ---- */
		int child_fd = -1;
		if (opts->ctrl_socket) {
			(void)close(sv[0]);     /* parent end */
			child_fd = sv[1];
		}
		if (opts->fn != NULL) {
			int r = opts->fn(child_fd, opts->arg);
			_exit(r & 0xff);
		}
		/* exec path: publish the control fd in the environment so the
		 * new image can find it; it is intentionally not CLOEXEC. */
		if (child_fd >= 0) {
			char buf[32];
			(void)snprintf(buf, sizeof buf, "%d", child_fd);
			(void)setenv("XTC_CTRL_FD", buf, 1);
		}
		(void)execvp(opts->argv[0], opts->argv);
		_exit(127);                     /* exec failed */
	}

	/* ---- parent ---- */
	p->pid = pid;
	if (opts->ctrl_socket) {
		int fl;
		(void)close(sv[1]);             /* child end */
		p->ctrl_fd = sv[0];
		fl = fcntl(p->ctrl_fd, F_GETFL, 0);  /* XTC_BLOCKING_OK: fd flag query */
		if (fl != -1) (void)fcntl(p->ctrl_fd, F_SETFL, fl | O_NONBLOCK);  /* XTC_BLOCKING_OK: fd flag set */
		(void)fcntl(p->ctrl_fd, F_SETFD, FD_CLOEXEC);  /* XTC_BLOCKING_OK: fd flag set */
	}
	p->pidfd = __pidfd_open(pid);
	*out = p;
	return XTC_OK;
}

long
xtc_osproc_pid(const xtc_osproc_t *p)
{
	return (p != NULL) ? (long)p->pid : -1;
}

int
xtc_osproc_ctrl_fd(const xtc_osproc_t *p)
{
	return (p != NULL) ? p->ctrl_fd : -1;
}

int
xtc_osproc_signal(const xtc_osproc_t *p, int sig)
{
	if (p == NULL || p->pid <= 0)
		return XTC_E_INVAL;
	if (p->reaped)
		return XTC_E_INVAL;             /* already exited */
	if (kill(p->pid, sig) != 0)
		return XTC_E_INTERNAL;
	return XTC_OK;
}

int
xtc_osproc_try_wait(xtc_osproc_t *p, int *status)
{
	pid_t r;
	int st = 0;

	if (p == NULL || p->pid <= 0)
		return XTC_E_INVAL;
	if (p->reaped) {
		if (status != NULL) *status = p->status;
		return XTC_OK;
	}
	do {
		r = waitpid(p->pid, &st, WNOHANG);  /* XTC_BLOCKING_OK: WNOHANG never blocks */
	} while (r < 0 && errno == EINTR);
	if (r == 0)
		return XTC_E_AGAIN;             /* still running */
	if (r < 0)
		return XTC_E_INTERNAL;
	p->reaped = 1;
	p->status = st;
	if (status != NULL) *status = st;
	return XTC_OK;
}

int
xtc_osproc_wait(xtc_osproc_t *p, int *status, int64_t timeout_ns)
{
	int rc;
	int64_t waited = 0;
	const int64_t slice = 10LL * 1000 * 1000;   /* 10ms poll fallback */

	if (p == NULL || p->pid <= 0)
		return XTC_E_INVAL;

	for (;;) {
		rc = xtc_osproc_try_wait(p, status);
		if (rc != XTC_E_AGAIN)
			return rc;              /* exited (OK) or error */

		if (p->pidfd >= 0) {
			/* Park on the pidfd: readable once the child exits. */
			uint32_t revents = 0;
			int prc = xtc_proc_wait_fd(p->pidfd, XTC_IO_READABLE,
			    timeout_ns, &revents);
			if (prc == XTC_E_AGAIN)
				return XTC_E_AGAIN; /* timeout */
			/* Woken (or wait_fd unusable off a loop): reap below. */
			if (prc != XTC_OK && prc != XTC_E_INVAL)
				return prc;
			if (prc == XTC_E_INVAL) {
				/* Not on a loop fiber -- fall back to sleeping. */
				if (timeout_ns >= 0 && waited >= timeout_ns)
					return XTC_E_AGAIN;
				(void)xtc_proc_sleep(slice);
				waited += slice;
			}
			continue;
		}

		/* No pidfd: cooperative poll.  Sleep a slice (bounded by the
		 * deadline) and retry; never blocks the loop thread. */
		if (timeout_ns >= 0 && waited >= timeout_ns)
			return XTC_E_AGAIN;
		{
			int64_t nap = slice;
			if (timeout_ns >= 0 && timeout_ns - waited < nap)
				nap = timeout_ns - waited;
			if (xtc_proc_sleep(nap) != XTC_OK) {
				/* Off a loop: a plain nanosleep equivalent. */
				struct timespec ts;
				ts.tv_sec  = (time_t)(nap / 1000000000LL);
				ts.tv_nsec = (long)(nap % 1000000000LL);
				(void)nanosleep(&ts, NULL);  /* XTC_BLOCKING_OK: off-loop fallback only */
			}
			waited += nap;
		}
	}
}

void
xtc_osproc_destroy(xtc_osproc_t *p)
{
	if (p == NULL)
		return;
	if (p->ctrl_fd >= 0)
		(void)close(p->ctrl_fd);
	if (p->pidfd >= 0)
		(void)close(p->pidfd);
	/* Best-effort reap so an already-exited child does not linger as a
	 * zombie; a still-running child is left for the caller. */
	if (!p->reaped && p->pid > 0) {
		int st = 0;
		(void)waitpid(p->pid, &st, WNOHANG);  /* XTC_BLOCKING_OK: WNOHANG */
	}
	__os_free(p);
}

int
xtc_osproc_isolated_spawn(const char *name, int (*fn)(int, void *),
                          void *arg, xtc_osproc_t **out)
{
	xtc_osproc_opts_t o;
	memset(&o, 0, sizeof o);
	o.name = name;
	o.fn = fn;
	o.arg = arg;
	o.ctrl_socket = 1;
	return xtc_osproc_spawn(&o, out);
}

int
xtc_osproc_call(xtc_osproc_t *p, const void *req, size_t req_len,
                void **reply, size_t *reply_len, size_t max_reply,
                int64_t timeout_ns)
{
	int fd, rc;
	if (p == NULL) return XTC_E_INVAL;
	fd = xtc_osproc_ctrl_fd(p);
	if (fd < 0) return XTC_E_INVAL;
	if ((rc = xtc_net_send_frame(fd, req, req_len)) != XTC_OK)
		return rc;
	return xtc_net_recv_frame(fd, reply, reply_len, max_reply, timeout_ns);
}

int
xtc_osproc_serve(int ctrl_fd, xtc_osproc_handler_fn handler, void *arg)
{
	if (ctrl_fd < 0 || handler == NULL) return XTC_E_INVAL;
	for (;;) {
		void  *req = NULL, *reply = NULL;
		size_t req_len = 0, reply_len = 0;
		int    rc, hr;
		rc = xtc_net_recv_frame(ctrl_fd, &req, &req_len, 0, -1);
		if (rc != XTC_OK)
			return XTC_OK;   /* peer closed: clean stop */
		hr = handler(req, req_len, &reply, &reply_len, arg);
		__os_free(req);
		if (hr != XTC_OK) {
			free(reply);
			return hr;
		}
		rc = xtc_net_send_frame(ctrl_fd, reply, reply_len);
		free(reply);
		if (rc != XTC_OK)
			return rc;
	}
}

#else  /* _WIN32 -- fork/exec + pidfd have no Windows equivalent yet */

int  xtc_osproc_spawn(const xtc_osproc_opts_t *opts, xtc_osproc_t **out)
{ (void)opts; if (out) *out = NULL; return XTC_E_NOSYS; }
long xtc_osproc_pid(const xtc_osproc_t *p) { (void)p; return -1; }
int  xtc_osproc_ctrl_fd(const xtc_osproc_t *p) { (void)p; return -1; }
int  xtc_osproc_signal(const xtc_osproc_t *p, int sig) { (void)p; (void)sig; return XTC_E_NOSYS; }
int  xtc_osproc_try_wait(xtc_osproc_t *p, int *status) { (void)p; (void)status; return XTC_E_NOSYS; }
int  xtc_osproc_wait(xtc_osproc_t *p, int *status, int64_t t) { (void)p; (void)status; (void)t; return XTC_E_NOSYS; }
void xtc_osproc_destroy(xtc_osproc_t *p) { (void)p; }

int xtc_osproc_isolated_spawn(const char *n, int (*f)(int, void *), void *a, xtc_osproc_t **o)
{ (void)n; (void)f; (void)a; (void)o; return XTC_E_NOSYS; }
int xtc_osproc_call(xtc_osproc_t *p, const void *r, size_t rl, void **rp, size_t *rpl, size_t m, int64_t t)
{ (void)p; (void)r; (void)rl; (void)rp; (void)rpl; (void)m; (void)t; return XTC_E_NOSYS; }
int xtc_osproc_serve(int fd, xtc_osproc_handler_fn h, void *a)
{ (void)fd; (void)h; (void)a; return XTC_E_NOSYS; }

#endif /* _WIN32 */
