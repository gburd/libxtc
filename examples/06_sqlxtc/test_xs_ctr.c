/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_xs_ctr.c
 *	xs_ctr.h, the striped statistics counter.
 *	C1  exact: N threads x M adds sum to N * M (and a zeroed counter
 *	    sums to 0; xs_ctr_init zeroes).
 *	C2  layout: every slot's value is on its own 64-byte line, and the
 *	    first XS_CTR_SLOTS threads get distinct slots.
 *	C3  cost (reported, not asserted): ns per add with 8 threads on a
 *	    plain shared atomic and on the striped counter.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xs_ctr.h"
#include "xtc.h"
#include "t_tmp.h"

#define N_THR  8
#define M_ADDS 2000000

static xs_ctr_t        g_ctr;
static _Atomic uint64_t g_plain;
static unsigned        g_slot[XS_CTR_SLOTS];
static int             g_mode;      /* 0 striped, 1 plain */

static void *
adder(void *arg)
{
	long i;

	g_slot[(long)arg] = xs_ctr_slot();
	for (i = 0; i < M_ADDS; i++) {
		if (g_mode == 0)
			xs_ctr_add(&g_ctr, 1);
		else
			atomic_fetch_add_explicit(&g_plain, 1, memory_order_relaxed);
	}
	return NULL;
}

static int64_t
run(int mode, int nthr)
{
	pthread_t t[XS_CTR_SLOTS];
	int64_t t0;
	long i;

	g_mode = mode;
	t0 = xtc_clock_mono();
	for (i = 0; i < nthr; i++)
		if (pthread_create(&t[i], NULL, adder, (void *)i) != 0)
			return -1;
	for (i = 0; i < nthr; i++)
		pthread_join(t[i], NULL);
	return xtc_clock_mono() - t0;
}

int
main(void)
{
	xs_ctr_t *h;
	int64_t ns_s, ns_p;
	int i, j, distinct = 1;

	/* C1 */
	CK(xs_ctr_sum(&g_ctr) == 0);
	ns_s = run(0, N_THR);
	CK(ns_s > 0);
	CK(xs_ctr_sum(&g_ctr) == (uint64_t)N_THR * M_ADDS);
	xs_ctr_add(&g_ctr, 5);
	CK(xs_ctr_sum(&g_ctr) == (uint64_t)N_THR * M_ADDS + 5);
	h = calloc(1, sizeof *h);
	CK(h != NULL);
	if (h != NULL) {
		xs_ctr_add(h, 3);
		CK(xs_ctr_sum(h) == 3);
		xs_ctr_init(h);
		CK(xs_ctr_sum(h) == 0);
	}

	/* C2: value i at a fixed 64-byte stride past a 64-byte lead. */
	for (i = 0; i < XS_CTR_SLOTS; i++)
		CK((uintptr_t)&g_ctr.slot[i].v - (uintptr_t)&g_ctr ==
		    (uintptr_t)XS_CTR_LINE * (uintptr_t)(i + 1));
	CK(sizeof(xs_ctr_t) == (size_t)XS_CTR_LINE * (XS_CTR_SLOTS + 1));
	for (i = 0; i < N_THR; i++)
		for (j = i + 1; j < N_THR; j++)
			if (g_slot[i] == g_slot[j]) distinct = 0;
	CK(distinct);

	/* C3: the same adds on one shared atomic. */
	ns_p = run(1, N_THR);
	CK(atomic_load(&g_plain) == (uint64_t)N_THR * M_ADDS);
	printf("  %s   C1 %d threads x %d adds: sum exact; C2 slots on their "
	    "own lines, %d threads in distinct slots\n", g_fail ? "FAIL" : "ok",
	    N_THR, M_ADDS, N_THR);
	printf("  info C3 %d threads: shared atomic %.2f ns/add, striped %.2f "
	    "ns/add\n", N_THR, (double)ns_p / ((double)N_THR * M_ADDS),
	    (double)ns_s / ((double)N_THR * M_ADDS));
	free(h);
	if (g_fail) {
		fprintf(stderr, "test_xs_ctr: FAILED\n");
		return 1;
	}
	printf("test_xs_ctr: ALL PASS\n");
	return 0;
}
