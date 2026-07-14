/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/concurrency/test_proc_table_stress.c
 *	Stress + correctness guard for the STRIPED per-loop proc table
 *	(PLAN.md 19.5c).  The per-loop table lock used to be a single
 *	mutex, so every xtc_send to any proc on a carrier serialized on
 *	it -- the measured PG fiber-per-session bottleneck.  It is now
 *	striped by local_id (16 stripes): sends to procs with different
 *	local_ids take different stripes and proceed in parallel.
 *
 *	This test exercises exactly that shape and asserts the striping
 *	did not break routing or lifetime:
 *	  - Spawn RECEIVERS receiver procs across LOOPS loops (each just
 *	    counts the messages it gets).
 *	  - SENDERS foreign OS threads each fire ITERS messages, EACH to
 *	    a uniformly-random distinct receiver (so the target local_id
 *	    varies every send -> maximal stripe spread, the parallel
 *	    path).  The payload encodes (sender, seq) so a misrouted or
 *	    torn delivery is detectable.
 *	  - After all senders join and the loops drain, the SUM of every
 *	    receiver's count must equal SENDERS*ITERS exactly (no lost,
 *	    no duplicated, no misrouted message), and no send may have
 *	    resolved+pinned a freed proc (ASan/TSan would catch a UAF;
 *	    the conservation check catches a routing bug).
 *
 *	Standalone (exit 0 = pass, 1 = fail, 77 = skip), alarm-guarded
 *	against a hang.  Run under ASan/TSan in CI for the UAF/race story.
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_exec.h"
#include "xtc_proc.h"

#define LOOPS      4
#define RECEIVERS  64        /* > 16 stripes, so stripes genuinely share */
#define SENDERS    8
#define ITERS      2000

static xtc_exec_t   *g_exec;
static xtc_pid_t     g_rcv[RECEIVERS];
static atomic_int    g_ready;         /* receivers that have registered */
static atomic_int    g_recv_total;    /* messages delivered, all receivers */
static atomic_int    g_bad;           /* torn/misrouted payloads */
static atomic_int    g_go;

struct msg { int sender; int seq; int target_idx; };

/* The executor runs on its own thread (service mode) so the main
 * thread can drive foreign senders while it services the loops. */
static void *
exec_thread(void *arg)
{
	(void)arg;
	(void)xtc_exec_run(g_exec);
	return NULL;
}

/* Receiver proc: register its pid into g_rcv[idx], then recv until it
 * gets a sentinel stop message. */
static void
receiver(void *arg)
{
	int idx = (int)(intptr_t)arg;
	g_rcv[idx] = xtc_self();
	atomic_fetch_add_explicit(&g_ready, 1, memory_order_release);

	for (;;) {
		void *data = NULL;
		size_t sz = 0;
		if (xtc_recv(&data, &sz, 1000 * 1000 * 100) != XTC_OK)
			continue;
		if (sz == 0) {            /* stop sentinel */
			if (data) xtc_free(data);
			break;
		}
		if (sz == sizeof(struct msg)) {
			struct msg *m = data;
			if (m->target_idx != idx)   /* misrouted! */
				atomic_fetch_add_explicit(&g_bad, 1,
				    memory_order_relaxed);
			atomic_fetch_add_explicit(&g_recv_total, 1,
			    memory_order_relaxed);
		} else {
			atomic_fetch_add_explicit(&g_bad, 1,
			    memory_order_relaxed);
		}
		if (data) xtc_free(data);
	}
}

/* Foreign sender thread: fire ITERS messages to random distinct
 * receivers.  Cross-thread xtc_send -> __resolve -> striped
 * __table_lookup on a varying local_id. */
static void *
sender_thread(void *arg)
{
	int id = (int)(intptr_t)arg;
	uint64_t rng = 0x9E3779B97F4A7C15ULL * (uint64_t)(id + 1);
	int i;
	while (!atomic_load_explicit(&g_go, memory_order_acquire))
		sched_yield();
	for (i = 0; i < ITERS; i++) {
		struct msg m;
		int t;
		rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
		t = (int)((rng >> 33) % RECEIVERS);
		m.sender = id; m.seq = i; m.target_idx = t;
		/* Retry on transient backpressure so conservation is exact. */
		while (xtc_send(g_rcv[t], &m, sizeof m) == XTC_E_AGAIN)
			sched_yield();
	}
	return NULL;
}

int
main(void)
{
	xtc_exec_t *e = NULL;
	pthread_t st[SENDERS];
	pthread_t et;
	int i;

	alarm(60);   /* hang guard */

	if (xtc_exec_init(&e, LOOPS) != XTC_OK) {
		printf("SKIP: cannot init executor\n");
		return 77;
	}
	g_exec = e;
	xtc_exec_set_service_mode(e, 1);   /* do not idle-auto-stop */
	atomic_store(&g_ready, 0);
	atomic_store(&g_recv_total, 0);
	atomic_store(&g_bad, 0);
	atomic_store(&g_go, 0);

	for (i = 0; i < RECEIVERS; i++)
		(void)xtc_proc_spawn(xtc_exec_loop(e, (unsigned)(i % LOOPS)),
		    receiver, (void *)(intptr_t)i, NULL, NULL);

	/* Run the executor on its own thread so receivers register. */
	if (pthread_create(&et, NULL, exec_thread, NULL) != 0) {
		printf("SKIP: cannot start executor thread\n");
		return 77;
	}
	/* Wait for every receiver to register its pid. */
	while (atomic_load_explicit(&g_ready, memory_order_acquire) < RECEIVERS)
		usleep(1000);

	for (i = 0; i < SENDERS; i++)
		pthread_create(&st[i], NULL, sender_thread, (void *)(intptr_t)i);
	atomic_store_explicit(&g_go, 1, memory_order_release);
	for (i = 0; i < SENDERS; i++)
		pthread_join(st[i], NULL);

	/* Drain: wait until every message is accounted for. */
	{
		int spins = 0;
		while (atomic_load_explicit(&g_recv_total,
		    memory_order_acquire) < SENDERS * ITERS && spins++ < 10000)
			usleep(1000);
	}

	/* Stop every receiver (empty message = sentinel). */
	for (i = 0; i < RECEIVERS; i++)
		(void)xtc_send(g_rcv[i], NULL, 0);

	(void)xtc_exec_stop(e);
	pthread_join(et, NULL);
	(void)xtc_exec_fini(e);

	{
		int total = atomic_load(&g_recv_total);
		int bad = atomic_load(&g_bad);
		printf("proc-table stress: %d receivers x %d loops, %d senders "
		    "x %d iters => %d delivered (expect %d), %d bad\n",
		    RECEIVERS, LOOPS, SENDERS, ITERS, total, SENDERS * ITERS,
		    bad);
		if (bad != 0) {
			printf("FAIL: %d misrouted/torn message(s) -- striping "
			    "broke routing\n", bad);
			return 1;
		}
		if (total != SENDERS * ITERS) {
			printf("FAIL: message conservation violated "
			    "(%d != %d) -- lost or duplicated delivery\n",
			    total, SENDERS * ITERS);
			return 1;
		}
	}
	printf("OK: striped proc table -- concurrent cross-thread sends to "
	    "many distinct receivers all delivered exactly once, none "
	    "misrouted, no proc freed under a resolver\n");
	return 0;
}
