/*-
 * Copyright (c) 2026, The XTC Project
 * @stability stable
 * Use of this source code is governed by the ISC License.
 *
 * src/inc/xtc_log.h
 *	Predictable, async-safe structured logger.
 *
 *	Design goals:
 *	  - Predictable: no malloc on the hot path; preallocated ring
 *	    buffer per logger; bounded latency.
 *	  - Cooperative: log calls don't block the loop; a background
 *	    consumer drains the ring and writes to the sink.
 *	  - A level and a message per record, printf-formatted
 *	    (xtc_log_write) or typed (xtc_logf, below).
 *	  - Lossy under pressure: when the ring is full the oldest record
 *	    is dropped (or, if it is being written or read at that moment,
 *	    the new one) and a counter is bumped so callers know about it.
 *	    Better than blocking the loop.
 *	  - Thread-safe: writers on any threads and one drain at a time;
 *	    the enqueue is lock-free.  A drain hands the sink only whole
 *	    records, each writer's in the order it wrote them, and stops
 *	    at a record that is still being written.
 *
 *	Sinks supported:
 *	  - stderr/stdout (default)
 *	  - file descriptor (preopened by caller)
 *	  - custom callback (user-supplied function)
 *
 *	Levels follow the usual syslog ordering: TRACE < DEBUG < INFO < WARN
 *	< ERROR < FATAL.  A record below the logger's floor returns at once,
 *	before formatting (a runtime check; the macros do not compile calls
 *	out).
 */

#ifndef XTC_LOG_H
#define XTC_LOG_H

#include "xtc_export.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

#include "xtc.h"

#ifdef __cplusplus
extern "C" {
#endif

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
	size_t           size;             /* sizeof(xtc_log_opts_t) */
	int              ring_size;        /* records, default 4096 */
	int              record_max;       /* per-record bytes, default 256 */
	xtc_log_level_t  floor;            /* default INFO */
	int              sink_fd;          /* if != -1, write to this fd */
	xtc_log_sink_fn  sink;             /* if non-NULL, takes precedence */
	void            *sink_user;
} xtc_log_opts_t;

#define XTC_LOG_OPTS_DEFAULT { \
	.size       = sizeof(xtc_log_opts_t), \
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
 * PUBLIC: int  xtc_log_swap_default __P((xtc_log_t *, xtc_log_t **));
 * PUBLIC: xtc_log_t *xtc_log_default __P((void));
 *
 * PUBLIC: void xtc_log_write __P((xtc_log_t *, xtc_log_level_t, const char *, ...));
 * PUBLIC: void xtc_log_vwrite __P((xtc_log_t *, xtc_log_level_t, const char *, va_list));
 *
 * PUBLIC: int  xtc_log_drain __P((xtc_log_t *));
 * PUBLIC: int  xtc_log_drop_count __P((const xtc_log_t *));
 *
 * PUBLIC: void xtc_logf_impl __P((xtc_log_t *, xtc_log_level_t, const char *, int, const xtc_logarg_t *));
 */

XTC_API int  xtc_log_create(const xtc_log_opts_t *opts, xtc_log_t **out);
XTC_API void xtc_log_destroy(xtc_log_t *log);

XTC_API int  xtc_log_set_floor(xtc_log_t *log, xtc_log_level_t lvl);

/*
 * Set the process-wide default logger (NULL: none).  The XTC_LOG_*_F
 * macros and libxtc's own diagnostics (the tuning advisor, a loop's
 * over-budget warning, a slab redzone report, an executor step error)
 * write to it.  There is no default until one is set, and those
 * messages are dropped until then.  Returns XTC_OK.
 *
 * xtc_log_swap_default does the same and returns the logger it replaced
 * in *out_prev (NULL if none), in one atomic exchange, so two threads
 * swapping cannot lose each other's logger.  NB: a writer that loaded
 * the old default just before the swap may still be inside
 * xtc_log_write on it, so do not destroy a logger right after swapping
 * it out unless no other thread can be logging.
 */
