/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/concurrency/test_proc_wake_crossthread.c
 *	Regression guard for xtc_proc_wake(): a fiber parked in
 *	xtc_proc_wait_fd on a plain pipe, with the readiness produced by
 *	a FOREIGN OS thread, must be reliably resumed when that thread
 *	pairs the readiness with xtc_proc_wake(pid).  This is the
 *	PostgreSQL cross-thread-latch shape (a foreign I/O-worker thread
 *	SetLatches a backend fiber): the waker makes the watched fd
 *	readable AND pokes the loop via xtc_proc_wake.
 *
 *	PORTABLE ACROSS BACKENDS.  The watched descriptor is a plain
 *	pipe read-end, which xtc_proc_wait_fd can watch through ANY I/O
 *	backend (epoll, io_uring, kqueue, poll, select).  This matters
 *	because the cross-thread loop-wake that xtc_proc_wake relies on
 *	is a DIFFERENT mechanism per backend -- a self-pipe/eventfd on
 *	epoll, an EVFILT_USER + NOTE_TRIGGER event on kqueue -- so
 *	running this guard on macOS / *BSD (kqueue) exercises a genuinely
 *	distinct code path from the Linux (epoll) one.  An earlier
 *	version used an epoll fd and SKIPped everywhere but Linux, which
 *	left the kqueue cross-thread wake path uncovered by this guard.
 *
 *	Standalone (exit 0 = every round woke; exit 1 = a lost wake
 *	caught by the alarm; exit 77 = environment skip).  Not munit: it
 *	needs a real foreign pthread + an alarm-based hang detector.
 *	Windows uses a different (IOCP) wake path and no POSIX pipe/
 *	pthread here, so it SKIPs; its wake path is covered by the MSVC
 *	smoke + the loopback echo in test/msvc.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#if defined(_WIN32)
#include <stdio.h>
int
main(void)
{
	printf("SKIP: test_proc_wake_crossthread uses POSIX pipe/pthread; "
	    "the Windows IOCP wake path is covered by test/msvc\n");
	return 77;
}
#else

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>

#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_proc.h"

#define ROUNDS 100

static int          g_pr = -1, g_pw = -1;
static xtc_exec_t  *g_e;
static atomic_int   g_woke;
static atomic_int   g_ready;
static xtc_pid_t    g_waiter;

static void
waiter(void *arg)
{
	uint32_t rev = 0;
	char buf[8];

	(void)arg;
	g_waiter = xtc_self();
	atomic_store(&g_ready, 1);
	/* Park on the pipe read-end through whatever I/O backend this
	 * platform built with (epoll / io_uring / kqueue / poll / select). */
	if (xtc_proc_wait_fd(g_pr, XTC_IO_READABLE, -1, &rev) == XTC_OK)
		atomic_fetch_add(&g_woke, 1);
	{ ssize_t rd = read(g_pr, buf, sizeof buf); (void)rd; }   /* drain */
	(void)xtc_exec_stop(g_e);
}

static void *
foreign(void *arg)
{
	struct timespec ts = { 0, 40 * 1000 * 1000 };   /* 40ms */

	(void)arg;
	while (atomic_load(&g_ready) == 0)
		;
	nanosleep(&ts, NULL);
	/* Make the watched fd readable (as an embedder latch would) AND
	 * poke the target loop -- the guaranteed cross-thread resume.
	 * The loop-poke goes through the active backend's wakeup
	 * primitive (epoll self-pipe or kqueue EVFILT_USER). */
	if (write(g_pw, "x", 1) != 1)
		perror("write");
	(void)xtc_proc_wake(g_waiter);
	return NULL;
}

static void
on_alarm(int s)
{
	(void)s;
	_exit(1);   /* a round hung: the wake was lost */
}

static int
one_round(void)
{
	pthread_t th;
	int pfd[2];

	if (pipe(pfd) != 0)
		return 77;
	g_pr = pfd[0];
	g_pw = pfd[1];

	atomic_store(&g_woke, 0);
	atomic_store(&g_ready, 0);
	if (xtc_exec_init(&g_e, 3) != XTC_OK)
		return 77;
	xtc_exec_set_service_mode(g_e, 1);
	if (xtc_proc_spawn(xtc_exec_loop(g_e, 0), waiter, NULL, NULL, NULL)
	    != XTC_OK)
		return 77;
	if (pthread_create(&th, NULL, foreign, NULL) != 0)
		return 77;

	(void)xtc_exec_run(g_e);
	pthread_join(th, NULL);
	(void)xtc_exec_fini(g_e);
	close(g_pr);
	close(g_pw);
	return atomic_load(&g_woke) ? 0 : 1;
}

int
main(void)
{
	int r, miss = 0;

	signal(SIGALRM, on_alarm);
	for (r = 0; r < ROUNDS; r++) {
		int rc;
		alarm(6);
		rc = one_round();
		if (rc == 77) {
			printf("SKIP: setup failed at round %d\n", r);
			return 77;
		}
		if (rc == 1) {
			printf("round %d: NO WAKE\n", r);
			miss++;
		}
	}
	alarm(0);
	printf("%d/%d rounds lost the wake\n", miss, ROUNDS);
	return miss ? 1 : 0;
}

#endif /* _WIN32 */
