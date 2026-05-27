/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/io/io_aix.c
 *	AIX pollset backend.
 *
 *	AIX's pollset_* family (pollset_create, pollset_ctl, pollset_poll,
 *	pollset_destroy, pollset_query) is a registered-fd model similar
 *	to epoll: persistent registration, one descriptor poll, edge or
 *	level triggered.  Unlike illumos event ports, registrations are
 *	NOT one-shot -- once added, an fd stays registered until
 *	explicitly removed via pollset_ctl(PS_DELETE).
 *
 *	xtc_io contract mapping:
 *	  reg_fd  -> pollset_ctl(PS_ADD,    fd, events)
 *	  mod_fd  -> pollset_ctl(PS_MOD,    fd, events)   (fall back to del+add)
 *	  del_fd  -> pollset_ctl(PS_DELETE, fd)
 *	  poll    -> pollset_poll(timeout)
 *
 *	The wakeup pipe is a regular fd registered with POLLIN.  A
 *	side-table of registered fds (mirroring kqueue/solaris) gives
 *	us duplicate-detection for the M2 contract.
 *
 *	NOTE: This file builds on AIX (xlc or gcc-aix) but has not yet
 *	run on a live AIX host.  See docs/M_AIX_KVM.md for the plan to
 *	obtain AIX testing infrastructure.  Structural correctness was
 *	reviewed against IBM's pollset man pages.
 */

#include "xtc_int.h"

#if defined(XTC_IO_BACKEND_AIX)

#include "io_int.h"

#include <errno.h>
#include <unistd.h>
#include <sys/poll.h>
#include <sys/pollset.h>

extern int __xtc_io_drain_wakeup(xtc_io_t *io);

/* Side-table tracking -- same shape as illumos and kqueue. */
struct __xtc_aix_reg {
	int       fd;
	uint32_t  interest;
	void     *tag;
};

static int
__find_reg(xtc_io_t *io, int fd)
{
	int i;
	for (i = 0; i < io->n_reg; i++)
		if (((struct __xtc_aix_reg *)io->reg_aix)[i].fd == fd) return i;
	return -1;
}

static int
__add_reg(xtc_io_t *io, int fd, uint32_t interest, void *tag)
{
	struct __xtc_aix_reg *t;
	if (io->n_reg >= io->cap_reg) {
		int new_cap = io->cap_reg == 0 ? 16 : io->cap_reg * 2;
		void *p = NULL;
		int rc = __os_realloc(io->reg_aix,
		    sizeof(struct __xtc_aix_reg) * (size_t)new_cap, &p);
		if (rc != XTC_OK) return rc;
		io->reg_aix = p;
		io->cap_reg = new_cap;
	}
	t = io->reg_aix;
	t[io->n_reg].fd       = fd;
	t[io->n_reg].interest = interest;
	t[io->n_reg].tag      = tag;
	io->n_reg++;
	return XTC_OK;
}

static void
__del_reg(xtc_io_t *io, int fd)
{
	struct __xtc_aix_reg *t = io->reg_aix;
	int idx = __find_reg(io, fd);
	if (idx < 0) return;
	io->n_reg--;
	if (idx != io->n_reg) t[idx] = t[io->n_reg];
}

static short
__pollev_for(uint32_t interest)
{
	short ev = 0;
	if (interest & XTC_IO_READABLE) ev |= POLLIN;
	if (interest & XTC_IO_WRITABLE) ev |= POLLOUT;
	return ev;
}

int
__xtc_io_backend_init(xtc_io_t *io)
{
	io->ps = pollset_create(-1);     /* -1 = no max-fd hint */
	if (io->ps < 0) return XTC_E_INTERNAL;
	io->reg_aix = NULL;
	io->n_reg = io->cap_reg = 0;
	return XTC_OK;
}

void
__xtc_io_backend_fini(xtc_io_t *io)
{
	if (io->ps >= 0) (void)pollset_destroy(io->ps);
	io->ps = -1;
	__os_free(io->reg_aix);
	io->reg_aix = NULL;
	io->n_reg = io->cap_reg = 0;
}

static int
__ctl_add(xtc_io_t *io, int fd, uint32_t interest, void *tag)
{
	struct poll_ctl c;
	c.cmd = PS_ADD;
	c.events = __pollev_for(interest);
	c.fd = fd;
	if (pollset_ctl(io->ps, &c, 1) != 0) return XTC_E_INTERNAL;
	(void)tag;   /* AIX pollset doesn't carry udata; we map fd->tag in side-table */
	return XTC_OK;
}

