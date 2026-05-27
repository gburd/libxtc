/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * bench/audit/false_sharing_mpsc.c
 *	Micro-benchmark demonstrating the performance impact of
 *	cache-line separation between MPSC channel head/tail counters.
 *
 *	Build:
 *	  gcc -O2 -pthread -I../../src/inc \
 *	      -o false_sharing_mpsc false_sharing_mpsc.c \
 *	      ../../build_unix/libxtc.a
 *
 *	Run:
 *	  ./false_sharing_mpsc
 *
 *	The test spawns multiple producer threads and one consumer thread.
 *	With proper cache-line separation (head on one line, tail on another),
 *	producers don't invalidate the consumer's cache line on CAS, and
 *	vice versa.  Without separation, every send/recv pair causes a
 *	cache-line bounce.
 */

#include "xtc_int.h"
#include "xtc_chan.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <time.h>

#define N_PRODUCERS   4
#define MSGS_PER_PROD 100000
#define CHAN_CAP      4096

static xtc_chan_mpsc_t *g_chan;
static _Atomic int g_producers_done;

static int64_t
now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

static void *
producer(void *arg)
{
	int id = (int)(intptr_t)arg;
	int i;
	(void)id;

	for (i = 0; i < MSGS_PER_PROD; i++) {
		void *msg = (void *)(uintptr_t)(i + 1);
		while (xtc_chan_mpsc_try_send(g_chan, msg) != XTC_OK)
			; /* spin on full */
	}

	atomic_fetch_add(&g_producers_done, 1);
	return NULL;
}

static void *
consumer(void *arg)
{
	int expected = N_PRODUCERS * MSGS_PER_PROD;
	int received = 0;
	(void)arg;

	while (received < expected) {
		void *msg;
		if (xtc_chan_mpsc_try_recv(g_chan, &msg) == XTC_OK) {
			received++;
		}
	}
	return NULL;
}

int
main(void)
{
	pthread_t prods[N_PRODUCERS];
	pthread_t cons;
	int64_t start, end;
	double elapsed_ms;
	int i;

	if (xtc_chan_mpsc_create(NULL, CHAN_CAP, &g_chan) != XTC_OK) {
		fprintf(stderr, "failed to create channel\n");
		return 1;
	}

	/*
	 * Note: struct xtc_chan_mpsc is opaque to users.
	 * The cache-line separation is verified via static_assert in chan.c.
	 */
	printf("MPSC channel cache-line audit\n");
	printf("XTC_CACHE_LINE: %d\n", XTC_CACHE_LINE);
	printf("\n");

	printf("Benchmark: %d producers x %d msgs = %d total\n",
	    N_PRODUCERS, MSGS_PER_PROD, N_PRODUCERS * MSGS_PER_PROD);
	printf("Channel capacity: %d\n", CHAN_CAP);
	printf("\n");

	atomic_store(&g_producers_done, 0);
	start = now_ns();

	if (pthread_create(&cons, NULL, consumer, NULL) != 0) {
		fprintf(stderr, "failed to create consumer\n");
		return 1;
	}

	for (i = 0; i < N_PRODUCERS; i++) {
		if (pthread_create(&prods[i], NULL, producer,
		    (void *)(intptr_t)i) != 0) {
			fprintf(stderr, "failed to create producer %d\n", i);
			return 1;
		}
	}

	for (i = 0; i < N_PRODUCERS; i++)
		pthread_join(prods[i], NULL);
	pthread_join(cons, NULL);

	end = now_ns();
	elapsed_ms = (double)(end - start) / 1000000.0;

	printf("Elapsed: %.2f ms\n", elapsed_ms);
	printf("Throughput: %.0f msgs/sec\n",
	    (double)(N_PRODUCERS * MSGS_PER_PROD) / (elapsed_ms / 1000.0));

	xtc_chan_mpsc_destroy(g_chan);
	return 0;
}
