/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * examples/05_rexis/conn.c
 *	Per-connection xtc_proc implementation.
 */

#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <sys/socket.h>

#include "conn.h"
#include "proto.h"
#include "cmd.h"
#include "xtc_io.h"
#include "xtc_inject.h"
#include "xtc_int.h"

#define DEFAULT_READ_BUF   (64 * 1024)
#define DEFAULT_WRITE_BUF  (64 * 1024)
#define MAX_ARGS           128

/* Connection state */
typedef struct conn_state {
	int         fd;
	db_t       *db;
	xtc_res_t  *res;

	/* Buffers */
	char       *read_buf;
	size_t      read_cap;
	size_t      read_len;
	size_t      max_read_buf;

	char       *write_buf;
	size_t      write_cap;
	size_t      write_len;
	size_t      write_pos;
	size_t      max_write_buf;

	/* Rate limiting */
	int64_t    *iops_tokens;
	int64_t     iops_cap;

	/* Flags */
	int         quit;
	int         closed;
} conn_state_t;

static int
conn_try_read(conn_state_t *st)
{
	ssize_t n;
	size_t avail;

	if (st->read_len >= st->read_cap) {
		/* Grow buffer */
		size_t new_cap = st->read_cap * 2;
		char *new_buf;
		if (new_cap > st->max_read_buf)
			return -1;
		new_buf = NULL;
		if (__os_realloc(st->read_buf, new_cap, (void **)&new_buf) != XTC_OK ||
		    !new_buf)
			return -1;
		st->read_buf = new_buf;
		st->read_cap = new_cap;
	}

	avail = st->read_cap - st->read_len;
	n = recv(st->fd, st->read_buf + st->read_len, avail, MSG_DONTWAIT);
	if (n > 0) {
		st->read_len += (size_t)n;
		return 0;
	} else if (n == 0) {
		/* EOF */
		st->closed = 1;
		return 0;
	} else {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return 0;
		st->closed = 1;
		return -1;
	}
}

static int
conn_try_write(conn_state_t *st)
{
	ssize_t n;
	size_t pending;

	if (st->write_pos >= st->write_len) {
		st->write_pos = 0;
		st->write_len = 0;
		return 0;
	}

	pending = st->write_len - st->write_pos;
	n = send(st->fd, st->write_buf + st->write_pos, pending, MSG_DONTWAIT);
	if (n > 0) {
		st->write_pos += (size_t)n;
		return 0;
	} else if (n == 0) {
		return 0;
	} else {
		if (errno == EAGAIN || errno == EWOULDBLOCK)
			return 0;
		st->closed = 1;
		return -1;
	}
}

static int
conn_process_commands(conn_state_t *st)
{
	resp_parser_t parser;
	resp_value_t argv[MAX_ARGS];
	int argc;
	size_t consumed;
	resp_err_t rc;
	resp_buf_t out;
	cmd_ctx_t ctx;

	/* Reset parser to current buffer */
	resp_parser_init(&parser, st->read_buf, st->read_len);

	while (!st->quit && !st->closed) {
		/* Check rate limit */
		if (st->iops_cap > 0 && st->iops_tokens) {
			int64_t tokens = __os_atomic_load_i64(st->iops_tokens);
			if (tokens <= 0) {
				/* Rate limited - wait for tokens */
				break;
			}
			__os_atomic_fetch_add_i64(st->iops_tokens, -1);
		}

		rc = resp_parse_command(&parser, argv, MAX_ARGS, &argc, &consumed);
		if (rc == RESP_NEED_MORE)
			break;

		XTC_INJECTION_POINT("rexis:parse_fail");

		if (rc != RESP_OK) {
			/* Protocol error - send error and close */
			resp_buf_init(&out, st->write_buf + st->write_len,
			              st->write_cap - st->write_len);
			resp_write_error(&out, "protocol error");
			st->write_len += out.len;
			st->quit = 1;
			break;
		}

		/* Execute command */
		resp_buf_init(&out, st->write_buf + st->write_len,
		              st->write_cap - st->write_len);
		ctx.db = st->db;
		ctx.out = &out;
		ctx.argc = argc;
		ctx.argv = argv;
		ctx.quit_flag = &st->quit;
		ctx.iops_tokens = st->iops_tokens;
		ctx.iops_cap = st->iops_cap;

		XTC_INJECTION_POINT("rexis:before_cmd");
		(void)cmd_execute(&ctx);
		XTC_INJECTION_POINT("rexis:after_cmd");

		st->write_len += out.len;

		/* Compact read buffer */
		if (consumed > 0) {
			if (st->read_len > consumed) {
				memmove(st->read_buf, st->read_buf + consumed,
				        st->read_len - consumed);
			}
			st->read_len -= consumed;
			parser.buf = st->read_buf;
			parser.len = st->read_len;
			parser.pos = 0;
		}

		/* Check write buffer capacity */
		if (st->write_len >= st->write_cap * 3 / 4) {
			/* Flush needed */
			break;
		}
	}

	return 0;
}