XTC_API int  xtc_log_set_default(xtc_log_t *log);
XTC_API int xtc_log_swap_default(xtc_log_t *log, xtc_log_t **out_prev);
XTC_API xtc_log_t *xtc_log_default(void);

/* Format a record and append it to the ring.  Non-blocking, any
 * thread; a full ring drops a record and bumps the drop counter. */
#if defined(__GNUC__) || defined(__clang__)
#  define XTC_LOG_PRINTF_FMT __attribute__((format(printf, 3, 4)))
#else
#  define XTC_LOG_PRINTF_FMT   /* MSVC: no prototype format attribute */
#endif
XTC_API void xtc_log_write(xtc_log_t *log, xtc_log_level_t lvl,
                           const char *fmt, ...)
    XTC_LOG_PRINTF_FMT;
XTC_API void xtc_log_vwrite(xtc_log_t *log, xtc_log_level_t lvl,
                            const char *fmt, va_list ap);

/* Drain the ring synchronously: write every finished record to the
 * sink, oldest first, stopping at one a writer is still filling (the
 * next drain resumes there).  Returns the number of records drained.
 * A drain with a sink_fd does a blocking write(2): in a server, call it
 * from a plain thread or the blocking pool, not a loop thread. */
XTC_API int  xtc_log_drain(xtc_log_t *log);

/* Number of records dropped because the ring was full.  Records the
 * drain wrote plus this equals records written above the floor. */
XTC_API int  xtc_log_drop_count(const xtc_log_t *log);

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

/*
 * Typed logging (2.2): xtc_logf(lvl, fmt, ...) writes to the default
 * logger, xtc_logf_to(log, lvl, fmt, ...) to log.
 *
 *	xtc_logf(XTC_LOG_INFO, "loop {} drained {} bytes in {} us", id, n, us);
 *
 * Each {} takes the next argument and formats it by the argument's own
 * type, so there is no conversion letter to get wrong and no PRIu64.
 * Every integer type prints as its value (int8_t to uint64_t, size_t,
 * ssize_t, off_t, pid_t, an enum, a char; _Bool as 0 or 1), a floating
 * type as %g, a char * or const char * as the string (NULL as "(null)"),
 * any other object pointer as 0x and its address in hex, and an xtc_pid_t
 * (with xtc_proc.h) as <loop_id.local_id.gen>.  A character literal is an
 * int in C, so 'x' prints 120.  A struct or union does not compile, and a
 * function pointer converts to a data pointer, which ISO C leaves
 * undefined (-Wpedantic warns).  A bit-field no wider than an int prints
 * as its value; gcc gives a wider one a type of its own: cast it.
 *
 * A spec after a colon changes the format: {:x} {:X} {:o} hex and octal
 * (a negative integer in its own width's two's complement), {:p} as an
 * address (a string's too), {:8} a minimum width, {:08x} zero-filled, and
 * {:.3} the decimal places of a double or the most characters taken from
 * a string.  {{ and }} print a brace; a brace that does not start a
 * placeholder prints as itself.
 *
 * At most 16 arguments: a 17th is a compile error.  A record whose
 * placeholders and arguments do not match is still written, prefixed
 * with "[xtc_logf: 2 args, 3 {}] " (at the front, so truncation to
 * record_max cannot cut it off); a placeholder with no argument prints as
 * written.  The level floor, the ring, the sink and the drop count are
 * xtc_log_write's.  Each argument is evaluated once, before the level is
 * checked, as a function's would be.
 *
 * C needs C11 (_Generic; MSVC: /std:c11, Visual Studio 2019 16.8 or
 * later, which also selects the conforming preprocessor the argument
 * count needs).  The macros and xtc_logarg are C only: a C++ translation
 * unit does not see them (xtc_log_write works there).  xtc_logarg(x)
 * converts one value; xtc_logf_impl is the function the macros call with
 * the converted arguments.
 */
