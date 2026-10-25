/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/06_sqlxtc/conn.c
 *	Per-connection xtc_proc implementation.  Read newline-delimited
 *	JSON, parse via Quack, dispatch to db_exec, stream the result
 *	back as Quack.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "admin.h"
#include "conn.h"
#include "quack.h"
#include "sql_parse.h"
#include "xtc_io.h"
#include "xtc_net.h"
#include "xtc_log.h"
#include "xtc_stats.h"

/* Query instrumentation; defined in metrics.c.  NULL-checked
 * before use so the connection path runs even if registration
 * failed. */
extern xtc_counter_t *sqlxtc_stat_query_total;
extern xtc_counter_t *sqlxtc_stat_query_errors;
extern xtc_hist_t    *sqlxtc_stat_query_latency;

/* Local helper wrapping xtc_clock_mono(). */
static inline int64_t xtc_now_ns(void) {
	int64_t t; t = xtc_clock_mono(); return t;
}

/* Case-insensitive match of the SQL text -- trimmed of leading and
 * trailing spaces -- against an admin keyword.  Used to intercept
 * admin commands (e.g. "SHOW PROCESSES") before the SQL engine. */
static int
sql_text_is(const char *q, size_t n, const char *kw)
{
	size_t i = 0, j = n, k, klen = strlen(kw);
	while (i < j && q[i] == ' ') i++;
	while (j > i && q[j - 1] == ' ') j--;
	if (j - i != klen) return 0;
	for (k = 0; k < klen; k++) {
		char c = q[i + k];
		if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
		if (c != kw[k]) return 0;
	}
	return 1;
}

#define SQLXTC_VERSION "0.1"

#define DEFAULT_READ_CAP   (16 * 1024)
#define DEFAULT_WRITE_CAP  (64 * 1024)
#define DEFAULT_MAX_LINE   (1024 * 1024)
#define CONN_OUT_SOFT      (256 * 1024)  /* stop running requests past this */

typedef struct conn_state {
	int        fd;
	db_t      *db;
	xtc_res_t *res;
	struct server *server;

	/* read buffer (line-oriented; we look for \n) */
	char      *rbuf;
	size_t     rcap;
	size_t     rlen;
	size_t     max_line;

	/* write buffer (Quack-encoded responses) */
	quack_buf_t wbuf;
	size_t      wpos;       /* bytes already sent */

	/* rate limit (token bucket pointer) */
	int64_t   *iops_tokens;
	int64_t    iops_cap;

	int64_t    max_memory;
	int        mem_refused;   /* a buffer grow was refused by res */

	int        quit;
	int        eof;           /* the peer shut its write side */
	int        closed;

	/* Prepared-statement cache: up to CONN_STMT_CACHE statements by SQL
	 * text, each kept (with its parse and plan) and reused -- reset +
	 * re-bind -- when the same text arrives again.  An OLTP client cycles
	 * through a few dozen texts (Turso's harness: 35), so one slot always
	 * missed.  Replacement: the least recently used.  Finalized when the
	 * connection closes. */
	struct {
		char     *sql;
		sx_stmt  *stmt;
		uint64_t  used;
	}          cache[64];
	uint64_t   cache_tick;

	/* Connection-lifetime engine handle (opened in conn_proc, closed at
	 * teardown).  h_owned == 1 when this is a private per-connection
	 * handle (connection-per-proc), 0 when it is the shared handle. */
	sx_db     *h_conn;
	int        h_owned;
} conn_state_t;

/* Forward declared in main.c */
struct server;
extern void server_inc_conn(struct server *);
extern void server_dec_conn(struct server *);

/*
 * Connection buffer memory is charged to the server's xtc_res
 * MEM_BYTES budget (--max-memory): every byte either buffer grows past
 * its initial size, given back when it shrinks or the connection
 * closes.  The initial buffers are not charged (their total is bounded
 * by --max-clients), as in rexis.  Nothing else in sqlxtc charges that
 * budget, so the check before each query used to compare a counter that
 * was always 0: a SELECT emitted its whole result, however large.
 */
static int
conn_charge(void *arg, int64_t delta)
{
	conn_state_t *st = arg;
	if (st->res == NULL)
		return 0;
	if (delta < 0) {
		xtc_res_release(st->res, XTC_RES_MEM_BYTES, -delta);
		return 0;
	}
	if (xtc_res_acquire(st->res, XTC_RES_MEM_BYTES, delta) != XTC_OK) {
		st->mem_refused = 1;
		return -1;
	}
	return 0;
}

