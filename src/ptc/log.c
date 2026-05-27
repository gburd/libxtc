/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/ptc/log.c
 *	Predictable async logger implementation.
 *
 *	Internal model:
 *	  - Fixed-size ring of fixed-size records.
 *	  - Each record carries: level + monotonic timestamp +
 *	    null-terminated formatted text (truncated to record_max).
 *	  - Producer side: claim slot via atomic FAA on tail; if the
 *	    tail catches the head, drop the OLDEST record by bumping
 *	    head past it (dropped += 1).
 *	  - Consumer side (xtc_log_drain): atomic load head; while
 *	    head < tail, write[head] -> sink, head++.
 *
 *	The dropped-oldest-on-full policy is the predictable choice:
 *	logging a hot loop never blocks the loop; the cost is that
 *	some records are lost when the consumer can't keep up.  An
 *	alternative would be drop-newest, but losing recent history
 *	at a crash is worse than losing old history.
 */

#include "xtc_int.h"
#include "xtc_log.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define XTC_LOG_HDR \
	xtc_log_level_t level; \
	int64_t         ts_ns; \
	int             len

struct log_record_hdr {
	XTC_LOG_HDR;
};

struct xtc_log {
	xtc_log_opts_t   opts;

	/* Ring storage: opts.ring_size records of (header + record_max
	 * bytes of text).  Total bytes = ring_size *
	 * (sizeof(hdr) + record_max). */
	uint8_t         *ring;
	size_t           record_stride;

	_Atomic uint64_t head;          /* next slot to drain */
	_Atomic uint64_t tail;          /* next slot to write */
	_Atomic int      dropped;
	pthread_mutex_t  drain_lock;    /* serialise drains across threads */
};

/* Process-wide default logger pointer (atomic). */
static _Atomic uintptr_t __default_logger;

static int64_t
__now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
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

int
xtc_log_create(const xtc_log_opts_t *opts, xtc_log_t **out)
{
	xtc_log_t *log;
	xtc_log_opts_t defaults = XTC_LOG_OPTS_DEFAULT;
	int rc;

	if (out == NULL) return XTC_E_INVAL;
	if (opts == NULL) opts = &defaults;
	if ((rc = __os_calloc(1, sizeof *log, (void **)&log)) != XTC_OK)
		return rc;
	log->opts = *opts;
	if (log->opts.ring_size  <= 0) log->opts.ring_size  = 4096;
	if (log->opts.record_max <= 0) log->opts.record_max = 256;
	log->record_stride = sizeof(struct log_record_hdr) +
	    (size_t)log->opts.record_max;

	if ((rc = __os_calloc((size_t)log->opts.ring_size, log->record_stride,
	    (void **)&log->ring)) != XTC_OK) {
		__os_free(log);
		return rc;
	}
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
	uint8_t *slot;
	struct log_record_hdr *hdr;
	char    *body;
	int      n;

	if (log == NULL || fmt == NULL) return;
	if ((int)lvl < (int)log->opts.floor) return;

	/* Claim a slot via fetch-add on tail. */
	pos = atomic_fetch_add_explicit(&log->tail, 1, memory_order_relaxed);

	/* If we're more than ring_size ahead of head, drop the oldest. */
	{
		uint64_t head = atomic_load_explicit(&log->head,
		    memory_order_acquire);
		if (pos - head >= (uint64_t)log->opts.ring_size) {
			uint64_t new_head = pos - (uint64_t)log->opts.ring_size + 1;
			(void)atomic_compare_exchange_strong_explicit(&log->head,
			    &head, new_head,
			    memory_order_acq_rel, memory_order_acquire);
			atomic_fetch_add_explicit(&log->dropped, 1,
			    memory_order_relaxed);
		}
	}

	slot = log->ring + (pos % (uint64_t)log->opts.ring_size) * log->record_stride;
	hdr  = (struct log_record_hdr *)slot;
	body = (char *)(slot + sizeof *hdr);

	hdr->level = lvl;
	hdr->ts_ns = __now_ns();
	n = vsnprintf(body, (size_t)log->opts.record_max, fmt, ap);
	if (n < 0) n = 0;
	if (n >= log->opts.record_max) n = log->opts.record_max - 1;
	hdr->len = n;
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
	int n_drained = 0;
	if (log == NULL) return 0;
	(void)pthread_mutex_lock(&log->drain_lock);
	for (;;) {
		uint64_t head = atomic_load_explicit(&log->head,
		    memory_order_acquire);
		uint64_t tail = atomic_load_explicit(&log->tail,
		    memory_order_acquire);
		uint8_t *slot;
		struct log_record_hdr *hdr;
		char    *body;
		char     line[1024];
		int      line_len;

		if (head >= tail) break;

		slot = log->ring + (head % (uint64_t)log->opts.ring_size) *
		    log->record_stride;
		hdr  = (struct log_record_hdr *)slot;
		body = (char *)(slot + sizeof *hdr);

		line_len = snprintf(line, sizeof line,
		    "[%lld.%09lld] %s %.*s\n",
		    (long long)(hdr->ts_ns / 1000000000LL),
		    (long long)(hdr->ts_ns % 1000000000LL),
		    __lvl_name(hdr->level),
		    hdr->len, body);
		if (line_len < 0) line_len = 0;
		if (line_len > (int)sizeof line) line_len = (int)sizeof line;

		if (log->opts.sink != NULL)
			(void)log->opts.sink(log->opts.sink_user, hdr->level,
			    line, (size_t)line_len);
		else if (log->opts.sink_fd >= 0)
			(void)write(log->opts.sink_fd, line, (size_t)line_len);   /* XTC_BLOCKING_OK: log writer thread is dedicated */

		(void)atomic_compare_exchange_strong_explicit(&log->head,
		    &head, head + 1,
		    memory_order_acq_rel, memory_order_acquire);
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
