/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * test/tnt/test_tnt_zcopy.c -- zero-copy staged I/O in the tnt layer.
 *
 *	An echo Isolate keeps the reactor buffer of a RECV_COMPLETE
 *	(xtc_tnt_recv_take) and sends that same block back
 *	(xtc_tnt_io_send_owned): no copy into the Isolate.  Checked:
 *	  - the echo arrives intact over a socketpair;
 *	  - the pointer the courier sends from IS the reactor buffer (the
 *	    allocator hook records the 4 KiB reactor allocation; the send
 *	    side sees the same address via the take);
 *	  - every reactor / owned buffer is freed exactly once: the run
 *	    ends with as many frees as allocations of that size (3: the
 *	    echoed reactor buffer, the EOF recv's, and a buffer handed to
 *	    io_send_owned by a handler that then did NOT return WAIT_IO --
 *	    never committed, so the runtime frees it).
 *	Before xtc_tnt_recv_take / xtc_tnt_io_send_owned existed the
 *	reactor buffer was freed when the handler returned and io_send
 *	required an in-Isolate buffer: the echo had to memcpy every byte.
 *
 *	Plain main(), exit 0 = pass, 77 = tnt unsupported (Windows).
 */

#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "xtc.h"
#include "xtc_tnt.h"
#include "os_alloc.h"

#if defined(_WIN32)
int main(void) { return 77; }
#else

#include <sys/socket.h>
#include <unistd.h>

#define RECVBUF  4096u
#define T_ECHO   0

static struct __os_alloc_hook g_down;
static _Atomic long g_alloc, g_free;          /* RECVBUF-sized blocks */
static void *g_taken, *g_sent;
static _Atomic int g_done, g_ok, g_dropped_ok, g_take_bad;

/* Counting hook: blocks of exactly RECVBUF bytes are reactor buffers
 * (and the one test buffer below); track them by address. */
#define TRACK 64
static void *g_live[TRACK];
static _Atomic int g_live_lock;

static void
live_add(void *p)
{
	int i;
	while (atomic_exchange(&g_live_lock, 1))
		;
	for (i = 0; i < TRACK; i++)
		if (g_live[i] == NULL) { g_live[i] = p; break; }
	atomic_store(&g_live_lock, 0);
}

static int
live_del(void *p)
{
	int i, hit = 0;
	while (atomic_exchange(&g_live_lock, 1))
		;
	for (i = 0; i < TRACK; i++)
		if (g_live[i] == p) { g_live[i] = NULL; hit = 1; break; }
	atomic_store(&g_live_lock, 0);
	return hit;
}

static void *
c_malloc(size_t n)
{
	void *p = g_down.malloc(n);
	if (p != NULL && n == RECVBUF) {
		atomic_fetch_add(&g_alloc, 1);
		live_add(p);
	}
	return p;
}
static void *c_calloc(size_t k, size_t n) { return g_down.calloc(k, n); }
static void *c_realloc(void *q, size_t n) { return g_down.realloc(q, n); }
static void
c_free(void *q)
{
	if (q != NULL && live_del(q))
		atomic_fetch_add(&g_free, 1);
	g_down.free(q);
}
static void *c_aligned(size_t a, size_t n) { return g_down.aligned(a, n); }
static void c_aligned_free(void *q) { g_down.aligned_free(q); }

typedef struct echo_iso {
	int fd;
	int step;
} echo_iso_t;

#define TAG_DROP  (XTC_TNT_USER_TAG_BASE)
#define TAG_READ  (XTC_TNT_USER_TAG_BASE + 1)

static xtc_tnt_transition_t
echo_init(void *self_raw, const void *args, size_t n)
{
	echo_iso_t *self = xtc_tnt_self_as(echo_iso_t, self_raw);
	(void)n;
	memcpy(&self->fd, args, sizeof(int));
	self->step = 0;
	(void)xtc_tnt_send(xtc_tnt_self(), TAG_DROP, NULL, 0);
	return XTC_TNT_TRANSITION_WAIT_MESSAGE;
}

