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

/* Connection state */
typedef struct conn_state {
	int         fd;
	db_t       *db;
	xtc_res_t  *res;

	/* Query buffer: rbuf[0..rlen) is received, unprocessed input.  It
	 * starts at CONN_BASE_BUF and grows (doubling) to hold one command
	 * of at most max_request bytes (or to CONN_READ_AHEAD for a deep
	 * pipeline); conn_trim shrinks it back. */
	char       *rbuf;
	size_t      rcap;
	size_t      rlen;
	size_t      rpeak;       /* largest rlen since the last trim */
	size_t      max_request;

	/* Output buffer: wbuf[wpos..wlen) is unsent reply bytes.  Grows up
	 * to max_output; conn_trim shrinks it back once drained. */
	char       *wbuf;
	size_t      wcap;
	size_t      wlen;
	size_t      wpos;
	size_t      wpeak;       /* largest wlen since the last trim */
	int64_t     trim_ns;     /* last_io_ns at the last trim */
	size_t      max_output;
	size_t      out_soft;
	int         out_fail;    /* OUT_FAIL_* of the last refused grow */
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

/* Resize *buf to ncap bytes (contents kept up to the smaller size),
 * charging or refunding the difference.  -1: budget or allocator said
 * no; *buf is unchanged. */
static int
conn_resize(conn_state_t *st, char **buf, size_t *cap, size_t ncap)
{
	char *nb;

	if (ncap == *cap)
		return 0;
	/* Only bytes above CONN_BASE_BUF are charged (see client_res). */
	if (ncap > *cap && conn_charge(st, ncap - *cap) != 0) {
		st->out_fail = OUT_FAIL_MEM;
		return -1;
	}
	if ((nb = xtc_realloc(*buf, ncap)) == NULL) {
		if (ncap > *cap)
			conn_refund(st, ncap - *cap);
		st->out_fail = OUT_FAIL_MEM;
		return -1;
	}
	if (ncap < *cap)
		conn_refund(st, *cap - ncap);
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

	if (need > st->max_output) {
		st->out_fail = OUT_FAIL_LIMIT;
		return -1;
	}
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
	if (st->wpos == 0)
		return;
	memmove(st->wbuf, st->wbuf + st->wpos, st->wlen - st->wpos);
	st->wlen -= st->wpos;
	st->wpos = 0;
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
	while (st->wpos < st->wlen) {
		n = send(st->fd, st->wbuf + st->wpos, st->wlen - st->wpos,
		    MSG_DONTWAIT | MSG_NOSIGNAL);
		if (n > 0) {
			st->wpos += (size_t)n;
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
	if (st->wcap > t && st->wpos == st->wlen)
		(void)conn_resize(st, &st->wbuf, &st->wcap, t);
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

	if (st->rlen == st->rcap) {
		/* Full of complete commands not yet run: run them first.
		 * Grow only for a single command larger than the buffer. */
		if (!st->partial)
			return;
		ncap = st->rcap > st->max_request / 2 ? st->max_request :
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
	size_t consumed, mark;
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
		if (st->wlen >= st->out_soft) {
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

		mark = out.len;
		XTC_INJECTION_POINT("rexis:before_cmd");
		(void)cmd_execute(&ctx);
		XTC_INJECTION_POINT("rexis:after_cmd");

		if (out.err) {
			/* The reply did not fit: drop ALL of it (a header
			 * without its body desynchronizes every reply after
			 * it) and answer with an error instead. */
			out.len = mark;
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
	if (st->fd >= 0)
		xtc_net_close(st->fd);
	if (st->conn_count != NULL)
		atomic_fetch_sub(st->conn_count, 1);   /* frees a --max-clients slot */
	atomic_fetch_sub(&g_loop_live[st->loop_idx], 1);
	conn_refund(st, st->rcap + st->wcap - 2 * CONN_BASE_BUF);
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
		if (st->quit && st->wpos == st->wlen)
			break;

		/* Read only when there is room to answer: not under output
		 * backpressure, and not while --max-iops holds buffered
		 * commands (the query buffer would only grow). */
		if (!st->quit && !st->eof && st->wlen - st->wpos < st->out_soft &&
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
		if (st->quit && st->wpos == st->wlen)
			break;

		/* Peer done sending and every complete command answered:
		 * close (a trailing partial command can never finish).
		 * EOF used to be noticed only by a recv that returned 0,
		 * and a full query buffer stopped the recvs, so a client
		 * that hung up mid-way through a big bulk left its proc
		 * spinning on a readable, closed socket forever. */
		if (st->eof && stop == STOP_NEED_MORE && st->wpos == st->wlen)
			break;

		/* Backpressure pass ended and the socket took enough of the
		 * output: run the rest of the buffered pipeline now (the
		 * client may have sent everything already, so no READABLE
		 * would ever come).  Yield first so one deep pipeline does not
		 * starve the other connections on this loop. */
		if (stop == STOP_BACKPRESSURE && st->wlen - st->wpos < st->out_soft) {
			xtc_yield();
			continue;
		}
		interest = 0;
		if (st->wpos < st->wlen)
			interest |= XTC_IO_WRITABLE;
		if (!st->quit && !st->eof && st->wlen - st->wpos < st->out_soft &&
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
		/* Wake at least once a second to see shutdown and the idle
		 * clock. */
		park_ns = stop == STOP_THROTTLED ? 10LL * 1000 * 1000 :
		    1000LL * 1000 * 1000;
		if (st->last_io_ns - st->trim_ns >= CONN_TRIM_NS)
			conn_trim(st, 0);
		if ((st->rcap > CONN_BASE_BUF || st->wcap > CONN_BASE_BUF) &&
		    park_ns > CONN_TRIM_NS)
			park_ns = CONN_TRIM_NS;   /* to see idleness soon */
		revents = 0;
		(void)xtc_proc_wait_fd(st->fd,
		    interest | XTC_IO_HUP | XTC_IO_ERR, park_ns, &revents);
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
	while (st->wpos < st->wlen && !st->closed) {
		conn_flush(st);
		if (st->wpos < st->wlen && !st->closed) {
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
	xtc_proc_opts_t proc_opts = { 0 };
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

	st->rcap = st->wcap = CONN_BASE_BUF;
	if ((st->rbuf = xtc_malloc(st->rcap)) == NULL ||
	    (st->wbuf = xtc_malloc(st->wcap)) == NULL) {
		xtc_free(st->rbuf);
		xtc_free(st);
		return XTC_E_NOMEM;
	}

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
