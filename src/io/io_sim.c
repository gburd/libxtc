/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/io/io_sim.c
 *	Deterministic-simulation (DST) I/O backend.
 *
 *	There is no kernel poller: readiness for registered fds and file-
 *	AIO completions come from a scripted, in-process event store the
 *	test/scheduler drives, and the cross-thread wakeup is an in-process
 *	flag rather than a self-pipe or PostQueuedCompletionStatus.  This
 *	makes every I/O outcome a function of the seed/schedule, hence
 *	reproducible under replay.
 *
 *	Phase 2 (this file) provides the backend surface and inline file-
 *	AIO completion so single-fiber aio works in sim; the deterministic
 *	scheduler (a later phase) drives readiness events against the
 *	virtual clock.
 */

#include "xtc_int.h"

#if defined(XTC_IO_BACKEND_SIM)

#include "io_int.h"
#include "xtc_aio.h"
#include <sys/uio.h>
#include "xtc_sim.h"

#include <string.h>
#include <unistd.h>
#include <errno.h>

/* Per-fd registration (tag map) for readiness simulation. */
struct __xtc_sim_reg {
	int       fd;
	uint32_t  interest;
	void     *tag;
};

/* A scripted, pending simulated event due at a virtual-time stamp.
 * Readiness on a registered fd or a file-AIO completion. */
struct __xtc_sim_ev {
	int64_t   due_ns;     /* virtual time at which it becomes ready */
	int       fd;         /* readiness fd, or -1 for an AIO completion */
	uint32_t  revents;    /* readiness flags (XTC_IO_*) */
	void     *tag;        /* event tag */
	xtc_aio_t *aio;       /* non-NULL for an AIO completion */
	int       aio_res;    /* the result to publish on the aio at due time */
	/* Generic deferred callback (non-NULL for a deferred cross-loop
	 * message delivery driven by xtc_sim_net_latency): invoked when the
	 * event becomes due instead of publishing readiness / an AIO. */
	void    (*cb)(void *);
	void     *cb_arg;
	struct __xtc_sim_ev *next;
};

struct __xtc_sim_io {
	struct __xtc_sim_reg *regs;
	int                   n_reg;
	int                   cap_reg;
	struct __xtc_sim_ev  *events;     /* due-ordered pending events */
	int                   wakeup;     /* in-process wakeup flag */
};

/* ---- lifecycle ---- */

/* PUBLIC: int __xtc_io_backend_init __P((xtc_io_t *)); */
int
__xtc_io_backend_init(xtc_io_t *io)
{
	struct __xtc_sim_io *s;
	if (__os_calloc(1, sizeof *s, (void **)&s) != XTC_OK)
		return XTC_E_NOMEM;
	io->sim = s;
	return XTC_OK;
}

/* PUBLIC: void __xtc_io_backend_fini __P((xtc_io_t *)); */
void
__xtc_io_backend_fini(xtc_io_t *io)
{
	struct __xtc_sim_io *s = io->sim;
	struct __xtc_sim_ev *e, *n;
	if (s == NULL)
		return;
	for (e = s->events; e != NULL; e = n) { n = e->next; __os_free(e); }
	if (s->regs != NULL) __os_free(s->regs);
	__os_free(s);
	io->sim = NULL;
}

/* ---- fd registration ---- */

static struct __xtc_sim_reg *
sim_find(struct __xtc_sim_io *s, int fd)
{
	int i;
	for (i = 0; i < s->n_reg; i++)
		if (s->regs[i].fd == fd)
			return &s->regs[i];
	return NULL;
}

/* PUBLIC: int xtc_io_reg_fd __P((xtc_io_t *, int, uint32_t, void *)); */
int
xtc_io_reg_fd(xtc_io_t *io, int fd, uint32_t interest, void *tag)
{
	struct __xtc_sim_io *s;
	if (io == NULL || io->sim == NULL || fd < 0)
		return XTC_E_INVAL;
	s = io->sim;
	if (sim_find(s, fd) != NULL)
		return XTC_E_INVAL;            /* duplicate */
	if (s->n_reg == s->cap_reg) {
		int nc = s->cap_reg ? s->cap_reg * 2 : 16;
		void *p = NULL;
		if (__os_realloc(s->regs, sizeof(*s->regs) * (size_t)nc, &p)
		    != XTC_OK)
			return XTC_E_NOMEM;
		s->regs = p;
		s->cap_reg = nc;
	}
	s->regs[s->n_reg].fd = fd;
	s->regs[s->n_reg].interest = interest;
	s->regs[s->n_reg].tag = tag;
	s->n_reg++;
	return XTC_OK;
}

