/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_wal_tail.c
 *	The reopen scan and its shortcut (wal_scan_ex + wal_opts_t.tail).
 *
 *	A clean reopen used to scan the whole log in wal_open(append=1)
 *	with three preads and a byte-at-a-time checksum per frame (2 s on a
 *	362 MB log); a crash reopen scanned it twice.  wal_scan_ex now
 *	reports where the log ends, and wal_open trusts such a tail after
 *	checking one frame.  This test pins:
 *	  - the unrolled checksum equals byte-at-a-time FNV-1a (the on-disk
 *	    format is unchanged, so an old log still reads);
 *	  - the windowed reader hands every callback the exact body, for
 *	    frames that straddle its window and frames larger than it;
 *	  - wal_scan_ex's tail (end, last frame, max LSN), including past a
 *	    torn tail and the preallocated zeros, and valid = 0 when the
 *	    callback stops the scan or the file is missing;
 *	  - wal_open uses a correct hint WITHOUT scanning (shown with a
 *	    corrupt frame mid-log that a scan would stop at), and falls back
 *	    to the scan for a stale or wrong one (appended-after, wrong
 *	    LSN, past the file, last frame not ending at end, valid 0).
 */

#include "wal.c"

#include "t_tmp.h"

#define NREC 3000

static uint32_t
t_len(uint32_t i)
{
	if (i == 777 || i == 2900)
		return (3u << 20) + i;       /* > the reader window */
	if (i % 97 == 0)
		return 300000 + i;           /* straddles window boundaries */
	return 1 + (i * 2654435761u) % 900;
}

static uint8_t
t_byte(uint32_t i, uint32_t k)
{
	return (uint8_t)(i * 13 + k * 7 + (k >> 9));
}

struct seen {
	uint32_t n, bad, stop_at;
	uint64_t last_lsn;
};

static int
check_cb(uint64_t lsn, const void *rec, uint32_t len, void *user)
{
	struct seen *s = user;
	const uint8_t *p = rec;
	uint32_t i = s->n, k;
	if (len != t_len(i) || lsn <= s->last_lsn)
		s->bad++;
	else
		for (k = 0; k < len; k++)
			if (p[k] != t_byte(i, k)) {
				s->bad++;
				break;
			}
	s->last_lsn = lsn;
	s->n++;
	return s->stop_at != 0 && s->n == s->stop_at;
}

static uint64_t
ref_fnv(const uint8_t *p, size_t n)
{
	uint64_t h = 1469598103934665603ull;
	size_t i;
	for (i = 0; i < n; i++)
		h = (h ^ p[i]) * 1099511628211ull;
	return h;
}

/* wal_open(append) with `t`; returns the handle's end and next LSN. */
static int
reopen(const char *path, const wal_tail_t *t, uint64_t *end, uint64_t *next)
{
	wal_opts_t wo;
	wal_t *w = NULL;
	uint64_t lsn = 0;
	memset(&wo, 0, sizeof wo);
	wo.path = path;
	wo.append = 1;
	wo.tail = t;
	if (wal_open(&wo, &w) != XTC_OK)
		return -1;
	*end = wal_size(w);
	if (wal_commit_sync(w, "z", 1, &lsn) != XTC_OK)
		lsn = 0;
	*next = lsn;
	/* undo the probe commit so the file is as it was */
	if (ftruncate(w->fd, (off_t)*end) != 0)
		lsn = 0;
	wal_close(w);
	return 0;
}

