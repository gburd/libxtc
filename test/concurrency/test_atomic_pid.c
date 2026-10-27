/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * test/concurrency/test_atomic_pid.c
 *	xtc_atomic_pid_t never hands a reader a torn pid.
 *
 *	2.0 widened xtc_pid_t to 16 bytes, so the places that published a
 *	pid through one _Atomic uint64_t (xtc_app children, the supervisor,
 *	gen_server, rexis's per-fd wake table) now go through
 *	xtc_atomic_pid_t, a seqlock over two words.  A torn read there names
 *	a proc that never existed -- or worse, a live one that is not the
 *	target.  Two writer threads store pids drawn from two families whose
 *	fields are tied together (loop_id == local_id ^ K, gen == f(local_id)),
 *	interleaving freely; four readers load continuously and check each
 *	loaded pid is internally consistent, i.e. came whole from ONE store.
 *	Every word changes between consecutive stores, so a mixed read
 *	breaks the tie in the first field it splits.
 *
 *	A control (`--torn`) runs the same readers against the same writers
 *	storing the two words with no seqlock, to show the check detects a
 *	torn read on this host (it must report torn > 0; that is the
 *	fail-before for a publication that does not go through the seqlock).
 *
 *	Standalone: exit 0 pass, 1 fail, 77 skip.  Bounded by alarm(60).
 */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#if defined(_WIN32)
#include <stdio.h>
int
main(void)
{
	printf("SKIP: test_atomic_pid uses pthreads + alarm\n");
	return 77;
}
#else

#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "xtc.h"
#include "xtc_proc.h"

#define N_WRITERS 2
#define N_READERS 4

static xtc_atomic_pid_t g_pid = XTC_ATOMIC_PID_INIT;
/* the --torn control: the same two words, written with no seqlock */
static _Atomic uint64_t g_raw[2];
static int g_control;
static _Atomic int g_stop;
static _Atomic uint64_t g_loads, g_torn, g_stores;

/* Writer w's i-th pid.  Family w is recognisable from every field. */
static xtc_pid_t
mk(unsigned w, uint64_t i)
{
	xtc_pid_t p;
	uint32_t local = (uint32_t)(i * 2654435761u) | 1u;   /* never 0 */

	p.local_id = local;
	p.loop_id = local ^ (w ? 0xA5A5A5A5u : 0x5A5A5A5Au);
	p.gen = ((uint64_t)local << 32 | (uint64_t)(~local)) ^
	    (w ? 0x1111111111111111ull : 0x2222222222222222ull);
	return p;
}

static int
consistent(xtc_pid_t p)
{
	unsigned w;

	if (xtc_pid_is_none(p))
		return 1;                       /* before the first store */
	for (w = 0; w < N_WRITERS; w++) {
		xtc_pid_t q;
		q.local_id = p.local_id;
		q.loop_id = p.local_id ^ (w ? 0xA5A5A5A5u : 0x5A5A5A5Au);
		q.gen = ((uint64_t)p.local_id << 32 |
		    (uint64_t)(~p.local_id)) ^
		    (w ? 0x1111111111111111ull : 0x2222222222222222ull);
		if (xtc_pid_eq(p, q))
			return 1;
	}
	return 0;
}

static void *
writer(void *arg)
{
	unsigned w = (unsigned)(uintptr_t)arg;
	uint64_t i = 1;
	xtc_pid_t p;

	while (!atomic_load_explicit(&g_stop, memory_order_relaxed)) {
		p = mk(w, i++);
		if (g_control) {
			atomic_store_explicit(&g_raw[0], (uint64_t)p.loop_id |
			    (uint64_t)p.local_id << 32, memory_order_relaxed);
			atomic_store_explicit(&g_raw[1], p.gen,
			    memory_order_relaxed);
		} else {
			xtc_atomic_pid_store(&g_pid, p);
		}
		atomic_fetch_add_explicit(&g_stores, 1, memory_order_relaxed);
	}
	return NULL;
}

static void *
reader(void *arg)
{
	uint64_t n = 0, torn = 0, w0;
	xtc_pid_t p;
	(void)arg;

	while (!atomic_load_explicit(&g_stop, memory_order_relaxed)) {
		if (g_control) {
			w0 = atomic_load_explicit(&g_raw[0],
			    memory_order_relaxed);
			p.loop_id = (uint32_t)w0;
			p.local_id = (uint32_t)(w0 >> 32);
			p.gen = atomic_load_explicit(&g_raw[1],
			    memory_order_relaxed);
		} else {
			p = xtc_atomic_pid_load(&g_pid);
		}
		if (!consistent(p))
			torn++;
		n++;
	}
	atomic_fetch_add(&g_loads, n);
	atomic_fetch_add(&g_torn, torn);
	return NULL;
}

static void
on_alarm(int s)
{
	(void)s;
	(void)!write(2, "FAIL: test_atomic_pid hung (alarm)\n", 35);
	_exit(1);
}

int
main(int argc, char **argv)
{
	pthread_t wt[N_WRITERS], rt[N_READERS];
	xtc_pid_t z;
	unsigned i;
	int64_t run_ns = 2000000000LL;   /* 2 s */

	g_control = argc > 1 && strcmp(argv[1], "--torn") == 0;
	if (getenv("XTC_UNDER_VALGRIND") != NULL ||
	    getenv("XTC_TEST_EMULATED") != NULL)
		run_ns = 200000000LL;        /* still bounded, fewer loads */
	signal(SIGALRM, on_alarm);
	alarm(60);

	/* Zeroed storage reads as XTC_PID_NONE; a store reads back whole. */
	z = xtc_atomic_pid_load(&g_pid);
	if (!xtc_pid_is_none(z)) {
		printf("FAIL: XTC_ATOMIC_PID_INIT does not read as NONE\n");
		return 1;
	}
	xtc_atomic_pid_store(&g_pid, mk(1, 7));
	if (!xtc_pid_eq(xtc_atomic_pid_load(&g_pid), mk(1, 7))) {
		printf("FAIL: single-thread store/load round trip\n");
		return 1;
	}

	for (i = 0; i < N_READERS; i++)
		if (pthread_create(&rt[i], NULL, reader, NULL) != 0)
			return 77;
	for (i = 0; i < N_WRITERS; i++)
		if (pthread_create(&wt[i], NULL, writer,
		    (void *)(uintptr_t)i) != 0)
			return 77;
	(void)xtc_sleep_ns(run_ns);
	atomic_store(&g_stop, 1);
	for (i = 0; i < N_WRITERS; i++)
		(void)pthread_join(wt[i], NULL);
	for (i = 0; i < N_READERS; i++)
		(void)pthread_join(rt[i], NULL);

	printf("%s: %d writers, %d readers: %llu stores, %llu loads, "
	    "%llu torn\n", g_control ? "control (no seqlock)" : "seqlock",
	    N_WRITERS, N_READERS, (unsigned long long)atomic_load(&g_stores),
	    (unsigned long long)atomic_load(&g_loads),
	    (unsigned long long)atomic_load(&g_torn));
	if (atomic_load(&g_stores) == 0 || atomic_load(&g_loads) == 0) {
		printf("FAIL: no progress\n");
		return 1;
	}
	if (g_control)
		return 0;                    /* the count is the evidence */
	if (atomic_load(&g_torn) != 0) {
		printf("FAIL: a reader saw a torn pid\n");
		return 1;
	}
	printf("OK\n");
	return 0;
}
#endif /* !_WIN32 */
