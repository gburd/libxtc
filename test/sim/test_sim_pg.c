/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/sim/test_sim_pg.c
 *	DST coverage of process-group fan-out (src/orc/pg.c): xtc_pg_send
 *	broadcasts to every current member under the seeded scheduler.  N
 *	subscriber procs across M loops join a group; a publisher fans a
 *	message out; every subscriber must receive exactly the broadcast,
 *	the reported send count must equal the membership, and the run must
 *	replay byte-identically.  (The registry dup-key mechanics are also
 *	DST-covered in test_sim_reg; this pins the send/deliver fan-out.)
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <stdint.h>
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_reg.h"
#include "xtc_pg.h"
#include "xtc_sim.h"

#define N_LOOPS 4
#define N_SUBS  8
#define PAYLOAD 0xC0DE

static xtc_reg_t  *g_reg;
static atomic_int   g_received;    /* subscribers that got the broadcast */
static atomic_int   g_bad;         /* wrong payload (bug) */
static atomic_int   g_ready;       /* subscribers that have joined */
static atomic_int   g_sent_count;  /* what xtc_pg_send reported */

/* Subscriber: join the group, then wait for the broadcast. */
static void
subscriber(void *arg)
{
	(void)arg;
	void *m = NULL; size_t n = 0;
	(void)xtc_pg_join(g_reg, "topic", xtc_self());
	atomic_fetch_add_explicit(&g_ready, 1, memory_order_relaxed);
	if (xtc_recv(&m, &n, 2000LL * 1000 * 1000) == XTC_OK) {
		if (n == sizeof(int) && *(int *)m == PAYLOAD)
			atomic_fetch_add_explicit(&g_received, 1,
			    memory_order_relaxed);
		else
			atomic_fetch_add_explicit(&g_bad, 1,
			    memory_order_relaxed);
	}
	if (m) xtc_free(m);
}

/* Publisher: wait until all subscribers have joined, then broadcast. */
static void
publisher(void *arg)
{
	(void)arg;
	int payload = PAYLOAD;
	int n;
	while (atomic_load_explicit(&g_ready, memory_order_relaxed) < N_SUBS)
		xtc_yield();
	n = xtc_pg_send(g_reg, "topic", &payload, sizeof payload);
	atomic_store_explicit(&g_sent_count, n, memory_order_relaxed);
}

static int
run_pg(uint64_t seed, int *out_recv, int *out_bad, int *out_sent,
       uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	int i, rc;

	atomic_store(&g_received, 0);
	atomic_store(&g_bad, 0);
	atomic_store(&g_ready, 0);
	atomic_store(&g_sent_count, 0);

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK) return -1;
	if (xtc_reg_create(&g_reg) != XTC_OK) {
		(void)xtc_exec_fini(e); return -1;
	}
	for (i = 0; i < N_SUBS; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e, (unsigned)((i % (N_LOOPS - 1)) + 1)),
		    subscriber, NULL, NULL, NULL);
	(void)xtc_proc_spawn(xtc_exec_loop(e, 0), publisher, NULL, NULL, NULL);

	rc = xtc_sim_exec_run(e, seed, 5000000);

	*out_recv = atomic_load(&g_received);
	*out_bad = atomic_load(&g_bad);
	*out_sent = atomic_load(&g_sent_count);
	if (out_state) *out_state = xtc_sim_state_hash(e);
	xtc_reg_destroy(g_reg);
	g_reg = NULL;
	(void)xtc_exec_fini(e);
	return rc;
}

int
main(void)
{
	int r1 = 0, b1 = 0, s1 = 0, r2 = 0, b2 = 0, s2 = 0, r3 = 0, b3 = 0, s3 = 0;
	uint64_t st1 = 0, st2 = 0, st3 = 0;
	int rc1, rc2, rc3;

	rc1 = run_pg(0x9C1DE, &r1, &b1, &s1, &st1);
	if (rc1 != XTC_OK) { printf("FAIL: pg run rc=%d (hang?)\n", rc1); return 1; }
	rc2 = run_pg(0x9C1DE, &r2, &b2, &s2, &st2);
	rc3 = run_pg(0x5B0AD, &r3, &b3, &s3, &st3);
	if (rc2 != XTC_OK || rc3 != XTC_OK) {
		printf("FAIL: pg replay/diff rc=%d/%d\n", rc2, rc3); return 1;
	}

	printf("run1: received=%d bad=%d sent=%d state=%016llx\n",
	    r1, b1, s1, (unsigned long long)st1);
	printf("run3 (diff seed): received=%d bad=%d sent=%d\n", r3, b3, s3);

	/* Every subscriber received exactly the broadcast, on both seeds. */
	if (r1 != N_SUBS || r3 != N_SUBS) {
		printf("FAIL: received=%d/%d want %d (lost broadcast)\n",
		    r1, r3, N_SUBS); return 1;
	}
	if (b1 != 0 || b3 != 0) {
		printf("FAIL: wrong payload delivered (bad=%d/%d)\n", b1, b3);
		return 1;
	}
	/* xtc_pg_send reported the full membership. */
	if (s1 != N_SUBS) {
		printf("FAIL: pg_send reported %d want %d\n", s1, N_SUBS);
		return 1;
	}
	/* Byte-identical replay. */
	if (r1 != r2 || b1 != b2 || s1 != s2 || st1 != st2) {
		printf("FAIL: pg run did not replay byte-identically\n");
		return 1;
	}

	printf("OK: pg_send fans a broadcast to every group member under the "
	    "seeded scheduler (all %d receive it, none lost or corrupted, "
	    "send count == membership), and the run replays byte-identically\n",
	    N_SUBS);
	return 0;
}
