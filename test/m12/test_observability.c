/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m12/test_observability.c -- verifies the four new
 * observability modules: log, cfg, inject, pdict.
 */

#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
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
	xtc_log_t *log, *saved;
	xtc_log_opts_t opts = XTC_LOG_OPTS_DEFAULT;
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

	/* Restore the process default before destroying ours. */
	munit_assert_int(xtc_log_set_default(saved), ==, XTC_OK);
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

/* ----- Config-file loading + reload --------------------------- */

static MunitResult
test_cfg_load_file(const MunitParameter p[], void *d)
{
	xtc_cfg_spec_t spec = {0};
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
	spec.name = "t.tls"; spec.kind = XTC_CFG_BOOL; spec.dflt.d_bool = 0;
	munit_assert_int(xtc_cfg_register(&spec), ==, XTC_OK);
	memset(&spec, 0, sizeof spec);
	spec.name = "t.banner"; spec.kind = XTC_CFG_STRING; spec.dflt.d_string = "x";
	munit_assert_int(xtc_cfg_register(&spec), ==, XTC_OK);
	memset(&spec, 0, sizeof spec);
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

	/* No attachment yet -- trigger is a no-op. */
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

	/* Acquire 70 -- under threshold, no fire. */
	munit_assert_int(xtc_res_acquire(&r, XTC_RES_TASKS, 70), ==, XTC_OK);
	munit_assert_int(atomic_load(&g_alert_count), ==, 0);
	/* Push to 85 -- fires once. */
	munit_assert_int(xtc_res_acquire(&r, XTC_RES_TASKS, 15), ==, XTC_OK);
	munit_assert_int(atomic_load(&g_alert_count), ==, 1);
	/* Stay above; no re-fire. */
	munit_assert_int(xtc_res_acquire(&r, XTC_RES_TASKS, 5), ==, XTC_OK);
	munit_assert_int(atomic_load(&g_alert_count), ==, 1);
	/* Drop below threshold and back up -- re-arms and fires again. */
	xtc_res_release(&r, XTC_RES_TASKS, 30);
	munit_assert_int(xtc_res_acquire(&r, XTC_RES_TASKS, 30), ==, XTC_OK);
	munit_assert_int(atomic_load(&g_alert_count), ==, 2);
	munit_assert_int(g_last_kind, ==, XTC_RES_TASKS);
	return MUNIT_OK;
}

/* ----- live introspection (xtc_inspect) ----------------------- */
#include "xtc_inspect.h"

static _Atomic int g_insp_seen;        /* procs the callback saw */
static _Atomic int g_insp_sleeper_mbox;/* mailbox depth seen on the sleeper */
static _Atomic int g_insp_loop_procs;  /* procs counted via xtc_inspect_loops */
static xtc_pid_t   g_insp_sleeper;

static int
insp_proc_cb(const xtc_proc_info_t *info, void *user)
{
	(void)user;
	atomic_fetch_add(&g_insp_seen, 1);
	if (xtc_pid_eq(info->pid, g_insp_sleeper))
		atomic_store(&g_insp_sleeper_mbox, (int)info->mbox_len);
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
		atomic_store(&g_insp_loop_procs, lp);
	}
	{
		xtc_proc_info_t info;
		int rc = xtc_proc_info(g_insp_sleeper, &info);
		if (rc == XTC_OK && info.mbox_len == 2 &&
		    info.run_state == XTC_PROC_PARKED)
			atomic_fetch_add(&g_insp_seen, 1000);   /* sentinel: per-pid ok */
	}
	/* The sleeper wakes on its own (~300ms); no need to kill it. */
}

