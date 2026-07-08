/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/concurrency/test_proc_wake_crossthread.c
 *	Regression guard for xtc_proc_wake(): a fiber parked in
 *	xtc_proc_wait_fd on an epoll fd, with the readiness produced by a
 *	FOREIGN OS thread, must be reliably resumed when that thread pairs
 *	the readiness with xtc_proc_wake(pid).  This is the PostgreSQL
 *	cross-thread-latch shape (a foreign I/O-worker thread SetLatches a
 *	backend fiber): the waker makes a self-pipe watched by the loop's
 *	epoll readable AND pokes the loop via xtc_proc_wake.
 *
 *	Standalone (exit 0 = every round woke; exit 1 = a lost wake caught
 *	by the alarm; exit 77 = environment skip).  Not munit: it needs a
 *	real foreign pthread + an alarm-based hang detector.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

/* This guard exercises the cross-thread wake using a Linux epoll fd as
 * the watched descriptor (the PG shape).  epoll is Linux-only; on every
 * other platform the test compiles to a clean SKIP so it stays in the
 * portable TESTS_C set without breaking macOS / *BSD / Windows builds.
 * The wake fix itself (xtc_proc_wake + the prepare/park race latch) is
 * backend-agnostic and covered on those platforms by make check. */
#if !defined(__linux__)
#include <stdio.h>
int
main(void)
{
	printf("SKIP: test_proc_wake_crossthread needs Linux epoll\n");
	return 77;
}
#else

#include <sys/epoll.h>
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

static int          g_epfd = -1, g_pr = -1, g_pw = -1;
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
	if (xtc_proc_wait_fd(g_epfd, XTC_IO_READABLE, -1, &rev) == XTC_OK)
		atomic_fetch_add(&g_woke, 1);
	(void)read(g_pr, buf, sizeof buf);   /* drain the pipe */
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
	 * poke the target loop -- the guaranteed cross-thread resume. */
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
	struct epoll_event ev;

	if (pipe(pfd) != 0)
		return 77;
	g_pr = pfd[0];
	g_pw = pfd[1];
	g_epfd = epoll_create1(EPOLL_CLOEXEC);
	if (g_epfd < 0)
		return 77;
	ev.events = EPOLLIN;
	ev.data.fd = g_pr;
	if (epoll_ctl(g_epfd, EPOLL_CTL_ADD, g_pr, &ev) != 0)
		return 77;

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
	close(g_epfd);
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

#endif /* __linux__ */