enum {
	XTC_LOGARG_INT = 1, /* v.i: a signed integer of `size` bytes */
	XTC_LOGARG_UINT,    /* v.u: an unsigned integer or _Bool */
	XTC_LOGARG_DOUBLE,  /* v.d */
	XTC_LOGARG_STR,     /* v.s, may be NULL */
	XTC_LOGARG_PTR,     /* v.p */
	XTC_LOGARG_PID      /* v.pid: an xtc_pid_t */
};

/* One converted xtc_logf argument.  Make it with xtc_logarg(x). */
typedef struct xtc_logarg {
	int kind; /* XTC_LOGARG_* */
	int size; /* sizeof the integer, for {:x} of a negative one */
	union {
		long long i;
		unsigned long long u;
		double d;
		const char *s;
		const void *p;
		struct {
			uint32_t loop_id;
			uint32_t local_id;
			uint64_t gen;
		} pid;
	} v;
} xtc_logarg_t;

/* Format fmt with the nargs arguments in args and append the record, as
 * xtc_log_write does.  Called by the xtc_logf macros. */
XTC_API void xtc_logf_impl(xtc_log_t *log, xtc_log_level_t lvl, const char *fmt,
    int nargs, const xtc_logarg_t *args);

#ifndef __cplusplus
static inline xtc_logarg_t
xtc__logarg_mk_i(long long v, size_t size)
{
	xtc_logarg_t a;

	a.kind = XTC_LOGARG_INT;
	a.size = (int)size;
	a.v.i = v;
	return a;
}

static inline xtc_logarg_t
xtc__logarg_mk_u(unsigned long long v, size_t size)
{
	xtc_logarg_t a;

	a.kind = XTC_LOGARG_UINT;
	a.size = (int)size;
	a.v.u = v;
	return a;
}

static inline xtc_logarg_t
xtc__logarg_mk_d(double v)
{
	xtc_logarg_t a;

	a.kind = XTC_LOGARG_DOUBLE;
	a.size = (int)sizeof v;
	a.v.d = v;
	return a;
}

static inline xtc_logarg_t
xtc__logarg_mk_ld(long double v)
{
	return xtc__logarg_mk_d((double)v);
}

static inline xtc_logarg_t
xtc__logarg_mk_s(const char *v)
{
	xtc_logarg_t a;

	a.kind = XTC_LOGARG_STR;
	a.size = (int)sizeof v;
	a.v.s = v;
	return a;
}

/* Through uintptr_t: a volatile pointee's address is printed, not read. */
static inline xtc_logarg_t
xtc__logarg_mk_p(const volatile void *v)
{
	xtc_logarg_t a;

	a.kind = XTC_LOGARG_PTR;
	a.size = (int)sizeof v;
	a.v.p = (const void *)(uintptr_t)v;
	return a;
}

/*
 * The integer types, X(suffix, type, maker): one converter per type, so
 * each knows its own width without a sizeof of the argument (which a
 * bit-field would not allow).
 */
#define XTC__LOGARG_INTS(X)                                                    \
	X(c, char, xtc__logarg_mk_i)                                           \
	X(sc, signed char, xtc__logarg_mk_i)                                   \
	X(s, short, xtc__logarg_mk_i)                                          \
	X(i, int, xtc__logarg_mk_i)                                            \
	X(l, long, xtc__logarg_mk_i)                                           \
	X(ll, long long, xtc__logarg_mk_i)                                     \
	X(uc, unsigned char, xtc__logarg_mk_u)                                 \
	X(us, unsigned short, xtc__logarg_mk_u)                                \
	X(u, unsigned int, xtc__logarg_mk_u)                                   \
	X(ul, unsigned long, xtc__logarg_mk_u)                                 \
	X(ull, unsigned long long, xtc__logarg_mk_u)

