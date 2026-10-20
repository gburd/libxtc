/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_wal_group_sync.c
 *	Off-loop commits (wal_commit_sync, the path every committer on a
 *	plain thread takes -- an embedded engine, the in-process binding)
 *	must GROUP: concurrent committers share an fdatasync.
 *
 *	wal_commit_sync used to write and fdatasync each record under one
 *	mutex, so N threads committing at once paid N fsyncs in series and
 *	the throughput of the whole store was 1/fsync-latency no matter how
 *	many threads committed.  Now the first committer to find no flush in
 *	progress flushes every record queued behind it.
 *
 *	NTHR threads x PER commits each.  Asserts: every commit got a unique
 *	LSN (sum and xor of 1..N), the log replays N records in strictly
 *	increasing LSN order with the right payloads, and fsyncs < commits/2
 *	(group factor > 2).  The old serial path gives exactly 1 fsync per
 *	commit.  Then a failed log (fd closed under it) fails every waiter
 *	with XTC_E_IO and latches.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "wal.h"
#include "xtc.h"
#include "t_tmp.h"

#define NTHR 16
#define PER  200

static int              g_fail;
static wal_t           *g_wal;
static _Atomic uint64_t g_sum, g_xor;
static _Atomic long     g_ok;
static uint64_t         g_seen_last;
static long             g_seen, g_bad_order, g_bad_body;

#ifdef CK
#undef CK
#endif
#define CK(x) do { if (!(x)) { \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
	g_fail = 1; } } while (0)

static void *
committer(void *arg)
{
	long id = (long)arg, i;
	char rec[64];
	for (i = 0; i < PER; i++) {
		uint64_t lsn = 0;
		int n = snprintf(rec, sizeof rec, "t%ld-%ld", id, i);
		if (wal_commit_sync(g_wal, rec, (uint32_t)n, &lsn) != XTC_OK) {
			g_fail = 1;
			break;
		}
		atomic_fetch_add(&g_sum, lsn);
		atomic_fetch_xor(&g_xor, lsn);
		atomic_fetch_add(&g_ok, 1);
	}
	return NULL;
}

static int
replay_cb(uint64_t lsn, const void *rec, uint32_t len, void *user)
{
	(void)user;
	if (lsn <= g_seen_last) g_bad_order++;
	g_seen_last = lsn;
	if (len < 4 || ((const char *)rec)[0] != 't') g_bad_body++;
	g_seen++;
	return 0;
}

static void *
failer(void *arg)
{
	uint64_t lsn;
	(void)arg;
	if (wal_commit_sync(g_wal, "x", 1, &lsn) != XTC_E_IO)
		g_fail = 1;
	return NULL;
}

int
main(void)
{
	char path[256];
	wal_opts_t wo;
	wal_stats_t st;
	pthread_t t[NTHR];
	uint64_t n, want_sum, want_xor = 0, i;
	int fd;
	long k;

	t_tmpl(path, sizeof path, "sqlxtc-walgrp");
	if ((fd = mkstemp(path)) < 0) return 1;
	close(fd);
	memset(&wo, 0, sizeof wo);
	wo.path = path;
	CK(wal_open(&wo, &g_wal) == XTC_OK);
	for (k = 0; k < NTHR; k++)
		CK(pthread_create(&t[k], NULL, committer, (void *)k) == 0);
	for (k = 0; k < NTHR; k++)
		(void)pthread_join(t[k], NULL);
	n = (uint64_t)NTHR * PER;
	want_sum = n * (n + 1) / 2;
	for (i = 1; i <= n; i++) want_xor ^= i;
	wal_get_stats(g_wal, &st);
	printf("  %llu commits, %llu fsyncs (group factor %.1f, max batch %llu)\n",
	    (unsigned long long)st.commits, (unsigned long long)st.batches,
	    st.batches ? (double)st.commits / (double)st.batches : 0.0,
	    (unsigned long long)st.max_batch_seen);
	CK(atomic_load(&g_ok) == (long)n);
	CK(st.commits == n);
	CK(atomic_load(&g_sum) == want_sum && atomic_load(&g_xor) == want_xor);
	CK(st.durable_lsn == n);
	CK(st.batches * 2 < st.commits);        /* the serial path: == commits */
	CK(wal_scan(path, replay_cb, NULL) == XTC_OK);
	CK(g_seen == (long)n && g_bad_order == 0 && g_bad_body == 0);

	/* Fail-stop: a broken fd fails the commit and every one after it. */
	close(wal_fd(g_wal));
	for (k = 0; k < 4; k++)
		CK(pthread_create(&t[k], NULL, failer, NULL) == 0);
	for (k = 0; k < 4; k++)
		(void)pthread_join(t[k], NULL);
	CK(wal_failed(g_wal));
	wal_close(g_wal);
	unlink(path);
	if (g_fail) { fprintf(stderr, "FAIL: test_wal_group_sync\n"); return 1; }
	printf("  ok   off-loop commits share fsyncs; replay is complete and ordered;"
	    " a failed log fails every waiter\n");
	return 0;
}
