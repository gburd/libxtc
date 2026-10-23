/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_wal_crc_once.c
 *	The one-time CRC32C table init cannot wedge a loop under involuntary
 *	preemption.
 *
 *	Two fibers on one loop checksum a frame; the first builds the table.
 *	The init is stretched to 20 ms of CPU (WAL_C32C_INIT_HOOK) so the
 *	50 us preemption timer fires inside it.  Before the fix the tick
 *	redirected the first fiber to the scheduler, the second fiber then
 *	blocked the loop's only OS thread in pthread_once, and the first could
 *	never run again: the loop hung (a watchdog aborts after 10 s).  After
 *	it the tick defers until the init is done, and both checksums finish.
 *
 *	Skips where preemption timers are not available.  Where involuntary
 *	preemption is not supported (it is Linux only) the ticks are deferred
 *	and the test passes without exercising the window; it then says so.
 */

#include <stdatomic.h>
#include <stdint.h>

#include "xtc.h"
#include "xtc_preempt.h"   /* xtc_preempt_ticks, used inside the hook */

static volatile unsigned long g_spin;
static atomic_int g_in_init;
static uint64_t   g_ticks_in_init;

/* 20 ms of CPU inside the init, long enough for many 50 us ticks. */
#define WAL_C32C_INIT_HOOK() do {                                      \
	int64_t end_ = xtc_clock_mono() + 20000000;                    \
	uint64_t t0_ = xtc_preempt_ticks();                            \
	atomic_store(&g_in_init, 1);                                   \
	while (xtc_clock_mono() < end_)                                \
		g_spin++;                                              \
	g_ticks_in_init = xtc_preempt_ticks() - t0_;                   \
} while (0)

#include "wal.c"

#include "xtc_exec.h"

static atomic_int g_done;
static uint32_t   g_sum[2];

static void
crc_fiber(void *arg)
{
	static const uint8_t buf[64] = { 1, 2, 3 };
	int i = (int)(intptr_t)arg;

	g_sum[i] = wal_c32c(~0u, buf, sizeof buf);
	atomic_fetch_add(&g_done, 1);
}

static void *
watchdog(void *arg)
{
	int i;
	(void)arg;
	for (i = 0; i < 1000 && atomic_load(&g_done) < 2; i++)
		(void)xtc_sleep_ns(10 * 1000 * 1000);
	if (atomic_load(&g_done) < 2) {
		fprintf(stderr, "FAIL: the loop wedged in the CRC32C init "
		    "(%d of 2 checksums done)\n", atomic_load(&g_done));
		abort();
	}
	return NULL;
}

int
main(void)
{
	xtc_exec_t *e = NULL;
	pthread_t wd;

	if (!xtc_preempt_supported()) {
		printf("SKIP: no preemption timer on this host\n");
		return 0;
	}
	if (xtc_exec_init(&e, 1) != XTC_OK ||
	    xtc_exec_set_preempt(e, 50 * 1000) != XTC_OK) {
		fprintf(stderr, "FAIL: executor setup\n");
		return 1;
	}
	xtc_preempt_set_involuntary(1);
	if (xtc_proc_spawn(xtc_exec_loop(e, 0), crc_fiber, (void *)0, NULL,
	    NULL) != XTC_OK ||
	    xtc_proc_spawn(xtc_exec_loop(e, 0), crc_fiber, (void *)1, NULL,
	    NULL) != XTC_OK ||
	    pthread_create(&wd, NULL, watchdog, NULL) != 0) {
		fprintf(stderr, "FAIL: spawn\n");
		return 1;
	}
	(void)xtc_exec_run(e);
	(void)pthread_join(wd, NULL);
	xtc_preempt_set_involuntary(0);
	(void)xtc_exec_fini(e);

	if (!atomic_load(&g_in_init) || g_sum[0] != g_sum[1]) {
		fprintf(stderr, "FAIL: init ran %d, sums %08x %08x\n",
		    atomic_load(&g_in_init), g_sum[0], g_sum[1]);
		return 1;
	}
	printf("ok   the CRC32C init defers preemption: 2 fibers, 1 loop, "
	    "no wedge (%llu ticks inside the init)\n",
	    (unsigned long long)g_ticks_in_init);
	return 0;
}
