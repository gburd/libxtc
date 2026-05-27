/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * bench/audit/false_sharing_proc.c
 *	Micro-benchmark demonstrating the performance impact of
 *	cache-line separation between proc mailbox producer/consumer fields.
 *
 *	Build:
 *	  gcc -O2 -pthread -I../../src/inc \
 *	      -o false_sharing_proc false_sharing_proc.c \
 *	      ../../build_unix/libxtc.a
 *
 *	Run:
 *	  ./false_sharing_proc
 *
 *	The test spawns a proc that receives messages from multiple sender
 *	threads.  With proper cache-line separation (mbox_head/mbox_n on one
 *	line, mbox_lock/mbox_tail on another), senders don't invalidate the
 *	receiver's cache line when appending to the tail.
 */

#include "xtc_int.h"
#include "xtc_loop.h"
#include "xtc_proc.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define N_SENDERS     4
#define MSGS_PER_SEND 50000

static xtc_loop_t *g_loop;
static xtc_pid_t   g_receiver;
static _Atomic int g_senders_done;
static _Atomic int g_received;

static int64_t
now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void *
sender_thread(void *arg)
{
	int id = (int)(intptr_t)arg;
	int i;
	uint8_t msg[16];

	memset(msg, 0, sizeof msg);
	msg[0] = (uint8_t)id;

	for (i = 0; i < MSGS_PER_SEND; i++) {
		msg[1] = (uint8_t)(i & 0xff);
		while (xtc_send(g_receiver, msg, sizeof msg) != XTC_OK) {
			/* Mailbox full; yield and retry. */
			sched_yield();
		}
	}

	atomic_fetch_add(&g_senders_done, 1);
	return NULL;
}

static void
receiver_proc(void *arg)
{
	int expected = N_SENDERS * MSGS_PER_SEND;
	int count = 0;
	(void)arg;

	while (count < expected) {
		void *msg;
		size_t sz;
		int rc = xtc_recv(&msg, &sz, 10 * 1000000LL); /* 10ms timeout */
		if (rc == XTC_OK) {
			count++;
			free(msg);
		}
	}
	atomic_store(&g_received, count);
}

int
main(void)
{
	pthread_t senders[N_SENDERS];
	int64_t start, end;
	double elapsed_ms;
	int i;

	if (xtc_loop_init(&g_loop) != XTC_OK) {
		fprintf(stderr, "failed to init loop\n");
		return 1;
	}

	xtc_proc_opts_t opts = { .mailbox_cap = 8192 };
	if (xtc_proc_spawn(g_loop, receiver_proc, NULL, &opts, &g_receiver) != XTC_OK) {
		fprintf(stderr, "failed to spawn receiver\n");
		return 1;
	}

	printf("Proc mailbox cache-line audit\n");
	printf("XTC_CACHE_LINE: %d\n", XTC_CACHE_LINE);
	printf("\n");

	printf("Benchmark: %d senders x %d msgs = %d total\n",
	    N_SENDERS, MSGS_PER_SEND, N_SENDERS * MSGS_PER_SEND);
	printf("Mailbox capacity: 8192\n");
	printf("\n");

	atomic_store(&g_senders_done, 0);
	atomic_store(&g_received, 0);

	start = now_ns();

	/* Start sender threads. */
	for (i = 0; i < N_SENDERS; i++) {
		if (pthread_create(&senders[i], NULL, sender_thread,
		    (void *)(intptr_t)i) != 0) {
			fprintf(stderr, "failed to create sender %d\n", i);
			return 1;
		}
	}

	/* Run the loop until all messages are received. */
	while (atomic_load(&g_received) < N_SENDERS * MSGS_PER_SEND) {
		xtc_loop_run(g_loop);
		/* Small sleep to avoid busy-wait if senders are slow. */
		if (atomic_load(&g_received) < N_SENDERS * MSGS_PER_SEND) {
			struct timespec ts = { .tv_nsec = 100000 }; /* 0.1ms */
			nanosleep(&ts, NULL);
		}
	}

	end = now_ns();

	for (i = 0; i < N_SENDERS; i++)
		pthread_join(senders[i], NULL);

	elapsed_ms = (double)(end - start) / 1000000.0;

	printf("Received: %d msgs\n", atomic_load(&g_received));
	printf("Elapsed: %.2f ms\n", elapsed_ms);
	printf("Throughput: %.0f msgs/sec\n",
	    (double)(N_SENDERS * MSGS_PER_SEND) / (elapsed_ms / 1000.0));

	xtc_loop_fini(g_loop);
	return 0;
}