#define XTC__LOGARG_C_DEF(sfx, T, mk)                                          \
	static inline xtc_logarg_t xtc__logarg_##sfx(T v)                      \
	{                                                                      \
		return mk(v, sizeof v);                                        \
	}
XTC__LOGARG_INTS(XTC__LOGARG_C_DEF)
#undef XTC__LOGARG_C_DEF

/* xtc_proc.h, which includes this header, redefines it to add xtc_pid_t. */
#define XTC__LOGARG_PID
#define XTC__LOGARG_ASSOC(sfx, T, mk) T: xtc__logarg_##sfx,
#define xtc_logarg(x)                                                          \
	_Generic((x),                                                          \
	    XTC__LOGARG_INTS(XTC__LOGARG_ASSOC) XTC__LOGARG_PID                \
	    _Bool: xtc__logarg_uc,                                             \
	    float: xtc__logarg_mk_d,                                           \
	    double: xtc__logarg_mk_d,                                          \
	    long double: xtc__logarg_mk_ld,                                    \
	    char *: xtc__logarg_mk_s,                                          \
	    const char *: xtc__logarg_mk_s,                                    \
	    default: XTC__LOGARG_PROMOTED(x))(x)

/*
 * Not one of the types above: select on the type after the integer
 * promotions.  cl matches an enum only by its own type, and gcc gives a
 * bit-field one of its own; an enum, and a bit-field no wider than an
 * int, promote to int or unsigned int.  Anything else (a pointer, an
 * array) goes on to the pointer converter.  The operand is not
 * evaluated.
 */
#define XTC__LOGARG_PROMOTED(x)                                                \
	_Generic(1 ? (x) : (x),                                                \
	    int: xtc__logarg_i,                                                \
	    unsigned int: xtc__logarg_u,                                       \
	    default: xtc__logarg_mk_p)

/*
 * Count the arguments after the format (0 to 16) and pick XTC__LOGF_<n>.
 * With 17 to 31 the count is E, which names an undeclared identifier: a
 * compile error that says what is wrong.
 */
#define xtc_logf_to(log, lvl, ...)                                             \
	XTC__LOGF_CAT(XTC__LOGF_, XTC__LOGF_N(__VA_ARGS__))                    \
	((log), (lvl), __VA_ARGS__)
#define XTC__LOGF_CAT(a, b) XTC__LOGF_CAT_(a, b)
#define XTC__LOGF_CAT_(a, b) a##b
#define XTC__LOGF_N(...)                                                       \
	XTC__LOGF_PICK(__VA_ARGS__, E, E, E, E, E, E, E, E, E, E, E, E, E, E,  \
	    E, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0, ~)
#define XTC__LOGF_PICK(_1, _2, _3, _4, _5, _6, _7, _8, _9, _10, _11, _12, _13, \
    _14, _15, _16, _17, _18, _19, _20, _21, _22, _23, _24, _25, _26, _27, _28, \
    _29, _30, _31, _32, n, ...)                                                \
	n
#define XTC__LOGF_E(...) xtc_logf_takes_at_most_16_arguments

