/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * src/io/io_kqueue.c
 *	The BSD/macOS kqueue backend.  Code complete; runtime tested
 *	on a *BSD VM (M6.5 task).  Tags travel via the kevent's udata
 *	field, mirroring the epoll backend's epoll_data.ptr trick.
 *
 *	The wakeup pipe registration uses a sentinel udata == io.
 */

#include "xtc_int.h"

#if defined(XTC_IO_BACKEND_KQUEUE)

#include "io_int.h"

#include <errno.h>
#include <unistd.h>
#include <sys/event.h>
#include <sys/time.h>
#include <sys/types.h>

extern int __xtc_io_drain_wakeup(xtc_io_t *io);

/* ----- registered-fd tracker (kqueue-only) ----------------------- */

static int
__find_reg(xtc_io_t *io, int fd)
{
	int i;
	for (i = 0; i < io->n_reg; i++)
		if (io->reg_fds[i] == fd) return i;
	return -1;
}

static int
__add_reg(xtc_io_t *io, int fd)
{
	if (io->n_reg >= io->cap_reg) {
		int new_cap = io->cap_reg == 0 ? 16 : io->cap_reg * 2;
		void *p = NULL;
		int rc = __os_realloc(io->reg_fds,
		    sizeof(int) * (size_t)new_cap, &p);
		if (rc != XTC_OK) return rc;
		io->reg_fds = p;
		io->cap_reg = new_cap;
	}
	io->reg_fds[io->n_reg++] = fd;
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
	io->epfd = kqueue();
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

static int
__kev_register(xtc_io_t *io, int fd, uint32_t interest, void *udata, int del)
{
	struct kevent kev[2];
	int n = 0;
	if (interest & XTC_IO_READABLE) {
		EV_SET(&kev[n], fd, EVFILT_READ,
		    del ? EV_DELETE : (EV_ADD | EV_CLEAR),
		    0, 0, udata);
		n++;
	}
	if (interest & XTC_IO_WRITABLE) {
		EV_SET(&kev[n], fd, EVFILT_WRITE,
		    del ? EV_DELETE : (EV_ADD | EV_CLEAR),
		    0, 0, udata);
		n++;
	}
	if (n == 0) return XTC_OK;
	if (kevent(io->epfd, kev, n, NULL, 0, NULL) < 0) {
		if (errno == ENOENT) return XTC_E_INVAL;
		return XTC_E_INTERNAL;
	}
	return XTC_OK;
}

int
__xtc_io_register_wakeup(xtc_io_t *io, int fd)
{
	return __kev_register(io, fd, XTC_IO_READABLE, io, 0);
}

/* PUBLIC: int xtc_io_reg_fd __P((xtc_io_t *, int, uint32_t, void *)); */
int
xtc_io_reg_fd(xtc_io_t *io, int fd, uint32_t interest, void *tag)
{
	int rc;
	if (io == NULL || fd < 0 || interest == 0) return XTC_E_INVAL;
	if (__find_reg(io, fd) >= 0) return XTC_E_INVAL;   /* duplicate */
	if ((rc = __add_reg(io, fd)) != XTC_OK) return rc;
	rc = __kev_register(io, fd, interest, tag, 0);
	if (rc != XTC_OK) __del_reg(io, fd);
	return rc;
}

/* PUBLIC: int xtc_io_mod_fd __P((xtc_io_t *, int, uint32_t, void *)); */
int
xtc_io_mod_fd(xtc_io_t *io, int fd, uint32_t interest, void *tag)
{
	if (io == NULL || fd < 0 || interest == 0) return XTC_E_INVAL;
	if (__find_reg(io, fd) < 0) return XTC_E_INVAL;
	/* Clear both filters first so a transition from R+W to just R
	 * doesn't leave a stale W subscription.  Then re-register with
	 * the requested interest. */
	(void)__kev_register(io, fd, XTC_IO_READABLE | XTC_IO_WRITABLE,
	                     NULL, 1);
	return __kev_register(io, fd, interest, tag, 0);
}

/* PUBLIC: int xtc_io_del_fd __P((xtc_io_t *, int)); */
int
xtc_io_del_fd(xtc_io_t *io, int fd)
{
	if (io == NULL || fd < 0) return XTC_E_INVAL;
	if (__find_reg(io, fd) < 0) return XTC_E_INVAL;
	(void)__kev_register(io, fd, XTC_IO_READABLE | XTC_IO_WRITABLE,
	                     NULL, 1);
	__del_reg(io, fd);
	return XTC_OK;
}

/* PUBLIC: int xtc_io_poll __P((xtc_io_t *, xtc_io_event_t *, int, int64_t, int *)); */
int
xtc_io_poll(xtc_io_t *io, xtc_io_event_t *events, int max,
            int64_t timeout_ns, int *n_out)
{
	struct kevent evs[64];
	struct timespec ts, *tsp;
	int batch, got, i, out_idx;

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
	batch = max < (int)(sizeof evs / sizeof evs[0])
	    ? max : (int)(sizeof evs / sizeof evs[0]);

	for (;;) {
		got = kevent(io->epfd, NULL, 0, evs, batch, tsp);
		if (got >= 0) break;
		if (errno == EINTR) continue;
		return XTC_E_INTERNAL;
	}
	if (got == 0) return XTC_OK;

	out_idx = 0;
	for (i = 0; i < got && out_idx < max; i++) {
		if (evs[i].udata == io) {
			int rc = __xtc_io_drain_wakeup(io);
			if (rc != XTC_OK) return rc;
			events[out_idx].tag = NULL;
			events[out_idx].flags = XTC_IO_WAKEUP;
		} else {
			uint32_t f = 0;
			if (evs[i].filter == EVFILT_READ)  f |= XTC_IO_READABLE;
			if (evs[i].filter == EVFILT_WRITE) f |= XTC_IO_WRITABLE;
			if (evs[i].flags & EV_EOF)         f |= XTC_IO_HUP;
			if (evs[i].flags & EV_ERROR)       f |= XTC_IO_ERR;
			events[out_idx].tag = evs[i].udata;
			events[out_idx].flags = f;
		}
		out_idx++;
	}
	*n_out = out_idx;
	return XTC_OK;
}

#endif /* XTC_IO_BACKEND_KQUEUE */

typedef int __xtc_io_kqueue_unused;
