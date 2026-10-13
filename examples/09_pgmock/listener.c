/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/09_pgmock/listener.c
 *	The postmaster proc.  Accepts connections on a listen fd and
 *	spawns one backend xtc_proc per connection -- the no-fork
 *	multiplexing the M16.1a runtime seam proves.  Structurally
 *	cloned from examples/06_sqlxtc's listener_proc.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <stdatomic.h>

#include "xtc.h"
#include "xtc_io.h"
#include "xtc_net.h"
#include "xtc_proc.h"

#include "backend.h"
#include "listener.h"

void
pgmock_listener_proc(void *arg)
{
	pgmock_listener_t *lst = arg;
	void *msg;
	size_t msg_len;
	uint32_t revents;

	while (!atomic_load(lst->shutdown)) {
		/* Drain the accept backlog.  xtc_net_accept maps EMFILE /
		 * ENFILE to XTC_E_RESOURCE: back off 10 ms (the connection
		 * stays in the backlog) instead of spinning on a listen fd
		 * that stays readable. */
		for (;;) {
			int fd = -1;
			int rc = xtc_net_accept(lst->listen_fd, &fd);
			pgmock_backend_opts_t bo;
			xtc_pid_t pid;

			if (rc == XTC_E_AGAIN)
				break;
			if (rc != XTC_OK) {
				(void)xtc_proc_sleep(10LL * 1000 * 1000);
				break;
			}
			bo.fd = fd;
			if (pgmock_backend_spawn(lst->loop, &bo, &pid) ==
			    XTC_OK) {
				atomic_fetch_add(&lst->accepted, 1);
				if (lst->pids != NULL) {
					int i = atomic_fetch_add(
					    &lst->pids_n, 1);
					if (i < lst->pids_cap)
						lst->pids[i] = pid;
				}
			} else {
				xtc_net_close(fd);
			}
		}

		/* Park until the listen fd is readable, a wake byte arrives,
		 * or the 100ms cap fires (to re-check shutdown). */
		revents = 0;
		(void)xtc_proc_wait_fd(lst->listen_fd, XTC_IO_READABLE,
		    100LL * 1000 * 1000, &revents);
		if (revents & XTC_WAIT_MAILBOX) {
			while (xtc_recv(&msg, &msg_len, 0) == XTC_OK) {
				if (msg != NULL)
					xtc_free(msg);
			}
		}
	}
}
