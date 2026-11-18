/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m12/test_observability.c -- verifies the four new
 * observability modules: log, cfg, inject, pdict.
 */

#include <pthread.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/types.h>
#include <unistd.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_fs.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_log.h"
#include "xtc_cfg.h"
#include "xtc_inject.h"
#include "xtc_pdict.h"
#include "xtc_inspect.h"
#include "xtc_int.h"

/* ----- Logger -------------------------------------------------- */

static char    g_log_buf[8192];
static size_t  g_log_n;
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

static int
sink_capture(void *user, xtc_log_level_t lvl, const char *buf, size_t len)
{
	(void)user; (void)lvl;
	(void)pthread_mutex_lock(&g_log_lock);
	if (g_log_n + len < sizeof g_log_buf) {
		memcpy(g_log_buf + g_log_n, buf, len);
		g_log_n += len;
	}
	(void)pthread_mutex_unlock(&g_log_lock);
	return 0;
}

static MunitResult
test_log_basic(const MunitParameter p[], void *d)
{
	xtc_log_t *log;
	xtc_log_opts_t opts = XTC_LOG_OPTS_INIT;
	(void)p; (void)d;

	g_log_n = 0;
	opts.sink = sink_capture;
	opts.sink_fd = -1;
	opts.floor = XTC_LOG_DEBUG;
	munit_assert_int(xtc_log_create(&opts, &log), ==, XTC_OK);

	xtc_log_write(log, XTC_LOG_INFO,  "hello %s %d", "world", 42);
	xtc_log_write(log, XTC_LOG_TRACE, "below floor");   /* dropped */
	xtc_log_write(log, XTC_LOG_DEBUG, "n=%d", 7);
	xtc_log_write(log, XTC_LOG_ERROR, "fail!");

	munit_assert_int(xtc_log_drain(log), ==, 3);
	munit_assert_not_null(strstr(g_log_buf, "hello world 42"));
	munit_assert_not_null(strstr(g_log_buf, "n=7"));
	munit_assert_not_null(strstr(g_log_buf, "fail!"));
	munit_assert_null(strstr(g_log_buf, "below floor"));
	munit_assert_int(xtc_log_drop_count(log), ==, 0);
	xtc_log_destroy(log);
	return MUNIT_OK;
}

/* When the ring is full we drop oldest and bump the counter. */
static MunitResult
test_log_drop_on_full(const MunitParameter p[], void *d)
{
	xtc_log_t *log;
	xtc_log_opts_t opts = XTC_LOG_OPTS_INIT;
	int i;
	(void)p; (void)d;
	g_log_n = 0;
	opts.sink = sink_capture;
	opts.sink_fd = -1;
	opts.ring_size = 8;
	opts.floor = XTC_LOG_INFO;
	munit_assert_int(xtc_log_create(&opts, &log), ==, XTC_OK);
	for (i = 0; i < 100; i++)
		xtc_log_write(log, XTC_LOG_INFO, "msg %d", i);
	munit_assert_int(xtc_log_drop_count(log), ==, 92);
	/* The oldest are dropped: the newest 8 come out, in order. */
	munit_assert_int(xtc_log_drain(log), ==, 8);
	g_log_buf[g_log_n] = '\0';
	munit_assert_null(strstr(g_log_buf, "msg 91\n"));
	munit_assert_not_null(strstr(g_log_buf, "msg 92\n"));
	munit_assert_ptr(strstr(g_log_buf, "msg 92\n"), <,
	    strstr(g_log_buf, "msg 99\n"));
	xtc_log_destroy(log);
	return MUNIT_OK;
}

/*
 * Several writer threads and one drainer thread (report 09).  Every record
 * the sink sees must be one whole record ("W<t>:<seq>:" and 200 copies of
 * one letter), each writer's records must arrive in the order it wrote
 * them, and printed + dropped must equal written.  Before the per-slot
 * sequence the drainer printed slots a writer had claimed but not filled
 * (an empty body, level TRACE, timestamp 0) and lost the real record.
 */
#define MPSC_WRITERS 4
#define MPSC_RECS 50000

static int g_mpsc_bad;
static int g_mpsc_lines;
static int g_mpsc_last[MPSC_WRITERS];
static int g_mpsc_stop;
static xtc_log_t *g_mpsc_log;

static int
sink_mpsc(void *user, xtc_log_level_t lvl, const char *line, size_t n)
{
	const char *p = strstr(line, " W");
	char *end;
	long t, seq;
	size_t k = 0;
	char c;
	(void)user;
	g_mpsc_lines++;
	if (lvl != XTC_LOG_INFO || p == NULL) {
		g_mpsc_bad++;
		return 0;
	}
	t = strtol(p + 2, &end, 10);
	if (*end != ':' || t < 0 || t >= MPSC_WRITERS) {
		g_mpsc_bad++;
		return 0;
	}
	seq = strtol(end + 1, &end, 10);
	if (*end != ':' || seq <= g_mpsc_last[t]) {
		g_mpsc_bad++;
		return 0;
	}
	g_mpsc_last[t] = (int)seq;
	c = end[1];
	while (end + 1 + k < line + n && end[1 + k] == c)
		k++;
	if (k != 200 || end[1 + k] != '\n')
		g_mpsc_bad++;
	return 0;
}

static void *
mpsc_writer(void *arg)
{
	int t = (int)(intptr_t)arg, s;
	char fill[201];
	for (s = 0; s < MPSC_RECS; s++) {
		memset(fill, 'a' + (s + t) % 26, 200);
		fill[200] = '\0';
		xtc_log_write(g_mpsc_log, XTC_LOG_INFO, "W%d:%d:%s", t, s,
		    fill);
	}
	return NULL;
}

static void *
mpsc_drainer(void *arg)
{
	(void)arg;
	while (!xtc_atomic_load_i32(&g_mpsc_stop, xtc_mo_acquire))
		(void)xtc_log_drain(g_mpsc_log);
	(void)xtc_log_drain(g_mpsc_log);
	return NULL;
}

static MunitResult
test_log_mpsc(const MunitParameter p[], void *d)
{
	static const int rings[] = { 4, 64, 65536 };
	pthread_t w[MPSC_WRITERS], dr;
	xtc_log_opts_t opts = XTC_LOG_OPTS_INIT;
	int r, i;
	(void)p;
	(void)d;

	for (r = 0; r < (int)(sizeof rings / sizeof rings[0]); r++) {
		g_mpsc_bad = g_mpsc_lines = 0;
		for (i = 0; i < MPSC_WRITERS; i++)
			g_mpsc_last[i] = -1;
		xtc_atomic_store_i32(&g_mpsc_stop, 0, xtc_mo_relaxed);
		opts.ring_size = rings[r];
		opts.sink = sink_mpsc;
		opts.sink_fd = -1;
		munit_assert_int(xtc_log_create(&opts, &g_mpsc_log), ==,
		    XTC_OK);
		munit_assert_int(pthread_create(&dr, NULL, mpsc_drainer, NULL),
		    ==, 0);
		for (i = 0; i < MPSC_WRITERS; i++)
			munit_assert_int(pthread_create(&w[i], NULL,
			                     mpsc_writer, (void *)(intptr_t)i),
			    ==, 0);
		for (i = 0; i < MPSC_WRITERS; i++)
			(void)pthread_join(w[i], NULL);
		xtc_atomic_store_i32(&g_mpsc_stop, 1, xtc_mo_release);
		(void)pthread_join(dr, NULL);
		munit_logf(MUNIT_LOG_INFO,
		    "ring %d: lines %d bad %d dropped %d", rings[r],
		    g_mpsc_lines, g_mpsc_bad, xtc_log_drop_count(g_mpsc_log));
		munit_assert_int(g_mpsc_bad, ==, 0);
		munit_assert_int(g_mpsc_lines + xtc_log_drop_count(g_mpsc_log),
		    ==, MPSC_WRITERS * MPSC_RECS);
		xtc_log_destroy(g_mpsc_log);
	}
	return MUNIT_OK;
}

