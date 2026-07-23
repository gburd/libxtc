/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * bench/bench_proc_table.c -- proc-table lookup (__table_lookup)
 *	contention probe.  The measurement PLAN.md 19.5c option A is
 *	gated on: "does the STRIPED per-loop proc-table lock (option B,
 *	XTC_PT_NSTRIPES=16, already shipped) still serialize the send
 *	hot path, or has striping recovered the parallelism?"
 *
 *	Every xtc_send resolves the target via __resolve -> __table_lookup,
 *	which takes a per-loop STRIPE mutex (stripe = local_id & 15) to
 *	read the slot and pin the target under a refcount.  Under the PG
 *	fiber-per-session pattern, many sender fibers share one carrier
 *	and send to well-distributed targets; striping should spread those
 *	lookups across 16 stripes.  This benchmark reproduces that shape
 *	and reports whether send throughput SCALES with sender count (the
 *	stripes work) or PLATEAUS (the stripe lock is still the wall, and
 *	option A -- fully lock-free reads -- would be justified).
 *
 *	Shape:
 *	  - TARGETS sink procs on loop 0 (each just drains its mailbox);
 *	    their local_ids span the stripe space.  Their table (loop 0's)
 *	    is the SHARED structure every sender resolves against.
 *	  - S sender procs spread across S loops (S OS threads), each
 *	    sending a fixed budget of tiny messages round-robin to all
 *	    TARGETS on loop 0, yielding periodically.  Every send from
 *	    every thread resolves via loop 0's striped proc table -- so
 *	    the stripe locks are contended CROSS-THREAD, the real case.
 *	  - We sweep S = 1, 2, 4, 8 and report sends/sec + scaling.
 *
 *	If sends/sec scales ~linearly in S up to ~min(S, NSTRIPES), the
 *	striped lock is NOT the ceiling and option A is unjustified.  If
 *	it plateaus early (2-4 senders), option A's measurement gate is
 *	met.
 *
 *	Usage: bench_proc_table [targets] [msgs_per_sender]
 */

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"

#define MAX_TARGETS 256
#define MAX_SENDERS 8

static xtc_pid_t g_targets[MAX_TARGETS];
static int       g_ntargets;
static long      g_msgs_per_sender;
static int       g_nsenders;
static int       g_nloops;

static atomic_long g_sends_done;      /* sends issued (all senders) */
static atomic_int  g_senders_done;
static xtc_exec_t *g_exec;

/* Sink: drain the mailbox forever; stopped by exec_stop at the end. */
static void
sink_body(void *arg)
{
	(void)arg;
	for (;;) {
		void *msg = NULL;
		size_t len = 0;
		if (xtc_recv(&msg, &len, 1000 * 1000) == XTC_OK) {
			if (msg != NULL)
				xtc_free(msg);
		}
	}
}

/* Sender: send g_msgs_per_sender tiny messages round-robin to all
 * targets, then mark done. */
static void
sender_body(void *arg)
{
	long i;
	int t = 0;
	uint64_t payload = 0;
	(void)arg;

	for (i = 0; i < g_msgs_per_sender; i++) {
		(void)xtc_send(g_targets[t], &payload, sizeof payload);
		if (++t >= g_ntargets)
			t = 0;
		atomic_fetch_add_explicit(&g_sends_done, 1,
		    memory_order_relaxed);
		if ((i & 255) == 0)
			xtc_yield();
	}
	if (atomic_fetch_add(&g_senders_done, 1) + 1 == g_nsenders)
		(void)xtc_exec_stop(g_exec);
}

/* Spawner on loop 0: create the sinks (spanning the stripe space) on
 * loop 0, and the senders spread ACROSS ALL loops -- every sender is on
 * a DIFFERENT OS thread but they all resolve targets that live in loop
 * 0's proc table, so their __table_lookup calls contend on loop 0's
 * stripe locks cross-thread.  This is the real option-A question: does
 * 16-way striping relieve that cross-thread contention, or is the lock
 * still the wall? */
static void
spawner(void *arg)
{
	xtc_exec_t *e = (xtc_exec_t *)arg;
	int i, nloops = g_nloops;

	for (i = 0; i < g_ntargets; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e, 0), sink_body, NULL,
		    NULL, &g_targets[i]);
	for (i = 0; i < g_nsenders; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e, (unsigned)(i % nloops)),
		    sender_body, NULL, NULL, NULL);
}

static double
now_sec(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Run the shape with `senders` senders; return sends/sec. */
static double
run_once(int senders)
{
	xtc_exec_t *e = NULL;
	double t0, t1;
	long total;

	g_nsenders = senders;
	g_nloops = senders;   /* one loop (OS thread) per sender: max cross-thread contention on loop 0's table */
	atomic_store(&g_sends_done, 0);
	atomic_store(&g_senders_done, 0);

	if (xtc_exec_init(&e, (unsigned)senders) != XTC_OK)
		return -1.0;
	g_exec = e;
	xtc_exec_set_service_mode(e, 1);

	(void)xtc_proc_spawn(xtc_exec_loop(e, 0), spawner, e, NULL, NULL);

	t0 = now_sec();
	if (xtc_exec_run(e) != XTC_OK) { (void)xtc_exec_fini(e); return -1.0; }
	t1 = now_sec();

	total = atomic_load(&g_sends_done);
	(void)xtc_exec_fini(e);
	return (t1 > t0) ? (double)total / (t1 - t0) : 0.0;
}

int
main(int argc, char **argv)
{
	int sweep[] = { 1, 2, 4, 8 };
	size_t k;
	double base = 0.0;

	g_ntargets = (argc > 1) ? atoi(argv[1]) : 64;
	if (g_ntargets < 1) g_ntargets = 1;
	if (g_ntargets > MAX_TARGETS) g_ntargets = MAX_TARGETS;
	g_msgs_per_sender = (argc > 2) ? atol(argv[2]) : 2000000;

	printf("# proc-table lookup contention probe "
	    "(targets=%d, msgs/sender=%ld, senders cross-thread vs loop 0's "
	    "XTC_PT_NSTRIPES striped table)\n", g_ntargets, g_msgs_per_sender);
	printf("# senders  sends/sec       scaling(x vs 1)  efficiency\n");

	for (k = 0; k < sizeof sweep / sizeof sweep[0]; k++) {
		int s = sweep[k];
		double r = run_once(s);
		if (r < 0) { printf("FAIL: exec run errored\n"); return 1; }
		if (k == 0) base = r;
		printf("  %6d  %12.0f  %14.2fx  %9d%%\n",
		    s, r, base > 0 ? r / base : 0.0,
		    base > 0 ? (int)(100.0 * (r / base) / s) : 0);
	}

	printf("#\n"
	    "# READ THE SCALING COLUMN: scaling that KEEPS CLIMBING toward\n"
	    "# min(senders, NSTRIPES) means the striped proc-table lock is\n"
	    "# NOT the ceiling -- option A (fully lock-free reads) is\n"
	    "# UNJUSTIFIED.  Scaling that PLATEAUS at 2-4 senders means the\n"
	    "# stripe lock is still the wall and option A's measurement gate\n"
	    "# is met.\n");
	return 0;
}