static MunitResult
test_inspect_procs(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t o = { 0 };
	xtc_pid_t dpid;
	(void)p; (void)d;

	atomic_store(&g_insp_seen, 0);
	atomic_store(&g_insp_sleeper_mbox, -1);

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	o.name = "sleeper";
	munit_assert_int(xtc_proc_spawn(loop, insp_sleeper, NULL, &o,
	    &g_insp_sleeper), ==, XTC_OK);
	o.name = "driver";
	munit_assert_int(xtc_proc_spawn(loop, insp_driver, NULL, &o, &dpid),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	/* The driver enumerated at least the two procs, saw the sleeper's
	 * mailbox at depth 2, and the per-pid lookup matched (sentinel). */
	munit_assert_int(atomic_load(&g_insp_seen), >=, 1002);
	munit_assert_int(atomic_load(&g_insp_sleeper_mbox), ==, 2);
	munit_assert_int(atomic_load(&g_insp_loop_procs), >=, 2);

	(void)xtc_loop_fini(loop);
	return MUNIT_OK;
}

/* Off-loop: xtc_proc_info for an unknown pid is NOTFOUND, not a crash. */
static MunitResult
test_inspect_notfound(const MunitParameter p[], void *d)
{
	xtc_proc_info_t info;
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
	xtc_proc_opts_t o = { 0 };
	xtc_pid_t ap;
	int dumped, i, j, sends = 0, recvs = 0, edges_ok = 0;
	(void)p; (void)d;

	g_tr_n = 0;
	xtc_trace_reset();
	(void)xtc_trace_enable(1);

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	o.name = "C";
	munit_assert_int(xtc_proc_spawn(loop, tr_c_proc, NULL, &o, &g_tr_c), ==, XTC_OK);
	o.name = "B";
	munit_assert_int(xtc_proc_spawn(loop, tr_b_proc, NULL, &o, &g_tr_b), ==, XTC_OK);
	o.name = "A";
	munit_assert_int(xtc_proc_spawn(loop, tr_a_proc, NULL, &o, &ap), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);

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
static _Atomic int g_dtor_calls;
static void
pdict_free_dtor(void *v)
{
	(void)v;
	atomic_fetch_add(&g_dtor_calls, 1);
}

static _Atomic int g_pdict_dtor_ok;
static void
pdict_dtor_user(void *arg)
{
	(void)arg;
	/* put_with_dtor, then replace (dtor fires for the old value). */
	if (xtc_pdict_put_with_dtor("k", (void *)(uintptr_t)1,
	    pdict_free_dtor) != XTC_OK) return;
	if (xtc_pdict_put_with_dtor("k", (void *)(uintptr_t)2,
	    pdict_free_dtor) != XTC_OK) return;
	if (atomic_load(&g_dtor_calls) != 1) return;   /* old value freed */
	/* erase fires the dtor for the current value. */
	if (xtc_pdict_erase("k") != XTC_OK) return;
	if (atomic_load(&g_dtor_calls) != 2) return;
	atomic_store(&g_pdict_dtor_ok, 1);
}

static MunitResult
test_pdict_dtor(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_pid_t pid;
	(void)p; (void)d;
	atomic_store(&g_dtor_calls, 0);
	atomic_store(&g_pdict_dtor_ok, 0);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, pdict_dtor_user, NULL, NULL, &pid),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(atomic_load(&g_pdict_dtor_ok), ==, 1);
	(void)xtc_loop_fini(loop);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/pdict/basic",          test_pdict_basic,          NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/inspect/procs",        test_inspect_procs,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/inspect/notfound",     test_inspect_notfound,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/trace/causal",         test_trace_causal,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/log/basic",            test_log_basic,            NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/log/drop_on_full",     test_log_drop_on_full,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/log/default_vwrite",   test_log_default_vwrite,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/cfg/int",              test_cfg_int,              NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/cfg/string",           test_cfg_string,           NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/cfg/load_file",        test_cfg_load_file,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/inject/callback",      test_inject_callback,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/inject/wait",          test_inject_wait,          NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/pdict/outside_proc",   test_pdict_outside_proc,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/pdict/dtor",           test_pdict_dtor,           NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/res/alert",            test_res_alert,            NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m12/observability", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
