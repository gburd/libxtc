/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m12/test_trace_causal.c -- A3 async causal trace: the per-fiber
 * ring of suspend/resume boundaries (xtc_trace_causal_enable /
 * xtc_trace_causal_dump) and its splice into xtc_dump.
 *
 * Each worker recvs FOREVER so it stays alive and parked while the
 * driver reads its causal chain (a proc that has exited is reaped and
 * xtc_trace_causal_dump returns XTC_E_NOTFOUND); the driver kills the
 * worker to end the loop.
 */

#define _GNU_SOURCE

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_dump.h"
#include "xtc_trace.h"

/* ---- a recorder callback: collect a proc's causal chain in order ---- */

#define MAXREC 64
struct collect {
	int  n;
	int  kind[MAXREC];
	char site[MAXREC][32];
};

static int
collect_cb(const xtc_causal_rec_t *rec, void *user)
{
	struct collect *c = user;
	if (c->n < MAXREC) {
		c->kind[c->n] = rec->kind;
		snprintf(c->site[c->n], sizeof c->site[c->n], "%s",
		    rec->site != NULL ? rec->site : "?");
		c->n++;
	}
	return 0;
}

/* A worker that parks on recv forever (stays alive/parked to be read). */
static void
recv_forever(void *a)
{
	void *msg = NULL;
	size_t sz = 0;
	(void)a;
	for (;;) {
		if (xtc_recv(&msg, &sz, -1) == XTC_OK)
			xtc_free(msg);
	}
}

/* ---------- /enable/toggle: enable returns previous state ---------- */

static MunitResult
test_enable_toggle(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	(void)xtc_trace_causal_enable(0);
	munit_assert_int(xtc_trace_causal_enable(1), ==, 0);  /* was off */
	munit_assert_int(xtc_trace_causal_enable(1), ==, 1);  /* was on */
	munit_assert_int(xtc_trace_causal_enable(0), ==, 1);  /* was on */
	munit_assert_int(xtc_trace_causal_enable(0), ==, 0);  /* was off */
	return MUNIT_OK;
}

/* ---------- /records/order: the ring records parks/resumes in order --- */

static xtc_pid_t g_worker;
static struct collect g_collect;

static void
order_driver(void *a)
{
	const char m = 'x';
	int i;
	(void)a;
	/* Deliver three messages, each after a delay so the worker parks
	 * (PARK_MAILBOX) then resumes (RESUME) each time.  After the third
	 * is consumed the worker re-parks a fourth time. */
	for (i = 0; i < 3; i++) {
		(void)xtc_proc_sleep(3LL * 1000 * 1000);
		(void)xtc_send(g_worker, &m, 1);
	}
	(void)xtc_proc_sleep(5LL * 1000 * 1000);
	memset(&g_collect, 0, sizeof g_collect);
	(void)xtc_trace_causal_dump(g_worker, collect_cb, &g_collect);
	(void)xtc_exit_pid(g_worker, 0);
}

static MunitResult
test_records_order(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t o = { 0 };
	xtc_pid_t dpid;
	int i;
	(void)p; (void)d;

	(void)xtc_trace_causal_enable(1);

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, recv_forever, NULL, &o,
	    &g_worker), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, order_driver, NULL, &o, &dpid),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	(void)xtc_loop_fini(loop);

	(void)xtc_trace_causal_enable(0);

	/* PARK,RESUME x3 then a trailing PARK: 7 records, exact order. */
	munit_assert_int(g_collect.n, ==, 7);
	for (i = 0; i + 1 < g_collect.n; i += 2) {
		munit_assert_int(g_collect.kind[i], ==,
		    XTC_CAUSAL_PARK_MAILBOX);
		munit_assert_int(g_collect.kind[i + 1], ==, XTC_CAUSAL_RESUME);
	}
	munit_assert_int(g_collect.kind[6], ==, XTC_CAUSAL_PARK_MAILBOX);
	/* The site label is the recv park function (__func__ at the site). */
	munit_assert_string_equal(g_collect.site[0], "__do_recv");
	return MUNIT_OK;
}

/* ---------- /disabled/zero: nothing recorded when the trace is off --- */

static struct collect g_collect2;

static void
disabled_driver(void *a)
{
	const char m = 'x';
	(void)a;
	(void)xtc_proc_sleep(3LL * 1000 * 1000);
	(void)xtc_send(g_worker, &m, 1);    /* worker parks + resumes once */
	(void)xtc_proc_sleep(5LL * 1000 * 1000);
	memset(&g_collect2, 0, sizeof g_collect2);
	(void)xtc_trace_causal_dump(g_worker, collect_cb, &g_collect2);
	(void)xtc_exit_pid(g_worker, 0);
}

