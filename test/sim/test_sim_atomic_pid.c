/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * test/sim/test_sim_atomic_pid.c
 *	DST coverage of xtc_atomic_pid_t, the 2.0 publication cell for a
 *	16-byte pid (xtc_app children, the supervisor and gen_server pids,
 *	rexis's per-fd wake table).
 *
 *	PUB_LOOPS loops each run PUBS publisher procs and READS reader
 *	procs.  A publisher stores ITS OWN pid into one of CELLS shared
 *	cells, yields, and repeats; a reader loads a cell and, if it is not
 *	NONE, checks the pid is one some publisher really owns (a torn or
 *	invented pid fails), then messages it.  Every message must land
 *	(publishers stay alive until all readers are done), and every
 *	publisher counts what it received.  INVARIANTS: no bad pid ever
 *	loaded; sent == received; quiescence; byte-identical replay of the
 *	counts, an order-sensitive fold of the loaded pids, and the sim
 *	state hash; a second seed is consistent too.
 *
 *	Under the sim a seqlock reader can only see an odd sequence if a
 *	writer yielded mid-store -- the store runs no yield point and sits
 *	inside the preemption bracket, so the replay hash proves the reader
 *	loop never spun on anything the scheduler did not order.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "xtc.h"
#include "xtc_async.h"
#include "xtc_exec.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_sim.h"

#define PUB_LOOPS 4
#define PUBS      3                 /* per loop */
#define READS     2                 /* per loop */
#define CELLS     5
#define ROUNDS    40
#define NPUB      (PUB_LOOPS * PUBS)
#define NREAD     (PUB_LOOPS * READS)

static xtc_atomic_pid_t g_cell[CELLS];
static xtc_pid_t   g_pub[NPUB];     /* every publisher's pid, set at start */
static atomic_int  g_pub_up, g_readers_done;
static atomic_int  g_bad, g_sent, g_recv;
static atomic_long g_fold;

static void
fold(xtc_pid_t p)
{
	long h = atomic_load_explicit(&g_fold, memory_order_relaxed);
	h = (long)((unsigned long)h * 1000003UL + p.loop_id * 131UL +
	    p.local_id * 7UL + (unsigned long)p.gen);
	atomic_store_explicit(&g_fold, h, memory_order_relaxed);
}

static void
publisher(void *arg)
{
	int id = (int)(intptr_t)arg, r;
	xtc_pid_t self = xtc_self();
	void *m;
	size_t sz;

	g_pub[id] = self;
	atomic_fetch_add(&g_pub_up, 1);
	for (r = 0; r < ROUNDS; r++) {
		xtc_atomic_pid_store(&g_cell[(id + r) % CELLS], self);
		xtc_yield();
		while (xtc_recv(&m, &sz, 0) == XTC_OK) {
			xtc_free(m);
			atomic_fetch_add(&g_recv, 1);
		}
	}
	/* Stay reachable until every reader is done, then drain. */
	while (atomic_load(&g_readers_done) < NREAD) {
		while (xtc_recv(&m, &sz, 0) == XTC_OK) {
			xtc_free(m);
			atomic_fetch_add(&g_recv, 1);
		}
		xtc_yield();
	}
	while (xtc_recv(&m, &sz, 0) == XTC_OK) {
		xtc_free(m);
		atomic_fetch_add(&g_recv, 1);
	}
}

static int
known(xtc_pid_t p)
{
	int i;

	for (i = 0; i < NPUB; i++)
		if (xtc_pid_eq(p, g_pub[i]))
			return 1;
	return 0;
}

static void
reader(void *arg)
{
	int id = (int)(intptr_t)arg, r;
	xtc_pid_t p;
	char c = 'x';

	while (atomic_load(&g_pub_up) < NPUB)
		xtc_yield();
	for (r = 0; r < ROUNDS; r++) {
		p = xtc_atomic_pid_load(&g_cell[(id * 3 + r) % CELLS]);
		if (!xtc_pid_is_none(p)) {
			if (!known(p))
				atomic_fetch_add(&g_bad, 1);
			else if (xtc_send(p, &c, 1) == XTC_OK)
				atomic_fetch_add(&g_sent, 1);
			else
				atomic_fetch_add(&g_bad, 1);  /* publishers live */
			fold(p);
		}
		xtc_yield();
	}
	atomic_fetch_add(&g_readers_done, 1);
}

static int
run(uint64_t seed, int *bad, int *sent, int *recv, long *fh, uint64_t *st)
{
	xtc_exec_t *e = NULL;
	int i, rc;

	memset(g_cell, 0, sizeof g_cell);
	memset(g_pub, 0, sizeof g_pub);
	atomic_store(&g_pub_up, 0);
	atomic_store(&g_readers_done, 0);
	atomic_store(&g_bad, 0);
	atomic_store(&g_sent, 0);
	atomic_store(&g_recv, 0);
	atomic_store(&g_fold, 0);
	if (xtc_exec_init(&e, PUB_LOOPS) != XTC_OK)
		return XTC_E_NOMEM;
	for (i = 0; i < NPUB; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e, i % PUB_LOOPS),
		    publisher, (void *)(intptr_t)i, NULL, NULL);
	for (i = 0; i < NREAD; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e, i % PUB_LOOPS),
		    reader, (void *)(intptr_t)i, NULL, NULL);
	rc = xtc_sim_exec_run(e, seed, 5000000);
	*bad = atomic_load(&g_bad);
	*sent = atomic_load(&g_sent);
	*recv = atomic_load(&g_recv);
	*fh = atomic_load(&g_fold);
	*st = xtc_sim_state_hash(e);
	(void)xtc_exec_fini(e);
	return rc;
}

int
main(void)
{
	int b1, s1, r1, b2, s2, r2, b3, s3, r3, rc;
	long f1, f2, f3;
	uint64_t h1, h2, h3;

	if ((rc = run(0xA70D1, &b1, &s1, &r1, &f1, &h1)) != XTC_OK) {
		printf("FAIL: atomic_pid run rc=%d (hang?)\n", rc);
		return 1;
	}
	(void)run(0xA70D1, &b2, &s2, &r2, &f2, &h2);
	if ((rc = run(0x5EED2, &b3, &s3, &r3, &f3, &h3)) != XTC_OK) {
		printf("FAIL: atomic_pid diff-seed rc=%d\n", rc);
		return 1;
	}
	printf("run1: bad=%d sent=%d recv=%d fold=%ld state=%016llx\n",
	    b1, s1, r1, f1, (unsigned long long)h1);
	if (b1 != 0 || b3 != 0) {
		printf("FAIL: a reader loaded a pid no publisher owns, or a "
		    "send to a live publisher failed (%d, %d)\n", b1, b3);
		return 1;
	}
	if (s1 == 0 || s1 != r1 || s3 != r3) {
		printf("FAIL: sent %d recv %d (seed 2: %d / %d)\n", s1, r1, s3,
		    r3);
		return 1;
	}
	if (s1 != s2 || r1 != r2 || f1 != f2 || h1 != h2) {
		printf("FAIL: did not replay (fold %ld/%ld state %016llx/"
		    "%016llx)\n", f1, f2, (unsigned long long)h1,
		    (unsigned long long)h2);
		return 1;
	}
	printf("OK: xtc_atomic_pid_t publication: no torn or foreign pid, "
	    "%d sends delivered, replay byte-identical\n", s1);
	return 0;
}
