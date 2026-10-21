/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_wal_prealloc.c
 *	The log keeps its file written with zeros ahead of the append point
 *	(wal_prealloc), so a commit's fdatasync overwrites allocated blocks
 *	instead of growing the file.  The zeros must never read as log:
 *	  (1) after N commits the file runs ahead of the log;
 *	  (2) a scan of the file (recovery) finds exactly the N records;
 *	  (3) a crash (close without truncation) and an append reopen
 *	      resume at the last record, not at the file end: M more commits
 *	      and a scan find N + M records, LSNs in order, none lost.
 *	  (4) a big record (written directly, past the ring) inside the
 *	      zero zone, then more small ones: all scanned.
 *	Prints the per-commit time (one thread, fdatasync each).
 */
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "wal.h"
#include "xtc.h"
#include "t_tmp.h"

#define N 2000
#define M 500

static int g_fail;
#ifdef CK
#undef CK
#endif
#define CK(x) do { if (!(x)) { \
	fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); \
	g_fail = 1; } } while (0)

struct sc { uint64_t prev, n; int ordered; uint64_t big; };

static int
scan_cb(uint64_t lsn, const void *rec, uint32_t len, void *user)
{
	struct sc *s = user;
	(void)rec;
	if (s->n > 0 && lsn <= s->prev) s->ordered = 0;
	if (len > 1000000) s->big++;
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

int
main(void)
{
	char path[256], rec[300];
	wal_opts_t wo;
	wal_t *w = NULL;
	struct sc s;
	uint64_t lsn = 0;
	uint8_t *big;
	int64_t t0;
	int i, fd;

	t_tmpl(path, sizeof path, "wal-prealloc");
	if ((fd = mkstemp(path)) < 0) return 1;
	close(fd);
	memset(rec, 'r', sizeof rec);
	memset(&wo, 0, sizeof wo);
	wo.path = path;
	CK(wal_open(&wo, &w) == XTC_OK);
	t0 = xtc_clock_mono();
	for (i = 0; i < N; i++)
		CK(wal_commit_sync(w, rec, sizeof rec, &lsn) == XTC_OK);
	printf("  %d commits of %zu B, one thread: %.1f us each\n", N, sizeof rec,
	    (double)(xtc_clock_mono() - t0) / N / 1000.0);
	printf("  log %llu B, file %ld B\n", (unsigned long long)wal_size(w),
	    file_size(path));
	CK(file_size(path) > (long)wal_size(w));             /* (1) */
	wal_close(w);                     /* a crash: no truncation, no marker */

	memset(&s, 0, sizeof s); s.ordered = 1;
	CK(wal_scan(path, scan_cb, &s) == XTC_OK);
	CK(s.n == N && s.ordered);                            /* (2) */

	wo.append = 1;                                        /* (3) */
	CK(wal_open(&wo, &w) == XTC_OK);
	for (i = 0; i < M; i++)
		CK(wal_commit_sync(w, rec, sizeof rec, &lsn) == XTC_OK);
	CK(lsn == (uint64_t)(N + M));
	/* (4) a 3 MiB record (> ring/4, written directly), then small ones */
	big = malloc(3u << 20);
	CK(big != NULL);
	if (big != NULL) {
		memset(big, 'b', 3u << 20);
		CK(wal_commit_sync(w, big, 3u << 20, &lsn) == XTC_OK);
		free(big);
	}
	for (i = 0; i < 10; i++)
		CK(wal_commit_sync(w, rec, sizeof rec, &lsn) == XTC_OK);
	wal_close(w);
	memset(&s, 0, sizeof s); s.ordered = 1;
	CK(wal_scan(path, scan_cb, &s) == XTC_OK);
	printf("  after a crash and an append reopen: %llu records (want %d), "
	    "big %llu, ordered %d\n", (unsigned long long)s.n, N + M + 11,
	    (unsigned long long)s.big, s.ordered);
	CK(s.n == (uint64_t)(N + M + 11) && s.big == 1 && s.ordered);
	unlink(path);
	if (g_fail) { fprintf(stderr, "FAIL: test_wal_prealloc\n"); return 1; }
	printf("  ok   the log preallocates ahead and its zeros never read as log\n");
	return 0;
}