static MunitResult
test_disabled_zero(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t o = { 0 };
	xtc_pid_t dpid;
	(void)p; (void)d;

	(void)xtc_trace_causal_enable(0);   /* off (the default) */

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, recv_forever, NULL, &o,
	    &g_worker), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, disabled_driver, NULL, &o, &dpid),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	(void)xtc_loop_fini(loop);

	/* Trace disabled: the fiber parked/resumed but recorded nothing. */
	munit_assert_int(g_collect2.n, ==, 0);
	return MUNIT_OK;
}

/* ---------- /ring/evict: capacity bounded, oldest evicted first ------ */

static xtc_pid_t g_spinner;
static struct collect g_collect3;

static void
ring_driver(void *a)
{
	const char m = 'x';
	int i;
	(void)a;
	/* Deliver 20 messages (40+ boundaries), each after a beat so the
	 * spinner parks and resumes each time -- far more than the ring
	 * capacity of 16. */
	for (i = 0; i < 20; i++) {
		(void)xtc_proc_sleep(1LL * 1000 * 1000);
		(void)xtc_send(g_spinner, &m, 1);
	}
	(void)xtc_proc_sleep(5LL * 1000 * 1000);
	memset(&g_collect3, 0, sizeof g_collect3);
	(void)xtc_trace_causal_dump(g_spinner, collect_cb, &g_collect3);
	(void)xtc_exit_pid(g_spinner, 0);
}

static MunitResult
test_ring_evict(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t o = { 0 };
	xtc_pid_t dpid;
	(void)p; (void)d;

	(void)xtc_trace_causal_enable(1);

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, recv_forever, NULL, &o,
	    &g_spinner), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, ring_driver, NULL, &o, &dpid),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	(void)xtc_loop_fini(loop);

	(void)xtc_trace_causal_enable(0);

	/* Ring capacity is 16: the trace holds at most the last 16 events,
	 * never more, even though 40+ boundaries happened. */
	munit_assert_int(g_collect3.n, ==, 16);
	return MUNIT_OK;
}

/* ---------- /dump/splice: xtc_dump prints the causal chain ---------- */

static xtc_pid_t g_dworker;
static int g_dumpfd;

static void
dump_driver(void *a)
{
	const char m = 'x';
	(void)a;
	(void)xtc_proc_sleep(3LL * 1000 * 1000);
	(void)xtc_send(g_dworker, &m, 1);   /* worker parks + resumes */
	(void)xtc_proc_sleep(5LL * 1000 * 1000);
	xtc_dump(g_dumpfd);                 /* worker still alive + parked */
	(void)xtc_exit_pid(g_dworker, 0);
}

static MunitResult
test_dump_splice(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t o = { 0 };
	xtc_pid_t dpid;
	char tmpl[] = "/tmp/xtc_ct_XXXXXX";
	int fd, n;
	char buf[8192];
	(void)p; (void)d;

	fd = mkstemp(tmpl);
	munit_assert_int(fd, >=, 0);
	g_dumpfd = fd;

	(void)xtc_trace_causal_enable(1);

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, recv_forever, NULL, &o,
	    &g_dworker), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, dump_driver, NULL, &o, &dpid),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	(void)xtc_loop_fini(loop);

	(void)xtc_trace_causal_enable(0);

	munit_assert_int(lseek(fd, 0, SEEK_SET), ==, 0);
	n = (int)read(fd, buf, sizeof buf - 1);
	munit_assert_int(n, >, 0);
	buf[n] = '\0';
	close(fd);
	unlink(tmpl);

	/* The dump carries the causal splice line for the worker. */
	munit_assert_not_null(strstr(buf, "causal:"));
	munit_assert_not_null(strstr(buf, "park:mailbox@__do_recv"));
	munit_assert_not_null(strstr(buf, "resume@__do_recv"));
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/enable/toggle",  test_enable_toggle,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/records/order",  test_records_order,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/disabled/zero",  test_disabled_zero,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/ring/evict",     test_ring_evict,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/dump/splice",    test_dump_splice,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite suite = {
	"/trace_causal", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

int
main(int argc, char **argv)
{
	return munit_suite_main(&suite, NULL, argc, argv);
}
