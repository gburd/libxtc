/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/09_pgmock/backend.c
 *	The mock PostgreSQL backend proc.  One xtc_proc per client
 *	connection, driven entirely by the xtc scheduler:
 *
 *	  StartupMessage -> AuthenticationOk (R,0) + ParameterStatus (S)
 *	                    + BackendKeyData (K) + ReadyForQuery (Z,'I')
 *	  Query "select 1" (any text) -> RowDescription (T) + DataRow (D)
 *	                    + CommandComplete (C,"SELECT 1") + ReadyForQuery
 *	  Terminate (X) or client disconnect -> close + xtc_exit_self
 *
 *	PG v3 framing is 1 type byte + a 4-byte big-endian length that
 *	COUNTS ITSELF (length = 4 + payload).  The StartupMessage is the
 *	one exception: it is untyped (no leading type byte), just the
 *	self-inclusive length + payload.  This is NOT xtc's pure 4-byte
 *	frame (xtc_net_recv_frame), so we hand-roll the read/write over
 *	raw recv/send + xtc_proc_wait_fd (PG's WaitLatchOrSocket).
 *
 *	No PostgreSQL source, no PG globals -- this proves only the
 *	runtime seam (M16.1a).  See docs/M16_PG_ADAPTER.md.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "xtc.h"
#include "xtc_io.h"
#include "xtc_proc.h"

#include "backend.h"
#include "pg_latch.h"

/* PG v3 protocol version 3.0, sent in the StartupMessage. */
#define PG_PROTO_V3 0x00030000u

typedef struct backend_state {
	int fd;
	int closed;
} backend_state_t;

/* -------------------------------------------------------------------
 * Big-endian codec helpers (PG wire is network byte order).
 * ----------------------------------------------------------------- */

static void
put_u32(uint8_t *p, uint32_t v)
{
	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)(v);
}

static void
put_u16(uint8_t *p, uint16_t v)
{
	p[0] = (uint8_t)(v >> 8);
	p[1] = (uint8_t)(v);
}

static uint32_t
get_u32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

/* -------------------------------------------------------------------
 * Blocking-on-the-loop socket I/O: recv/send that yield the proc via
 * xtc_proc_wait_fd instead of blocking the thread.  Return >0 bytes,
 * 0 on clean EOF, -1 on error/kill.
 * ----------------------------------------------------------------- */

/* Read exactly `n` bytes into buf.  Returns 0 on success, -1 on EOF or
 * error (peer disconnect is one of these).  Parks the proc on the fd
 * when the socket would block. */
static int
read_full(backend_state_t *st, void *buf, size_t n)
{
	uint8_t *p = buf;
	size_t got = 0;

	while (got < n) {
		ssize_t r = recv(st->fd, p + got, n - got, MSG_DONTWAIT);
		if (r > 0) {
			got += (size_t)r;
			continue;
		}
		if (r == 0) {          /* peer closed */
			st->closed = 1;
			return -1;
		}
		if (errno == EAGAIN || errno == EWOULDBLOCK) {
			uint32_t revents = 0;
			uint32_t want = XTC_IO_READABLE | XTC_IO_HUP |
			                XTC_IO_ERR;
			int rc = pg_wait_latch_or_socket(st->fd, want, -1,
			                                  &revents);
			if (rc != XTC_OK && rc != XTC_E_AGAIN) {
				st->closed = 1;
				return -1;
			}
			if (revents & XTC_WAIT_MAILBOX)
				pg_reset_latch();
			if (revents & (XTC_IO_ERR | XTC_IO_HUP)) {
				/* HUP with data still buffered is possible;
				 * loop once more to drain, then EOF. */
				continue;
			}
			continue;
		}
		if (errno == EINTR)
			continue;
		st->closed = 1;
		return -1;
	}
	return 0;
}

/* Write all `n` bytes.  Returns 0 on success, -1 on error. */
static int
write_full(backend_state_t *st, const void *buf, size_t n)
{
	const uint8_t *p = buf;
	size_t sent = 0;

	while (sent < n) {
		ssize_t w = send(st->fd, p + sent, n - sent,
		                 MSG_DONTWAIT | MSG_NOSIGNAL);
		if (w > 0) {
			sent += (size_t)w;
			continue;
		}
		if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			uint32_t revents = 0;
			uint32_t want = XTC_IO_WRITABLE | XTC_IO_HUP |
			                XTC_IO_ERR;
			int rc = pg_wait_latch_or_socket(st->fd, want, -1,
			                                  &revents);
			if (rc != XTC_OK && rc != XTC_E_AGAIN) {
				st->closed = 1;
				return -1;
			}
			if (revents & XTC_WAIT_MAILBOX)
				pg_reset_latch();
			if (revents & (XTC_IO_ERR | XTC_IO_HUP)) {
				st->closed = 1;
				return -1;
			}
			continue;
		}
		if (w < 0 && errno == EINTR)
			continue;
		st->closed = 1;
		return -1;
	}
	return 0;
}

