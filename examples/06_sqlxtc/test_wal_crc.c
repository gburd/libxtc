/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/test_wal_crc.c
 *	The frame checksum is CRC32C, and logs written with the FNV-1a
 *	trailer still read.
 *
 *	  1. wal_c32c is CRC32C: the standard check value ("123456789" ->
 *	     0xe3069283) and the RFC 3720 vectors, on the hardware path and
 *	     the table path, for every length and alignment up to 64 and
 *	     continued across split points (the two paths agree).
 *	  2. a new frame's trailer is the tagged CRC32C of its header and
 *	     body (read back from the file).
 *	  3. an old log -- frames built here with the FNV-1a trailer, as
 *	     every build before this one wrote them, including a frame larger
 *	     than the scan window -- scans completely, reopens at its end,
 *	     and takes appends: the mixed log (FNV frames, then CRC32C ones)
 *	     scans completely in order.
 *	  4. a corrupt body byte or trailer in a CRC32C frame ends the scan
 *	     there, as it does for an FNV frame.
 *	  5. the online checkpoint's copy of a mixed log (it re-verifies
 *	     every frame it copies) scans completely.
 *
 *	With the FNV-only reader (before this change) 1 and 2 fail; with a
 *	CRC32C-only reader 3 and 5 fail.
 *
 *	Built twice: test_wal_crc (hardware CRC32C where the host has it)
 *	and test_wal_crc_sw (-DWAL_C32C_HW=0: the table path only).
 */

#include "wal.c"

#include "t_tmp.h"

#define TAG  0x43524332u              /* wal.c's WAL_SUM_TAG, "2CRC" */
#define NOLD 500
#define NNEW 300
#define BIG  ((3u << 20) + 17)       /* > the reader window */

static uint32_t
t_len(uint32_t i)
{
	return i == 123 ? BIG : 1 + (i * 2654435761u) % 700;
}

static uint8_t
t_byte(uint32_t i, uint32_t k)
{
	return (uint8_t)(i * 29 + k * 3 + (k >> 7));
}

/* The bitwise CRC32C, independent of the code under test. */
static uint32_t
ref_c32c(const uint8_t *p, size_t n)
{
	uint32_t c = ~0u;
	int k;
	while (n-- > 0) {
		c ^= *p++;
		for (k = 0; k < 8; k++)
			c = (c >> 1) ^ (c & 1 ? 0x82f63b78u : 0);
	}
	return ~c;
}

static uint64_t
ref_fnv(const uint8_t *p, size_t n)
{
	uint64_t h = 1469598103934665603ull;
	while (n-- > 0)
		h = (h ^ *p++) * 1099511628211ull;
	return h;
}

struct seen {
	uint32_t n, bad;
	uint64_t last_lsn;
};

/* Frames 0..NOLD-1 are the old ones, then the new ones, payload by index. */
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
	return 0;
}

/* Write frame i with lsn at off the way the FNV-1a builds did. */
static off_t
put_old(int fd, off_t off, uint64_t lsn, uint32_t i, uint8_t *buf)
{
	uint32_t len = t_len(i), k;
	uint64_t h;
	memcpy(buf, &lsn, 8);
	memcpy(buf + 8, &len, 4);
	for (k = 0; k < len; k++)
		buf[12 + k] = t_byte(i, k);
	h = ref_fnv(buf, 12 + (size_t)len);
	memcpy(buf + 12 + len, &h, 8);
	if (pwrite(fd, buf, 20 + (size_t)len, off) != (ssize_t)(20 + len))
		return -1;
	return off + 20 + (off_t)len;
}

