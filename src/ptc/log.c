/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/ptc/log.c
 *	Predictable async logger implementation.
 *
 *	Internal model:
 *	  - Fixed-size ring of fixed-size slots.  Each slot carries a
 *	    sequence word, the level, a monotonic timestamp and up to
 *	    record_max bytes of formatted text.
 *	  - The sequence word is D. Vyukov's bounded-queue protocol: slot
 *	    i starts at seq = i.  A writer claims position pos only when
 *	    its slot reads seq == pos (free for this lap), by a CAS of tail
 *	    from pos to pos + 1, fills the slot, and publishes it with a
 *	    release store of seq = pos + 1.  A consumer takes position h
 *	    only when its slot reads seq == h + 1 (published), by a CAS of
 *	    head from h to h + 1, and gives the slot back for the next lap
 *	    with seq = h + ring_size.  So the drainer never reads a slot a
 *	    writer has claimed but not filled: it stops there, and the
 *	    records behind it wait for the next drain, in order.
 *	  - Full ring: the writer consumes the oldest record itself (the
 *	    same CAS of head the drainer uses) without printing it, counts
 *	    it in `dropped`, and claims the freed slot.  If the oldest
 *	    record is at that instant being written or read, the writer
 *	    drops its own record instead (also counted), so a writer never
 *	    spins on another thread.
 *
 *	The dropped-oldest-on-full policy is the predictable choice:
 *	logging a hot loop never blocks the loop; the cost is that
 *	some records are lost when the consumer can't keep up.  An
 *	alternative would be drop-newest, but losing recent history
 *	at a crash is worse than losing old history.
 */

#include "xtc_int.h"
#include "xtc_log.h"
#include "preempt_int.h" /* __xtc_unsafe_enter/leave */

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* One ring slot; record_max bytes of text follow it. */
struct log_slot {
	uint64_t seq; /* see the internal model above */
	xtc_log_level_t level;
	int len;
	int64_t ts_ns;
};

struct xtc_log {
	xtc_log_opts_t   opts;

	/* Ring storage: opts.ring_size slots of record_stride bytes (the
	 * slot header plus record_max bytes of text, rounded up so every
	 * slot's seq word is aligned). */
	uint8_t         *ring;
	size_t           record_stride;
	uint64_t n_slots;

	uint64_t head; /* next record to drain or drop */
	uint64_t tail; /* next position to claim */
	int dropped;
	pthread_mutex_t  drain_lock;    /* serialise drains across threads */
};

/* Process-wide default logger pointer (atomic). */
static uintptr_t __default_logger;

static int64_t
__log_now_ns(void)
{
	int64_t ns = 0;
	(void)__os_clock_mono(&ns);
	return ns;
}

static const char *
__lvl_name(xtc_log_level_t l)
{
	switch (l) {
	case XTC_LOG_TRACE: return "TRACE";
	case XTC_LOG_DEBUG: return "DEBUG";
	case XTC_LOG_INFO:  return "INFO ";
	case XTC_LOG_WARN:  return "WARN ";
	case XTC_LOG_ERROR: return "ERROR";
	case XTC_LOG_FATAL: return "FATAL";
	}
	return "?????";
}

static struct log_slot *
__log_slot(const xtc_log_t *log, uint64_t pos)
{
	return (struct log_slot *)(void *)(log->ring +
	    (size_t)(pos % log->n_slots) * log->record_stride);
}

/*
 * The ring is full: position pos needs the slot that still holds record
 * pos - n_slots, the oldest.  Consume that record without printing it
 * and count it, so the caller can claim the slot.  Returns 0, having
 * done nothing, when the oldest record is still being written (its seq
 * is not published) or already being read (head moved past it), so the
 * caller drops its own record instead of waiting.
 */
static int
__log_drop_oldest(xtc_log_t *log, uint64_t pos)
{
	uint64_t h = pos - log->n_slots;
	struct log_slot *s = __log_slot(log, h);

	if (xtc_atomic_load_u64(&log->head, xtc_mo_relaxed) != h ||
	    xtc_atomic_load_u64(&s->seq, xtc_mo_acquire) != h + 1 ||
	    !xtc_atomic_cas_u64(&log->head, &h, h + 1, xtc_mo_relaxed,
	        xtc_mo_relaxed))
		return 0;
	xtc_atomic_store_u64(&s->seq, h + log->n_slots, xtc_mo_release);
	xtc_atomic_fetch_add_i32(&log->dropped, 1, xtc_mo_relaxed);
	return 1;
}

