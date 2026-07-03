/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_steal_torn.c
 *	Torn-tail safety of crash recovery (Increment 1 of M_SQLXTC_STEAL.md).
 *
 *	A crash mid-append leaves a partially written trailing record.  A
 *	prior STEAL probe showed that when recovery TRUSTED the length/id
 *	fields of such a torn record it drove an unbounded allocation
 *	(multi-GB balloon -> OOM-killed the box).  The root fix is a
 *	per-record checksum (wal.c: [lsn][len][body][crc] with an FNV-1a
 *	trailer): wal_scan / wal_scan_tail recompute it and treat a
 *	MISMATCH as end-of-log, so a torn record is DROPPED before it ever
 *	reaches xl_parse_* and no length/id from a torn record is ever
 *	trusted.
 *
 *	This test builds a log of several committed transactions, then
 *	torns the tail three different ways, and for EACH corruption runs
 *	both recovery paths (xstore_recover -- the safe logical rebuild --
 *	and xstore_recover_inplace -- the in-place path that trusts the
 *	base) asserting:
 *	  (a) recovery COMPLETES without ballooning memory or crashing;
 *	  (b) every COMPLETE (checksum-valid) committed transaction before
 *	      the torn point is recovered;
 *	  (c) the torn tail is cleanly excluded (its would-be row is
 *	      absent -- no garbage row leaks in).
 *
 *	MANDATORY SAFETY: the process caps its own address space with
 *	setrlimit(RLIMIT_AS, 256 MB) BEFORE any recovery runs, so a
 *	regression that reintroduces the unbounded allocation fails a
 *	malloc (recovery returns/absent-row asserts fire) instead of
 *	OOM-killing the machine.  A prior probe OOM'd the box; this cap is
 *	the guard that makes the regression fail cleanly.
 *
 *	Plain asserts; no loop; the WAL is driven with wal_commit_sync.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/resource.h>

#include "bufmgr.h"
#include "btree.h"
#include "xstore.h"
#include "wal.h"
#include "xlog.h"
#include "xtc.h"
#include "t_tmp.h"

#define PAGE_SZ  4096
#define AS_CAP   (256u * 1024u * 1024u)   /* 256 MB address-space cap */
#define N_GOOD   6                        /* committed txns before the tear */

/* On-disk per-record framing (must match wal.c). */
#define REC_HDR  12u                      /* u64 lsn + u32 len */
#define REC_CRC  8u                       /* u64 checksum trailer */

/* Replicates xstore.c's enc_vkey: (tableid:4 BE)(rowid^signbit:8 BE)(~ts:8 BE). */
static void
vkey(uint32_t tableid, int64_t rowid, uint64_t ts, uint8_t out[20])
{
	uint64_t r = (uint64_t)rowid ^ 0x8000000000000000ull;
	uint64_t t = ~ts;
	int i;
	for (i = 3; i >= 0; i--) { out[i] = (uint8_t)(tableid & 0xFF); tableid >>= 8; }
	for (i = 7; i >= 0; i--) { out[4 + i] = (uint8_t)(r & 0xFF); r >>= 8; }
	for (i = 7; i >= 0; i--) { out[12 + i] = (uint8_t)(t & 0xFF); t >>= 8; }
}

/* Append one XL_UPDATE for (tableid,rowid,ts,val) to p; returns new cursor. */
static uint8_t *
put_update(uint8_t *p, size_t cap, uint64_t txn, uint32_t tableid,
    int64_t rowid, uint64_t ts, const char *val)
{
	xl_hdr_t h;
	xl_body_t b;
	int n;
	h.type = XL_UPDATE; h.txn_id = txn; h.prev_lsn = 0;
	memset(&b, 0, sizeof b);
	b.tableid = tableid; b.rowid = rowid; b.commit_ts = ts;
	b.redo = val; b.redo_len = (uint16_t)(strlen(val) + 1);
	n = xl_enc_update(p, cap, &h, &b);
	return n < 0 ? NULL : p + n;
}

/* Build a fresh log of N_GOOD committed one-row transactions, each an
 * atomic frame [XL_UPDATE][XL_COMMIT].  Rows are (tableid=1, rowid=i,
 * ts=100+i, "row-i").  Returns the on-disk size after the last record. */
static off_t
build_good_log(const char *logp)
{
	wal_t *wal = NULL;
	wal_opts_t wo;
	uint64_t lsn;
	int i;
	off_t sz = 0;

	memset(&wo, 0, sizeof wo);
	wo.path = logp;                  /* append=0: fresh log */
	CK(wal_open(&wo, &wal) == XTC_OK);
	for (i = 0; i < N_GOOD; i++) {
		uint8_t fr[128], *p;
		char val[16];
		snprintf(val, sizeof val, "row-%d", i);
		p = put_update(fr, sizeof fr, (uint64_t)(10 + i), 1, i,
		    (uint64_t)(100 + i), val);
		CK(p != NULL);
		{
			xl_hdr_t h; int n;
			h.type = XL_COMMIT; h.txn_id = (uint64_t)(10 + i);
			h.prev_lsn = 0;
			n = xl_enc_simple(p, sizeof fr - (size_t)(p - fr), &h);
			CK(n > 0); p += n;
		}
		CK(wal_commit_sync(wal, fr, (uint32_t)(p - fr), &lsn) == XTC_OK);
	}
	wal_close(wal);
	{ int fd = open(logp, O_RDONLY); if (fd >= 0) { sz = lseek(fd, 0, SEEK_END); close(fd); } }
	return sz;
}