/* xtc_log_default / xtc_log_set_default / xtc_log_set_floor / xtc_log_vwrite */
static void
log_via_vwrite(xtc_log_t *log, xtc_log_level_t lvl, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	xtc_log_vwrite(log, lvl, fmt, ap);
	va_end(ap);
}

static MunitResult
test_log_default_vwrite(const MunitParameter p[], void *d)
{
	xtc_log_t *log, *saved, *prev;
	xtc_log_opts_t opts = XTC_LOG_OPTS_INIT;
	(void)p; (void)d;

	/* Save the current process default (may be NULL: no auto-default). */
	saved = xtc_log_default();

	g_log_n = 0;
	opts.sink = sink_capture;
	opts.sink_fd = -1;
	opts.floor = XTC_LOG_INFO;
	munit_assert_int(xtc_log_create(&opts, &log), ==, XTC_OK);

	/* Install as the default; xtc_log_default now returns it. */
	munit_assert_int(xtc_log_set_default(log), ==, XTC_OK);
	munit_assert_ptr_equal(xtc_log_default(), log);

	/* vwrite: one above the floor is kept, one below is dropped. */
	log_via_vwrite(log, XTC_LOG_ERROR, "boom %d", 9);
	log_via_vwrite(log, XTC_LOG_TRACE, "noise");   /* below floor */
	munit_assert_int(xtc_log_drain(log), ==, 1);
	munit_assert_not_null(strstr(g_log_buf, "boom 9"));
	munit_assert_null(strstr(g_log_buf, "noise"));

	/* Raise the floor so INFO is now dropped. */
	munit_assert_int(xtc_log_set_floor(log, XTC_LOG_ERROR), ==, XTC_OK);
	g_log_n = 0;
	log_via_vwrite(log, XTC_LOG_INFO, "info gone");
	munit_assert_int(xtc_log_drain(log), ==, 0);
	munit_assert_null(strstr(g_log_buf, "info gone"));

	/* NULL-arg guards. */
	munit_assert_int(xtc_log_set_floor(NULL, XTC_LOG_INFO), ==, XTC_E_INVAL);

	/* swap_default hands back the logger it replaced, atomically. */
	munit_assert_int(xtc_log_swap_default(NULL, &prev), ==, XTC_OK);
	munit_assert_ptr_equal(prev, log);
	munit_assert_null(xtc_log_default());
	munit_assert_int(xtc_log_swap_default(log, &prev), ==, XTC_OK);
	munit_assert_null(prev);
	munit_assert_ptr_equal(xtc_log_default(), log);
	munit_assert_int(xtc_log_swap_default(log, NULL), ==, XTC_E_INVAL);

	/* Restore the process default before destroying ours. */
	munit_assert_int(xtc_log_set_default(saved), ==, XTC_OK);
	xtc_log_destroy(log);
	return MUNIT_OK;
}

/*
 * xtc_logf: typed arguments, no conversion letters.  Each call is drained
 * at once and its record text (the line after "[ts] LEVEL ") compared
 * whole, so a wrong digit, a stray byte, or a missing mismatch marker all
 * fail.  record_max 1024 keeps the long cases whole.
 */
static xtc_log_t *g_logf_log;
static char g_logf_rec[1100];

static int
sink_logf(void *user, xtc_log_level_t lvl, const char *buf, size_t len)
{
	const char *body;

	(void)user;
	(void)lvl;
	/* "[sec.nsec] LEVEL text\n": the text starts after the level. */
	body = memchr(buf, ']', len);
	munit_assert_not_null(body);
	body += 8;
	len -= (size_t)(body - buf) + 1;
	munit_assert_size(len, <, sizeof g_logf_rec);
	memcpy(g_logf_rec, body, len);
	g_logf_rec[len] = '\0';
	return 0;
}

/* Drain the one record a xtc_logf call wrote and compare its text. */
#define LOGF_IS(want)                                                          \
	do {                                                                   \
		g_logf_rec[0] = '\0';                                          \
		munit_assert_int(xtc_log_drain(g_logf_log), ==, 1);            \
		munit_assert_string_equal(g_logf_rec, (want));                 \
	} while (0)

static void
logf_open(xtc_log_level_t floor)
{
	xtc_log_opts_t opts = XTC_LOG_OPTS_INIT;

	opts.sink = sink_logf;
	opts.sink_fd = -1;
	opts.floor = floor;
	opts.record_max = 1024;
	munit_assert_int(xtc_log_create(&opts, &g_logf_log), ==, XTC_OK);
}

enum logf_color { LOGF_RED, LOGF_GREEN };
struct logf_bits {
	unsigned u3 : 3;
	int s4 : 4;
};

/* Every integer typedef prints its value, at both extremes. */
static MunitResult
test_logf_types(const MunitParameter p[], void *d)
{
	int8_t i8 = INT8_MIN;
	int16_t i16 = INT16_MIN;
	int32_t i32 = INT32_MIN;
	int64_t i64 = INT64_MIN;
	uint8_t u8 = UINT8_MAX;
	uint16_t u16 = UINT16_MAX;
	uint32_t u32 = UINT32_MAX;
	uint64_t u64 = UINT64_MAX;
	size_t z = SIZE_MAX;
	ssize_t sz = -1;
	ptrdiff_t pd = PTRDIFF_MIN;
	intptr_t ip = INTPTR_MIN;
	uintptr_t up = UINTPTR_MAX;
#if !defined(_WIN32) /* the CRT has no pid_t */
	pid_t pid = 12345;
#endif
	off_t off = -((off_t)1 << (8 * sizeof(off_t) - 2));
	xtc_pid_t xp = { 7, 8, 0xFFFFFFFFFFFFFFFFull };
	enum logf_color col = LOGF_GREEN;
	struct logf_bits bits = { 5, -3 };
	bool yes = true, no = false;
	char ch = 'A';
	unsigned char uch = 200;
	signed char sch = -100;
	short sh = -2;
	unsigned short ush = 65535;
	long lg = -3;
	unsigned long ulg = 3;
	long long llg = LLONG_MIN;
	unsigned long long ullg = ULLONG_MAX;
	char exp[256];
	(void)p;
	(void)d;

	logf_open(XTC_LOG_INFO);
	xtc_logf_to(g_logf_log, XTC_LOG_INFO, "{} {} {} {}", i8, i16, i32, i64);
	LOGF_IS("-128 -32768 -2147483648 -9223372036854775808");
	xtc_logf_to(g_logf_log, XTC_LOG_INFO, "{} {} {} {}", u8, u16, u32, u64);
	LOGF_IS("255 65535 4294967295 18446744073709551615");

	/* The pointer-sized types differ by data model: compare with what
	 * the right printf conversion prints on this target. */
	xtc_logf_to(g_logf_log, XTC_LOG_INFO, "{} {} {} {} {}", z, sz, pd, ip,
	    up);
	(void)snprintf(exp, sizeof exp, "%zu %zd %td %jd %ju", z, sz, pd,
	    (intmax_t)ip, (uintmax_t)up);
	LOGF_IS(exp);
#if !defined(_WIN32)
	xtc_logf_to(g_logf_log, XTC_LOG_INFO, "{}", pid);
	LOGF_IS("12345");
#endif
	xtc_logf_to(g_logf_log, XTC_LOG_INFO, "{} {}", off, xp);
	(void)snprintf(exp, sizeof exp, "%jd <7.8.18446744073709551615>",
	    (intmax_t)off);
	LOGF_IS(exp);

	xtc_logf_to(g_logf_log, XTC_LOG_INFO, "{} {} {} {} {} {}", col, yes, no,
	    ch, uch, sch);
	LOGF_IS("1 1 0 65 200 -100");
	xtc_logf_to(g_logf_log, XTC_LOG_INFO, "{} {}", bits.u3, bits.s4);
	LOGF_IS("5 -3");
	xtc_logf_to(g_logf_log, XTC_LOG_INFO, "{} {} {} {} {} {}", sh, ush, lg,
	    ulg, llg, ullg);
	LOGF_IS("-2 65535 -3 3 -9223372036854775808 18446744073709551615");

	/* Literals: an int, a negative long long, an unsigned suffix. */
	xtc_logf_to(g_logf_log, XTC_LOG_INFO, "{} {} {}", 42, -5LL, 7u);
	LOGF_IS("42 -5 7");
	xtc_log_destroy(g_logf_log);
	return MUNIT_OK;
}

