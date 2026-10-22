/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * examples/15_usdt/convoy.c -- a small program with two classic
 * concurrency bugs, for finding them with the libxtc USDT probes
 * (xtc_usdt(3), tools/dtrace, tools/bpftrace; docs/guide/12-tracing.md).
 *
 *   ./convoy convoy    WORKERS fibers each take one shared xtc_amutex and
 *                      then "do I/O" (xtc_proc_sleep) while holding it:
 *                      a lock convoy.  Every worker spends most of its
 *                      time queued on the lock, not working.
 *   ./convoy fixed     the same work with the I/O moved outside the lock.
 *   ./convoy lostwake  a client asks a server for a reply; for one request
 *                      in DROP_EVERY the server forgets to answer, so the
 *                      client's xtc_recv parks and is never woken until
 *                      its timeout.
 *
 * Every mode exits 0 and prints its elapsed time, so the bug is a
 * performance symptom, not a crash -- the kind a probe finds faster than
 * a debugger.  Uses only the public xtc_* API.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_sync.h"

#define WORKERS    8
#define ITERS      20
#define IO_NS      (200 * 1000)          /* 200 us of "I/O" */
#define REQUESTS   40
#define DROP_EVERY 10
#define REPLY_WAIT (20LL * 1000 * 1000)  /* 20 ms recv timeout */

static xtc_amutex_t *g_mu;
static int           g_fixed;
static long          g_shared;
static int           g_finished;
static int           g_timeouts;
static xtc_pid_t     g_server;

static void
worker(void *arg)
{
	int i;
	(void)arg;
	for (i = 0; i < ITERS; i++) {
		if (xtc_amutex_lock(g_mu, -1) != XTC_OK)
			return;
		g_shared++;
		if (!g_fixed)
			(void)xtc_proc_sleep(IO_NS);   /* BUG: I/O under the lock */
		(void)xtc_amutex_unlock(g_mu);
		if (g_fixed)
			(void)xtc_proc_sleep(IO_NS);   /* I/O outside it */
	}
	g_finished++;
}

struct req {
	xtc_pid_t from;
	int       n;
};

static void
server(void *arg)
{
	int handled = 0;
	(void)arg;
	while (handled < REQUESTS) {
		void *m;
		size_t sz;
		struct req r;
		if (xtc_recv(&m, &sz, -1) != XTC_OK)
			return;
		if (sz != sizeof r) {
			xtc_free(m);
			continue;
		}
		memcpy(&r, m, sizeof r);
		xtc_free(m);
		handled++;
		if (r.n % DROP_EVERY == DROP_EVERY - 1)
			continue;                     /* BUG: reply forgotten */
		if (xtc_send(r.from, &r.n, sizeof r.n) != XTC_OK)
			return;
	}
}

static void
client(void *arg)
{
	int i;
	(void)arg;
	for (i = 0; i < REQUESTS; i++) {
		struct req r;
		void *m;
		size_t sz;
		int rc;
		r.from = xtc_self();
		r.n = i;
		if (xtc_send(g_server, &r, sizeof r) != XTC_OK)
			return;
		rc = xtc_recv(&m, &sz, REPLY_WAIT);
		if (rc == XTC_E_AGAIN) {
			g_timeouts++;
			continue;
		}
		if (rc != XTC_OK)
			return;
		xtc_free(m);
	}
	g_finished++;
}

int
main(int argc, char **argv)
{
	xtc_loop_t *loop;
	int64_t t0, t1;
	int i, lostwake;
	const char *mode = argc > 1 ? argv[1] : "convoy";

	if (strcmp(mode, "--help") == 0 || (strcmp(mode, "convoy") != 0 &&
	    strcmp(mode, "fixed") != 0 && strcmp(mode, "lostwake") != 0)) {
		printf("usage: %s [convoy|fixed|lostwake]\n"
		    "A lock convoy, its fix, or a lost reply, to find with "
		    "tools/dtrace or tools/bpftrace.\n", argv[0]);
		return strcmp(mode, "--help") == 0 ? 0 : 2;
	}
	g_fixed = strcmp(mode, "fixed") == 0;
	lostwake = strcmp(mode, "lostwake") == 0;

	if (xtc_amutex_create(&g_mu) != XTC_OK ||
	    xtc_loop_init(&loop) != XTC_OK)
		return 1;
	t0 = xtc_clock_mono();
	if (lostwake) {
		if (xtc_proc_spawn(loop, server, NULL, NULL, &g_server) != XTC_OK ||
		    xtc_proc_spawn(loop, client, NULL, NULL, NULL) != XTC_OK)
			return 1;
	} else {
		for (i = 0; i < WORKERS; i++)
			if (xtc_proc_spawn(loop, worker, NULL, NULL, NULL) != XTC_OK)
				return 1;
	}
	if (xtc_loop_run(loop) != XTC_OK)
		return 1;
	t1 = xtc_clock_mono();
	(void)xtc_loop_fini(loop);
	xtc_amutex_destroy(g_mu);

	if (lostwake) {
		printf("lostwake: %d requests, %d replies never came, %.1f ms\n",
		    REQUESTS, g_timeouts, (double)(t1 - t0) / 1e6);
		return g_finished == 1 ? 0 : 1;
	}
	printf("%s: %d workers x %d iterations, %.1f ms\n", mode, WORKERS,
	    ITERS, (double)(t1 - t0) / 1e6);
	return g_finished == WORKERS && g_shared == WORKERS * ITERS ? 0 : 1;
}
