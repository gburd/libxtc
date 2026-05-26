/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the PostgreSQL License.
 *
 * src/inc/xtc_log.h
 *	Predictable, async-safe structured logger.
 *
 *	Design goals:
 *	  - Predictable: no malloc on the hot path; preallocated ring
 *	    buffer per logger; bounded latency.
 *	  - Cooperative: log calls don't block the loop; a background
 *	    consumer drains the ring and writes to the sink.
 *	  - Structured: severity + message + key/value pairs.
 *	  - Lossy under pressure: when the ring is full we drop the
 *	    oldest record and bump a counter so callers know about it.
 *	    Better than blocking the loop.
 *	  - Thread-safe: writers and the consumer can be on different
 *	    OS threads; ring uses lock-free MPSC enqueue.
 *
 *	Sinks supported in v1:
 *	  - stderr/stdout (default)
 *	  - file descriptor (preopened by caller)
 *	  - custom callback (user-supplied function)
 *
 *	Levels follow Postgres / syslog: TRACE < DEBUG < INFO < WARN
 *	< ERROR < FATAL.  At configure-time the floor is set; calls
 *	below the floor are eliminated by the macro to a no-op.
 */

#ifndef XTC_LOG_H
#define XTC_LOG_H

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "xtc.h"

typedef enum xtc_log_level {
	XTC_LOG_TRACE = 0,
	XTC_LOG_DEBUG = 1,
	XTC_LOG_INFO  = 2,
	XTC_LOG_WARN  = 3,
	XTC_LOG_ERROR = 4,
	XTC_LOG_FATAL = 5
} xtc_log_level_t;

typedef struct xtc_log xtc_log_t;

/* Sink callback shape.  buf is a complete formatted line including
 * trailing newline.  Return <0 to indicate the sink failed. */
typedef int (*xtc_log_sink_fn)(void *user, xtc_log_level_t lvl,
                               const char *buf, size_t len);

typedef struct xtc_log_opts {
	int              ring_size;        /* records, default 4096 */
	int              record_max;       /* per-record bytes, default 256 */
	xtc_log_level_t  floor;            /* default INFO */
	int              sink_fd;          /* if != -1, write to this fd */
	xtc_log_sink_fn  sink;             /* if non-NULL, takes precedence */
	void            *sink_user;
} xtc_log_opts_t;

#define XTC_LOG_OPTS_DEFAULT { \
	.ring_size  = 4096, \
	.record_max = 256, \
	.floor      = XTC_LOG_INFO, \
	.sink_fd    = 2,                  /* stderr */ \
	.sink       = NULL, \
	.sink_user  = NULL \
}

/*
 * PUBLIC: int  xtc_log_create __P((const xtc_log_opts_t *, xtc_log_t **));
 * PUBLIC: void xtc_log_destroy __P((xtc_log_t *));
 * PUBLIC: int  xtc_log_set_floor __P((xtc_log_t *, xtc_log_level_t));
 * PUBLIC: int  xtc_log_set_default __P((xtc_log_t *));
 * PUBLIC: xtc_log_t *xtc_log_default __P((void));
 *
 * PUBLIC: void xtc_log_write __P((xtc_log_t *, xtc_log_level_t, const char *, ...));
 * PUBLIC: void xtc_log_vwrite __P((xtc_log_t *, xtc_log_level_t, const char *, va_list));
 *
 * PUBLIC: int  xtc_log_drain __P((xtc_log_t *));
 * PUBLIC: int  xtc_log_drop_count __P((const xtc_log_t *));
 */

int  xtc_log_create(const xtc_log_opts_t *opts, xtc_log_t **out);
void xtc_log_destroy(xtc_log_t *log);

int  xtc_log_set_floor(xtc_log_t *log, xtc_log_level_t lvl);

/* Set the process-wide default logger.  Subsequent macro-driven
 * calls (XTC_LOG_INFO, etc.) route here.  Returns the prior default
 * (or NULL if none). */
int  xtc_log_set_default(xtc_log_t *log);
xtc_log_t *xtc_log_default(void);

/* Append a record to the ring.  Non-blocking; if the ring is full
 * the oldest record is dropped and a counter is bumped. */
void xtc_log_write(xtc_log_t *log, xtc_log_level_t lvl,
                   const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
void xtc_log_vwrite(xtc_log_t *log, xtc_log_level_t lvl,
                    const char *fmt, va_list ap);

/* Drain the ring synchronously: read every queued record and write
 * it to the sink.  Returns the number of records drained.  Useful
 * for tests; in production a dedicated consumer thread/proc would
 * call this periodically. */
int  xtc_log_drain(xtc_log_t *log);

/* Number of records dropped because the ring was full. */
int  xtc_log_drop_count(const xtc_log_t *log);

/* Convenience macros: pass through the default logger.  Calls below
 * the default-logger's floor are eliminated cheaply via a runtime
 * check (the default logger pointer is loaded once). */
#define XTC_LOG(lvl, ...) \
	xtc_log_write(xtc_log_default(), (lvl), __VA_ARGS__)
#define XTC_LOG_TRACE_F(...) XTC_LOG(XTC_LOG_TRACE, __VA_ARGS__)
#define XTC_LOG_DEBUG_F(...) XTC_LOG(XTC_LOG_DEBUG, __VA_ARGS__)
#define XTC_LOG_INFO_F(...)  XTC_LOG(XTC_LOG_INFO,  __VA_ARGS__)
#define XTC_LOG_WARN_F(...)  XTC_LOG(XTC_LOG_WARN,  __VA_ARGS__)
#define XTC_LOG_ERROR_F(...) XTC_LOG(XTC_LOG_ERROR, __VA_ARGS__)
#define XTC_LOG_FATAL_F(...) XTC_LOG(XTC_LOG_FATAL, __VA_ARGS__)

#endif /* XTC_LOG_H */