static int g_logf_calls;

static int
logf_count(void)
{
	return ++g_logf_calls;
}

/* Doubles, strings (NULL too), and pointers. */
static MunitResult
test_logf_values(const MunitParameter p[], void *d)
{
	char *ns = NULL;
	const char *cs = "const";
	char buf[] = "array";
	int obj, *ip = &obj;
	const void *cvp = &obj;
	char exp[256];
	float f = 1.5f;
	(void)p;
	(void)d;

	logf_open(XTC_LOG_INFO);
	xtc_logf_to(g_logf_log, XTC_LOG_INFO, "{} {} {} {}", 0.5, f, 1e100,
	    -0.25);
	LOGF_IS("0.5 1.5 1e+100 -0.25");
	xtc_logf_to(g_logf_log, XTC_LOG_INFO, "[{}] [{}] [{}] [{}]", ns, cs,
	    buf, "lit");
	LOGF_IS("[(null)] [const] [array] [lit]");

	/* Pointers print as 0x and hex on every platform. */
	xtc_logf_to(g_logf_log, XTC_LOG_INFO, "{} {} {}", ip, cvp, (void *)0);
	(void)snprintf(exp, sizeof exp, "0x%jx 0x%jx 0x0",
	    (uintmax_t)(uintptr_t)ip, (uintmax_t)(uintptr_t)cvp);
	LOGF_IS(exp);
	xtc_logf_to(g_logf_log, XTC_LOG_INFO, "{:p}", cs);
	(void)snprintf(exp, sizeof exp, "0x%jx", (uintmax_t)(uintptr_t)cs);
	LOGF_IS(exp);

	/* Each argument is evaluated once. */
	g_logf_calls = 0;
	xtc_logf_to(g_logf_log, XTC_LOG_INFO, "{}", logf_count());
	LOGF_IS("1");
	munit_assert_int(g_logf_calls, ==, 1);
	xtc_log_destroy(g_logf_log);
	return MUNIT_OK;
}

/* The spec mini-language and literal braces. */
static MunitResult
test_logf_specs(const MunitParameter p[], void *d)
{
	int8_t m8 = -1;
	int32_t m32 = -1;
	int64_t m64 = -1;
	(void)p;
	(void)d;

	logf_open(XTC_LOG_INFO);
	xtc_logf_to(g_logf_log, XTC_LOG_INFO, "{:x} {:X} {:o} {}", 255, 0xABCu,
	    8, UINT64_MAX);
	LOGF_IS("ff ABC 10 18446744073709551615");

	/* Width and zero fill, numbers and strings. */
	xtc_logf_to(g_logf_log, XTC_LOG_INFO, "[{:08x}] [{:8}] [{:08}] [{:6}]",
	    0xbeefu, 42, -42, "ab");
	LOGF_IS("[0000beef] [      42] [-0000042] [    ab]");

	/* A negative integer in hex is its own width's two's complement. */
	xtc_logf_to(g_logf_log, XTC_LOG_INFO, "{:x} {:x} {:X} {:o}", m8, m32,
	    m64, m8);
	LOGF_IS("ff ffffffff FFFFFFFFFFFFFFFF 377");

	/* Precision: decimal places of a double, characters of a string. */
	xtc_logf_to(g_logf_log, XTC_LOG_INFO, "{:.3} {:.0} [{:8.2}] [{:.2}]",
	    3.14159, 2.5, 2.25, "abcdef");
	LOGF_IS("3.142 2 [    2.25] [ab]");

	/* {{ and }} are braces; a brace that is not a placeholder is text,
	 * and does not count as one. */
	xtc_logf_to(g_logf_log, XTC_LOG_INFO, "{{}} {{{}}} }{ {x} {:q} {:.} {",
	    1);
	LOGF_IS("{} {1} }{ {x} {:q} {:.} {");
	xtc_log_destroy(g_logf_log);
	return MUNIT_OK;
}

/* Zero and sixteen arguments, mismatches both ways, and the floor. */
static MunitResult
test_logf_counts(const MunitParameter p[], void *d)
{
	xtc_log_t *saved;
	(void)p;
	(void)d;

	logf_open(XTC_LOG_INFO);
	xtc_logf_to(g_logf_log, XTC_LOG_INFO, "plain");
	LOGF_IS("plain");
	xtc_logf_to(g_logf_log, XTC_LOG_INFO,
	    "{}{}{}{}{}{}{}{}{}{}{}{}{}{}{}{}", 1, 2, 3, 4, 5, 6, 7, 8, 9, 0, 1,
	    2, 3, 4, 5, 6);
	LOGF_IS("1234567890123456");

	/* Too few arguments: the record is marked and the extra {} kept. */
	xtc_logf_to(g_logf_log, XTC_LOG_INFO, "a={} b={} c={:x}", 1, 2);
	LOGF_IS("[xtc_logf: 2 args, 3 {}] a=1 b=2 c={:x}");
	xtc_logf_to(g_logf_log, XTC_LOG_INFO, "none {}");
	LOGF_IS("[xtc_logf: 0 args, 1 {}] none {}");
	/* Too many: marked, the extra arguments not printed. */
	xtc_logf_to(g_logf_log, XTC_LOG_INFO, "one={}", 1, "two", 3.0);
	LOGF_IS("[xtc_logf: 3 args, 1 {}] one=1");
	xtc_logf_to(g_logf_log, XTC_LOG_INFO, "{{}}", 1);
	LOGF_IS("[xtc_logf: 1 arg, 0 {}] {}");

	/* The direct call tolerates what the macros never pass. */
	xtc_logf_impl(g_logf_log, XTC_LOG_INFO, "{}", 1, NULL);
	LOGF_IS("[xtc_logf: 0 args, 1 {}] {}");
	xtc_logf_impl(g_logf_log, XTC_LOG_INFO, NULL, 0, NULL);
	xtc_logf_impl(NULL, XTC_LOG_INFO, "x", 0, NULL);
	munit_assert_int(xtc_log_drain(g_logf_log), ==, 0);

	/* Below the floor: nothing.  xtc_logf goes to the default logger. */
	xtc_logf_to(g_logf_log, XTC_LOG_DEBUG, "quiet {}", 1);
	munit_assert_int(xtc_log_drain(g_logf_log), ==, 0);
	munit_assert_int(xtc_log_swap_default(g_logf_log, &saved), ==, XTC_OK);
	xtc_logf(XTC_LOG_WARN, "default {}", 9);
	LOGF_IS("default 9");
	munit_assert_int(xtc_log_set_default(saved), ==, XTC_OK);
	munit_assert_int(xtc_log_drop_count(g_logf_log), ==, 0);
	xtc_log_destroy(g_logf_log);
	return MUNIT_OK;
}

