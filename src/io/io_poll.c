/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/io/io_poll.c
 *	The poll(2) backend.  Portable to every Tier 1 platform; the
 *	floor we promise (PLAN.md (S)3.6).  Maintains a parallel
 *	(pollfd[], tag[]) so the public API can return user tags even
 *	though poll(2) itself does not store user data per fd.
 */

#define _POSIX_C_SOURCE 200809L

#include "xtc_int.h"   /* pulls xtc_config.h -- defines XTC_IO_BACKEND_* */

#if defined(XTC_IO_BACKEND_POLL)

#include "io_int.h"

#include <errno.h>
#include <poll.h>
#include <unistd.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>

/* Wakeup pipe is always at slot 0; user fds start at 1. */
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

void __xtc_io_backend_fini(xtc_io_t *io);

int
__xtc_io_backend_init(xtc_io_t *io)
{
	int rc;
	io->pfds = NULL;
	io->tags = NULL;
	io->n = 0;
	io->cap = 0;
	io->pending_del = NULL; io->n_pending_del = io->cap_pending_del = 0;
	atomic_store_explicit(&io->has_pending_del, 0, memory_order_relaxed);
	atomic_store_explicit(&io->owner_set, 0, memory_order_relaxed);
	if (pthread_mutex_init(&io->del_lock, NULL) != 0)
		return XTC_E_INTERNAL;
	/* __grow reallocs two arrays; if the second fails the first is
	 * already ours, and the caller (xtc_io_init) does not call
	 * backend_fini on an init failure -- release it here (found by the
	 * OOM-injection sweep on the poll backend: 1 allocation leaked). */
	if ((rc = __grow(io)) != XTC_OK)
		__xtc_io_backend_fini(io);
	return rc;
}

void
__xtc_io_backend_fini(xtc_io_t *io)
{
	__os_free(io->pfds);
	__os_free(io->tags);
	io->pfds = NULL;
	io->tags = NULL;
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
	__apply_pending_del_for(io, fd);
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

/* Non-io_uring backends: reg/del are kernel-synchronized (epoll) or
 * this backend keeps its own registry; the cross-loop deferred-
 * unregister that io_uring needs is a no-op passthrough here (the
 * caller is xtc_proc_wait_fd cleanup after a migration).  Provided so
 * the single caller links on every backend. */
/* No-op: only io_uring has a single-owner fds list + SQ ring that needs
 * an eagerly-recorded owner thread; this backend is kernel-synchronized
 * or keeps its own registry.  Provided so every backend links. */


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

	__drain_pending_del(io);
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
			events[out_idx].fd = -1;
			out_idx++;
		}
		io->pfds[WAKEUP_SLOT].revents = 0;
	}

	/* User fds. */
	for (i = 1; i < io->n && out_idx < max; i++) {
		if (io->pfds[i].revents == 0) continue;
		events[out_idx].tag   = io->tags[i];
		events[out_idx].flags = __revents_to_flags(io->pfds[i].revents);
		events[out_idx].fd    = io->pfds[i].fd;
		io->pfds[i].revents = 0;
		out_idx++;
	}

	*n_out = out_idx;
	return XTC_OK;
}

#endif /* XTC_IO_BACKEND_POLL */

typedef int __xtc_io_poll_unused;   /* avoid -Wpedantic empty-TU when not selected */
