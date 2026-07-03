/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/09_pgmock/backend.h
 *	Mock PostgreSQL backend: one xtc_proc per connection.
 */

#ifndef PGMOCK_BACKEND_H
#define PGMOCK_BACKEND_H

#include "xtc_loop.h"
#include "xtc_proc.h"

/* Options handed to a backend proc (owns `fd` after spawn). */
typedef struct pgmock_backend_opts {
	int fd;   /* accepted client socket, non-blocking */
} pgmock_backend_opts_t;

/* Spawn one backend proc on `loop` to serve the connection in
 * `opts->fd`.  On XTC_OK the proc owns and closes the fd; on error
 * the caller retains ownership.  *out_pid is the backend pid. */
int pgmock_backend_spawn(xtc_loop_t *loop,
                         const pgmock_backend_opts_t *opts,
                         xtc_pid_t *out_pid);

#endif /* PGMOCK_BACKEND_H */