static int
__ctl_del(xtc_io_t *io, int fd)
{
	struct poll_ctl c;
	c.cmd = PS_DELETE;
	c.events = 0;
	c.fd = fd;
	(void)pollset_ctl(io->ps, &c, 1);
	return XTC_OK;
}

int
__xtc_io_register_wakeup(xtc_io_t *io, int fd)
{
	int rc;
	if ((rc = __add_reg(io, fd, XTC_IO_READABLE, io)) != XTC_OK) return rc;
	if ((rc = __ctl_add(io, fd, XTC_IO_READABLE, io)) != XTC_OK) {
		__del_reg(io, fd);
		return rc;
	}
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
	if ((rc = __ctl_add(io, fd, interest, tag)) != XTC_OK) {
		__del_reg(io, fd);
		return rc;
	}
	return XTC_OK;
}

int
xtc_io_mod_fd(xtc_io_t *io, int fd, uint32_t interest, void *tag)
{
	int idx;
	struct __xtc_aix_reg *t;
	if (io == NULL || fd < 0 || interest == 0) return XTC_E_INVAL;
	idx = __find_reg(io, fd);
	if (idx < 0) return XTC_E_INVAL;
	t = io->reg_aix;
	/* AIX pollset_ctl PS_MOD changes events on an existing entry. */
	{
		struct poll_ctl c;
		c.cmd = PS_MOD;
		c.events = __pollev_for(interest);
		c.fd = fd;
		if (pollset_ctl(io->ps, &c, 1) != 0) {
			/* Fallback: PS_DELETE + PS_ADD. */
			(void)__ctl_del(io, fd);
			return __ctl_add(io, fd, interest, tag);
		}
	}
	t[idx].interest = interest;
	t[idx].tag      = tag;
	return XTC_OK;
}

int
xtc_io_del_fd(xtc_io_t *io, int fd)
{
	if (io == NULL || fd < 0) return XTC_E_INVAL;
	if (__find_reg(io, fd) < 0) return XTC_E_INVAL;
	(void)__ctl_del(io, fd);
	__del_reg(io, fd);
	return XTC_OK;
}

int
xtc_io_poll(xtc_io_t *io, xtc_io_event_t *events, int max,
            int64_t timeout_ns, int *n_out)
{
	struct pollfd evs[64];
	int batch, got, i, out_idx;
	int timeout_ms;

	if (io == NULL || events == NULL || max <= 0 || n_out == NULL)
		return XTC_E_INVAL;
	*n_out = 0;

	if (timeout_ns < 0)        timeout_ms = -1;
	else if (timeout_ns == 0)  timeout_ms = 0;
	else                       timeout_ms = (int)(timeout_ns / 1000000LL);

	batch = max < (int)(sizeof evs / sizeof evs[0])
	    ? max : (int)(sizeof evs / sizeof evs[0]);

	for (;;) {
		got = pollset_poll(io->ps, evs, batch, timeout_ms);
		if (got >= 0) break;
		if (errno == EINTR) continue;
		return XTC_E_INTERNAL;
	}
	if (got == 0) return XTC_OK;

	out_idx = 0;
	for (i = 0; i < got && out_idx < max; i++) {
		uint32_t flags = 0;
		int idx;
		struct __xtc_aix_reg *t;
		if (evs[i].revents & POLLIN)  flags |= XTC_IO_READABLE;
		if (evs[i].revents & POLLOUT) flags |= XTC_IO_WRITABLE;
		if (evs[i].revents & POLLHUP) flags |= XTC_IO_HUP;
		if (evs[i].revents & POLLERR) flags |= XTC_IO_ERR;
		idx = __find_reg(io, evs[i].fd);
		if (idx < 0) continue;
		t = io->reg_aix;
		if (t[idx].tag == io) {
			(void)__xtc_io_drain_wakeup(io);
			events[out_idx].tag = NULL;
			events[out_idx].flags = XTC_IO_WAKEUP;
		} else {
			events[out_idx].tag = t[idx].tag;
			events[out_idx].flags = flags;
		}
		out_idx++;
	}
	*n_out = out_idx;
	return XTC_OK;
}

#endif /* XTC_IO_BACKEND_AIX */

typedef int __xtc_io_aix_unused;
