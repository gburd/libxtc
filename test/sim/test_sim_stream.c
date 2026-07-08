/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * test/sim/test_sim_stream.c
 *	Deterministic Simulation Testing of a BYTE-STREAM CONNECTION
 *	abstraction under the seeded scheduler -- the piece the message-
 *	level partition sim (test_sim_partition) does not model.
 *
 *	xtc's real cross-machine transport is raw kernel TCP sockets
 *	(io_net.c), which cannot run under the single-thread sim (a real
 *	socket's send/recv touch the kernel outside the sim's control) --
 *	the same not-coverable-by-design boundary documented in
 *	the design notes.  This test does NOT reimplement kernel TCP.  Instead
 *	it models the CONNECTION ABSTRACTION that sits above it -- an
 *	ordered, bidirectional byte stream between a client and a server
 *	fiber -- with a deterministic in-process "wire" (a pair of mpsc
 *	byte-chunk channels), and drives a length-prefixed request/response
 *	protocol over it while a seeded fault schedule injects the things a
 *	stream faces that a discrete message does not:
 *
 *	  - PARTIAL reads: the wire delivers a chunk that is a seeded slice
 *	    of a frame, so the reader must reassemble across reads;
 *	  - REORDERING is impossible on a stream (a stream is ordered by
 *	    definition) -- the test asserts the reader sees bytes in send
 *	    order even as chunk boundaries vary;
 *	  - LATENCY: each chunk delivery is deferred by a seeded delay, so
 *	    the interleaving of client/server progress is part of the
 *	    replayable schedule.
 *
 *	Protocol: the client sends N length-prefixed requests (u32 len +
 *	body); the server reassembles each frame, replies with a frame
 *	whose body is the request body reversed; the client reassembles
 *	each reply and checks it.  This exercises the exact
 *	frame-reassembly logic a real xtc_net_recv_frame consumer needs,
 *	under a stream that fragments arbitrarily -- deterministically.
 *
 *	Invariants (per seed): every request got its correct reversed
 *	reply, in order, with no lost/duplicated/corrupted byte; the run
 *	quiesces; and a repeated seed reproduces the exact byte-chunk
 *	schedule (result fingerprint) -- the replay property for a
 *	simulated stream connection.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xtc.h"
#include "xtc_exec.h"
#include "xtc_proc.h"
#include "xtc_chan.h"
#include "xtc_sim.h"

#define N_LOOPS   2
#define N_REQS    8
#define MAX_BODY  32

/* One direction of the wire: an ordered channel of byte-chunks.  A
 * chunk is a heap buffer + length; the reader concatenates chunks in
 * arrival order to recover the exact byte stream that was written. */
struct chunk { size_t len; uint8_t data[64]; };

/* A simulated bidirectional connection: two ordered byte channels. */
struct conn {
	xtc_chan_mpsc_t *c2s;   /* client -> server bytes */
	xtc_chan_mpsc_t *s2c;   /* server -> client bytes */
};

static atomic_int g_ok;         /* replies that matched */
static atomic_int g_server_done;
static atomic_int g_client_done;

/* Write `len` bytes to `ch` as one or more seeded-size chunks, each
 * delivery deferred by a seeded latency -- the stream fragments and
 * paces arbitrarily but stays ordered. */
static void
stream_write(xtc_chan_mpsc_t *ch, const uint8_t *buf, size_t len)
{
	size_t off = 0;
	while (off < len) {
		size_t remain = len - off;
		size_t take = 1 + (size_t)__xtc_sim_rng_range(XTC_SIM_RNG_APP,
		    (remain < 8 ? remain : 8));
		struct chunk *ck;
		if (take > remain) take = remain;
		ck = malloc(sizeof *ck);
		if (ck == NULL) return;
		ck->len = take;
		memcpy(ck->data, buf + off, take);
		off += take;
		/* Seeded inter-chunk pacing (advances the virtual clock so the
		 * peer interleaves). */
		if (__xtc_sim_rng_range(XTC_SIM_RNG_APP, 3) == 0)
			(void)xtc_proc_sleep(
			    (int64_t)(1 + __xtc_sim_rng_range(XTC_SIM_RNG_APP, 3))
			    * 1000 * 1000LL);
		while (xtc_chan_mpsc_try_send(ch, ck) != XTC_OK)
			(void)xtc_proc_sleep(500 * 1000LL);
	}
}

