/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * bench/bench_xproc_fanout.c -- cross-fork spawn/monitor scale probe.
 *
 * Fork N children (via xtc_xspawn), monitor each, send each a message,
 * and wait for all N DOWNs.  Reports children/sec and the wall time, to
 * find where the xtc_xproc relay/monitor machinery (or the OS fork/fd
 * limits) caps throughput.  A ceiling well below the OS process limit
 * points at a library bottleneck (the shadow-proc-per-child, the
 * control-socket framing, the monitor delivery).
 *
 *   bench_xproc_fanout [n_children]     (default 200)
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_xproc.h"

/* Child: receive one int, exit with (it & 0x7f). */
static void
child_body(void *arg)
{
	int code = 0;
	void *m = NULL; size_t n = 0;
	(void)arg;
	if (xtc_recv(&m, &n, 2000LL * 1000 * 1000) == XTC_OK && n == sizeof(int))
		code = *(int *)m;
	if (m) xtc_free(m);
	xtc_exit_self(code & 0x7f);
}

struct driver_ctx {
	xtc_loop_t *loop;
	int         n;
	int         downs;
	int         spawn_fail;
	double      secs;
};

static void
driver(void *a)
{
	struct driver_ctx *d = a;
	xtc_xproc_t **kids;
	int i;
	int64_t t0 = 0, t1 = 0;

	kids = calloc((size_t)d->n, sizeof *kids);
	if (kids == NULL) return;

	t0 = xtc_clock_mono();
	for (i = 0; i < d->n; i++) {
		int code = 1 + (i & 0x3f);
		if (xtc_xspawn(d->loop, "kid", child_body, &code, sizeof code,
		    &kids[i]) != XTC_OK) {
			d->spawn_fail++;
			kids[i] = NULL;
			continue;
		}
		(void)xtc_xmonitor(kids[i], NULL);
		(void)xtc_xsend(kids[i], &code, sizeof code);
	}
	/* Collect one DOWN per successfully spawned child. */
	for (i = 0; i < d->n - d->spawn_fail; i++) {
		void *m = NULL; size_t n = 0;
		if (xtc_recv(&m, &n, 10LL * 1000 * 1000 * 1000) != XTC_OK)
			break;
		{
			xtc_down_info_t di;
			if (xtc_down_decode_ex(m, n, &di) == XTC_OK)
				d->downs++;
		}
		if (m) xtc_free(m);
	}
	t1 = xtc_clock_mono();
	d->secs = (double)(t1 - t0) / 1e9;

	for (i = 0; i < d->n; i++)
		if (kids[i]) xtc_xproc_destroy(kids[i]);
	free(kids);
}

int
main(int argc, char **argv)
{
	xtc_loop_t *loop = NULL;
	struct driver_ctx d;
	int n = argc > 1 ? atoi(argv[1]) : 200;

	memset(&d, 0, sizeof d);
	d.n = n;
	if (xtc_loop_init(&loop) != XTC_OK) { fprintf(stderr, "loop\n"); return 1; }
	d.loop = loop;
	if (xtc_proc_spawn(loop, driver, &d, NULL, NULL) != XTC_OK) {
		fprintf(stderr, "spawn\n"); return 1;
	}
	(void)xtc_loop_run(loop);
	(void)xtc_loop_fini(loop);

	printf("xproc_fanout: n=%d spawned=%d downs=%d spawn_fail=%d "
	    "%.3fs  %.0f children/sec\n",
	    n, n - d.spawn_fail, d.downs, d.spawn_fail, d.secs,
	    d.secs > 0 ? (double)d.downs / d.secs : 0.0);
	if (d.downs != n - d.spawn_fail) {
		fprintf(stderr, "FAIL: downs=%d != spawned=%d\n",
		    d.downs, n - d.spawn_fail);
		return 1;
	}
	return 0;
}
