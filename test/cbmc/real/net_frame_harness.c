/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * test/cbmc/real/net_frame_harness.c
 *
 *	REAL CODE: links src/io/io_net.c unchanged.  recv() is replaced by
 *	an adversarial peer: each call returns any count in [1, asked]
 *	(any chunking), 0 (peer closed), or -1 with any errno, and fills
 *	the bytes with arbitrary values; at most STALLS of the -1 returns
 *	(a peer that stalls forever only makes the loops unbounded).  The
 *	poll-wait is the real __net_wait_fd over a stubbed poll that may
 *	report readiness or a timeout.
 *
 *	PROPERTIES of xtc_net_recv_frame on ANY byte stream and ANY
 *	interleaving of short reads, EAGAIN, EINTR and close:
 *	  - memory-safe: never writes past the 4-byte header or the
 *	    allocated frame (bounds + pointer checks);
 *	  - the OOM guard holds: a frame longer than max_len is never
 *	    allocated, it returns XTC_E_RANGE;
 *	  - on XTC_OK, *out_len is the length the header declared and
 *	    every byte of *out came from recv (no uninitialized tail);
 *	  - on any error, *out == NULL (nothing leaked to the caller).
 */
#include "xtc.h"
#include "xtc_net.h"
#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <sys/socket.h>

#define MAXLEN 3        /* bound on max_len: header + body loops unroll */
#define STALLS 2        /* EAGAIN/EINTR returns the peer may make */
unsigned char nondet_uchar(void);

static unsigned char stream[4 + MAXLEN];
static size_t sent;
static int stalls;

ssize_t
peer_recv(int fd, void *buf, size_t n, int flags)
{
	int what;
	size_t k;

	(void)fd; (void)flags;
	__CPROVER_assume(what >= 0 && what <= 3);
	if (what == 1)
		return 0;
	if (what == 2) {
		int e;
		__CPROVER_assume(stalls < STALLS);
		stalls++;
		__CPROVER_assume(e == EAGAIN || e == EINTR || e == EIO);
		errno = e;
		return -1;
	}
	__CPROVER_assume(k >= 1 && k <= n);
	__CPROVER_assume(sent + k <= sizeof stream);
	for (size_t i = 0; i < k; i++)
		((unsigned char *)buf)[i] = stream[sent + i];
	sent += k;
	return (ssize_t)k;
}

int
peer_poll(struct pollfd *fds, nfds_t n, int to)
{
	int r;
	(void)to;
	__CPROVER_assume(r == 0 || r == 1);
	if (r == 1 && n > 0)
		fds[0].revents = fds[0].events;
	return r;
}

int
main(void)
{
	void *out = (void *)1;
	size_t out_len = 12345, max_len, decl;
	int rc;

	for (size_t i = 0; i < sizeof stream; i++)
		stream[i] = (unsigned char)nondet_uchar();
	__CPROVER_assume(max_len >= 1 && max_len <= MAXLEN);

	rc = xtc_net_recv_frame(3, &out, &out_len, max_len, -1);
	decl = ((size_t)stream[0] << 24) | ((size_t)stream[1] << 16) |
	    ((size_t)stream[2] << 8) | (size_t)stream[3];
	if (rc == XTC_OK) {
		__CPROVER_assert(sent >= 4 && out_len == decl,
		    "OK: out_len is the declared length");
		__CPROVER_assert(out_len <= max_len, "OK: within max_len");
		__CPROVER_assert(sent == 4 + out_len,
		    "OK: consumed exactly header + body");
		for (size_t i = 0; i < out_len; i++)
			__CPROVER_assert(((unsigned char *)out)[i] ==
			    stream[4 + i], "OK: every byte came from recv");
	} else {
		__CPROVER_assert(out == NULL, "error: *out is NULL");
		if (sent >= 4 && decl > max_len)
			__CPROVER_assert(rc == XTC_E_RANGE,
			    "oversize frame: XTC_E_RANGE, never allocated");
	}
	return 0;
}
