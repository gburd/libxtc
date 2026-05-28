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

#include <stdint.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_res.h"
#include "xtc_slab.h"

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

	/* Rate limiting */
	int64_t        *iops_tokens;
	int64_t         iops_cap;

	/* Limits */
	size_t          max_read_buf;  /* default 1 MB */
	size_t          max_write_buf; /* default 1 MB */
} conn_opts_t;

/* Spawn a connection proc.  The proc takes ownership of the fd and
 * will close it on exit.  Returns XTC_OK on success. */
int conn_spawn(xtc_loop_t *loop, const conn_opts_t *opts, xtc_pid_t *out_pid);

#endif /* REXIS_CONN_H */
