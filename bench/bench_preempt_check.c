/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * bench/bench_preempt_check.c
 *	L2 microbench: per-yield-check cost of the io_uring ring-pointer
 *	preempt source vs the signal-flag source.
 *
 *	The ring source (INSPIRED BY Glommio's need_preempt) is
 *	__xtc_io_uring_preempt_due(io): two relaxed/acquire loads of a
 *	dedicated ring's CQ head/tail on the not-due fast path.  The
 *	signal source is xtc_preempt_tick_pending(): one atomic exchange
 *	on the thread-local flag.  We time the not-due fast path of each
 *	(the one hit on every xtc_yield_check that does NOT slice), since
 *	that is the steady-state cost paid per checkpoint.
 *
 *	Prints ns/check for each.  Purely a cost report; no pass/fail.
 *	io_uring backend only for the ring half (skips its ring timing
 *	elsewhere and reports the signal half alone).
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "xtc.h"
#include "xtc_io.h"
#include "xtc_preempt.h"

/* Internal seam (declared here to keep the bench a plain consumer TU;
 * resolved from libxtc.a).  Real on uring, no-op stub elsewhere. */
extern int  __xtc_io_uring_preempt_arm(xtc_io_t *io, int64_t interval_ns);
extern int  __xtc_io_uring_preempt_due(xtc_io_t *io);
extern void __xtc_io_uring_preempt_disarm(xtc_io_t *io);

#define ITERS (50 * 1000 * 1000)

static volatile int g_sink;

int
main(void)
{
	xtc_io_t *io = NULL;
	int64_t t0, t1;
	long i;
	int is_uring = (strcmp(xtc_io_backend_name(), "uring") == 0);

	printf("bench_preempt_check: backend=%s iters=%d\n",
	    xtc_io_backend_name(), ITERS);

	/* ---- signal-flag source: xtc_preempt_tick_pending() ---- *
	 * Not armed -> the flag is never set -> each call is the not-due
	 * fast path: one atomic exchange returning 0. */
	t0 = xtc_clock_mono();
	for (i = 0; i < ITERS; i++)
		g_sink += xtc_preempt_tick_pending();
	t1 = xtc_clock_mono();
	printf("  signal-flag (tick_pending)   : %.3f ns/check\n",
	    (double)(t1 - t0) / ITERS);

	if (!is_uring) {
		printf("  ring-pointer                 : n/a (non-uring)\n");
		return 0;
	}

	/* ---- ring-pointer source: __xtc_io_uring_preempt_due() ---- *
	 * Arm with a very long interval so the timeout never fires during
	 * the run: every call is the not-due fast path (two loads of the
	 * ring head/tail via io_uring_cq_ready). */
	if (xtc_io_init(&io) != XTC_OK) {
		fprintf(stderr, "xtc_io_init failed\n");
		return 1;
	}
	if (__xtc_io_uring_preempt_arm(io, 3600LL * 1000000000LL) != XTC_OK) {
		fprintf(stderr, "preempt_arm failed\n");
		(void)xtc_io_fini(io);
		return 1;
	}
	t0 = xtc_clock_mono();
	for (i = 0; i < ITERS; i++)
		g_sink += __xtc_io_uring_preempt_due(io);
	t1 = xtc_clock_mono();
	printf("  ring-pointer (preempt_due)   : %.3f ns/check\n",
	    (double)(t1 - t0) / ITERS);

	__xtc_io_uring_preempt_disarm(io);
	(void)xtc_io_fini(io);
	return 0;
}
