/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/io/io_solaris.c
 *	Solaris / illumos event-ports backend.
 *
 *	port_associate semantics: each association is one-shot -- once
 *	an event fires, the registration is automatically dropped, and
 *	must be re-armed if the caller wants to keep watching.  We
 *	carry the per-fd interest mask + tag in a small side table
 *	and re-associate after each delivery, mirroring how the kqueue
 *	backend explicitly tracks fds (and giving us free duplicate
 *	detection).
 *
 *	Wakeup pipe: we use a sentinel udata = io_self pointer so the
 *	poll loop can distinguish wakeups from user fd events; same
 *	pattern as kqueue.
 */

#include "xtc_int.h"

#if defined(XTC_IO_BACKEND_SOLARIS)

#include "io_int.h"

#include <errno.h>
#include <port.h>
#include <poll.h>
#include <unistd.h>
#include <sys/time.h>
#include <aio.h>
#include <string.h>
#include <stdlib.h>

extern int __xtc_io_drain_wakeup(xtc_io_t *io);

/*
 * Native async file I/O via POSIX AIO + SIGEV_PORT.  illumos delivers an
 * aio_read/aio_write/aio_fsync completion as a PORT_SOURCE_AIO event on
 * the same event port the loop already waits on, so file AIO integrates
 * with no thread hop (unlike the blocking-pool offload other readiness-
 * only backends fall back to).  Verified on illumos-31d3d510d0.
 *
 * Each submission carries a heap sol_aio whose aiocb AND port_notify_t
 * MUST outlive the async call (a stack-local port_notify was the cause
 * of the initial EINVAL during bring-up); portnfy_user points back at
 * the sol_aio so the drain recovers the xtc_aio_t, publishes its result,
 * and frees the sol_aio.  If submission fails (e.g. no AIO support),
 * xtc_io_aio_submit returns XTC_E_NOSYS and xtc_aio falls back to the
 * blocking pool -- the offload path stays the proven default.
 */
struct sol_aio {
	struct aiocb   cb;
	port_notify_t  pn;
	xtc_aio_t     *a;
};

/* PUBLIC: int xtc_io_aio_submit __P((xtc_io_t *, xtc_aio_t *)); */
int
xtc_io_aio_submit(xtc_io_t *io, xtc_aio_t *a)
{
	struct sol_aio *s;
	int rc;
	if (io == NULL || a == NULL || a->fd < 0)
		return XTC_E_INVAL;
	/* Vectored ops have no lio_listio-with-port form here; decline them
	 * so xtc_aio offloads (preadv/pwritev via the pool). */
	if (a->op != XTC_AIO_PREAD && a->op != XTC_AIO_PWRITE &&
	    a->op != XTC_AIO_FSYNC && a->op != XTC_AIO_FDATASYNC)
		return XTC_E_NOSYS;
	if ((rc = __os_calloc(1, sizeof *s, (void **)&s)) != XTC_OK)
		return XTC_E_NOSYS;   /* let the caller offload */
	s->a = a;
	s->pn.portnfy_port = io->epfd;
	s->pn.portnfy_user = s;
	s->cb.aio_fildes = a->fd;
	s->cb.aio_reqprio = 0;
	s->cb.aio_sigevent.sigev_notify = SIGEV_PORT;
	s->cb.aio_sigevent.sigev_value.sival_ptr = &s->pn;
	switch (a->op) {
	case XTC_AIO_PREAD:
		s->cb.aio_buf = a->buf;
		s->cb.aio_nbytes = a->len;
		s->cb.aio_offset = (off_t)a->off;
		rc = aio_read(&s->cb);
		break;
	case XTC_AIO_PWRITE:
		s->cb.aio_buf = a->buf;
		s->cb.aio_nbytes = a->len;
		s->cb.aio_offset = (off_t)a->off;
		rc = aio_write(&s->cb);
		break;
	case XTC_AIO_FSYNC:
		rc = aio_fsync(O_SYNC, &s->cb);
		break;
	case XTC_AIO_FDATASYNC:
		rc = aio_fsync(O_DSYNC, &s->cb);
		break;
	default:
		rc = -1;
		break;
	}
	if (rc != 0) {
		__os_free(s);
		return XTC_E_NOSYS;   /* AIO unavailable: offload instead */
	}
	return XTC_OK;
}

/* Translate xtc interest -> POLL* events. */
static int
__pollev_for(uint32_t interest)
{
	int ev = 0;
	if (interest & XTC_IO_READABLE) ev |= POLLIN;
	if (interest & XTC_IO_WRITABLE) ev |= POLLOUT;
	return ev;
}

static int
__find_reg(xtc_io_t *io, int fd)
{
	int i;
	for (i = 0; i < io->n_reg; i++)
		if (io->reg_fds[i].fd == fd) return i;
	return -1;
}

