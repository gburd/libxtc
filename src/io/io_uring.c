/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the PostgreSQL License.
 *
 * src/io/io_uring.c
 *	The Linux io_uring backend.  Maps the readiness-style xtc_io
 *	contract onto io_uring's completion model using IORING_OP_POLL_ADD
 *	with POLL_MULTISHOT so a single SQE delivers many CQEs as the fd
 *	repeatedly becomes ready.
 *
 *	M6 ships this as the preferred Linux backend when liburing is
 *	available at configure time.  The internal data structures
 *	parallel the epoll backend; tags travel via the SQE's user_data.
 */

#define _GNU_SOURCE

#include "xtc_int.h"

#if defined(XTC_IO_BACKEND_URING)

#include "io_int.h"

#include <errno.h>
#include <unistd.h>
#include <liburing.h>
#include <poll.h>

#define WAKEUP_INTEREST  POLLIN

extern int __xtc_io_drain_wakeup(xtc_io_t *io);

static struct __xtc_uring_fd *
__find_fd(xtc_io_t *io, int fd)
{
	struct __xtc_uring_fd *p;
	for (p = io->fds; p != NULL; p = p->next)
		if (p->fd == fd) return p;
	return NULL;
}

static short
__interest_to_pollmask(uint32_t interest)
{
	short m = 0;
	if (interest & XTC_IO_READABLE) m |= POLLIN;
	if (interest & XTC_IO_WRITABLE) m |= POLLOUT;
	return m;
}

static uint32_t
__pollres_to_flags(uint32_t res)
{
	uint32_t f = 0;
	if (res & POLLIN)  f |= XTC_IO_READABLE;
	if (res & POLLOUT) f |= XTC_IO_WRITABLE;
	if (res & POLLHUP) f |= XTC_IO_HUP;
	if (res & POLLERR) f |= XTC_IO_ERR;
	if (res & POLLNVAL) f |= XTC_IO_ERR;
	return f;
}

/*
 * Submit a POLL_ADD SQE for the given uring_fd.  POLL_MULTISHOT means
 * the kernel will keep firing CQEs as the fd becomes ready, until we
 * cancel.  user_data is the uring_fd pointer.
 */
static int
__submit_poll_add(xtc_io_t *io, struct __xtc_uring_fd *uf)
{
	struct io_uring_sqe *sqe = io_uring_get_sqe(&io->ring);
	if (sqe == NULL) {
		(void)io_uring_submit(&io->ring);
		sqe = io_uring_get_sqe(&io->ring);
		if (sqe == NULL) return XTC_E_AGAIN;
	}
	/*
	 * MULTISHOT for user fds (continuous notification matching
	 * epoll-EPOLLET / kqueue-EV_CLEAR semantics).  Single-shot for
	 * the internal wakeup pipe so we can coalesce trivially: each
	 * drain re-arms exactly once, regardless of how many bytes the
	 * pipe carried.
	 */
	if (uf->is_wakeup)
		io_uring_prep_poll_add(sqe, uf->fd,
		    __interest_to_pollmask(uf->interest));
	else
		io_uring_prep_poll_multishot(sqe, uf->fd,
		    __interest_to_pollmask(uf->interest));
	io_uring_sqe_set_data(sqe, uf);
	return XTC_OK;
}

static int
__submit_poll_remove(xtc_io_t *io, struct __xtc_uring_fd *uf)
{
	struct io_uring_sqe *sqe = io_uring_get_sqe(&io->ring);
	if (sqe == NULL) {
		(void)io_uring_submit(&io->ring);
		sqe = io_uring_get_sqe(&io->ring);
		if (sqe == NULL) return XTC_E_AGAIN;
	}
	io_uring_prep_poll_remove(sqe, (__u64)(uintptr_t)uf);
	io_uring_sqe_set_data(sqe, NULL);   /* discard the cancel CQE */
	return XTC_OK;
}

int
__xtc_io_backend_init(xtc_io_t *io)
{
	int rc = io_uring_queue_init(256, &io->ring, 0);
	if (rc < 0) {
		errno = -rc;
		return XTC_E_INTERNAL;
	}
	io->fds = NULL;
	return XTC_OK;
}

void
__xtc_io_backend_fini(xtc_io_t *io)
{
	struct __xtc_uring_fd *p, *n;
	for (p = io->fds; p != NULL; p = n) {
		n = p->next;
		__os_free(p);
	}
	io->fds = NULL;
	io_uring_queue_exit(&io->ring);
}

int
__xtc_io_register_wakeup(xtc_io_t *io, int fd)
{
	struct __xtc_uring_fd *uf;
	int rc;
	if ((rc = __os_calloc(1, sizeof *uf, (void **)&uf)) != XTC_OK)
		return rc;
	uf->fd = fd;
	uf->interest = XTC_IO_READABLE;
	uf->tag = NULL;
	uf->is_wakeup = 1;
	uf->next = io->fds;
	io->fds = uf;
	if ((rc = __submit_poll_add(io, uf)) != XTC_OK) {
		io->fds = uf->next;
		__os_free(uf);
		return rc;
	}
	(void)io_uring_submit(&io->ring);
	return XTC_OK;
}

