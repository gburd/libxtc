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
#include <errno.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <unistd.h>

#include "xtc.h"
#include "xtc_io.h"
#include "xtc_net.h"
#include "xtc_proc.h"
#include "os_alloc.h"

#include "backend.h"
#include "listener.h"

void
pgmock_listener_proc(void *arg)
{
	pgmock_listener_t *lst = arg;
	void *msg;
	size_t msg_len;

	while (!atomic_load(lst->shutdown)) {
		/* Drain the accept backlog. */
		for (;;) {
			int fd = accept(lst->listen_fd, NULL, NULL);
			pgmock_backend_opts_t bo;
			xtc_pid_t pid;

			if (fd < 0) {
				if (errno == EAGAIN || errno == EWOULDBLOCK)
					break;
				if (errno == EINTR)
					continue;
				break;
			}

			xtc_net_setnonblock(fd);
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
				close(fd);
			}
		}

		/* Park until the listen fd is readable, a wake byte arrives,
		 * or the 100ms cap fires (to re-check shutdown). */
		{
			uint32_t revents = 0;
			(void)xtc_proc_wait_fd(lst->listen_fd,
			    XTC_IO_READABLE,
			    100LL * 1000 * 1000,
			    &revents);
			if (revents & XTC_WAIT_MAILBOX) {
				while (xtc_recv(&msg, &msg_len, 0) == XTC_OK) {
					if (msg != NULL)
						__os_free(msg);
				}
			}
		}
	}
}