/* Read exactly `want` bytes from `ch` into `out`, reassembling across
 * arbitrarily-sized chunks -- the frame-reassembly a stream consumer
 * must do.  Leftover bytes from an over-read chunk are stashed for the
 * next call.  Returns 0 on success, -1 if the wire closed early. */
struct reader { uint8_t stash[64]; size_t stash_len; };

static int
stream_read(xtc_chan_mpsc_t *ch, struct reader *rd, uint8_t *out, size_t want)
{
	size_t got = 0;
	while (got < want) {
		if (rd->stash_len > 0) {
			size_t n = rd->stash_len < (want - got) ?
			    rd->stash_len : (want - got);
			memcpy(out + got, rd->stash, n);
			memmove(rd->stash, rd->stash + n, rd->stash_len - n);
			rd->stash_len -= n;
			got += n;
			continue;
		}
		{
			void *m = NULL;
			int idle = 0;
			while (xtc_chan_mpsc_try_recv(ch, &m) != XTC_OK ||
			    m == NULL) {
				if (++idle > 8000)
					return -1;   /* wire idle too long */
				(void)xtc_proc_sleep(500 * 1000LL);
			}
			{
				struct chunk *ck = m;
				memcpy(rd->stash + rd->stash_len, ck->data,
				    ck->len);
				rd->stash_len += ck->len;
				free(ck);
			}
		}
	}
	return 0;
}

/* Frame = u32 length (little-endian) + body. */
static void
frame_write(xtc_chan_mpsc_t *ch, const uint8_t *body, uint32_t len)
{
	uint8_t hdr[4];
	hdr[0] = (uint8_t)(len & 0xff);
	hdr[1] = (uint8_t)((len >> 8) & 0xff);
	hdr[2] = (uint8_t)((len >> 16) & 0xff);
	hdr[3] = (uint8_t)((len >> 24) & 0xff);
	stream_write(ch, hdr, 4);
	stream_write(ch, body, len);
}

static int
frame_read(xtc_chan_mpsc_t *ch, struct reader *rd, uint8_t *body,
    uint32_t *out_len)
{
	uint8_t hdr[4];
	uint32_t len;
	if (stream_read(ch, rd, hdr, 4) != 0)
		return -1;
	len = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) |
	    ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
	if (len > MAX_BODY)
		return -1;
	if (stream_read(ch, rd, body, len) != 0)
		return -1;
	*out_len = len;
	return 0;
}

/* SERVER: read N frames, reply each with the body reversed. */
static void
server(void *arg)
{
	struct conn *cn = arg;
	struct reader rd = {0};
	int i;
	for (i = 0; i < N_REQS; i++) {
		uint8_t body[MAX_BODY], rev[MAX_BODY];
		uint32_t len, j;
		if (frame_read(cn->c2s, &rd, body, &len) != 0)
			break;
		for (j = 0; j < len; j++)
			rev[j] = body[len - 1 - j];
		frame_write(cn->s2c, rev, len);
	}
	atomic_fetch_add(&g_server_done, 1);
}

/* CLIENT: send N seeded requests, verify each reply is the reverse. */
static void
client(void *arg)
{
	struct conn *cn = arg;
	struct reader rd = {0};
	int i;
	for (i = 0; i < N_REQS; i++) {
		uint8_t body[MAX_BODY], reply[MAX_BODY], expect[MAX_BODY];
		uint32_t len = 1 + (uint32_t)__xtc_sim_rng_range(
		    XTC_SIM_RNG_APP, MAX_BODY - 1);
		uint32_t rlen = 0, j;
		for (j = 0; j < len; j++)
			body[j] = (uint8_t)__xtc_sim_rng_range(
			    XTC_SIM_RNG_APP, 256);
		frame_write(cn->c2s, body, len);
		if (frame_read(cn->s2c, &rd, reply, &rlen) != 0)
			break;
		if (rlen != len)
			continue;
		for (j = 0; j < len; j++)
			expect[j] = body[len - 1 - j];
		if (memcmp(reply, expect, len) == 0)
			atomic_fetch_add(&g_ok, 1);
	}
	atomic_fetch_add(&g_client_done, 1);
}

