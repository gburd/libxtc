/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * examples/05_rexis/conn.c
 *	Per-connection xtc_proc implementation.
 */

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include "conn.h"
#include "proto.h"
#include "cmd.h"
#include "xtc_async.h"
#include "xtc_io.h"
#include "xtc_inject.h"
#include "xtc_log.h"
#include "xtc_net.h"
#include "xtc_pg.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0          /* SIGPIPE is ignored process-wide (main.c) */
#endif

#define MAX_ARGS           128

/*
 * Output backpressure.  While more than out_soft reply bytes are unsent
 * (OUT_SOFT, or max_output / 2 if that is smaller), the connection stops
 * reading and running its pipeline and only waits for the socket to
 * drain -- so a client that pipelines faster than it reads holds at most
 * out_soft + one reply in server memory, and TCP flow control pushes
 * back on its writes.  (A client that writes an unbounded pipeline and
 * never reads therefore stalls once the socket buffers fill -- the
 * bounded-memory alternative to buffering every reply, which is what
 * Redis does with its default unlimited normal-client output limit.)
 * One reply may take the pending output up to max_output (the hard
 * limit); a reply that would pass it is replaced by an error.
 */
#define OUT_SOFT           (1024 * 1024)

/*
 * Buffer hysteresis.  A buffer grown past CONN_BASE_BUF keeps the
 * capacity it needed: it is cut back (conn_trim) at most once per
 * CONN_TRIM_NS to the power-of-two size of its peak use in that window
 * (Redis resizes client buffers from its 100 ms clientsCron the same
 * way), and to what it holds now (normally CONN_BASE_BUF) once the
 * connection has been idle that long.  Shrinking right after every command made a
 * client whose commands or replies pass CONN_BASE_BUF realloc up and
 * down per command (an mremap and fresh page faults above 128 KiB).
 * The kept capacity stays charged to client_res, so --max-memory still
 * bounds it, and an idle connection returns to the base.
 */
#define CONN_TRIM_NS       (100LL * 1000 * 1000)

/* A recv that fills the query buffer means more is waiting: grow (read
 * ahead, charged) up to this so a deep pipeline is parsed in one pass. */
#define CONN_READ_AHEAD    (64UL * 1024)

/* Why a reply could not be buffered (conn_state_t.out_fail). */
#define OUT_FAIL_LIMIT     1    /* would pass max_output */
#define OUT_FAIL_MEM       2    /* client budget (--max-memory) refused */

/* Why conn_process_commands stopped. */
#define STOP_NEED_MORE     0    /* no complete command buffered */
#define STOP_BACKPRESSURE  1    /* pending output >= OUT_SOFT */
#define STOP_THROTTLED     2    /* --max-iops bucket empty */
#define STOP_QUIT          3

/*
 * A reply piece sent by reference: len bytes of a held keyspace value
 * (db_value_hold) that go out after wbuf[..at) and before wbuf[at..).
 * The hold and a client-budget charge of len are dropped when its last
 * byte has left (seg_done).
 */
typedef struct out_seg {
	size_t      at;          /* wbuf offset the bytes go before */
	const char *data;
	size_t      len;
	size_t      sent;
} out_seg_t;

#define SEG_IOV_MAX  64          /* iovecs per sendmsg */

/* Connection state */
typedef struct conn_state {
	int         fd;
	db_t       *db;
	xtc_res_t  *res;

	/* Query buffer: rbuf[0..rlen) is received, unprocessed input.  It
	 * is allocated (CONN_BASE_BUF) by the first read, grows (doubling)
	 * to hold one command of at most max_request bytes (or to
	 * CONN_READ_AHEAD for a deep pipeline), and conn_trim shrinks it
	 * back.  NULL with rcap 0 until the first read: a connection that
	 * never sends holds no buffer. */
	char       *rbuf;
	size_t      rcap;
	size_t      rlen;
	size_t      rpeak;       /* largest rlen since the last trim */
	size_t      max_request;

	/* Output buffer: wbuf[wpos..wlen) is unsent reply bytes.  Allocated
	 * by the first reply (conn_out_grow), grows up to max_output;
	 * conn_trim shrinks it back once drained. */
	char       *wbuf;
	size_t      wcap;
	size_t      wlen;
	size_t      wpos;
	size_t      wpeak;       /* largest wlen since the last trim */
	int64_t     trim_ns;     /* last_io_ns at the last trim */
	size_t      max_output;
	size_t      out_soft;
	int         out_fail;    /* OUT_FAIL_* of the last refused grow */

	/* By-reference reply pieces, FIFO segs[seg_head..seg_head+nseg).
	 * seg_pending = their unsent bytes; the pending output is
	 * wlen - wpos + seg_pending. */
	out_seg_t  *segs;
	size_t      seg_cap;
	size_t      seg_head;
	size_t      nseg;
	size_t      seg_pending;
	int         throttled;   /* last pass stopped on --max-iops */
	int         partial;     /* rbuf holds no complete command */

	/* Every buffer byte above the two CONN_BASE_BUF base buffers is
	 * charged to this budget (NULL = not).  The base is not: its total
	 * is bounded by --max-clients, and charging it would let a full
	 * keyspace lock every new client out (even the one sent to run
	 * FLUSHDB). */
	xtc_res_t  *client_res;

	/* Idle timeout (0 = off) and the last time a byte moved. */
	int64_t     idle_timeout_ns;
	int64_t     last_io_ns;

	/* Rate limiting */
	int64_t    *iops_tokens;
	int64_t     iops_cap;

	/* Server-wide state */
	_Atomic int *conn_count;
	_Atomic int *shutdown;
	int          loop_idx;   /* census slot (g_loop_live) */

	/* Pub/sub: this connection's own pid + the shared channel registry
	 * (an xtc_reg_t used as a duplicate-key process-group set).  A
	 * SUBSCRIBE joins st->self to a channel group; a PUBLISH from any
	 * connection fans a message out to every subscriber's mailbox, which
	 * the conn proc drains and writes to its socket. */
	xtc_pid_t   self;
	xtc_reg_t  *pubsub;
	int         sub_count;   /* channels this connection is subscribed to */

	/* Flags */
	int         quit;        /* finish flushing, then close */
	int         eof;         /* peer finished sending */
	int         closed;      /* socket error / peer gone: close now */
} conn_state_t;