/* A reply that the budget refused: drop all of it (a partial result
 * would desynchronize the stream), answer {"err":"OOM"} and close once
 * that is flushed (PROTOCOL.md).  If even the error line cannot be
 * buffered, close without it. */
static void
conn_refuse(conn_state_t *st, size_t mark)
{
	st->wbuf.len = mark;
	if (quack_emit_err(&st->wbuf, "OOM") < 0)
		st->closed = 1;
	st->quit = 1;
}

static int
try_read(conn_state_t *st)
{
	ssize_t n;
	size_t avail;

	if (st->rlen >= st->rcap) {
		size_t newcap = st->rcap * 2;
		char  *nb;
		if (newcap > st->max_line) {
			st->closed = 1;
			return -1;
		}
		if (conn_charge(st, (int64_t)(newcap - st->rcap)) != 0) {
			conn_refuse(st, st->wbuf.len);
			return -1;
		}
		nb = (char *)realloc(st->rbuf, newcap);
		if (!nb) {
			(void)conn_charge(st, -(int64_t)(newcap - st->rcap));
			st->closed = 1;
			return -1;
		}
		st->rbuf = nb;
		st->rcap = newcap;
	}

	avail = st->rcap - st->rlen;
	n = recv(st->fd, st->rbuf + st->rlen, avail, MSG_DONTWAIT);
	if (n > 0) { st->rlen += (size_t)n; return 0; }
	/* EOF is a half-close, not an error: the requests already
	 * buffered still get their whole replies. */
	if (n == 0) { st->eof = 1; return 0; }
	if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
	st->closed = 1;
	return -1;
}

static int
try_write(conn_state_t *st)
{
	while (st->wpos < st->wbuf.len) {
		ssize_t n = send(st->fd, st->wbuf.p + st->wpos,
		                 st->wbuf.len - st->wpos, MSG_DONTWAIT);
		if (n > 0) { st->wpos += (size_t)n; continue; }
		if (n == 0) break;
		if (errno == EAGAIN || errno == EWOULDBLOCK) break;
		st->closed = 1;
		return -1;
	}
	if (st->wpos >= st->wbuf.len) {
		st->wbuf.len = 0;
		st->wpos = 0;
	}
	return 0;
}

/* The cache slot holding `sql` (q, n), or -1. */
static int
conn_cache_find(conn_state_t *st, const char *q, size_t n)
{
	int i;
	for (i = 0; i < (int)(sizeof st->cache / sizeof st->cache[0]); i++)
		if (st->cache[i].sql != NULL && strlen(st->cache[i].sql) == n &&
		    memcmp(st->cache[i].sql, q, n) == 0)
			return i;
	return -1;
}

/* A slot for new text `sql`: an empty one, else the least recently
 * used (its statement finalized).  -1 on allocation failure. */
static int
conn_cache_take(conn_state_t *st, const char *sql)
{
	int i, v = 0;
	for (i = 0; i < (int)(sizeof st->cache / sizeof st->cache[0]); i++) {
		if (st->cache[i].sql == NULL) { v = i; break; }
		if (st->cache[i].used < st->cache[v].used) v = i;
	}
	if (st->cache[v].stmt != NULL) sx_finalize(st->cache[v].stmt);
	free(st->cache[v].sql);
	st->cache[v].stmt = NULL;
	st->cache[v].sql = strdup(sql);
	return st->cache[v].sql != NULL ? v : -1;
}

