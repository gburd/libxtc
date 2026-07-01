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
#include "xtc_sim.h"

/*
 * DST coverage of BUGGIFY (FoundationDB-style pessimal-path injection).
 * A buggify point is a named site in the REAL runtime that, under sim,
 * lets code take a legal-but-pessimal path -- decided once per run per
 * site (a coin flipped on first reach, cached), so a buggified site is
 * consistent within a run and the run replays from the seed.
 *
 * This test exercises the "proc.mbox.spurious_full" buggify planted in
 * xtc_send: with buggify enabled, a send can report XTC_E_AGAIN even
 * with mailbox room to spare (a legal backpressure outcome).  The
 * senders here RETRY on AGAIN (yielding between attempts), so every
 * message is eventually delivered -- proving the sender's backpressure
 * handling is correct under the pessimal path.
 *
 * Asserts:
 *   - with buggify ON, at least one buggify point activated (the
 *     pessimal path was actually taken -- coverage);
 *   - all messages are still delivered (senders survive the spurious
 *     fulls by retrying);
 *   - the run replays: same seed -> same buggify activation count AND
 *     same delivery-order hash;
 *   - with buggify OFF, zero activations (production never buggifies).
 */

#define N_LOOPS 4
#define N_PAIRS 12
#define N_MSGS  8        /* messages each ping sends to its pong */

static atomic_int  g_delivered;
static atomic_long g_hash;

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
			int v;
			memcpy(&v, m, sizeof v);
			free(m);
			got++;
			atomic_fetch_add_explicit(&g_delivered, 1,
			    memory_order_relaxed);
			long h = atomic_load_explicit(&g_hash, memory_order_relaxed);
			h = h * 1000003L + (v + 1);
			atomic_store_explicit(&g_hash, h, memory_order_relaxed);
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
		/* Retry on a (possibly buggified-spurious) AGAIN: the send
		 * must eventually succeed, and the sender must not lose or
		 * duplicate the message.  This is the backpressure-handling
		 * path buggify exists to exercise. */
		for (;;) {
			int rc = xtc_send(pa->peer, &v, sizeof v);
			if (rc == XTC_OK)
				break;
			if (rc == XTC_E_AGAIN) {
				xtc_yield();   /* back off, let the pong drain */
				continue;
			}
			return;   /* a real error (target dead): stop */
		}
	}
}

static void
run_once(uint64_t seed, unsigned bug_pct, int *out_delivered,
    long *out_hash, int *out_bug_active)
{
	xtc_exec_t *e = NULL;
	int i;
	atomic_store(&g_delivered, 0);
	atomic_store(&g_hash, 0);
	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK) { *out_delivered = -1; return; }

	if (bug_pct > 0)
		xtc_sim_buggify_enable(bug_pct);
	else
		xtc_sim_buggify_disable();

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

	*out_delivered = atomic_load(&g_delivered);
	*out_hash = atomic_load(&g_hash);
	*out_bug_active = xtc_sim_buggify_active_count();

	xtc_sim_buggify_disable();
	(void)xtc_exec_fini(e);
}

int
main(void)
{
	int d1 = 0, d2 = 0, doff = 0, b1 = 0, b2 = 0, boff = 0;
	long h1 = 0, h2 = 0, hoff = 0;
	int expect = N_PAIRS * N_MSGS;

	/* Buggify ON at 100% activation: the single point (proc.mbox.
	 * spurious_full) is a once-per-run coin, so 100% guarantees it
	 * activates -- every send then reports a spurious full once, and
	 * the senders must retry.  (A sub-100% probability is a per-run
	 * coin that often lands 0 with a single point; 100% makes the
	 * pessimal-path coverage deterministic for this test.) */
	run_once(0xB0661F, 1000, &d1, &h1, &b1);
	run_once(0xB0661F, 1000, &d2, &h2, &b2);
	/* Buggify OFF. */
	run_once(0xB0661F, 0, &doff, &hoff, &boff);

	printf("bug ON  run1: delivered=%d/%d buggify_active=%d\n", d1, expect, b1);
	printf("bug ON  run2: delivered=%d/%d buggify_active=%d\n", d2, expect, b2);
	printf("bug OFF run : delivered=%d/%d buggify_active=%d\n", doff, expect, boff);

	if (d1 != expect || d2 != expect || doff != expect) {
		printf("FAIL: not all messages delivered under buggify "
		    "(%d/%d/%d, want %d) -- sender backpressure handling "
		    "broke\n", d1, d2, doff, expect);
		return 1;
	}
	if (b1 == 0) {
		printf("FAIL: buggify enabled but no point activated -- the "
		    "pessimal path was never taken\n");
		return 1;
	}
	if (b1 != b2 || h1 != h2) {
		printf("FAIL: buggify run did not replay (active %d/%d, "
		    "hash %ld/%ld)\n", b1, b2, h1, h2);
		return 1;
	}
	if (boff != 0) {
		printf("FAIL: buggify DISABLED but %d points activated\n", boff);
		return 1;
	}

	printf("OK: buggify under DST -- %d pessimal-path activation(s), all "
	    "%d messages still delivered (senders retried the spurious "
	    "fulls), replays from seed; disabled => zero activations\n",
	    b1, expect);
	return 0;
}