/* Charged client buffer bytes held by all connections (INFO). */
static _Atomic int64_t g_client_bytes;

int64_t
conn_client_bytes(void)
{
	return atomic_load(&g_client_bytes);
}

/* ----- per-loop census (conn.h) ----- */

static int              g_n_loops = 1;
static int              g_loop_cap = CONN_LOOP_MAX;
static _Atomic int      g_loop_live[CONN_MAX_LOOPS];
static _Atomic int64_t  g_rejected;
static _Atomic int64_t  g_idle_wakes;     /* park timer was the only cause */
static _Atomic int64_t  g_ref_bytes;      /* value bytes sent by reference */

int64_t
conn_ref_bytes(void)
{
	return atomic_load(&g_ref_bytes);
}

/*
 * fd -> pid of the conn proc that owns it, so shutdown can wake procs
 * parked with no deadline (conn_wake_all).  A proc stores its pid when it
 * starts and clears it in teardown BEFORE closing the fd, so a later
 * connection that reuses the number cannot be clobbered.  An fd at or
 * above g_fd_cap (or a failed allocation) is not recorded, and its proc
 * keeps a 1 s park deadline instead.
 */
static _Atomic uint64_t *g_fd_pid;
static int               g_fd_cap;

static uint64_t
pid_pack(xtc_pid_t p)
{
	return (uint64_t)p.loop_id << 48 | (uint64_t)p.local_id << 32 | p.gen;
}

static xtc_pid_t
pid_unpack(uint64_t v)
{
	xtc_pid_t p;

	p.loop_id = (uint16_t)(v >> 48);
	p.local_id = (uint16_t)(v >> 32);
	p.gen = (uint32_t)v;
	return p;
}

void
conn_wake_init(int max_fd)
{
	if (max_fd > 0 && (g_fd_pid = xtc_calloc((size_t)max_fd,
	    sizeof *g_fd_pid)) != NULL)
		g_fd_cap = max_fd;
}

void
conn_wake_all(void)
{
	uint64_t v;
	int i;

	for (i = 0; i < g_fd_cap; i++)
		if ((v = atomic_load(&g_fd_pid[i])) != 0)
			(void)xtc_proc_wake(pid_unpack(v));
}

int64_t
conn_idle_wakeups(void)
{
	return atomic_load(&g_idle_wakes);
}

void
conn_census_init(int n_loops, int per_loop_cap)
{
	g_n_loops = n_loops < 1 ? 1 : n_loops > CONN_MAX_LOOPS ?
	    CONN_MAX_LOOPS : n_loops;
	g_loop_cap = per_loop_cap < 1 || per_loop_cap > CONN_LOOP_MAX ?
	    CONN_LOOP_MAX : per_loop_cap;
}

int
conn_pick_loop(void)
{
	int i, best = -1, n, best_n = g_loop_cap;

	for (i = 0; i < g_n_loops; i++) {
		n = atomic_load(&g_loop_live[i]);
		if (n < best_n) {
			best = i;
			best_n = n;
		}
	}
	return best;
}

int
conn_census_loops(void)
{
	return g_n_loops;
}

int
conn_census_live(int loop_idx)
{
	return loop_idx >= 0 && loop_idx < g_n_loops ?
	    atomic_load(&g_loop_live[loop_idx]) : 0;
}

int
conn_census_total(void)
{
	int i, t = 0;

	for (i = 0; i < g_n_loops; i++)
		t += atomic_load(&g_loop_live[i]);
	return t;
}

void
conn_census_reject(void)
{
	atomic_fetch_add(&g_rejected, 1);
}

int64_t
conn_census_rejected(void)
{
	return atomic_load(&g_rejected);
}