static int
__add_reg(xtc_io_t *io, int fd, uint32_t interest, void *tag)
{
	if (io->n_reg >= io->cap_reg) {
		int new_cap = io->cap_reg == 0 ? 16 : io->cap_reg * 2;
		void *p = NULL;
		int rc = __os_realloc(io->reg_fds,
		    sizeof(*io->reg_fds) * (size_t)new_cap, &p);
		if (rc != XTC_OK) return rc;
		io->reg_fds = p;
		io->cap_reg = new_cap;
	}
	io->reg_fds[io->n_reg].fd       = fd;
	io->reg_fds[io->n_reg].interest = interest;
	io->reg_fds[io->n_reg].tag      = tag;
	io->n_reg++;
	return XTC_OK;
}

static void
__del_reg(xtc_io_t *io, int fd)
{
	int idx = __find_reg(io, fd);
	if (idx < 0) return;
	io->n_reg--;
	if (idx != io->n_reg) io->reg_fds[idx] = io->reg_fds[io->n_reg];
}

int
__xtc_io_backend_init(xtc_io_t *io)
{
	io->epfd = port_create();
	if (io->epfd < 0) return XTC_E_INTERNAL;
	io->reg_fds = NULL;
	io->n_reg = io->cap_reg = 0;
	return XTC_OK;
}

void
__xtc_io_backend_fini(xtc_io_t *io)
{
	if (io->epfd >= 0) (void)close(io->epfd);
	io->epfd = -1;
	__os_free(io->reg_fds);
	io->reg_fds = NULL;
	io->n_reg = io->cap_reg = 0;
}

int
__xtc_io_register_wakeup(xtc_io_t *io, int fd)
{
	if (port_associate(io->epfd, PORT_SOURCE_FD, (uintptr_t)fd,
	    POLLIN, io) < 0)
		return XTC_E_INTERNAL;
	return XTC_OK;
}

/* Re-arm a registration after a port_get drops it.  Caller already
 * owns the side-table entry; we just push it back to the kernel. */
static int
__rearm(xtc_io_t *io, int fd, uint32_t interest, void *tag)
{
	if (port_associate(io->epfd, PORT_SOURCE_FD, (uintptr_t)fd,
	    __pollev_for(interest), tag) < 0)
		return XTC_E_INTERNAL;
	return XTC_OK;
}

/* PUBLIC: int xtc_io_reg_fd __P((xtc_io_t *, int, uint32_t, void *)); */
int
xtc_io_reg_fd(xtc_io_t *io, int fd, uint32_t interest, void *tag)
{
	int rc;
	if (io == NULL || fd < 0 || interest == 0) return XTC_E_INVAL;
	if (__find_reg(io, fd) >= 0) return XTC_E_INVAL;
	if ((rc = __add_reg(io, fd, interest, tag)) != XTC_OK) return rc;
	if ((rc = __rearm(io, fd, interest, tag)) != XTC_OK) {
		__del_reg(io, fd);
		return rc;
	}
	return XTC_OK;
}

/* PUBLIC: int xtc_io_mod_fd __P((xtc_io_t *, int, uint32_t, void *)); */
int
xtc_io_mod_fd(xtc_io_t *io, int fd, uint32_t interest, void *tag)
{
	int idx;
	if (io == NULL || fd < 0 || interest == 0) return XTC_E_INVAL;
	idx = __find_reg(io, fd);
	if (idx < 0) return XTC_E_INVAL;
	(void)port_dissociate(io->epfd, PORT_SOURCE_FD, (uintptr_t)fd);
	io->reg_fds[idx].interest = interest;
	io->reg_fds[idx].tag      = tag;
	return __rearm(io, fd, interest, tag);
}

/* PUBLIC: int xtc_io_del_fd __P((xtc_io_t *, int)); */
int
xtc_io_del_fd(xtc_io_t *io, int fd)
{
	if (io == NULL || fd < 0) return XTC_E_INVAL;
	if (__find_reg(io, fd) < 0) return XTC_E_INVAL;
	(void)port_dissociate(io->epfd, PORT_SOURCE_FD, (uintptr_t)fd);
	__del_reg(io, fd);
	return XTC_OK;
}

/* Non-io_uring backends: reg/del are kernel-synchronized (epoll) or
 * this backend keeps its own registry; the cross-loop deferred-
 * unregister that io_uring needs is a no-op passthrough here (the
 * caller is xtc_proc_wait_fd cleanup after a migration).  Provided so
 * the single caller links on every backend. */
/* No-op: only io_uring has a single-owner fds list + SQ ring that needs
 * an eagerly-recorded owner thread; this backend is kernel-synchronized
 * or keeps its own registry.  Provided so every backend links. */
void
__xtc_io_set_owner(xtc_io_t *io)
{
	(void)io;
}

int
__xtc_io_defer_del_fd(xtc_io_t *io, int fd)
{
	return xtc_io_del_fd(io, fd);
}

