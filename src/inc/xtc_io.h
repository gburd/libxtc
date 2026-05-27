/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_io.h
 *	The L1 event-notification engine: register interest in fd
 *	readiness, poll for ready events, wake the poller from another
 *	thread.
 *
 *	Exactly one backend is compiled in per binary (configure-time).
 *	M2 ships poll(2) and epoll; later milestones add io_uring,
 *	kqueue, IOCP, Solaris event ports, and AIX pollset.
 *
 *	See M2_CLAIMS.md.
 */

#ifndef XTC_IO_H
#define XTC_IO_H

#include <stdint.h>

typedef struct xtc_io xtc_io_t;

/*
 * Event flag bits.  Stable across minor versions; new flags appear
 * at higher bit positions only (PLAN.md (S)18).
 */
#define XTC_IO_READABLE  0x01u
#define XTC_IO_WRITABLE  0x02u
#define XTC_IO_HUP       0x04u
#define XTC_IO_ERR       0x08u
#define XTC_IO_WAKEUP    0x10u   /* set on the synthetic event delivered
                                    by xtc_io_wakeup; tag is NULL. */

typedef struct xtc_io_event {
	void     *tag;        /* the value passed at registration; NULL on wakeups */
	uint32_t  flags;      /* XTC_IO_* bitset */
} xtc_io_event_t;

/*
 * PUBLIC: int          xtc_io_init __P((xtc_io_t **));
 * PUBLIC: int          xtc_io_fini __P((xtc_io_t *));
 * PUBLIC: const char  *xtc_io_backend_name __P((void));
 * PUBLIC: int          xtc_io_reg_fd __P((xtc_io_t *, int, uint32_t, void *));
 * PUBLIC: int          xtc_io_mod_fd __P((xtc_io_t *, int, uint32_t, void *));
 * PUBLIC: int          xtc_io_del_fd __P((xtc_io_t *, int));
 * PUBLIC: int          xtc_io_poll __P((xtc_io_t *, xtc_io_event_t *, int, int64_t, int *));
 * PUBLIC: int          xtc_io_wakeup __P((xtc_io_t *));
 */

/* Lifecycle. */
int          xtc_io_init(xtc_io_t **out);
int          xtc_io_fini(xtc_io_t *io);
const char  *xtc_io_backend_name(void);

/* Registration. */
int          xtc_io_reg_fd(xtc_io_t *io, int fd, uint32_t interest, void *tag);
int          xtc_io_mod_fd(xtc_io_t *io, int fd, uint32_t interest, void *tag);
int          xtc_io_del_fd(xtc_io_t *io, int fd);

/*
 * xtc_io_poll --
 *	Wait for events.  On return *n_out holds the number of events
 *	written (0..max).
 *
 *	timeout_ns:
 *		== 0  -> non-blocking
 *		>  0  -> wait at most that many nanoseconds
 *		<  0  -> wait indefinitely (until an fd becomes ready or
 *		         xtc_io_wakeup is called)
 */
int          xtc_io_poll(xtc_io_t *io, xtc_io_event_t *events, int max,
                         int64_t timeout_ns, int *n_out);

/*
 * xtc_io_wakeup --
 *	From any thread, cause the next (or in-flight) xtc_io_poll on
 *	this io to return.  Safe to call concurrently from many threads;
 *	multiple wakeups before the next poll coalesce into one event.
 */
int          xtc_io_wakeup(xtc_io_t *io);

#endif /* XTC_IO_H */
