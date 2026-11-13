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
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* One ring slot; record_max bytes of text follow it. */
struct log_slot {
	_Atomic uint64_t seq; /* see the internal model above */
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

	_Atomic uint64_t head; /* next record to drain or drop */
	_Atomic uint64_t tail; /* next position to claim */
	_Atomic int dropped;
	pthread_mutex_t  drain_lock;    /* serialise drains across threads */
};

/* Process-wide default logger pointer (atomic). */
static _Atomic uintptr_t __default_logger;

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

	if (atomic_load_explicit(&log->head, memory_order_relaxed) != h ||
	    atomic_load_explicit(&s->seq, memory_order_acquire) != h + 1 ||
	    !atomic_compare_exchange_strong_explicit(&log->head, &h, h + 1,
	        memory_order_relaxed, memory_order_relaxed))
		return 0;
	atomic_store_explicit(&s->seq, h + log->n_slots, memory_order_release);
	atomic_fetch_add_explicit(&log->dropped, 1, memory_order_relaxed);
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

	pos = atomic_load_explicit(&log->tail, memory_order_relaxed);
	for (;;) {
		s = __log_slot(log, pos);
		seq = atomic_load_explicit(&s->seq, memory_order_acquire);
		if (seq == pos) {
			if (atomic_compare_exchange_weak_explicit(&log->tail,
			        &pos, pos + 1, memory_order_relaxed,
			        memory_order_relaxed))
				break;
		} else if (seq > pos) {
			pos = atomic_load_explicit(&log->tail,
			    memory_order_relaxed);
		} else if (!__log_drop_oldest(log, pos)) {
			/* The tail moved (retry), the slot came free (retry),
			 * or the oldest record is mid-write or mid-read:
			 * drop this record rather than wait for a thread. */
			was = pos;
			pos = atomic_load_explicit(&log->tail,
			    memory_order_relaxed);
			seq =
			    atomic_load_explicit(&s->seq, memory_order_acquire);
			if (pos == was && seq < pos) {
				atomic_fetch_add_explicit(&log->dropped, 1,
				    memory_order_relaxed);
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
		h = atomic_load_explicit(&log->head, memory_order_relaxed);
		s = __log_slot(log, h);
		seq = atomic_load_explicit(&s->seq, memory_order_acquire);
		if (seq < h + 1)
			return NULL; /* empty, or record h is not written */
		if (seq == h + 1 &&
		    atomic_compare_exchange_strong_explicit(&log->head, &h,
		        h + 1, memory_order_relaxed, memory_order_relaxed))
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
	xtc_log_opts_t defaults = XTC_LOG_OPTS_DEFAULT, o = defaults;
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
		atomic_store_explicit(&__log_slot(log, i)->seq, i,
		    memory_order_relaxed);
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
	atomic_store_explicit(&__default_logger, (uintptr_t)log,
	    memory_order_release);
	return XTC_OK;
}

xtc_log_t *
xtc_log_default(void)
{
	return (xtc_log_t *)atomic_load_explicit(&__default_logger,
	    memory_order_acquire);
}

void
xtc_log_vwrite(xtc_log_t *log, xtc_log_level_t lvl,
               const char *fmt, va_list ap)
{
	uint64_t pos;
	struct log_slot *s;
	char text[1024];
	int64_t ts;
	int      n;

	if (log == NULL || fmt == NULL) return;
	if ((int)lvl < (int)log->opts.floor) return;

	/* Format before claiming a slot, so no caller code (the format's
	 * arguments) runs while a slot is claimed and unpublished: the
	 * drainer stops at such a slot.  The drain prints at most a
	 * 1024-byte line, so a longer record was never printed whole. */
	ts = __log_now_ns();
	n = vsnprintf(text, sizeof text, fmt, ap);
	if (n < 0) n = 0;
	if (n >= (int)sizeof text)
		n = (int)sizeof text - 1;
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
	atomic_store_explicit(&s->seq, pos + 1, memory_order_release);
	__xtc_unsafe_leave();
}

void
xtc_log_write(xtc_log_t *log, xtc_log_level_t lvl, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	xtc_log_vwrite(log, lvl, fmt, ap);
	va_end(ap);
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
		atomic_store_explicit(&s->seq, h + log->n_slots,
		    memory_order_release);
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
	return atomic_load_explicit(&((xtc_log_t *)log)->dropped,
	    memory_order_relaxed);
}