void
conn_refuse(int fd, const char *msg)
{
	char line[160];
	int n;

	n = snprintf(line, sizeof line, "-ERR %s\r\n", msg);
	if (n > 0 && (size_t)n < sizeof line)
		(void)send(fd, line, (size_t)n, MSG_DONTWAIT | MSG_NOSIGNAL);
	xtc_net_close(fd);
	conn_census_reject();
}

static int
conn_charge(conn_state_t *st, size_t n)
{
	if (st->client_res != NULL &&
	    xtc_res_acquire(st->client_res, XTC_RES_MEM_BYTES, (int64_t)n) !=
	    XTC_OK)
		return -1;
	atomic_fetch_add(&g_client_bytes, (int64_t)n);
	return 0;
}

static void
conn_refund(conn_state_t *st, size_t n)
{
	if (st->client_res != NULL)
		xtc_res_release(st->client_res, XTC_RES_MEM_BYTES, (int64_t)n);
	atomic_fetch_sub(&g_client_bytes, (int64_t)n);
}

/* The charged part of a buffer of cap bytes: only bytes above
 * CONN_BASE_BUF are charged (see client_res). */
static size_t
conn_charged(size_t cap)
{
	return cap > CONN_BASE_BUF ? cap - CONN_BASE_BUF : 0;
}

/* Resize *buf to ncap bytes (contents kept up to the smaller size),
 * charging or refunding the difference.  -1: budget or allocator said
 * no; *buf is unchanged. */
static int
conn_resize(conn_state_t *st, char **buf, size_t *cap, size_t ncap)
{
	size_t c0 = conn_charged(*cap), c1 = conn_charged(ncap);
	char *nb;

	if (ncap == *cap)
		return 0;
	if (c1 > c0 && conn_charge(st, c1 - c0) != 0) {
		st->out_fail = OUT_FAIL_MEM;
		return -1;
	}
	if ((nb = xtc_realloc(*buf, ncap)) == NULL) {
		if (c1 > c0)
			conn_refund(st, c1 - c0);
		st->out_fail = OUT_FAIL_MEM;
		return -1;
	}
	if (c1 < c0)
		conn_refund(st, c0 - c1);
	*buf = nb;
	*cap = ncap;
	return 0;
}

/* resp_buf_t grow callback over the connection's output buffer. */
static int
conn_out_grow(resp_buf_t *b, size_t need)
{
	conn_state_t *st = b->grow_arg;
	size_t ncap = st->wcap;

	if (need + st->seg_pending > st->max_output) {
		st->out_fail = OUT_FAIL_LIMIT;
		return -1;
	}
	if (ncap == 0)
		ncap = CONN_BASE_BUF;        /* the first reply (lazy base) */
	while (ncap < need)
		ncap = ncap > st->max_output / 2 ? st->max_output : ncap * 2;
	if (conn_resize(st, &st->wbuf, &st->wcap, ncap) != 0)
		return -1;
	b->data = st->wbuf;
	b->cap = st->wcap;
	return 0;
}

/* Move unsent output to the front so new replies append after it. */
static void
conn_out_compact(conn_state_t *st)
{
	size_t i;

	if (st->wpos == 0)
		return;
	memmove(st->wbuf, st->wbuf + st->wpos, st->wlen - st->wpos);
	for (i = 0; i < st->nseg; i++)
		st->segs[st->seg_head + i].at -= st->wpos;
	st->wlen -= st->wpos;
	st->wpos = 0;
}

/* Unsent reply bytes, by-reference pieces included. */
static size_t
conn_pending(const conn_state_t *st)
{
	return st->wlen - st->wpos + st->seg_pending;
}

static int
conn_has_output(const conn_state_t *st)
{
	return st->wpos < st->wlen || st->nseg > 0;
}

/* Drop a by-reference piece: the value hold, the charge, the bytes it
 * had not sent. */
static void
seg_drop(conn_state_t *st, out_seg_t *g)
{
	st->seg_pending -= g->len - g->sent;
	conn_refund(st, g->len);
	db_value_release(g->data);
}

/* The head piece has been sent whole. */
static void
seg_done(conn_state_t *st)
{
	out_seg_t *g = &st->segs[st->seg_head];

	atomic_fetch_add(&g_ref_bytes, (int64_t)g->len);
	seg_drop(st, g);
	st->seg_head++;
	if (--st->nseg == 0)
		st->seg_head = 0;
}

/* cmd_ctx_t.out_ref: queue len bytes of a held value at out's end.  The
 * pending output (max_output) and the client budget (--max-memory) count
 * it as if it were copied. */
