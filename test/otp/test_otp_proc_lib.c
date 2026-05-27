/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/otp/test_otp_proc_lib.c -- port of selected proc_lib_SUITE
 * test cases from `lib/stdlib/test/proc_lib_SUITE.erl` (Erlang/OTP).
 *
 * proc_lib in OTP is the lower-level "spawn a properly-supervised
 * process" library that lies under gen_server.  Its tests exercise:
 *   - spawn / spawn_link / spawn_monitor
 *   - exit signals and reasons
 *   - link signaling on death
 *   - monitor DOWN messages
 *   - timed receives
 *
 * xtc maps proc_lib's surface to xtc_proc:
 *   spawn          -> xtc_proc_spawn
 *   spawn_link     -> xtc_proc_spawn + xtc_link
 *   spawn_monitor  -> xtc_proc_spawn + xtc_monitor
 *   exit/2         -> xtc_exit_pid
 *   exit/1 (self)  -> xtc_exit_self
 *   process_flag(trap_exit, true) -> not directly modeled; we
 *                                    receive 'E' envelopes the
 *                                    same way Erlang's
 *                                    {'EXIT', Pid, Reason} works.
 *
 * Tests omitted (require BEAM-specific infra):
 *   - hibernate/wakeup (no BEAM-style heap in xtc)
 *   - format_status callbacks (gen_server-specific)
 *   - sys:get_state (introspection)
 *   - distributed nodes, registered names (have xtc_reg, but
 *     OTP's name format is global:Name; we use string keys)
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_int.h"

/* ----- proc_lib:spawn -- basic ---------------------------------- */

static _Atomic int g_spawned_ran;

static void
trivial_proc(void *arg)
{
	(void)arg;
	atomic_fetch_add(&g_spawned_ran, 1);
}

/* Maps to: spawn(Mod, Fun, Args) returns a valid Pid; the spawned
 * process runs and exits normally.  See proc_lib_SUITE: t_spawn/1. */
static MunitResult
test_t_spawn(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_pid_t   pid;
	(void)p; (void)d;
	atomic_store(&g_spawned_ran, 0);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, trivial_proc, NULL, NULL, &pid),
	    ==, XTC_OK);
	munit_assert_false(xtc_pid_is_none(pid));
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(atomic_load(&g_spawned_ran), ==, 1);
	(void)xtc_loop_fini(loop);
	return MUNIT_OK;
}

/* ----- proc_lib:spawn_link -- abnormal exit propagates ----------- */

static _Atomic int g_link_signal_seen;
static _Atomic int g_link_signal_reason;

struct link_state { xtc_pid_t target; };

static int
__match_exit(const void *data, size_t size, void *u)
{
	(void)u;
	if (size < 1) return 0;
	return ((const uint8_t *)data)[0] == 'E';
}

static void
link_observer(void *arg)
{
	struct link_state *st = arg;
	void *m; size_t sz;
	if (xtc_link(st->target) != XTC_OK) return;
	if (xtc_recv_match(__match_exit, NULL, &m, &sz,
	    1000LL * 1000 * 1000) == XTC_OK) {
		const uint8_t *e = m;
		atomic_store(&g_link_signal_seen, 1);
		if (sz >= 2) atomic_store(&g_link_signal_reason, (int)e[1]);
		__os_free(m);
	}
}

static void
crashing_proc(void *arg)
{
	void *m; size_t sz;
	(void)arg;
	/* Yield first so the linker can set up the link. */
	(void)xtc_recv(&m, &sz, 30 * 1000 * 1000);
	if (m) __os_free(m);
	xtc_exit_self(42);   /* abnormal */
}

/* Maps to: spawn_link + abnormal exit floods linked process with
 * an EXIT signal.  See proc_lib_SUITE: spawn_link tests. */
static MunitResult
test_spawn_link_abnormal(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_pid_t crasher_pid, observer_pid;
	struct link_state st;
	(void)p; (void)d;
	atomic_store(&g_link_signal_seen, 0);
	atomic_store(&g_link_signal_reason, 0);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, crashing_proc, NULL, NULL,
	    &crasher_pid), ==, XTC_OK);
	st.target = crasher_pid;
	munit_assert_int(xtc_proc_spawn(loop, link_observer, &st, NULL,
	    &observer_pid), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(atomic_load(&g_link_signal_seen), ==, 1);
	(void)xtc_loop_fini(loop);
	return MUNIT_OK;
}

