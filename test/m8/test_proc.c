/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the PostgreSQL License.
 *
 * test/m8/test_proc.c — verifies M8 process + mailbox + selective receive.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_async.h"
#include "xtc_proc.h"
#include "xtc_int.h"

/* Helper: a "shared" pointer the test main thread uses to read out
 * results from the proc body.  Each test owns its own. */

/* ---------- basic send/recv ---------- */

struct echo_state { int got; int value; };
static void
echo_proc(void *arg)
{
	struct echo_state *s = arg;
	void *msg; size_t sz;
	int rc = xtc_recv(&msg, &sz, 5LL * 1000 * 1000 * 1000);
	if (rc != XTC_OK) return;
	if (sz == sizeof(int)) s->value = *(int *)msg;
	s->got = 1;
	__os_free(msg);
}

static MunitResult
test_send_recv_basic(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	struct echo_state s = {0, 0};
	xtc_pid_t pid;
	int payload = 42;
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, echo_proc, &s, NULL, &pid),
	    ==, XTC_OK);
	munit_assert_int(xtc_send(pid, &payload, sizeof payload), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(s.got, ==, 1);
	munit_assert_int(s.value, ==, 42);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* ---------- selective receive ---------- */

struct sel_state {
	int order_seen[5];
	int n;
};

static int
__match_42(const void *data, size_t size, void *u)
{
	(void)u;
	if (size != sizeof(int)) return 0;
	return *(const int *)data == 42;
}

static int
__match_any(const void *data, size_t size, void *u)
{
	(void)data; (void)size; (void)u;
	return 1;
}

static void
selective_proc(void *arg)
{
	struct sel_state *s = arg;
	void *msg; size_t sz; int rc;
	int v;
	/* First grab the "42" message specifically, even though it's
	 * not the first in the mailbox. */
	rc = xtc_recv_match(__match_42, NULL, &msg, &sz, 1000LL * 1000 * 1000);
	if (rc != XTC_OK) return;
	v = *(int *)msg;
	s->order_seen[s->n++] = v;
	__os_free(msg);

	/* Then drain the rest in order. */
	while (s->n < 5) {
		rc = xtc_recv_match(__match_any, NULL, &msg, &sz,
		    100LL * 1000 * 1000);
		if (rc != XTC_OK) break;
		v = *(int *)msg;
		s->order_seen[s->n++] = v;
		__os_free(msg);
	}
}

static MunitResult
test_selective_receive(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	struct sel_state s; memset(&s, 0, sizeof s);
	xtc_pid_t pid;
	int sent[5] = { 1, 2, 42, 3, 4 };
	int i;
	(void)p; (void)d;

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, selective_proc, &s, NULL, &pid),
	    ==, XTC_OK);
	for (i = 0; i < 5; i++)
		munit_assert_int(xtc_send(pid, &sent[i], sizeof(int)), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	/* Order: 42 came out first (selective); then 1, 2, 3, 4 in
	 * arrival order from the save queue / remaining mailbox. */
	munit_assert_int(s.n, ==, 5);
	munit_assert_int(s.order_seen[0], ==, 42);
	munit_assert_int(s.order_seen[1], ==, 1);
	munit_assert_int(s.order_seen[2], ==, 2);
	munit_assert_int(s.order_seen[3], ==, 3);
	munit_assert_int(s.order_seen[4], ==, 4);

	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* ---------- xtc_self ---------- */

struct self_state { xtc_pid_t mine; };
static void
self_proc(void *arg)
{
	struct self_state *s = arg;
	s->mine = xtc_self();
}

static MunitResult
test_self(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	struct self_state s = {{0,0,0}};
	xtc_pid_t pid;
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, self_proc, &s, NULL, &pid),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_true(xtc_pid_eq(s.mine, pid));
	/* From outside any proc, xtc_self returns NONE. */
	munit_assert_true(xtc_pid_is_none(xtc_self()));
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* ---------- monitor ---------- */

struct mon_state { int saw_down; int reason; };

static void
mon_target(void *arg)
{
	struct mon_state *s = arg;
	(void)s;
	xtc_exit_self(7);   /* exits with reason 7 */
}

static void
mon_watcher(void *arg)
{
	struct mon_state *s = arg;
	void *msg; size_t sz;
	int rc;
	struct {
		uint8_t kind; uint64_t ref; xtc_pid_t pid; int reason;
	} __attribute__((packed)) *down;
	xtc_pid_t target_pid;
	uint64_t ref;

	/* The watcher gets the target's pid via the first message. */
	rc = xtc_recv(&msg, &sz, 1000LL * 1000 * 1000);
	if (rc != XTC_OK) return;
	memcpy(&target_pid, msg, sizeof target_pid);
	__os_free(msg);

	if (xtc_monitor(target_pid, &ref) != XTC_OK) return;

	/* Wait for the DOWN signal. */
	rc = xtc_recv(&msg, &sz, 5LL * 1000 * 1000 * 1000);
	if (rc != XTC_OK) return;
	if (sz >= sizeof *down) {
		down = msg;
		if (down->kind == 'D') {
			s->saw_down = 1;
			s->reason = down->reason;
		}
	}
	__os_free(msg);
}

static MunitResult
test_monitor(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	struct mon_state s = {0, 0};
	xtc_pid_t target, watcher;
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	/* Spawn watcher first, then target.  Tell watcher about target via msg. */
	munit_assert_int(xtc_proc_spawn(loop, mon_watcher, &s, NULL, &watcher),
	    ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, mon_target, &s, NULL, &target),
	    ==, XTC_OK);
	munit_assert_int(xtc_send(watcher, &target, sizeof target), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(s.saw_down, ==, 1);
	munit_assert_int(s.reason, ==, 7);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/send_recv_basic",   test_send_recv_basic,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/self",              test_self,             NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/selective_receive", test_selective_receive,NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/monitor",           test_monitor,          NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m8/proc", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
