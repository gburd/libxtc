/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/09_pgmock/listener.h
 *	The postmaster: an xtc_proc that accepts on a listen fd and
 *	spawns one backend proc per connection.
 */

#ifndef PGMOCK_LISTENER_H
#define PGMOCK_LISTENER_H

#include <stdatomic.h>

#include "xtc_loop.h"
#include "xtc_proc.h"

/* Listener proc argument.  The proc runs until *shutdown becomes
 * non-zero, accepting connections on `listen_fd` and spawning a
 * backend on `loop` for each. */
typedef struct pgmock_listener {
	int              listen_fd;   /* non-blocking listen socket */
	xtc_loop_t      *loop;        /* loop to place backends on */
	_Atomic int     *shutdown;    /* set non-zero to stop the loop */
	_Atomic int      accepted;    /* connections accepted (observability) */
	/* Optional: the last few backend pids spawned, so a test can assert
	 * distinct procs serve concurrent connections.  NULL = not tracked. */
	xtc_pid_t       *pids;        /* array of pids_cap slots */
	int              pids_cap;
	_Atomic int      pids_n;      /* number of slots filled */
} pgmock_listener_t;

/* xtc_proc_fn entry: arg is a pgmock_listener_t *. */
void pgmock_listener_proc(void *arg);

#endif /* PGMOCK_LISTENER_H */
