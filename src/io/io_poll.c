/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * src/io/io_poll.c
 *	The poll(2) backend.  Portable to every Tier 1 platform; the
 *	floor we promise (PLAN.md §3.6).  Maintains a parallel
 *	(pollfd[], tag[]) so the public API can return user tags even
 *	though poll(2) itself does not store user data per fd.
 */

#define _POSIX_C_SOURCE 200809L

#include "xtc_int.h"   /* pulls xtc_config.h — defines XTC_IO_BACKEND_* */

#if defined(XTC_IO_BACKEND_POLL)

#include "io_int.h"

#include <errno.h>
#include <poll.h>
#include <unistd.h>
#include <string.h>

/* Wakeup pipe is always at slot 0; user fds start at 1. */
#define WAKEUP_SLOT 0

extern int  __xtc_io_drain_wakeup(xtc_io_t *io);

static short
__interest_to_events(uint32_t interest)
{
	short e = 0;
	if (interest & XTC_IO_READABLE) e |= POLLIN;
	if (interest & XTC_IO_WRITABLE) e |= POLLOUT;
	return e;
}

static uint32_t
__revents_to_flags(short revents)
{
	uint32_t f = 0;
	if (revents & POLLIN)  f |= XTC_IO_READABLE;
	if (revents & POLLOUT) f |= XTC_IO_WRITABLE;
	if (revents & POLLHUP) f |= XTC_IO_HUP;
	if (revents & POLLERR) f |= XTC_IO_ERR;
	if (revents & POLLNVAL) f |= XTC_IO_ERR;
	return f;
}

static int
__find_slot(const xtc_io_t *io, int fd)
{
	int i;
	for (i = 0; i < io->n; i++)
		if (io->pfds[i].fd == fd)
			return i;
	return -1;
}

static int
__grow(xtc_io_t *io)
{
	int new_cap = io->cap == 0 ? 8 : io->cap * 2;
	void *npf = NULL, *ntg = NULL;
	int rc;
	if ((rc = __os_realloc(io->pfds, sizeof(*io->pfds) * (size_t)new_cap, &npf)) != XTC_OK)
		return rc;
	io->pfds = npf;
	if ((rc = __os_realloc(io->tags, sizeof(*io->tags) * (size_t)new_cap, &ntg)) != XTC_OK)
		return rc;
	io->tags = ntg;
	io->cap = new_cap;
	return XTC_OK;
}

int
__xtc_io_backend_init(xtc_io_t *io)
{
	io->pfds = NULL;
	io->tags = NULL;
	io->n = 0;
	io->cap = 0;
	return __grow(io);
}

void
__xtc_io_backend_fini(xtc_io_t *io)
{
	__os_free(io->pfds);
	__os_free(io->tags);
	io->pfds = NULL;
	io->tags = NULL;
	io->n = io->cap = 0;
}

int
__xtc_io_register_wakeup(xtc_io_t *io, int fd)
{
	if (io->n >= io->cap) {
		int rc = __grow(io);
		if (rc != XTC_OK) return rc;
	}
	io->pfds[WAKEUP_SLOT].fd = fd;
	io->pfds[WAKEUP_SLOT].events = POLLIN;
	io->pfds[WAKEUP_SLOT].revents = 0;
	io->tags[WAKEUP_SLOT] = NULL;   /* sentinel: wakeup */
	io->n = 1;
	return XTC_OK;
}

/*
 * PUBLIC: int xtc_io_reg_fd __P((xtc_io_t *, int, uint32_t, void *));
 */
int
xtc_io_reg_fd(xtc_io_t *io, int fd, uint32_t interest, void *tag)
{
	int rc;
	if (io == NULL || fd < 0 || interest == 0)
		return XTC_E_INVAL;
	if (__find_slot(io, fd) >= 0)
		return XTC_E_INVAL;        /* duplicate */
	if (io->n >= io->cap) {
		if ((rc = __grow(io)) != XTC_OK) return rc;
	}
	io->pfds[io->n].fd = fd;
	io->pfds[io->n].events = __interest_to_events(interest);
	io->pfds[io->n].revents = 0;
	io->tags[io->n] = tag;
	io->n++;
	return XTC_OK;
}

/*
 * PUBLIC: int xtc_io_mod_fd __P((xtc_io_t *, int, uint32_t, void *));
 */
int
xtc_io_mod_fd(xtc_io_t *io, int fd, uint32_t interest, void *tag)
{
	int slot;
	if (io == NULL || fd < 0 || interest == 0)
		return XTC_E_INVAL;
	if ((slot = __find_slot(io, fd)) < 0)
		return XTC_E_INVAL;
	if (slot == WAKEUP_SLOT)
		return XTC_E_INVAL;        /* never modify the wakeup slot */
	io->pfds[slot].events = __interest_to_events(interest);
	io->tags[slot] = tag;
	return XTC_OK;
}

/*
 * PUBLIC: int xtc_io_del_fd __P((xtc_io_t *, int));
 */
int
xtc_io_del_fd(xtc_io_t *io, int fd)
{
	int slot;
	if (io == NULL || fd < 0)
		return XTC_E_INVAL;
	if ((slot = __find_slot(io, fd)) < 0)
		return XTC_E_INVAL;
	if (slot == WAKEUP_SLOT)
		return XTC_E_INVAL;
	/* swap with last and shrink */
	io->n--;
	if (slot != io->n) {
		io->pfds[slot] = io->pfds[io->n];
		io->tags[slot] = io->tags[io->n];
	}
	return XTC_OK;
}

/*
 * PUBLIC: int xtc_io_poll __P((xtc_io_t *, xtc_io_event_t *, int, int64_t, int *));
 */
int
xtc_io_poll(xtc_io_t *io, xtc_io_event_t *events, int max,
            int64_t timeout_ns, int *n_out)
{
	int n, i, out_idx;
	int timeout_ms;

	if (io == NULL || events == NULL || max <= 0 || n_out == NULL)
		return XTC_E_INVAL;
	*n_out = 0;

	if (timeout_ns < 0)        timeout_ms = -1;
	else if (timeout_ns == 0)  timeout_ms = 0;
	else                       timeout_ms = (int)((timeout_ns + 999999) / 1000000);

	for (;;) {
		n = poll(io->pfds, (nfds_t)io->n, timeout_ms);
		if (n >= 0) break;
		if (errno == EINTR) continue;
		return XTC_E_INTERNAL;
	}
	if (n == 0) return XTC_OK;

	out_idx = 0;

	/* Wakeup first if it fired. */
	if (io->pfds[WAKEUP_SLOT].revents != 0) {
		int rc = __xtc_io_drain_wakeup(io);
		if (rc != XTC_OK) return rc;
		if (out_idx < max) {
			events[out_idx].tag = NULL;
			events[out_idx].flags = XTC_IO_WAKEUP;
			out_idx++;
		}
		io->pfds[WAKEUP_SLOT].revents = 0;
	}

	/* User fds. */
	for (i = 1; i < io->n && out_idx < max; i++) {
		if (io->pfds[i].revents == 0) continue;
		events[out_idx].tag   = io->tags[i];
		events[out_idx].flags = __revents_to_flags(io->pfds[i].revents);
		io->pfds[i].revents = 0;
		out_idx++;
	}

	*n_out = out_idx;
	return XTC_OK;
}

#endif /* XTC_IO_BACKEND_POLL */

typedef int __xtc_io_poll_unused;   /* avoid -Wpedantic empty-TU when not selected */
