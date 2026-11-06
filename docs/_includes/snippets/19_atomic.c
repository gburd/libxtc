/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * docs/_includes/snippets/19_atomic.c -- the public atomics on plain
 * fields, each operation with its memory order spelled out.
 *
 * One writer thread fills a quote (two plain fields) and hands it over
 * through a pointer with a release store; the main thread takes it with
 * an acq_rel exchange, so it sees every field written before the
 * release, and the writer (whose acquire load sees the slot empty)
 * never refills a buffer the reader is still reading.  The writer counts
 * its updates with a relaxed fetch_add (it is the only writer) and a CAS
 * loop raises a high-water mark.  The struct has no `_Atomic` member, so
 * it is the same in C and C++.
 *
 * Exits 0 when every quote taken was whole and the counts are exact.
 */

/* !region full */
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>

#include "xtc.h"

#define N 100000

struct quote {
	int64_t bid, ask;	/* ask == bid + 1 in every published quote */
};

struct stats {
	uint64_t n_updates;	/* one writer: relaxed fetch_add */
	int64_t hi_bid;		/* many writers: CAS loop */
	struct quote *top;	/* release store, acquire load */
};

static struct stats g_st;
static struct quote g_q[2];

static void
raise_hi(int64_t bid)
{
	int64_t cur = xtc_atomic_load_i64(&g_st.hi_bid, xtc_mo_relaxed);

	while (cur < bid && !xtc_atomic_cas_weak_i64(&g_st.hi_bid, &cur, bid,
	    xtc_mo_relaxed, xtc_mo_relaxed))
		;
}

static void *
writer(void *arg)
{
	int i;
	struct quote *q;

	(void)arg;
	for (i = 1; i <= N; i++) {
		/* wait for the reader to take the last quote */
		while (xtc_atomic_load_ptr(&g_st.top, xtc_mo_acquire) != NULL)
			XTC_CPU_RELAX();
		q = &g_q[i & 1];
		q->bid = i;
		q->ask = i + 1;
		xtc_atomic_store_ptr(&g_st.top, q, xtc_mo_release);
		xtc_atomic_fetch_add_u64(&g_st.n_updates, 1, xtc_mo_relaxed);
		raise_hi(i);
	}
	return NULL;
}

int
main(void)
{
	pthread_t th;
	struct quote *q;
	long taken = 0, torn = 0;

	if (pthread_create(&th, NULL, writer, NULL) != 0)
		return 1;
	while (taken < N) {
		q = xtc_atomic_exchange_ptr(&g_st.top, NULL, xtc_mo_acq_rel);
		if (q == NULL) {
			XTC_CPU_RELAX();
			continue;
		}
		if (q->ask != q->bid + 1 || q->bid != taken + 1)
			torn++;
		taken++;
	}
	pthread_join(th, NULL);
	printf("taken %ld, updates %llu, high bid %lld, torn %ld\n", taken,
	    (unsigned long long)g_st.n_updates, (long long)g_st.hi_bid, torn);
	return (torn == 0 && g_st.n_updates == N && g_st.hi_bid == N) ? 0 : 1;
}
/* !endregion full */