/* A long record is truncated to record_max with the marker intact, and a
 * full ring drops oldest, exactly as xtc_log_write does. */
static MunitResult
test_logf_ring(const MunitParameter p[], void *d)
{
	xtc_log_opts_t opts = XTC_LOG_OPTS_INIT;
	char big[2000];
	int i;
	(void)p;
	(void)d;

	memset(big, 'z', sizeof big - 1);
	big[sizeof big - 1] = '\0';
	opts.sink = sink_logf;
	opts.sink_fd = -1;
	opts.record_max = 32;
	opts.ring_size = 4;
	munit_assert_int(xtc_log_create(&opts, &g_logf_log), ==, XTC_OK);
	xtc_logf_to(g_logf_log, XTC_LOG_INFO, "{} {}", big);
	LOGF_IS("[xtc_logf: 1 arg, 2 {}] zzzzzzz");
	for (i = 0; i < 10; i++)
		xtc_logf_to(g_logf_log, XTC_LOG_INFO, "n={}", i);
	munit_assert_int(xtc_log_drop_count(g_logf_log), ==, 6);
	munit_assert_int(xtc_log_drain(g_logf_log), ==, 4);
	munit_assert_string_equal(g_logf_rec, "n=9");
	xtc_log_destroy(g_logf_log);
	return MUNIT_OK;
}

/* ----- Config / GUC ------------------------------------------- */

static int g_changed_count;
static int g_changed_old, g_changed_new;
static void
my_changed(const char *name, const void *o, const void *n, void *u)
{
	(void)name; (void)u;
	g_changed_count++;
	if (o) g_changed_old = *(const int *)o;
	if (n) g_changed_new = *(const int *)n;
}

static MunitResult
test_cfg_int(const MunitParameter p[], void *d)
{
	xtc_cfg_spec_t spec = XTC_CFG_SPEC_INIT;
	int v;
	(void)p; (void)d;
	g_changed_count = 0;

	spec.name = "test.work_mem";
	spec.short_desc = "test int knob";
	spec.kind = XTC_CFG_INT;
	spec.dflt.d_int = 100;
	spec.min_int = 0;
	spec.max_int = 1000;
	spec.on_change = my_changed;
	munit_assert_int(xtc_cfg_register(&spec), ==, XTC_OK);

	munit_assert_int(xtc_cfg_get_int("test.work_mem", &v), ==, XTC_OK);
	munit_assert_int(v, ==, 100);

	munit_assert_int(xtc_cfg_set_int("test.work_mem", 500), ==, XTC_OK);
	munit_assert_int(xtc_cfg_get_int("test.work_mem", &v), ==, XTC_OK);
	munit_assert_int(v, ==, 500);
	munit_assert_int(g_changed_count, ==, 1);
	munit_assert_int(g_changed_old, ==, 100);
	munit_assert_int(g_changed_new, ==, 500);

	/* Out of bounds rejected. */
	munit_assert_int(xtc_cfg_set_int("test.work_mem", 9999), ==, XTC_E_RANGE);
	munit_assert_int(xtc_cfg_get_int("test.work_mem", &v), ==, XTC_OK);
	munit_assert_int(v, ==, 500);

	/* Wrong type rejected. */
	munit_assert_int(xtc_cfg_get_string("test.work_mem", NULL), ==, XTC_E_INVAL);

	munit_assert_int(xtc_cfg_unregister("test.work_mem"), ==, XTC_OK);
	munit_assert_int(xtc_cfg_get_int("test.work_mem", &v), ==, XTC_E_INVAL);
	return MUNIT_OK;
}

static MunitResult
test_cfg_string(const MunitParameter p[], void *d)
{
	xtc_cfg_spec_t spec = XTC_CFG_SPEC_INIT;
	const char *s;
	(void)p; (void)d;
	spec.name = "test.label";
	spec.kind = XTC_CFG_STRING;
	spec.dflt.d_string = "default";
	munit_assert_int(xtc_cfg_register(&spec), ==, XTC_OK);
	munit_assert_int(xtc_cfg_get_string("test.label", &s), ==, XTC_OK);
	munit_assert_string_equal(s, "default");
	munit_assert_int(xtc_cfg_set_string("test.label", "new"), ==, XTC_OK);
	munit_assert_int(xtc_cfg_get_string("test.label", &s), ==, XTC_OK);
	munit_assert_string_equal(s, "new");
	munit_assert_int(xtc_cfg_unregister("test.label"), ==, XTC_OK);
	return MUNIT_OK;
}

/* ----- Config-file loading + reload --------------------------- */

static MunitResult
test_cfg_load_file(const MunitParameter p[], void *d)
{
	xtc_cfg_spec_t spec = XTC_CFG_SPEC_INIT;
	static const char *levels[] = { "quiet", "normal", "loud" };
	char tmpdir[512];
	char path[640];
	int iv = 0, bv = 0, ev = 0;
	const char *sv = NULL;
	FILE *f;
	(void)p; (void)d;

	munit_assert_int(xtc_fs_tmpdir(tmpdir, sizeof tmpdir), ==, XTC_OK);
	snprintf(path, sizeof path, "%s/xtc-cfg-test-%ld.conf", tmpdir,
	    (long)getpid());

	spec.name = "t.workers"; spec.kind = XTC_CFG_INT;
	spec.dflt.d_int = 4; spec.min_int = 1; spec.max_int = 64;
	munit_assert_int(xtc_cfg_register(&spec), ==, XTC_OK);
	memset(&spec, 0, sizeof spec);
	spec.size = sizeof spec;
	spec.name = "t.tls"; spec.kind = XTC_CFG_BOOL; spec.dflt.d_bool = 0;
	munit_assert_int(xtc_cfg_register(&spec), ==, XTC_OK);
	memset(&spec, 0, sizeof spec);
	spec.size = sizeof spec;
	spec.name = "t.banner"; spec.kind = XTC_CFG_STRING; spec.dflt.d_string = "x";
	munit_assert_int(xtc_cfg_register(&spec), ==, XTC_OK);
	memset(&spec, 0, sizeof spec);
	spec.size = sizeof spec;
	spec.name = "t.level"; spec.kind = XTC_CFG_ENUM; spec.dflt.d_enum = 0;
	spec.enum_labels = levels; spec.n_enum_labels = 3;
	munit_assert_int(xtc_cfg_register(&spec), ==, XTC_OK);

	f = fopen(path, "w");
	munit_assert_not_null(f);
	fprintf(f,
	    "# a comment line\n"
	    "t.workers = 16\n"
	    "t.tls = on\n"
	    "t.banner = 'hi there'   # inline comment\n"
	    "t.level = loud\n"
	    "unknown.key = 5\n"        /* skipped */
	    "\n");
	fclose(f);

	/* 4 known keys applied; unknown skipped. */
	munit_assert_int(xtc_cfg_load_file(path), ==, 4);
	munit_assert_int(xtc_cfg_get_int("t.workers", &iv), ==, XTC_OK);
	munit_assert_int(iv, ==, 16);
	munit_assert_int(xtc_cfg_get_bool("t.tls", &bv), ==, XTC_OK);
	munit_assert_int(bv, ==, 1);
	munit_assert_int(xtc_cfg_get_string("t.banner", &sv), ==, XTC_OK);
	munit_assert_string_equal(sv, "hi there");
	munit_assert_int(xtc_cfg_get_enum("t.level", &ev), ==, XTC_OK);
	munit_assert_int(ev, ==, 2);   /* "loud" */

	/* Edit + reload re-applies. */
	f = fopen(path, "w");
	munit_assert_not_null(f);
	fprintf(f, "t.workers = 8\n");
	fclose(f);
	munit_assert_int(xtc_cfg_reload(), ==, 1);
	munit_assert_int(xtc_cfg_get_int("t.workers", &iv), ==, XTC_OK);
	munit_assert_int(iv, ==, 8);

	/* Bad path is XTC_E_IO; NULL path is XTC_E_INVAL. */
	munit_assert_int(xtc_cfg_load_file("/tmp/xtc-no-such-cfg.zzz"), ==, XTC_E_IO);
	munit_assert_int(xtc_cfg_load_file(NULL), ==, XTC_E_INVAL);

	(void)unlink(path);
	(void)xtc_cfg_unregister("t.workers");
	(void)xtc_cfg_unregister("t.tls");
	(void)xtc_cfg_unregister("t.banner");
	(void)xtc_cfg_unregister("t.level");
	return MUNIT_OK;
}