/* PUBLIC: int xtc_io_mod_fd __P((xtc_io_t *, int, uint32_t, void *)); */
int
xtc_io_mod_fd(xtc_io_t *io, int fd, uint32_t interest, void *tag)
{
	struct __xtc_sim_reg *r;
	if (io == NULL || io->sim == NULL)
		return XTC_E_INVAL;
	r = sim_find(io->sim, fd);
	if (r == NULL)
		return XTC_E_NOTFOUND;
	r->interest = interest;
	r->tag = tag;
	return XTC_OK;
}

/* PUBLIC: int xtc_io_del_fd __P((xtc_io_t *, int)); */
int
xtc_io_del_fd(xtc_io_t *io, int fd)
{
	struct __xtc_sim_io *s;
	int i;
	if (io == NULL || io->sim == NULL)
		return XTC_E_INVAL;
	s = io->sim;
	for (i = 0; i < s->n_reg; i++) {
		if (s->regs[i].fd == fd) {
			s->regs[i] = s->regs[--s->n_reg];   /* swap-remove */
			return XTC_OK;
		}
	}
	return XTC_E_NOTFOUND;
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

/* ---- wakeup (in-process flag) ---- */

int
__xtc_io_sim_wakeup_post(xtc_io_t *io)
{
	if (io == NULL || io->sim == NULL)
		return XTC_E_INVAL;
	io->sim->wakeup = 1;
	return XTC_OK;
}

/* PUBLIC: int64_t __xtc_io_sim_next_due __P((xtc_io_t *)); */
/*
 * The virtual-time stamp of the earliest pending sim event (readiness
 * or a deferred AIO completion), or -1 if none.  The deterministic
 * scheduler consults this so a loop whose only pending work is an
 * in-flight (deferred) AIO completion is not mistaken for deadlocked:
 * when no loop is otherwise runnable, the scheduler advances the
 * virtual clock to this stamp, making the completion due.  A pending
 * wakeup counts as due-now (0). */
int64_t
__xtc_io_sim_next_due(xtc_io_t *io)
{
	struct __xtc_sim_io *s;
	struct __xtc_sim_ev *e;
	int64_t best = -1;
	if (io == NULL || io->sim == NULL)
		return -1;
	s = io->sim;
	/*
	 * Note: a bare wakeup flag is NOT reported as pending work.  In
	 * production a wakeup breaks a blocking io_poll; the DST sim
	 * scheduler never blocks in poll, and a cross-loop wake also pushes
	 * an XTC_INB_WAKE inbox message (which __sim_loop_runnable already
	 * sees), so the wakeup flag is redundant here.  Counting it would
	 * keep a loop perpetually "runnable" after its work drained -- the
	 * scheduler would never reach quiescence.  Only real pending events
	 * (readiness, deferred AIO completions) count. */
	for (e = s->events; e != NULL; e = e->next)
		if (best < 0 || e->due_ns < best)
			best = e->due_ns;
	return best;
}

/*
 * Insert `ev` into the loop's due-ordered event list.  Normally this is
 * a stable insert at the due-order position.  Under swizzle
 * (xtc_sim_swizzle_enable), on a seeded XTC_SIM_RNG_IO coin the event is
 * placed one slot LATER than due order -- a legal reordering (the waiter
 * simply wakes after a sibling completion it would otherwise have
 * preceded) that explicitly explores completion/message-order
 * interleavings.  Deterministic: the coin is a seeded draw, so replay
 * holds.  A no-op (plain due-order insert) when swizzle is off.
 */
static void
sim_events_insert(struct __xtc_sim_io *s, struct __xtc_sim_ev *ev)
{
	struct __xtc_sim_ev **pp;
	for (pp = &s->events;
	    *pp != NULL && (*pp)->due_ns <= ev->due_ns;
	    pp = &(*pp)->next)
		;
	/* pp now points at the due-order slot.  Swizzle: if there is a
	 * following event, step one past it so `ev` is delivered after a
	 * peer it would otherwise precede. */
	{
		int sw = __xtc_sim_swizzle_pct();
		if (sw > 0 && *pp != NULL &&
		    (int)__xtc_sim_rng_range(XTC_SIM_RNG_IO, 1000) < sw)
			pp = &(*pp)->next;
	}
	ev->next = *pp;
	*pp = ev;
}

/* PUBLIC: int __xtc_io_sim_defer_cb __P((xtc_io_t *, int64_t, void (*)(void *), void *)); */
/*
 * Enqueue a generic callback to run at virtual time `due_ns` on this
 * loop's poll drain (in due order).  Used by the cross-loop message-
 * latency path (proc.c __mbox_deliver under xtc_sim_net_latency): the
 * callback pushes the deferred envelope into the target mailbox and
 * fires the receiver's waker when it becomes due.  Because the event
 * sits on the target loop's event store, __xtc_io_sim_next_due reports
 * it, so the sim scheduler advances the clock to it and never mistakes
 * an in-flight delayed message for a deadlock.
 */
int
__xtc_io_sim_defer_cb(xtc_io_t *io, int64_t due_ns, void (*fn)(void *),
    void *arg)
{
	struct __xtc_sim_io *s;
	struct __xtc_sim_ev *ev;
	if (io == NULL || io->sim == NULL || fn == NULL)
		return XTC_E_INVAL;
	s = io->sim;
	if (__os_calloc(1, sizeof *ev, (void **)&ev) != XTC_OK)
		return XTC_E_NOMEM;
	ev->due_ns = due_ns;
	ev->fd = -1;
	ev->cb = fn;
	ev->cb_arg = arg;
	sim_events_insert(s, ev);
	return XTC_OK;
}

int
__xtc_io_sim_wakeup_drain(xtc_io_t *io)
{
	if (io != NULL && io->sim != NULL)
		io->sim->wakeup = 0;
	return XTC_OK;
}

/* PUBLIC: int __xtc_io_register_wakeup __P((xtc_io_t *, int)); */
int
__xtc_io_register_wakeup(xtc_io_t *io, int fd)
{
	(void)io; (void)fd;
	return XTC_OK;       /* no fd-based wakeup in sim */
}

/* ---- poll ---- */

/* Pop every event due at or before the current virtual time into the
 * caller's array; emit the wakeup if pending.  Never blocks: in sim the
 * scheduler (not poll) owns advancing the clock when nothing is ready. */
/* PUBLIC: int xtc_io_poll __P((xtc_io_t *, xtc_io_event_t *, int, int64_t, int *)); */
int
xtc_io_poll(xtc_io_t *io, xtc_io_event_t *events, int max,
            int64_t timeout_ns, int *n_out)
{
	struct __xtc_sim_io *s;
	int64_t now = 0;
	int idx = 0;
	struct __xtc_sim_ev **pp;

	(void)timeout_ns;   /* sim never blocks on a timeout */
	if (n_out != NULL)
		*n_out = 0;
	if (io == NULL || io->sim == NULL || events == NULL || max <= 0)
		return XTC_E_INVAL;
	s = io->sim;
	(void)__xtc_sim_vclock(&now);

	/* Wakeup first (coalesced, like the other backends). */
	if (s->wakeup && idx < max) {
		s->wakeup = 0;
		events[idx].tag = NULL;
		events[idx].flags = XTC_IO_WAKEUP;
		idx++;
	}

	/* Drain due events in due order. */
	pp = &s->events;
	while (*pp != NULL && idx < max) {
		struct __xtc_sim_ev *e = *pp;
		if (e->due_ns > now) {
			pp = &e->next;        /* not yet due; keep scanning */
			continue;
		}
		if (e->aio != NULL) {
			/* A deferred AIO completion: publish the (possibly
			 * fault-injected) result computed at submit time and
			 * wake the parked op. */
			e->aio->res = e->aio_res;
			e->aio->done = 1;
			events[idx].tag = e->aio->tag;
			events[idx].flags = XTC_IO_AIO;
			idx++;
		} else if (e->cb != NULL) {
			/* A deferred cross-loop message delivery (net latency):
			 * run the callback now (it pushes the envelope into the
			 * target mailbox and fires the recv waker).  It does not
			 * occupy a caller event slot -- the waker it fires enqueues
			 * the parked receiver directly. */
			void (*cb)(void *) = e->cb;
			void *cb_arg = e->cb_arg;
			*pp = e->next;
			__os_free(e);
			cb(cb_arg);
			continue;
		} else {
			events[idx].tag = e->tag;
			events[idx].flags = e->revents;
			idx++;
		}
		*pp = e->next;
		__os_free(e);
	}

	if (n_out != NULL)
		*n_out = idx;
	return XTC_OK;
}

/* ---- file AIO ---- */

/* PUBLIC: int xtc_io_aio_submit __P((xtc_io_t *, xtc_aio_t *)); */
int
xtc_io_aio_submit(xtc_io_t *io, xtc_aio_t *a)
{
	ssize_t n = 0;
	int res;

	if (io == NULL || a == NULL)
		return XTC_E_INVAL;

	/*
	 * Perform the real I/O against the real fd now (a sim run still
	 * operates on real files), computing the result.  Then either
	 * complete INLINE (the default -- single-fiber sim AIO works with
	 * no scheduler), or, when sim I/O faults are enabled, DEFER the
	 * completion to now + a seeded latency and optionally fault the
	 * result -- so the completion ORDER across concurrent ops becomes
	 * part of the replayable schedule and the fiber genuinely parks.
	 */
	switch (a->op) {
	case XTC_AIO_PREAD:
		n = pread(a->fd, a->buf, a->len, (off_t)a->off);  /* XTC_BLOCKING_OK: sim inline file op */
		res = n < 0 ? -1 : (int)n;
		/* Stale read: on a seeded coin, replace the just-read bytes
		 * with a PRIOR durable version of this exact (fd,off) -- the
		 * bytes are well-formed but out of date, so recovery/cache
		 * code that skips a version/LSN check accepts stale data. */
		if (res > 0)
			(void)__xtc_sim_io_stale_read(a->fd, a->off, a->buf, res);
		/* Corrupt read: silently bit-flip one returned byte.  The
		 * bytes on disk are fine; the caller receives one bad byte a
		 * checksum must catch -- FDB's corrupt-read fault class. */
		if (res > 0) {
			int fb = __xtc_sim_io_flip_byte(res);
			if (fb >= 0)
				((unsigned char *)a->buf)[fb] ^= 0x40;
		}
		break;
	case XTC_AIO_PWRITE: {
		/* Disk-full: on a seeded coin the whole write fails ENOSPC
		 * (nothing persists), before the torn-write model -- a hard
		 * error the caller must degrade on, not a silent short tail. */
		if (__xtc_sim_io_enospc()) {
			res = -1;
			errno = ENOSPC;
			break;
		}
		/* Stale-data model: before overwriting, snapshot the CURRENT
		 * (about-to-be-superseded) bytes at this offset into the ring,
		 * so a later seeded stale read can return this now-prior
		 * version.  Cheap no-op unless stale injection is armed. */
		{
			unsigned char old[4096];
			int want = (int)a->len < (int)sizeof old ?
			    (int)a->len : (int)sizeof old;
			ssize_t on = pread(a->fd, old, (size_t)want,
			    (off_t)a->off);  /* XTC_BLOCKING_OK: sim snapshot */
			if (on > 0)
				__xtc_sim_io_stale_record(a->fd, a->off, old,
				    (int)on);
		}
		/* Torn write: persist only a seeded prefix of the buffer but
		 * still report FULL success -- exactly a torn page.  Untorn
		 * (the default) writes the whole buffer.  Reporting a->len
		 * keeps the caller believing the write succeeded, leaving the
		 * missing tail bytes as whatever was on disk (latent bad data
		 * a checksum on read-back must catch). */
		int persist = __xtc_sim_io_torn_prefix((int)a->len);
		n = pwrite(a->fd, a->buf, (size_t)persist, (off_t)a->off); /* XTC_BLOCKING_OK: sim inline file op */
		if (n > 0)
			__xtc_sim_io_wb_wrote(a->fd, a->off + (uint64_t)n);
		if (n < 0)
			res = -1;
		else if (persist < (int)a->len)
			res = (int)a->len;   /* torn: report full despite short persist */
		else
			res = (int)n;
		break;
	}
	case XTC_AIO_FSYNC:
		res = fsync(a->fd) == 0 ? 0 : -1;
		if (res == 0) __xtc_sim_io_wb_synced(a->fd);
		break;
	case XTC_AIO_FDATASYNC:
#if defined(__APPLE__)
		res = fsync(a->fd) == 0 ? 0 : -1;
#else
		res = fdatasync(a->fd) == 0 ? 0 : -1;
#endif
		if (res == 0) __xtc_sim_io_wb_synced(a->fd);
		break;
	case XTC_AIO_PREADV:
		n = preadv(a->fd, (const struct iovec *)a->iov, a->iovcnt,
		    (off_t)a->off);  /* XTC_BLOCKING_OK: sim inline file op */
		res = n < 0 ? -1 : (int)n;
		break;
	case XTC_AIO_PWRITEV:
		n = pwritev(a->fd, (const struct iovec *)a->iov, a->iovcnt,
		    (off_t)a->off); /* XTC_BLOCKING_OK: sim inline file op */
		res = n < 0 ? -1 : (int)n;
		break;
	default:
		return XTC_E_INVAL;
	}

	/*
	 * Seeded fault (only when I/O faults are enabled).  A read/write
	 * that transferred >1 byte may be reported SHORT (a partial
	 * transfer the caller must handle by re-issuing the remainder).
	 * fsync/fdatasync may be reported as an error.  Real bytes already
	 * moved; the fault is in what we REPORT, which is exactly how a
	 * short read or an EIO presents to a caller.
	 */
	if (res > 0 && (a->op == XTC_AIO_PREAD || a->op == XTC_AIO_PWRITE) &&
	    __xtc_sim_io_should_fault()) {
		res = 1 + (int)__xtc_sim_rng_range(XTC_SIM_RNG_IO, (uint64_t)res);
	} else if (res == 0 && (a->op == XTC_AIO_FSYNC ||
	    a->op == XTC_AIO_FDATASYNC) && __xtc_sim_io_should_fault()) {
		res = -1;   /* fsync/fdatasync EIO */
	}

	if (__xtc_sim_io_faults_active() && a->tag != NULL && io->sim != NULL) {
		/* Defer: enqueue a completion at now + seeded latency, in due
		 * order, and leave a->done == 0 so the fiber parks.  The drain
		 * path publishes a->res = aio_res and wakes a->tag. */
		struct __xtc_sim_io *s = io->sim;
		struct __xtc_sim_ev *ev;
		int64_t now = 0;
		if (__os_calloc(1, sizeof *ev, (void **)&ev) == XTC_OK) {
			int64_t extra = 0;
			(void)__xtc_sim_vclock(&now);
			/* Buggify: under DST, DEFER this AIO completion an
			 * EXTRA tick beyond its seeded latency.  A later
			 * completion is always legal (the fiber is parked
			 * awaiting it and simply wakes later), so this widens
			 * the completion-order interleaving deterministically
			 * -- FDB's "slow disk" pessimal path.  Per-call
			 * BUGGIFY-stream coin, so it never perturbs unrelated
			 * tests.  A no-op in production. */
			if (XTC_SIM_BUGGIFY("io.aio.slow_completion") &&
			    xtc_sim_buggify_fault(250))
				extra = 5000;   /* +5us */
			ev->due_ns = now + __xtc_sim_io_latency() + extra;
			ev->fd = -1;
			ev->aio = a;
			ev->aio_res = res;
			sim_events_insert(s, ev);
			return XTC_OK;
		}
		/* calloc failed -- fall through to inline completion. */
	}

	a->res = res;
	a->done = 1;
	return XTC_OK;
}

#endif /* XTC_IO_BACKEND_SIM */
