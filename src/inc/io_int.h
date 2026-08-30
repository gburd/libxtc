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
#include <stdatomic.h>

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
	int64_t   armed_at_ns;      /* __os_clock_mono() when this arm was issued;
	                             * drives the bounded re-poll workaround for
	                             * the AFD async-completion bug (see io_iocp.c
	                             * __xtc_iocp_repoll_sweep) */
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
#include <pthread.h>
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
#elif defined(XTC_IO_BACKEND_SIM)
/* Deterministic-simulation backend (DST): no kernel poller.  Readiness
 * and file-AIO completions come from a scripted in-process event store
 * driven against the virtual clock; the wakeup is an in-process flag.
 * See src/io/io_sim.c. */
#else
# error "M2 build expects XTC_IO_BACKEND_{POLL,EPOLL,URING,KQUEUE,IOCP,SOLARIS,AIX,SELECT,SIM} to be defined"
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
	_Atomic int            wakeup_pending; /* 1 = a wakeup completion is queued */
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
	/*
	 * Cross-loop deferred unregister queue.  io->fds / io->zombies and
	 * the SQ ring are single-producer, owned by this io's loop thread.
	 * A migratable fiber that parked on this loop but was woken via a
	 * non-fd path (timeout / xtc_proc_wake / mailbox) and then work-
	 * stolen resumes on ANOTHER thread with its park_fd still live, and
	 * its xtc_proc_wait_fd cleanup would otherwise call xtc_io_del_fd on
	 * THIS io from the wrong thread -- racing the fd list AND the SQ ring
	 * (the native-path concurrent-commit collapse, TSan-caught
	 * 2026-08-30).  Instead the foreign thread posts the fd here under
	 * del_lock and nudges this loop; the owning thread drains it at the
	 * top of xtc_io_poll and performs the real unregister on its own
	 * ring.  del_lock guards ONLY this small queue, never the hot fds
	 * list or the ring. */
	int             *pending_del;    /* fds awaiting owner-thread unregister */
	int              n_pending_del;
	int              cap_pending_del;
	_Atomic int      has_pending_del;  /* fast, lock-free "is queue non-empty?" */
	pthread_mutex_t  del_lock;        /* guards ONLY pending_del (not fds/ring) */
	pthread_t        owner_tid;       /* the thread that polls this io */
	_Atomic int      owner_set;       /* 1 once owner_tid is recorded */
	/*
	 * L2 ring-pointer preempt (INSPIRED BY Glommio's need_preempt():
	 * reactor.rs / sys/uring.rs preempt_pointers).  A dedicated tiny
	 * ring carrying ONLY a rearmed TIMEOUT SQE, so
	 * io_uring_cq_ready(&preempt_ring) -- two relaxed/acquire loads of
	 * that ring's own head/tail, no syscall, no signal -- means "the
	 * preempt interval elapsed".  A separate ring (not io->ring) is
	 * what makes the pointer test isolate the timeout: I/O CQEs on the
	 * main ring advance its head/tail unpredictably, but this ring sees
	 * only the timeout.  preempt_armed gates it (0 = off, the ring is
	 * not initialised). */
	struct io_uring  preempt_ring;
	int              preempt_armed;
	struct __kernel_timespec preempt_ts;   /* the rearm interval */
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
#elif defined(XTC_IO_BACKEND_SIM)
	/* Registered fds (tag map) for readiness simulation, a scripted
	 * event queue ordered by virtual-time due, and an in-process
	 * wakeup flag.  Defined in io_sim.c; opaque here. */
	struct __xtc_sim_io *sim;
#endif
};

/*
 * Cross-loop deferred fd-unregister.  Each backend .c provides one:
 * io_uring queues the fd and drains it on the owning thread (its fds
 * list + SQ ring are single-owner); the other backends passthrough to
 * xtc_io_del_fd (kernel-synchronized or their own registry).  Internal;
 * the sole caller is xtc_proc_wait_fd's post-migration cleanup.
 */
int __xtc_io_defer_del_fd(xtc_io_t *io, int fd);

#endif /* XTC_IO_INT_H */