/* ----- Injection points -------------------------------------- */

static int g_inject_fired;

static void
inject_cb(const char *name, void *user)
{
	(void)name; (void)user;
	xtc_atomic_fetch_add_i32(&g_inject_fired, 1, xtc_mo_seq_cst);
}

static MunitResult
test_inject_callback(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	xtc_atomic_store_i32(&g_inject_fired, 0, xtc_mo_seq_cst);

	/* No attachment yet -- trigger is a no-op. */
	xtc_inject_trigger("xtc.test.point1");
	munit_assert_int(xtc_atomic_load_i32(&g_inject_fired, xtc_mo_seq_cst),
	    ==, 0);

	/* Attach + trigger fires. */
	munit_assert_int(xtc_inject_attach("xtc.test.point1", inject_cb, NULL),
	    ==, XTC_OK);
	xtc_inject_trigger("xtc.test.point1");
	xtc_inject_trigger("xtc.test.point1");
	munit_assert_int(xtc_atomic_load_i32(&g_inject_fired, xtc_mo_seq_cst),
	    ==, 2);

	munit_assert_int(xtc_inject_detach("xtc.test.point1"), ==, XTC_OK);
	xtc_inject_trigger("xtc.test.point1");
	munit_assert_int(xtc_atomic_load_i32(&g_inject_fired, xtc_mo_seq_cst),
	    ==, 2);
	return MUNIT_OK;
}

/* Wait-style: trigger blocks until wakeup is called from a peer. */
static int g_wait_phase;

static void *
wait_trigger_thread(void *arg)
{
	(void)arg;
	xtc_atomic_store_i32(&g_wait_phase, 1, xtc_mo_seq_cst);
	xtc_inject_trigger("xtc.test.wait_point");
	xtc_atomic_store_i32(&g_wait_phase, 2, xtc_mo_seq_cst);
	return NULL;
}

static MunitResult
test_inject_wait(const MunitParameter p[], void *d)
{
	pthread_t th;
	struct timespec ts = { 0, 100 * 1000 * 1000 };  /* 100ms */
	(void)p; (void)d;
	xtc_atomic_store_i32(&g_wait_phase, 0, xtc_mo_seq_cst);
	munit_assert_int(xtc_inject_attach_wait("xtc.test.wait_point"),
	    ==, XTC_OK);

	pthread_create(&th, NULL, wait_trigger_thread, NULL);

	/* Phase 1: trigger thread reaches and blocks.  Sleep 100ms;
	 * phase should still be 1. */
	(void)nanosleep(&ts, NULL);
	munit_assert_int(xtc_atomic_load_i32(&g_wait_phase, xtc_mo_seq_cst), ==,
	    1);

	/* Wake it up. */
	munit_assert_int(xtc_inject_wakeup("xtc.test.wait_point"), ==, XTC_OK);
	pthread_join(th, NULL);
	munit_assert_int(xtc_atomic_load_i32(&g_wait_phase, xtc_mo_seq_cst), ==,
	    2);

	(void)xtc_inject_detach("xtc.test.wait_point");
	return MUNIT_OK;
}

/* ----- Proc dictionary --------------------------------------- */

static int g_pdict_test_ok;

static void
pdict_user(void *arg)
{
	void *v = NULL;
	(void)arg;

	if (xtc_pdict_count() != 0) return;
	if (xtc_pdict_put("trace_id", (void *)(uintptr_t)42) != XTC_OK) return;
	if (xtc_pdict_put("user", (void *)(uintptr_t)"alice") != XTC_OK) return;
	if (xtc_pdict_count() != 2) return;

	if (xtc_pdict_get("trace_id", &v) != XTC_OK) return;
	if ((int)(uintptr_t)v != 42) return;
	if (xtc_pdict_get("missing", &v) != XTC_E_INVAL) return;

	/* Replace existing key. */
	if (xtc_pdict_put("trace_id", (void *)(uintptr_t)99) != XTC_OK) return;
	if (xtc_pdict_count() != 2) return;
	if (xtc_pdict_get("trace_id", &v) != XTC_OK) return;
	if ((int)(uintptr_t)v != 99) return;

	if (xtc_pdict_erase("user") != XTC_OK) return;
	if (xtc_pdict_count() != 1) return;
	if (xtc_pdict_erase("user") != XTC_E_INVAL) return;

	(void)xtc_pdict_clear();
	if (xtc_pdict_count() != 0) return;

	xtc_atomic_store_i32(&g_pdict_test_ok, 1, xtc_mo_seq_cst);
}

static MunitResult
test_pdict_basic(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_pid_t pid;
	(void)p; (void)d;
	xtc_atomic_store_i32(&g_pdict_test_ok, 0, xtc_mo_seq_cst);
	munit_assert_int(xtc_loop_create(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, pdict_user, NULL, NULL, &pid),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_atomic_load_i32(&g_pdict_test_ok, xtc_mo_seq_cst),
	    ==, 1);
	xtc_loop_destroy(loop);
	return MUNIT_OK;
}

/* Outside any proc, pdict ops fail. */
static MunitResult
test_pdict_outside_proc(const MunitParameter p[], void *d)
{
	void *v;
	(void)p; (void)d;
	munit_assert_int(xtc_pdict_put("k", (void *)1), ==, XTC_E_INVAL);
	munit_assert_int(xtc_pdict_get("k", &v), ==, XTC_E_INVAL);
	munit_assert_int(xtc_pdict_count(), ==, 0);
	return MUNIT_OK;
}

/* ----- Resource alerts ---------------------------------------- */

#include "xtc_res.h"

static int g_alert_count;
static xtc_res_kind_t g_last_kind;

static void
alert_cb(xtc_res_kind_t k, int64_t used, int64_t cap, void *user)
{
	(void)used; (void)cap; (void)user;
	g_last_kind = k;
	xtc_atomic_fetch_add_i32(&g_alert_count, 1, xtc_mo_seq_cst);
}

static MunitResult
test_res_alert(const MunitParameter p[], void *d)
{
	xtc_res_t r;
	xtc_res_caps_t caps = XTC_RES_CAPS_INIT;
	(void)p; (void)d;
	caps.tasks = 100;
	munit_assert_int(xtc_res_init(&r, &caps), ==, XTC_OK);
	munit_assert_int(xtc_res_set_alert(&r, XTC_RES_TASKS, 0.8), ==, XTC_OK);
	munit_assert_int(xtc_res_set_alert_fn(&r, alert_cb, NULL), ==, XTC_OK);
	xtc_atomic_store_i32(&g_alert_count, 0, xtc_mo_seq_cst);

	/* Acquire 70 -- under threshold, no fire. */
	munit_assert_int(xtc_res_acquire(&r, XTC_RES_TASKS, 70), ==, XTC_OK);
	munit_assert_int(xtc_atomic_load_i32(&g_alert_count, xtc_mo_seq_cst),
	    ==, 0);
	/* Push to 85 -- fires once. */
	munit_assert_int(xtc_res_acquire(&r, XTC_RES_TASKS, 15), ==, XTC_OK);
	munit_assert_int(xtc_atomic_load_i32(&g_alert_count, xtc_mo_seq_cst),
	    ==, 1);
	/* Stay above; no re-fire. */
	munit_assert_int(xtc_res_acquire(&r, XTC_RES_TASKS, 5), ==, XTC_OK);
	munit_assert_int(xtc_atomic_load_i32(&g_alert_count, xtc_mo_seq_cst),
	    ==, 1);
	/* Drop below threshold and back up -- re-arms and fires again. */
	xtc_res_release(&r, XTC_RES_TASKS, 30);
	munit_assert_int(xtc_res_acquire(&r, XTC_RES_TASKS, 30), ==, XTC_OK);
	munit_assert_int(xtc_atomic_load_i32(&g_alert_count, xtc_mo_seq_cst),
	    ==, 2);
	munit_assert_int(g_last_kind, ==, XTC_RES_TASKS);
	return MUNIT_OK;
}

