/*-
 * Copyright (c) 2026, The XTC Project
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
/*
 * Windows IOCP backend (round 2): native completion port plus the
 * AFD poll fast path that turns a connected/listening socket's
 * readiness into an IOCP completion.
 *
 * Each socket registration owns one AFD poll OVERLAPPED that is
 * armed (submitted via NtDeviceIoControlFile(IOCTL_AFD_POLL)) into
 * the kernel and re-armed after every completion -- level-triggered
 * emulation matching the epoll/kqueue contract.
 *
 * OVERLAPPED OWNERSHIP RULE (the classic IOCP correctness invariant,
 * enforced throughout io_iocp.c):
 *   The OVERLAPPED (ov) and the AFD_POLL_INFO (poll_info) embedded in
 *   a registration belong to the KERNEL from the moment the AFD poll
 *   is armed (NtDeviceIoControlFile returns STATUS_PENDING) until the
 *   matching completion is dequeued from the port by
 *   GetQueuedCompletionStatusEx.  While armed (pending == 1) neither
 *   buffer may be freed or reused.  Deregistering an armed socket
 *   issues NtCancelIoFileEx and marks the registration dead; the
 *   storage is only released once the (possibly canceled) completion
 *   is reaped.  This is why the struct embeds ov/poll_info by value
 *   and the registration node is freed lazily, never inline with
 *   xtc_io_del_fd while a poll is in flight.
 *
 * The OVERLAPPED is the FIRST member so a completion's
 * OVERLAPPED_ENTRY.lpOverlapped pointer can be cast straight back to
 * the owning registration.  Each registration is a separately
 * heap-allocated node with a STABLE address (held in a pointer array,
 * never an inline array), because the kernel-owned OVERLAPPED carries
 * a back-pointer to its node for the whole time the poll is armed --
 * a realloc or swap-remove of an inline array would dangle it.
 */
struct __xtc_iocp_reg {
	struct __xtc_iocp_overlapped *ovp; /* OVERLAPPED + AFD_POLL_INFO (heap) */
	int       fd;               /* the SOCKET as an int (Winsock handle) */
	void     *base;             /* base socket HANDLE (SIO_BASE_HANDLE) */
	uint32_t  interest;
	void     *tag;
	int       pending;          /* 1 while an AFD poll is armed in the kernel */
	int       dead;             /* deregistered; awaiting terminal completion */
};
/* A file AIO (pread/pwrite) in flight on the IOCP backend.  The
 * file HANDLE is associated with the completion port, so the
 * overlapped ReadFile/WriteFile completion is dequeued by
 * GetQueuedCompletionStatusEx like any socket event -- no hEvent and
 * no WaitForMultipleObjects.  fsync has no async form on Windows
 * (FlushFileBuffers is synchronous) and is offloaded.  The OVERLAPPED
 * is the FIRST member so the completion's lpOverlapped recovers the
 * node; ownership follows the same kernel-owns-while-pending rule as
 * the socket poll OVERLAPPED above. */
struct __xtc_iocp_aio {
	void *ov;      /* OVERLAPPED * (heap; first field; owned here) */
	void *aio;     /* xtc_aio_t * awaiting completion */
	void *fh;      /* HANDLE: the file, for GetOverlappedResult */
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
	int        dead;          /* deleted; awaiting terminal CQE before free */
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
	void                  *iocp;       /* HANDLE: the completion port */
	void                  *afd;        /* HANDLE: \Device\Afd, port-associated */
	struct __xtc_iocp_reg **reg_iocp;   /* live registration nodes (stable) */
	int  n_reg;
	int  cap_reg;
	struct __xtc_iocp_reg **dead_iocp;  /* deregistered, awaiting completion */
	int  n_dead;
	int  cap_dead;
	struct __xtc_iocp_aio  *aio_pend;   /* file AIOs in flight */
	int  n_aio;
	int  cap_aio;
#elif defined(XTC_IO_BACKEND_AIX)
	int   ps;                          /* pollset_t */
	void *reg_aix;                     /* struct __xtc_aix_reg * */
	int   n_reg;
	int   cap_reg;
#elif defined(XTC_IO_BACKEND_URING)
	struct io_uring  ring;
	struct __xtc_uring_fd *fds;
	struct __xtc_uring_fd *zombies;  /* deleted fds awaiting terminal CQE */
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
