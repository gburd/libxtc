/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/sqlxtc/conn.h
 *	Per-connection xtc_proc.  Reads newline-delimited JSON, parses
 *	into Quack messages, dispatches to db_exec, streams JSON
 *	results back.
 */

#ifndef SQLXTC_CONN_H
#define SQLXTC_CONN_H

#include <stdint.h>

#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_res.h"

#include "db.h"

struct server;

typedef struct conn_opts {
	int             fd;
	db_t           *db;
	xtc_res_t      *res;
	struct server  *server;

	/* rate limit */
	int64_t        *iops_tokens;
	int64_t         iops_cap;

	/* memory */
	int64_t         max_memory;        /* 0 = unlimited */

	/* line size cap */
	size_t          max_line_bytes;    /* 0 = 1 MiB default */
} conn_opts_t;

int conn_spawn(xtc_loop_t *loop, const conn_opts_t *opts, xtc_pid_t *out_pid);

#endif /* SQLXTC_CONN_H */