static int
conn_out_ref(void *arg, resp_buf_t *out, const char *data, size_t len)
{
	conn_state_t *st = arg;
	out_seg_t *ns;
	size_t ncap;

	if (out->len + st->seg_pending + len > st->max_output) {
		st->out_fail = OUT_FAIL_LIMIT;
		goto refuse;
	}
	if (st->seg_head + st->nseg == st->seg_cap) {
		if (st->seg_head > 0) {
			memmove(st->segs, st->segs + st->seg_head,
			    st->nseg * sizeof *st->segs);
			st->seg_head = 0;
		} else {
			ncap = st->seg_cap == 0 ? 8 : st->seg_cap * 2;
			if ((ns = xtc_realloc(st->segs, ncap * sizeof *ns)) ==
			    NULL) {
				st->out_fail = OUT_FAIL_MEM;
				goto refuse;
			}
			st->segs = ns;
			st->seg_cap = ncap;
		}
	}
	if (conn_charge(st, len) != 0) {
		st->out_fail = OUT_FAIL_MEM;
		goto refuse;
	}
	ns = &st->segs[st->seg_head + st->nseg++];
	ns->at = out->len;
	ns->data = data;
	ns->len = len;
	ns->sent = 0;
	st->seg_pending += len;
	return 0;
refuse:
	db_value_release(data);
	out->err = 1;
	return -1;
}

/* Drop the pieces queued after the first `keep` (a refused reply). */
static void
seg_truncate(conn_state_t *st, size_t keep)
{
	while (st->nseg > keep)
		seg_drop(st, &st->segs[st->seg_head + --st->nseg]);
	if (st->nseg == 0)
		st->seg_head = 0;
}

/* sendmsg over wbuf and the by-reference pieces, in stream order.
 * Returns what send() would. */
static ssize_t
conn_sendv(conn_state_t *st)
{
	struct iovec iov[SEG_IOV_MAX];
	struct msghdr mh;
	size_t pos = st->wpos, i;
	int n = 0;
	out_seg_t *g;
	ssize_t r;

	for (i = 0; i < st->nseg && n < SEG_IOV_MAX - 1; i++) {
		g = &st->segs[st->seg_head + i];
		if (g->at > pos) {
			iov[n].iov_base = st->wbuf + pos;
			iov[n++].iov_len = g->at - pos;
			pos = g->at;
		}
		iov[n].iov_base = (void *)(uintptr_t)(g->data + g->sent);
		iov[n++].iov_len = g->len - g->sent;
	}
	if (i == st->nseg && pos < st->wlen && n < SEG_IOV_MAX) {
		iov[n].iov_base = st->wbuf + pos;
		iov[n++].iov_len = st->wlen - pos;
	}
	memset(&mh, 0, sizeof mh);
	mh.msg_iov = iov;
	mh.msg_iovlen = n;           /* size_t on glibc, int on BSD/musl */
	r = sendmsg(st->fd, &mh, MSG_DONTWAIT | MSG_NOSIGNAL);
	return r;
}

/* Account n sent bytes against wbuf and the pieces, in stream order. */
static void
conn_sent(conn_state_t *st, size_t n)
{
	out_seg_t *g;
	size_t lim, k;

	while (n > 0) {
		g = st->nseg > 0 ? &st->segs[st->seg_head] : NULL;
		lim = g != NULL ? g->at : st->wlen;
		if (st->wpos < lim) {
			k = lim - st->wpos < n ? lim - st->wpos : n;
			st->wpos += k;
			n -= k;
			continue;
		}
		if (g == NULL)
			break;           /* cannot happen: more sent than given */
		k = g->len - g->sent < n ? g->len - g->sent : n;
		g->sent += k;
		st->seg_pending -= k;
		n -= k;
		if (g->sent == g->len)
			seg_done(st);
	}
}

/* Append already-encoded RESP bytes (a pub/sub push) to the output.  A
 * subscriber that falls so far behind that the push does not fit under
 * max_output / the budget is disconnected (Redis's pubsub
 * client-output-buffer-limit): dropping the message silently would hand
 * it a stream with a hole in it. */
static void
conn_append_write(conn_state_t *st, const void *data, size_t len)
{
	resp_buf_t out;

	conn_out_compact(st);
	resp_buf_init(&out, st->wbuf, st->wcap);
	out.len = st->wlen;
	out.grow = conn_out_grow;
	out.grow_arg = st;
	if (resp_write_raw(&out, data, len) != 0) {
		XTC_LOG_WARN_F("rexis: subscriber output over its limit "
		    "(%zu bytes pending); disconnecting", st->wlen);
		st->closed = 1;
		return;
	}
	st->wlen = out.len;
}

/* Send as much pending output as the socket takes now. */
static void
conn_flush(conn_state_t *st)
{
	ssize_t n;

	if (st->wlen > st->wpeak)
		st->wpeak = st->wlen;
	while (conn_has_output(st)) {
		/* No by-reference pieces: the one-buffer send, as before. */
		n = st->nseg == 0 ? send(st->fd, st->wbuf + st->wpos,
		    st->wlen - st->wpos, MSG_DONTWAIT | MSG_NOSIGNAL) :
		    conn_sendv(st);
		if (n > 0) {
			conn_sent(st, (size_t)n);
			st->last_io_ns = xtc_clock_mono();
			continue;
		}
		if (n < 0 && errno == EINTR)
			continue;
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return;
		st->closed = 1;          /* EPIPE / ECONNRESET / ... */
		return;
	}
	st->wpos = st->wlen = 0;
}

/* Hysteresis shrink (see CONN_TRIM_NS): to the peak of the last window,
 * or, when idle, to what the buffers hold now. */
