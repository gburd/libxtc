/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * tools/sim-monitor/recorder.c
 *	Runs a small multi-loop ping/pong DST scenario with buggify
 *	enabled, recording an xtc_tail trace (SCHED + SIM sources) to a
 *	file for tools/sim-monitor/viewer.c to render.
 *
 *	This is a DEVELOPER TOOL, not part of the library: it is not
 *	wired into make check, libxtc.a, or the default build/meson
 *	target -- see tools/sim-monitor/README.md.
 *
 * Usage: recorder <seed> <out-trace-file>
 * Build: needs a --with-io-backend=sim libxtc.a (see README.md).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <fcntl.h>
#include <unistd.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_sim.h"
#include "xtc_tail.h"

#define N_LOOPS 4
#define N_PAIRS 10
#define N_MSGS  6

static atomic_int g_delivered;

static void
pong(void *arg)
{
	long expect = (long)(intptr_t)arg;
	long got = 0;
	while (got < expect) {
		void *m = NULL;
		size_t n = 0;
		if (xtc_recv(&m, &n, -1) != XTC_OK)
			break;
		if (m != NULL) {
			free(m);
			got++;
			atomic_fetch_add_explicit(&g_delivered, 1,
			    memory_order_relaxed);
		}
	}
}

struct ping_arg { xtc_pid_t peer; long base; };
static struct ping_arg g_args[N_PAIRS];

static void
ping(void *arg)
{
	struct ping_arg *pa = arg;
	int i;
	for (i = 0; i < N_MSGS; i++) {
		int v = (int)(pa->base + i);
		for (;;) {
			int rc = xtc_send(pa->peer, &v, sizeof v);
			if (rc == XTC_OK)
				break;
			if (rc == XTC_E_AGAIN) {
				xtc_yield();
				continue;
			}
			return;
		}
	}
}

int
main(int argc, char **argv)
{
	uint64_t seed;
	int fd, i;
	xtc_exec_t *e = NULL;

	if (argc != 3) {
		fprintf(stderr, "usage: %s <seed> <out-trace-file>\n", argv[0]);
		return 2;
	}
	seed = (uint64_t)strtoull(argv[1], NULL, 10);

	(void)xtc_tail_reset();
	(void)xtc_tail_enable(XTC_TAIL_SCHED | XTC_TAIL_SIM);
	xtc_sim_buggify_enable(300);   /* 30% activation, a lively trace */

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK) {
		fprintf(stderr, "xtc_exec_init failed\n");
		return 1;
	}
	for (i = 0; i < N_PAIRS; i++) {
		xtc_loop_t *lp = xtc_exec_loop(e, (unsigned)(i % N_LOOPS));
		xtc_loop_t *li = xtc_exec_loop(e, (unsigned)((i + 1) % N_LOOPS));
		xtc_pid_t pong_pid;
		(void)xtc_proc_spawn(lp, pong, (void *)(intptr_t)N_MSGS, NULL,
		    &pong_pid);
		g_args[i].peer = pong_pid;
		g_args[i].base = (long)i * 1000;
		(void)xtc_proc_spawn(li, ping, &g_args[i], NULL, NULL);
	}

	(void)xtc_sim_exec_run(e, seed, 5000000);

	fprintf(stderr, "delivered=%d buggify_active=%d records=%zu\n",
	    atomic_load(&g_delivered), xtc_sim_buggify_active_count(),
	    xtc_tail_count());

	xtc_sim_buggify_disable();
	(void)xtc_exec_fini(e);

	fd = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		perror("open");
		return 1;
	}
	if (xtc_tail_dump(fd) != XTC_OK) {
		fprintf(stderr, "xtc_tail_dump failed\n");
		close(fd);
		return 1;
	}
	close(fd);
	xtc_tail_disable();
	return 0;
}