/* ----- proc_lib:spawn_monitor -- DOWN message ------------------- */

static _Atomic int g_monitor_down_seen;

static int
__match_down(const void *data, size_t size, void *u)
{
	(void)u;
	if (size < 1) return 0;
	return ((const uint8_t *)data)[0] == 'D';
}

static void
monitor_observer(void *arg)
{
	xtc_pid_t target = *(xtc_pid_t *)arg;
	uint64_t  ref;
	void *m; size_t sz;
	if (xtc_monitor(target, &ref) != XTC_OK) {
		atomic_fetch_add(&g_monitor_down_seen, 1);
		return;
	}
	if (xtc_recv_match(__match_down, NULL, &m, &sz,
	    1000LL * 1000 * 1000) == XTC_OK) {
		atomic_fetch_add(&g_monitor_down_seen, 1);
		if (m) __os_free(m);
	}
}

/* Maps to: spawn_monitor + crash -> {'DOWN', Ref, process, Pid,
 * Reason}.  See proc_lib_SUITE: monitor tests. */
static MunitResult
test_spawn_monitor_down(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_pid_t target_pid, observer_pid;
	(void)p; (void)d;
	atomic_store(&g_monitor_down_seen, 0);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, crashing_proc, NULL, NULL,
	    &target_pid), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, monitor_observer, &target_pid,
	    NULL, &observer_pid), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(atomic_load(&g_monitor_down_seen), ==, 1);
	(void)xtc_loop_fini(loop);
	return MUNIT_OK;
}

/* ----- proc_lib: exit-with-self-pid is_alive --------------------- */

static _Atomic int g_self_observed;

static void
self_observer(void *arg)
{
	xtc_pid_t self;
	(void)arg;
	self = xtc_self();
	if (!xtc_pid_is_none(self)) atomic_store(&g_self_observed, 1);
}

/* Maps to: self() returns a non-bottom Pid in a spawned process. */
static MunitResult
test_self_returns_pid(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_pid_t pid;
	(void)p; (void)d;
	atomic_store(&g_self_observed, 0);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, self_observer, NULL, NULL, &pid),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(atomic_load(&g_self_observed), ==, 1);
	(void)xtc_loop_fini(loop);
	return MUNIT_OK;
}

/* ----- proc_lib: send/recv between two procs (counter pingpong) - */

static _Atomic int g_pingpong_count;

struct ppmsg { xtc_pid_t from; int n; };

static void
pingpong_pong(void *arg)
{
	void *m; size_t sz;
	(void)arg;
	for (;;) {
		struct ppmsg req, rep;
		if (xtc_recv(&m, &sz, 500LL * 1000 * 1000) != XTC_OK) return;
		if (sz != sizeof req) { __os_free(m); return; }
		memcpy(&req, m, sizeof req);
		__os_free(m);
		if (req.n >= 50) return;
		rep.from = xtc_self();
		rep.n    = req.n + 1;
		(void)xtc_send(req.from, &rep, sizeof rep);
	}
}

static void
pingpong_ping(void *arg)
{
	xtc_pid_t pong = *(xtc_pid_t *)arg;
	xtc_pid_t my_pid = xtc_self();
	int n = 0;
	void *m; size_t sz;
	while (n < 50) {
		struct ppmsg req = { .from = my_pid, .n = n };
		struct ppmsg rep;
		if (xtc_send(pong, &req, sizeof req) != XTC_OK) break;
		if (xtc_recv(&m, &sz, 500LL * 1000 * 1000) != XTC_OK) break;
		if (sz != sizeof rep) { __os_free(m); break; }
		memcpy(&rep, m, sizeof rep);
		__os_free(m);
		n = rep.n;
	}
	atomic_store(&g_pingpong_count, n);
}

/* Maps to: classic Erlang ping-pong.  See proc_lib examples. */
static MunitResult
test_pingpong_50(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_pid_t pong_pid, ping_pid;
	(void)p; (void)d;
	atomic_store(&g_pingpong_count, 0);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, pingpong_pong, NULL, NULL,
	    &pong_pid), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, pingpong_ping, &pong_pid, NULL,
	    &ping_pid), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(atomic_load(&g_pingpong_count), >=, 50);
	(void)xtc_loop_fini(loop);
	return MUNIT_OK;
}

/* ----- proc_lib: recv with timeout returns AGAIN ---------------- */