/*
 * Claim the next ring position for a record.  Returns its slot, with the
 * position in *out_pos, or NULL when the record is dropped (counted):
 * the ring is full and its oldest record is mid-write or mid-read.  The
 * caller fills the slot and publishes it with seq = *out_pos + 1.
 */
static struct log_slot *
__log_claim(xtc_log_t *log, uint64_t *out_pos)
{
	uint64_t pos, seq, was;
	struct log_slot *s;

	pos = xtc_atomic_load_u64(&log->tail, xtc_mo_relaxed);
	for (;;) {
		s = __log_slot(log, pos);
		seq = xtc_atomic_load_u64(&s->seq, xtc_mo_acquire);
		if (seq == pos) {
			if (xtc_atomic_cas_weak_u64(&log->tail, &pos, pos + 1,
			        xtc_mo_relaxed, xtc_mo_relaxed))
				break;
		} else if (seq > pos) {
			pos = xtc_atomic_load_u64(&log->tail, xtc_mo_relaxed);
		} else if (!__log_drop_oldest(log, pos)) {
			/* The tail moved (retry), the slot came free (retry),
			 * or the oldest record is mid-write or mid-read:
			 * drop this record rather than wait for a thread. */
			was = pos;
			pos = xtc_atomic_load_u64(&log->tail, xtc_mo_relaxed);
			seq = xtc_atomic_load_u64(&s->seq, xtc_mo_acquire);
			if (pos == was && seq < pos) {
				xtc_atomic_fetch_add_i32(&log->dropped, 1,
				    xtc_mo_relaxed);
				return NULL;
			}
		}
	}
	*out_pos = pos;
	return s;
}

/*
 * Take the oldest published record for the drainer (which holds
 * drain_lock).  Returns its slot, with its position in *out_h, or NULL
 * when the ring is empty or its oldest record is not written yet.  The
 * caller reads the slot and gives it back with seq = *out_h + n_slots.
 */
static struct log_slot *
__log_take(xtc_log_t *log, uint64_t *out_h)
{
	uint64_t h, seq;
	struct log_slot *s;

	for (;;) {
		h = xtc_atomic_load_u64(&log->head, xtc_mo_relaxed);
		s = __log_slot(log, h);
		seq = xtc_atomic_load_u64(&s->seq, xtc_mo_acquire);
		if (seq < h + 1)
			return NULL; /* empty, or record h is not written */
		if (seq == h + 1 &&
		    xtc_atomic_cas_u64(&log->head, &h, h + 1, xtc_mo_relaxed,
		        xtc_mo_relaxed))
			break;
		/* A full-ring writer dropped record h first: look again. */
	}
	*out_h = h;
	return s;
}

int
xtc_log_create(const xtc_log_opts_t *opts, xtc_log_t **out)
{
	xtc_log_t *log;
	xtc_log_opts_t defaults = XTC_LOG_OPTS_INIT, o = defaults;
	uint64_t i;
	int rc;

	if (out == NULL) return XTC_E_INVAL;
	if (opts != NULL &&
	    (rc = __xtc_sized_in(&o, &defaults, sizeof o, opts, 0)) != XTC_OK)
		return rc;
	if ((rc = __os_calloc(1, sizeof *log, (void **)&log)) != XTC_OK)
		return rc;
	log->opts = o;
	if (log->opts.ring_size  <= 0) log->opts.ring_size  = 4096;
	/* One slot cannot tell "published" (seq = pos + 1) from "free for
	 * the next lap" (seq = pos + ring_size). */
	if (log->opts.ring_size == 1)
		log->opts.ring_size = 2;
	if (log->opts.record_max <= 0) log->opts.record_max = 256;
	log->record_stride =
	    sizeof(struct log_slot) + (size_t)log->opts.record_max;
	log->record_stride =
	    (log->record_stride + _Alignof(struct log_slot) - 1) &
	    ~(size_t)(_Alignof(struct log_slot) - 1);
	log->n_slots = (uint64_t)log->opts.ring_size;

	if ((rc = __os_calloc((size_t)log->opts.ring_size, log->record_stride,
	    (void **)&log->ring)) != XTC_OK) {
		__os_free(log);
		return rc;
	}
	for (i = 0; i < log->n_slots; i++)
		xtc_atomic_store_u64(&__log_slot(log, i)->seq, i,
		    xtc_mo_relaxed);
	(void)pthread_mutex_init(&log->drain_lock, NULL);
	*out = log;
	return XTC_OK;
}

