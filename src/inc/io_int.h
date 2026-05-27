/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/io_int.h
 *	Internal struct definition for the L1 backend implementations.
 */

#ifndef XTC_IO_INT_H
#define XTC_IO_INT_H

#include "xtc_io.h"

#if defined(XTC_IO_BACKEND_EPOLL)
/* nothing to predefine; epoll keeps tags via epoll_data_t */
#elif defined(XTC_IO_BACKEND_KQUEUE)
/* kqueue's EV_ADD is idempotent and EV_DELETE silently ignores absent
 * filters, so we keep an authoritative side-table of currently-
 * registered fds to enforce M2's semantic contract. */
#elif defined(XTC_IO_BACKEND_SOLARIS)
/* illumos event ports are one-shot per association, so we maintain
 * the same side-table as kqueue and re-arm on each delivery. */
struct __xtc_solaris_reg {
	int       fd;
	uint32_t  interest;
	void     *tag;
};
#elif defined(XTC_IO_BACKEND_IOCP)
/* Windows IOCP backend with WSAEventSelect-based readiness emulation
 * for round 1.  Each registration tracks the socket, its WSAEvent,
 * and the user's interest+tag. */
struct __xtc_iocp_reg {
	int       fd;
	void     *event;            /* HANDLE (WSAEVENT) */
	uint32_t  interest;
	void     *tag;
};
#elif defined(XTC_IO_BACKEND_AIX)
/* AIX pollset_* backend.  Like solaris/kqueue, we maintain a
 * side-table for duplicate detection and to map fd -> user tag
 * (pollset itself doesn't carry udata). */
struct __xtc_aix_reg {
	int       fd;
	uint32_t  interest;
	void     *tag;
};
#elif defined(XTC_IO_BACKEND_URING)
#include <liburing.h>
/*
 * Per-fd state for the io_uring backend.  The user_data passed to
 * each POLL_ADD points at one of these.  The fd_table maps fd ->
 * uring_fd so we can find/cancel a registration on _del/_mod.
 */
struct __xtc_uring_fd {
	int        fd;
	uint32_t   interest;
	void      *tag;
	int        is_wakeup;     /* 1 for the internal wakeup pipe */
	struct __xtc_uring_fd *next;  /* free-list / fd-list linkage */
};
#elif defined(XTC_IO_BACKEND_POLL)
#include <poll.h>
#elif defined(XTC_IO_BACKEND_SELECT)
#include <sys/select.h>
#else
# error "M2 build expects XTC_IO_BACKEND_{POLL,EPOLL,URING,KQUEUE,IOCP,SOLARIS,AIX,SELECT} to be defined"
#endif

struct xtc_io {
	int wakeup_rfd;
	int wakeup_wfd;

#if defined(XTC_IO_BACKEND_EPOLL)
	int epfd;
#elif defined(XTC_IO_BACKEND_KQUEUE)
	int epfd;                          /* kqueue fd */
	int *reg_fds;                      /* registered fd list */
	int  n_reg;
	int  cap_reg;
#elif defined(XTC_IO_BACKEND_SOLARIS)
	int epfd;                          /* event-port fd */
	struct __xtc_solaris_reg *reg_fds;
	int  n_reg;
	int  cap_reg;
#elif defined(XTC_IO_BACKEND_IOCP)
	void                  *iocp;       /* HANDLE */
	void                  *wakeup_ev;  /* HANDLE: manual-reset event */
	struct __xtc_iocp_reg *reg_iocp;
	int  n_reg;
	int  cap_reg;
#elif defined(XTC_IO_BACKEND_AIX)
	int   ps;                          /* pollset_t */
	void *reg_aix;                     /* struct __xtc_aix_reg * */
	int   n_reg;
	int   cap_reg;
#elif defined(XTC_IO_BACKEND_URING)
	struct io_uring  ring;
	struct __xtc_uring_fd *fds;
#elif defined(XTC_IO_BACKEND_POLL)
	struct pollfd *pfds;
	void         **tags;
	int            n;
	int            cap;
#elif defined(XTC_IO_BACKEND_SELECT)
	/* Parallel fd[], interest[], tag[] arrays.  fd_set is built
	 * each poll() call from these.  Capped at FD_SETSIZE. */
	int           *fds;
	uint32_t      *interests;
	void         **tags;
	int            n;
	int            cap;
#endif
};

#endif /* XTC_IO_INT_H */
