/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/dist/probe_workload.c
 *	A fixed workload whose USDT probe counts are known by construction,
 *	for test/dist/test_probes.sh.  Everything runs on one loop and every
 *	step is ordered by a message hand-off, so no count depends on timing:
 *
 *	  proc-spawn / proc-start / proc-exit     2 each
 *	  msg-send / msg-recv                     2*ROUNDS + 2*CONTEND each
 *	  lock-wait-start / lock-wait-done        CONTEND each, kind 1 (amutex)
 *	  fiber-park reason 6 (sync)              CONTEND
 *	  fiber-park reason 2 (sleep)             CONTEND
 *
 *	Each contention round: main takes the lock, sends "go", and sleeps
 *	holding it; echo, woken by "go", blocks on the lock (one contended
 *	wait); main's unlock hands the lock to echo, which releases it and
 *	replies "done".  Exits 0 iff the workload completed.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_sync.h"

#define ROUNDS  50
#define CONTEND 5

static xtc_amutex_t *g_mu;
static xtc_pid_t     g_echo, g_main;
static int           g_done;

static int
recv_one(void)
{
	void *m;
	size_t sz;
	if (xtc_recv(&m, &sz, -1) != XTC_OK)
		return -1;
	xtc_free(m);
	return 0;
}

static void
echo_proc(void *arg)
{
	int i;
	(void)arg;
	for (i = 0; i < ROUNDS; i++)
		if (recv_one() != 0 || xtc_send(g_main, &i, sizeof i) != XTC_OK)
			return;
	for (i = 0; i < CONTEND; i++) {
		if (recv_one() != 0 || xtc_amutex_lock(g_mu, -1) != XTC_OK)
			return;
		(void)xtc_amutex_unlock(g_mu);
		if (xtc_send(g_main, &i, sizeof i) != XTC_OK)
			return;
	}
	g_done++;
}

static void
main_proc(void *arg)
{
	int i;
	(void)arg;
	for (i = 0; i < ROUNDS; i++)
		if (xtc_send(g_echo, &i, sizeof i) != XTC_OK || recv_one() != 0)
			return;
	for (i = 0; i < CONTEND; i++) {
		if (xtc_amutex_lock(g_mu, -1) != XTC_OK ||
		    xtc_send(g_echo, &i, sizeof i) != XTC_OK)
			return;
		(void)xtc_proc_sleep(1000000);   /* echo blocks on g_mu here */
		(void)xtc_amutex_unlock(g_mu);
		if (recv_one() != 0)
			return;
	}
	g_done++;
}

int
main(int argc, char **argv)
{
	xtc_loop_t *loop;

	if (argc > 1 && strcmp(argv[1], "--help") == 0) {
		printf("usage: %s\nA fixed workload for the USDT probe test.\n",
		    argv[0]);
		return 0;
	}
	if (xtc_amutex_create(&g_mu) != XTC_OK ||
	    xtc_loop_init(&loop) != XTC_OK)
		return 1;
	if (xtc_proc_spawn(loop, echo_proc, NULL, NULL, &g_echo) != XTC_OK ||
	    xtc_proc_spawn(loop, main_proc, NULL, NULL, &g_main) != XTC_OK)
		return 1;
	if (xtc_loop_run(loop) != XTC_OK)
		return 1;
	(void)xtc_loop_fini(loop);
	xtc_amutex_destroy(g_mu);
	if (g_done != 2) {
		fprintf(stderr, "probe_workload: incomplete (%d of 2)\n", g_done);
		return 1;
	}
	return 0;
}
