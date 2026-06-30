/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * examples/08_tnt/echo.c
 *	The canonical tnt example: a TCP echo server, mirroring Tina's
 *	README echo (github.com/pmbanugo/tina).
 *
 *	Each connection is its own Isolate -- no shared socket table, no
 *	lock.  echo_init sets up + submits a recv; echo_handler handles
 *	RECV_COMPLETE -> send, SEND_COMPLETE -> recv, CLOSE_COMPLETE ->
 *	done.  A listener proc accepts and spawns one EchoConnection
 *	Isolate per accepted connection (round-robin across shards).
 *
 *	Compare the Odin original:
 *
 *	    echo_init :: proc(self_raw, args, ctx) -> Effect {
 *	        self := tina.self_as(EchoConnection, self_raw, ctx)
 *	        conn := tina.payload_as(ConnectionArgs, args)
 *	        self.fd = conn.client_fd
 *	        return tina.Effect_Io{operation = IoOp_Recv{fd = self.fd}}
 *	    }
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "xtc_tnt.h"

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_net.h"
#include "xtc_proc.h"

/* ---- The EchoConnection Isolate ----------------------------------- */

#define ECHO_TYPE 0
#define ECHO_BUFSZ 256

/* The Isolate's state -- a typed struct living in a dense arena.  The
 * buffer must live inside the struct so a staged send reads stable
 * memory at commit time. */
typedef struct echo_conn {
	int     fd;
	uint8_t buffer[ECHO_BUFSZ];
	int     bytes;
} echo_conn_t;

/* Init args: the accepted client fd. */
typedef struct echo_args {
	int client_fd;
} echo_args_t;

/* Initialize: record the fd and submit the first recv. */
static xtc_tnt_transition_t
echo_init(void *self_raw, const void *args, size_t args_size)
{
	echo_conn_t *self = xtc_tnt_self_as(echo_conn_t, self_raw);
	const echo_args_t *a = args;

	(void)args_size;
	self->fd = a->client_fd;
	self->bytes = 0;

	if (xtc_tnt_submit_recv(self->fd) != XTC_TNT_IO_OK)
		return xtc_tnt_transition_to_crash(XTC_TNT_FAULT_CONTRACT_VIOLATION);
	return XTC_TNT_TRANSITION_WAIT_IO;
}

/* Handle I/O completions synchronously.  No callbacks, no hidden
 * queues.  If anything fails: let it crash (close + done). */
static xtc_tnt_transition_t
echo_handler(void *self_raw, xtc_tnt_message_t *msg)
{
	echo_conn_t *self = xtc_tnt_self_as(echo_conn_t, self_raw);

	switch (msg->tag) {
	case XTC_TNT_IO_TAG_RECV_COMPLETE:
		if (msg->body.io.result <= 0) {
			/* peer closed or error -> close */
			(void)xtc_tnt_submit_close(self->fd);
			return XTC_TNT_TRANSITION_WAIT_IO;
		}
		/* copy the received bytes into our stable buffer, then send */
		self->bytes = msg->body.io.result;
		if (self->bytes > ECHO_BUFSZ)
			self->bytes = ECHO_BUFSZ;
		memcpy(self->buffer, msg->body.io.buffer, self->bytes);
		if (xtc_tnt_io_send(self->fd, self->buffer, self->bytes)
		    != XTC_TNT_IO_OK)
			return xtc_tnt_transition_to_crash(
			    XTC_TNT_FAULT_CONTRACT_VIOLATION);
		return XTC_TNT_TRANSITION_WAIT_IO;

	case XTC_TNT_IO_TAG_SEND_COMPLETE:
		if (msg->body.io.result < 0) {
			(void)xtc_tnt_submit_close(self->fd);
			return XTC_TNT_TRANSITION_WAIT_IO;
		}
		/* echoed; wait for the next chunk */
		(void)xtc_tnt_submit_recv(self->fd);
		return XTC_TNT_TRANSITION_WAIT_IO;

	case XTC_TNT_IO_TAG_CLOSE_COMPLETE:
		return XTC_TNT_TRANSITION_DONE;

	default:
		return XTC_TNT_TRANSITION_WAIT_MESSAGE;
	}
}

/* ---- Listener (a plain proc, not an Isolate) ---------------------- */

static int            g_listen_fd = -1;
static int            g_port = 0;
static int            g_shards = 1;
static atomic_int     g_next_shard;
static atomic_int     g_shutdown;

