/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * test/concurrency/test_pid_xexec.c
 *	A pid names one loop in the whole process.
 *
 *	Through 2.1 pid.loop_id was the executor slot + 1 (0 for every
 *	standalone loop), so two xtc_execs both used 1..n and two standalone
 *	loops both used 0.  A send to a pid minted on exec A, from a proc on
 *	exec B, reached the proc in the same slot of B; a send to one
 *	standalone loop's pid could reach the other.  xtc_send returned
 *	XTC_OK both times.
 *
 *	Phase 1: two execs of two loops and two standalone loops, one proc on
 *	each of the six loops, all spawned first in each loop so their pids
 *	collide in (local_id, gen).  Every proc sends to every other proc a
 *	message naming its intended receiver; every proc then receives until
 *	it has heard from the five others.  PASS: no proc receives a message
 *	addressed to another, all 30 arrive, the six loop ids (pid.loop_id
 *	and xtc_inspect_loops) are distinct and nonzero.
 *
 *	Phase 2: finalize exec A and standalone loop S1, then create exec C
 *	and standalone loop S2, whose first procs take the same (local_id,
 *	gen) as the dead ones.  A send to a dead pid -- from the main thread,
 *	from a proc on C and from a proc on S2 -- must fail, and the decoys
 *	on C and S2 must receive nothing.
 *
 *	Exit 0 pass, 1 fail.  2.1 (fail-before): phase 1 misdelivers.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#if defined(_WIN32)
#include <stdio.h>
int
main(void)
{
	printf("SKIP: test_pid_xexec uses pthreads\n");
	return 77;
}
#else

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_inspect.h"
#include "xtc_loop.h"
#include "xtc_proc.h"

#define NP        6                     /* A0 A1 B0 B1 S0 S1 */
#define RECV_NS   (2LL * 1000 * 1000 * 1000)

struct msg {
	int from, to;
};

static const char *g_name[NP] = { "A0", "A1", "B0", "B1", "S0", "S1" };
static xtc_atomic_pid_t g_pid[NP];
static atomic_int g_up;
static atomic_int g_got[NP];            /* messages addressed to me */
static atomic_int g_wrong[NP];          /* messages addressed to another */
static atomic_int g_send_fail;

static void
peer(void *arg)
{
	int me = (int)(intptr_t)arg, i, got = 0;
	struct msg m;
	void *buf;
	size_t sz;
	int64_t t0, now;

	xtc_atomic_pid_store(&g_pid[me], xtc_self());
	atomic_fetch_add(&g_up, 1);
	while (atomic_load(&g_up) < NP)
		(void)xtc_proc_sleep(1000000);
	for (i = 0; i < NP; i++) {
		if (i == me)
			continue;
		m.from = me;
		m.to = i;
		if (xtc_send(xtc_atomic_pid_load(&g_pid[i]), &m, sizeof m) !=
		    XTC_OK)
			atomic_fetch_add(&g_send_fail, 1);
	}
	t0 = xtc_clock_mono();
	while (got < NP - 1) {
		now = xtc_clock_mono();
		if (now - t0 > RECV_NS)
			break;
		if (xtc_recv(&buf, &sz, RECV_NS - (now - t0)) != XTC_OK)
			break;
		memcpy(&m, buf, sizeof m);
		xtc_free(buf);
		if (m.to == me) {
			atomic_fetch_add(&g_got[me], 1);
			got++;
		} else {
			atomic_fetch_add(&g_wrong[me], 1);
			printf("  %s received a message %s sent to %s\n",
			    g_name[me], g_name[m.from], g_name[m.to]);
		}
	}
}

static void *
run_exec(void *e)
{
	(void)xtc_exec_run(e);
	return NULL;
}

static void *
run_loop(void *l)
{
	(void)xtc_loop_run(l);
	return NULL;
}

/* Phase 2 state. */
static xtc_pid_t g_dead[2];             /* A0's and S1's pids */
static atomic_int g_decoy_got, g_dead_ok;

static void
decoy(void *arg)
{
	void *buf;
	size_t sz;

	(void)arg;
	while (xtc_recv(&buf, &sz, 300LL * 1000 * 1000) == XTC_OK) {
		xtc_free(buf);
		atomic_fetch_add(&g_decoy_got, 1);
	}
}

static void
dead_sender(void *arg)
{
	int i;

	(void)arg;
	for (i = 0; i < 2; i++)
		if (xtc_send(g_dead[i], "x", 1) == XTC_OK)
			atomic_fetch_add(&g_dead_ok, 1);
}

struct ids {
	uint32_t id[16];
	int n;
};

static int
collect(const xtc_loop_info_t *li, void *u)
{
	struct ids *s = u;

	if (s->n < 16)
		s->id[s->n++] = (uint32_t)li->loop_id;
	return 0;
}