/* Append a plausible-but-WRONG partial record to the tail: a valid
 * header claiming a modest len, a full body, but a CORRUPTED trailer
 * (checksum will not match).  Without the checksum this record would be
 * decoded as a good XL_UPDATE (row 999) and leak in. */
static void
tear_bad_trailer(const char *logp)
{
	uint8_t fr[128], *p;
	uint32_t len;
	uint64_t lsn = 9999, badcrc = 0xDEADBEEFCAFEBABEull;
	int fd;

	p = put_update(fr, sizeof fr, 20, 1, 999, 500, "leak-me");
	CK(p != NULL);
	len = (uint32_t)(p - fr);
	fd = open(logp, O_WRONLY | O_APPEND);
	CK(fd >= 0);
	if (fd >= 0) {
		uint8_t hdr[REC_HDR];
		memcpy(hdr, &lsn, 8); memcpy(hdr + 8, &len, 4);
		CK(write(fd, hdr, REC_HDR) == (ssize_t)REC_HDR);
		CK(write(fd, fr, len) == (ssize_t)len);
		/* wrong trailer: the writer never finished (or a bit flipped) */
		CK(write(fd, &badcrc, REC_CRC) == (ssize_t)REC_CRC);
		close(fd);
	}
}

/* Append a torn record MISSING its trailer entirely (crash between the
 * body write and the trailer write): header + full body, no crc. */
static void
tear_missing_trailer(const char *logp)
{
	uint8_t fr[128], *p;
	uint32_t len;
	uint64_t lsn = 9999;
	int fd;

	p = put_update(fr, sizeof fr, 20, 1, 999, 500, "leak-me");
	CK(p != NULL);
	len = (uint32_t)(p - fr);
	fd = open(logp, O_WRONLY | O_APPEND);
	CK(fd >= 0);
	if (fd >= 0) {
		uint8_t hdr[REC_HDR];
		memcpy(hdr, &lsn, 8); memcpy(hdr + 8, &len, 4);
		CK(write(fd, hdr, REC_HDR) == (ssize_t)REC_HDR);
		CK(write(fd, fr, len) == (ssize_t)len);
		/* no trailer written: torn mid-record */
		close(fd);
	}
}

/* Flip a byte in the middle of the LAST complete record's body, without
 * touching its trailer: the recorded checksum no longer matches, so the
 * scan must drop this (now torn) tail record -- and everything after it
 * (there is nothing after, it is the tail). */
static void
tear_flip_last_body(const char *logp, off_t good_sz)
{
	int fd = open(logp, O_RDWR);
	uint8_t b;
	/* The last frame is REC_HDR + body + REC_CRC.  Flip a body byte:
	 * a few bytes in from the header, safely inside the body. */
	off_t at = good_sz - (off_t)REC_CRC - 4;
	CK(fd >= 0);
	if (fd >= 0) {
		CK(pread(fd, &b, 1, at) == 1);
		b ^= 0xFF;
		CK(pwrite(fd, &b, 1, at) == 1);
		close(fd);
	}
}

/* Recover `logp` two ways and assert (a) both complete, (b) all N_GOOD
 * committed rows present, (c) the torn tail's row (rowid 999) absent.
 * `expect_good` is how many of the leading committed rows must survive
 * (N_GOOD when the tear is a pure append; N_GOOD-1 when we corrupt the
 * last good record's body, since that record becomes the torn tail). */
