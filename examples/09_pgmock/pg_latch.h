/*-
 * Copyright (c) 2026, The XTC Project -- All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * SPDX-License-Identifier: ISC
 *
 * examples/09_pgmock/pg_latch.h
 *	Latch-shaped glue over the xtc proc mailbox + xtc_proc_wait_fd.
 *
 *	This is the artifact M16.1b (the real PostgreSQL bringup)
 *	reuses verbatim: PostgreSQL's Latch is a set-a-flag-and-wake
 *	object read by WaitLatch / WaitLatchOrSocket.  xtc has no
 *	xtc_notify; the equivalent is the proc mailbox (SetLatch ->
 *	xtc_send one byte) plus xtc_proc_wait_fd, which waits on
 *	fd-readiness OR mailbox OR timeout in one call -- exactly the
 *	shape of WaitLatchOrSocket.
 *
 *	Keep this PG-shaped and free of mock-specific code.
 */

#ifndef PG_LATCH_H
#define PG_LATCH_H

#include <stdint.h>

#include "xtc_proc.h"

/* A Latch is owned by the proc that waits on it; SetLatch is called
 * from any proc (or the same proc) and wakes the owner. */
typedef struct pg_latch {
	xtc_pid_t owner;   /* the proc that waits on this latch */
} pg_latch_t;

/* Initialise a latch owned by `owner` (typically xtc_self()). */
static inline void
pg_latch_init(pg_latch_t *latch, xtc_pid_t owner)
{
	latch->owner = owner;
}

/* SetLatch(latch): wake the owner proc.  Maps to xtc_send of a single
 * wake byte into the owner's mailbox.  Returns the xtc_send status
 * (XTC_OK, or XTC_E_AGAIN if the mailbox is full -- the wake byte is
 * coalescible, so a full mailbox already implies a pending wake). */
static inline int
pg_set_latch(const pg_latch_t *latch)
{
	uint8_t wake = 1;
	return xtc_send(latch->owner, &wake, sizeof wake);
}

/* WaitLatchOrSocket(fd, events, timeout): block the calling proc until
 * the socket is ready, the latch is set (a mailbox message arrived),
 * or the timeout elapses.  `events` is a bitwise-or of XTC_IO_*;
 * *out_revents receives the XTC_IO_* bits plus XTC_WAIT_MAILBOX /
 * XTC_WAIT_TIMEOUT.  timeout_ns < 0 waits indefinitely.
 *
 * The caller still drains its mailbox (xtc_recv) after a
 * XTC_WAIT_MAILBOX wake -- the wake byte is a signal, not payload. */
static inline int
pg_wait_latch_or_socket(int fd, uint32_t events, int64_t timeout_ns,
                        uint32_t *out_revents)
{
	return xtc_proc_wait_fd(fd, events, timeout_ns, out_revents);
}

/* Drain and discard any pending wake bytes from the calling proc's
 * mailbox (ResetLatch equivalent). */
static inline void
pg_reset_latch(void)
{
	void *msg;
	size_t len;
	while (xtc_recv(&msg, &len, 0) == XTC_OK) {
		/* xtc_free is the PUBLIC name for the library allocator's
		 * release entry point; a consumer must not reach for the
		 * internal __os_free (AGENTS.md API-discipline rule 3).
		 * xtc_free(NULL) is a no-op. */
		xtc_free(msg);
	}
}

#endif /* PG_LATCH_H */
