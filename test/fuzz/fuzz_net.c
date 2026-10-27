/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/fuzz/fuzz_net.c -- libFuzzer harness for the length-framed
 * transport (xtc_net_recv_frame, xtc_net_send_framev) and the DOWN/EXIT
 * signal decoder a received frame can reach (xtc_down_decode_ex).
 *
 *	Input: byte 0 selects the mode, the rest is the payload.
 *
 *	  mode & 1 == 0  RECV: the payload is what a peer wrote on the
 *	                 socket.  It goes into one end of a socketpair, the
 *	                 write side is shut, and recv_frame reads frames off
 *	                 the other end until it stops.  Each frame must be
 *	                 exactly the bytes the 4-byte prefix claimed, and a
 *	                 claimed length over the cap must be XTC_E_RANGE.
 *	  mode & 1 == 1  SEND: the payload is cut into up to 63 pieces (each
 *	                 cut length is one input byte), sent as ONE frame
 *	                 with send_framev and received with recv_frame: the
 *	                 frame must be the concatenation of the pieces.
 *
 *	(mode >> 1) & 3 picks recv_frame's max_len: 1 MiB, 16, 4096 or
 *	65536.  Never 0: "no cap" allocates whatever a peer claims, up to
 *	4 GiB, by contract, which only measures the allocator.
 *	mode & 8 makes the reading end non-blocking (the poll(2)
 *	off-loop wait path).  No loop is created: the off-loop path is the
 *	one a peer reaches before any fiber is involved, and it needs no
 *	scheduler, so a run is deterministic.
 *
 *	Inputs over 60 KiB are ignored so a write never blocks on a full
 *	socket buffer (the harness is single-threaded).
 */

#define _DEFAULT_SOURCE
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include "xtc.h"
#include "xtc_net.h"
#include "xtc_proc.h"

#define FZ_MAX_INPUT (60 * 1024)

static void
fz_check(int ok)
{
	if (!ok)
		abort();
}

static int
fz_pair(int sv[2])
{
	int sz = 256 * 1024;
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
		return -1;
	(void)setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &sz, sizeof sz);
	(void)setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &sz, sizeof sz);
	return 0;
}

static void
fz_write_all(int fd, const uint8_t *p, size_t n)
{
	while (n > 0) {
		ssize_t w = write(fd, p, n);
		fz_check(w > 0);
		p += w;
		n -= (size_t)w;
	}
}

static uint32_t
fz_be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	    ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void
fz_recv(const uint8_t *d, size_t n, size_t max_len, int nonblock)
{
	int sv[2];
	size_t off = 0;
	void *f;
	size_t flen;
	xtc_down_info_t di;
	int rc;

	if (fz_pair(sv) != 0)
		return;
	fz_write_all(sv[0], d, n);
	(void)shutdown(sv[0], SHUT_WR);
	if (nonblock)
		(void)xtc_net_setnonblock(sv[1]);
	for (;;) {
		f = NULL;
		flen = 0;
		rc = xtc_net_recv_frame(sv[1], &f, &flen, max_len, -1);
		if (rc != XTC_OK) {
			fz_check(f == NULL && flen == 0);
			/* A cap breach is RANGE; anything else here is a
			 * short stream (INVAL) at the end of the input. */
			if (n - off >= 4 && max_len > 0 &&
			    fz_be32(d + off) > max_len)
				fz_check(rc == XTC_E_RANGE);
			else
				fz_check(rc == XTC_E_INVAL);
			break;
		}
		fz_check(n - off >= 4);
		fz_check(flen == fz_be32(d + off));
		off += 4;
		fz_check(n - off >= flen);
		fz_check(flen == 0 ? f == NULL : f != NULL);
		if (flen > 0)
			fz_check(memcmp(f, d + off, flen) == 0);
		off += flen;
		/* A frame forwarded to a mailbox may be decoded as a
		 * DOWN/EXIT signal: it must not read past flen. */
		if (f != NULL) {
			di = (xtc_down_info_t)XTC_DOWN_INFO_INIT;
			(void)xtc_down_decode_ex(f, flen, &di);
		}
		xtc_free(f);
	}
	(void)close(sv[0]);
	(void)close(sv[1]);
}

static void
fz_send(const uint8_t *d, size_t n, size_t max_len, int nonblock)
{
	struct iovec iov[63];
	int sv[2], cnt = 0, rc;
	size_t at = 0, total = 0, piece;
	void *f = NULL;
	size_t flen = 0;
	uint8_t *want;

	/* Each piece: one length byte, then up to that many bytes. */
	while (at < n && cnt < 63) {
		piece = d[at++];
		if (piece > n - at)
			piece = n - at;
		iov[cnt].iov_base = (void *)(uintptr_t)(d + at);
		iov[cnt].iov_len = piece;
		cnt++;
		at += piece;
		total += piece;
	}
	if (fz_pair(sv) != 0)
		return;
	rc = xtc_net_send_framev(sv[0], iov, cnt);
	fz_check(rc == XTC_OK);
	(void)shutdown(sv[0], SHUT_WR);
	if (nonblock)
		(void)xtc_net_setnonblock(sv[1]);
	rc = xtc_net_recv_frame(sv[1], &f, &flen, max_len, -1);
	if (max_len > 0 && total > max_len) {
		fz_check(rc == XTC_E_RANGE && f == NULL);
	} else {
		fz_check(rc == XTC_OK && flen == total);
		want = malloc(total ? total : 1);
		fz_check(want != NULL);
		for (at = 0, rc = 0; rc < cnt; rc++) {
			if (iov[rc].iov_len > 0)
				memcpy(want + at, iov[rc].iov_base,
				    iov[rc].iov_len);
			at += iov[rc].iov_len;
		}
		fz_check(total == 0 || memcmp(f, want, total) == 0);
		free(want);
		xtc_free(f);
		/* Nothing may follow the one frame. */
		f = NULL;
		rc = xtc_net_recv_frame(sv[1], &f, &flen, max_len, -1);
		fz_check(rc == XTC_E_INVAL && f == NULL);
	}
	(void)close(sv[0]);
	(void)close(sv[1]);
}

int LLVMFuzzerTestOneInput(const uint8_t *d, size_t n);

int
LLVMFuzzerTestOneInput(const uint8_t *d, size_t n)
{
	static const size_t caps[4] = { 1u << 20, 16, 4096, 65536 };
	uint8_t mode;

	if (n < 1 || n > FZ_MAX_INPUT)
		return 0;
	mode = d[0];
	if (mode & 1)
		fz_send(d + 1, n - 1, caps[(mode >> 1) & 3], mode & 8);
	else
		fz_recv(d + 1, n - 1, caps[(mode >> 1) & 3], mode & 8);
	return 0;
}
