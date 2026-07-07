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
#include "xtc_int.h"
#include "xtc_io.h"
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

	int        quit;
	int        closed;

	/* Prepared-statement cache (one slot): cached_stmt holds the
	 * statement last prepared for cached_sql on the connection's handle;
	 * reused (reset + re-bind) when the next query text matches.
	 * Finalized when the text changes or the connection closes. */
	char      *cached_sql;
	sx_stmt   *cached_stmt;

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
extern int  server_take_iops(struct server *, int);

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
		nb = (char *)realloc(st->rbuf, newcap);
		if (!nb) { st->closed = 1; return -1; }
		st->rbuf = nb;
		st->rcap = newcap;
	}

	avail = st->rcap - st->rlen;
	n = recv(st->fd, st->rbuf + st->rlen, avail, MSG_DONTWAIT);
	if (n > 0) { st->rlen += (size_t)n; return 0; }
	if (n == 0) { st->closed = 1; return 0; }
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

static void
handle_query(conn_state_t *st, const quack_msg_t *msg)
{
	sx_db *h = st->h_conn;
	int64_t  rows = 0;
	char    *err = NULL;
	sql_info_t info;
	char    *normalized = NULL;
	const char *sql_to_exec = msg->q;
	size_t      sql_len     = msg->q_len;

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

	{
		int64_t t0 = xtc_now_ns();
		int exec_rc;
		const struct quack_param *bp =
		    msg->n_params > 0 ? msg->params : NULL;
		/* The connection's handle is stable for its lifetime, so the
		 * prepared-statement cache is always valid.  Refresh the slot
		 * when the query text changes. */
		if (st->cached_sql == NULL ||
		    strcmp(st->cached_sql, sql_to_exec) != 0) {
			if (st->cached_stmt != NULL) {
				sx_finalize(st->cached_stmt);
				st->cached_stmt = NULL;
			}
			free(st->cached_sql);
			st->cached_sql = strdup(sql_to_exec);
		}
		exec_rc = db_exec_cached(h, &st->cached_stmt, sql_to_exec,
		    bp, msg->n_params, msg->limit, &st->wbuf, &rows, &err);
		if (sqlxtc_stat_query_total != NULL)
			xtc_counter_inc(sqlxtc_stat_query_total);
		if (sqlxtc_stat_query_latency != NULL)
			xtc_hist_record(sqlxtc_stat_query_latency,
			    xtc_now_ns() - t0);
		if (exec_rc < 0) {
			if (sqlxtc_stat_query_errors != NULL)
				xtc_counter_inc(sqlxtc_stat_query_errors);
			if (err) {
				quack_emit_err(&st->wbuf, err);
				free(err);
			} else {
				quack_emit_err(&st->wbuf, "exec failed");
			}
		}
	}

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
		size_t line_len;
		quack_msg_t msg;

		if (st->wbuf.len > 256 * 1024) break;     /* let writer drain */

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
			/* Rate limit */
			if (st->iops_cap > 0 && st->iops_tokens) {
				int64_t t = xtc_atomic_i64_load(st->iops_tokens);
				if (t <= 0) {
					quack_emit_err(&st->wbuf,
					    "OVER_LIMIT");
					break;
				}
				xtc_atomic_i64_add(st->iops_tokens,
				                          -1);
			}
			/* Memory check */
			if (st->res && st->max_memory > 0) {
				int64_t used = xtc_res_used(st->res,
				    XTC_RES_MEM_BYTES);
				if (used >= st->max_memory) {
					quack_emit_err(&st->wbuf, "OOM");
					st->closed = 1;
					break;
				}
			}
			handle_query(st, &msg);
			break;
		case QUACK_MSG_UNKNOWN:
			quack_emit_err(&st->wbuf, "unknown message");
			break;
		default:
			quack_emit_err(&st->wbuf, "empty message");
			break;
		}

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

static void
conn_proc(void *arg)
{
	conn_state_t *st = arg;
	void *msg; size_t msg_len;

	/* Open this connection's engine handle once, for its whole life.
	 * In connection-per-proc mode this is a private handle on the loop
	 * the proc runs on, so connections execute SQL in parallel. */
	if (db_conn_open(st->db, &st->h_conn, &st->h_owned) != XTC_OK) {
		st->h_conn = NULL;
		quack_emit_err(&st->wbuf, "no db handle");
	}

	/* Send banner. */
	quack_emit_hello(&st->wbuf, SQLXTC_VERSION);

	while (!st->quit && !st->closed) {
		try_write(st);
		if (st->closed) break;

		try_read(st);
		if (st->closed && st->rlen == 0) break;

		process_lines(st);

		try_write(st);

		if (st->quit && st->wbuf.len == st->wpos) break;

		/* Wait for inbound bytes (or the quit/shutdown signal).
		 * Wakes exactly on fd readiness; no busy-poll. */
		{
			uint32_t revents = 0;
			uint32_t want = XTC_IO_READABLE | XTC_IO_HUP | XTC_IO_ERR;
			if (st->wbuf.len > st->wpos)
				want |= XTC_IO_WRITABLE;
			(void)xtc_proc_wait_fd(st->fd, want,
			    1000LL * 1000 * 1000,
			    &revents);
			if (revents & XTC_WAIT_MAILBOX) {
				while (xtc_recv(&msg, &msg_len, 0) == XTC_OK) {
					if (msg) xtc_free(msg);
				}
			}
		}
	}

	/* Final flush. */
	{
		int spins = 0;
		while (st->wbuf.len > st->wpos && !st->closed && spins < 50) {
			try_write(st);
			if (st->wbuf.len > st->wpos) {
				(void)xtc_recv(&msg, &msg_len,
				    5LL * 1000 * 1000);
				if (msg) xtc_free(msg);
			}
			spins++;
		}
	}

	if (st->server) server_dec_conn(st->server);

	/* Release the cached prepared statement and the connection handle
	 * (finalize the statement before closing its handle). */
	if (st->cached_stmt != NULL)
		sx_finalize(st->cached_stmt);
	free(st->cached_sql);
	if (st->h_conn != NULL)
		db_conn_close(st->db, st->h_conn, st->h_owned);

	close(st->fd);
	free(st->rbuf);
	quack_buf_free(&st->wbuf);
	free(st);
}

int
conn_spawn(xtc_loop_t *loop, const conn_opts_t *opts, xtc_pid_t *out_pid)
{
	conn_state_t *st;
	xtc_proc_opts_t po = { 0 };

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

	po.name = "sqlxtc-conn";
	return xtc_proc_spawn(loop, conn_proc, st, &po, out_pid);
}