/* Connection proc entry point */
static void
conn_proc(void *arg)
{
	conn_state_t *st = arg;
	void *msg;
	size_t msg_len;

	while (!st->quit && !st->closed) {
		uint32_t interest = XTC_IO_READABLE;

		/* Try to write pending data */
		if (st->write_len > st->write_pos) {
			conn_try_write(st);
			if (st->write_len > st->write_pos)
				interest |= XTC_IO_WRITABLE;
		}

		if (st->closed)
			break;

		/* Try to read */
		conn_try_read(st);
		if (st->closed)
			break;

		/* Process commands */
		conn_process_commands(st);

		if (st->quit || st->closed)
			break;

		/* Wait for I/O with timeout.  Per-connection busy-poll;
		 * see README.md "Gaps in xtc" item 8: needs async-fd
		 * readiness in xtc_proc to drop this. */
		(void)xtc_recv(&msg, &msg_len, 50LL * 1000 * 1000);  /* 50 ms */
		if (msg)
			__os_free(msg);
	}

	/* Final flush */
	while (st->write_len > st->write_pos && !st->closed) {
		conn_try_write(st);
		if (st->write_len > st->write_pos) {
			/* Brief wait for writability */
			void *m;
			size_t s;
			(void)xtc_recv(&m, &s, 5 * 1000 * 1000);
			if (m) __os_free(m);
		}
	}

	/* Cleanup */
	close(st->fd);
	__os_free(st->read_buf);
	__os_free(st->write_buf);
	__os_free(st);
}

int
conn_spawn(xtc_loop_t *loop, const conn_opts_t *opts, xtc_pid_t *out_pid)
{
	conn_state_t *st;
	xtc_proc_opts_t proc_opts = { 0 };

	if (__os_calloc(1, sizeof(*st), (void **)&st) != XTC_OK || !st)
		return XTC_E_NOMEM;

	st->fd = opts->fd;
	st->db = opts->db;
	st->res = opts->res;

	st->read_cap = DEFAULT_READ_BUF;
	if (__os_malloc(st->read_cap, (void **)&st->read_buf) != XTC_OK ||
	    !st->read_buf) {
		__os_free(st);
		return XTC_E_NOMEM;
	}
	st->read_len = 0;
	st->max_read_buf = opts->max_read_buf ? opts->max_read_buf :
	                   (1024 * 1024);

	st->write_cap = DEFAULT_WRITE_BUF;
	if (__os_malloc(st->write_cap, (void **)&st->write_buf) != XTC_OK ||
	    !st->write_buf) {
		__os_free(st->read_buf);
		__os_free(st);
		return XTC_E_NOMEM;
	}
	st->write_len = 0;
	st->write_pos = 0;
	st->max_write_buf = opts->max_write_buf ? opts->max_write_buf :
	                    (1024 * 1024);

	st->iops_tokens = opts->iops_tokens;
	st->iops_cap = opts->iops_cap;
	st->quit = 0;
	st->closed = 0;

	proc_opts.name = "rexis-conn";

	return xtc_proc_spawn(loop, conn_proc, st, &proc_opts, out_pid);
}