static void
conn_trim(conn_state_t *st, int idle)
{
	size_t t;

	/* Not while a large command is still arriving: its buffer was
	 * doubled ahead of rpeak and would only grow back. */
	if (idle || st->rlen <= CONN_BASE_BUF) {
		for (t = CONN_BASE_BUF; t < (idle ? st->rlen : st->rpeak);
		    t *= 2)
			;
		if (st->rcap > t)
			(void)conn_resize(st, &st->rbuf, &st->rcap, t);
	}
	for (t = CONN_BASE_BUF; !idle && t < st->wpeak; t *= 2)
		;
	if (st->wcap > t && !conn_has_output(st))
		(void)conn_resize(st, &st->wbuf, &st->wcap, t);
	if (idle && st->nseg == 0 && st->segs != NULL) {
		xtc_free(st->segs);
		st->segs = NULL;
		st->seg_cap = st->seg_head = 0;
	}
	st->rpeak = st->rlen;
	st->wpeak = st->wlen;
	st->trim_ns = st->last_io_ns;
}

/* One recv into the query buffer, growing it (up to max_request) when
 * one incomplete command fills it.  A grow the client budget refuses ends the connection with an
 * -OOM reply: the bytes would not be accounted otherwise, and
 * --max-memory has to bound what clients can make the server hold. */
static void
conn_try_read(conn_state_t *st)
{
	static const char oom_query[] =
	    "-OOM client query buffer exceeds the client memory budget\r\n";
	ssize_t n;
	size_t ncap, room;
	char c;

	/* No query buffer yet: allocate it only when a byte is there (a
	 * peek; one extra recv in the connection's life). */
	if (st->rcap == 0) {
		n = recv(st->fd, &c, 1, MSG_PEEK | MSG_DONTWAIT);
		if (n == 0) {
			st->eof = 1;
			return;
		}
		if (n < 0) {
			if (errno != EAGAIN && errno != EWOULDBLOCK &&
			    errno != EINTR)
				st->closed = 1;
			return;
		}
	}
	if (st->rlen == st->rcap) {
		/* Full of complete commands not yet run: run them first.
		 * Grow only for a single command larger than the buffer. */
		if (!st->partial)
			return;
		ncap = st->rcap == 0 ? CONN_BASE_BUF :   /* first read */
		    st->rcap > st->max_request / 2 ? st->max_request :
		    st->rcap * 2;
		if (ncap <= st->rcap)
			return;      /* at max_request: conn_process rejects */
		if (conn_resize(st, &st->rbuf, &st->rcap, ncap) != 0) {
			conn_append_write(st, oom_query, sizeof oom_query - 1);
			st->quit = 1;
			return;
		}
	}
again:
	room = st->rcap - st->rlen;
	n = recv(st->fd, st->rbuf + st->rlen, room, MSG_DONTWAIT);
	if (n > 0) {
		st->rlen += (size_t)n;
		st->last_io_ns = xtc_clock_mono();
		if (st->rlen > st->rpeak)
			st->rpeak = st->rlen;
		/* Filled it: read ahead (optional; a refused grow only
		 * stops reading). */
		if ((size_t)n == room && st->rcap < CONN_READ_AHEAD &&
		    st->rcap * 2 <= st->max_request &&
		    conn_resize(st, &st->rbuf, &st->rcap, st->rcap * 2) == 0)
			goto again;
	} else if (n == 0) {
		st->eof = 1;
	} else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
		st->closed = 1;
	}
}

/* Append an error reply; if even that cannot be buffered, give up on
 * the connection (it would otherwise get a reply-less command). */
static void
conn_reply_error(conn_state_t *st, resp_buf_t *out, const char *prefix,
    const char *msg)
{
	out->err = 0;
	if (resp_write_error_n(out, prefix, msg) != 0)
		st->closed = 1;
	st->wlen = out->len;
}