/* PUBLIC: int xtc_io_poll __P((xtc_io_t *, xtc_io_event_t *, int, int64_t, int *)); */
int
xtc_io_poll(xtc_io_t *io, xtc_io_event_t *events, int max,
            int64_t timeout_ns, int *n_out)
{
	port_event_t evs[64];
	struct timespec ts, *tsp;
	uint_t  nevents = 1;     /* we'll batch up to nevents */
	uint_t  i;
	int     rc, out_idx;

	if (io == NULL || events == NULL || max <= 0 || n_out == NULL)
		return XTC_E_INVAL;
	*n_out = 0;

	if (timeout_ns < 0) {
		tsp = NULL;
	} else {
		ts.tv_sec  = (time_t)(timeout_ns / 1000000000LL);
		ts.tv_nsec = (long)(timeout_ns % 1000000000LL);
		tsp = &ts;
	}
	if (max < (int)(sizeof evs / sizeof evs[0])) nevents = (uint_t)max;
	else                                          nevents = (uint_t)(sizeof evs / sizeof evs[0]);

	for (;;) {
		uint_t got = 1;     /* MIN events to wait for; buffer size is `nevents` */
		rc = port_getn(io->epfd, evs, nevents, &got, tsp);
		if (rc < 0) {
			if (errno == EINTR) continue;
			/* On illumos, ETIME with got > 0 means partial delivery
			 * -- keep what we got and return.  ETIME with got == 0
			 * is a normal poll timeout. */
			if (errno == ETIME) {
				if (got == 0) { *n_out = 0; return XTC_OK; }
				/* fall through with got events */
			} else {
				return XTC_E_INTERNAL;
			}
		}
		nevents = got;
		break;
	}

	out_idx = 0;
	for (i = 0; i < nevents && out_idx < max; i++) {
		uint32_t flags = 0;
		int      fd;
		void    *tag = evs[i].portev_user;

		/* Native AIO completion: a PORT_SOURCE_AIO event's portev_user
		 * is the sol_aio we submitted.  Publish the result onto the
		 * xtc_aio_t, emit an XTC_IO_AIO event so the loop wakes the
		 * parked fiber (a->tag), and free the sol_aio. */
		if (evs[i].portev_source == PORT_SOURCE_AIO) {
			struct sol_aio *s = evs[i].portev_user;
			if (s != NULL) {
				xtc_aio_t *a = s->a;
				int aerr = aio_error(&s->cb);
				ssize_t ares = aio_return(&s->cb);
				if (a != NULL) {
					a->res = (aerr == 0)
					    ? (int32_t)ares : -(int32_t)aerr;
					a->done = 1;
					if (out_idx < max) {
						events[out_idx].tag = a->tag;
						events[out_idx].flags = XTC_IO_AIO;
						out_idx++;
					}
				}
				__os_free(s);
			}
			continue;
		}

		if (evs[i].portev_source != PORT_SOURCE_FD) continue;
		fd = (int)evs[i].portev_object;

		if (tag == io) {
			/* Wakeup pipe: drain it, then re-associate (one-shot).
			 * This ordering is lost-wakeup-free on event ports: a
			 * foreign xtc_io_wakeup write landing in the drain->rearm
			 * window leaves a byte in the pipe, and port_associate on
			 * a pipe that already has data delivers a POLLIN event
			 * immediately (on the next port_getn) -- so the wakeup is
			 * seen, not lost.  A write after the re-associate fires
			 * the freshly-armed association.  Draining before the
			 * re-arm also keeps the W3 coalesce contract (no stale
			 * wakeup reported once the pipe is empty). */
			(void)__xtc_io_drain_wakeup(io);
			(void)port_associate(io->epfd, PORT_SOURCE_FD,
			    (uintptr_t)fd, POLLIN, io);
			events[out_idx].tag = NULL;
			events[out_idx].flags = XTC_IO_WAKEUP;
			out_idx++;
			continue;
		}

		if (evs[i].portev_events & POLLIN)  flags |= XTC_IO_READABLE;
		if (evs[i].portev_events & POLLOUT) flags |= XTC_IO_WRITABLE;
		if (evs[i].portev_events & POLLHUP) flags |= XTC_IO_HUP;
		if (evs[i].portev_events & POLLERR) flags |= XTC_IO_ERR;
		events[out_idx].tag = tag;
		events[out_idx].flags = flags;
		out_idx++;

		/* Re-arm: port associations are one-shot.  If the fd was
		 * deleted between delivery and now, find_reg returns -1
		 * and we skip the re-arm. */
		{
			int idx = __find_reg(io, fd);
			if (idx >= 0)
				(void)__rearm(io, fd,
				    io->reg_fds[idx].interest,
				    io->reg_fds[idx].tag);
		}
	}
	*n_out = out_idx;
	return XTC_OK;
}

#endif /* XTC_IO_BACKEND_SOLARIS */

typedef int __xtc_io_solaris_unused;