/* -------------------------------------------------------------------
 * PG v3 message read/write.
 * ----------------------------------------------------------------- */

/* Read the untyped StartupMessage: [int32 len][int32 proto][payload].
 * On success *out_proto has the protocol code; the parameter payload
 * is drained and discarded (the mock does not authenticate).  Returns
 * 0 on success, -1 on EOF/error. */
static int
read_startup(backend_state_t *st, uint32_t *out_proto)
{
	uint8_t hdr[8];
	uint32_t len, body;

	/* First 8 bytes: length (self-inclusive) + protocol/request code. */
	if (read_full(st, hdr, sizeof hdr) < 0)
		return -1;
	len = get_u32(hdr);
	*out_proto = get_u32(hdr + 4);

	/* len counts itself; guard a sane bound (PG caps startup packets). */
	if (len < 8 || len > (10u * 1024 * 1024))
		return -1;

	body = len - 8;   /* remaining parameter bytes after the two int32s */
	while (body > 0) {
		uint8_t scratch[512];
		size_t chunk = body < sizeof scratch ? body : sizeof scratch;
		if (read_full(st, scratch, chunk) < 0)
			return -1;
		body -= (uint32_t)chunk;
	}
	return 0;
}

/* Read one typed message: [byte type][int32 len][payload].  Allocates
 * *out_payload (may be NULL for a zero-length body); caller frees with
 * free().  Returns 0 on success, -1 on EOF/error. */
static int
read_typed(backend_state_t *st, uint8_t *out_type,
           uint8_t **out_payload, uint32_t *out_len)
{
	uint8_t hdr[5];
	uint32_t len, body;
	uint8_t *payload = NULL;

	if (read_full(st, hdr, sizeof hdr) < 0)
		return -1;
	*out_type = hdr[0];
	len = get_u32(hdr + 1);   /* counts the 4 length bytes, not the type */
	if (len < 4 || len > (256u * 1024 * 1024))
		return -1;
	body = len - 4;

	if (body > 0) {
		payload = malloc(body);
		if (payload == NULL)
			return -1;
		if (read_full(st, payload, body) < 0) {
			free(payload);
			return -1;
		}
	}
	*out_payload = payload;
	*out_len = body;
	return 0;
}

/* Write a typed message: [type][int32 len = 4 + plen][payload]. */
static int
write_typed(backend_state_t *st, uint8_t type,
            const void *payload, uint32_t plen)
{
	uint8_t hdr[5];
	hdr[0] = type;
	put_u32(hdr + 1, plen + 4);
	if (write_full(st, hdr, sizeof hdr) < 0)
		return -1;
	if (plen > 0 && write_full(st, payload, plen) < 0)
		return -1;
	return 0;
}

/* -------------------------------------------------------------------
 * Handshake + query responses.
 * ----------------------------------------------------------------- */

/* AuthenticationOk (R) + ParameterStatus (S) + BackendKeyData (K) +
 * ReadyForQuery (Z,'I').  Returns 0 on success. */
static int
send_startup_reply(backend_state_t *st)
{
	uint8_t auth[4];
	uint8_t keydata[8];
	uint8_t rfq = 'I';   /* idle, not in a transaction */
	/* ParameterStatus payloads are "name\0value\0". */
	static const char ps_ver[] = "server_version\09.6.0-pgmock";
	static const char ps_enc[] = "client_encoding\0UTF8";

	put_u32(auth, 0);   /* AuthenticationOk */
	if (write_typed(st, 'R', auth, sizeof auth) < 0)
		return -1;

	if (write_typed(st, 'S', ps_ver, (uint32_t)sizeof ps_ver) < 0)
		return -1;
	if (write_typed(st, 'S', ps_enc, (uint32_t)sizeof ps_enc) < 0)
		return -1;

	/* BackendKeyData: int32 pid + int32 secret key.  Values are
	 * cosmetic in the mock. */
	put_u32(keydata, 4242);
	put_u32(keydata + 4, 0xC0FFEEu);
	if (write_typed(st, 'K', keydata, sizeof keydata) < 0)
		return -1;

	if (write_typed(st, 'Z', &rfq, 1) < 0)
		return -1;
	return 0;
}

/* Respond to any Query with a single-column, single-row "1":
 *   RowDescription (T) column "?column?" int4,
 *   DataRow (D) one column = the text "1",
 *   CommandComplete (C) "SELECT 1",
 *   ReadyForQuery (Z,'I').
 * Returns 0 on success. */