int
main(void)
{
	char path[256];
	wal_opts_t wo;
	wal_t *w = NULL;
	wal_tail_t t, t2;
	struct seen s;
	uint8_t *buf, junk[64];
	uint64_t lsn = 0, last_lsn = 0, last_off = 0, end, e2, n2, bad_off;
	uint32_t i, k, l;
	int fd;

	/* 1. the unrolled checksum is FNV-1a */
	buf = malloc((3u << 20) + 4096);
	if (buf == NULL)
		return 1;
	for (k = 0; k < 5000; k++)
		buf[k] = (uint8_t)(k * 31 + 7);
	for (l = 0; l < 64; l++)
		CK(wal_fnv(WAL_FNV_BASIS, buf, l) == ref_fnv(buf, l));
	CK(wal_fnv(WAL_FNV_BASIS, buf, 4999) == ref_fnv(buf, 4999));

	/* 2. a log of NREC frames (straddling + oversized ones) */
	t_tmpl(path, sizeof path, "sqlxtc-waltail");
	if ((fd = mkstemp(path)) < 0)
		return 1;
	close(fd);
	memset(&wo, 0, sizeof wo);
	wo.path = path;
	CK(wal_open(&wo, &w) == XTC_OK);
	for (i = 0; i < NREC; i++) {
		l = t_len(i);
		for (k = 0; k < l; k++)
			buf[k] = t_byte(i, k);
		last_off = wal_size(w);
		CK(wal_commit_sync(w, buf, l, &lsn) == XTC_OK);
	}
	last_lsn = lsn;
	end = wal_size(w);
	wal_close(w);                     /* leaves the preallocated zeros */

	memset(&s, 0, sizeof s);
	CK(wal_scan_ex(path, check_cb, &s, &t) == XTC_OK);
	CK(s.n == NREC && s.bad == 0);
	CK(t.valid && t.end == end && t.last_off == last_off &&
	    t.max_lsn == last_lsn);
	printf("  scan: %u frames, %u bad bodies; tail end %llu (want %llu) "
	    "last %llu max_lsn %llu\n", s.n, s.bad, (unsigned long long)t.end,
	    (unsigned long long)end, (unsigned long long)t.last_off,
	    (unsigned long long)t.max_lsn);

	/* the callback stops the scan: no tail */
	memset(&s, 0, sizeof s);
	s.stop_at = 10;
	CK(wal_scan_ex(path, check_cb, &s, &t2) == XTC_OK && !t2.valid);
	CK(wal_scan_ex("/nonexistent/wal", check_cb, &s, &t2) == XTC_OK &&
	    !t2.valid);

	/* 3. a correct hint is trusted with no scan: corrupt a byte in the
	 *    middle of the log -- a scan stops there, the hint does not look
	 *    (wal_scan_ex shows where a scan stops without truncating). */
	bad_off = end / 2;
	fd = open(path, O_RDWR);
	CK(fd >= 0);
	CK(pread(fd, junk, 1, (off_t)bad_off) == 1);
	junk[0] ^= 0xff;
	CK(pwrite(fd, junk, 1, (off_t)bad_off) == 1);
	CK(reopen(path, &t, &e2, &n2) == 0);
	CK(e2 == end && n2 == last_lsn + 1);
	memset(&s, 0, sizeof s);
	CK(wal_scan_ex(path, check_cb, &s, &t2) == XTC_OK);
	CK(t2.valid && t2.end <= bad_off && s.n < NREC);
	printf("  hint trusted: end %llu; a scan stops at the corrupt frame, "
	    "%llu (%u frames)\n", (unsigned long long)end,
	    (unsigned long long)t2.end, s.n);
	junk[0] ^= 0xff;
	CK(pwrite(fd, junk, 1, (off_t)bad_off) == 1);   /* repair */

	/* 4. wrong hints fall back to the scan (the truth is end/last_lsn) */
	t2 = t; t2.max_lsn = last_lsn - 1;               /* wrong LSN */
	CK(reopen(path, &t2, &e2, &n2) == 0 && e2 == end && n2 == last_lsn + 1);
	t2 = t; t2.end = end + 8 * 1024 * 1024 * 100ull; /* past the file */
	CK(reopen(path, &t2, &e2, &n2) == 0 && e2 == end && n2 == last_lsn + 1);
	t2 = t; t2.last_off = last_off - 1;              /* not a frame */
	CK(reopen(path, &t2, &e2, &n2) == 0 && e2 == end && n2 == last_lsn + 1);
	t2 = t; t2.end = last_off; t2.last_off = 0;      /* stops short */
	CK(reopen(path, &t2, &e2, &n2) == 0 && e2 == end && n2 == last_lsn + 1);
	memset(&t2, 0, sizeof t2); t2.valid = 1;          /* "empty" */
	CK(reopen(path, &t2, &e2, &n2) == 0 && e2 == end && n2 == last_lsn + 1);

	/* stale: a record appended after the scan */
	memset(&wo, 0, sizeof wo);
	wo.path = path;
	wo.append = 1;
	CK(wal_open(&wo, &w) == XTC_OK);
	CK(wal_commit_sync(w, "late", 4, &lsn) == XTC_OK);
	e2 = wal_size(w);
	wal_close(w);
	CK(lsn == last_lsn + 1);
	CK(reopen(path, &t, &end, &n2) == 0 && end == e2 && n2 == lsn + 1);

	/* 5. a torn tail: the scan's tail ends before it, the hint is
	 *    accepted, and the open drops the torn bytes */
	CK(ftruncate(fd, (off_t)e2) == 0);
	memset(junk, 0x5a, sizeof junk);
	memcpy(junk, &lsn, 8);                       /* a plausible header */
	l = 40;
	memcpy(junk + 8, &l, 4);
	CK(pwrite(fd, junk, 30, (off_t)e2) == 30);
	memset(&s, 0, sizeof s);
	s.n = 0;
	CK(wal_scan_ex(path, check_cb, &s, &t) == XTC_OK);
	CK(t.valid && t.end == e2 && t.max_lsn == lsn);
	CK(reopen(path, &t, &end, &n2) == 0 && end == e2 && n2 == lsn + 1);
	CK(wal_fsize(fd) == (off_t)e2);

	/* 6. an empty log and its hint */
	CK(ftruncate(fd, 0) == 0);
	CK(wal_scan_ex(path, check_cb, &s, &t) == XTC_OK);
	CK(t.valid && t.end == 0 && t.last_off == 0 && t.max_lsn == 0);
	CK(reopen(path, &t, &end, &n2) == 0 && end == 0 && n2 == 1);
	close(fd);
	unlink(path);
	free(buf);
	if (g_fail) {
		fprintf(stderr, "FAIL: test_wal_tail\n");
		return 1;
	}
	printf("  ok   the reopen tail hint is checked in one frame and the "
	    "scan reads every body exactly\n");
	return 0;
}
