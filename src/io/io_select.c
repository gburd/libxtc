/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * src/io/io_select.c
 *	The select(2) backend.  The most portable IO multiplexor —
 *	works on every POSIX-ish system and Windows Winsock.
 *	Capped at FD_SETSIZE descriptors (typically 1024 on Linux,
 *	configurable per OS).  Use poll/epoll/kqueue if you have
 *	more than ~512 simultaneous fds.
 */

#define _POSIX_C_SOURCE 200809L

#include "xtc_int.h"

#if defined(XTC_IO_BACKEND_SELECT)

#include "io_int.h"

#include <errno.h>
#include <string.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

#define WAKEUP_SLOT 0

extern int __xtc_io_drain_wakeup(xtc_io_t *io);

static int
__find_slot(const xtc_io_t *io, int fd)
{
	int i;
	for (i = 0; i < io->n; i++)
		if (io->fds[i] == fd) return i;
	return -1;
}

static int
__grow(xtc_io_t *io)
{
	int new_cap = io->cap == 0 ? 8 : io->cap * 2;
	void *nf = NULL, *ni = NULL, *nt = NULL;
	int rc;
	if ((rc = __os_realloc(io->fds, sizeof(int) * (size_t)new_cap, &nf)) != XTC_OK)
		return rc;
	io->fds = nf;
	if ((rc = __os_realloc(io->interests, sizeof(uint32_t) * (size_t)new_cap, &ni)) != XTC_OK)
		return rc;
	io->interests = ni;
	if ((rc = __os_realloc(io->tags, sizeof(void *) * (size_t)new_cap, &nt)) != XTC_OK)
		return rc;
	io->tags = nt;
	io->cap = new_cap;
	return XTC_OK;
}

int
__xtc_io_backend_init(xtc_io_t *io)
{
	io->fds = NULL; io->interests = NULL; io->tags = NULL;
	io->n = 0; io->cap = 0;
	return __grow(io);
}

void
__xtc_io_backend_fini(xtc_io_t *io)
{
	__os_free(io->fds); io->fds = NULL;
	__os_free(io->interests); io->interests = NULL;
	__os_free(io->tags); io->tags = NULL;
	io->n = io->cap = 0;
}

int
__xtc_io_register_wakeup(xtc_io_t *io, int fd)
{
	if (io->n >= io->cap) {
		int rc = __grow(io);
		if (rc != XTC_OK) return rc;
	}
	io->fds[WAKEUP_SLOT]       = fd;
	io->interests[WAKEUP_SLOT] = XTC_IO_READABLE;
	io->tags[WAKEUP_SLOT]      = NULL;     /* sentinel */
	io->n = 1;
	return XTC_OK;
}

int
xtc_io_reg_fd(xtc_io_t *io, int fd, uint32_t interest, void *tag)
{
	int rc;
	if (io == NULL || fd < 0 || interest == 0) return XTC_E_INVAL;
	if (fd >= FD_SETSIZE) return XTC_E_RESOURCE;   /* select(2) limit */
	if (__find_slot(io, fd) >= 0) return XTC_E_INVAL;
	if (io->n >= io->cap) {
		if ((rc = __grow(io)) != XTC_OK) return rc;
	}
	io->fds[io->n]       = fd;
	io->interests[io->n] = interest;
	io->tags[io->n]      = tag;
	io->n++;
	return XTC_OK;
}

int
xtc_io_mod_fd(xtc_io_t *io, int fd, uint32_t interest, void *tag)
{
	int slot;
	if (io == NULL || fd < 0 || interest == 0) return XTC_E_INVAL;
	if ((slot = __find_slot(io, fd)) < 0) return XTC_E_INVAL;
	io->interests[slot] = interest;
	io->tags[slot]      = tag;
	return XTC_OK;
}

int
xtc_io_del_fd(xtc_io_t *io, int fd)
{
	int slot;
	if (io == NULL || fd < 0) return XTC_E_INVAL;
	if ((slot = __find_slot(io, fd)) < 0) return XTC_E_INVAL;
	if (slot == WAKEUP_SLOT) return XTC_E_INVAL;
	/* Compact tail. */
	if (slot != io->n - 1) {
		io->fds[slot]       = io->fds[io->n - 1];
		io->interests[slot] = io->interests[io->n - 1];
		io->tags[slot]      = io->tags[io->n - 1];
	}
	io->n--;
	return XTC_OK;
}

int
xtc_io_poll(xtc_io_t *io, xtc_io_event_t *out_events, int max_events,
            int64_t timeout_ns, int *out_n)
{
	fd_set rd, wr, er;
	int max_fd = -1, i, rc, drained = 0;
	struct timeval tv;
	struct timeval *tvp;

	if (io == NULL || out_events == NULL || max_events <= 0 || out_n == NULL)
		return XTC_E_INVAL;
	*out_n = 0;

	FD_ZERO(&rd); FD_ZERO(&wr); FD_ZERO(&er);
	for (i = 0; i < io->n; i++) {
		if (io->fds[i] >= FD_SETSIZE) continue;
		if (io->interests[i] & XTC_IO_READABLE) FD_SET(io->fds[i], &rd);
		if (io->interests[i] & XTC_IO_WRITABLE) FD_SET(io->fds[i], &wr);
		FD_SET(io->fds[i], &er);
		if (io->fds[i] > max_fd) max_fd = io->fds[i];
	}
	if (timeout_ns < 0) {
		tvp = NULL;
	} else {
		tv.tv_sec  = (long)(timeout_ns / 1000000000LL);
		tv.tv_usec = (long)((timeout_ns % 1000000000LL) / 1000LL);
		tvp = &tv;
	}
	rc = select(max_fd + 1, &rd, &wr, &er, tvp);
	if (rc < 0) {
		if (errno == EINTR) return XTC_OK;
		return XTC_E_INTERNAL;
	}
	if (rc == 0) return XTC_OK;

	for (i = 0; i < io->n && *out_n < max_events; i++) {
		uint32_t f = 0;
		int fd = io->fds[i];
		if (fd >= FD_SETSIZE) continue;
		if (FD_ISSET(fd, &rd)) f |= XTC_IO_READABLE;
		if (FD_ISSET(fd, &wr)) f |= XTC_IO_WRITABLE;
		if (FD_ISSET(fd, &er)) f |= XTC_IO_ERR;
		if (f == 0) continue;

		if (i == WAKEUP_SLOT) {
			if (!drained) {
				(void)__xtc_io_drain_wakeup(io);
				drained = 1;
			}
			/* Emit a wakeup event so the caller can dispatch. */
			out_events[*out_n].flags = XTC_IO_WAKEUP;
			out_events[*out_n].tag   = NULL;
			(*out_n)++;
			continue;
		}
		out_events[*out_n].flags = f;
		out_events[*out_n].tag = io->tags[i];
		(*out_n)++;
	}
	return XTC_OK;
}

#endif /* XTC_IO_BACKEND_SELECT */