int
main(void)
{
	xtc_exec_t *A = NULL, *B = NULL, *C = NULL;
	xtc_loop_t *S0 = NULL, *S1 = NULL, *S2 = NULL;
	xtc_pid_t pid[NP], p;
	pthread_t th[4];
	struct ids ids;
	int i, j, fail = 0, total = 0, wrong = 0;

	if (xtc_exec_init(&A, 2) != XTC_OK || xtc_exec_init(&B, 2) != XTC_OK ||
	    xtc_loop_init(&S0) != XTC_OK || xtc_loop_init(&S1) != XTC_OK) {
		printf("FAIL: setup\n");
		return 1;
	}
	if (xtc_proc_spawn(xtc_exec_loop(A, 0), peer, (void *)0, NULL,
	    &pid[0]) != XTC_OK ||
	    xtc_proc_spawn(xtc_exec_loop(A, 1), peer, (void *)1, NULL,
	    &pid[1]) != XTC_OK ||
	    xtc_proc_spawn(xtc_exec_loop(B, 0), peer, (void *)2, NULL,
	    &pid[2]) != XTC_OK ||
	    xtc_proc_spawn(xtc_exec_loop(B, 1), peer, (void *)3, NULL,
	    &pid[3]) != XTC_OK ||
	    xtc_proc_spawn(S0, peer, (void *)4, NULL, &pid[4]) != XTC_OK ||
	    xtc_proc_spawn(S1, peer, (void *)5, NULL, &pid[5]) != XTC_OK) {
		printf("FAIL: spawn\n");
		return 1;
	}
	for (i = 0; i < NP; i++)
		printf("  %s pid %u.%u.%llu\n", g_name[i],
		    (unsigned)pid[i].loop_id, (unsigned)pid[i].local_id,
		    (unsigned long long)pid[i].gen);
	(void)pthread_create(&th[0], NULL, run_exec, A);
	(void)pthread_create(&th[1], NULL, run_exec, B);
	(void)pthread_create(&th[2], NULL, run_loop, S0);
	(void)pthread_create(&th[3], NULL, run_loop, S1);
	for (i = 0; i < 4; i++)
		(void)pthread_join(th[i], NULL);

	for (i = 0; i < NP; i++) {
		total += atomic_load(&g_got[i]);
		wrong += atomic_load(&g_wrong[i]);
	}
	printf("phase 1: delivered to the addressee %d/%d, to another proc "
	    "%d, send failures %d\n", total, NP * (NP - 1), wrong,
	    atomic_load(&g_send_fail));
	if (wrong != 0 || total != NP * (NP - 1) ||
	    atomic_load(&g_send_fail) != 0) {
		printf("FAIL: a pid reached a proc other than its own\n");
		fail = 1;
	}
	for (i = 0; i < NP; i++)
		for (j = i + 1; j < NP; j++)
			if (pid[i].loop_id == pid[j].loop_id) {
				printf("FAIL: %s and %s share loop_id %u\n",
				    g_name[i], g_name[j],
				    (unsigned)pid[i].loop_id);
				fail = 1;
			}
	memset(&ids, 0, sizeof ids);
	(void)xtc_inspect_loops(collect, &ids);
	for (i = 0; i < ids.n; i++) {
		if (ids.id[i] == 0) {
			printf("FAIL: xtc_inspect_loops reports loop_id 0\n");
			fail = 1;
		}
		for (j = i + 1; j < ids.n; j++)
			if (ids.id[i] == ids.id[j]) {
				printf("FAIL: xtc_inspect_loops reports "
				    "loop_id %u twice\n", (unsigned)ids.id[i]);
				fail = 1;
			}
	}

	/* Phase 2: pids of finalized loops. */
	g_dead[0] = pid[0];
	g_dead[1] = pid[5];
	(void)xtc_exec_fini(A);
	(void)xtc_loop_fini(S1);
	if (xtc_exec_init(&C, 2) != XTC_OK || xtc_loop_init(&S2) != XTC_OK) {
		printf("FAIL: setup 2\n");
		return 1;
	}
	if (xtc_proc_spawn(xtc_exec_loop(C, 0), decoy, NULL, NULL, &p) !=
	    XTC_OK ||
	    xtc_proc_spawn(xtc_exec_loop(C, 1), dead_sender, NULL, NULL, NULL)
	    != XTC_OK ||
	    xtc_proc_spawn(S2, decoy, NULL, NULL, &p) != XTC_OK ||
	    xtc_proc_spawn(S2, dead_sender, NULL, NULL, NULL) != XTC_OK) {
		printf("FAIL: spawn 2\n");
		return 1;
	}
	for (i = 0; i < 2; i++)
		if (xtc_send(g_dead[i], "x", 1) == XTC_OK)
			atomic_fetch_add(&g_dead_ok, 1);
	(void)pthread_create(&th[0], NULL, run_exec, C);
	(void)pthread_create(&th[1], NULL, run_loop, S2);
	(void)pthread_join(th[0], NULL);
	(void)pthread_join(th[1], NULL);
	printf("phase 2: sends to dead pids accepted %d/6, decoys reached %d\n",
	    atomic_load(&g_dead_ok), atomic_load(&g_decoy_got));
	if (atomic_load(&g_dead_ok) != 0 || atomic_load(&g_decoy_got) != 0) {
		printf("FAIL: a pid of a finalized loop reached a new loop\n");
		fail = 1;
	}

	(void)xtc_exec_fini(B);
	(void)xtc_exec_fini(C);
	(void)xtc_loop_fini(S0);
	(void)xtc_loop_fini(S2);
	if (fail)
		return 1;
	printf("OK: pids cross executors and standalone loops exactly; a "
	    "dead loop's pid reaches nothing\n");
	return 0;
}

#endif /* !_WIN32 */
