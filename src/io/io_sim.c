/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/io/io_sim.c
 *	Deterministic-simulation (DST) I/O backend.  See docs/M_DST.md.
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
#include "xtc_sim.h"

#include <string.h>
#include <unistd.h>

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

/* ---- wakeup (in-process flag) ---- */

int
__xtc_io_sim_wakeup_post(xtc_io_t *io)
{
	if (io == NULL || io->sim == NULL)
		return XTC_E_INVAL;
	io->sim->wakeup = 1;
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
			/* A scripted AIO completion: finish the parked op. */
			e->aio->done = 1;
			events[idx].tag = e->aio->tag;
			events[idx].flags = XTC_IO_AIO;
		} else {
			events[idx].tag = e->tag;
			events[idx].flags = e->revents;
		}
		idx++;
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
	if (io == NULL || a == NULL)
		return XTC_E_INVAL;
	/*
	 * Phase 2: complete the op INLINE against the real fd (a sim run
	 * still operates on real files), so a single fiber doing aio works
	 * in a sim build without the scheduler.  The deterministic
	 * scheduler (a later phase) will instead enqueue a completion at
	 * now + seeded-latency so the completion ORDER is part of the
	 * replayable schedule.  Inline completion keeps the same observable
	 * contract: the byte count is set and the op is marked done.
	 */
	{
		ssize_t n = 0;
		switch (a->op) {
		case XTC_AIO_PREAD:
			n = pread(a->fd, a->buf, a->len, (off_t)a->off);  /* XTC_BLOCKING_OK: sim inline file op */
			a->res = n < 0 ? -1 : (int)n;
			break;
		case XTC_AIO_PWRITE:
			n = pwrite(a->fd, a->buf, a->len, (off_t)a->off); /* XTC_BLOCKING_OK: sim inline file op */
			a->res = n < 0 ? -1 : (int)n;
			break;
		case XTC_AIO_FSYNC:
			a->res = fsync(a->fd) == 0 ? 0 : -1;
			break;
		case XTC_AIO_FDATASYNC:
#if defined(__APPLE__)
			a->res = fsync(a->fd) == 0 ? 0 : -1;
#else
			a->res = fdatasync(a->fd) == 0 ? 0 : -1;
#endif
			break;
		default:
			return XTC_E_INVAL;
		}
	}
	a->done = 1;
	return XTC_OK;
}

#endif /* XTC_IO_BACKEND_SIM */
