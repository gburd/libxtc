/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m8/test_proc.c -- verifies M8 process + mailbox + selective receive.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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

/* Infinite-wait recv must PARK (not busy-reschedule): a waiter blocks
 * on xtc_recv(timeout = -1) while a sender, after parking ~120 ms on
 * a timer, sends one message.  The whole run consumes far less CPU
 * than its wall-clock duration -- a busy-loop would burn a core for
 * the full 120 ms. */
struct inf_state { xtc_pid_t waiter; int got; };

static void
inf_waiter(void *arg)
{
	struct inf_state *s = arg;
	void *m = NULL; size_t n = 0;
	if (xtc_recv(&m, &n, -1) == XTC_OK) {
		s->got = 1;
		if (m) __os_free(m);
	}
}

static void
inf_sender(void *arg)
{
	struct inf_state *s = arg;
	void *m = NULL; size_t n = 0;
	int v = 1;
	/* Park ~120 ms on a timer via a finite recv that will time out. */
	(void)xtc_recv(&m, &n, 120LL * 1000 * 1000);
	if (m) __os_free(m);
	(void)xtc_send(s->waiter, &v, sizeof v);
}

static MunitResult
test_recv_inf_parks(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t opts = { 0 };
	struct inf_state s = { XTC_PID_NONE, 0 };
	xtc_pid_t sp;
	struct timespec c0, c1, w0, w1;
	double cpu, wall;
	(void)p; (void)d;

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	opts.name = "inf-waiter";
	munit_assert_int(xtc_proc_spawn(loop, inf_waiter, &s, &opts,
	    &s.waiter), ==, XTC_OK);
	opts.name = "inf-sender";
	munit_assert_int(xtc_proc_spawn(loop, inf_sender, &s, &opts, &sp),
	    ==, XTC_OK);

	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &c0);
	clock_gettime(CLOCK_MONOTONIC, &w0);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	clock_gettime(CLOCK_MONOTONIC, &w1);
	clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &c1);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);

	cpu  = (c1.tv_sec - c0.tv_sec) + (c1.tv_nsec - c0.tv_nsec) / 1e9;
	wall = (w1.tv_sec - w0.tv_sec) + (w1.tv_nsec - w0.tv_nsec) / 1e9;

	munit_assert_int(s.got, ==, 1);           /* message delivered */
	munit_assert_double(wall, >, 0.10);        /* we really waited */
	/* Parked: CPU is a small fraction of the wait.  A busy-loop would
	 * make cpu ~= wall.  Generous bound so a loaded CI runner passes. */
	munit_assert_double(cpu, <, 0.040);
	return MUNIT_OK;
}

/* Mailbox observability + watermark.  Sends accumulate in a small
 * capped mailbox before the proc runs; the watermark callback fires
 * on the rising edge, over-cap sends are rejected and counted, and
 * xtc_proc_mailbox_stats reports depth / peak / totals. */
static _Atomic int    g_wm_fires;
static _Atomic size_t g_wm_depth;

static void
wm_cb(xtc_pid_t self, size_t depth, size_t cap, void *user)
{
	(void)self; (void)cap; (void)user;
	atomic_fetch_add(&g_wm_fires, 1);
	atomic_store(&g_wm_depth, depth);
}

static void
drainer(void *arg)
{
	void *m; size_t n;
	(void)arg;
	/* Drain whatever is queued, then exit so the loop terminates. */
	while (xtc_recv(&m, &n, 0) == XTC_OK) {
		if (m) __os_free(m);
	}
}

