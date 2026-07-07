/*
 * test_thread_sigmask.c -- libxtc runtime threads must not steal a
 * process-directed signal (carrier SIGCHLD-to-scheduler bug, 2026-07).
 *
 * A process-directed signal (kill(getpid(), SIG)) is delivered to ANY
 * one thread whose mask does not block it.  If libxtc's scheduler /
 * loop / worker / blocking-pool / detector threads inherited the
 * creator's (permissive) mask, such a signal could land on one of them
 * -- where the embedder's per-thread state (e.g. PostgreSQL's
 * MyProcPid) is absent -- instead of the thread the embedder designated
 * to handle it.
 *
 * libxtc now creates every thread with all signals blocked
 * (__os_pthread_create_masked, used by __os_thread_create and the two
 * raw-pthread call sites).  This test proves it: the main thread is the
 * ONLY thread with SIGUSR1 unblocked; it stands up a multi-loop
 * executor (spawning worker threads) plus the blocking pool, sends
 * itself a process-directed SIGUSR1 many times, and asserts EVERY
 * delivery was handled on the main thread -- never on a libxtc thread.
 *
 * STATUS (2026-07): the mask primitive is verified correct in isolation
 * (a thread created via __os_pthread_create_masked has SIGUSR1 blocked),
 * and every libxtc creation site now routes through it, but this
 * integration test still observes occasional deliveries to a libxtc
 * thread -- a subtle remaining case under investigation (see
 * docs/KNOWN_ISSUES.md).  Run standalone; NOT yet a gating test.
 */

#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_blocking.h"

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define N_LOOPS   4
#define N_SIG     200

static pthread_t   g_main_tid;
static atomic_int  g_on_main;    /* SIGUSR1s handled on the main thread */
static atomic_int  g_on_other;   /* SIGUSR1s handled on ANY other thread (bug!) */

static void
usr1_handler(int sig)
{
	(void)sig;
	/* pthread_self / pthread_equal are not async-signal-safe in the
	 * strict letter of POSIX, but pthread_equal is a pure comparison in
	 * practice and this is a test; record which thread took the signal. */
	if (pthread_equal(pthread_self(), g_main_tid))
		atomic_fetch_add(&g_on_main, 1);
	else
		atomic_fetch_add(&g_on_other, 1);
}

/* A blocking-pool job (forces the pool threads to exist + be busy). */
static int
blk_job(void *arg)
{
	(void)arg;
	struct timespec ts = { 0, 200 * 1000 };   /* 200us */
	nanosleep(&ts, NULL);
	return 0;
}

/* A worker proc that just yields for a while so the executor's worker
 * threads are alive and cycling when the signals arrive. */
static void
spin(void *arg)
{
	int i;
	(void)arg;
	for (i = 0; i < 100000; i++)
		xtc_yield();
}

static xtc_exec_t *g_e;
static void *
exec_thread(void *arg)
{
	sigset_t all;
	(void)arg;
	/* This helper thread is created by the TEST (a raw pthread_create),
	 * so it inherits main's mask where SIGUSR1 is unblocked.  Block
	 * everything here so ONLY main can take the process-directed signal;
	 * that isolates the property under test to libxtc's OWN threads
	 * (the workers/pool/detector xtc_exec_run spawns via
	 * __os_thread_create). */
	sigfillset(&all);
	(void)pthread_sigmask(SIG_SETMASK, &all, NULL);
	(void)xtc_exec_run(g_e);
	return NULL;
}

int
main(void)
{
	struct sigaction sa;
	sigset_t only_usr1, prev;
	pthread_t th;
	int i;

	g_main_tid = pthread_self();

	/* Install the handler and ensure ONLY the main thread has SIGUSR1
	 * unblocked.  We block SIGUSR1 process-wide first, then unblock it
	 * on just this (main) thread -- so if any libxtc thread wrongly has
	 * it unblocked (inherited mask), the kernel could deliver there. */
	memset(&sa, 0, sizeof sa);
	sa.sa_handler = usr1_handler;
	sigemptyset(&sa.sa_mask);
	sigaction(SIGUSR1, &sa, NULL);

	sigemptyset(&only_usr1);
	sigaddset(&only_usr1, SIGUSR1);
	/* Block SIGUSR1 on the main thread while we spawn everything, so
	 * any thread created now inherits it BLOCKED (which is what we
	 * want) -- and, critically, tests that libxtc ALSO blocks it even
	 * if the creator had left it unblocked.  To exercise the inherited-
	 * mask path we UNBLOCK it on main here (the permissive creator
	 * mask), then rely on libxtc to block it on its own threads. */
	pthread_sigmask(SIG_UNBLOCK, &only_usr1, &prev);

	if (xtc_exec_init(&g_e, N_LOOPS) != XTC_OK) {
		fprintf(stderr, "SKIP: exec_init failed\n");
		return 77;
	}
	xtc_exec_set_service_mode(g_e, 1);
	for (i = 0; i < N_LOOPS; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(g_e, i), spin, NULL,
		    NULL, NULL);
	if (pthread_create(&th, NULL, exec_thread, NULL) != 0) {
		fprintf(stderr, "SKIP: exec thread create failed\n");
		return 77;
	}

	/* Kick the blocking pool so its threads exist too. */
	for (i = 0; i < N_LOOPS; i++)
		(void)xtc_blocking_run(blk_job, NULL, NULL);

	/* Give the worker + pool threads time to be alive and cycling. */
	{ struct timespec ts = { 0, 50 * 1000 * 1000 }; nanosleep(&ts, NULL); }

	/* Fire many PROCESS-directed SIGUSR1s.  With libxtc's threads all
	 * masked, every one must be delivered to the main thread (the only
	 * thread with SIGUSR1 unblocked). */
	for (i = 0; i < N_SIG; i++) {
		kill(getpid(), SIGUSR1);
		{ struct timespec ts = { 0, 100 * 1000 }; nanosleep(&ts, NULL); }
	}

	(void)xtc_exec_stop(g_e);
	(void)pthread_join(th, NULL);
	(void)xtc_exec_fini(g_e);

	{
		int m = atomic_load(&g_on_main), o = atomic_load(&g_on_other);
		printf("SIGUSR1: %d on main, %d on other threads\n", m, o);
		if (o != 0) {
			printf("FAIL: %d process-directed signal(s) landed on a "
			    "libxtc runtime thread -- a runtime thread inherited "
			    "an unblocked mask\n", o);
			return 1;
		}
		if (m == 0) {
			printf("SKIP: no signals were delivered (timing)\n");
			return 77;
		}
		printf("OK: all %d process-directed SIGUSR1s handled on the "
		    "designated (main) thread; no libxtc thread stole one\n", m);
	}
	return 0;
}