static _Atomic int g_timeout_seen;

static void
timeout_proc(void *arg)
{
	void *m; size_t sz;
	(void)arg;
	if (xtc_recv(&m, &sz, 50 * 1000 * 1000) == XTC_E_AGAIN)
		atomic_store(&g_timeout_seen, 1);
}

/* Maps to: receive ... after Timeout -> timeout_value.  Erlang's
 * 'after' clause becomes XTC_E_AGAIN. */
static MunitResult
test_recv_timeout(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_pid_t pid;
	(void)p; (void)d;
	atomic_store(&g_timeout_seen, 0);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, timeout_proc, NULL, NULL, &pid),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(atomic_load(&g_timeout_seen), ==, 1);
	(void)xtc_loop_fini(loop);
	return MUNIT_OK;
}

/* ----- proc_lib: selective receive (skip non-matching messages) - */

struct selective_state { int n; int order_seen[5]; };

static int
__match_42(const void *data, size_t sz, void *u)
{
	(void)u;
	if (sz != sizeof(int)) return 0;
	return *(const int *)data == 42;
}

static void
selective_proc(void *arg)
{
	struct selective_state *st = arg;
	void *m; size_t sz;
	int  i;

	/* First receive: only accept 42.  All other messages stay in
	 * the save queue. */
	if (xtc_recv_match(__match_42, NULL, &m, &sz,
	    1000LL * 1000 * 1000) != XTC_OK) return;
	st->order_seen[st->n++] = *(const int *)m;
	__os_free(m);

	/* Drain the rest in arrival order. */
	for (i = 0; i < 4; i++) {
		if (xtc_recv(&m, &sz, 100 * 1000 * 1000) != XTC_OK) break;
		st->order_seen[st->n++] = *(const int *)m;
		__os_free(m);
	}
}

/* Maps to: receive {tag, _} -> ... end with one tag higher
 * priority.  Classic Erlang selective receive. */
static MunitResult
test_selective_receive(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_pid_t  pid;
	struct selective_state s = {0};
	int sent[5] = { 1, 2, 42, 3, 4 };
	int i;
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, selective_proc, &s, NULL, &pid),
	    ==, XTC_OK);
	for (i = 0; i < 5; i++)
		munit_assert_int(xtc_send(pid, &sent[i], sizeof(int)), ==,
		    XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	/* Order: 42 first (selective), then 1, 2, 3, 4. */
	munit_assert_int(s.n, ==, 5);
	munit_assert_int(s.order_seen[0], ==, 42);
	munit_assert_int(s.order_seen[1], ==, 1);
	munit_assert_int(s.order_seen[2], ==, 2);
	munit_assert_int(s.order_seen[3], ==, 3);
	munit_assert_int(s.order_seen[4], ==, 4);
	(void)xtc_loop_fini(loop);
	return MUNIT_OK;
}

/* ----- proc_lib: many spawns + send_msg fanout ------------------ */

#define FANOUT_N 100
static _Atomic int g_fanout_count;

static void
fanout_worker(void *arg)
{
	void *m; size_t sz;
	(void)arg;
	if (xtc_recv(&m, &sz, 1000LL * 1000 * 1000) == XTC_OK) {
		atomic_fetch_add(&g_fanout_count, 1);
		if (m) __os_free(m);
	}
}

static MunitResult
test_fanout_100_workers(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_pid_t pids[FANOUT_N];
	int marker = 1;
	int i;
	(void)p; (void)d;
	atomic_store(&g_fanout_count, 0);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	for (i = 0; i < FANOUT_N; i++)
		munit_assert_int(xtc_proc_spawn(loop, fanout_worker, NULL,
		    NULL, &pids[i]), ==, XTC_OK);
	for (i = 0; i < FANOUT_N; i++)
		(void)xtc_send(pids[i], &marker, sizeof marker);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(atomic_load(&g_fanout_count), ==, FANOUT_N);
	(void)xtc_loop_fini(loop);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/t_spawn",            test_t_spawn,            NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/spawn_link_abnormal", test_spawn_link_abnormal, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/spawn_monitor_down", test_spawn_monitor_down, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/self_returns_pid",   test_self_returns_pid,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/pingpong_50",        test_pingpong_50,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/recv_timeout",       test_recv_timeout,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/selective_receive",  test_selective_receive,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/fanout_100",         test_fanout_100_workers, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/otp/proc_lib", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
