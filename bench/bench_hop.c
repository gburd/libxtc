/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * bench/bench_hop.c
 *	Cross-loop request/reply throughput: a client fiber on loop A asks
 *	the owner proc on loop B to do a tiny operation and waits for the
 *	answer (xtc_send + xtc_recv each way) -- the "hop" a shard-per-loop
 *	server makes for every key it does not own.  SAME=1 keeps the owner
 *	on the client's loop (the same-loop control).
 *
 *	usage: bench_hop LOOPS CLIENTS_PER_LOOP ITERS SAME
 *	prints: loops=L clients=C same=S hops=N ns_per_hop_wall=X hops_per_s=Y
 *
 *	One of the cells guarded by scripts/perf-gate.sh.
 */

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_loop.h"
#include "xtc_proc.h"

#define MAX_LOOPS 64

typedef struct req {
	xtc_pid_t	from;
	uint64_t	key;
} req_t;

static int g_loops, g_cpl, g_iters, g_same;
static xtc_pid_t g_owner[MAX_LOOPS];
static uint64_t g_counter[MAX_LOOPS];
static _Atomic int g_ready, g_done;
static _Atomic uint64_t g_total;

static void
owner_proc(void *arg)
{
	int id = (int)(intptr_t)arg;
	uint64_t v;
	req_t *r;
	size_t sz;
	void *m;

	for (;;) {
		if (xtc_recv(&m, &sz, -1) != XTC_OK)
			continue;
		if (sz != sizeof(req_t)) {      /* the stop message */
			xtc_free(m);
			return;
		}
		r = m;
		v = ++g_counter[id] + r->key;
		(void)xtc_send(r->from, &v, sizeof v);
		xtc_free(m);
	}
}

static void
client_proc(void *arg)
{
	int me = (int)(intptr_t)arg, i, tgt;
	size_t sz;
	void *m;
	req_t r;

	r.from = xtc_self();
	while (atomic_load(&g_ready) == 0)
		(void)xtc_proc_sleep(1000000);
	tgt = g_same ? me % g_loops : (me + 1) % g_loops;
	for (i = 0; i < g_iters; i++) {
		r.key = (uint64_t)i;
		(void)xtc_send(g_owner[tgt], &r, sizeof r);
		while (xtc_recv(&m, &sz, -1) != XTC_OK)
			;
		xtc_free(m);
	}
	atomic_fetch_add(&g_total, (uint64_t)g_iters);
	if (atomic_fetch_add(&g_done, 1) + 1 == g_loops * g_cpl)
		for (i = 0; i < g_loops; i++)
			(void)xtc_send(g_owner[i], "x", 1);
}

int
main(int argc, char **argv)
{
	xtc_exec_t *e = NULL;
	int64_t t0, t1;
	double per;
	int i, j;

	if (argc < 5) {
		fprintf(stderr, "usage: bench_hop LOOPS CLIENTS_PER_LOOP "
		    "ITERS SAME\n");
		return 2;
	}
	g_loops = atoi(argv[1]);
	g_cpl = atoi(argv[2]);
	g_iters = atoi(argv[3]);
	g_same = atoi(argv[4]);
	if (g_loops < 1 || g_loops > MAX_LOOPS || g_cpl < 1 || g_iters < 1)
		return 2;
	if (xtc_exec_init(&e, g_loops) != XTC_OK)
		return 1;
	for (i = 0; i < g_loops; i++)
		if (xtc_proc_spawn(xtc_exec_loop(e, i), owner_proc,
		    (void *)(intptr_t)i, NULL, &g_owner[i]) != XTC_OK)
			return 1;
	for (i = 0; i < g_loops; i++)
		for (j = 0; j < g_cpl; j++)
			if (xtc_proc_spawn(xtc_exec_loop(e, i), client_proc,
			    (void *)(intptr_t)(i + j * g_loops), NULL,
			    NULL) != XTC_OK)
				return 1;
	t0 = xtc_clock_mono();
	atomic_store(&g_ready, 1);
	(void)xtc_exec_run(e);
	t1 = xtc_clock_mono();
	per = (double)atomic_load(&g_total) / (double)(g_loops * g_cpl);
	printf("loops=%d clients=%d same=%d hops=%llu ns_per_hop_wall=%.0f "
	    "hops_per_s=%.0f\n", g_loops, g_loops * g_cpl, g_same,
	    (unsigned long long)atomic_load(&g_total),
	    (double)(t1 - t0) / per,
	    (double)atomic_load(&g_total) * 1e9 / (double)(t1 - t0));
	(void)xtc_exec_fini(e);
	return 0;
}