void
xtc_log_destroy(xtc_log_t *log)
{
	if (log == NULL) return;
	(void)xtc_log_drain(log);
	(void)pthread_mutex_destroy(&log->drain_lock);
	__os_free(log->ring);
	__os_free(log);
}

int
xtc_log_set_floor(xtc_log_t *log, xtc_log_level_t lvl)
{
	if (log == NULL) return XTC_E_INVAL;
	log->opts.floor = lvl;
	return XTC_OK;
}

int
xtc_log_set_default(xtc_log_t *log)
{
	xtc_atomic_store_uptr(&__default_logger, (uintptr_t)log,
	    xtc_mo_release);
	return XTC_OK;
}

int
xtc_log_swap_default(xtc_log_t *log, xtc_log_t **out_prev)
{
	if (out_prev == NULL)
		return XTC_E_INVAL;
	*out_prev = (xtc_log_t *)xtc_atomic_exchange_uptr(&__default_logger,
	    (uintptr_t)log, xtc_mo_acq_rel);
	return XTC_OK;
}

xtc_log_t *
xtc_log_default(void)
{
	return (
	    xtc_log_t *)xtc_atomic_load_uptr(&__default_logger, xtc_mo_acquire);
}

/*
 * Append a formatted record of n bytes to the ring: the one record path
 * of xtc_log_vwrite and xtc_logf_impl, which format first, so no caller
 * code (the format's arguments) runs while a slot is claimed and
 * unpublished (the drainer stops at such a slot).
 */
static void
__log_append(xtc_log_t *log, xtc_log_level_t lvl, int64_t ts, const char *text,
    int n)
{
	uint64_t pos;
	struct log_slot *s;

	if (n >= log->opts.record_max) n = log->opts.record_max - 1;

	/* Claim to publish is a memcpy; keep the timer from preempting a
	 * fiber inside it. */
	__xtc_unsafe_enter();
	if ((s = __log_claim(log, &pos)) == NULL) {
		__xtc_unsafe_leave();
		return;
	}
	s->level = lvl;
	s->ts_ns = ts;
	s->len = n;
	memcpy(s + 1, text, (size_t)n);
	xtc_atomic_store_u64(&s->seq, pos + 1, xtc_mo_release);
	__xtc_unsafe_leave();
}

void
xtc_log_vwrite(xtc_log_t *log, xtc_log_level_t lvl, const char *fmt, va_list ap)
{
	char text[1024];
	int64_t ts;
	int n;

	if (log == NULL || fmt == NULL)
		return;
	if ((int)lvl < (int)log->opts.floor)
		return;

	/* The drain prints at most a 1024-byte line, so a longer record
	 * was never printed whole. */
	ts = __log_now_ns();
	n = vsnprintf(text, sizeof text, fmt, ap);
	if (n < 0)
		n = 0;
	if (n >= (int)sizeof text)
		n = (int)sizeof text - 1;
	__log_append(log, lvl, ts, text, n);
}

void
xtc_log_write(xtc_log_t *log, xtc_log_level_t lvl, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	xtc_log_vwrite(log, lvl, fmt, ap);
	va_end(ap);
}

/* The record text an xtc_logf call is formatted into. */
struct logf_out {
	char *buf;
	size_t cap; /* bytes of text buf holds; one more for the NUL */
	size_t len; /* bytes written, at most cap */
};

/* One placeholder's spec: "{}" or "{:[0][width][.prec][x|X|o|p]}". */
struct logf_spec {
	int zero;  /* '0' flag: zero-fill a number to the width */
	int width; /* 0: none */
	int prec;  /* -1: none */
	char conv; /* 0 (by type), 'x', 'X', 'o' or 'p' */
};

/* Append n bytes of s to o, as many as fit.  o NULL: count only. */
static void
__logf_put(struct logf_out *o, const char *s, size_t n)
{
	if (o == NULL)
		return;
	if (n > o->cap - o->len)
		n = o->cap - o->len;
	memcpy(o->buf + o->len, s, n);
	o->len += n;
}

/* Append a printf-formatted piece to o, truncated to what fits. */
static void
__logf_printf(struct logf_out *o, const char *fmt, ...) XTC_PRINTF_FMT(2, 3);