/* PUBLIC: int xtc_io_reg_fd __P((xtc_io_t *, int, uint32_t, void *)); */
int
xtc_io_reg_fd(xtc_io_t *io, int fd, uint32_t interest, void *tag)
{
	struct __xtc_uring_fd *uf;
	int rc;
	if (io == NULL || fd < 0 || interest == 0)
		return XTC_E_INVAL;
	if (__find_fd(io, fd) != NULL)
		return XTC_E_INVAL;        /* duplicate */
	if ((rc = __os_calloc(1, sizeof *uf, (void **)&uf)) != XTC_OK)
		return rc;
	uf->fd = fd;
	uf->interest = interest;
	uf->tag = tag;
	uf->is_wakeup = 0;
	uf->next = io->fds;
	io->fds = uf;
	if ((rc = __submit_poll_add(io, uf)) != XTC_OK) {
		io->fds = uf->next;
		__os_free(uf);
		return rc;
	}
	(void)io_uring_submit(&io->ring);
	return XTC_OK;
}

/* PUBLIC: int xtc_io_mod_fd __P((xtc_io_t *, int, uint32_t, void *)); */
int
xtc_io_mod_fd(xtc_io_t *io, int fd, uint32_t interest, void *tag)
{
	struct __xtc_uring_fd *uf;
	int rc;
	if (io == NULL || fd < 0 || interest == 0)
		return XTC_E_INVAL;
	uf = __find_fd(io, fd);
	if (uf == NULL || uf->is_wakeup)
		return XTC_E_INVAL;
	/* Cancel the existing multishot, then re-submit with new mask. */
	(void)__submit_poll_remove(io, uf);
	uf->interest = interest;
	uf->tag = tag;
	if ((rc = __submit_poll_add(io, uf)) != XTC_OK) return rc;
	(void)io_uring_submit(&io->ring);
	return XTC_OK;
}

/* PUBLIC: int xtc_io_del_fd __P((xtc_io_t *, int)); */
int
xtc_io_del_fd(xtc_io_t *io, int fd)
{
	struct __xtc_uring_fd *uf, **pp;
	if (io == NULL || fd < 0) return XTC_E_INVAL;
	uf = __find_fd(io, fd);
	if (uf == NULL || uf->is_wakeup) return XTC_E_INVAL;
	(void)__submit_poll_remove(io, uf);
	(void)io_uring_submit(&io->ring);
	for (pp = &io->fds; *pp != NULL; pp = &(*pp)->next) {
		if (*pp == uf) { *pp = uf->next; break; }
	}
	__os_free(uf);
	return XTC_OK;
}

/* PUBLIC: int xtc_io_poll __P((xtc_io_t *, xtc_io_event_t *, int, int64_t, int *)); */
int
xtc_io_poll(xtc_io_t *io, xtc_io_event_t *events, int max,
            int64_t timeout_ns, int *n_out)
{
	struct __kernel_timespec ts, *tsp;
	int got = 0, i;
	struct io_uring_cqe *cqe;
	int rc;

	if (io == NULL || events == NULL || max <= 0 || n_out == NULL)
		return XTC_E_INVAL;
	*n_out = 0;

	if (timeout_ns < 0) {
		tsp = NULL;
	} else {
		ts.tv_sec  = timeout_ns / 1000000000LL;
		ts.tv_nsec = timeout_ns % 1000000000LL;
		tsp = &ts;
	}

	/* Wait for at least one CQE (or timeout). */
	rc = io_uring_wait_cqe_timeout(&io->ring, &cqe, tsp);
	if (rc == -ETIME || rc == -EINTR) return XTC_OK;
	if (rc < 0) {
		errno = -rc;
		return XTC_E_INTERNAL;
	}

	/* Drain the available CQEs up to max.  We coalesce all wakeup CQEs
	 * (from POLL_MULTISHOT on the wakeup pipe) into a single
	 * XTC_IO_WAKEUP event per poll, matching the M2 W3 coalesce
	 * contract. */
	int wakeup_emitted = 0;
	for (i = 0; i < max; i++) {
		struct __xtc_uring_fd *uf;
		if (cqe == NULL) {
			if (io_uring_peek_cqe(&io->ring, &cqe) != 0) break;
		}
		uf = (struct __xtc_uring_fd *)io_uring_cqe_get_data(cqe);
		if (uf != NULL) {
			if (uf->is_wakeup) {
				if (!wakeup_emitted) {
					int drc = __xtc_io_drain_wakeup(io);
					if (drc != XTC_OK) {
						io_uring_cqe_seen(&io->ring, cqe);
						return drc;
					}
					if (got < max) {
						events[got].tag = NULL;
						events[got].flags = XTC_IO_WAKEUP;
						got++;
					}
					wakeup_emitted = 1;
				}
				/* Re-arm single-shot poll on the wakeup pipe. */
				(void)__submit_poll_add(io, uf);
				(void)io_uring_submit(&io->ring);
			} else {
				if (cqe->res < 0) {
					if (got < max) {
						events[got].tag = uf->tag;
						events[got].flags = XTC_IO_ERR;
						got++;
					}
				} else {
					if (got < max) {
						events[got].tag = uf->tag;
						events[got].flags =
						    __pollres_to_flags((uint32_t)cqe->res);
						got++;
					}
				}
			}
		}
		io_uring_cqe_seen(&io->ring, cqe);
		cqe = NULL;
	}
	*n_out = got;
	return XTC_OK;
}

#endif /* XTC_IO_BACKEND_URING */

typedef int __xtc_io_uring_unused;
