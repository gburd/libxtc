/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_wal_zero_step.c
 *	The flush leader zero-fills the log ahead of the append point at
 *	most 1 MiB at a time.  It wrote 8 MiB (8 O_DSYNC writes, ~3 ms)
 *	inside one flush, once per 8 MiB of log, and every commit queued
 *	behind it waited: the top latency slots of an open-loop ledger run
 *	(35k commits/s: p99.9 2.9 ms, 0.6 ms with 1 MiB steps).
 *	  (a) one thread, 48 MiB of log in 64 KiB records: no single
 *	      zero-fill exceeds 1 MiB, and the file still runs ahead of
 *	      the log (the appends overwrite zeros, they do not grow it);
 *	  (b) 8 threads, records of 100 B .. 192 KiB plus 3 MiB records
 *	      (written directly, they can pass the zeros): a scan of the
 *	      file finds every record, in LSN order.
 *	#includes wal.c for the zero-fill hook (WAL_ZERO_TEST_HOOK).
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/stat.h>

static _Atomic uint64_t g_zmax, g_zcalls;  /* largest zero-fill, count */
static void
zero_seen(uint64_t n)
{
	uint64_t cur = atomic_load(&g_zmax);
	atomic_fetch_add(&g_zcalls, 1);
	while (n > cur && !atomic_compare_exchange_weak(&g_zmax, &cur, n))
		;
}
#define WAL_ZERO_TEST_HOOK(n) zero_seen((uint64_t)(n))
#include "wal.c"

#include "xtc.h"
#include "t_tmp.h"

#define NT    8
#define PER   200
#define BIGN  (3u << 20)

static wal_t *g_w;
static _Atomic long g_ok, g_bad, g_bigs;

struct sc { uint64_t prev, n, big; int ordered; };

static int
scan_cb(uint64_t lsn, const void *rec, uint32_t len, void *user)
{
	struct sc *s = user;
	(void)rec;
	if (s->n > 0 && lsn <= s->prev) s->ordered = 0;
	if (len >= BIGN) s->big++;
	s->prev = lsn;
	s->n++;
	return 0;
}

static long
file_size(const char *p)
{
	struct stat st;
	return stat(p, &st) == 0 ? (long)st.st_size : -1;
}

static void *
committer(void *arg)
{
	static uint8_t big[BIGN];
	static uint8_t rec[NT][192 * 1024];
	uint8_t *r = rec[(long)arg];
	uint64_t lsn, seed = 0x9E3779B97F4A7C15ull * (uint64_t)((long)arg + 1);
	int i;
	memset(r, 0x5a, sizeof rec[0]);
	for (i = 0; i < PER; i++) {
		uint32_t len;
		seed ^= seed << 13; seed ^= seed >> 7; seed ^= seed << 17;
		len = 100 + (uint32_t)(seed % (sizeof rec[0] - 100));
		if (i % 50 == 25) {
			if (wal_commit_sync(g_w, big, BIGN, &lsn) == XTC_OK)
				atomic_fetch_add(&g_bigs, 1);
			else
				atomic_fetch_add(&g_bad, 1);
			continue;
		}
		if (wal_commit_sync(g_w, r, len, &lsn) == XTC_OK)
			atomic_fetch_add(&g_ok, 1);
		else
			atomic_fetch_add(&g_bad, 1);
	}
	return NULL;
}

int
main(void)
{
	char path[256];
	static uint8_t rec[64 * 1024];
	wal_opts_t wo;
	struct sc s;
	pthread_t th[NT];
	uint64_t lsn = 0;
	long n;
	int i, fd;

	t_tmpl(path, sizeof path, "wal-zero-step");
	if ((fd = mkstemp(path)) < 0) return 1;
	close(fd);
	memset(rec, 'r', sizeof rec);
	memset(&wo, 0, sizeof wo);
	wo.path = path;

	/* (a) */
	CK(wal_open(&wo, &g_w) == XTC_OK);
	if (g_w == NULL) return 1;
	for (n = 0; wal_size(g_w) < (48u << 20); n++)
		CK(wal_commit_sync(g_w, rec, sizeof rec, &lsn) == XTC_OK);
	printf("  (a) %ld commits, log %llu MiB, file %ld MiB: %llu zero-fills, "
	    "the largest %llu KiB\n", n, (unsigned long long)(wal_size(g_w) >> 20),
	    file_size(path) >> 20, (unsigned long long)atomic_load(&g_zcalls),
	    (unsigned long long)(atomic_load(&g_zmax) >> 10));
	CK(atomic_load(&g_zmax) <= (1u << 20));
	CK(file_size(path) > (long)wal_size(g_w));
	wal_close(g_w);

	/* (b) */
	wo.append = 1;
	CK(wal_open(&wo, &g_w) == XTC_OK);
	if (g_w == NULL) return 1;
	for (i = 0; i < NT; i++)
		(void)pthread_create(&th[i], NULL, committer, (void *)(long)i);
	for (i = 0; i < NT; i++)
		(void)pthread_join(th[i], NULL);
	wal_close(g_w);                   /* a crash: no truncation */
	memset(&s, 0, sizeof s); s.ordered = 1;
	CK(wal_scan(path, scan_cb, &s) == XTC_OK);
	printf("  (b) %d threads: %ld + %ld big committed, %ld failed; scan %llu "
	    "records (%llu big), ordered %d; largest zero-fill %llu KiB\n", NT,
	    atomic_load(&g_ok), atomic_load(&g_bigs), atomic_load(&g_bad),
	    (unsigned long long)s.n, (unsigned long long)s.big, s.ordered,
	    (unsigned long long)(atomic_load(&g_zmax) >> 10));
	CK(atomic_load(&g_bad) == 0);
	CK(s.n == (uint64_t)n + (uint64_t)atomic_load(&g_ok) +
	    (uint64_t)atomic_load(&g_bigs));
	CK(s.big == (uint64_t)atomic_load(&g_bigs));
	CK(s.ordered);
	CK(atomic_load(&g_zmax) <= (1u << 20));

	unlink(path);
	if (g_fail) { fprintf(stderr, "FAIL: test_wal_zero_step\n"); return 1; }
	printf("  ok   the log zero-fills 1 MiB at a time and never over a record\n");
	return 0;
}
