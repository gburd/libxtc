/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m10/test_pg.c
 *	R6 process groups (xtc_pg_join/leave/members/send) over the
 *	duplicate-key registry.  A group of subscriber procs each parks in
 *	xtc_recv; the test joins their pids to a group and broadcasts, then
 *	asserts every subscriber received exactly the broadcast.
 */

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_reg.h"
#include "xtc_pg.h"
#include "xtc_int.h"

/* ---------- membership (no procs) ---------- */

static int
count_cb(xtc_pid_t pid, void *user)
{
	int *n = user;
	(void)pid;
	(*n)++;
	return 0;
}

static MunitResult
test_pg_membership(const MunitParameter p[], void *d)
{
	xtc_reg_t *r = NULL;
	xtc_pid_t a = { 0, 1, 1 }, b = { 0, 2, 1 }, c = { 0, 3, 1 };
	int n;
	(void)p; (void)d;
	munit_assert_int(xtc_reg_create(&r), ==, XTC_OK);

	munit_assert_int(xtc_pg_join(r, "g", a), ==, XTC_OK);
	munit_assert_int(xtc_pg_join(r, "g", b), ==, XTC_OK);
	munit_assert_int(xtc_pg_join(r, "g", c), ==, XTC_OK);
	munit_assert_int(xtc_pg_join(r, "g", a), ==, XTC_OK);  /* idempotent */
	n = 0;
	munit_assert_int(xtc_pg_members(r, "g", count_cb, &n), ==, 3);
	munit_assert_int(n, ==, 3);

	munit_assert_int(xtc_pg_leave(r, "g", b), ==, XTC_OK);
	n = 0;
	munit_assert_int(xtc_pg_members(r, "g", count_cb, &n), ==, 2);
	munit_assert_int(xtc_pg_leave(r, "g", b), ==, XTC_E_INVAL);

	/* An empty group sends to nobody. */
	munit_assert_int(xtc_pg_send(r, "empty", "x", 1), ==, 0);

	xtc_reg_destroy(r);
	return MUNIT_OK;
}

/* ---------- broadcast to live subscribers ---------- */

struct sub_state { _Atomic int got; int value; };

static void
sub_proc(void *arg)
{
	struct sub_state *s = arg;
	void *msg = NULL; size_t sz = 0;
	if (xtc_recv(&msg, &sz, 2LL * 1000 * 1000 * 1000) == XTC_OK) {
		if (sz == sizeof(int)) s->value = *(int *)msg;
		atomic_store(&s->got, 1);
		if (msg) __os_free(msg);
	}
}

static MunitResult
test_pg_broadcast(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_reg_t *r = NULL;
	struct sub_state st[4];
	xtc_pid_t pid[4];
	int i, sent, payload = 7;
	(void)p; (void)d;

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_reg_create(&r), ==, XTC_OK);

	/* Spawn four subscribers, each parked in recv, and join their pids
	 * to the group.  Spawning does not run them yet (cooperative loop),
	 * so joining here races nothing. */
	for (i = 0; i < 4; i++) {
		memset(&st[i], 0, sizeof st[i]);
		munit_assert_int(xtc_proc_spawn(loop, sub_proc, &st[i], NULL,
		    &pid[i]), ==, XTC_OK);
		munit_assert_int(xtc_pg_join(r, "topic", pid[i]), ==, XTC_OK);
	}

	/* Broadcast before running the loop; the messages queue in each
	 * subscriber's mailbox and are delivered when the loop runs. */
	sent = xtc_pg_send(r, "topic", &payload, sizeof payload);
	munit_assert_int(sent, ==, 4);

	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	for (i = 0; i < 4; i++) {
		munit_assert_int(atomic_load(&st[i].got), ==, 1);
		munit_assert_int(st[i].value, ==, 7);
	}

	xtc_reg_destroy(r);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/membership", test_pg_membership, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/broadcast",  test_pg_broadcast,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m10.6/pg", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