static xtc_tnt_transition_t
echo_handler(void *self_raw, xtc_tnt_message_t *msg)
{
	echo_iso_t *self = xtc_tnt_self_as(echo_iso_t, self_raw);
	void *b;

	switch (msg->tag) {
	case TAG_DROP:
		/* A buffer handed to io_send_owned by a turn that does NOT
		 * return WAIT_IO is never committed: the runtime frees it. */
		if ((b = xtc_malloc(RECVBUF)) != NULL &&
		    xtc_tnt_io_send_owned(self->fd, b, 8) == XTC_TNT_IO_OK)
			atomic_store(&g_dropped_ok, 1);
		(void)xtc_tnt_send(xtc_tnt_self(), TAG_READ, NULL, 0);
		return XTC_TNT_TRANSITION_WAIT_MESSAGE;
	case TAG_READ:
		(void)xtc_tnt_submit_recv(self->fd);
		return XTC_TNT_TRANSITION_WAIT_IO;
	case XTC_TNT_IO_TAG_RECV_COMPLETE:
		if (msg->body.io.result <= 0 || self->step++ > 0) {
			(void)xtc_tnt_submit_close(self->fd);   /* EOF */
			return XTC_TNT_TRANSITION_WAIT_IO;
		}
		/* Echo: keep the reactor buffer, send it back as is. */
		b = xtc_tnt_recv_take(msg);
		if (b == NULL || msg->body.io.buffer != NULL ||
		    xtc_tnt_recv_take(msg) != NULL) {
			atomic_store(&g_take_bad, 1);        /* -> FAIL */
			(void)xtc_tnt_submit_close(self->fd);
			return XTC_TNT_TRANSITION_WAIT_IO;
		}
		g_taken = b;
		if (xtc_tnt_io_send_owned(self->fd, b,
		    (size_t)msg->body.io.result) != XTC_TNT_IO_OK) {
			xtc_free(b);
			(void)xtc_tnt_submit_close(self->fd);
			return XTC_TNT_TRANSITION_WAIT_IO;
		}
		g_sent = b;
		return XTC_TNT_TRANSITION_WAIT_IO;
	case XTC_TNT_IO_TAG_SEND_COMPLETE:
		if (msg->body.io.result == 8)
			atomic_store(&g_ok, 1);
		(void)xtc_tnt_submit_recv(self->fd);   /* next: EOF */
		return XTC_TNT_TRANSITION_WAIT_IO;
	case XTC_TNT_IO_TAG_CLOSE_COMPLETE:
		atomic_store(&g_done, 1);
		xtc_tnt_stop();
		return XTC_TNT_TRANSITION_DONE;
	default:
		return XTC_TNT_TRANSITION_WAIT_MESSAGE;
	}
}

static int g_fd;

static xtc_tnt_transition_t
boot_init(void *self_raw, const void *args, size_t n)
{
	xtc_tnt_handle_t h;
	(void)self_raw; (void)args; (void)n;
	(void)xtc_tnt_spawn(T_ECHO, &g_fd, sizeof g_fd, &h);
	return XTC_TNT_TRANSITION_DONE;
}

static xtc_tnt_transition_t
boot_handler(void *self_raw, xtc_tnt_message_t *msg)
{
	(void)self_raw; (void)msg;
	return XTC_TNT_TRANSITION_DONE;
}

static const xtc_tnt_type_t types[] = {
	{ .id = T_ECHO, .name = "Echo", .slot_count = 4,
	  .stride = sizeof(echo_iso_t), .mailbox_capacity = 4,
	  .budget_weight = 4, .init_fn = echo_init,
	  .handler_fn = echo_handler },
	{ .id = 1, .name = "Boot", .slot_count = 1, .stride = 8,
	  .mailbox_capacity = 1, .budget_weight = 1, .init_fn = boot_init,
	  .handler_fn = boot_handler },
};

int
main(void)
{
	struct __os_alloc_hook h;
	xtc_tnt_spec_t spec;
	char back[16];
	int sv[2], rc, fail = 0;
	ssize_t n;

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0)
		return 1;
	if (write(sv[0], "zerocopy", 8) != 8)
		return 1;
	g_fd = sv[1];

	if (__os_alloc_get_hook(&g_down) != XTC_OK)
		return 1;
	h.malloc = c_malloc;
	h.calloc = c_calloc;
	h.realloc = c_realloc;
	h.free = c_free;
	h.aligned = c_aligned;
	h.aligned_free = c_aligned_free;
	(void)__os_alloc_set_hook(&h);

	memset(&spec, 0, sizeof spec);
	spec.name = "tnt-zcopy";
	spec.types = types;
	spec.n_types = 2;
	spec.shard_count = 1;
	spec.scratch_size = 8192;          /* not RECVBUF: the hook counts by size */
	spec.recv_buf_size = RECVBUF;
	spec.boot_type = 1;

	/* The echo's second recv sees EOF (our write side is shut) and
	 * closes, which stops the runtime. */
	if (shutdown(sv[0], SHUT_WR) != 0)
		return 1;
	rc = xtc_tnt_start(&spec);
	(void)__os_alloc_set_hook(&g_down);
	if (rc == XTC_E_NOSYS)
		return 77;
	n = read(sv[0], back, sizeof back);

	printf("start rc=%d done=%d send_ok=%d echo=%zd [%.*s]\n", rc,
	    atomic_load(&g_done), atomic_load(&g_ok), n,
	    (int)(n > 0 ? n : 0), back);
	printf("reactor/owned %u-byte blocks: alloc=%ld free=%ld; "
	    "dropped-owned staged=%d\n", RECVBUF, atomic_load(&g_alloc),
	    atomic_load(&g_free), atomic_load(&g_dropped_ok));
	printf("taken=%p sent=%p\n", g_taken, g_sent);

	if (atomic_load(&g_take_bad)) {
		printf("recv_take did not hand the reactor buffer over\n");
		fail = 1;
	}
	if (!atomic_load(&g_done) || !atomic_load(&g_ok)) fail = 1;
	if (n < 8 || memcmp(back, "zerocopy", 8) != 0) fail = 1;
	if (g_taken == NULL || g_sent != g_taken) fail = 1;
	if (!atomic_load(&g_dropped_ok)) fail = 1;
	/* the dropped buffer + 2 reactor buffers, every one freed once. */
	if (atomic_load(&g_alloc) != 3 ||
	    atomic_load(&g_alloc) != atomic_load(&g_free)) fail = 1;
	close(sv[0]);
	printf("%s\n", fail ? "FAIL" : "PASS");
	return fail;
}

#endif /* !_WIN32 */