#define XTC__LOGF_0(log, lvl, fmt) xtc_logf_impl(log, lvl, fmt, 0, NULL)
#define XTC__LOGF_ARR(log, lvl, fmt, n, ...)                                   \
	xtc_logf_impl(log, lvl, fmt, n,                                        \
	    (const xtc_logarg_t[]) { XTC__LOGA_##n(__VA_ARGS__) })
#define XTC__LOGF_1(l, v, f, ...) XTC__LOGF_ARR(l, v, f, 1, __VA_ARGS__)
#define XTC__LOGF_2(l, v, f, ...) XTC__LOGF_ARR(l, v, f, 2, __VA_ARGS__)
#define XTC__LOGF_3(l, v, f, ...) XTC__LOGF_ARR(l, v, f, 3, __VA_ARGS__)
#define XTC__LOGF_4(l, v, f, ...) XTC__LOGF_ARR(l, v, f, 4, __VA_ARGS__)
#define XTC__LOGF_5(l, v, f, ...) XTC__LOGF_ARR(l, v, f, 5, __VA_ARGS__)
#define XTC__LOGF_6(l, v, f, ...) XTC__LOGF_ARR(l, v, f, 6, __VA_ARGS__)
#define XTC__LOGF_7(l, v, f, ...) XTC__LOGF_ARR(l, v, f, 7, __VA_ARGS__)
#define XTC__LOGF_8(l, v, f, ...) XTC__LOGF_ARR(l, v, f, 8, __VA_ARGS__)
#define XTC__LOGF_9(l, v, f, ...) XTC__LOGF_ARR(l, v, f, 9, __VA_ARGS__)
#define XTC__LOGF_10(l, v, f, ...) XTC__LOGF_ARR(l, v, f, 10, __VA_ARGS__)
#define XTC__LOGF_11(l, v, f, ...) XTC__LOGF_ARR(l, v, f, 11, __VA_ARGS__)
#define XTC__LOGF_12(l, v, f, ...) XTC__LOGF_ARR(l, v, f, 12, __VA_ARGS__)
#define XTC__LOGF_13(l, v, f, ...) XTC__LOGF_ARR(l, v, f, 13, __VA_ARGS__)
#define XTC__LOGF_14(l, v, f, ...) XTC__LOGF_ARR(l, v, f, 14, __VA_ARGS__)
#define XTC__LOGF_15(l, v, f, ...) XTC__LOGF_ARR(l, v, f, 15, __VA_ARGS__)
#define XTC__LOGF_16(l, v, f, ...) XTC__LOGF_ARR(l, v, f, 16, __VA_ARGS__)

/* XTC__LOGA_<n>(a1, ..., an): the n converted arguments. */
#define XTC__LOGA_1(a) xtc_logarg(a)
#define XTC__LOGA_2(a, ...) xtc_logarg(a), XTC__LOGA_1(__VA_ARGS__)
#define XTC__LOGA_3(a, ...) xtc_logarg(a), XTC__LOGA_2(__VA_ARGS__)
#define XTC__LOGA_4(a, ...) xtc_logarg(a), XTC__LOGA_3(__VA_ARGS__)
#define XTC__LOGA_5(a, ...) xtc_logarg(a), XTC__LOGA_4(__VA_ARGS__)
#define XTC__LOGA_6(a, ...) xtc_logarg(a), XTC__LOGA_5(__VA_ARGS__)
#define XTC__LOGA_7(a, ...) xtc_logarg(a), XTC__LOGA_6(__VA_ARGS__)
#define XTC__LOGA_8(a, ...) xtc_logarg(a), XTC__LOGA_7(__VA_ARGS__)
#define XTC__LOGA_9(a, ...) xtc_logarg(a), XTC__LOGA_8(__VA_ARGS__)
#define XTC__LOGA_10(a, ...) xtc_logarg(a), XTC__LOGA_9(__VA_ARGS__)
#define XTC__LOGA_11(a, ...) xtc_logarg(a), XTC__LOGA_10(__VA_ARGS__)
#define XTC__LOGA_12(a, ...) xtc_logarg(a), XTC__LOGA_11(__VA_ARGS__)
#define XTC__LOGA_13(a, ...) xtc_logarg(a), XTC__LOGA_12(__VA_ARGS__)
#define XTC__LOGA_14(a, ...) xtc_logarg(a), XTC__LOGA_13(__VA_ARGS__)
#define XTC__LOGA_15(a, ...) xtc_logarg(a), XTC__LOGA_14(__VA_ARGS__)
#define XTC__LOGA_16(a, ...) xtc_logarg(a), XTC__LOGA_15(__VA_ARGS__)

#define xtc_logf(lvl, ...) xtc_logf_to(xtc_log_default(), (lvl), __VA_ARGS__)

#endif /* !__cplusplus */

#ifdef __cplusplus
}
#endif

#endif /* XTC_LOG_H */