/* Execute msg through cache slot `slot` (preparing on first use). */
static void
conn_run(conn_state_t *st, sx_db *h, int slot, const quack_msg_t *msg)
{
	int64_t t0 = xtc_now_ns(), rows = 0;
	char *err = NULL;
	int exec_rc;
	const struct quack_param *bp = msg->n_params > 0 ? msg->params : NULL;
	st->cache[slot].used = ++st->cache_tick;
	exec_rc = db_exec_cached(h, &st->cache[slot].stmt, st->cache[slot].sql,
	    bp, msg->n_params, msg->limit, &st->wbuf, &rows, &err);
	if (sqlxtc_stat_query_total != NULL)
		xtc_counter_inc(sqlxtc_stat_query_total);
	if (sqlxtc_stat_query_latency != NULL)
		xtc_hist_record(sqlxtc_stat_query_latency, xtc_now_ns() - t0);
	if (exec_rc < 0) {
		if (sqlxtc_stat_query_errors != NULL)
			xtc_counter_inc(sqlxtc_stat_query_errors);
		quack_emit_err(&st->wbuf, err ? err : "exec failed");
		free(err);
		/* A statement that failed to prepare is not kept: the next
		 * arrival of the text re-validates it (a table may appear). */
		if (st->cache[slot].stmt == NULL) {
			free(st->cache[slot].sql);
			st->cache[slot].sql = NULL;
		}
	}
}

static void
handle_query(conn_state_t *st, const quack_msg_t *msg)
{
	sx_db *h = st->h_conn;
	sql_info_t info;
	char    *normalized = NULL;
	const char *sql_to_exec = msg->q;
	size_t      sql_len     = msg->q_len;
	int      slot;

	/* A statement text seen before skips the classifier and prepare. */
	slot = conn_cache_find(st, msg->q, msg->q_len);
	if (slot >= 0) {
		conn_run(st, h, slot, msg);
		return;
	}

	/* Pre-parse: validate and route. */
	if (sql_parse(msg->q, msg->q_len, &info) < 0) {
		quack_emit_err(&st->wbuf, info.err ? info.err :
		                                     "syntax error");
		return;
	}

	/* Round-trip canonicalization (Phase 2 entry-point; in Phase 1
	 * this is identity). */
	normalized = sql_canonicalize(msg->q, msg->q_len);
	if (normalized) {
		sql_to_exec = normalized;
		sql_len = strlen(normalized);
	}
	(void)sql_len;

	if (h == NULL) {
		quack_emit_err(&st->wbuf, "no db handle");
		if (sqlxtc_stat_query_errors != NULL)
			xtc_counter_inc(sqlxtc_stat_query_errors);
		free(normalized);
		return;
	}

	/* The connection's handle is stable for its lifetime, so a cached
	 * statement stays valid; take the least recently used slot. */
	slot = conn_cache_take(st, sql_to_exec);
	if (slot < 0) {
		quack_emit_err(&st->wbuf, "oom");
		free(normalized);
		return;
	}
	conn_run(st, h, slot, msg);
	free(normalized);
}

/* Process as many full lines as available in rbuf.  Returns 0
 * on success.  Stops early if the write buffer grows large; the
 * conn loop will flush before continuing. */
static int
process_lines(conn_state_t *st)
{
	while (!st->quit && !st->closed) {
		char *nl;
		size_t line_len, mark = st->wbuf.len;
		quack_msg_t msg;

		if (st->wbuf.len > CONN_OUT_SOFT) break;  /* let writer drain */

		nl = (char *)memchr(st->rbuf, '\n', st->rlen);
		if (!nl) break;
		line_len = (size_t)(nl - st->rbuf);

		/* Parse */
		if (quack_parse(st->rbuf, line_len, &msg) < 0) {
			quack_emit_err(&st->wbuf, msg.err ? msg.err :
			                                     "json error");
			goto consume;
		}

		switch (msg.kind) {
		case QUACK_MSG_PING:
			quack_emit_pong(&st->wbuf);
			break;
		case QUACK_MSG_QUIT:
			st->quit = 1;
			break;
		case QUACK_MSG_QUERY:
			/* Admin command: SHOW PROCESSES bypasses the DB
			 * query path entirely. */
			if (sql_text_is(msg.q, msg.q_len,
			                "SHOW PROCESSES")) {
				admin_show_processes(&st->wbuf);
				break;
			}
			/* Rate limit: take a token in one atomic step (a
			 * load, then a decrement, let two loops spend the
			 * last token); no token: give it back. */
			if (st->iops_cap > 0 && st->iops_tokens &&
			    xtc_atomic_i64_add(st->iops_tokens, -1) <= 0) {
				(void)xtc_atomic_i64_add(st->iops_tokens, 1);
				quack_emit_err(&st->wbuf, "OVER_LIMIT");
				break;
			}
			/* Memory check */
			if (st->res && st->max_memory > 0) {
				int64_t used = xtc_res_used(st->res,
				    XTC_RES_MEM_BYTES);
				if (used >= st->max_memory) {
					/* quit, not closed: the error is flushed */
					conn_refuse(st, st->wbuf.len);
					break;
				}
			}
			/* The whole request is the unit of kill delivery, not
			 * only sx_step (which masks itself): prepare, reset and
			 * the cache's finalize run unmasked, and a kill landing
			 * in one of them with a latch held leaves it held -- no
			 * teardown can release it. */
			(void)xtc_mask_enter();
			handle_query(st, &msg);
			(void)xtc_mask_leave();
			break;
		case QUACK_MSG_UNKNOWN:
			quack_emit_err(&st->wbuf, "unknown message");
			break;
		default:
			quack_emit_err(&st->wbuf, "empty message");
			break;
		}
		/* The budget refused a grow somewhere in this reply. */
		if (st->mem_refused)
			conn_refuse(st, mark);

	consume:
		/* Compact: drop the consumed line + newline. */
		{
			size_t consumed = line_len + 1;
			if (st->rlen > consumed) {
				memmove(st->rbuf, st->rbuf + consumed,
				        st->rlen - consumed);
			}
			st->rlen -= consumed;
		}
	}
	return 0;
}