static void
recover_and_check(const char *logp, const char *tag, int expect_good)
{
	char btp[256];
	int fd;
	int mode;

	/* Both recovery paths must be torn-safe: 0 = logical rebuild
	 * (xstore_recover), 1 = in-place (xstore_recover_inplace). */
	for (mode = 0; mode <= 1; mode++) {
		bm_opts_t bo = BM_OPTS_DEFAULT;
		bm_t *bm = NULL;
		bt_t *bt = NULL;
		uint8_t k[20], buf[64];
		uint16_t vl;
		int i;

		t_tmpl(btp, sizeof btp, "steal-torn-bt");
		if ((fd = mkstemp(btp)) >= 0) close(fd);

		bo.path = btp; bo.page_size = PAGE_SZ; bo.n_frames = 64;
		if (mode == 1) bo.lsn_off = 0;   /* in-place needs an ARIES page LSN */
		CK(bm_create(&bo, &bm) == XTC_OK);
		CK(bt_open(bm, &bt) == XTC_OK);

		/* (a) completes without ballooning: under the RLIMIT_AS cap a
		 * regression that reallocs on a torn length would fail here
		 * (XTC_E_NOMEM) rather than OOM the box.  The checksum fix means
		 * the torn tail is dropped before any such alloc is attempted. */
		if (mode == 0)
			CK(xstore_recover(bt, logp) == XTC_OK);
		else
			CK(xstore_recover_inplace(bt, bm, logp, NULL) == XTC_OK);

		/* (b) every complete committed row before the tear survives. */
		for (i = 0; i < expect_good; i++) {
			char want[16];
			snprintf(want, sizeof want, "row-%d", i);
			vkey(1, i, (uint64_t)(100 + i), k);
			CK(bt_lookup(bt, k, 20, buf, sizeof buf, &vl) == XTC_OK);
			CK(vl == (uint16_t)(strlen(want) + 2));   /* flags byte + str + NUL */
			CK(buf[0] == 0 && memcmp(buf + 1, want, strlen(want) + 1) == 0);
		}

		/* (c) the torn tail's would-be row (rowid 999, ts 500) never
		 * leaked in -- a torn record was excluded before decode. */
		vkey(1, 999, 500, k);
		CK(bt_lookup(bt, k, 20, buf, sizeof buf, &vl) == XTC_E_NOTFOUND);

		/* If the last good record was the corrupted one, its row must
		 * also be gone (it became the torn tail). */
		if (expect_good < N_GOOD) {
			char want[16];
			snprintf(want, sizeof want, "row-%d", N_GOOD - 1);
			vkey(1, N_GOOD - 1, (uint64_t)(100 + N_GOOD - 1), k);
			CK(bt_lookup(bt, k, 20, buf, sizeof buf, &vl) == XTC_E_NOTFOUND);
		}

		bt_close(bt);
		bm_destroy(bm);
		unlink(btp);
		{ char s[280]; snprintf(s, sizeof s, "%s.dwb", btp); unlink(s); }
		printf("  ok   %s: %s recovered %d committed rows, torn tail excluded\n",
		    tag, mode == 0 ? "logical" : "in-place", expect_good);
	}
}

int
main(void)
{
	char logp[256];
	off_t good_sz;
	int fd;
	struct rlimit rl;

	/* MANDATORY: cap our own address space so a regression that
	 * reintroduces the unbounded torn-record allocation fails cleanly
	 * (malloc returns NULL) instead of OOM-killing the machine.  Set
	 * BEFORE any recovery runs.  Skip only if the current soft cap is
	 * already tighter.
	 *
	 * Exception: AddressSanitizer reserves huge virtual mappings for its
	 * shadow memory, which a 256 MB RLIMIT_AS denies ("Failed to mmap").
	 * Under ASan the cap is unnecessary anyway -- ASan itself catches the
	 * allocation bug -- so leave RLIMIT_AS alone in that build. */
#if !defined(__SANITIZE_ADDRESS__) && \
    !(defined(__has_feature) && __has_feature(address_sanitizer))
	if (getrlimit(RLIMIT_AS, &rl) == 0) {
		if (rl.rlim_cur == RLIM_INFINITY || rl.rlim_cur > (rlim_t)AS_CAP) {
			rl.rlim_cur = (rlim_t)AS_CAP;
			if (rl.rlim_max != RLIM_INFINITY && rl.rlim_max < (rlim_t)AS_CAP)
				rl.rlim_cur = rl.rlim_max;
			(void)setrlimit(RLIMIT_AS, &rl);
		}
	}
#else
	(void)rl;
#endif

	t_tmpl(logp, sizeof logp, "steal-torn-wal");
	if ((fd = mkstemp(logp)) >= 0) close(fd);

	/* ---- tear 1: a plausible-but-wrong appended record (bad trailer) ----
	 * The header/len look fine and the body is a valid XL_UPDATE, so a
	 * checksum-less scan would decode it and leak row 999.  The wrong
	 * trailer makes the scan drop it. */
	good_sz = build_good_log(logp);
	CK(good_sz > 0);
	tear_bad_trailer(logp);
	recover_and_check(logp, "bad-trailer append", N_GOOD);
	unlink(logp);

	/* ---- tear 2: a record torn BEFORE its trailer (crash mid-record) ---- */
	if ((fd = mkstemp(logp)) >= 0) close(fd);
	good_sz = build_good_log(logp);
	CK(good_sz > 0);
	tear_missing_trailer(logp);
	recover_and_check(logp, "missing-trailer torn", N_GOOD);
	unlink(logp);

	/* ---- tear 3: a bit flipped in the LAST good record's body ----
	 * That record's stored checksum no longer matches, so it is dropped
	 * as a torn tail: only the first N_GOOD-1 rows survive. */
	if ((fd = mkstemp(logp)) >= 0) close(fd);
	good_sz = build_good_log(logp);
	CK(good_sz > 0);
	tear_flip_last_body(logp, good_sz);
	recover_and_check(logp, "flipped last body", N_GOOD - 1);
	unlink(logp);

	if (g_fail) { fprintf(stderr, "test_steal_torn: FAILED\n"); return 1; }
	printf("All sqlxtc torn-tail recovery tests passed "
	    "(both recovery paths torn-safe under a 256 MB RLIMIT_AS cap).\n");
	return 0;
}