static int
send_select_1(backend_state_t *st)
{
	uint8_t buf[128];
	size_t off;
	uint8_t rfq = 'I';

	/* --- RowDescription (T) ---
	 * int16 field count = 1, then per field:
	 *   String name "\0", int32 table oid, int16 attnum,
	 *   int32 type oid (int4 = 23), int16 typlen (4),
	 *   int32 typmod (-1), int16 format (0 = text). */
	off = 0;
	put_u16(buf + off, 1); off += 2;
	memcpy(buf + off, "?column?", 8); off += 8;
	buf[off++] = 0;                       /* NUL terminator */
	put_u32(buf + off, 0); off += 4;      /* table oid */
	put_u16(buf + off, 0); off += 2;      /* column attnum */
	put_u32(buf + off, 23); off += 4;     /* type oid: int4 */
	put_u16(buf + off, 4); off += 2;      /* typlen */
	put_u32(buf + off, 0xffffffffu); off += 4;  /* typmod = -1 */
	put_u16(buf + off, 0); off += 2;      /* format: text */
	if (write_typed(st, 'T', buf, (uint32_t)off) < 0)
		return -1;

	/* --- DataRow (D) ---
	 * int16 column count = 1, then per column:
	 *   int32 value length, value bytes.  Value is the text "1". */
	off = 0;
	put_u16(buf + off, 1); off += 2;
	put_u32(buf + off, 1); off += 4;      /* one byte of value */
	buf[off++] = '1';
	if (write_typed(st, 'D', buf, (uint32_t)off) < 0)
		return -1;

	/* --- CommandComplete (C) --- tag string, NUL-terminated. */
	if (write_typed(st, 'C', "SELECT 1", (uint32_t)sizeof "SELECT 1") < 0)
		return -1;

	if (write_typed(st, 'Z', &rfq, 1) < 0)
		return -1;
	return 0;
}

/* -------------------------------------------------------------------
 * The proc entry.
 * ----------------------------------------------------------------- */

/* Release everything the backend proc holds: close the fd (once) and
 * free the state.  Runs as the at-exit hook so a normal return, a
 * kill, or a contained fault all clean up -- no leaked fd or mctx. */
static void
backend_cleanup(void *arg)
{
	backend_state_t *st = arg;
	if (st->fd >= 0) {
		close(st->fd);
		st->fd = -1;
	}
	free(st);
}

static void
backend_proc(void *arg)
{
	backend_state_t *st = arg;
	uint32_t proto = 0;

	/* Register cleanup as an at-exit hook so a normal return, a kill,
	 * or a contained fault all release the socket and state.  st stays
	 * live until the hook runs (do NOT free it before xtc_exit_self). */
	(void)xtc_proc_at_exit(backend_cleanup, st);

	/* PG v3 handshake. */
	if (read_startup(st, &proto) < 0)
		goto done;
	if (proto != PG_PROTO_V3) {
		/* SSLRequest / GSSENCRequest / CancelRequest land here in real
		 * PG; the mock rejects anything but a clean v3 startup. */
		goto done;
	}
	if (send_startup_reply(st) < 0)
		goto done;

	/* Simple-query loop. */
	while (!st->closed) {
		uint8_t type = 0;
		uint8_t *payload = NULL;
		uint32_t plen = 0;

		if (read_typed(st, &type, &payload, &plen) < 0)
			break;

		switch (type) {
		case 'Q':   /* Query */
			free(payload);
			if (send_select_1(st) < 0) {
				st->closed = 1;
			}
			break;
		case 'X':   /* Terminate */
			free(payload);
			goto done;
		default:
			/* Parse/Bind/Execute (extended protocol) etc. are not
			 * modelled; ignore and keep the session alive. */
			free(payload);
			break;
		}
	}

done:
	/* The at-exit hook (backend_cleanup) closes the fd and frees st;
	 * exit the proc cleanly so monitors observe a normal DOWN. */
	xtc_exit_self(0);
}

int
pgmock_backend_spawn(xtc_loop_t *loop, const pgmock_backend_opts_t *opts,
                     xtc_pid_t *out_pid)
{
	backend_state_t *st;
	xtc_proc_opts_t po = { 0 };
	int rc;

	st = calloc(1, sizeof *st);
	if (st == NULL)
		return XTC_E_NOMEM;
	st->fd = opts->fd;

	po.name = "pgmock-backend";
	rc = xtc_proc_spawn(loop, backend_proc, st, &po, out_pid);
	if (rc != XTC_OK)
		free(st);   /* caller keeps the fd */
	return rc;
}