/*
 * Connection teardown: finalize the cached statements, close the engine
 * handle (sx_close discards an open transaction's write set and frees
 * its snapshot slot and row intents, as a client disconnect does), give
 * the buffers back to the budget, close the socket, free the --max-
 * clients slot.  Registered with xtc_proc_at_exit, so it runs exactly
 * once on EVERY exit path: a normal return, and an xtc_exit_pid kill.
 * Before, it was straight-line code at the end of conn_proc and a kill
 * unwound past it: the transaction stayed open for the life of the
 * server (version GC pinned behind its snapshot, its rows' writers
 * waiting out the 10 s intent wait), and the fd, the buffers' budget
 * and the conn slot leaked.  A kill reaches the fiber only between
 * statements (handle_query runs masked), so this always runs from the
 * state a disconnect tears down from.
 */
static void
conn_teardown(void *arg)
{
	conn_state_t *st = arg;
	int ci;

	if (st->server) server_dec_conn(st->server);

	/* Finalize the cached statements before closing their handle. */
	for (ci = 0; ci < (int)(sizeof st->cache / sizeof st->cache[0]); ci++) {
		if (st->cache[ci].stmt != NULL) sx_finalize(st->cache[ci].stmt);
		free(st->cache[ci].sql);
	}
	if (st->h_conn != NULL)
		db_conn_close(st->db, st->h_conn, st->h_owned);

	/* Give the buffers back to the budget BEFORE the close: a client
	 * that sees EOF and reconnects must find the bytes released. */
	if (st->rcap > DEFAULT_READ_CAP)
		(void)conn_charge(st, -(int64_t)(st->rcap - DEFAULT_READ_CAP));
	free(st->rbuf);
	quack_buf_free(&st->wbuf);
	xtc_net_close(st->fd);   /* pairs with xtc_net_accept (fd accounting) */
	free(st);
}

