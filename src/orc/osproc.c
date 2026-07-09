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
#include "xtc_sim.h"
#include "loop_int.h"   /* __xtc_current_loop: place the sim child fiber */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>   /* pthread_sigmask: reset the child's mask post-fork */
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

	/* Deterministic-simulation model.  Under sim there is no real OS
	 * process (a fork'd child runs on the real scheduler/clock, outside
	 * the sim's control -- the same not-coverable boundary FoundationDB
	 * hits, which is why FDB models a "process" as an in-process actor
	 * with a simulated lifecycle rather than fork/exec'ing under sim).
	 * We do the same: the fn-callback child runs as an xtc_proc FIBER
	 * and its lifecycle (running -> exited-with-status, signalled) is
	 * modelled here on the sim clock. */
	int          is_sim;
	_Atomic int  sim_done;     /* 1 once the fiber returned / was killed */
	int          sim_status;   /* raw waitpid-style status when done */
	_Atomic int  sim_signal;   /* last signal delivered (0 = none) */
};

/* Argument for the simulated child fiber. */
struct sim_child_arg {
	struct xtc_osproc *p;
	int  (*fn)(int, void *);
	void  *arg;
};

static _Atomic long g_sim_pid_next = 1;   /* synthetic sim pids */

static void
sim_child_main(void *a)
{
	struct sim_child_arg *ca = a;
	struct xtc_osproc *p = ca->p;
	int r;

	/* A signal delivered before we start (or during) terminates the
	 * child: model SIGKILL/SIGTERM/SIGINT as termination.  Otherwise run
	 * the callback and encode its return as a normal exit status. */
	r = ca->fn(-1, ca->arg);
	if (atomic_load(&p->sim_signal) != 0 && !atomic_load(&p->sim_done)) {
		/* Killed by signal: encode as WIFSIGNALED (low 7 bits = sig). */
		p->sim_status = atomic_load(&p->sim_signal) & 0x7f;
	} else {
		p->sim_status = (r & 0xff) << 8;   /* WIFEXITED, code r */
	}
	atomic_store(&p->sim_done, 1);
	__os_free(ca);
}

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

	/* Deterministic simulation: model the fn-callback child as an
	 * in-process fiber with a simulated lifecycle (FoundationDB's
	 * "process = actor" pattern), instead of fork()ing a real child
	 * whose scheduling/clock the sim cannot control.  The exec (argv)
	 * path has no in-process equivalent -- there is no real program to
	 * run under sim -- and the live control SOCKET would need a real
	 * kernel socketpair, so both decline cleanly under sim (a consumer
	 * that needs them is exercising the not-coverable real-kernel tier,
	 * matching FDB pushing real exec out to fdbmonitor).  The common
	 * "run isolated work, collect its exit status" contract IS
	 * modelled.  ADDITIVE: gated on __xtc_sim_active(); the production
	 * fork path below is byte-identical. */
	if (__xtc_sim_active()) {
		struct sim_child_arg *ca;
		xtc_pid_t cpid;
		if (opts->argv != NULL || opts->ctrl_socket) {
			__os_free(p);
			return XTC_E_NOSYS;   /* real-kernel tier, not simulated */
		}
		if (__os_calloc(1, sizeof *ca, (void **)&ca) != XTC_OK) {
			__os_free(p);
			return XTC_E_NOMEM;
		}
		p->is_sim = 1;
		p->pid = (pid_t)atomic_fetch_add(&g_sim_pid_next, 1);
		atomic_init(&p->sim_done, 0);
		atomic_init(&p->sim_signal, 0);
		ca->p = p;
		ca->fn = opts->fn;
		ca->arg = opts->arg;
		if (xtc_proc_spawn(__xtc_current_loop, sim_child_main, ca,
		    NULL, &cpid) != XTC_OK) {
			__os_free(ca);
			__os_free(p);
			return XTC_E_INTERNAL;
		}
		*out = p;
		return XTC_OK;
	}

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
		/* Reset the signal mask to a clean default.  The parent was a
		 * proc fiber, whose ucontext uc_sigmask blocks all signals
		 * (except the preemption timer) so process-directed signals do
		 * not land on runtime scheduler threads; that mask is inherited
		 * across fork and would leave this child (and any image it
		 * exec's) with everything blocked -- breaking its own signal
		 * handling and any wait that relies on delivery.  A fresh child
		 * process must start from an empty mask.  pthread_sigmask is
		 * async-signal-safe. */
		{
			sigset_t empty;
			sigemptyset(&empty);
			(void)pthread_sigmask(SIG_SETMASK, &empty, NULL);
		}
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
	if (p->is_sim) {
		/* Model the signal: record it, and for a terminating signal
		 * mark the (not-yet-exited) child done with a signalled
		 * status.  A cooperative child could also observe sim_signal. */
		xtc_osproc_t *m = (xtc_osproc_t *)p;   /* atomics are mutable */
		if (atomic_load(&m->sim_done))
			return XTC_E_INVAL;             /* already exited */
		atomic_store(&m->sim_signal, sig);
		if (sig == 9 /*SIGKILL*/ || sig == 15 /*SIGTERM*/ ||
		    sig == 2 /*SIGINT*/) {
			m->sim_status = sig & 0x7f;     /* WIFSIGNALED */
			atomic_store(&m->sim_done, 1);
		}
		return XTC_OK;
	}
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
	if (p->is_sim) {
		if (p->reaped) {
			if (status != NULL) *status = p->status;
			return XTC_OK;
		}
		if (!atomic_load(&((xtc_osproc_t *)p)->sim_done))
			return XTC_E_AGAIN;             /* still running */
		((xtc_osproc_t *)p)->reaped = 1;
		((xtc_osproc_t *)p)->status = p->sim_status;
		if (status != NULL) *status = p->sim_status;
		return XTC_OK;
	}
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
	if (p->is_sim) {
		/* No real fds or child; the fiber (if still running) is owned by
		 * the executor and will complete on the sim clock.  Just free
		 * the handle. */
		__os_free(p);
		return;
	}
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
			free(reply);   /* XTC_RAW_OK: reply is malloc'd by the user handler (see xtc_osproc.h), freed with the matching free */
			return hr;
		}
		rc = xtc_net_send_frame(ctrl_fd, reply, reply_len);
		free(reply);   /* XTC_RAW_OK: user-handler-malloc'd reply */
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
