/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * test/m16/test_pgmock_smoke.c
 *	M16.1a acceptance test.  Drives the in-tree mock PostgreSQL
 *	backend (examples/09_pgmock) IN-PROCESS on an xtc_app with an
 *	ephemeral TCP port, using a RAW-SOCKET client (no libpq, so CI
 *	needs no PostgreSQL).  Asserts:
 *
 *	  1. the PG v3 handshake completes (AuthenticationOk + ...
 *	     + ReadyForQuery),
 *	  2. Query("select 1") returns RowDescription + a DataRow whose
 *	     single column bytes are "1" + CommandComplete + ReadyForQuery,
 *	  3. two concurrent connections are served by DISTINCT backend
 *	     pids on one loop (no-fork multiplexing),
 *	  4. Terminate exits the backend cleanly (the listener's accept
 *	     count matches; no hang, no leaked fd -- ASan-clean).
 *
 *	The postmaster runs on the main thread (xtc_app_run blocks); the
 *	raw client runs on a plain pthread with blocking sockets.  The
 *	client requests app shutdown when finished.
 *
 *	Plain asserts + main(); no munit (matches the "simpler" option in
 *	the M16.1 plan).
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <arpa/inet.h>
#include <assert.h>
#include <errno.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "xtc.h"
#include "xtc_app.h"
#include "xtc_net.h"
#include "xtc_proc.h"

#include "listener.h"

/* ---- test harness state shared main thread <-> client thread ---- */
static xtc_app_t         *g_app;
static pgmock_listener_t  g_listener;
static _Atomic int        g_shutdown;
static int                g_client_ok;   /* set by the client thread */
static int                g_port;        /* ephemeral port bound by main */

/* -------------------------------------------------------------------
 * Big-endian codec + framed raw-socket client helpers.
 * ----------------------------------------------------------------- */

static void
put_u32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)(v);
}

static uint32_t
get_u32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* Blocking read of exactly n bytes.  Returns 0 or -1. */
static int
xread(int fd, void *buf, size_t n)
{
	uint8_t *p = buf;
	size_t got = 0;
	while (got < n) {
		ssize_t r = recv(fd, p + got, n - got, 0);
		if (r > 0) { got += (size_t)r; continue; }
		if (r < 0 && errno == EINTR) continue;
		return -1;
	}
	return 0;
}

static int
xwrite(int fd, const void *buf, size_t n)
{
	const uint8_t *p = buf;
	size_t sent = 0;
	while (sent < n) {
		ssize_t w = send(fd, p + sent, n - sent, MSG_NOSIGNAL);
		if (w > 0) { sent += (size_t)w; continue; }
		if (w < 0 && errno == EINTR) continue;
		return -1;
	}
	return 0;
}

/* Read one typed message: [type][int32 len][payload].  Fills *out_type,
 * copies up to cap payload bytes, sets *out_plen to the FULL payload
 * length (may exceed cap).  Returns 0 or -1. */
static int
read_typed(int fd, uint8_t *out_type, uint8_t *payload, size_t cap,
           uint32_t *out_plen)
{
	uint8_t hdr[5];
	uint32_t len, body;
	if (xread(fd, hdr, sizeof hdr) < 0)
		return -1;
	*out_type = hdr[0];
	len = get_u32(hdr + 1);
	if (len < 4)
		return -1;
	body = len - 4;
	*out_plen = body;
	{
		uint32_t taken = body < cap ? body : (uint32_t)cap;
		if (taken > 0 && xread(fd, payload, taken) < 0)
			return -1;
		/* drain the remainder we didn't keep */
		{
			uint32_t rest = body - taken;
			uint8_t scratch[256];
			while (rest > 0) {
				uint32_t c = rest < sizeof scratch ? rest :
				    (uint32_t)sizeof scratch;
				if (xread(fd, scratch, c) < 0)
					return -1;
				rest -= c;
			}
		}
	}
	return 0;
}