static void
conn_proc(void *arg)
{
	conn_state_t *st = arg;
	void *msg; size_t msg_len;
	uint32_t want, revents;
	int more, hooked;

	/* First, so a kill anywhere below still tears down (see
	 * conn_teardown); without the hook, tear down on return. */
	hooked = xtc_proc_at_exit(conn_teardown, st) == XTC_OK;

	/* Open this connection's engine handle once, for its whole life.
	 * In connection-per-proc mode this is a private handle on the loop
	 * the proc runs on, so connections execute SQL in parallel.
	 * Masked: a kill inside the open would leak a half-registered
	 * handle. */
	(void)xtc_mask_enter();
	if (db_conn_open(st->db, &st->h_conn, &st->h_owned) != XTC_OK) {
		st->h_conn = NULL;
		quack_emit_err(&st->wbuf, "no db handle");
	}
	(void)xtc_mask_leave();

	/* Send banner. */
	quack_emit_hello(&st->wbuf, SQLXTC_VERSION);

	while (!st->quit && !st->closed) {
		try_write(st);
		if (st->closed) break;

		if (!st->eof)
			try_read(st);
		if (st->closed) break;

		process_lines(st);

		try_write(st);

		if (st->quit && st->wbuf.len == st->wpos) break;

		/* A complete request still buffered (a pass stops past
		 * CONN_OUT_SOFT of output): with room to answer, run it now
		 * -- the client may have sent everything already, so no
		 * READABLE would come and only the park timeout woke us
		 * (1 s per stop).  Yield first, so one pipeline does not
		 * starve the other connections on this loop. */
		more = memchr(st->rbuf, '\n', st->rlen) != NULL;
		if (more && st->wbuf.len <= CONN_OUT_SOFT) {
			xtc_yield();
			continue;
		}
		/* Half-closed and every request answered: done. */
		if (st->eof && !more && st->wbuf.len == st->wpos) break;

		/* Nothing left to send: give a grown write buffer back to
		 * the budget before waiting for the next request.
		 * shortcut: a client whose every result passes 64 KiB pays a
		 * realloc up and down per query; trim on a timer (rexis's
		 * conn_trim) if that shows up in a profile. */
		if (st->wbuf.len == 0)
			quack_buf_trim(&st->wbuf);

		/* Wait for inbound bytes, room to send, or a message.
		 * Wakes exactly on fd readiness; no busy-poll.  Once the
		 * peer has half-closed, only writability matters (a
		 * readable EOF, or a HUP on kqueue, would wake at once). */
		want = XTC_IO_ERR;
		if (!st->eof)
			want |= XTC_IO_READABLE | XTC_IO_HUP;
		if (st->wbuf.len > st->wpos)
			want |= XTC_IO_WRITABLE;
		revents = 0;
		(void)xtc_proc_wait_fd(st->fd, want, 1000LL * 1000 * 1000,
		    &revents);
		if (revents & XTC_WAIT_MAILBOX) {
			while (xtc_recv(&msg, &msg_len, 0) == XTC_OK) {
				if (msg) xtc_free(msg);
			}
		}
	}

	/* Final flush (QUIT, or an OOM refusal): wait for writability,
	 * not a fixed sleep, giving up once the client has read nothing
	 * for a second -- one that stops reading cannot hold the proc. */
	while (st->wbuf.len > st->wpos && !st->closed) {
		try_write(st);
		if (st->wbuf.len <= st->wpos || st->closed)
			break;
		revents = 0;
		if (xtc_proc_wait_fd(st->fd, XTC_IO_WRITABLE | XTC_IO_ERR,
		    1000LL * 1000 * 1000, &revents) != XTC_OK ||
		    (revents & XTC_IO_ERR))
			break;
		if (revents & XTC_WAIT_MAILBOX) {   /* else it wakes at once */
			while (xtc_recv(&msg, &msg_len, 0) == XTC_OK) {
				if (msg) xtc_free(msg);
			}
		}
	}

	if (!hooked)
		conn_teardown(st);   /* else the at-exit hook runs it */
}

int
conn_spawn(xtc_loop_t *loop, const conn_opts_t *opts, xtc_pid_t *out_pid)
{
	conn_state_t *st;
	xtc_proc_opts_t po = { 0 };
	int rc;

	st = (conn_state_t *)calloc(1, sizeof *st);
	if (!st) return XTC_E_NOMEM;
	st->fd = opts->fd;
	st->db = opts->db;
	st->res = opts->res;
	st->server = opts->server;
	st->iops_tokens = opts->iops_tokens;
	st->iops_cap = opts->iops_cap;
	st->max_memory = opts->max_memory;
	st->max_line = opts->max_line_bytes ? opts->max_line_bytes :
	                                       DEFAULT_MAX_LINE;
	st->rcap = DEFAULT_READ_CAP;
	st->rbuf = (char *)malloc(st->rcap);
	if (!st->rbuf) { free(st); return XTC_E_NOMEM; }
	if (quack_buf_init(&st->wbuf, DEFAULT_WRITE_CAP) < 0) {
		free(st->rbuf); free(st); return XTC_E_NOMEM;
	}
	st->wbuf.charge = conn_charge;
	st->wbuf.charge_arg = st;

	po.name = "sqlxtc-conn";
	if ((rc = xtc_proc_spawn(loop, conn_proc, st, &po, out_pid)) != XTC_OK) {
		quack_buf_free(&st->wbuf);     /* the proc never ran: ours */
		free(st->rbuf);
		free(st);
	}
	return rc;
}