static void
listener_proc(void *arg)
{
	(void)arg;

	while (!atomic_load(&g_shutdown)) {
		for (;;) {
			struct sockaddr_in addr;
			socklen_t alen = sizeof(addr);
			int fd = accept(g_listen_fd, (struct sockaddr *)&addr,
			    &alen);
			echo_args_t ea;
			uint8_t shard;

			if (fd < 0) {
				if (errno == EAGAIN || errno == EWOULDBLOCK)
					break;
				if (errno == EINTR)
					continue;
				break;
			}
			{
				xtc_tcp_opts_t t = { .nodelay = 1 };
				(void)xtc_net_apply_tcp_opts(fd, &t);
				xtc_net_setnonblock(fd);
			}
			ea.client_fd = fd;
			shard = (uint8_t)(atomic_fetch_add(&g_next_shard, 1) %
			    g_shards);
			if (xtc_tnt_spawn_on(shard, ECHO_TYPE, &ea, sizeof(ea))
			    != XTC_TNT_SPAWN_OK) {
				close(fd);
			}
		}

		{
			uint32_t revents = 0;
			(void)xtc_proc_wait_fd(g_listen_fd, XTC_IO_READABLE,
			    100LL * 1000 * 1000, &revents);
		}
	}
}

/* ---- Spec --------------------------------------------------------- */

static const xtc_tnt_type_t echo_types[] = {
	{
		.id = ECHO_TYPE,
		.name = "EchoConnection",
		.slot_count = 4096,
		.stride = sizeof(echo_conn_t),
		.working_memory_size = 0,
		.mailbox_capacity = 16,
		.budget_weight = 256,
		.init_fn = echo_init,
		.handler_fn = echo_handler,
	},
};

static void
on_signal(int sig)
{
	(void)sig;
	atomic_store(&g_shutdown, 1);
	xtc_tnt_stop();
}

/* The listener runs on a raw loop spawned alongside the shards.  We
 * use a dedicated proc on shard 0's loop via a bootstrap task is
 * overkill; instead we set up the listener after the runtime is up by
 * spawning it from a side thread that calls into the running loops.
 * For this slice the listener is spawned as a plain proc on a separate
 * standalone loop driven by its own thread. */

#include <pthread.h>

static xtc_loop_t *g_listen_loop = NULL;

static void *
listen_thread(void *arg)
{
	(void)arg;
	xtc_pid_t pid;
	xtc_proc_opts_t popts = { 0 };

	if (xtc_loop_init(&g_listen_loop) != XTC_OK)
		return NULL;
	popts.name = "tnt-listener";
	(void)xtc_proc_spawn(g_listen_loop, listener_proc, NULL, &popts, &pid);
	(void)xtc_loop_run(g_listen_loop);
	(void)xtc_loop_fini(g_listen_loop);
	return NULL;
}

int
main(int argc, char **argv)
{
	xtc_tnt_spec_t spec;
	xtc_tcp_opts_t topts = XTC_TCP_OPTS_DEFAULT;
	pthread_t lt;
	int shards = 1;

	if (argc > 1)
		g_port = atoi(argv[1]);
	if (g_port == 0)
		g_port = 7777;
	if (argc > 2)
		shards = atoi(argv[2]);
	if (shards < 1)
		shards = 1;
	if (shards > 8)
		shards = 8;
	g_shards = shards;

	atomic_init(&g_next_shard, 0);
	atomic_init(&g_shutdown, 0);

	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);
	signal(SIGPIPE, SIG_IGN);

	topts.reuseaddr = 1;
	if (xtc_net_listen(XTC_NET_INET, "127.0.0.1", g_port, &topts,
	    &g_listen_fd) != XTC_OK) {
		fprintf(stderr, "echo: failed to bind 127.0.0.1:%d\n", g_port);
		return 1;
	}
	xtc_net_setnonblock(g_listen_fd);

	fprintf(stderr, "tnt-echo: listening on 127.0.0.1:%d (%d shard%s)\n",
	    g_port, g_shards, g_shards == 1 ? "" : "s");
	fflush(stderr);

	/* Start the listener thread (its own loop). */
	if (pthread_create(&lt, NULL, listen_thread, NULL) != 0) {
		fprintf(stderr, "echo: failed to start listener\n");
		return 1;
	}

	/* Boot the tnt runtime (blocks until xtc_tnt_stop). */
	memset(&spec, 0, sizeof(spec));
	spec.name = "tnt-echo";
	spec.types = echo_types;
	spec.n_types = 1;
	spec.shard_count = g_shards;
	spec.scratch_size = 65536;
	spec.recv_buf_size = ECHO_BUFSZ;
	spec.boot_type = -1;   /* echo has no boot isolate; the listener
	                        * spawns connections via xtc_tnt_spawn_on */

	(void)xtc_tnt_start(&spec);

	/* Shutdown. */
	atomic_store(&g_shutdown, 1);
	if (g_listen_loop)
		(void)xtc_loop_stop(g_listen_loop);
	pthread_join(lt, NULL);
	close(g_listen_fd);
	return 0;
}
