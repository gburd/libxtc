/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
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

/*
 * io-wq worker cap.  Each io_uring ring has a kernel io-wq worker pool
 * (bounded + unbounded); with flags=0 and no cap the BOUNDED pool
 * defaults to ~max(nr_online_cpus, 4) workers PER RING.  A libxtc
 * executor creates one ring per carrier loop, so on a big box
 * (N carriers x M cores) the process accumulates N*M kernel io-wq
 * threads -- thousands on a 192-core host -- which sit idle but still
 * cost the CFS load balancer (update_sg_lb_stats) real CPU walking
 * their runqueues.  A carrier ring does socket poll + a bounded amount
 * of file AIO; it does not need per-cpu io-wq breadth.  Cap it.
 *
 * values[0] = bounded max, values[1] = unbounded max (0 = leave the
 * kernel default for that class).  Process-global, read once per ring
 * at init; settable before the executor starts via
 * xtc_io_set_iowq_max_workers.  0 for a field means "leave kernel
 * default"; the shipped default caps the BOUNDED pool (the one that
 * scales with nr_cpus) to a small value and leaves unbounded alone.
 * On kernels without the register op (< 5.15) the call returns ENOSYS
 * and we silently keep the old behavior.
 */
#define XTC_IOWQ_BOUND_DEFAULT   4u   /* per-ring bounded io-wq cap */
static _Atomic unsigned __xtc_iowq_bound   = XTC_IOWQ_BOUND_DEFAULT;
static _Atomic unsigned __xtc_iowq_unbound = 0u;   /* 0 = kernel default */

/*
 * PUBLIC: void xtc_io_set_iowq_max_workers __P((unsigned, unsigned));
 *
 * Set the per-ring io_uring io-wq worker caps applied to every ring
 * created afterward: bound = bounded-work workers (the ones that scale
 * with nr_cpus and cause the thread explosion on big boxes), unbound =
 * unbounded-work workers.  0 for either leaves the kernel default for
 * that class.  Must be called before xtc_exec_run / xtc_loop_run
 * creates the rings.  A no-op effect on kernels lacking the register
 * operation.
 */
XTC_API void
xtc_io_set_iowq_max_workers(unsigned bound, unsigned unbound)
{
	atomic_store_explicit(&__xtc_iowq_bound, bound, memory_order_relaxed);
	atomic_store_explicit(&__xtc_iowq_unbound, unbound,
	    memory_order_relaxed);
}

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
	 * MULTISHOT for user fds (continuous notification, epoll-EPOLLET /
	 * kqueue-EV_CLEAR semantics).  Single-shot for the internal wakeup
	 * pipe so a drained poll reports no stale wakeup on the next poll
	 * (the W3 coalesce contract).  The lost-wakeup window a naive
	 * single-shot has (drain-then-rearm: a cross-thread write landing
	 * between draining the pipe and re-arming is missed) is closed in
	 * the drain path by RE-ARMING BEFORE draining -- see the wakeup
	 * branch of xtc_io_poll. */
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
	/*
	 * Cap this ring's kernel io-wq worker pool so N carriers on an
	 * M-core box do not accumulate N*M idle io-wq threads (see the
	 * XTC_IOWQ_BOUND_DEFAULT comment).  Best-effort: an older kernel
	 * without the register op returns -ENOSYS -- ignore it and keep
	 * the previous (uncapped) behavior. */
	{
		unsigned vals[2];
		vals[0] = atomic_load_explicit(&__xtc_iowq_bound,
		    memory_order_relaxed);
		vals[1] = atomic_load_explicit(&__xtc_iowq_unbound,
		    memory_order_relaxed);
		if (vals[0] != 0u || vals[1] != 0u)
			(void)io_uring_register_iowq_max_workers(&io->ring,
			    vals);
	}
	io->fds = NULL;
	io->zombies = NULL;
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
	/* Free any zombies whose terminal CQE never arrived (e.g. a
	 * multishot that auto-disarmed); the ring is being torn down so
	 * no CQE can reference them after this. */
	for (p = io->zombies; p != NULL; p = n) {
		n = p->next;
		__os_free(p);
	}
	io->zombies = NULL;
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

