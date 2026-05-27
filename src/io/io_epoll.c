/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * src/io/io_epoll.c
 *	The Linux epoll backend.  Tags travel via epoll_data_t.ptr,
 *	so this file is much smaller than the poll backend.
 */

#define _GNU_SOURCE

#include "xtc_int.h"   /* pulls xtc_config.h — defines XTC_IO_BACKEND_* */

#if defined(XTC_IO_BACKEND_EPOLL)

#include "io_int.h"

#include <errno.h>
#include <unistd.h>
#include <sys/epoll.h>

extern int __xtc_io_drain_wakeup(xtc_io_t *io);

/* Internal sentinel used as the epoll_data.ptr for the wakeup fd.
 * We store the io pointer itself; on poll, ptr == io means wakeup. */

static uint32_t
__interest_to_events(uint32_t interest)
{
	uint32_t e = 0;
	if (interest & XTC_IO_READABLE) e |= EPOLLIN;
	if (interest & XTC_IO_WRITABLE) e |= EPOLLOUT;
	return e;
}

static uint32_t
__epoll_to_flags(uint32_t epev)
{
	uint32_t f = 0;
	if (epev & EPOLLIN)  f |= XTC_IO_READABLE;
	if (epev & EPOLLOUT) f |= XTC_IO_WRITABLE;
	if (epev & EPOLLHUP) f |= XTC_IO_HUP;
	if (epev & EPOLLERR) f |= XTC_IO_ERR;
	return f;
}

int
__xtc_io_backend_init(xtc_io_t *io)
{
	io->epfd = epoll_create1(EPOLL_CLOEXEC);
	if (io->epfd == -1) return XTC_E_INTERNAL;
	return XTC_OK;
}

void
__xtc_io_backend_fini(xtc_io_t *io)
{
	if (io->epfd >= 0) (void)close(io->epfd);
	io->epfd = -1;
}

int
__xtc_io_register_wakeup(xtc_io_t *io, int fd)
{
	struct epoll_event ev;
	ev.events = EPOLLIN;
	ev.data.ptr = io;          /* sentinel: ptr == io means wakeup */
	if (epoll_ctl(io->epfd, EPOLL_CTL_ADD, fd, &ev) == -1)
		return XTC_E_INTERNAL;
	return XTC_OK;
}

/* PUBLIC: int xtc_io_reg_fd __P((xtc_io_t *, int, uint32_t, void *)); */
int
xtc_io_reg_fd(xtc_io_t *io, int fd, uint32_t interest, void *tag)
{
	struct epoll_event ev;
	if (io == NULL || fd < 0 || interest == 0)
		return XTC_E_INVAL;
	ev.events = __interest_to_events(interest);
	ev.data.ptr = tag;
	if (epoll_ctl(io->epfd, EPOLL_CTL_ADD, fd, &ev) == -1) {
		if (errno == EEXIST) return XTC_E_INVAL;
		return XTC_E_INTERNAL;
	}
	return XTC_OK;
}

/* PUBLIC: int xtc_io_mod_fd __P((xtc_io_t *, int, uint32_t, void *)); */
int
xtc_io_mod_fd(xtc_io_t *io, int fd, uint32_t interest, void *tag)
{
	struct epoll_event ev;
	if (io == NULL || fd < 0 || interest == 0)
		return XTC_E_INVAL;
	ev.events = __interest_to_events(interest);
	ev.data.ptr = tag;
	if (epoll_ctl(io->epfd, EPOLL_CTL_MOD, fd, &ev) == -1) {
		if (errno == ENOENT || errno == EBADF) return XTC_E_INVAL;
		return XTC_E_INTERNAL;
	}
	return XTC_OK;
}

/* PUBLIC: int xtc_io_del_fd __P((xtc_io_t *, int)); */
int
xtc_io_del_fd(xtc_io_t *io, int fd)
{
	if (io == NULL || fd < 0)
		return XTC_E_INVAL;
	if (epoll_ctl(io->epfd, EPOLL_CTL_DEL, fd, NULL) == -1) {
		if (errno == ENOENT || errno == EBADF) return XTC_E_INVAL;
		return XTC_E_INTERNAL;
	}
	return XTC_OK;
}

/* PUBLIC: int xtc_io_poll __P((xtc_io_t *, xtc_io_event_t *, int, int64_t, int *)); */
int
xtc_io_poll(xtc_io_t *io, xtc_io_event_t *events, int max,
            int64_t timeout_ns, int *n_out)
{
	struct epoll_event evs[64];
	int batch, got, i, out_idx;
	int timeout_ms;

	if (io == NULL || events == NULL || max <= 0 || n_out == NULL)
		return XTC_E_INVAL;
	*n_out = 0;

	if (timeout_ns < 0)        timeout_ms = -1;
	else if (timeout_ns == 0)  timeout_ms = 0;
	else                       timeout_ms = (int)((timeout_ns + 999999) / 1000000);

	batch = max < (int)(sizeof evs / sizeof evs[0])
	    ? max : (int)(sizeof evs / sizeof evs[0]);

	for (;;) {
		got = epoll_wait(io->epfd, evs, batch, timeout_ms);
		if (got >= 0) break;
		if (errno == EINTR) continue;
		return XTC_E_INTERNAL;
	}
	if (got == 0) return XTC_OK;

	out_idx = 0;
	for (i = 0; i < got && out_idx < max; i++) {
		if (evs[i].data.ptr == io) {
			int rc = __xtc_io_drain_wakeup(io);
			if (rc != XTC_OK) return rc;
			events[out_idx].tag = NULL;
			events[out_idx].flags = XTC_IO_WAKEUP;
		} else {
			events[out_idx].tag = evs[i].data.ptr;
			events[out_idx].flags = __epoll_to_flags(evs[i].events);
		}
		out_idx++;
	}
	*n_out = out_idx;
	return XTC_OK;
}

#endif /* XTC_IO_BACKEND_EPOLL */

typedef int __xtc_io_epoll_unused;   /* avoid -Wpedantic empty-TU when not selected */