static void
coordinator(void *arg)
{
	xtc_exec_t *e = arg;
	int tries;
	for (tries = 0; tries < 8000; tries++) {
		if (atomic_load(&g_client_done) && atomic_load(&g_server_done))
			break;
		(void)xtc_proc_sleep(1000 * 1000LL);
	}
	(void)xtc_proc_sleep(2 * 1000 * 1000LL);
	(void)xtc_exec_stop(e);
}

static int
run_one(uint64_t seed, int *out_ok, uint64_t *out_state)
{
	xtc_exec_t *e = NULL;
	struct conn cn;
	int rc;

	atomic_store(&g_ok, 0);
	atomic_store(&g_server_done, 0);
	atomic_store(&g_client_done, 0);
	memset(&cn, 0, sizeof cn);

	if (xtc_chan_mpsc_create(NULL, 256, &cn.c2s) != XTC_OK)
		return -1;
	if (xtc_chan_mpsc_create(NULL, 256, &cn.s2c) != XTC_OK) {
		xtc_chan_mpsc_destroy(cn.c2s);
		return -1;
	}

	if (xtc_exec_init(&e, N_LOOPS) != XTC_OK) {
		xtc_chan_mpsc_destroy(cn.c2s);
		xtc_chan_mpsc_destroy(cn.s2c);
		return -1;
	}
	xtc_exec_set_service_mode(e, 1);

	/* Client on loop 0, server on loop 1 -- opposite loops, so the
	 * "wire" is a genuine cross-loop stream under the seeded scheduler. */
	(void)xtc_proc_spawn(xtc_exec_loop(e, 0), client, &cn, NULL, NULL);
	(void)xtc_proc_spawn(xtc_exec_loop(e, 1), server, &cn, NULL, NULL);
	(void)xtc_proc_spawn(xtc_exec_loop(e, 0), coordinator, e, NULL, NULL);

	rc = xtc_sim_exec_run(e, seed, 20000000);

	if (out_ok != NULL) *out_ok = atomic_load(&g_ok);
	if (out_state != NULL) *out_state = xtc_sim_state_hash(e);

	(void)xtc_exec_fini(e);
	/* Drain any leftover chunks so nothing leaks. */
	{
		void *m;
		while (xtc_chan_mpsc_try_recv(cn.c2s, &m) == XTC_OK && m)
			free(m);
		while (xtc_chan_mpsc_try_recv(cn.s2c, &m) == XTC_OK && m)
			free(m);
	}
	xtc_chan_mpsc_destroy(cn.c2s);
	xtc_chan_mpsc_destroy(cn.s2c);
	return rc;
}

int
main(int argc, char **argv)
{
	uint64_t base = 0x73746d; /* "stm" */
	int n = 16, i, fails = 0;

	if (argc > 1) base = strtoull(argv[1], NULL, 0);
	if (argc > 2) n = atoi(argv[2]);

	printf("== stream-connection DST: %d seeds from base 0x%llx ==\n",
	    n, (unsigned long long)base);

	for (i = 0; i < n; i++) {
		uint64_t seed = base + (uint64_t)i * 0x9E3779B97F4A7C15ull;
		int ok = 0, ok2 = 0, rc, rc2, pass = 1;
		uint64_t st = 0, st2 = 0;

		rc = run_one(seed, &ok, &st);
		if (rc != XTC_OK) pass = 0;
		else if (ok != N_REQS) pass = 0;   /* every reply verified */

		if (pass) {
			rc2 = run_one(seed, &ok2, &st2);
			if (rc2 != rc || ok2 != ok || st2 != st)
				pass = 0;
		}

		if (!pass) {
			printf("  seed 0x%016llx: FAIL (ok=%d/%d rc=%d)\n",
			    (unsigned long long)seed, ok, N_REQS, rc);
			fails++;
		}
	}

	if (fails == 0) {
		printf("OK: stream-connection DST -- %d seeds, ordered "
		    "byte-stream frame reassembly under fragmenting wire, "
		    "all replay\n", n);
		return 0;
	}
	printf("FAIL: %d/%d stream-connection seeds failed\n", fails, n);
	return 1;
}
