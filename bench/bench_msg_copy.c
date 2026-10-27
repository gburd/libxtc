/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * bench/bench_msg_copy.c -- proc message throughput by payload size and
 * send mode: how much the mailbox copies cost.
 *
 *   bench_msg_copy MODE SIZE NMSG LOOPS [STRIDE [WORK_NS [SPIN]]]
 *     MODE   copy   xtc_send from one reused source buffer
 *            owned  xtc_send_owned of a fresh xtc_malloc'd buffer per
 *                   message
 *            In both modes the producer writes the whole payload per
 *            message (it produces new data), so the modes differ only
 *            in what the mailbox does with it.
 *     SIZE   payload bytes
 *     NMSG   messages to stream
 *     LOOPS  1 = producer and consumer on one loop, 2 = two loops (two
 *            OS threads, cross-thread delivery)
 *
 * The consumer reads one byte every STRIDE bytes of each payload
 * (default 4096; 64 reads every cache line, as a consumer that parses
 * the message would), then frees it.  WORK_NS (default 0) adds that
 * much busy work per message in the consumer, so a run can be made
 * consumer-bound (a real consumer does more than free the buffer).
 * SPIN = 1 makes the consumer poll (xtc_recv timeout 0 + xtc_yield)
 * instead of parking, which separates the copy cost from the cost of a
 * cross-thread park/wake when the consumer outruns the producer.
 * Prints one line:
 *   mode size loops nmsg secs msgs_per_s MiB_per_s
 * Build against a library without xtc_send_owned with -DBENCH_NO_OWNED.
 */

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int        g_owned;
static size_t     g_size;
static long       g_nmsg;
static xtc_atomic_pid_t g_rx;
static _Atomic int g_ready;
static int64_t    g_t0, g_t1;
static uint64_t   g_sum;
static size_t     g_stride = 4096;
static int64_t    g_work_ns;
static int        g_spin;

static void
rx_proc(void *arg)
{
	long i;
	uint64_t sum = 0;
	(void)arg;
	xtc_atomic_pid_store(&g_rx, xtc_self());
	atomic_store(&g_ready, 1);
	for (i = 0; i < g_nmsg; i++) {
		void *m = NULL;
		size_t n = 0, k;
		if (g_spin) {
			while (xtc_recv(&m, &n, 0) != XTC_OK)
				(void)xtc_yield();
		} else if (xtc_recv(&m, &n, 10LL * 1000 * 1000 * 1000) !=
		    XTC_OK) {
			fprintf(stderr, "recv timeout at %ld\n", i);
			exit(1);
		}
		for (k = 0; k < n; k += g_stride)
			sum += ((const uint8_t *)m)[k];
		xtc_free(m);
		if (g_work_ns > 0) {
			int64_t until = xtc_clock_mono() + g_work_ns;
			while (xtc_clock_mono() < until)
				;
		}
	}
	g_t1 = xtc_clock_mono();
	g_sum = sum;
}

static void
tx_proc(void *arg)
{
	long i;
	uint8_t *src = NULL;
	xtc_pid_t to;
	(void)arg;
	while (!atomic_load(&g_ready))
		(void)xtc_yield();
	to = xtc_atomic_pid_load(&g_rx);
	if (!g_owned) {
		if ((src = xtc_malloc(g_size)) == NULL)
			exit(1);
		memset(src, 0x5a, g_size);
	}
	g_t0 = xtc_clock_mono();
	for (i = 0; i < g_nmsg; i++) {
		int rc;
#ifndef BENCH_NO_OWNED
		if (g_owned) {
			uint8_t *b = xtc_malloc(g_size);
			if (b == NULL)
				exit(1);
			memset(b, (int)(i & 0xff), g_size);
			while ((rc = xtc_send_owned(to, b, g_size)) ==
			    XTC_E_AGAIN)
				(void)xtc_yield();
			if (rc != XTC_OK)
				exit(1);
			continue;
		}
#endif
		memset(src, (int)(i & 0xff), g_size);
		while ((rc = xtc_send(to, src, g_size)) == XTC_E_AGAIN)
			(void)xtc_yield();
		if (rc != XTC_OK)
			exit(1);
	}
	xtc_free(src);
}

int
main(int argc, char **argv)
{
	xtc_proc_opts_t o = XTC_PROC_OPTS_INIT;
	int loops;
	double secs;

	if (argc < 5 || argc > 8) {
		fprintf(stderr, "usage: %s copy|owned SIZE NMSG LOOPS "
		    "[STRIDE [WORK_NS [SPIN]]]\n",
		    argv[0]);
		return 2;
	}
	g_owned = strcmp(argv[1], "owned") == 0;
#ifdef BENCH_NO_OWNED
	if (g_owned) {
		fprintf(stderr, "owned mode not built\n");
		return 2;
	}
#endif
	g_size = (size_t)strtoull(argv[2], NULL, 10);
	g_nmsg = strtol(argv[3], NULL, 10);
	loops = atoi(argv[4]);
	if (argc == 6 && strtoull(argv[5], NULL, 10) > 0)
		g_stride = (size_t)strtoull(argv[5], NULL, 10);
	if (argc >= 7)
		g_work_ns = strtoll(argv[6], NULL, 10);
	if (argc == 8)
		g_spin = atoi(argv[7]);
	/* Bound the in-flight bytes: 64 queued messages. */
	o.mailbox_cap = 64;
	if (loops <= 1) {
		xtc_loop_t *loop;
		if (xtc_loop_init(&loop) != XTC_OK ||
		    xtc_proc_spawn(loop, rx_proc, NULL, &o, NULL) != XTC_OK ||
		    xtc_proc_spawn(loop, tx_proc, NULL, NULL, NULL) != XTC_OK)
			return 1;
		(void)xtc_loop_run(loop);
		(void)xtc_loop_fini(loop);
	} else {
		xtc_exec_t *e;
		if (xtc_exec_init(&e, 2) != XTC_OK ||
		    xtc_proc_spawn(xtc_exec_loop(e, 0), rx_proc, NULL, &o,
		    NULL) != XTC_OK ||
		    xtc_proc_spawn(xtc_exec_loop(e, 1), tx_proc, NULL, NULL,
		    NULL) != XTC_OK)
			return 1;
		(void)xtc_exec_run(e);
		(void)xtc_exec_fini(e);
	}
	secs = (double)(g_t1 - g_t0) / 1e9;
	printf("%s %zu %d %ld %.4f %.0f %.1f %llu\n", argv[1], g_size, loops,
	    g_nmsg, secs, (double)g_nmsg / secs,
	    (double)g_nmsg * (double)g_size / secs / (1024.0 * 1024.0),
	    (unsigned long long)(g_sum & 1));
	return 0;
}
