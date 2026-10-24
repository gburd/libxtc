/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * docs/_includes/snippets/17_zero_copy_msg.c -- move a buffer between
 * processes without copying it, and frame it onto a socket from pieces.
 *
 * Part 1: a producer xtc_malloc's a 64 KiB buffer, fills it, and MOVES
 * it to the consumer with xtc_send_owned.  The consumer's xtc_recv
 * returns the very same pointer (checked below), and frees it.  No byte
 * of the payload is copied in user space.
 *
 * Part 2: xtc_net_send_framev writes one length-prefixed frame from
 * three separate pieces (a header, the body, a trailer) in one
 * sendmsg -- no assembly buffer -- and xtc_net_recv_frame reads it back
 * into a single block.
 *
 * Exits 0 when the pointer arrived unchanged and the frame round-trips.
 */

/* !region full */
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_net.h"
#include "xtc_proc.h"

#define SZ (64 * 1024)

static void *g_sent;      /* the producer's pointer, for the check */
static int   g_ok;

static void
consumer(void *arg)
{
	void *msg = NULL;
	size_t n = 0;
	(void)arg;
	if (xtc_recv(&msg, &n, -1) != XTC_OK)
		return;
	/* Same block the producer filled: it was moved, not copied. */
	g_ok = (msg == g_sent && n == SZ && ((char *)msg)[SZ - 1] == 'z');
	xtc_free(msg);                    /* the consumer owns it now */
}

static void
producer(void *arg)
{
	xtc_pid_t to = *(xtc_pid_t *)arg;
	char *buf = xtc_malloc(SZ);       /* must come from libxtc's allocator */
	if (buf == NULL)
		return;
	memset(buf, 'z', SZ);
	g_sent = buf;
	if (xtc_send_owned(to, buf, SZ) != XTC_OK)
		xtc_free(buf);                /* on ANY error it is still ours */
	/* on XTC_OK: do not touch buf again */
}

int
main(void)
{
	xtc_loop_t *loop = NULL;
	xtc_pid_t cons, prod;
	struct iovec iov[3];
	void *frame = NULL;
	size_t flen = 0;
	int sv[2], frame_ok;

	/* Part 1: zero-copy message. */
	if (xtc_loop_init(&loop) != XTC_OK)
		return 1;
	if (xtc_proc_spawn(loop, consumer, NULL, NULL, &cons) != XTC_OK ||
	    xtc_proc_spawn(loop, producer, &cons, NULL, &prod) != XTC_OK)
		return 1;
	(void)xtc_loop_run(loop);
	(void)xtc_loop_fini(loop);

	/* Part 2: one frame from three pieces, no assembly copy. */
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
		return 1;
	iov[0].iov_base = (void *)"HDR:";
	iov[0].iov_len = 4;
	iov[1].iov_base = (void *)"body bytes";
	iov[1].iov_len = 10;
	iov[2].iov_base = (void *)":END";
	iov[2].iov_len = 4;
	frame_ok = xtc_net_send_framev(sv[0], iov, 3) == XTC_OK &&
	    xtc_net_recv_frame(sv[1], &frame, &flen, 0, 1000000000LL) == XTC_OK &&
	    flen == 18 && memcmp(frame, "HDR:body bytes:END", 18) == 0;
	xtc_free(frame);
	close(sv[0]);
	close(sv[1]);

	printf("moved without a copy: %s; framev round trip: %s\n",
	    g_ok ? "yes" : "NO", frame_ok ? "yes" : "NO");
	return g_ok && frame_ok ? 0 : 1;
}
/* !endregion full */
