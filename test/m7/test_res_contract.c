/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m7/test_res_contract.c -- PLAN 19.27.13 res/slab contract probes.
 *
 *	Each case is the probe line that found the inconsistency:
 *	  - xtc_res_set_alert accepted pct 0 and 1.0 (documented open
 *	    interval (0.0, 1.0));
 *	  - xtc_res_set_cap(-5) silently made the kind unbounded;
 *	  - xtc_sleep_ns(-5) returned an undocumented -1 (it is
 *	    XTC_E_INVAL; pinned here so the documented code cannot drift);
 *	  - an xtc_slab redzone violation wrote to the process's stderr
 *	    from library code instead of going through xtc_log.
 */

#include <math.h>     /* NAN */
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_res.h"
#include "xtc_slab.h"
#include "xtc_log.h"

static MunitResult
test_alert_open_interval(const MunitParameter p[], void *d)
{
	xtc_res_t r;
	(void)p; (void)d;
	munit_assert_int(xtc_res_init(&r, NULL), ==, XTC_OK);
	munit_assert_int(xtc_res_set_alert(&r, XTC_RES_TASKS, 0.0), ==,
	    XTC_E_INVAL);
	munit_assert_int(xtc_res_set_alert(&r, XTC_RES_TASKS, 1.0), ==,
	    XTC_E_INVAL);
	munit_assert_int(xtc_res_set_alert(&r, XTC_RES_TASKS, NAN), ==,
	    XTC_E_INVAL);
	munit_assert_int(xtc_res_set_alert(&r, XTC_RES_TASKS, 0.001), ==,
	    XTC_OK);
	munit_assert_int(xtc_res_set_alert(&r, XTC_RES_TASKS, 0.999), ==,
	    XTC_OK);
	return MUNIT_OK;
}

static MunitResult
test_set_cap_negative(const MunitParameter p[], void *d)
{
	xtc_res_t r;
	xtc_res_caps_t caps = XTC_RES_CAPS_DEFAULT;
	(void)p; (void)d;
	caps.tasks = 10;
	munit_assert_int(xtc_res_init(&r, &caps), ==, XTC_OK);
	xtc_res_set_cap(&r, XTC_RES_TASKS, -5);   /* ignored: cap stays 10 */
	munit_assert_int(xtc_res_acquire(&r, XTC_RES_TASKS, 10), ==, XTC_OK);
	munit_assert_int(xtc_res_acquire(&r, XTC_RES_TASKS, 1), ==,
	    XTC_E_RESOURCE);
	xtc_res_set_cap(&r, XTC_RES_TASKS, 0);    /* 0 = deliberate no cap */
	munit_assert_int(xtc_res_acquire(&r, XTC_RES_TASKS, 1000), ==, XTC_OK);
	return MUNIT_OK;
}

static MunitResult
test_sleep_negative(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	munit_assert_int(xtc_sleep_ns(-5), ==, XTC_E_INVAL);
	munit_assert_int(xtc_sleep_ns(0), ==, XTC_OK);
	return MUNIT_OK;
}

static int g_rz_lines;
static char g_rz_line[256];
static int
rz_sink(void *user, xtc_log_level_t lvl, const char *buf, size_t len)
{
	(void)user;
	if (lvl == XTC_LOG_ERROR && len < sizeof g_rz_line) {
		memcpy(g_rz_line, buf, len);
		g_rz_line[len] = '\0';
		g_rz_lines++;
	}
	return 0;
}

static MunitResult
test_redzone_via_log(const MunitParameter p[], void *d)
{
	xtc_slab_t *s;
	xtc_slab_opts_t opts = XTC_SLAB_OPTS_DEFAULT;
	xtc_log_opts_t lo = XTC_LOG_OPTS_DEFAULT;
	xtc_log_t *log;
	xtc_slab_stats_t st;
	uint8_t *o;
	int errpipe[2], saved, n;
	char buf[256];
	(void)p; (void)d;
#if defined(_WIN32)
	return MUNIT_SKIP;   /* POSIX stderr capture (pipe/dup2) */
#endif

	/* Capture the process's stderr, so a raw library fprintf is seen. */
	munit_assert_int(pipe(errpipe), ==, 0);
	saved = dup(2);
	munit_assert_int(saved, >=, 0);
	munit_assert_int(dup2(errpipe[1], 2), ==, 2);

	/* 1. No logger installed: the violation is counted, and the
	 *    library writes NOTHING to stderr. */
	opts.name = "rzlog"; opts.obj_size = 16; opts.flags = XTC_SLAB_REDZONE;
	munit_assert_int(xtc_slab_create(&opts, &s), ==, XTC_OK);
	o = xtc_slab_alloc(s);
	munit_assert_not_null(o);
	memset(o, 'y', 24);               /* 8 bytes past the end */
	xtc_slab_free(s, o);

	/* Restore stderr before asserting (munit reports through it). */
	(void)dup2(saved, 2);
	(void)close(saved);
	(void)close(errpipe[1]);
	n = (int)read(errpipe[0], buf, sizeof buf - 1);
	(void)close(errpipe[0]);
	(void)xtc_slab_stat(s, &st);
	munit_assert_uint64(st.redzone_violations, ==, 1);
	munit_assert_int(n, ==, 0);

	/* 2. With a logger installed, the report arrives through it. */
	lo.sink = rz_sink;
	lo.sink_fd = -1;
	munit_assert_int(xtc_log_create(&lo, &log), ==, XTC_OK);
	munit_assert_int(xtc_log_set_default(log), ==, XTC_OK);
	g_rz_lines = 0;
	o = xtc_slab_alloc(s);
	munit_assert_not_null(o);
	memset(o, 'y', 24);
	xtc_slab_free(s, o);
	(void)xtc_log_drain(log);
	munit_assert_int(g_rz_lines, ==, 1);
	munit_assert_not_null(strstr(g_rz_line, "xtc_slab[rzlog]: redzone violation"));
	(void)xtc_log_set_default(NULL);
	xtc_log_destroy(log);
	xtc_slab_destroy(s);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/alert_open_interval", test_alert_open_interval, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/set_cap_negative",    test_set_cap_negative,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/sleep_negative",      test_sleep_negative,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/redzone_via_log",     test_redzone_via_log,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m7/res_contract", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