static void
__logf_printf(struct logf_out *o, const char *fmt, ...)
{
	va_list ap;
	int n;

	va_start(ap, fmt);
	n = vsnprintf(o->buf + o->len, o->cap - o->len + 1, fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if ((size_t)n > o->cap - o->len)
		n = (int)(o->cap - o->len);
	o->len += (size_t)n;
}

/* Read a decimal width or precision at *pp (at least one digit). */
static int
__logf_num(const char **pp)
{
	const char *p = *pp;
	int v = 0;

	/* Capped at the longest record text, so it cannot overflow. */
	for (; (unsigned)(*p - '0') <= 9; p++) {
		v = v * 10 + (*p - '0');
		if (v > 1023)
			v = 1023;
	}
	*pp = p;
	return v;
}

/*
 * f points at a '{'.  Returns the length of the placeholder there, with
 * its spec in *sp, or 0 when the brace does not start one (it then prints
 * as itself).
 */
static int
__logf_spec(const char *f, struct logf_spec *sp)
{
	const char *p = f + 1;

	sp->zero = 0;
	sp->width = 0;
	sp->prec = -1;
	sp->conv = 0;
	if (*p == '}')
		return 2;
	if (*p++ != ':')
		return 0;
	if (*p == '0') {
		sp->zero = 1;
		p++;
	}
	if ((unsigned)(*p - '0') <= 9)
		sp->width = __logf_num(&p);
	if (*p == '.') {
		p++;
		if ((unsigned)(*p - '0') > 9)
			return 0;
		sp->prec = __logf_num(&p);
	}
	if (*p == 'x' || *p == 'X' || *p == 'o' || *p == 'p')
		sp->conv = *p++;
	if (*p != '}')
		return 0;
	return (int)(p + 1 - f);
}

/* An unsigned value in the spec's base, width and fill. */
static void
__logf_uint(struct logf_out *o, const struct logf_spec *sp,
    unsigned long long u)
{
	switch (sp->conv) {
	case 'x':
		__logf_printf(o, sp->zero ? "%0*llx" : "%*llx", sp->width, u);
		break;
	case 'X':
		__logf_printf(o, sp->zero ? "%0*llX" : "%*llX", sp->width, u);
		break;
	case 'o':
		__logf_printf(o, sp->zero ? "%0*llo" : "%*llo", sp->width, u);
		break;
	default:
		__logf_printf(o, sp->zero ? "%0*llu" : "%*llu", sp->width, u);
		break;
	}
}

/* A signed value; hex and octal show its two's complement in its width. */
static void
__logf_int(struct logf_out *o, const struct logf_spec *sp,
    const xtc_logarg_t *a)
{
	unsigned long long u = (unsigned long long)a->v.i;

	if (sp->conv == 'x' || sp->conv == 'X' || sp->conv == 'o') {
		if (a->size > 0 && (size_t)a->size < sizeof u)
			u &= (1ULL << (8 * a->size)) - 1;
		__logf_uint(o, sp, u);
		return;
	}
	__logf_printf(o, sp->zero ? "%0*lld" : "%*lld", sp->width, a->v.i);
}

/* An address as 0x and hex, the same on every platform. */
static void
__logf_ptr(struct logf_out *o, const struct logf_spec *sp, const void *p)
{
	char tmp[2 + 2 * sizeof(unsigned long long) + 1];

	(void)snprintf(tmp, sizeof tmp, "0x%llx",
	    (unsigned long long)(uintptr_t)p);
	__logf_printf(o, "%*s", sp->width, tmp);
}

/* Format one argument by its kind and the placeholder's spec.  A spec
 * letter that does not apply to the kind is ignored. */
static void
__logf_arg(struct logf_out *o, const struct logf_spec *sp,
    const xtc_logarg_t *a)
{
	char tmp[48];
	const char *s;

	switch (a->kind) {
	case XTC_LOGARG_INT:
		__logf_int(o, sp, a);
		break;
	case XTC_LOGARG_UINT:
		__logf_uint(o, sp, a->v.u);
		break;
	case XTC_LOGARG_DOUBLE:
		if (sp->prec >= 0)
			__logf_printf(o, sp->zero ? "%0*.*f" : "%*.*f",
			    sp->width, sp->prec, a->v.d);
		else
			__logf_printf(o, sp->zero ? "%0*g" : "%*g", sp->width,
			    a->v.d);
		break;
	case XTC_LOGARG_STR:
		if (sp->conv == 'p') {
			__logf_ptr(o, sp, a->v.s);
			break;
		}
		s = a->v.s != NULL ? a->v.s : "(null)";
		if (sp->prec >= 0)
			__logf_printf(o, "%*.*s", sp->width, sp->prec, s);
		else
			__logf_printf(o, "%*s", sp->width, s);
		break;
	case XTC_LOGARG_PTR:
		__logf_ptr(o, sp, a->v.p);
		break;
	case XTC_LOGARG_PID:
		(void)snprintf(tmp, sizeof tmp, "<%u.%u.%llu>",
		    (unsigned)a->v.pid.loop_id, (unsigned)a->v.pid.local_id,
		    (unsigned long long)a->v.pid.gen);
		__logf_printf(o, "%*s", sp->width, tmp);
		break;
	default:
		__logf_put(o, "{?}", 3);
		break;
	}
}

/*
 * Walk fmt, formatting placeholder k with args[k] into o (o NULL: only
 * count).  A placeholder past the last argument prints as written.
 * Returns the number of placeholders.
 */
static int
__logf_render(struct logf_out *o, const char *f, int nargs,
    const xtc_logarg_t *args)
{
	struct logf_spec sp;
	size_t n;
	int k, nph = 0;

	for (;;) {
		n = strcspn(f, "{}");
		__logf_put(o, f, n);
		f += n;
		if (*f == '\0')
			break;
		if (f[1] == f[0]) {
			/* {{ or }}: one literal brace */
			__logf_put(o, f, 1);
			f += 2;
		} else if (*f == '{' && (k = __logf_spec(f, &sp)) > 0) {
			if (o != NULL && nph < nargs)
				__logf_arg(o, &sp, &args[nph]);
			else
				__logf_put(o, f, (size_t)k);
			nph++;
			f += k;
		} else {
			__logf_put(o, f, 1);
			f++;
		}
	}
	return nph;
}

void
xtc_logf_impl(xtc_log_t *log, xtc_log_level_t lvl, const char *fmt, int nargs,
    const xtc_logarg_t *args)
{
	char text[1024];
	struct logf_out o;
	int64_t ts;
	int nph;

	if (log == NULL || fmt == NULL)
		return;
	if ((int)lvl < (int)log->opts.floor)
		return;
	if (nargs < 0 || args == NULL)
		nargs = 0;

	/* Count the placeholders first, so a mismatch is marked at the front
	 * of the record, where truncation to record_max cannot cut it. */
	ts = __log_now_ns();
	o.buf = text;
	o.cap = sizeof text - 1;
	o.len = 0;
	nph = __logf_render(NULL, fmt, nargs, args);
	if (nph != nargs)
		__logf_printf(&o, "[xtc_logf: %d arg%s, %d {}] ", nargs,
		    nargs == 1 ? "" : "s", nph);
	(void)__logf_render(&o, fmt, nargs, args);
	__log_append(log, lvl, ts, text, (int)o.len);
}

int
xtc_log_drain(xtc_log_t *log)
{
	struct log_slot *s;
	xtc_log_level_t lvl;
	uint64_t h;
	char line[1024];
	int line_len, n_drained = 0;

	if (log == NULL) return 0;
	(void)pthread_mutex_lock(&log->drain_lock);
	for (;;) {
		/* Format out of the slot and give the slot back, then call
		 * the sink with no slot held. */
		__xtc_unsafe_enter();
		if ((s = __log_take(log, &h)) == NULL) {
			__xtc_unsafe_leave();
			break;
		}
		lvl = s->level;
		line_len =
		    snprintf(line, sizeof line, "[%lld.%09lld] %s %.*s\n",
		        (long long)(s->ts_ns / 1000000000LL),
		        (long long)(s->ts_ns % 1000000000LL), __lvl_name(lvl),
		        s->len, (const char *)(s + 1));
		xtc_atomic_store_u64(&s->seq, h + log->n_slots, xtc_mo_release);
		__xtc_unsafe_leave();
		if (line_len < 0) line_len = 0;
		if (line_len >=
		    (int)sizeof line) { /* truncated: keep the newline */
			line_len = (int)sizeof line - 1;
			line[line_len - 1] = '\n';
		}

		if (log->opts.sink != NULL)
			(void)log->opts.sink(log->opts.sink_user, lvl, line,
			    (size_t)line_len);
		else if (log->opts.sink_fd >= 0) {
			ssize_t wr = write(log->opts.sink_fd, line, (size_t)line_len);   /* XTC_BLOCKING_OK: log writer thread is dedicated */
			(void)wr;   /* best-effort log sink; a short write is tolerated */
		}
		n_drained++;
	}
	(void)pthread_mutex_unlock(&log->drain_lock);
	return n_drained;
}

int
xtc_log_drop_count(const xtc_log_t *log)
{
	if (log == NULL) return 0;
	return xtc_atomic_load_i32(&((xtc_log_t *)log)->dropped,
	    xtc_mo_relaxed);
}