int
main(void)
{
	static const uint8_t v32[32] = { 0 };
	char path[256];
	wal_opts_t wo;
	wal_t *w = NULL;
	wal_ock_t *ok = NULL;
	wal_tail_t t;
	struct seen s;
	uint8_t *buf, tmp[200], trl[8];
	uint64_t lsn = 0, crc, last_off = 0;
	uint32_t i, k, l, a, b;
	off_t off = 0, end_old;
	int fd;

	buf = malloc(BIG + 64);
	if (buf == NULL)
		return 1;

	/* 1. CRC32C, both paths (compiled out against a wal.c without it,
	 *    so the test also builds -- and fails at 2 -- before the change) */
#ifdef WAL_SUM_TAG
	(void)pthread_once(&wal_c32c_once, wal_c32c_init);
	printf("  crc32c path: %s\n", wal_c32c_hw ? "hardware" : "table");
	CK(WAL_SUM_TAG == TAG);
	CK(~wal_c32c(~0u, (const uint8_t *)"123456789", 9) == 0xe3069283u);
	CK(~wal_c32c_sw(~0u, (const uint8_t *)"123456789", 9) == 0xe3069283u);
	CK(~wal_c32c(~0u, v32, 32) == 0x8a9136aau);           /* RFC 3720 */
	memset(tmp, 0xff, 32);
	CK(~wal_c32c(~0u, tmp, 32) == 0x62a8ab43u);
	for (k = 0; k < 32; k++)
		tmp[k] = (uint8_t)k;
	CK(~wal_c32c(~0u, tmp, 32) == 0x46dd794eu);
	for (k = 0; k < sizeof tmp; k++)
		tmp[k] = (uint8_t)(k * 37 + 11);
	for (a = 0; a < 8; a++)
		for (l = 0; l + a <= 64; l++) {
			uint32_t want = ref_c32c(tmp + a, l);
			CK(~wal_c32c(~0u, tmp + a, l) == want);
			CK(~wal_c32c_sw(~0u, tmp + a, l) == want);
			for (b = 0; b <= l; b += 3)
				CK(~wal_c32c(wal_c32c(~0u, tmp + a, b), tmp + a + b,
				    l - b) == want);
		}
#else
	(void)v32; (void)a; (void)b;
	printf("  (wal.c has no CRC32C)\n");
#endif

	/* 3. an old (FNV-1a) log: NOLD frames, one of them > the window */
	t_tmpl(path, sizeof path, "sqlxtc-walcrc");
	if ((fd = mkstemp(path)) < 0)
		return 1;
	for (i = 0; i < NOLD; i++) {
		last_off = (uint64_t)off;
		CK((off = put_old(fd, off, 1000 + i, i, buf)) > 0);
	}
	end_old = off;
	memset(&s, 0, sizeof s);
	CK(wal_scan_ex(path, check_cb, &s, &t) == XTC_OK);
	CK(s.n == NOLD && s.bad == 0);
	CK(t.valid && t.end == (uint64_t)end_old && t.last_off == last_off &&
	    t.max_lsn == 1000 + NOLD - 1);
	printf("  old log: %u FNV-1a frames scanned, %u bad\n", s.n, s.bad);

	/* appends after it, reopened with no hint (a full tail scan) */
	memset(&wo, 0, sizeof wo);
	wo.path = path;
	wo.append = 1;
	CK(wal_open(&wo, &w) == XTC_OK);
	CK(w != NULL && wal_size(w) == (uint64_t)end_old);
	for (i = NOLD; i < NOLD + NNEW; i++) {
		l = t_len(i);
		for (k = 0; k < l; k++)
			buf[k] = t_byte(i, k);
		CK(wal_commit_sync(w, buf, l, &lsn) == XTC_OK);
	}
	CK(lsn == 1000 + NOLD + NNEW - 1);

	/* 2. the new frames carry the tagged CRC32C */
	CK(pread(fd, tmp, 12, end_old) == 12);
	memcpy(&l, tmp + 8, 4);
	CK(l == t_len(NOLD));
	CK(pread(fd, buf, 12 + (size_t)l, end_old) == (ssize_t)(12 + l));
	CK(pread(fd, trl, 8, end_old + 12 + (off_t)l) == 8);
	memcpy(&crc, trl, 8);
	CK(crc == (((uint64_t)TAG << 32) | ref_c32c(buf, 12 + (size_t)l)));
	CK(crc != ref_fnv(buf, 12 + (size_t)l));

	memset(&s, 0, sizeof s);
	CK(wal_scan_ex(path, check_cb, &s, &t) == XTC_OK);
	CK(s.n == NOLD + NNEW && s.bad == 0);
	CK(t.valid && t.end == wal_size(w) && t.max_lsn == lsn);
	printf("  mixed log: %u frames (%u FNV-1a, %u CRC32C), %u bad\n", s.n,
	    NOLD, NNEW, s.bad);

	/* 5. the online checkpoint re-verifies (and re-frames) every frame
	 *    it copies: copy the whole mixed log (the copy start rewound to
	 *    0 -- this file includes wal.c) with an empty dump */
	CK(wal_ockpt_begin(w, path, &ok) == XTC_OK);
	if (ok != NULL) {
		ok->src_off = 0;
		CK(wal_ockpt_finish(ok) == XTC_OK);
	}
	wal_close(w);
	memset(&s, 0, sizeof s);
	CK(wal_scan_ex(path, check_cb, &s, &t) == XTC_OK);
	CK(s.n == NOLD + NNEW && s.bad == 0 && t.valid);
	printf("  online checkpoint copy of the mixed log: %u frames, %u bad\n",
	    s.n, s.bad);

	/* 4. corruption ends the scan, CRC32C frames as FNV ones */
	close(fd);
	unlink(path);
	t_tmpl(path, sizeof path, "sqlxtc-walcrc");
	if ((fd = mkstemp(path)) < 0)
		return 1;
	close(fd);
	memset(&wo, 0, sizeof wo);
	wo.path = path;
	CK(wal_open(&wo, &w) == XTC_OK);
	for (i = 0; i < 50; i++) {
		l = t_len(i);
		for (k = 0; k < l; k++)
			buf[k] = t_byte(i, k);
		if (i == 30)
			last_off = wal_size(w);
		CK(wal_commit_sync(w, buf, l, &lsn) == XTC_OK);
	}
	fd = wal_fd(w);
	CK(pread(fd, tmp, 1, (off_t)last_off + 15) == 1);
	tmp[0] ^= 0x01;                   /* a body bit of frame 30 */
	CK(pwrite(fd, tmp, 1, (off_t)last_off + 15) == 1);
	memset(&s, 0, sizeof s);
	CK(wal_scan_ex(path, check_cb, &s, &t) == XTC_OK);
	CK(s.n == 30 && s.bad == 0 && t.valid && t.end == last_off);
	tmp[0] ^= 0x01;
	CK(pwrite(fd, tmp, 1, (off_t)last_off + 15) == 1);
	l = t_len(30);
	CK(pread(fd, trl, 8, (off_t)last_off + 12 + (off_t)l) == 8);
	trl[7] ^= 0x80;                   /* the tag half of the trailer */
	CK(pwrite(fd, trl, 8, (off_t)last_off + 12 + (off_t)l) == 8);
	memset(&s, 0, sizeof s);
	CK(wal_scan_ex(path, check_cb, &s, &t) == XTC_OK);
	CK(s.n == 30 && t.end == last_off);
	trl[7] ^= 0x80;
	trl[0] ^= 0x01;                   /* the CRC half */
	CK(pwrite(fd, trl, 8, (off_t)last_off + 12 + (off_t)l) == 8);
	memset(&s, 0, sizeof s);
	CK(wal_scan_ex(path, check_cb, &s, &t) == XTC_OK);
	CK(s.n == 30 && t.end == last_off);
	printf("  a flipped body bit, tag bit or CRC bit ends the scan at the "
	    "frame (%u frames kept)\n", s.n);
	wal_close(w);
	unlink(path);
	free(buf);
	if (g_fail) {
		fprintf(stderr, "FAIL: test_wal_crc\n");
		return 1;
	}
	printf("  ok   new frames are CRC32C; FNV-1a logs and mixed logs read "
	    "completely\n");
	return 0;
}
