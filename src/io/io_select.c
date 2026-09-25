/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * src/io/io_select.c
 *	The select(2) backend.  The most portable IO multiplexor --
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
#include <fcntl.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

#define WAKEUP_SLOT 0

/* ---- cross-loop deferred unregister (see io_int.h) ---------------- */

int xtc_io_del_fd(xtc_io_t *io, int fd);

static int
__is_owner(const xtc_io_t *io)
{
	return !atomic_load_explicit(&io->owner_set, memory_order_acquire) ||
	    pthread_equal(pthread_self(), io->owner_tid);
}

/* Owner thread: perform every queued unregister.  Called at the top of
 * xtc_io_poll, before the registry is read. */
static void
__drain_pending_del(xtc_io_t *io)
{
	int i, n, local[32];
	if (!atomic_load_explicit(&io->has_pending_del, memory_order_acquire))
		return;
	for (;;) {
		(void)pthread_mutex_lock(&io->del_lock);
		n = io->n_pending_del;
		if (n > (int)(sizeof local / sizeof local[0]))
			n = (int)(sizeof local / sizeof local[0]);
		for (i = 0; i < n; i++)
			local[i] = io->pending_del[i];
		for (i = n; i < io->n_pending_del; i++)
			io->pending_del[i - n] = io->pending_del[i];
		io->n_pending_del -= n;
		if (io->n_pending_del == 0)
			atomic_store_explicit(&io->has_pending_del, 0,
			    memory_order_relaxed);
		(void)pthread_mutex_unlock(&io->del_lock);
		for (i = 0; i < n; i++)
			(void)xtc_io_del_fd(io, local[i]);
		if (n < (int)(sizeof local / sizeof local[0]))
			return;
	}
}

/* Owner thread, before (re-)registering fd: a deferred delete of the same
 * fd still queued would otherwise make the add fail as a duplicate (the
 * io_uring lesson, 8bf7b45).  Apply it now. */
static void
__apply_pending_del_for(xtc_io_t *io, int fd)
{
	int i, found = 0;
	if (!atomic_load_explicit(&io->has_pending_del, memory_order_acquire))
		return;
	(void)pthread_mutex_lock(&io->del_lock);
	for (i = 0; i < io->n_pending_del; i++) {
		if (io->pending_del[i] != fd)
			continue;
		found = 1;
		io->pending_del[i] = io->pending_del[--io->n_pending_del];
		i--;
	}
	if (io->n_pending_del == 0)
		atomic_store_explicit(&io->has_pending_del, 0,
		    memory_order_relaxed);
	(void)pthread_mutex_unlock(&io->del_lock);
	if (found)
		(void)xtc_io_del_fd(io, fd);
}

void
__xtc_io_set_owner(xtc_io_t *io)
{
	if (io != NULL &&
	    !atomic_load_explicit(&io->owner_set, memory_order_relaxed)) {
		io->owner_tid = pthread_self();
		atomic_store_explicit(&io->owner_set, 1, memory_order_release);
	}
}

/* Unregister fd on io from ANY thread: inline on the owner, queued (and
 * the owner nudged) otherwise.  Before, this was a passthrough that
 * mutated the registry from the wrong thread. */
int
__xtc_io_defer_del_fd(xtc_io_t *io, int fd)
{
	int *np;
	if (io == NULL || fd < 0) return XTC_E_INVAL;
	if (__is_owner(io))
		return xtc_io_del_fd(io, fd);
	(void)pthread_mutex_lock(&io->del_lock);
	if (io->n_pending_del == io->cap_pending_del) {
		int nc = io->cap_pending_del ? io->cap_pending_del * 2 : 16;
		if (__os_realloc(io->pending_del, (size_t)nc * sizeof(int),
		    (void **)&np) != XTC_OK) {
			(void)pthread_mutex_unlock(&io->del_lock);
			return XTC_E_NOMEM;
		}
		io->pending_del = np;
		io->cap_pending_del = nc;
	}
	io->pending_del[io->n_pending_del++] = fd;
	atomic_store_explicit(&io->has_pending_del, 1, memory_order_release);
	(void)pthread_mutex_unlock(&io->del_lock);
	(void)xtc_io_wakeup(io);
	return XTC_OK;
}

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

void __xtc_io_backend_fini(xtc_io_t *io);

int
__xtc_io_backend_init(xtc_io_t *io)
{
	int rc;
	io->fds = NULL; io->interests = NULL; io->tags = NULL;
	io->n = 0; io->cap = 0;
	io->pending_del = NULL; io->n_pending_del = io->cap_pending_del = 0;
	atomic_store_explicit(&io->has_pending_del, 0, memory_order_relaxed);
	atomic_store_explicit(&io->owner_set, 0, memory_order_relaxed);
	if (pthread_mutex_init(&io->del_lock, NULL) != 0)
		return XTC_E_INTERNAL;
	/* Release a partially grown set: xtc_io_init does not call
	 * backend_fini when init fails (see io_poll.c). */
	if ((rc = __grow(io)) != XTC_OK)
		__xtc_io_backend_fini(io);
	return rc;
}

void
__xtc_io_backend_fini(xtc_io_t *io)
{
	__os_free(io->fds); io->fds = NULL;
	__os_free(io->interests); io->interests = NULL;
	__os_free(io->tags); io->tags = NULL;
	io->n = io->cap = 0;
	__os_free(io->pending_del); io->pending_del = NULL;
	io->n_pending_del = io->cap_pending_del = 0;
	(void)pthread_mutex_destroy(&io->del_lock);
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
	__apply_pending_del_for(io, fd);
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

/* Non-io_uring backends: reg/del are kernel-synchronized (epoll) or
 * this backend keeps its own registry; the cross-loop deferred-
 * unregister that io_uring needs is a no-op passthrough here (the
 * caller is xtc_proc_wait_fd cleanup after a migration).  Provided so
 * the single caller links on every backend. */
/* No-op: only io_uring has a single-owner fds list + SQ ring that needs
 * an eagerly-recorded owner thread; this backend is kernel-synchronized
 * or keeps its own registry.  Provided so every backend links. */


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
	__drain_pending_del(io);

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
		if (errno == EBADF) {
			/*
			 * One registered fd was closed while still registered.
			 * poll(2) reports that per fd (POLLNVAL); select(2)
			 * fails the WHOLE call, which used to take the loop's
			 * worker down.  Find the dead fd(s), drop them from the
			 * registry, and report each to its owner as XTC_IO_ERR,
			 * exactly as the poll backend's POLLNVAL does.
			 */
			for (i = io->n - 1; i > WAKEUP_SLOT &&
			    *out_n < max_events; i--) {
				int fd = io->fds[i];
				if (fcntl(fd, F_GETFD) != -1 || errno != EBADF) /* XTC_BLOCKING_OK: fd validity probe */
					continue;
				out_events[*out_n].flags = XTC_IO_ERR;
				out_events[*out_n].tag   = io->tags[i];
				out_events[*out_n].fd    = fd;
				(*out_n)++;
				(void)xtc_io_del_fd(io, fd);
			}
			return XTC_OK;
		}
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
			out_events[*out_n].fd    = -1;
			(*out_n)++;
			continue;
		}
		out_events[*out_n].flags = f;
		out_events[*out_n].tag = io->tags[i];
		out_events[*out_n].fd = io->fds[i];
		(*out_n)++;
	}
	return XTC_OK;
}

#endif /* XTC_IO_BACKEND_SELECT */