/* ----- live introspection (xtc_inspect) ----------------------- */
#include "xtc_inspect.h"

static int g_insp_seen;         /* procs the callback saw */
static int g_insp_sleeper_mbox; /* mailbox depth seen on the sleeper */
static int g_insp_loop_procs;   /* procs counted via xtc_inspect_loops */
static xtc_pid_t   g_insp_sleeper;

static int
insp_proc_cb(const xtc_proc_info_t *info, void *user)
{
	(void)user;
	xtc_atomic_fetch_add_i32(&g_insp_seen, 1, xtc_mo_seq_cst);
	if (xtc_pid_eq(info->pid, g_insp_sleeper))
		xtc_atomic_store_i32(&g_insp_sleeper_mbox, (int)info->mbox_len,
		    xtc_mo_seq_cst);
	return 0;
}

static int
insp_loop_cb(const xtc_loop_info_t *info, void *user)
{
	int *procs = user;
	*procs += info->n_procs;
	return 0;
}

static void
insp_sleeper(void *a)
{
	(void)a;
	(void)xtc_proc_sleep(300LL * 1000 * 1000);         /* park ~300ms */
}

static void
insp_driver(void *a)
{
	(void)a;
	const char *m = "x";
	/* Queue messages at the (timer-parked) sleeper: they sit in its
	 * mailbox undelivered, so inspect sees a non-zero depth. */
	xtc_send(g_insp_sleeper, m, 2);
	xtc_send(g_insp_sleeper, m, 2);
	(void)xtc_proc_sleep(2LL * 1000 * 1000);           /* let sends land */
	(void)xtc_inspect_procs(insp_proc_cb, NULL);
	{
		int lp = 0;
		(void)xtc_inspect_loops(insp_loop_cb, &lp);
		xtc_atomic_store_i32(&g_insp_loop_procs, lp, xtc_mo_seq_cst);
	}
	{
		xtc_proc_info_t info = XTC_PROC_INFO_INIT;
		int rc = xtc_proc_info(g_insp_sleeper, &info);
		if (rc == XTC_OK && info.mbox_len == 2 &&
		    info.run_state == XTC_PROC_PARKED)
			xtc_atomic_fetch_add_i32(&g_insp_seen, 1000,
			    xtc_mo_seq_cst); /* sentinel: per-pid ok */
	}
	/* The sleeper wakes on its own (~300ms); no need to kill it. */
}