static MunitResult
test_mailbox_stats(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t opts = { 0 };
	xtc_pid_t pid;
	xtc_mailbox_stats_t st;
	int i, v = 7;
	(void)p; (void)d;

	atomic_store(&g_wm_fires, 0);
	atomic_store(&g_wm_depth, 0);

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	opts.name = "mbx";
	opts.mailbox_cap = 8;
	opts.mailbox_watermark_pct = 50;        /* level = 4 */
	opts.mailbox_watermark_fn = wm_cb;
	munit_assert_int(xtc_proc_spawn(loop, drainer, NULL, &opts, &pid),
	    ==, XTC_OK);

	/* Send 10 before the loop runs: 8 accepted, 2 rejected. */
	for (i = 0; i < 10; i++) {
		int rc = xtc_send(pid, &v, sizeof v);
		if (i < 8) munit_assert_int(rc, ==, XTC_OK);
		else       munit_assert_int(rc, ==, XTC_E_AGAIN);
	}

	munit_assert_int(xtc_proc_mailbox_stats(pid, &st), ==, XTC_OK);
	munit_assert_size(st.depth, ==, 8);
	munit_assert_size(st.peak, ==, 8);
	munit_assert_size(st.cap, ==, 8);
	munit_assert_uint64(st.recv_total, ==, 8);
	munit_assert_uint64(st.drop_total, ==, 2);

	/* Watermark fired once on the rising edge, at depth >= 4. */
	munit_assert_int(atomic_load(&g_wm_fires), ==, 1);
	munit_assert_size(atomic_load(&g_wm_depth), >=, 4);

	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* Save-queue cap: the mailbox bound must count the selective-receive
 * save queue too, or a flood of non-matching messages drains into the
 * unbounded save queue and defeats mailbox_cap.  A saver proc parks in
 * a selective receive whose predicate never matches, so every message
 * sent to it lands in its save queue; a flooder confirms that once
 * depth+saved reaches cap, further sends are rejected. */
static int
never_match(const void *data, size_t size, void *user)
{
	(void)data; (void)size; (void)user;
	return 0;
}

struct sqc_state {
	xtc_pid_t saver;
	int       accepted;     /* sends that returned XTC_OK */
	int       rejected;     /* sends that returned XTC_E_AGAIN */
	size_t    final_saved;
	size_t    final_depth;
};

static void
sqc_saver(void *arg)
{
	void *m = NULL; size_t n = 0;
	(void)arg;
	/* Never matches: pulls every delivered message into the save
	 * queue, re-parking on each, until the timeout. */
	(void)xtc_recv_match(never_match, NULL, &m, &n, 400LL * 1000 * 1000);
}

static void
sqc_flooder(void *arg)
{
	struct sqc_state *s = arg;
	xtc_mailbox_stats_t st;
	int v = 1, i, spins;

	/* Fill to cap (8): all accepted, then drained into the save
	 * queue by the saver. */
	for (i = 0; i < 8; i++) {
		if (xtc_send(s->saver, &v, sizeof v) == XTC_OK) s->accepted++;
		else s->rejected++;
	}
	/* Let the saver move them all from mailbox to save queue. */
	for (spins = 0; spins < 200; spins++) {
		void *m = NULL; size_t n = 0;
		if (xtc_proc_mailbox_stats(s->saver, &st) == XTC_OK &&
		    st.depth == 0 && st.saved == 8)
			break;
		(void)xtc_recv(&m, &n, 2LL * 1000 * 1000);   /* yield ~2ms */
	}
	/* Save queue now holds cap messages.  Further sends must be
	 * rejected because the cap counts mailbox + save, not just the
	 * (now empty) mailbox. */
	for (i = 0; i < 4; i++) {
		if (xtc_send(s->saver, &v, sizeof v) == XTC_OK) s->accepted++;
		else s->rejected++;
	}
	(void)xtc_proc_mailbox_stats(s->saver, &st);
	s->final_saved = st.saved;
	s->final_depth = st.depth;
}

static MunitResult
test_save_queue_cap(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t opts = { 0 };
	struct sqc_state s = { XTC_PID_NONE, 0, 0, 0, 0 };
	xtc_pid_t fl;
	(void)p; (void)d;

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	opts.name = "saver";
	opts.mailbox_cap = 8;
	munit_assert_int(xtc_proc_spawn(loop, sqc_saver, NULL, &opts,
	    &s.saver), ==, XTC_OK);
	opts.name = "flooder";
	opts.mailbox_cap = 0;
	munit_assert_int(xtc_proc_spawn(loop, sqc_flooder, &s, &opts, &fl),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);

	/* Exactly cap accepted across both bursts; the 4 over-cap sends
	 * after the save queue filled were rejected. */
	munit_assert_int(s.accepted, ==, 8);
	munit_assert_int(s.rejected, ==, 4);
	/* Total held never exceeded the cap. */
	munit_assert_size(s.final_saved + s.final_depth, <=, 8);
	munit_assert_size(s.final_saved, ==, 8);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/send_recv_basic",   test_send_recv_basic,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/self",              test_self,             NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/selective_receive", test_selective_receive,NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/monitor",           test_monitor,          NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/recv_inf_parks",    test_recv_inf_parks,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/mailbox_stats",     test_mailbox_stats,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/save_queue_cap",    test_save_queue_cap,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m8/proc", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
