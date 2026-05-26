/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the PostgreSQL License.
 *
 * test/m12/test_observability.c — verifies the four new
 * observability modules: log, cfg, inject, pdict.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_log.h"
#include "xtc_cfg.h"
#include "xtc_inject.h"
#include "xtc_pdict.h"
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
	xtc_log_opts_t opts = XTC_LOG_OPTS_DEFAULT;
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
	xtc_log_opts_t opts = XTC_LOG_OPTS_DEFAULT;
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
	munit_assert_int(xtc_log_drop_count(log), >, 0);
	(void)xtc_log_drain(log);
	xtc_log_destroy(log);
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
	xtc_cfg_spec_t spec = {0};
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
	xtc_cfg_spec_t spec = {0};
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

/* ----- Injection points -------------------------------------- */

static _Atomic int g_inject_fired;

static void
inject_cb(const char *name, void *user)
{
	(void)name; (void)user;
	atomic_fetch_add(&g_inject_fired, 1);
}

static MunitResult
test_inject_callback(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	atomic_store(&g_inject_fired, 0);

	/* No attachment yet — trigger is a no-op. */
	xtc_inject_trigger("xtc.test.point1");
	munit_assert_int(atomic_load(&g_inject_fired), ==, 0);

	/* Attach + trigger fires. */
	munit_assert_int(xtc_inject_attach("xtc.test.point1", inject_cb, NULL),
	    ==, XTC_OK);
	xtc_inject_trigger("xtc.test.point1");
	xtc_inject_trigger("xtc.test.point1");
	munit_assert_int(atomic_load(&g_inject_fired), ==, 2);

	munit_assert_int(xtc_inject_detach("xtc.test.point1"), ==, XTC_OK);
	xtc_inject_trigger("xtc.test.point1");
	munit_assert_int(atomic_load(&g_inject_fired), ==, 2);
	return MUNIT_OK;
}

/* Wait-style: trigger blocks until wakeup is called from a peer. */
static _Atomic int g_wait_phase;

static void *
wait_trigger_thread(void *arg)
{
	(void)arg;
	atomic_store(&g_wait_phase, 1);
	xtc_inject_trigger("xtc.test.wait_point");
	atomic_store(&g_wait_phase, 2);
	return NULL;
}

static MunitResult
test_inject_wait(const MunitParameter p[], void *d)
{
	pthread_t th;
	struct timespec ts = { 0, 100 * 1000 * 1000 };  /* 100ms */
	(void)p; (void)d;
	atomic_store(&g_wait_phase, 0);
	munit_assert_int(xtc_inject_attach_wait("xtc.test.wait_point"),
	    ==, XTC_OK);

	pthread_create(&th, NULL, wait_trigger_thread, NULL);

	/* Phase 1: trigger thread reaches and blocks.  Sleep 100ms;
	 * phase should still be 1. */
	(void)nanosleep(&ts, NULL);
	munit_assert_int(atomic_load(&g_wait_phase), ==, 1);

	/* Wake it up. */
	munit_assert_int(xtc_inject_wakeup("xtc.test.wait_point"), ==, XTC_OK);
	pthread_join(th, NULL);
	munit_assert_int(atomic_load(&g_wait_phase), ==, 2);

	(void)xtc_inject_detach("xtc.test.wait_point");
	return MUNIT_OK;
}

/* ----- Proc dictionary --------------------------------------- */

static _Atomic int g_pdict_test_ok;

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

	atomic_store(&g_pdict_test_ok, 1);
}

static MunitResult
test_pdict_basic(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_pid_t pid;
	(void)p; (void)d;
	atomic_store(&g_pdict_test_ok, 0);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, pdict_user, NULL, NULL, &pid),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(atomic_load(&g_pdict_test_ok), ==, 1);
	(void)xtc_loop_fini(loop);
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

static _Atomic int g_alert_count;
static xtc_res_kind_t g_last_kind;

static void
alert_cb(xtc_res_kind_t k, int64_t used, int64_t cap, void *user)
{
	(void)used; (void)cap; (void)user;
	g_last_kind = k;
	atomic_fetch_add(&g_alert_count, 1);
}

static MunitResult
test_res_alert(const MunitParameter p[], void *d)
{
	xtc_res_t r;
	xtc_res_caps_t caps = XTC_RES_CAPS_DEFAULT;
	(void)p; (void)d;
	caps.tasks = 100;
	munit_assert_int(xtc_res_init(&r, &caps), ==, XTC_OK);
	munit_assert_int(xtc_res_set_alert(&r, XTC_RES_TASKS, 0.8), ==, XTC_OK);
	munit_assert_int(xtc_res_set_alert_fn(&r, alert_cb, NULL), ==, XTC_OK);
	atomic_store(&g_alert_count, 0);

	/* Acquire 70 — under threshold, no fire. */
	munit_assert_int(xtc_res_acquire(&r, XTC_RES_TASKS, 70), ==, XTC_OK);
	munit_assert_int(atomic_load(&g_alert_count), ==, 0);
	/* Push to 85 — fires once. */
	munit_assert_int(xtc_res_acquire(&r, XTC_RES_TASKS, 15), ==, XTC_OK);
	munit_assert_int(atomic_load(&g_alert_count), ==, 1);
	/* Stay above; no re-fire. */
	munit_assert_int(xtc_res_acquire(&r, XTC_RES_TASKS, 5), ==, XTC_OK);
	munit_assert_int(atomic_load(&g_alert_count), ==, 1);
	/* Drop below threshold and back up — re-arms and fires again. */
	xtc_res_release(&r, XTC_RES_TASKS, 30);
	munit_assert_int(xtc_res_acquire(&r, XTC_RES_TASKS, 30), ==, XTC_OK);
	munit_assert_int(atomic_load(&g_alert_count), ==, 2);
	munit_assert_int(g_last_kind, ==, XTC_RES_TASKS);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/pdict/basic",          test_pdict_basic,          NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/log/basic",            test_log_basic,            NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/log/drop_on_full",     test_log_drop_on_full,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/cfg/int",              test_cfg_int,              NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/cfg/string",           test_cfg_string,           NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/inject/callback",      test_inject_callback,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/inject/wait",          test_inject_wait,          NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/pdict/outside_proc",   test_pdict_outside_proc,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/res/alert",            test_res_alert,            NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m12/observability", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