static MunitResult
test_inspect_procs(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t o = XTC_PROC_OPTS_INIT;
	xtc_pid_t dpid;
	(void)p; (void)d;

	xtc_atomic_store_i32(&g_insp_seen, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_insp_sleeper_mbox, -1, xtc_mo_seq_cst);

	munit_assert_int(xtc_loop_create(&loop), ==, XTC_OK);
	o.name = "sleeper";
	munit_assert_int(xtc_proc_spawn(loop, insp_sleeper, NULL, &o,
	    &g_insp_sleeper), ==, XTC_OK);
	o.name = "driver";
	munit_assert_int(xtc_proc_spawn(loop, insp_driver, NULL, &o, &dpid),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	/* The driver enumerated at least the two procs, saw the sleeper's
	 * mailbox at depth 2, and the per-pid lookup matched (sentinel). */
	munit_assert_int(xtc_atomic_load_i32(&g_insp_seen, xtc_mo_seq_cst), >=,
	    1002);
	munit_assert_int(xtc_atomic_load_i32(&g_insp_sleeper_mbox,
	                     xtc_mo_seq_cst),
	    ==, 2);
	munit_assert_int(xtc_atomic_load_i32(&g_insp_loop_procs,
	                     xtc_mo_seq_cst),
	    >=, 2);

	xtc_loop_destroy(loop);
	return MUNIT_OK;
}

/*
 * park_reason must name the MAILBOX park.
 *
 * A fiber blocked in xtc_recv(..., -1) reported run_state=PARKED with
 * park_reason=XTC_PROC_PARK_NONE -- a bare, sourceless park.  The branch meant
 * to catch it tested task->park_requested, but the coro substrate CONSUMES
 * and clears that one-shot when it turns the yield into the PENDING
 * verdict, so it is ALWAYS 0 by the time the task is PARKED: dead code.
 *
 * The mislabel was not cosmetic.  A sourceless park is also what a LOST
 * WAKE looks like, so a consumer chasing a hang could not tell a fiber
 * healthily waiting for a message from one whose wake had been dropped --
 * and reasonably read "queued mailbox + sourceless park" as the latter.
 * (Reported by the PostgreSQL-on-libxtc integration, which spent days on
 * exactly that misreading.)
 *
 * The other two park shapes are asserted alongside it, so this pins the
 * whole park_reason mapping rather than one branch.
 */
static xtc_pid_t g_pr_mbox, g_pr_timer;
static int g_pr_mbox_reason, g_pr_timer_reason, g_pr_checked;

static void
pr_mbox_proc(void *a)
{
	void *m = NULL;
	size_t n = 0;
	(void)a;
	/* Infinite mailbox park: no fd, no timer.  Woken by the probe's send. */
	(void)xtc_recv(&m, &n, -1);
	xtc_free(m);
}

static void
pr_timer_proc(void *a)
{
	(void)a;
	(void)xtc_proc_sleep(400LL * 1000 * 1000);
}

static void
pr_probe(void *a)
{
	xtc_proc_info_t info = XTC_PROC_INFO_INIT;
	(void)a;
	/* Let both targets reach their parks. */
	(void)xtc_proc_sleep(30LL * 1000 * 1000);

	if (xtc_proc_info(g_pr_mbox, &info) == XTC_OK &&
	    info.run_state == XTC_PROC_PARKED)
		xtc_atomic_store_i32(&g_pr_mbox_reason, info.park_reason,
		    xtc_mo_seq_cst);
	if (xtc_proc_info(g_pr_timer, &info) == XTC_OK &&
	    info.run_state == XTC_PROC_PARKED)
		xtc_atomic_store_i32(&g_pr_timer_reason, info.park_reason,
		    xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_pr_checked, 1, xtc_mo_seq_cst);

	/* Release the mailbox parker so the loop can drain. */
	(void)xtc_send(g_pr_mbox, "go", 3);
}

static MunitResult
test_inspect_park_reason(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t o = XTC_PROC_OPTS_INIT;
	xtc_pid_t probe;
	(void)p; (void)d;

	xtc_atomic_store_i32(&g_pr_mbox_reason, -1, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_pr_timer_reason, -1, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_pr_checked, 0, xtc_mo_seq_cst);

	munit_assert_int(xtc_loop_create(&loop), ==, XTC_OK);
	o.name = "pr-mbox";
	munit_assert_int(xtc_proc_spawn(loop, pr_mbox_proc, NULL, &o,
	    &g_pr_mbox), ==, XTC_OK);
	o.name = "pr-timer";
	munit_assert_int(xtc_proc_spawn(loop, pr_timer_proc, NULL, &o,
	    &g_pr_timer), ==, XTC_OK);
	o.name = "pr-probe";
	munit_assert_int(xtc_proc_spawn(loop, pr_probe, NULL, &o, &probe),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	munit_assert_int(xtc_atomic_load_i32(&g_pr_checked, xtc_mo_seq_cst), ==,
	    1);
	/* The regression: this was XTC_PROC_PARK_NONE (0) for every recv park. */
	munit_assert_int(xtc_atomic_load_i32(&g_pr_mbox_reason, xtc_mo_seq_cst),
	    ==, XTC_PROC_PARK_MAILBOX);
	munit_assert_int(xtc_atomic_load_i32(&g_pr_timer_reason,
	                     xtc_mo_seq_cst),
	    ==, XTC_PROC_PARK_TIMER);

	xtc_loop_destroy(loop);
	return MUNIT_OK;
}

/*
 * Names, labels and the critical-section depth (davros report 03).
 * xtc_proc_opts_t.name and xtc_child_spec_t.name used to be accepted and
 * dropped, and xtc_proc_info_t had no crit_depth, so a /debug/procs page
 * saw anonymous pids and could not tell a proc wedged in a critical
 * section.  A proc spawned "engine" labels itself, enters two critical
 * sections and parks; a supervised child named "listener" parks; the
 * probe reads both and dumps.
 */
#include "xtc_orc.h"
#include "xtc_dump.h"

static xtc_pid_t g_nm_engine;
static xtc_proc_info_t g_nm_eng_info, g_nm_kid_info;
static int g_nm_kid_found, g_nm_label_rc_off, g_nm_dump_fd = -1;

static void
nm_engine(void *a)
{
	(void)a;
	(void)xtc_proc_set_label("chatcmpl-abc123 with a label that is longer"
	                         " than XTC_PROC_LABEL_MAX bytes, to be cut");
	xtc_proc_critical_enter();
	xtc_proc_critical_enter();
	(void)xtc_proc_sleep(100LL * 1000 * 1000);
	xtc_proc_critical_leave();
	xtc_proc_critical_leave();
}

static void
nm_kid(void *a)
{
	(void)a;
	(void)xtc_proc_sleep(100LL * 1000 * 1000);
}

static int
nm_find_kid(const xtc_proc_info_t *info, void *user)
{
	(void)user;
	if (strcmp(info->name, "listener") == 0) {
		g_nm_kid_info = *info;
		g_nm_kid_found = 1;
	}
	return 0;
}

static void
nm_probe(void *a)
{
	xtc_supervisor_t *sup = a;
	(void)xtc_proc_sleep(10LL * 1000 * 1000); /* both are parked */
	g_nm_eng_info.size = sizeof g_nm_eng_info;
	(void)xtc_proc_info(g_nm_engine, &g_nm_eng_info);
	(void)xtc_inspect_procs(nm_find_kid, NULL);
	xtc_dump(g_nm_dump_fd);
	(void)xtc_sup_stop(sup);
}

static MunitResult
test_inspect_name_label_crit(const MunitParameter p[], void *d)
{
	char tmpl[512], buf[16384];
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t o = XTC_PROC_OPTS_INIT;
	xtc_sup_opts_t so = XTC_SUP_OPTS_INIT;
	xtc_child_spec_t kid = XTC_CHILD_SPEC_INIT;
	xtc_supervisor_t *sup = NULL;
	ssize_t n;
	(void)p;
	(void)d;

	g_nm_label_rc_off = xtc_proc_set_label("off a proc");
	munit_assert_int(g_nm_label_rc_off, ==, XTC_E_INVAL);
	/* Not "/tmp/...": on Windows that names a directory on the current
	 * drive, and CI's D: has none. */
	munit_assert_int(xtc_fs_tmpdir(tmpl, sizeof tmpl), ==, XTC_OK);
	strncat(tmpl, "/xtc_nm_dump_XXXXXX", sizeof tmpl - strlen(tmpl) - 1);
	g_nm_dump_fd = mkstemp(tmpl);
	munit_assert_int(g_nm_dump_fd, >=, 0);
	munit_assert_int(xtc_loop_create(&loop), ==, XTC_OK);
	o.name = "engine";
	munit_assert_int(xtc_proc_spawn(loop, nm_engine, NULL, &o,
	                     &g_nm_engine),
	    ==, XTC_OK);
	kid.name = "listener";
	kid.fn = nm_kid;
	kid.policy = XTC_RESTART_TEMPORARY;
	munit_assert_int(xtc_sup_start(loop, &so, &kid, 1, &sup), ==, XTC_OK);
	o.name = NULL;
	munit_assert_int(xtc_proc_spawn(loop, nm_probe, sup, &o, NULL), ==,
	    XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	(void)xtc_sup_join(sup, 1000LL * 1000 * 1000);
	xtc_loop_destroy(loop);

	munit_assert_true(XTC_FIELD_OK(&g_nm_eng_info, label));
	munit_assert_string_equal(g_nm_eng_info.name, "engine");
	munit_assert_size(strlen(g_nm_eng_info.label), ==,
	    XTC_PROC_LABEL_MAX - 1);
	munit_assert_int(strncmp(g_nm_eng_info.label, "chatcmpl-abc123 ", 16),
	    ==, 0);
	munit_assert_uint(g_nm_eng_info.crit_depth, ==, 2);
	munit_assert_int(g_nm_kid_found, ==, 1);
	munit_assert_uint(g_nm_kid_info.crit_depth, ==, 0);
	munit_assert_string_equal(g_nm_kid_info.label, "");

	munit_assert_int(lseek(g_nm_dump_fd, 0, SEEK_SET), ==, 0);
	n = read(g_nm_dump_fd, buf, sizeof buf - 1);
	munit_assert_int((int)n, >, 0);
	buf[n] = '\0';
	(void)close(g_nm_dump_fd);
	(void)unlink(tmpl);
	munit_assert_not_null(strstr(buf,
	    "    crit=2    name=engine    "
	    "label=chatcmpl-abc123 "));
	munit_assert_not_null(strstr(buf, "    name=listener\n"));
	return MUNIT_OK;
}

/* Off-loop: xtc_proc_info for an unknown pid is NOTFOUND, not a crash. */
static MunitResult
test_inspect_notfound(const MunitParameter p[], void *d)
{
	xtc_proc_info_t info = XTC_PROC_INFO_INIT;
	xtc_pid_t nobody = { 9, 9, 9 };
	(void)p; (void)d;
	munit_assert_int(xtc_proc_info(nobody, &info), ==, XTC_E_NOTFOUND);
	munit_assert_int(xtc_proc_info(nobody, NULL), ==, XTC_E_INVAL);
	return MUNIT_OK;
}

/* ----- causal tracing (xtc_trace) ----------------------------- */
#include "xtc_trace.h"

static xtc_pid_t g_tr_b, g_tr_c;
static void tr_c_proc(void *a){ void *m=NULL; size_t n=0; (void)a;
	(void)xtc_recv(&m,&n, 500LL*1000*1000); free(m); }
static void tr_b_proc(void *a){ void *m=NULL; size_t n=0; (void)a;
	(void)xtc_recv(&m,&n, 500LL*1000*1000); free(m);
	(void)xtc_send(g_tr_c, "m2", 3); }
static void tr_a_proc(void *a){ (void)a; (void)xtc_send(g_tr_b, "m1", 3); }

#define TR_MAX 256
static xtc_trace_rec_t g_tr[TR_MAX];
static int g_tr_n;
static int tr_collect(const xtc_trace_rec_t *r, void *u){ (void)u;
	if (g_tr_n < TR_MAX) g_tr[g_tr_n++] = *r;
	return 0; }

static MunitResult
test_trace_causal(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t o = XTC_PROC_OPTS_INIT;
	xtc_pid_t ap;
	int dumped, i, j, sends = 0, recvs = 0, edges_ok = 0;
	(void)p; (void)d;

	g_tr_n = 0;
	xtc_trace_reset();
	(void)xtc_trace_enable(1);

	munit_assert_int(xtc_loop_create(&loop), ==, XTC_OK);
	o.name = "C";
	munit_assert_int(xtc_proc_spawn(loop, tr_c_proc, NULL, &o, &g_tr_c), ==, XTC_OK);
	o.name = "B";
	munit_assert_int(xtc_proc_spawn(loop, tr_b_proc, NULL, &o, &g_tr_b), ==, XTC_OK);
	o.name = "A";
	munit_assert_int(xtc_proc_spawn(loop, tr_a_proc, NULL, &o, &ap), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	xtc_loop_destroy(loop);

	dumped = xtc_trace_dump(tr_collect, NULL);
	(void)xtc_trace_enable(0);

	/* The chain A->B->C produced two sends and two receives. */
	munit_assert_int(dumped, >=, 4);
	for (i = 0; i < g_tr_n; i++) {
		if (g_tr[i].kind == XTC_TRACE_SEND) sends++;
		if (g_tr[i].kind == XTC_TRACE_RECV) {
			recvs++;
			/* Every receive links back to a real send. */
			for (j = 0; j < g_tr_n; j++)
				if (g_tr[j].kind == XTC_TRACE_SEND &&
				    g_tr[j].hlc == g_tr[i].cause) {
					edges_ok++;
					break;
				}
		}
	}
	munit_assert_int(sends, >=, 2);
	munit_assert_int(recvs, >=, 2);
	munit_assert_int(edges_ok, ==, recvs);          /* no orphan receives */
	/* Causal order: dump is HLC-ascending, so the last event's stamp
	 * exceeds the first, and the clock advanced. */
	munit_assert_true(g_tr[g_tr_n - 1].hlc > g_tr[0].hlc);
	munit_assert_true(xtc_hlc_now() > 0);
	return MUNIT_OK;
}

/* xtc_pdict_put_with_dtor: the dtor runs on erase/clear/replace. */
static int g_dtor_calls;
static void
pdict_free_dtor(void *v)
{
	(void)v;
	xtc_atomic_fetch_add_i32(&g_dtor_calls, 1, xtc_mo_seq_cst);
}

static int g_pdict_dtor_ok;
static void
pdict_dtor_user(void *arg)
{
	(void)arg;
	/* put_with_dtor, then replace (dtor fires for the old value). */
	if (xtc_pdict_put_with_dtor("k", (void *)(uintptr_t)1,
	    pdict_free_dtor) != XTC_OK) return;
	if (xtc_pdict_put_with_dtor("k", (void *)(uintptr_t)2,
	    pdict_free_dtor) != XTC_OK) return;
	if (xtc_atomic_load_i32(&g_dtor_calls, xtc_mo_seq_cst) != 1)
		return; /* old value freed */
	/* erase fires the dtor for the current value. */
	if (xtc_pdict_erase("k") != XTC_OK) return;
	if (xtc_atomic_load_i32(&g_dtor_calls, xtc_mo_seq_cst) != 2)
		return;
	xtc_atomic_store_i32(&g_pdict_dtor_ok, 1, xtc_mo_seq_cst);
}

static MunitResult
test_pdict_dtor(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_pid_t pid;
	(void)p; (void)d;
	xtc_atomic_store_i32(&g_dtor_calls, 0, xtc_mo_seq_cst);
	xtc_atomic_store_i32(&g_pdict_dtor_ok, 0, xtc_mo_seq_cst);
	munit_assert_int(xtc_loop_create(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, pdict_dtor_user, NULL, NULL, &pid),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_atomic_load_i32(&g_pdict_dtor_ok, xtc_mo_seq_cst),
	    ==, 1);
	xtc_loop_destroy(loop);
	return MUNIT_OK;
}

/*
 * A proc that returns, or is torn down by xtc_loop_destroy, holding pdict
 * entries has them released: each destructor runs once.  Before 2.2
 * nothing released them (0 of PD_EXIT_PROCS destructors ran).
 */
#define PD_EXIT_PROCS 3
static int g_pd_exit_dtors;
static void
pd_exit_dtor(void *v)
{
	(void)v;
	xtc_atomic_fetch_add_i32(&g_pd_exit_dtors, 1, xtc_mo_seq_cst);
}
static void
pd_exit_returner(void *arg)
{
	(void)arg;
	(void)xtc_pdict_put_with_dtor("ctx", NULL, pd_exit_dtor);
	(void)xtc_pdict_put_with_dtor("span", NULL, pd_exit_dtor);
}
static void
pd_exit_sleeper(void *arg)
{
	(void)arg;
	(void)xtc_pdict_put_with_dtor("ctx", NULL, pd_exit_dtor);
	(void)xtc_loop_stop((xtc_loop_t *)arg);
	(void)xtc_proc_sleep((int64_t)3600 * 1000000000);
}

static MunitResult
test_pdict_exit_cleanup(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	int i;
	(void)p;
	(void)d;
	xtc_atomic_store_i32(&g_pd_exit_dtors, 0, xtc_mo_seq_cst);
	munit_assert_int(xtc_loop_create(&loop), ==, XTC_OK);
	for (i = 0; i < PD_EXIT_PROCS; i++)
		munit_assert_int(xtc_proc_spawn(loop, pd_exit_returner, NULL,
		                     NULL, NULL),
		    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_atomic_load_i32(&g_pd_exit_dtors, xtc_mo_seq_cst),
	    ==, 2 * PD_EXIT_PROCS);

	/* A proc still parked when the loop is finalized. */
	munit_assert_int(xtc_proc_spawn(loop, pd_exit_sleeper, loop, NULL,
	                     NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_atomic_load_i32(&g_pd_exit_dtors, xtc_mo_seq_cst),
	    ==, 2 * PD_EXIT_PROCS);
	xtc_loop_destroy(loop);
	munit_assert_int(xtc_atomic_load_i32(&g_pd_exit_dtors, xtc_mo_seq_cst),
	    ==, 2 * PD_EXIT_PROCS + 1);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/pdict/basic",          test_pdict_basic,          NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/inspect/procs",        test_inspect_procs,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/inspect/park_reason",  test_inspect_park_reason,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/inspect/notfound",     test_inspect_notfound,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/inspect/name_label_crit", test_inspect_name_label_crit, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/trace/causal",         test_trace_causal,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/log/basic",            test_log_basic,            NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/log/drop_on_full",     test_log_drop_on_full,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/log/mpsc",             test_log_mpsc,             NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/log/default_vwrite",   test_log_default_vwrite,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/logf/types",           test_logf_types,           NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/logf/values",          test_logf_values,          NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/logf/specs",           test_logf_specs,           NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/logf/counts",          test_logf_counts,          NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/logf/ring",            test_logf_ring,            NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/cfg/int",              test_cfg_int,              NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/cfg/string",           test_cfg_string,           NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/cfg/load_file",        test_cfg_load_file,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/inject/callback",      test_inject_callback,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/inject/wait",          test_inject_wait,          NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/pdict/outside_proc",   test_pdict_outside_proc,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/pdict/dtor",           test_pdict_dtor,           NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/pdict/exit_cleanup",   test_pdict_exit_cleanup,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/res/alert",            test_res_alert,            NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m12/observability", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