static int
conn_process_commands(conn_state_t *st)
{
	resp_parser_t parser;
	resp_value_t argv[MAX_ARGS];
	int argc, stop = STOP_NEED_MORE;
	size_t consumed, mark, mark_seg;
	resp_err_t rc;
	resp_buf_t out;
	cmd_ctx_t ctx;
	char msg[128];

	conn_out_compact(st);
	resp_parser_init(&parser, st->rbuf, st->rlen);
	parser.max_bulk = st->max_request;   /* refuse at the header */
	resp_buf_init(&out, st->wbuf, st->wcap);
	out.len = st->wlen;
	out.grow = conn_out_grow;
	out.grow_arg = st;

	while (!st->quit && !st->closed) {
		if (st->wlen + st->seg_pending >= st->out_soft) {
			stop = STOP_BACKPRESSURE;
			break;
		}
		/* Rate limit: one token per command, taken only when a
		 * complete command is actually parsed.  Out of tokens: stop
		 * here WITHOUT consuming the command; it stays buffered and
		 * runs after the listener refills the bucket (the client sees
		 * added latency, not an error).  A decrement that races below
		 * zero is handed back. */
		if (st->iops_cap > 0 && st->iops_tokens &&
		    xtc_atomic_i64_load(st->iops_tokens) <= 0) {
			stop = STOP_THROTTLED;
			break;
		}

		rc = resp_parse_command(&parser, argv, MAX_ARGS, &argc, &consumed);
		if (rc == RESP_NEED_MORE) {
			/* A command that does not fit in max_request bytes
			 * can never complete: refuse it rather than wait
			 * (and rather than spin on a full buffer). */
			if (parser.pos == 0 && st->rlen >= st->max_request)
				rc = RESP_ERR_TOOLARGE;
			else
				break;
		}

		if (rc == RESP_OK && st->iops_cap > 0 && st->iops_tokens &&
		    xtc_atomic_i64_add(st->iops_tokens, -1) <= 0) {
			(void)xtc_atomic_i64_add(st->iops_tokens, 1);
			parser.pos -= consumed;      /* not run: keep it */
			stop = STOP_THROTTLED;
			break;
		}

		XTC_INJECTION_POINT("rexis:parse_fail");

		if (rc != RESP_OK) {
			/* Malformed or oversized: reply and close, as Redis
			 * does -- the rest of the stream cannot be trusted. */
			if (rc == RESP_ERR_TOOLARGE)
				snprintf(msg, sizeof msg, "Protocol error: "
				    "request larger than %zu bytes or more than "
				    "%d arguments", st->max_request, MAX_ARGS);
			else
				snprintf(msg, sizeof msg, "Protocol error");
			conn_reply_error(st, &out, "ERR", msg);
			st->quit = 1;
			stop = STOP_QUIT;
			break;
		}
		if (argc == 0)
			continue;    /* blank inline line / "*0": no reply */

		ctx.db = st->db;
		ctx.out = &out;
		ctx.argc = argc;
		ctx.argv = argv;
		ctx.quit_flag = &st->quit;
		ctx.iops_tokens = st->iops_tokens;
		ctx.iops_cap = st->iops_cap;
		ctx.self = st->self;
		ctx.pubsub = st->pubsub;
		ctx.sub_count = &st->sub_count;
		ctx.out_ref = conn_out_ref;
		ctx.out_ref_arg = st;

		mark = out.len;
		mark_seg = st->nseg;
		XTC_INJECTION_POINT("rexis:before_cmd");
		(void)cmd_execute(&ctx);
		XTC_INJECTION_POINT("rexis:after_cmd");

		if (out.err) {
			/* The reply did not fit: drop ALL of it (a header
			 * without its body desynchronizes every reply after
			 * it) and answer with an error instead. */
			out.len = mark;
			seg_truncate(st, mark_seg);
			if (st->out_fail == OUT_FAIL_LIMIT) {
				snprintf(msg, sizeof msg, "reply exceeds the "
				    "client output limit (%zu bytes)",
				    st->max_output);
				conn_reply_error(st, &out, "ERR", msg);
			} else {
				conn_reply_error(st, &out, "OOM", "reply "
				    "exceeds the client memory budget");
			}
		}
		st->wlen = out.len;
	}

	/* Drop the consumed input once per pass (not per command). */
	if (parser.pos > 0) {
		memmove(st->rbuf, st->rbuf + parser.pos, st->rlen - parser.pos);
		st->rlen -= parser.pos;
	}
	if (st->quit)
		stop = STOP_QUIT;
	st->partial = stop == STOP_NEED_MORE;
	return stop;
}

/*
 * Connection teardown, run as an xtc_scope finalizer -- see conn_proc.
 * Leaves every pub/sub channel this connection subscribed to (so a
 * closed subscriber does not linger in the groups), closes the socket,
 * and frees (and refunds) the buffers and the state.  The scope runs it
 * exactly once, on whatever exit path the fiber takes (normal, error,
 * or async kill).
 */
static void
conn_teardown(void *arg)
{
	conn_state_t *st = arg;
	if (st == NULL)
		return;
	if (st->pubsub != NULL && st->sub_count > 0)
		(void)xtc_reg_drop_pid(st->pubsub, st->self);
	if (st->fd >= 0 && st->fd < g_fd_cap)
		atomic_store(&g_fd_pid[st->fd], 0);    /* before the close */
	if (st->fd >= 0)
		xtc_net_close(st->fd);
	if (st->conn_count != NULL)
		atomic_fetch_sub(st->conn_count, 1);   /* frees a --max-clients slot */
	atomic_fetch_sub(&g_loop_live[st->loop_idx], 1);
	seg_truncate(st, 0);
	xtc_free(st->segs);
	conn_refund(st, conn_charged(st->rcap) + conn_charged(st->wcap));
	xtc_free(st->rbuf);
	xtc_free(st->wbuf);
	xtc_free(st);
}