/* Connect a blocking TCP socket to 127.0.0.1:g_port. */
static int
client_connect(void)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in sa;
	int one = 1;
	if (fd < 0)
		return -1;
	(void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_port = htons((uint16_t)g_port);
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/* Send a v3 StartupMessage advertising protocol 3.0 with a "user"
 * parameter, then assert the AuthenticationOk (R,0) ... ReadyForQuery
 * (Z) handshake.  Returns 0 on success. */
static int
do_handshake(int fd)
{
	uint8_t msg[64];
	size_t off = 4;   /* leave room for the self-inclusive length */
	uint8_t type;
	uint8_t payload[64];
	uint32_t plen;
	int saw_auth_ok = 0;

	put_u32(msg + off, 0x00030000u); off += 4;   /* protocol 3.0 */
	memcpy(msg + off, "user", 4); off += 4; msg[off++] = 0;
	memcpy(msg + off, "smoke", 5); off += 5; msg[off++] = 0;
	msg[off++] = 0;   /* terminating empty key */
	put_u32(msg, (uint32_t)off);   /* length counts itself */
	if (xwrite(fd, msg, off) < 0)
		return -1;

	/* Read messages until ReadyForQuery (Z). */
	for (;;) {
		if (read_typed(fd, &type, payload, sizeof payload, &plen) < 0)
			return -1;
		if (type == 'R') {
			if (plen >= 4 && get_u32(payload) == 0)
				saw_auth_ok = 1;
		} else if (type == 'E') {
			return -1;   /* ErrorResponse */
		} else if (type == 'Z') {
			break;
		}
	}
	return saw_auth_ok ? 0 : -1;
}

/* Send Query("select 1") and assert RowDescription (T) with column
 * "?column?", DataRow (D) whose one column value is "1",
 * CommandComplete (C) "SELECT 1", ReadyForQuery (Z).  Returns 0. */
static int
do_select_1(int fd)
{
	static const char q[] = "select 1";
	uint8_t hdr[5];
	uint8_t type;
	uint8_t payload[256];
	uint32_t plen;
	int saw_T = 0, saw_D = 0, saw_C = 0;

	hdr[0] = 'Q';
	put_u32(hdr + 1, (uint32_t)(4 + sizeof q));   /* len + NUL */
	if (xwrite(fd, hdr, sizeof hdr) < 0)
		return -1;
	if (xwrite(fd, q, sizeof q) < 0)   /* includes the NUL */
		return -1;

	for (;;) {
		if (read_typed(fd, &type, payload, sizeof payload, &plen) < 0)
			return -1;
		if (type == 'T') {
			/* int16 count, then "?column?\0" ... */
			if (plen >= 2 + 9 &&
			    memcmp(payload + 2, "?column?", 8) == 0 &&
			    payload[2 + 8] == 0)
				saw_T = 1;
		} else if (type == 'D') {
			/* int16 col count = 1, int32 len = 1, byte '1' */
			uint16_t ncol = (uint16_t)((payload[0] << 8) |
			    payload[1]);
			uint32_t vlen = get_u32(payload + 2);
			if (ncol == 1 && vlen == 1 && payload[6] == '1')
				saw_D = 1;
		} else if (type == 'C') {
			if (plen >= 8 && memcmp(payload, "SELECT 1", 8) == 0)
				saw_C = 1;
		} else if (type == 'E') {
			return -1;
		} else if (type == 'Z') {
			break;
		}
	}
	return (saw_T && saw_D && saw_C) ? 0 : -1;
}

/* Send Terminate (X) and close. */
static void
do_terminate(int fd)
{
	uint8_t hdr[5];
	hdr[0] = 'X';
	put_u32(hdr + 1, 4);
	(void)xwrite(fd, hdr, sizeof hdr);
	close(fd);
}

/* -------------------------------------------------------------------
 * The client thread: full scenario, then request app shutdown.
 * ----------------------------------------------------------------- */
static void *
client_thread(void *arg)
{
	int fd1, fd2;
	int ok = 1;
	(void)arg;

	/* Open TWO concurrent connections BEFORE terminating either, so
	 * both backends are alive at once -- proving no-fork multiplexing
	 * on one loop. */
	fd1 = client_connect();
	assert(fd1 >= 0);
	fd2 = client_connect();
	assert(fd2 >= 0);

	ok &= (do_handshake(fd1) == 0);
	ok &= (do_handshake(fd2) == 0);
	ok &= (do_select_1(fd1) == 0);
	ok &= (do_select_1(fd2) == 0);

	/* Two connections were accepted -> two backend procs spawned. */
	assert(atomic_load(&g_listener.accepted) >= 2);
	assert(atomic_load(&g_listener.pids_n) >= 2);
	/* ...and they are DISTINCT pids. */
	assert(!xtc_pid_eq(g_listener.pids[0], g_listener.pids[1]));

	do_terminate(fd1);
	do_terminate(fd2);

	g_client_ok = ok;

	/* Ask the postmaster loop to stop; the main thread's xtc_app_run
	 * returns. */
	atomic_store(&g_shutdown, 1);
	(void)xtc_app_stop(g_app);
	return NULL;
}

/* -------------------------------------------------------------------
 * Bind an ephemeral loopback TCP listen socket; return the fd and set
 * *out_port to the kernel-assigned port.  (xtc_net_listen rejects
 * port 0, so bind directly here.)
 * ----------------------------------------------------------------- */
static int
bind_ephemeral(int *out_port)
{
	int fd = socket(AF_INET, SOCK_STREAM, 0);
	struct sockaddr_in sa;
	socklen_t slen = sizeof sa;
	int one = 1;
	if (fd < 0)
		return -1;
	(void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
	memset(&sa, 0, sizeof sa);
	sa.sin_family = AF_INET;
	sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	sa.sin_port = 0;   /* ephemeral */
	if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0 ||
	    listen(fd, 128) != 0 ||
	    getsockname(fd, (struct sockaddr *)&sa, &slen) != 0) {
		close(fd);
		return -1;
	}
	*out_port = ntohs(sa.sin_port);
	return fd;
}

int
main(void)
{
	int listen_fd;
	xtc_app_opts_t app_opts = XTC_APP_OPTS_DEFAULT;
	xtc_child_spec_t kids[1];
	xtc_pid_t pid_slots[8];
	pthread_t client;

	listen_fd = bind_ephemeral(&g_port);
	assert(listen_fd >= 0);
	xtc_net_setnonblock(listen_fd);

	app_opts.name = "pgmock-test";
	app_opts.n_loops = 1;
	assert(xtc_app_create(&app_opts, &g_app) == XTC_OK);

	atomic_init(&g_shutdown, 0);
	atomic_init(&g_listener.accepted, 0);
	atomic_init(&g_listener.pids_n, 0);
	memset(pid_slots, 0, sizeof pid_slots);
	g_listener.listen_fd = listen_fd;
	g_listener.loop = xtc_app_loop(g_app);
	g_listener.shutdown = &g_shutdown;
	g_listener.pids = pid_slots;
	g_listener.pids_cap = (int)(sizeof pid_slots / sizeof pid_slots[0]);

	memset(kids, 0, sizeof kids);
	kids[0].name = "postmaster";
	kids[0].fn = pgmock_listener_proc;
	kids[0].arg = &g_listener;
	kids[0].loop = 0;
	kids[0].policy = XTC_RESTART_PERMANENT;
	assert(xtc_app_start(g_app, kids, 1) == XTC_OK);

	/* The client drives the scenario on its own thread while the
	 * postmaster runs the loop on this one. */
	assert(pthread_create(&client, NULL, client_thread, NULL) == 0);

	xtc_app_run(g_app);   /* blocks until the client calls xtc_app_stop */

	pthread_join(client, NULL);

	xtc_app_destroy(g_app);
	close(listen_fd);

	assert(g_client_ok);
	printf("test_pgmock_smoke: PASS "
	    "(handshake + SELECT 1, %d conns, 2 distinct backend pids, "
	    "clean terminate)\n",
	    atomic_load(&g_listener.accepted));
	return 0;
}
