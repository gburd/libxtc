/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * examples/05_rexis/conn.h
 *	Per-connection xtc_proc; reads RESP commands, dispatches to
 *	handlers, writes responses.
 */

#ifndef REXIS_CONN_H
#define REXIS_CONN_H

#include <stdatomic.h>
#include <stdint.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_res.h"
#include "xtc_slab.h"
#include "xtc_reg.h"

#include "db.h"

/* Forward declaration */
struct server;

/* Connection configuration */
typedef struct conn_opts {
	int             fd;
	db_t           *db;
	xtc_res_t      *res;
	xtc_slab_t     *read_slab;     /* for read buffers */
	xtc_slab_t     *write_slab;    /* for write buffers */
	struct server  *server;        /* back-pointer for stats */
	xtc_reg_t      *pubsub;        /* shared pub/sub channel registry */

	/* Rate limiting */
	int64_t        *iops_tokens;
	int64_t         iops_cap;

	/* Limits (0 = the CONN_DEFAULT_* below) */
	size_t          max_request;   /* largest command accepted, bytes */
	size_t          max_output;    /* per-client pending reply bytes */
	int64_t         idle_timeout_ns; /* close after this long without
	                                  * I/O; 0 = never (Redis `timeout`) */
	/* Budget client buffer growth (every byte above the two
	 * CONN_BASE_BUF base buffers) is charged to (XTC_RES_MEM_BYTES);
	 * NULL = uncharged.  Shared by all connections. */
	xtc_res_t      *client_res;

	/* Server-wide state the connection proc updates / watches. */
	_Atomic int    *conn_count;    /* live connections; decremented on exit */
	_Atomic int    *shutdown;      /* set by the signal handler */
} conn_opts_t;

#define CONN_DEFAULT_MAX_REQUEST  (64UL * 1024 * 1024)
#define CONN_DEFAULT_MAX_OUTPUT   (128UL * 1024 * 1024)
#define CONN_BASE_BUF             (16UL * 1024)  /* each direction */

/* Spawn a connection proc.  The proc takes ownership of the fd and
 * will close it on exit, and decrements *conn_count when it exits.
 * On failure nothing was spawned: the caller still owns the fd. */
int conn_spawn(xtc_loop_t *loop, const conn_opts_t *opts, xtc_pid_t *out_pid);

/* Client buffer bytes currently charged to client_res (all
 * connections' query + output buffers above their base). */
int64_t conn_client_bytes(void);

#endif /* REXIS_CONN_H */