/* Connection proc entry point */
static void
conn_proc(void *arg)
{
	conn_state_t *st = arg;
	void *msg;
	size_t msg_len;
	xtc_scope_t *scope;
	uint32_t interest, revents;
	int64_t park_ns, now;
	int stop;

	st->self = xtc_self();   /* for pub/sub group membership */
	st->last_io_ns = st->trim_ns = xtc_clock_mono();
	if (st->fd < g_fd_cap)
		atomic_store(&g_fd_pid[st->fd], pid_pack(st->self));

	/*
	 * Resource scope (mechanism, not manner): register the connection
	 * teardown ONCE, up front, so it runs on EVERY exit path -- a normal
	 * return, an error, and -- crucially -- an ASYNCHRONOUS KILL from the
	 * supervisor while this fiber is parked in xtc_proc_wait_fd below.
	 * Before xtc_scope, the cleanup block at the bottom of this function
	 * was skipped on a kill, leaking the fd and both buffers; the scope
	 * closes them LIFO no matter how the fiber leaves.  See
	 * conn_teardown(). */
	scope = xtc_scope_open();
	if (scope != NULL)
		(void)xtc_scope_defer(scope, conn_teardown, st);

	/*
	 * One pass: flush what we can, read what fits, run the buffered
	 * pipeline while the pending output is under OUT_SOFT, flush again,
	 * then park.  The park interest is the backpressure: with output
	 * over OUT_SOFT the proc asks only for WRITABLE, so it neither reads
	 * nor runs more commands until the client drains its replies.
	 */
	while (!st->closed &&
	    (st->shutdown == NULL || !atomic_load(st->shutdown))) {
		conn_flush(st);
		if (st->closed)
			break;
		if (st->quit && !conn_has_output(st))
			break;

		/* Read only when there is room to answer: not under output
		 * backpressure, and not while --max-iops holds buffered
		 * commands (the query buffer would only grow). */
		if (!st->quit && !st->eof && conn_pending(st) < st->out_soft &&
		    !st->throttled)
			conn_try_read(st);
		if (st->closed)
			break;

		stop = STOP_NEED_MORE;
		if (!st->quit)
			stop = conn_process_commands(st);
		st->throttled = stop == STOP_THROTTLED;
		if (st->closed)
			break;
		conn_flush(st);
		if (st->closed)
			break;
		if (st->quit && !conn_has_output(st))
			break;

		/* Peer done sending and every complete command answered:
		 * close (a trailing partial command can never finish).
		 * EOF used to be noticed only by a recv that returned 0,
		 * and a full query buffer stopped the recvs, so a client
		 * that hung up mid-way through a big bulk left its proc
		 * spinning on a readable, closed socket forever. */
		if (st->eof && stop == STOP_NEED_MORE && !conn_has_output(st))
			break;

		/* Backpressure pass ended and the socket took enough of the
		 * output: run the rest of the buffered pipeline now (the
		 * client may have sent everything already, so no READABLE
		 * would ever come).  Yield first so one deep pipeline does not
		 * starve the other connections on this loop. */
		if (stop == STOP_BACKPRESSURE && conn_pending(st) < st->out_soft) {
			xtc_yield();
			continue;
		}
		interest = 0;
		if (conn_has_output(st))
			interest |= XTC_IO_WRITABLE;
		if (!st->quit && !st->eof && conn_pending(st) < st->out_soft &&
		    stop != STOP_THROTTLED)
			interest |= XTC_IO_READABLE;
		if (interest == 0) {
			/* Throttled (--max-iops) with commands buffered and
			 * nothing to send: no readiness would mean anything,
			 * so re-check the bucket in 10ms -- well under the 1s
			 * refill period, < 1% of the window.  (Also the EOF
			 * case with commands still to run.) */
			if (stop == STOP_THROTTLED)
				(void)xtc_proc_sleep(10LL * 1000 * 1000);
			else
				xtc_yield();
			continue;
		}
		/* No deadline unless something is due: a socket event, a
		 * published message or the shutdown scan (conn_wake_all)
		 * wakes the proc.  Every idle proc used to wake once a second
		 * to look at the shutdown flag: 100,000 idle connections cost
		 * half a core.  Deadlines: --max-iops re-checks in 10 ms,
		 * --timeout at the idle deadline, grown buffers at the trim
		 * window, and an fd the wake table cannot hold once a second. */
		park_ns = stop == STOP_THROTTLED ? 10LL * 1000 * 1000 :
		    st->fd < g_fd_cap ? -1 : 1000LL * 1000 * 1000;
		if (st->idle_timeout_ns > 0 && st->sub_count == 0) {
			now = st->last_io_ns + st->idle_timeout_ns -
			    xtc_clock_mono();
			if (now < 0)
				now = 0;
			if (park_ns < 0 || now < park_ns)
				park_ns = now;
		}
		if (st->last_io_ns - st->trim_ns >= CONN_TRIM_NS)
			conn_trim(st, 0);
		if ((st->rcap > CONN_BASE_BUF || st->wcap > CONN_BASE_BUF) &&
		    (park_ns < 0 || park_ns > CONN_TRIM_NS))
			park_ns = CONN_TRIM_NS;   /* to see idleness soon */
		revents = 0;
		if (xtc_proc_wait_fd(st->fd, interest | XTC_IO_HUP | XTC_IO_ERR,
		    park_ns, &revents) == XTC_E_AGAIN)
			atomic_fetch_add(&g_idle_wakes, 1);
		if ((st->rcap > CONN_BASE_BUF || st->wcap > CONN_BASE_BUF) &&
		    xtc_clock_mono() - st->last_io_ns >= CONN_TRIM_NS)
			conn_trim(st, 1);     /* idle: back to the base */
		/* ERR: the socket is dead.  HUP is left to the next recv /
		 * send to classify (kqueue reports a half-close as HUP, and
		 * the commands buffered before it still get their replies). */
		if (revents & XTC_IO_ERR)
			st->closed = 1;
		if (revents & XTC_WAIT_MAILBOX) {
			while (xtc_recv(&msg, &msg_len, 0) == XTC_OK) {
				/* A published message: append its RESP bytes to
				 * the output; the next pass flushes it. */
				if (msg != NULL && msg_len > 0 && !st->closed)
					conn_append_write(st, msg, msg_len);
				if (msg) xtc_free(msg);
			}
		}
		/* Redis `timeout`: no byte in either direction for that long
		 * (a half-sent command or an unread reply does not count as
		 * activity) closes the connection; subscribers are exempt. */
		if (st->idle_timeout_ns > 0 && st->sub_count == 0) {
			now = xtc_clock_mono();
			if (now - st->last_io_ns >= st->idle_timeout_ns)
				break;
		}
	}

	/* Final flush on shutdown / QUIT -- wait for actual writability
	 * rather than poll, bounded so a client that never reads cannot
	 * hold the shutdown. */
	while (conn_has_output(st) && !st->closed) {
		conn_flush(st);
		if (conn_has_output(st) && !st->closed) {
			revents = 0;
			if (xtc_proc_wait_fd(st->fd,
			    XTC_IO_WRITABLE | XTC_IO_HUP | XTC_IO_ERR,
			    100LL * 1000 * 1000, &revents) != XTC_OK ||
			    (revents & (XTC_IO_HUP | XTC_IO_ERR)))
				break;
		}
	}

	/* Normal-path teardown: closing the scope runs conn_teardown() (the
	 * finalizer registered above) exactly once, LIFO.  On an async kill
	 * while parked, the proc's exit path closes the still-open scope and
	 * runs the same finalizer -- so this one call and the kill path are
	 * the SAME cleanup, and neither can leak. */
	if (scope != NULL)
		xtc_scope_close(scope);
	else
		conn_teardown(st);   /* off a proc / scope exhausted: clean up directly */
}

