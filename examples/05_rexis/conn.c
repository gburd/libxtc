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
#include "xtc_pg.h"
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

	/* Pub/sub: this connection's own pid + the shared channel registry
	 * (an xtc_reg_t used as a duplicate-key process-group set).  A
	 * SUBSCRIBE joins st->self to a channel group; a PUBLISH from any
	 * connection fans a message out to every subscriber's mailbox, which
	 * the conn proc drains and writes to its socket. */
	xtc_pid_t   self;
	xtc_reg_t  *pubsub;
	int         sub_count;   /* channels this connection is subscribed to */

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
		new_buf = xtc_realloc(st->read_buf, new_cap);
		if (new_buf == NULL)
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

/* Append `len` bytes to the write buffer, growing it up to max_write_buf.
 * Used to enqueue a pub/sub push message for delivery to this
 * subscriber's socket.  On overflow the bytes are dropped (a slow
 * subscriber must not let the server buffer without bound). */
static void
conn_append_write(conn_state_t *st, const void *data, size_t len)
{
	if (st->write_len + len > st->write_cap) {
		size_t new_cap = st->write_cap ? st->write_cap : DEFAULT_WRITE_BUF;
		char *nb;
		while (new_cap < st->write_len + len && new_cap <= st->max_write_buf)
			new_cap *= 2;
		if (new_cap > st->max_write_buf)
			return;   /* would exceed the cap: drop (backpressure) */
		if ((nb = xtc_realloc(st->write_buf, new_cap)) == NULL)
			return;
		st->write_buf = nb;
		st->write_cap = new_cap;
	}
	memcpy(st->write_buf + st->write_len, data, len);
	st->write_len += len;
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
			int64_t tokens = xtc_atomic_i64_load(st->iops_tokens);
			if (tokens <= 0) {
				/* Rate limited - wait for tokens */
				break;
			}
			xtc_atomic_i64_add(st->iops_tokens, -1);
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
		ctx.self = st->self;
		ctx.pubsub = st->pubsub;
		ctx.sub_count = &st->sub_count;

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

/*
 * Connection teardown, run as an xtc_scope finalizer -- see conn_proc.
 * Leaves every pub/sub channel this connection subscribed to (so a
 * closed subscriber does not linger in the groups), closes the socket,
 * and frees the buffers and the state.  The scope runs it exactly once,
 * on whatever exit path the fiber takes (normal, error, or async kill).
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
		close(st->fd);
	xtc_free(st->read_buf);
	xtc_free(st->write_buf);
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

	st->self = xtc_self();   /* for pub/sub group membership */

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

		/* Wait for the next inbound chunk (or for shutdown).  This
		 * wakes exactly on fd readiness or mailbox traffic, not on
		 * a polling timer. */
		{
			uint32_t revents = 0;
			(void)xtc_proc_wait_fd(st->fd,
			    XTC_IO_READABLE | XTC_IO_HUP | XTC_IO_ERR,
			    1000LL * 1000 * 1000,  /* 1s timeout to re-check quit flag */
			    &revents);
			if (revents & XTC_WAIT_MAILBOX) {
				while (xtc_recv(&msg, &msg_len, 0) == XTC_OK) {
					/* A published message: append its RESP
					 * bytes to the write buffer so the next
					 * loop iteration flushes it to the
					 * subscriber's socket. */
					if (msg != NULL && msg_len > 0)
						conn_append_write(st, msg, msg_len);
					if (msg) xtc_free(msg);
				}
			}
		}
	}

	/* Final flush -- drain the write buffer to the wire before
	 * closing.  Wait for actual writability rather than poll.  This
	 * is the technique requested by the user: an asynchronous I/O
	 * completion wait, not a sleep + retry. */
	while (st->write_len > st->write_pos && !st->closed) {
		conn_try_write(st);
		if (st->write_len > st->write_pos) {
			uint32_t revents = 0;
			int rc = xtc_proc_wait_fd(st->fd,
			    XTC_IO_WRITABLE | XTC_IO_HUP | XTC_IO_ERR,
			    100LL * 1000 * 1000,  /* 100ms cap on shutdown */
			    &revents);
			if (rc != XTC_OK || (revents & (XTC_IO_HUP | XTC_IO_ERR)))
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

	if ((st = xtc_calloc(1, sizeof(*st))) == NULL)
		return XTC_E_NOMEM;

	st->fd = opts->fd;
	st->db = opts->db;
	st->res = opts->res;
	st->pubsub = opts->pubsub;

	st->read_cap = DEFAULT_READ_BUF;
	if ((st->read_buf = xtc_malloc(st->read_cap)) == NULL) {
		xtc_free(st);
		return XTC_E_NOMEM;
	}
	st->read_len = 0;
	st->max_read_buf = opts->max_read_buf ? opts->max_read_buf :
	                   (1024 * 1024);

	st->write_cap = DEFAULT_WRITE_BUF;
	if ((st->write_buf = xtc_malloc(st->write_cap)) == NULL) {
		xtc_free(st->read_buf);
		xtc_free(st);
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