/* PUBLIC: int xtc_io_aio_submit __P((xtc_io_t *, xtc_aio_t *)); */
int
xtc_io_aio_submit(xtc_io_t *io, xtc_aio_t *a)
{
	struct io_uring_sqe *sqe;
	if (io == NULL || a == NULL || a->fd < 0)
		return XTC_E_INVAL;
	sqe = io_uring_get_sqe(&io->ring);
	if (sqe == NULL) {
		(void)io_uring_submit(&io->ring);
		sqe = io_uring_get_sqe(&io->ring);
		if (sqe == NULL) return XTC_E_AGAIN;
	}
	switch (a->op) {
	case XTC_AIO_PREAD:
		io_uring_prep_read(sqe, a->fd, a->buf, a->len,
		    (unsigned long long)a->off);
		break;
	case XTC_AIO_PWRITE:
		io_uring_prep_write(sqe, a->fd, a->buf, a->len,
		    (unsigned long long)a->off);
		break;
	case XTC_AIO_PREADV:
		io_uring_prep_readv(sqe, a->fd,
		    (const struct iovec *)a->iov, (unsigned)a->iovcnt,
		    (unsigned long long)a->off);
		break;
	case XTC_AIO_PWRITEV:
		io_uring_prep_writev(sqe, a->fd,
		    (const struct iovec *)a->iov, (unsigned)a->iovcnt,
		    (unsigned long long)a->off);
		break;
	case XTC_AIO_FSYNC:
		io_uring_prep_fsync(sqe, a->fd, 0);
		break;
	case XTC_AIO_FDATASYNC:
		io_uring_prep_fsync(sqe, a->fd, IORING_FSYNC_DATASYNC);
		break;
	default:
		return XTC_E_INVAL;
	}
	a->done = 0;
	a->res = 0;
	/* Low-bit tag distinguishes this completion from a poll-add CQE. */
	io_uring_sqe_set_data(sqe, (void *)((uintptr_t)a | 1u));
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
	/*
	 * Do not free uf here: the multishot poll submitted with
	 * user_data == uf may still have CQEs in flight (an already-ready
	 * notification, plus the -ECANCELED terminal CQE the poll_remove
	 * triggers).  Freeing now and then draining those CQEs is a
	 * use-after-free.  Move uf to the zombie list and free it when
	 * its terminal (non-MORE) CQE is drained in xtc_io_poll.
	 */
	uf->dead = 1;
	uf->next = io->zombies;
	io->zombies = uf;
	return XTC_OK;
}

/* PUBLIC: int xtc_io_poll __P((xtc_io_t *, xtc_io_event_t *, int, int64_t, int *)); */
/* XTC_NOALLOC_BEGIN: io_uring per-poll CQE reap path (PLAN.md 19.23).
 * KNOWN GAP (flagged in the s_noalloc rollout report, not silenced):
 * the zombie-reap branch a few lines below (the `if (uf != NULL &&
 * uf->dead)` block) calls __os_free(uf) to release a deregistered
 * fd's node once its terminal CQE drains.  That call is bounded to
 * once per xtc_io_del_fd (not once per steady-state poll), so it is
 * carved OUT of this marked region below rather than hidden behind an
 * XTC_NOALLOC_OK -- the region resumes immediately after it.  The
 * steady-state dispatch of a ready fd, an AIO completion, or the
 * wakeup event never allocates. */
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
		void *data;
		if (cqe == NULL) {
			if (io_uring_peek_cqe(&io->ring, &cqe) != 0) break;
		}
		data = io_uring_cqe_get_data(cqe);
		/* Low bit tags an async file-I/O completion (xtc_aio_t *);
		 * __xtc_uring_fd pointers are calloc-aligned, so bit 0 is 0. */
		if (((uintptr_t)data & 1u) != 0) {
			xtc_aio_t *a = (xtc_aio_t *)((uintptr_t)data & ~(uintptr_t)1);
			a->res = cqe->res;
			a->done = 1;
			if (got < max) {
				events[got].tag = a->tag;
				events[got].flags = XTC_IO_AIO;
				got++;
			}
			io_uring_cqe_seen(&io->ring, cqe);
			cqe = NULL;
			continue;
		}
		uf = (struct __xtc_uring_fd *)data;
		if (uf != NULL && uf->dead) {
/* XTC_NOALLOC_END -- see the KNOWN GAP note above xtc_io_poll: the
 * next block's __os_free(uf) is a real, bounded (per-deregistration,
 * not per-poll) allocator touch left OUT of the marked region rather
 * than hidden behind an exception marker. */
			/* Registration was deleted.  Do not dispatch to it.
			 * Once its multishot poll delivers the terminal CQE
			 * (no IORING_CQE_F_MORE -- e.g. the -ECANCELED from
			 * poll_remove), no further CQE can reference uf, so it
			 * is safe to free. */
			if (!(cqe->flags & IORING_CQE_F_MORE)) {
				struct __xtc_uring_fd **pp;
				for (pp = &io->zombies; *pp != NULL;
				    pp = &(*pp)->next) {
					if (*pp == uf) {
						*pp = uf->next;
						break;
					}
				}
				__os_free(uf);
			}
			io_uring_cqe_seen(&io->ring, cqe);
			cqe = NULL;
			continue;
/* XTC_NOALLOC_BEGIN: resuming the steady-state dispatch path (ready fd
 * / wakeup) after the zombie-reap carve-out above. */
		}
		if (uf != NULL) {
			if (uf->is_wakeup) {
				/* Re-arm the single-shot wakeup poll BEFORE draining
				 * the pipe.  Ordering is the fix for the cross-thread
				 * idle-loop wake miss (carrier report 2026-07-06): if
				 * we drained first and re-armed after, a foreign
				 * xtc_io_wakeup write landing in that window would
				 * find no armed poll and be missed, hanging the idle
				 * loop forever.  Re-arming first means the poll is
				 * always armed across the drain, so any write -- before
				 * or after the drain -- surfaces a CQE and breaks the
				 * next io_uring_wait.  (A write between re-arm and
				 * drain is caught by the already-armed poll; a byte
				 * still present at re-arm fires the poll immediately
				 * -- level-triggered -- which the next poll coalesces.) */
				(void)__submit_poll_add(io, uf);
				(void)io_uring_submit(&io->ring);
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
/* XTC_NOALLOC_END */

#endif /* XTC_IO_BACKEND_URING */

typedef int __xtc_io_uring_unused;