int
conn_spawn(xtc_loop_t *loop, const conn_opts_t *opts, xtc_pid_t *out_pid)
{
	conn_state_t *st;
	xtc_proc_opts_t proc_opts = XTC_PROC_OPTS_INIT;
	int rc;

	if ((st = xtc_calloc(1, sizeof(*st))) == NULL)
		return XTC_E_NOMEM;

	st->fd = opts->fd;
	st->db = opts->db;
	st->res = opts->res;
	st->pubsub = opts->pubsub;
	st->client_res = opts->client_res;
	st->max_request = opts->max_request ? opts->max_request :
	    CONN_DEFAULT_MAX_REQUEST;
	st->max_output = opts->max_output ? opts->max_output :
	    CONN_DEFAULT_MAX_OUTPUT;
	if (st->max_request < CONN_BASE_BUF)
		st->max_request = CONN_BASE_BUF;
	if (st->max_output < CONN_BASE_BUF)
		st->max_output = CONN_BASE_BUF;
	st->out_soft = st->max_output / 2 < OUT_SOFT ? st->max_output / 2 :
	    OUT_SOFT;
	st->idle_timeout_ns = opts->idle_timeout_ns;
	st->partial = 1;

	/* rbuf / wbuf start NULL (rcap = wcap = 0): the first read and
	 * the first reply allocate them, so an idle connection that never
	 * sent a byte holds no buffer (measured: 100,000 idle connections
	 * held 15.6 KB each, about 8 KB of it these buffers' touched
	 * pages). */

	st->iops_tokens = opts->iops_tokens;
	st->iops_cap = opts->iops_cap;
	st->conn_count = opts->conn_count;
	st->shutdown = opts->shutdown;
	st->loop_idx = opts->loop_idx >= 0 && opts->loop_idx < CONN_MAX_LOOPS ?
	    opts->loop_idx : 0;

	proc_opts.name = "rexis-conn";

	/* Count BEFORE the spawn: the proc may run (and exit) on its own
	 * loop's thread before xtc_proc_spawn returns. */
	atomic_fetch_add(&g_loop_live[st->loop_idx], 1);
	rc = xtc_proc_spawn(loop, conn_proc, st, &proc_opts, out_pid);
	if (rc != XTC_OK) {
		atomic_fetch_sub(&g_loop_live[st->loop_idx], 1);
		/* Never ran, so conn_teardown never will: free here.  The fd
		 * stays the caller's (see conn.h). */
		xtc_free(st->rbuf);
		xtc_free(st->wbuf);
		xtc_free(st);
	}
	return rc;
}
