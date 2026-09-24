/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m8/test_proc.c -- verifies M8 process + mailbox + selective receive.
 */

#include <stdint.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#if defined(_WIN32)
#include <windows.h>          /* GetProcessTimes for the park-CPU check */
#else
#include <sys/wait.h>
#include <pthread.h>          /* the foreign-thread sender below */
#endif

#include "munit.h"
#include "fd_probe_compat.h"
#include "io_pipe_compat.h"   /* portable pipe pair for the wait_fd tests */
#include "os_time.h"           /* __os_sleep_ns: the foreign-thread racer */
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_async.h"
#include "xtc_proc.h"
#include "xtc_inspect.h"   /* xtc_proc_info: supervisor-side mask state */
#include "xtc_mctx.h"
#include "xtc_int.h"
#include "xtc_res.h"
#include "xtc_inject.h"   /* drive the wait_fd check/arm window deterministically */

/* Process CPU seconds, portably.  POSIX: CLOCK_PROCESS_CPUTIME_ID.
 * Windows: GetProcessTimes (kernel+user), which IS the clean Win32
 * equivalent -- so the "a park costs no CPU" proof below runs on
 * Windows too, where it is worth more than elsewhere (the IOCP backend
 * has an 8 ms AFD repoll sweep that a regression could turn into a
 * spin). */
static double
test_proc_cpu_secs(void)
{
#if defined(_WIN32)
	FILETIME cr, ex, kt, ut;
	ULARGE_INTEGER k, u;
	if (!GetProcessTimes(GetCurrentProcess(), &cr, &ex, &kt, &ut))
		return 0.0;
	k.LowPart = kt.dwLowDateTime; k.HighPart = kt.dwHighDateTime;
	u.LowPart = ut.dwLowDateTime; u.HighPart = ut.dwHighDateTime;
	return (double)(k.QuadPart + u.QuadPart) / 1e7;   /* 100 ns units */
#else
	struct timespec ts;
	if (clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts) != 0)
		return 0.0;
	return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
#endif
}

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

/* ---------- xtc_proc_userdata ---------- */

struct ud_state {
	int  set_rc;      /* xtc_proc_set_userdata rc, in-proc */
	void *got;        /* xtc_proc_userdata after set, in-proc */
	void *got_default;/* xtc_proc_userdata BEFORE set (must be NULL) */
};
static void
ud_proc(void *arg)
{
	struct ud_state *s = arg;
	s->got_default = xtc_proc_userdata();        /* NULL by default */
	s->set_rc = xtc_proc_set_userdata((void *)(uintptr_t)0xABCD1234);
	s->got = xtc_proc_userdata();                /* reads back the set value */
}

static MunitResult
test_userdata(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	struct ud_state s = { -1, (void *)1, (void *)1 };
	xtc_pid_t pid;
	(void)p; (void)d;

	/* Off a proc: set returns XTC_E_INVAL, get returns NULL. */
	munit_assert_int(xtc_proc_set_userdata((void *)1), ==, XTC_E_INVAL);
	munit_assert_ptr_null(xtc_proc_userdata());

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, ud_proc, &s, NULL, &pid),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	/* In-proc: default NULL, set succeeds, get returns exactly what
	 * was set. */
	munit_assert_ptr_null(s.got_default);
	munit_assert_int(s.set_rc, ==, XTC_OK);
	munit_assert_ptr_equal(s.got, (void *)(uintptr_t)0xABCD1234);

	/* Still off a proc after the loop finishes. */
	munit_assert_ptr_null(xtc_proc_userdata());
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
	XTC_PACK_PUSH
	struct mon_down {
		uint8_t kind; uint64_t ref; xtc_pid_t pid; int reason;
	} XTC_PACKED;
	XTC_PACK_POP
	struct mon_down *down;
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

/* ---------- atomic spawn_monitor / spawn_link ---------- */

static void
instant_child(void *arg)
{
	(void)arg;
	(void)xtc_exit_self(42);
}

struct sm_state { int saw_down; int reason; int link_saw_exit; xtc_loop_t *loop; };

static void
sm_parent(void *arg)
{
	struct sm_state *s = arg;
	void *msg = NULL; size_t sz = 0;
	XTC_PACK_PUSH
	struct sm_down { uint8_t kind; uint64_t ref; xtc_pid_t pid; int reason; }
	    XTC_PACKED;
	XTC_PACK_POP
	struct sm_down *down;
	xtc_pid_t child;
	uint64_t ref = 0;
	if (xtc_proc_spawn_monitor(s->loop, instant_child,
	    NULL, NULL, &child, &ref) != XTC_OK)
		return;
	(void)child;
	if (xtc_recv(&msg, &sz, 5LL * 1000 * 1000 * 1000) != XTC_OK) return;
	if (sz >= sizeof *down) {
		down = msg;
		if (down->kind == 'D') { s->saw_down = 1; s->reason = down->reason; }
	}
	xtc_free(msg);
}

static MunitResult
test_spawn_monitor_atomic(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	struct sm_state s = {0, 0, 0, NULL};
	xtc_pid_t parent;
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	s.loop = loop;
	munit_assert_int(xtc_proc_spawn(loop, sm_parent, &s, NULL, &parent),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(s.saw_down, ==, 1);
	munit_assert_int(s.reason, ==, 42);
	munit_assert_int(xtc_down_is_noproc(s.reason), ==, 0);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

static void
sl_bad_child(void *arg) { (void)arg; (void)xtc_exit_self(9); }

static void
sl_parent(void *arg)
{
	struct sm_state *s = arg;
	void *msg = NULL; size_t sz = 0;
	/* Link EXIT signal layout is { kind='E', reason, pid } -- distinct
	 * from the monitor DOWN { kind='D', ref, pid, reason }. */
	XTC_PACK_PUSH
	struct sl_exit { uint8_t kind; int reason; xtc_pid_t pid; }
	    XTC_PACKED;
	XTC_PACK_POP
	struct sl_exit *ex;
	xtc_pid_t child;
	if (xtc_proc_spawn_link(s->loop, sl_bad_child,
	    NULL, NULL, &child) != XTC_OK)
		return;
	(void)child;
	if (xtc_recv(&msg, &sz, 5LL * 1000 * 1000 * 1000) != XTC_OK) return;
	if (sz >= sizeof *ex) {
		ex = msg;
		if (ex->kind == 'E') {
			s->link_saw_exit = 1;
			s->reason = ex->reason;
		}
	}
	xtc_free(msg);
}

static MunitResult
test_spawn_link_atomic(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	struct sm_state s = {0, 0, 0, NULL};
	xtc_pid_t parent;
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	s.loop = loop;
	munit_assert_int(xtc_proc_spawn(loop, sl_parent, &s, NULL, &parent),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(s.link_saw_exit, ==, 1);
	munit_assert_int(s.reason, ==, 9);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

static MunitResult
test_spawn_rel_needs_proc(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	xtc_pid_t pid; uint64_t ref;
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn_link(loop, instant_child, NULL, NULL,
	    &pid), ==, XTC_E_INVAL);
	munit_assert_int(xtc_proc_spawn_monitor(loop, instant_child, NULL,
	    NULL, &pid, &ref), ==, XTC_E_INVAL);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* ---- self-describing DOWN: xtc_exit_self(1) is an EXIT code 1, NOT a
 *      signal-1 fault (the carrier team's sharp edge). ---- */
static void
exit1_child(void *arg) { (void)arg; (void)xtc_exit_self(1); }

struct dinfo_state { int kind; int signal; int exit_code; int got; xtc_loop_t *loop; };

static void
dinfo_parent(void *arg)
{
	struct dinfo_state *s = arg;
	void *msg = NULL; size_t sz = 0;
	xtc_pid_t child;
	uint64_t ref = 0;
	xtc_down_info_t info;
	if (xtc_proc_spawn_monitor(s->loop, exit1_child, NULL, NULL,
	    &child, &ref) != XTC_OK)
		return;
	if (xtc_recv(&msg, &sz, 5LL * 1000 * 1000 * 1000) != XTC_OK) return;
	if (xtc_down_decode_ex(msg, sz, &info) == XTC_OK) {
		s->got = 1;
		s->kind = (int)info.kind;
		s->signal = info.signal;
		s->exit_code = info.exit_code;
	}
	xtc_free(msg);
}

static MunitResult
test_down_decode_ex_exit_vs_signal(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop;
	struct dinfo_state s = {0, 0, 0, 0, NULL};
	xtc_pid_t parent;
	(void)p; (void)d;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	s.loop = loop;
	munit_assert_int(xtc_proc_spawn(loop, dinfo_parent, &s, NULL,
	    &parent), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(s.got, ==, 1);
	/* The decisive assertion: xtc_exit_self(1) is an EXIT (code 1),
	 * NOT a signal-1 fault. */
	munit_assert_int(s.kind, ==, XTC_DOWN_KIND_EXIT);
	munit_assert_int(s.exit_code, ==, 1);
	munit_assert_int(s.signal, ==, 0);
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
	double c0, c1, w0, w1;
	double cpu, wall;
	(void)p; (void)d;

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	opts.name = "inf-waiter";
	munit_assert_int(xtc_proc_spawn(loop, inf_waiter, &s, &opts,
	    &s.waiter), ==, XTC_OK);
	opts.name = "inf-sender";
	munit_assert_int(xtc_proc_spawn(loop, inf_sender, &s, &opts, &sp),
	    ==, XTC_OK);

	c0 = test_proc_cpu_secs();
	w0 = (double)xtc_clock_mono() / 1e9;
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	w1 = (double)xtc_clock_mono() / 1e9;
	c1 = test_proc_cpu_secs();
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);

	cpu  = c1 - c0;
	wall = w1 - w0;

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

/* R1 fault containment: a REAL SIGSEGV inside one proc must unwind
 * only that proc -- the sibling/monitor survives and observes DOWN
 * with the fault reason.  This is the F5 crash-containment spike made
 * real (an actual wild-pointer write, not a modelled trigger). */
#define FLT_REASON 42

struct flt_state {
	int saw_down;
	int reason;
	int cleanup_ran;
	int atexit_ran;
};

/* Stands in for an embedder's lock-manager release-all: registered
 * with xtc_proc_at_exit, it must run even when the proc dies via a
 * contained fault, so a faulted session never leaves a lock held. */
static void
flt_release(void *arg)
{
	struct flt_state *s = arg;
	s->atexit_ran = 1;
}

static void
flt_faulter(void *arg)
{
	struct flt_state *s = arg;
	void *m = NULL; size_t n = 0;
	int sig = xtc_proc_recovery_arm();
	if (sig != 0) {
		/* Recovered from the contained fault: clean up and exit,
		 * which delivers DOWN(reason) to the monitor. */
		s->cleanup_ran = 1;
		(void)xtc_exit_self(FLT_REASON);
		return;
	}
	/* Normal path: register the resource-release hook (it must run on
	 * the contained-fault exit), wait for the monitor to be
	 * established, then dereference a wild pointer -> genuine SIGSEGV. */
	(void)xtc_proc_at_exit(flt_release, s);
	if (xtc_recv(&m, &n, 2LL * 1000 * 1000 * 1000) == XTC_OK && m)
		__os_free(m);
	{
		/* Route the address through a volatile so the compiler
		 * cannot prove it out of bounds (-Warray-bounds) -- it is a
		 * deliberate wild write. */
		volatile uintptr_t addr = 0x10;
		*(volatile int *)addr = 1;     /* boom */
	}
}

/* Item 3 (xtc-carrier report): a proc that faults in its FIRST
 * statement -- before it ever calls xtc_proc_recovery_arm() -- must
 * still be contained and deliver a DOWN, thanks to the default recovery
 * frame __proc_entry now auto-arms.  This faulter does NO arming and no
 * work: it waits for the monitor to be established, then dereferences a
 * wild pointer immediately. */
static void
flt_early_faulter(void *arg)
{
	struct flt_state *s = arg;
	void *m = NULL; size_t n = 0;
	(void)s;
	/* Wait for the watcher's go (monitor established) with NO prior
	 * xtc_proc_recovery_arm().  Then fault immediately. */
	if (xtc_recv(&m, &n, 2LL * 1000 * 1000 * 1000) == XTC_OK && m)
		__os_free(m);
	{
		volatile uintptr_t addr = 0x10;
		*(volatile int *)addr = 1;     /* boom -- before arming */
	}
}

static void
flt_watcher(void *arg)
{
	struct flt_state *s = arg;
	void *msg = NULL; size_t sz = 0;
	xtc_pid_t target, down_pid;
	uint64_t ref;
	int go = 1, down_reason = 0;

	if (xtc_recv(&msg, &sz, 1000LL * 1000 * 1000) != XTC_OK) return;
	memcpy(&target, msg, sizeof target);
	__os_free(msg);
	if (xtc_monitor(target, &ref) != XTC_OK) return;
	/* Monitor established: tell the faulter to proceed. */
	(void)xtc_send(target, &go, sizeof go);
	/* Await DOWN; decode it with the library helper rather than a
	 * hand-rolled packed struct. */
	if (xtc_recv(&msg, &sz, 5LL * 1000 * 1000 * 1000) != XTC_OK) return;
	if (xtc_down_decode(msg, sz, &down_pid, &down_reason) == XTC_OK) {
		s->saw_down = 1;
		s->reason = down_reason;
	}
	__os_free(msg);
}

/* Recovery resource registry: a proc registers an fd, a memory
 * context, a lock-manager release-all, and a generic callback, then
 * faults.  xtc_proc_recovery_arm_clean's recovered branch releases all
 * four automatically (LIFO) -- the embedder writes no cleanup code. */
struct rec_state {
	int saw_down;
	int reason;
	int cb_ran;          /* generic callback released */
	int lock_released;   /* lock-manager release-all ran */
	int mctx_reset;      /* mctx reset ran (via a register_cleanup hook) */
	int fd;              /* a real pipe fd; closed on recovery */
};
static struct rec_state g_rec;

static void
rec_cb(void *arg)
{
	struct rec_state *s = arg;
	s->cb_ran = 1;
}

/* Stands in for xtc_lock_release_all(mgr, locker): the track_locks
 * callback shape is void(*)(void *mgr, uint64_t locker). */
static void
rec_release_all(void *mgr, uint64_t locker)
{
	struct rec_state *s = mgr;
	(void)locker;
	s->lock_released = 1;
}

static void
rec_mctx_cleanup(void *arg)
{
	struct rec_state *s = arg;
	s->mctx_reset = 1;
}

static void
rec_faulter(void *arg)
{
	struct rec_state *s = arg;
	void *m = NULL; size_t n = 0;
	struct xtc_mctx *mctx;
	int pfd[2];

	/* Arm with the auto-cleanup convenience: on a contained fault it
	 * releases every tracked resource and exits -- no custom block. */
	xtc_proc_recovery_arm_clean();

	/* Register the resources this "session" holds. */
	if (pipe(pfd) == 0) {
		s->fd = pfd[1];
		(void)xtc_proc_recovery_track_fd(pfd[1]);
		(void)close(pfd[0]);
	}
	mctx = xtc_proc_mctx();
	if (mctx != NULL) {
		(void)xtc_mctx_register_cleanup(mctx, rec_mctx_cleanup, s);
		(void)xtc_proc_recovery_track_mctx(mctx);
	}
	(void)xtc_proc_recovery_track_locks(s, 7u, rec_release_all);
	(void)xtc_proc_recovery_track(rec_cb, s);

	/* Wait for the monitor, then fault. */
	if (xtc_recv(&m, &n, 2LL * 1000 * 1000 * 1000) == XTC_OK && m)
		__os_free(m);
	{
		volatile uintptr_t addr = 0x10;
		*(volatile int *)addr = 1;     /* boom */
	}
}

static MunitResult
test_recovery_registry(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_pid_t faulter, watcher;
	(void)p; (void)d;

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
	return MUNIT_SKIP;   /* sanitizer owns SIGSEGV / flags recovery alloc */
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
	return MUNIT_SKIP;
#  endif
#endif
	memset(&g_rec, 0, sizeof g_rec);
	g_rec.fd = -1;

	munit_assert_int(xtc_fault_guard_install(), ==, XTC_OK);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, flt_watcher, &g_rec, NULL,
	    &watcher), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, rec_faulter, &g_rec, NULL,
	    &faulter), ==, XTC_OK);
	munit_assert_int(xtc_send(watcher, &faulter, sizeof faulter),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);

	/* All four registered resources were released on the contained
	 * fault, with no hand-written cleanup in the recovery block. */
	munit_assert_int(g_rec.cb_ran, ==, 1);
	munit_assert_int(g_rec.lock_released, ==, 1);
	munit_assert_int(g_rec.mctx_reset, ==, 1);
	/* The tracked fd was closed: probing it reports "not a live fd".
	 * NOT a bare `close(fd) == -1` -- on Windows that is fatal (the
	 * MSVC CRT __fastfail's on an invalid descriptor); see
	 * test/include/fd_probe_compat.h. */
	munit_assert_int(xtc_test_fd_is_closed(g_rec.fd), ==, 1);
	munit_assert_int(g_rec.saw_down, ==, 1);   /* monitor observed it */
	return MUNIT_OK;
}

static MunitResult
test_fault_contain(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	struct flt_state s = { 0, 0, 0, 0 };
	xtc_pid_t faulter, watcher;
	(void)p; (void)d;

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
	return MUNIT_SKIP;   /* the sanitizer owns SIGSEGV / flags the
	                      * recovery alloc as signal-unsafe; cannot
	                      * test our fault handler under it (gcc form) */
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
	return MUNIT_SKIP;
#  endif
#endif

	munit_assert_int(xtc_fault_guard_install(), ==, XTC_OK);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, flt_watcher, &s, NULL,
	    &watcher), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, flt_faulter, &s, NULL,
	    &faulter), ==, XTC_OK);
	munit_assert_int(xtc_send(watcher, &faulter, sizeof faulter),
	    ==, XTC_OK);
	/* If containment failed, the loop thread takes a SIGSEGV here and
	 * the test crashes -- exactly the regression we are guarding. */
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);

	munit_assert_int(s.cleanup_ran, ==, 1);   /* faulter recovered */
	munit_assert_int(s.atexit_ran, ==, 1);     /* at-exit ran on fault */
	munit_assert_int(s.saw_down, ==, 1);       /* sibling observed it */
	munit_assert_int(s.reason, ==, FLT_REASON);
	return MUNIT_OK;
}

/* Item 3: a fault BEFORE the app arms its own recovery frame must still
 * be contained and deliver a DOWN (the default frame auto-armed in
 * __proc_entry catches it).  The DOWN reason is the positive signal
 * number (SIGSEGV = 11), NOT swallowed and NOT escalated to a process
 * abort. */
static MunitResult
test_fault_early_contain(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	struct flt_state s = { 0, 0, 0, 0 };
	xtc_pid_t faulter, watcher;
	(void)p; (void)d;

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
	return MUNIT_SKIP;
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
	return MUNIT_SKIP;
#  endif
#endif

	munit_assert_int(xtc_fault_guard_install(), ==, XTC_OK);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, flt_watcher, &s, NULL,
	    &watcher), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, flt_early_faulter, &s, NULL,
	    &faulter), ==, XTC_OK);
	munit_assert_int(xtc_send(watcher, &faulter, sizeof faulter),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);

	/* The key assertion: a DOWN arrived for the early fault (Item 3
	 * was: no DOWN at all).  Reason is the positive signal number. */
	munit_assert_int(s.saw_down, ==, 1);
	munit_assert_int(s.reason, ==, SIGSEGV);   /* 11 */
	return MUNIT_OK;
}

/* Escalate path: a fault INSIDE a critical section must NOT be
 * contained -- shared state may be torn -- so it takes the whole
 * process down.  Verified in a forked child: the child arms a
 * recovery frame, enters a critical section, then faults; the parent
 * confirms the child died by SIGSEGV rather than recovering. */
static void
esc_faulter(void *arg)
{
	(void)arg;
	if (xtc_proc_recovery_arm() != 0)
		_exit(99);     /* contained -- WRONG inside a critical section */
	xtc_proc_critical_enter();
	{
		volatile uintptr_t addr = 0x10;
		*(volatile int *)addr = 1;    /* fault in crit -> escalate */
	}
	_exit(98);         /* survived the fault -- also wrong */
}

static MunitResult
test_fault_escalate(const MunitParameter p[], void *d)
{
#if defined(_WIN32)
	/* fork() has no Win32 equivalent, and the escalation contract is
	 * observed differently there: the SEH vectored handler returns
	 * EXCEPTION_CONTINUE_SEARCH so the process dies with
	 * 0xC0000005.  That half is verified by a dedicated driver on a
	 * Windows host (see KNOWN_ISSUES.md "Windows fault containment
	 * (SEH)"), not by this fork-based case.  Skipping ONLY this case
	 * lets the other ~40 test_proc cases -- including
	 * /selective_receive -- build and run under MSVC. */
	(void)p; (void)d;
	return MUNIT_SKIP;
#else
	pid_t pid;
	int st;
	(void)p; (void)d;

#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
	return MUNIT_SKIP;
#elif defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
	return MUNIT_SKIP;
#  endif
#endif

	pid = fork();
	munit_assert_int(pid, >=, 0);
	if (pid == 0) {
		/* Child: a single-loop process that faults in a critical
		 * section.  Containment is armed but must be overridden. */
		xtc_loop_t *loop = NULL;
		xtc_proc_opts_t opts = { 0 };
		xtc_pid_t fp;
		(void)xtc_fault_guard_install();
		if (xtc_loop_init(&loop) != XTC_OK) _exit(97);
		opts.name = "esc";
		if (xtc_proc_spawn(loop, esc_faulter, NULL, &opts, &fp)
		    != XTC_OK) _exit(96);
		(void)xtc_loop_run(loop);
		_exit(0);     /* loop returned normally -> fault was contained */
	}
	munit_assert_int(waitpid(pid, &st, 0), ==, pid);
	/* The child must have been killed by the fault signal, proving
	 * the critical-section fault escalated instead of being
	 * contained. */
	munit_assert_true(WIFSIGNALED(st));
	munit_assert_true(WTERMSIG(st) == SIGSEGV || WTERMSIG(st) == SIGBUS);
	return MUNIT_OK;
#endif /* !_WIN32 */
}

/* at-exit hooks run LIFO on a normal proc exit. */
static _Atomic int g_atx_order;
static _Atomic int g_atx_a, g_atx_b;
static void atx_a(void *u) { (void)u; atomic_store(&g_atx_a, atomic_fetch_add(&g_atx_order, 1) + 1); }
static void atx_b(void *u) { (void)u; atomic_store(&g_atx_b, atomic_fetch_add(&g_atx_order, 1) + 1); }
static void atx_proc(void *arg)
{
	(void)arg;
	munit_assert_int(xtc_proc_at_exit(atx_a, NULL), ==, XTC_OK);
	munit_assert_int(xtc_proc_at_exit(atx_b, NULL), ==, XTC_OK);
	/* return -> normal exit -> hooks run */
}

static MunitResult
test_proc_at_exit(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t opts = { 0 };
	xtc_pid_t pid;
	(void)p; (void)d;
	atomic_store(&g_atx_order, 0);
	atomic_store(&g_atx_a, 0);
	atomic_store(&g_atx_b, 0);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	opts.name = "atx";
	munit_assert_int(xtc_proc_spawn(loop, atx_proc, NULL, &opts, &pid),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	/* Both ran; LIFO -> b (registered second) ran first. */
	munit_assert_int(atomic_load(&g_atx_b), ==, 1);
	munit_assert_int(atomic_load(&g_atx_a), ==, 2);
	return MUNIT_OK;
}

/*
 * [A1] pid uniqueness at the local_id ceiling.
 *
 * xtc_pid_t.local_id is uint16_t, so slot 65536 would truncate to 0 and
 * hand out a pid byte-identical to the live proc in slot 0 (gen is
 * per-slot, so it cannot disambiguate either) -- wrong-proc send/wake/
 * DOWN delivery and a release that clears someone else's slot.  Before
 * the fix this was masked only incidentally by the default fds cap
 * (65536) while the tasks cap defaults to 100000, i.e. ABOVE it; an
 * embedder that legitimately raised fds hit the collision.
 *
 * The proc table now refuses past the ceiling with XTC_E_RESOURCE.
 * This test raises EVERY relevant cap well past 65536 -- the exact
 * configuration that used to collide -- and asserts that (a) spawning
 * stops with XTC_E_RESOURCE rather than wrapping, and (b) every pid
 * handed out is unique.
 *
 * Procs park forever (they only exit when released), so no slot is
 * recycled and the run genuinely walks the table to its ceiling.
 */
#define A1_PROBE_TARGET 66000            /* > 65536, so we cross it */
static _Atomic int g_a1_release;
static void
a1_parker(void *arg)
{
	(void)arg;
	while (!atomic_load(&g_a1_release))
		xtc_yield();
}

static MunitResult
test_pid_local_id_ceiling(const MunitParameter p[], void *d)
{
	xtc_loop_t *lp = NULL;
	xtc_proc_opts_t o;
	xtc_pid_t *pids;
	struct xtc_res *res;
	int i, n = 0, rc = XTC_OK;
	(void)p; (void)d;

	atomic_store(&g_a1_release, 0);
	munit_assert_int(xtc_loop_init(&lp), ==, XTC_OK);

	/* Raise every cap that could stop us BEFORE the pid ceiling, so the
	 * proc table's own limit is what we are testing. */
	res = xtc_loop_res(lp);
	munit_assert_ptr_not_null(res);
	xtc_res_set_cap(res, XTC_RES_TASKS,      2 * A1_PROBE_TARGET);
	xtc_res_set_cap(res, XTC_RES_FDS,        2 * A1_PROBE_TARGET);
	xtc_res_set_cap(res, XTC_RES_INBOX_MSGS, 2 * A1_PROBE_TARGET);
	xtc_res_set_cap(res, XTC_RES_MEM_BYTES,  8LL * 1024 * 1024 * 1024);

	pids = calloc((size_t)A1_PROBE_TARGET, sizeof *pids);
	munit_assert_ptr_not_null(pids);

	for (i = 0; i < A1_PROBE_TARGET; i++) {
		memset(&o, 0, sizeof o);
		o.name = "a1";
		rc = xtc_proc_spawn(lp, a1_parker, NULL, &o, &pids[i]);
		if (rc != XTC_OK)
			break;
		n = i + 1;
	}

	/* (a) We must be refused -- cleanly -- rather than wrapping.
	 *
	 * The host may run out of address-space mappings first: every fiber
	 * stack is two VMAs (the stack + its PROT_NONE guard page), so the
	 * stock Linux vm.max_map_count of 65530 caps a process near 32 K
	 * live fibers, well short of 65536.  That exhaustion must surface as
	 * XTC_E_NOMEM (releases before 1.50 reported XTC_E_INTERNAL, i.e.
	 * "a bug in xtc", for an ordinary out-of-mappings condition).  The
	 * pid ceiling itself is only reachable on a host whose map limit
	 * admits > 65536 stacks, and there it must be XTC_E_RESOURCE. */
	if (rc == XTC_E_NOMEM && n < 65536) {
		munit_logf(MUNIT_LOG_INFO, "host mapping limit hit at %d procs "
		    "(raise vm.max_map_count to reach the pid ceiling)", n);
	} else {
		munit_assert_int(rc, ==, XTC_E_RESOURCE);
		munit_assert_int(n, <=, 65536);
	}

	/* (b) No two live pids may share (loop_id, local_id).  Check via a
	 * direct-indexed table rather than O(n^2) comparison. */
	{
		unsigned char *seen = calloc(65536, 1);
		int dup = 0;
		munit_assert_ptr_not_null(seen);
		for (i = 0; i < n; i++) {
			if (seen[pids[i].local_id]) { dup = 1; break; }
			seen[pids[i].local_id] = 1;
		}
		free(seen);
		munit_assert_int(dup, ==, 0);
	}

	atomic_store(&g_a1_release, 1);
	free(pids);
	(void)xtc_loop_run(lp);
	(void)xtc_loop_fini(lp);
	return MUNIT_OK;
}


/* ---- xtc_cancel_poll: a cancellation window inside a masked region ---
 *
 * xtc_uncancelable(body) defers a kill so a masked region can finish its
 * bookkeeping.  Sometimes a long masked region wants to CHECK for a
 * pending cancellation at a safe point without abandoning the mask
 * wholesale -- that is xtc_cancel_poll(body, ud): it drops mask_depth to
 * 0 for the duration of `body`, drains any deferred kill (so the fiber
 * may not return), then restores the previous depth.
 *
 * It is public and man-paged and had NO test: the whole
 * save/zero/drain/restore sequence in proc.c was unexecuted.  This
 * covers both shapes -- polling with nothing pending (must run the body
 * and restore the mask), and polling with a kill already deferred (must
 * honor it, so the code after the poll never runs).
 */
static _Atomic int g_cp_body_ran;
static _Atomic int g_cp_after_poll_ran;
static _Atomic int g_cp_finalizer_ran;
static _Atomic int g_cp_nopending_ok;

static int
cp_poll_body(void *ud)
{
	(void)ud;
	atomic_fetch_add(&g_cp_body_ran, 1);
	return 7;
}

/* Masked region that polls with NOTHING pending: the body runs, the
 * return value passes through, and the mask is still in force after. */
static int
cp_nothing_pending(void *ud)
{
	(void)ud;
	if (xtc_cancel_poll(cp_poll_body, NULL) == 7)
		atomic_fetch_add(&g_cp_nopending_ok, 1);
	/* Still masked here: a kill arriving now must not tear us down
	 * before we record that we got this far. */
	atomic_fetch_add(&g_cp_after_poll_ran, 1);
	return 0;
}

static void
cp_proc_nothing(void *arg)
{
	(void)arg;
	(void)xtc_uncancelable(cp_nothing_pending, NULL);
}

/* Masked region that polls AFTER a kill has been deferred: the poll must
 * honor the kill, so the statement after it never executes -- while the
 * at-exit finalizer still runs (the A1+A2 guarantee). */
static int
cp_kill_pending(void *ud)
{
	void  *m = NULL;
	size_t sz = 0;
	(void)ud;
	/*
	 * Kill ourselves while masked.  xtc_exit_pid only LATCHES into
	 * kill_pending; it becomes mask_deferred when a DELIVERY POINT
	 * observes it while masked -- so poll a delivery point (a
	 * zero-timeout recv) to move it into the deferred latch.  Only then
	 * does xtc_cancel_poll's drain have something to honor.
	 */
	(void)xtc_exit_pid(xtc_self(), 42);
	(void)xtc_recv(&m, &sz, 0);   /* delivery point: defers the kill */
	xtc_free(m);
	(void)xtc_cancel_poll(cp_poll_body, NULL);
	/* Unreachable: the poll unmasked and drained the deferred kill. */
	atomic_fetch_add(&g_cp_after_poll_ran, 100);
	return 0;
}

static void
cp_finalizer(void *ud)
{
	(void)ud;
	atomic_fetch_add(&g_cp_finalizer_ran, 1);
}

static void
cp_proc_killed(void *arg)
{
	(void)arg;
	(void)xtc_proc_at_exit(cp_finalizer, NULL);
	(void)xtc_uncancelable(cp_kill_pending, NULL);
}

static MunitResult
test_cancel_poll(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t opts = { 0 };
	xtc_pid_t pid;
	(void)p; (void)d;

	atomic_store(&g_cp_body_ran, 0);
	atomic_store(&g_cp_after_poll_ran, 0);
	atomic_store(&g_cp_finalizer_ran, 0);
	atomic_store(&g_cp_nopending_ok, 0);

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	opts.name = "cp-none";
	munit_assert_int(xtc_proc_spawn(loop, cp_proc_nothing, NULL, &opts,
	    &pid), ==, XTC_OK);
	opts.name = "cp-kill";
	munit_assert_int(xtc_proc_spawn(loop, cp_proc_killed, NULL, &opts,
	    &pid), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	/* Only the nothing-pending proc reaches the poll body: the killed
	 * proc is torn down by the drain INSIDE xtc_cancel_poll, before it
	 * ever calls the body. */
	munit_assert_int(atomic_load(&g_cp_body_ran), ==, 1);
	/* Nothing-pending: the body's value passed through and the code
	 * after the poll still ran (mask restored, not abandoned). */
	munit_assert_int(atomic_load(&g_cp_nopending_ok), ==, 1);
	munit_assert_int(atomic_load(&g_cp_after_poll_ran), ==, 1);
	/* Kill-pending: the poll honored the deferred kill, so the +100
	 * after it never ran -- and the finalizer still did. */
	munit_assert_int(atomic_load(&g_cp_finalizer_ran), ==, 1);

	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* ---- xtc_exit_pid_deadline: report WHICH way a kill went ------------
 *
 * xtc_exit_pid is fire-and-forget -- XTC_OK means "flag set", never
 * "kill landed".  A supervisor could not tell a fiber that is unwinding
 * from one wedged inside xtc_uncancelable(), where the kill is deferred
 * with no bound.  (Reported by the PostgreSQL-on-libxtc integration:
 * pg_ctl -m immediate could not distinguish the two and had to guess
 * after a fixed 5s.)
 *
 * All three statuses are covered, because the DEFERRED one is the whole
 * point and a test that only proved DELIVERED would be the vacuous
 * half: a bug that reported DELIVERED unconditionally would pass it.
 */
static _Atomic int g_kd_masked_entered;
static _Atomic int g_kd_release_mask;

/* A fiber that parks forever WITHOUT a mask: killable, so a kill must
 * be reported DELIVERED. */
static void
kd_proc_killable(void *arg)
{
	void  *m = NULL;
	size_t sz = 0;
	(void)arg;
	/* Infinite recv: a delivery point, so the kill fires here. */
	(void)xtc_recv(&m, &sz, -1);
	xtc_free(m);
}

/* The wedged shape: a masked region that never finishes on its own.
 * This is the PostgreSQL case -- a fiber stuck INSIDE the critical
 * section, where masking makes cancellation safe but cannot make the
 * stuck section killable.  It leaves only when the test says so, so the
 * suite cannot hang if the reporting is wrong. */
static int
kd_masked_body(void *ud)
{
	void  *m = NULL;
	size_t sz = 0;
	(void)ud;
	atomic_store(&g_kd_masked_entered, 1);
	/* Park at delivery points while masked: each one OBSERVES a pending
	 * kill and latches it into mask_deferred (which is exactly what
	 * xtc_exit_pid_deadline reports as DEFERRED), without unwinding. */
	while (!atomic_load(&g_kd_release_mask)) {
		(void)xtc_recv(&m, &sz, 1000000);   /* 1ms */
		xtc_free(m);
		m = NULL;
	}
	return 0;
}

static void
kd_proc_wedged(void *arg)
{
	(void)arg;
	(void)xtc_uncancelable(kd_masked_body, NULL);
}

/* The supervisor fiber: drives the kills and checks the reports. */
struct kd_targets {
	xtc_pid_t killable;
	xtc_pid_t wedged;
	_Atomic int delivered_ok;
	_Atomic int deferred_ok;
	_Atomic int selfkill_rejected;
	_Atomic int dead_ok;
	_Atomic int mask_seen;
};

static void
kd_proc_supervisor(void *arg)
{
	struct kd_targets *t = arg;
	xtc_proc_info_t info;
	int status = -1;

	/* Killing yourself is xtc_exit_self, not this. */
	if (xtc_exit_pid_deadline(xtc_self(), 1, 0, &status) == XTC_E_INVAL)
		atomic_store(&t->selfkill_rejected, 1);

	/* Wait for the wedged fiber to actually be inside its mask, so the
	 * DEFERRED assertion tests the mask and not a startup race. */
	while (!atomic_load(&g_kd_masked_entered))
		(void)xtc_proc_sleep(200000);

	/* (1) An unmasked fiber parked at a delivery point: DELIVERED. */
	status = -1;
	if (xtc_exit_pid_deadline(t->killable, 9, 2000000000LL, &status) ==
	    XTC_OK && status == XTC_KILL_DELIVERED)
		atomic_store(&t->delivered_ok, 1);

	/* (2) The wedged fiber: masked with the kill latched => DEFERRED.
	 * A short deadline is enough BECAUSE the report does not wait it
	 * out -- once the mask is observed holding a latched kill, waiting
	 * longer cannot change the answer. */
	status = -1;
	if (xtc_exit_pid_deadline(t->wedged, 9, 500000000LL, &status) ==
	    XTC_OK && status == XTC_KILL_DEFERRED)
		atomic_store(&t->deferred_ok, 1);

	/* (3) The same state is visible to a supervisor through inspection
	 * (item 2 of the request): "is it stuck in a critical section?" */
	if (xtc_proc_info(t->wedged, &info) == XTC_OK &&
	    info.mask_depth > 0 && info.mask_deferred != 0)
		atomic_store(&t->mask_seen, 1);

	/* (4) Already-dead target reports DELIVERED, not an error: that IS
	 * the outcome the caller asked for. */
	status = -1;
	if (xtc_exit_pid_deadline(t->killable, 9, 0, &status) == XTC_OK &&
	    status == XTC_KILL_DELIVERED)
		atomic_store(&t->dead_ok, 1);

	/* Let the wedged fiber leave its mask; the latched kill fires as the
	 * mask drops, so the loop drains rather than hanging. */
	atomic_store(&g_kd_release_mask, 1);
}

static MunitResult
test_exit_pid_deadline(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t opts = { 0 };
	struct kd_targets t;
	xtc_pid_t sup;
	(void)p; (void)d;

	memset(&t, 0, sizeof t);
	atomic_store(&g_kd_masked_entered, 0);
	atomic_store(&g_kd_release_mask, 0);

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	opts.name = "kd-killable";
	munit_assert_int(xtc_proc_spawn(loop, kd_proc_killable, NULL, &opts,
	    &t.killable), ==, XTC_OK);
	opts.name = "kd-wedged";
	munit_assert_int(xtc_proc_spawn(loop, kd_proc_wedged, NULL, &opts,
	    &t.wedged), ==, XTC_OK);
	opts.name = "kd-sup";
	munit_assert_int(xtc_proc_spawn(loop, kd_proc_supervisor, &t, &opts,
	    &sup), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	munit_assert_int(atomic_load(&t.selfkill_rejected), ==, 1);
	munit_assert_int(atomic_load(&t.delivered_ok), ==, 1);
	/* The one that matters: a wedged critical section is REPORTED as
	 * such instead of silently swallowing the kill. */
	munit_assert_int(atomic_load(&t.deferred_ok), ==, 1);
	munit_assert_int(atomic_load(&t.mask_seen), ==, 1);
	munit_assert_int(atomic_load(&t.dead_ok), ==, 1);

	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* ---- xtc_mask_enter / xtc_mask_leave: the paired mask -------------
 *
 * The callback-free form for macro-pair bridges (PostgreSQL's
 * START_CRIT_SECTION / END_CRIT_SECTION).  Asserts the property the
 * consumer said it would build on: a fiber BETWEEN enter and leave
 * reports mask_depth > 0 and xtc_exit_pid_deadline returns
 * XTC_KILL_DEFERRED, while the same kill OUTSIDE the pair is DELIVERED.
 * That is what turns "never killed mid-mutation" from a documented hope
 * into a mechanically-checkable invariant.
 */
static xtc_pid_t   g_mk_worker;
static _Atomic int g_mk_in_mask;     /* worker is between enter and leave */
static _Atomic int g_mk_release;     /* supervisor tells worker to leave */
static _Atomic int g_mk_reached_after; /* code after the deferred kill ran? */
static _Atomic int g_mk_finalizer_ran;

static void
mk_finalizer(void *ud)
{
	(void)ud;
	atomic_fetch_add(&g_mk_finalizer_ran, 1);
}

static void
mk_worker_proc(void *arg)
{
	void  *m = NULL;
	size_t sz = 0;
	(void)arg;
	(void)xtc_proc_at_exit(mk_finalizer, NULL);

	/* Straight-line masked region -- exactly the shape a
	 * START_CRIT_SECTION() macro bridge produces: enter, then arbitrary
	 * code with its own control flow, then leave.  No callback body. */
	munit_assert_int(xtc_mask_enter(), ==, XTC_OK);
	atomic_store(&g_mk_in_mask, 1);

	/* Park at delivery points while masked so the supervisor's kill is
	 * OBSERVED and latched (mask_deferred) rather than unwinding here. */
	while (!atomic_load(&g_mk_release)) {
		(void)xtc_recv(&m, &sz, 1000000);   /* 1ms */
		xtc_free(m);
		m = NULL;
	}

	atomic_store(&g_mk_in_mask, 0);
	/* Leaving drops the mask to 0 and honors the latched kill: this call
	 * does NOT return, so the line below must never run. */
	(void)xtc_mask_leave();
	atomic_fetch_add(&g_mk_reached_after, 1);   /* must stay 0 */
}

struct mk_sup {
	_Atomic int deferred_ok;   /* kill inside the mask -> DEFERRED */
	_Atomic int mask_seen;     /* xtc_proc_info shows mask_depth > 0 */
};

static void
mk_sup_proc(void *arg)
{
	struct mk_sup *s = arg;
	xtc_proc_info_t info;
	int status = -1;

	while (!atomic_load(&g_mk_in_mask))
		(void)xtc_proc_sleep(200000);

	/* A supervisor can SEE the fiber is inside a critical section... */
	if (xtc_proc_info(g_mk_worker, &info) == XTC_OK &&
	    info.mask_depth > 0)
		atomic_store(&s->mask_seen, 1);

	/* ...and a kill aimed at it is DEFERRED, not delivered, because the
	 * paired mask defers exactly like xtc_uncancelable() would. */
	if (xtc_exit_pid_deadline(g_mk_worker, 42, 500000000LL, &status) ==
	    XTC_OK && status == XTC_KILL_DEFERRED)
		atomic_store(&s->deferred_ok, 1);

	/* Release: the latched kill fires as the mask drops in leave(). */
	atomic_store(&g_mk_release, 1);
}

static MunitResult
test_mask_enter_leave(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t opts = { 0 };
	struct mk_sup s;
	xtc_pid_t sup;
	(void)p; (void)d;

	memset(&s, 0, sizeof s);
	atomic_store(&g_mk_in_mask, 0);
	atomic_store(&g_mk_release, 0);
	atomic_store(&g_mk_reached_after, 0);
	atomic_store(&g_mk_finalizer_ran, 0);

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	opts.name = "mk-worker";
	munit_assert_int(xtc_proc_spawn(loop, mk_worker_proc, NULL, &opts,
	    &g_mk_worker), ==, XTC_OK);
	opts.name = "mk-sup";
	munit_assert_int(xtc_proc_spawn(loop, mk_sup_proc, &s, &opts, &sup),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	/* Inside the pair: visible as masked, and the kill was deferred. */
	munit_assert_int(atomic_load(&s.mask_seen), ==, 1);
	munit_assert_int(atomic_load(&s.deferred_ok), ==, 1);
	/* leave() honored the deferred kill, so the line after it never ran,
	 * while the at-exit finalizer still did (the A1+A2 guarantee). */
	munit_assert_int(atomic_load(&g_mk_reached_after), ==, 0);
	munit_assert_int(atomic_load(&g_mk_finalizer_ran), ==, 1);

	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* enter/leave NEST: N enters need N leaves, only the outermost unwinds;
 * and off a proc both return XTC_E_INVAL (so a bridge macro can ignore
 * the return).  A pure-logic check, no kill involved. */
static _Atomic int g_mkn_after_inner, g_mkn_after_outer;

static void
mkn_proc(void *arg)
{
	(void)arg;
	/* Depth 2, then a self-kill latched at a delivery point. */
	(void)xtc_mask_enter();
	(void)xtc_mask_enter();
	{
		void *m = NULL; size_t sz = 0;
		(void)xtc_exit_pid(xtc_self(), 7);
		(void)xtc_recv(&m, &sz, 0);   /* delivery point: defers it */
		xtc_free(m);
	}
	(void)xtc_mask_leave();                 /* depth 2->1: must NOT unwind */
	atomic_fetch_add(&g_mkn_after_inner, 1);/* must reach here (== 1) */
	(void)xtc_mask_leave();                 /* depth 1->0: honors the kill */
	atomic_fetch_add(&g_mkn_after_outer, 1);/* must NOT reach here (== 0) */
}

static MunitResult
test_mask_nesting(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t opts = { 0 };
	xtc_pid_t pid;
	(void)p; (void)d;

	/* Off a proc: both reject cleanly. */
	munit_assert_int(xtc_mask_enter(), ==, XTC_E_INVAL);
	munit_assert_int(xtc_mask_leave(), ==, XTC_E_INVAL);

	atomic_store(&g_mkn_after_inner, 0);
	atomic_store(&g_mkn_after_outer, 0);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	opts.name = "mkn";
	munit_assert_int(xtc_proc_spawn(loop, mkn_proc, NULL, &opts, &pid),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	/* The inner leave only decremented; the outer leave unwound. */
	munit_assert_int(atomic_load(&g_mkn_after_inner), ==, 1);
	munit_assert_int(atomic_load(&g_mkn_after_outer), ==, 0);

	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* ---- cross-thread send WAKES a parked receiver ----------------------
 *
 * xtc_send's documented WAKE GUARANTEE: a successful send makes a
 * receiver parked in xtc_recv runnable, from ANY thread -- including a
 * plain OS thread with no loop and no proc identity (envelope `from` is
 * XTC_PID_NONE).  Windows had this covered (test/msvc/smoke.c scenario
 * 2, via PostQueuedCompletionStatus); POSIX did not, so the epoll /
 * io_uring inbox+wakeup path had no gate.
 *
 * Shape taken from a consumer report of a wedged per-loop supervisor: a
 * non-fiber "postmaster" thread bursts sends at fibers blocked in
 * xtc_recv(-1) while the loop thread sits in epoll_wait/io_uring.  A lost
 * wake here leaves the receiver PARKED with a non-empty mailbox forever
 * -- so this test HANGS on regression rather than failing fast, which is
 * why the receivers exit and let xtc_loop_run return only once every send
 * has been received (n_alive -> 0).
 */
#if !defined(_WIN32)
#define XT_N          64
#define XT_PER_PROC   16

static xtc_pid_t   g_xt_pid[XT_N];
static _Atomic int g_xt_ready;
static _Atomic int g_xt_recv;
static _Atomic int g_xt_sent;

static void
xt_worker(void *arg)
{
	int want = XT_PER_PROC;
	(void)arg;
	atomic_fetch_add(&g_xt_ready, 1);
	while (want > 0) {
		void  *m = NULL;
		size_t sz = 0;
		/* Infinite park: ONLY a send can wake this.  No timeout to
		 * paper over a lost wake with a timer-driven re-check. */
		if (xtc_recv(&m, &sz, -1) != XTC_OK)
			continue;
		xtc_free(m);
		atomic_fetch_add(&g_xt_recv, 1);
		want--;
	}
}

/* A plain OS thread: no loop, no proc, xtc_self() == XTC_PID_NONE. */
static void *
xt_sender(void *u)
{
	int i, k;
	(void)u;
	while (atomic_load(&g_xt_ready) < XT_N)
		(void)usleep(1000);
	/* Burst every target, retrying only the backpressure code so a full
	 * mailbox is not miscounted as a lost wake. */
	for (i = 0; i < XT_PER_PROC; i++) {
		for (k = 0; k < XT_N; k++) {
			unsigned char msg[24];
			memset(msg, (unsigned char)i, sizeof msg);
			for (;;) {
				int rc = xtc_send(g_xt_pid[k], msg, sizeof msg);
				if (rc == XTC_OK) {
					atomic_fetch_add(&g_xt_sent, 1);
					break;
				}
				if (rc != XTC_E_AGAIN && rc != XTC_E_RESOURCE)
					break;   /* real error: counted below */
				(void)usleep(200);
			}
		}
	}
	return NULL;
}

static MunitResult
test_cross_thread_send_wakes(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t o = { 0 };
	pthread_t th;
	int i;
	(void)p; (void)d;

	atomic_store(&g_xt_ready, 0);
	atomic_store(&g_xt_recv, 0);
	atomic_store(&g_xt_sent, 0);

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	o.name = "xt-worker";
	for (i = 0; i < XT_N; i++)
		munit_assert_int(xtc_proc_spawn(loop, xt_worker, NULL, &o,
		    &g_xt_pid[i]), ==, XTC_OK);
	munit_assert_int(pthread_create(&th, NULL, xt_sender, NULL), ==, 0);

	/* Returns only when every worker got all its messages and exited.
	 * A lost wake strands a worker here (n_alive never reaches 0). */
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(pthread_join(th, NULL), ==, 0);

	munit_assert_int(atomic_load(&g_xt_sent), ==, XT_N * XT_PER_PROC);
	munit_assert_int(atomic_load(&g_xt_recv), ==, XT_N * XT_PER_PROC);

	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}
#endif /* !_WIN32 */

/* ---- arena groups: wholesale shared-state discard on kill --------
 *
 * The two-phase safety property: discard() kills every member, waits
 * until ALL are gone, and only THEN resets the arena.  A killable
 * cohort -> arena reset (all_gone=1).  A cohort with a fiber WEDGED
 * inside xtc_uncancelable -> the deferred kill leaves it alive, so the
 * arena is NOT reset (all_gone=0) and the caller must escalate.
 */
static xtc_arena_group_t *g_ag;
static _Atomic int g_ag_ready;      /* members that have added + parked */
static _Atomic int g_ag_release;

static void
ag_member_proc(void *arg)
{
	void  *m = NULL;
	size_t sz = 0;
	int wedge = (int)(intptr_t)arg;
	/* Join the group and allocate SHARED state in its arena. */
	(void)xtc_arena_group_add(g_ag);
	(void)xtc_mctx_alloc(xtc_arena_group_mctx(g_ag), 128);
	atomic_fetch_add(&g_ag_ready, 1);
	if (wedge) {
		/* Wedged inside the mask: observe the kill (park at a
		 * delivery point) but defer it, so discard's deadline
		 * expires with this fiber still alive. */
		(void)xtc_mask_enter();
		while (!atomic_load(&g_ag_release)) {
			(void)xtc_recv(&m, &sz, 1000000);
			xtc_free(m); m = NULL;
		}
		(void)xtc_mask_leave();
	} else {
		/* Killable: an unmasked infinite park honors the kill. */
		(void)xtc_recv(&m, &sz, -1);
		xtc_free(m);
	}
}

struct ag_sup {
	_Atomic int all_gone;
	_Atomic int rc;
	_Atomic int chunks_after;
	_Atomic int done;
};

static void
ag_sup_proc(void *arg)
{
	struct ag_sup *s = arg;
	int all_gone = -1, r;

	while (atomic_load(&g_ag_ready) < 2)
		(void)xtc_proc_sleep(200000);
	/* Let the wedged member reach its mask before we kill. */
	(void)xtc_proc_sleep(20LL * 1000 * 1000);

	r = xtc_arena_group_discard(g_ag, 9, 300000000LL, &all_gone);
	atomic_store(&s->rc, r);
	atomic_store(&s->all_gone, all_gone);
	/* Arena reset iff all_gone: chunks drop to 0 only then. */
	atomic_store(&s->chunks_after,
	    (int)xtc_mctx_total_chunks(xtc_arena_group_mctx(g_ag)));
	atomic_store(&s->done, 1);

	/* Release any wedged member so the loop can drain and finish. */
	atomic_store(&g_ag_release, 1);
}

static MunitResult
ag_run(int wedge, int expect_all_gone, int expect_chunks_zero)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t opts = { 0 };
	struct ag_sup s;
	xtc_pid_t p;

	memset(&s, 0, sizeof s);
	atomic_store(&g_ag_ready, 0);
	atomic_store(&g_ag_release, 0);
	munit_assert_int(xtc_arena_group_create("ag", &g_ag), ==, XTC_OK);

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	opts.name = "ag-killable";
	munit_assert_int(xtc_proc_spawn(loop, ag_member_proc, (void *)0,
	    &opts, &p), ==, XTC_OK);
	opts.name = "ag-other";
	munit_assert_int(xtc_proc_spawn(loop, ag_member_proc,
	    (void *)(intptr_t)wedge, &opts, &p), ==, XTC_OK);
	opts.name = "ag-sup";
	munit_assert_int(xtc_proc_spawn(loop, ag_sup_proc, &s, &opts, &p),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	munit_assert_int(atomic_load(&s.done), ==, 1);
	munit_assert_int(atomic_load(&s.rc), ==, XTC_OK);
	munit_assert_int(atomic_load(&s.all_gone), ==, expect_all_gone);
	if (expect_chunks_zero)
		munit_assert_int(atomic_load(&s.chunks_after), ==, 0);
	else
		munit_assert_int(atomic_load(&s.chunks_after), >, 0);

	xtc_arena_group_destroy(g_ag);
	g_ag = NULL;
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

static MunitResult
test_arena_group_discard(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	/* Killable cohort: every member dies, arena is reset (chunks -> 0). */
	return ag_run(/*wedge=*/0, /*all_gone=*/1, /*chunks_zero=*/1);
}

static MunitResult
test_arena_group_wedged(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	/* One member wedged in a mask: kill is deferred, it stays alive at
	 * the deadline, so all_gone=0 and the arena is NOT reset -- its
	 * chunks survive.  Discarding under a live member is exactly the
	 * corruption the two-phase order avoids. */
	return ag_run(/*wedge=*/1, /*all_gone=*/0, /*chunks_zero=*/0);
}

/* ---- xtc_exit_pid_deadline: unknown pid vs already-dead pid ----------
 *
 * PLAN 19.27.13.  An UNKNOWN pid is a caller bug and must be XTC_E_INVAL,
 * as xtc_exit_pid says; an ALREADY-DEAD pid is the outcome the caller
 * wanted and must be XTC_OK + DELIVERED -- including while the target is
 * still in the table running its at-exit hooks.  Pre-fix both were
 * backwards: a never-issued pid got XTC_OK + DELIVERED, and a target
 * inside its at-exit hook got XTC_E_INVAL.
 */
static _Atomic int g_ku_in_hook, g_ku_release_hook;
static _Atomic int g_ku_gen_rc, g_ku_gen_st, g_ku_cap_rc, g_ku_loop_rc;
static _Atomic int g_ku_dying_rc, g_ku_dying_st, g_ku_dead_rc, g_ku_dead_st;
static xtc_pid_t g_ku_victim;

static void
ku_hook(void *arg)
{
	(void)arg;
	atomic_store(&g_ku_in_hook, 1);
	while (!atomic_load(&g_ku_release_hook))
		(void)xtc_proc_sleep(1000000);
}

static void
ku_victim_proc(void *arg)
{
	(void)arg;
	munit_assert_int(xtc_proc_at_exit(ku_hook, NULL), ==, XTC_OK);
}

static void
ku_checker_proc(void *arg)
{
	xtc_proc_info_t info;
	xtc_pid_t bogus;
	int st, spins = 0;
	(void)arg;

	/* Never issued: a generation the victim's slot has not reached. */
	bogus = g_ku_victim;
	bogus.gen += 1000;
	st = -1;
	atomic_store(&g_ku_gen_rc, xtc_exit_pid_deadline(bogus, 9, 0, &st));
	atomic_store(&g_ku_gen_st, st);
	/* Never issued: a slot index past the table. */
	bogus = g_ku_victim;
	bogus.local_id = 60000;
	atomic_store(&g_ku_cap_rc, xtc_exit_pid_deadline(bogus, 9, 0, NULL));
	/* Never issued: a loop that does not exist. */
	bogus = g_ku_victim;
	bogus.loop_id = 77;
	atomic_store(&g_ku_loop_rc, xtc_exit_pid_deadline(bogus, 9, 0, NULL));

	/* Dying: body returned, at-exit hook parked, pid still in table. */
	while (!atomic_load(&g_ku_in_hook) && spins++ < 5000)
		(void)xtc_proc_sleep(1000000);
	st = -1;
	atomic_store(&g_ku_dying_rc,
	    xtc_exit_pid_deadline(g_ku_victim, 9, 0, &st));
	atomic_store(&g_ku_dying_st, st);

	/* Dead and reaped: still DELIVERED (the control). */
	atomic_store(&g_ku_release_hook, 1);
	spins = 0;
	while (xtc_proc_info(g_ku_victim, &info) == XTC_OK && spins++ < 5000)
		(void)xtc_proc_sleep(1000000);
	st = -1;
	atomic_store(&g_ku_dead_rc,
	    xtc_exit_pid_deadline(g_ku_victim, 9, 0, &st));
	atomic_store(&g_ku_dead_st, st);
}

static MunitResult
test_exit_pid_deadline_unknown(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t opts = { 0 };
	xtc_pid_t checker;
	(void)p; (void)d;

	atomic_store(&g_ku_in_hook, 0);
	atomic_store(&g_ku_release_hook, 0);
	atomic_store(&g_ku_gen_rc, 12345);
	atomic_store(&g_ku_dying_rc, 12345);
	atomic_store(&g_ku_dead_rc, 12345);

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	opts.name = "ku-victim";
	munit_assert_int(xtc_proc_spawn(loop, ku_victim_proc, NULL, &opts,
	    &g_ku_victim), ==, XTC_OK);
	opts.name = "ku-checker";
	munit_assert_int(xtc_proc_spawn(loop, ku_checker_proc, NULL, &opts,
	    &checker), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	/* The header contract, matching xtc_exit_pid: unknown -> INVAL, and
	 * the out-status is not written. */
	munit_assert_int(atomic_load(&g_ku_gen_rc), ==, XTC_E_INVAL);
	munit_assert_int(atomic_load(&g_ku_gen_st), ==, -1);
	munit_assert_int(atomic_load(&g_ku_cap_rc), ==, XTC_E_INVAL);
	munit_assert_int(atomic_load(&g_ku_loop_rc), ==, XTC_E_INVAL);
	/* Already dead -> DELIVERED, whether unwinding or reaped. */
	munit_assert_int(atomic_load(&g_ku_in_hook), ==, 1);
	munit_assert_int(atomic_load(&g_ku_dying_rc), ==, XTC_OK);
	munit_assert_int(atomic_load(&g_ku_dying_st), ==, XTC_KILL_DELIVERED);
	munit_assert_int(atomic_load(&g_ku_dead_rc), ==, XTC_OK);
	munit_assert_int(atomic_load(&g_ku_dead_st), ==, XTC_KILL_DELIVERED);

	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* ---- cancellation safety in xtc_proc_wait_fd + the exit path -------
 *
 * Four regressions, each of which FAILED (hung, or returned the wrong
 * code) before the fix it guards.  A pipe read end that is never written
 * is the "never ready" fd throughout: it makes the fd wake source
 * provably absent, so what is left is exactly the path under test.
 */

/* [wait_fd/wake_is_not_timeout] An explicit wake of a TIMED wait must
 * report XTC_OK, not XTC_E_AGAIN.  Pre-fix, wait_fd returned XTC_E_AGAIN
 * whenever no non-timeout bit was seen and a timeout had been SUPPLIED,
 * so a 1s wait woken at ~10ms reported "deadline expired" with revents
 * == 0 and consumers ended waits ~990ms early. */
static _Atomic int g_wnt_rc, g_wnt_revents, g_wnt_early;
static xtc_pid_t g_wnt_victim;

static void
wnt_victim_proc(void *arg)
{
	int rfd = (int)(intptr_t)arg;
	uint32_t revents = 0;
	int64_t t0, t1;
	int rc;

	t0 = xtc_clock_mono();
	rc = xtc_proc_wait_fd(rfd, XTC_IO_READABLE,
	    2000LL * 1000 * 1000, &revents);
	t1 = xtc_clock_mono();
	atomic_store(&g_wnt_rc, rc);
	atomic_store(&g_wnt_revents, (int)revents);
	/* Woken well before the 2s deadline: proves the wake -- not an
	 * expiry -- is what returned, independently of the return code. */
	atomic_store(&g_wnt_early, (t1 - t0) < 1000LL * 1000 * 1000);
}

static void
wnt_waker_proc(void *arg)
{
	(void)arg;
	(void)xtc_proc_sleep(20LL * 1000 * 1000);
	(void)xtc_proc_wake(g_wnt_victim);
}

static MunitResult
test_wait_fd_wake_is_not_timeout(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t opts = { 0 };
	xtc_pid_t other;
	int pipefd[2];
	(void)p; (void)d;

	atomic_store(&g_wnt_rc, 12345);
	atomic_store(&g_wnt_revents, -1);
	atomic_store(&g_wnt_early, 0);

	munit_assert_int(xtc_test_make_pipe(&pipefd[0], &pipefd[1]), ==, 0);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	opts.name = "wnt-victim";
	munit_assert_int(xtc_proc_spawn(loop, wnt_victim_proc,
	    (void *)(intptr_t)pipefd[0], &opts, &g_wnt_victim), ==, XTC_OK);
	opts.name = "wnt-waker";
	munit_assert_int(xtc_proc_spawn(loop, wnt_waker_proc, NULL, &opts,
	    &other), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	/* It really was the wake, not the deadline. */
	munit_assert_int(atomic_load(&g_wnt_early), ==, 1);
	/* The timeout bit must NOT be set -- the timer never fired. */
	munit_assert_int(atomic_load(&g_wnt_revents) & XTC_WAIT_TIMEOUT,
	    ==, 0);
	/* THE REGRESSION: a non-timeout wake returns XTC_OK (xtc_proc.h). */
	munit_assert_int(atomic_load(&g_wnt_rc), ==, XTC_OK);

	xtc_test_close_pipe(pipefd[0], pipefd[1]);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* [wait_fd/kill_releases_registration] A KILLED fd-waiter must leave no
 * fd registration behind.  Pre-fix the kill unwound inside xtc_yield --
 * before wait_fd's unregister ran -- so the dead proc's registration
 * survived and the NEXT waiter on that same fd got XTC_E_INTERNAL from
 * the duplicate-registration reject instead of its own timeout.  Proc
 * exit does not cover this: the recovery registry and at-exit hooks know
 * nothing about scheduler park state. */
static _Atomic int g_krr_second_rc, g_krr_second_revents;
static xtc_pid_t g_krr_victim;

static void
krr_victim_proc(void *arg)
{
	int rfd = (int)(intptr_t)arg;
	uint32_t revents = 0;
	/* Infinite wait on an fd that never becomes ready: the only way out
	 * is the kill. */
	(void)xtc_proc_wait_fd(rfd, XTC_IO_READABLE, -1, &revents);
}

static void
krr_reuser_proc(void *arg)
{
	int rfd = (int)(intptr_t)arg;
	uint32_t revents = 0;
	int rc;

	/* Let the victim reach its park, then kill it and let it unwind. */
	(void)xtc_proc_sleep(30LL * 1000 * 1000);
	munit_assert_int(xtc_exit_pid(g_krr_victim, 42), ==, XTC_OK);
	(void)xtc_proc_sleep(30LL * 1000 * 1000);

	/* Re-wait on the SAME fd.  This must behave like a fresh wait and
	 * time out; XTC_E_INTERNAL here means the dead proc's registration
	 * is still in the loop's registry. */
	rc = xtc_proc_wait_fd(rfd, XTC_IO_READABLE, 30LL * 1000 * 1000,
	    &revents);
	atomic_store(&g_krr_second_rc, rc);
	atomic_store(&g_krr_second_revents, (int)revents);
}

static MunitResult
test_wait_fd_kill_releases_registration(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t opts = { 0 };
	xtc_pid_t reuser;
	int pipefd[2];
	(void)p; (void)d;

	atomic_store(&g_krr_second_rc, 12345);
	atomic_store(&g_krr_second_revents, -1);

	munit_assert_int(xtc_test_make_pipe(&pipefd[0], &pipefd[1]), ==, 0);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	opts.name = "krr-victim";
	munit_assert_int(xtc_proc_spawn(loop, krr_victim_proc,
	    (void *)(intptr_t)pipefd[0], &opts, &g_krr_victim), ==, XTC_OK);
	opts.name = "krr-reuser";
	munit_assert_int(xtc_proc_spawn(loop, krr_reuser_proc,
	    (void *)(intptr_t)pipefd[0], &opts, &reuser), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	/* THE REGRESSION: pre-fix this was XTC_E_INTERNAL (a leaked
	 * registration); it must be the clean timeout the wait asked for. */
	munit_assert_int(atomic_load(&g_krr_second_rc), ==, XTC_E_AGAIN);
	munit_assert_int(atomic_load(&g_krr_second_revents) &
	    XTC_WAIT_TIMEOUT, ==, XTC_WAIT_TIMEOUT);

	xtc_test_close_pipe(pipefd[0], pipefd[1]);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* [wait_fd/mailbox_wake_not_lost] A message or kill delivered by a
 * FOREIGN OS thread while a proc is ENTERING wait_fd must never be parked
 * on.  Pre-fix the mailbox check and the waker arming sat in two separate
 * mbox_lock holds with the fd/timer registration in between; a sender or
 * killer landing in that gap pushed its message / latched its kill, found
 * waker_armed == 0, fired NO waker, and the proc then parked on the
 * already-pending event.  The fd here is a pipe read end that is NEVER
 * written and the timeout is INFINITE, so the gap wake is the only way
 * out: a lost wake is an unconditional hang, not a slow test.
 *
 * The window is a few instructions wide, so timing alone does not hit it
 * -- a purely time-based version of this test PASSED on the unfixed
 * source, i.e. proved nothing.  It is therefore driven through the
 * "proc.wait_fd.armed" injection point: the callback runs ON the victim's
 * loop thread and holds it until a foreign thread has done its send/kill,
 * so the foreign action provably lands in the window.  Placing the same
 * hook at the pre-fix gap makes this test hang on the unfixed source,
 * which is how it was verified to have teeth.
 */
#if !defined(_WIN32)
static _Atomic int g_wnl_in_window, g_wnl_release, g_wnl_returned;
static _Atomic int g_wnl_rc, g_wnl_revents;
static xtc_pid_t g_wnl_victim;
static int g_wnl_mode;               /* 0 = send, 1 = kill */

/* Runs on the victim's loop thread, inside the check/arm window. */
static void
wnl_window_cb(const char *name, void *user)
{
	int spins = 0;
	(void)name; (void)user;
	if (atomic_exchange(&g_wnl_in_window, 1))
		return;                      /* hold only the first pass */
	while (!atomic_load(&g_wnl_release) && spins++ < 200000)
		__os_sleep_ns(100LL * 1000);
}

static void
wnl_victim_proc(void *arg)
{
	int rfd = (int)(intptr_t)arg;
	uint32_t revents = 0;
	int rc;

	rc = xtc_proc_wait_fd(rfd, XTC_IO_READABLE, -1, &revents);
	atomic_store(&g_wnl_rc, rc);
	atomic_store(&g_wnl_revents, (int)revents);
	atomic_store(&g_wnl_returned, 1);
}

static void *
wnl_racer_thread(void *arg)
{
	int spins = 0;
	(void)arg;
	/* Wait until the victim is provably stopped inside the window. */
	while (!atomic_load(&g_wnl_in_window) && spins++ < 200000)
		__os_sleep_ns(100LL * 1000);
	if (atomic_load(&g_wnl_in_window)) {
		if (g_wnl_mode == 0) {
			int v = 7;
			(void)xtc_send(g_wnl_victim, &v, sizeof v);
		} else {
			(void)xtc_exit_pid(g_wnl_victim, 42);
		}
	}
	atomic_store(&g_wnl_release, 1);
	return NULL;
}

static MunitResult
wnl_run(int mode)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t opts = { 0 };
	pthread_t racer;
	int pipefd[2];

	g_wnl_mode = mode;
	atomic_store(&g_wnl_in_window, 0);
	atomic_store(&g_wnl_release, 0);
	atomic_store(&g_wnl_returned, 0);
	atomic_store(&g_wnl_rc, 12345);
	atomic_store(&g_wnl_revents, -1);

	munit_assert_int(xtc_test_make_pipe(&pipefd[0], &pipefd[1]), ==, 0);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_inject_attach("proc.wait_fd.armed",
	    wnl_window_cb, NULL), ==, XTC_OK);
	opts.name = "wnl-victim";
	munit_assert_int(xtc_proc_spawn(loop, wnl_victim_proc,
	    (void *)(intptr_t)pipefd[0], &opts, &g_wnl_victim), ==, XTC_OK);
	munit_assert_int(pthread_create(&racer, NULL, wnl_racer_thread,
	    NULL), ==, 0);
	/* THE REGRESSION: pre-fix this loop_run never returns -- the victim
	 * parks forever on the message/kill delivered in the window. */
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(pthread_join(racer, NULL), ==, 0);
	munit_assert_int(xtc_inject_detach("proc.wait_fd.armed"), ==, XTC_OK);

	/* The window really was held; otherwise the test proved nothing. */
	munit_assert_int(atomic_load(&g_wnl_in_window), ==, 1);
	if (mode == 0) {
		/* Sent: wait_fd returned and reported the mailbox. */
		munit_assert_int(atomic_load(&g_wnl_returned), ==, 1);
		munit_assert_int(atomic_load(&g_wnl_rc), ==, XTC_OK);
		munit_assert_int(atomic_load(&g_wnl_revents) &
		    XTC_WAIT_MAILBOX, ==, XTC_WAIT_MAILBOX);
	} else {
		/* Killed: the proc unwound instead of parking, so wait_fd
		 * never returned normally. */
		munit_assert_int(atomic_load(&g_wnl_returned), ==, 0);
	}

	xtc_test_close_pipe(pipefd[0], pipefd[1]);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

static MunitResult
test_wait_fd_mailbox_wake_not_lost(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	return wnl_run(/*mode=*/0);
}

static MunitResult
test_wait_fd_kill_wake_not_lost(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	return wnl_run(/*mode=*/1);
}
#endif /* !_WIN32 */

#if !defined(_WIN32)
/* [wait_fd/closed_fd_is_inval] PLAN 19.27.13.  Waiting on a CLOSED fd
 * is a programming error and must say so: XTC_E_INVAL, no park, no
 * revents.  Pre-fix it presented as something else on every backend --
 * io_uring returned XTC_E_AGAIN + XTC_WAIT_TIMEOUT for timeout 0 and
 * XTC_OK + XTC_IO_ERR for a finite or infinite one; epoll returned
 * XTC_E_INTERNAL.  The infinite wait is included on purpose: it must not
 * park.  Control: the surviving read end of the same pipe (EOF) works. */
static _Atomic int g_cf_rc[3], g_cf_rev[3], g_cf_ctl_rc, g_cf_ran;

static void
cf_proc(void *arg)
{
	static const int64_t to[3] = { 0, 50LL * 1000 * 1000, -1 };
	int pipefd[2], i;
	uint32_t revents;
	(void)arg;
	munit_assert_int(xtc_test_make_pipe(&pipefd[0], &pipefd[1]), ==, 0);
	/* Close the WRITE end and probe it: a genuinely closed fd.  The
	 * control below then waits READABLE on the surviving read end, which
	 * is readable at once (EOF: its writer is gone) on every backend.
	 * The first cut closed the read end and used "write end WRITABLE" as
	 * the control -- but a pipe with no reader is not a portable
	 * writability case: FreeBSD's kqueue refuses EVFILT_WRITE on it
	 * (EPIPE -> XTC_E_INTERNAL), which failed the CI freebsd job. */
	munit_assert_int(close(pipefd[1]), ==, 0);
	for (i = 0; i < 3; i++) {
		revents = 0;
		atomic_store(&g_cf_rc[i], xtc_proc_wait_fd(pipefd[1],
		    XTC_IO_READABLE, to[i], &revents));
		atomic_store(&g_cf_rev[i], (int)revents);
	}
	revents = 0;
	atomic_store(&g_cf_ctl_rc, xtc_proc_wait_fd(pipefd[0],
	    XTC_IO_READABLE, 1000LL * 1000 * 1000, &revents));
	(void)close(pipefd[0]);
	atomic_store(&g_cf_ran, 1);
}

static MunitResult
test_wait_fd_closed_fd_is_inval(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	xtc_pid_t pid;
	int i;
	(void)p; (void)d;
	for (i = 0; i < 3; i++) {
		atomic_store(&g_cf_rc[i], 12345);
		atomic_store(&g_cf_rev[i], -1);
	}
	atomic_store(&g_cf_ran, 0);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, cf_proc, NULL, NULL, &pid),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(atomic_load(&g_cf_ran), ==, 1);
	for (i = 0; i < 3; i++) {
		munit_assert_int(atomic_load(&g_cf_rc[i]), ==, XTC_E_INVAL);
		munit_assert_int(atomic_load(&g_cf_rev[i]), ==, 0);
	}
	munit_assert_int(atomic_load(&g_cf_ctl_rc), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}
#endif /* !_WIN32 */

/* [at_exit/park_after_kill] An at-exit hook may PARK even when the proc
 * is dying from an async KILL.  Pre-fix kill_pending was still latched
 * when the hooks ran, so the hook's first park point re-delivered the
 * same kill, longjmp'd back to the exit path, and re-ran the whole
 * at-exit list -- unboundedly, with the hook's xtc_proc_sleep never
 * returning.  The hook must run EXACTLY ONCE and its park must complete.
 *
 * `kill` parameterises the control: on a CLEAN exit the identical hook
 * always worked, which is what isolated the defect to the kill path. */
static _Atomic int g_pak_entered, g_pak_sleep_rc, g_pak_completed;
static _Atomic int g_pak_kill_status;
static xtc_pid_t g_pak_victim;

static void
pak_hook(void *arg)
{
	(void)arg;
	atomic_fetch_add(&g_pak_entered, 1);
	/* The park that used to re-trigger the latched kill. */
	atomic_store(&g_pak_sleep_rc, xtc_proc_sleep(20LL * 1000 * 1000));
	atomic_fetch_add(&g_pak_completed, 1);
}

static void
pak_victim_proc(void *arg)
{
	int killed = (int)(intptr_t)arg;
	void *msg = NULL;
	size_t sz = 0;

	munit_assert_int(xtc_proc_at_exit(pak_hook, NULL), ==, XTC_OK);
	if (!killed) {
		(void)xtc_proc_sleep(10LL * 1000 * 1000);
		return;                  /* control: clean exit */
	}
	/* Park forever; the killer ends this. */
	(void)xtc_recv(&msg, &sz, -1);
	if (msg != NULL) xtc_free(msg);
}

static void
pak_killer_proc(void *arg)
{
	int st = -1;
	int rc;
	(void)arg;
	(void)xtc_proc_sleep(30LL * 1000 * 1000);
	rc = xtc_exit_pid_deadline(g_pak_victim, 9, 2000LL * 1000 * 1000,
	    &st);
	munit_assert_int(rc, ==, XTC_OK);
	atomic_store(&g_pak_kill_status, st);
}

static MunitResult
pak_run(int killed)
{
	xtc_loop_t *loop = NULL;
	xtc_proc_opts_t opts = { 0 };
	xtc_pid_t killer;

	atomic_store(&g_pak_entered, 0);
	atomic_store(&g_pak_sleep_rc, 12345);
	atomic_store(&g_pak_completed, 0);
	atomic_store(&g_pak_kill_status, -1);

	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	opts.name = "pak-victim";
	munit_assert_int(xtc_proc_spawn(loop, pak_victim_proc,
	    (void *)(intptr_t)killed, &opts, &g_pak_victim), ==, XTC_OK);
	if (killed) {
		opts.name = "pak-killer";
		munit_assert_int(xtc_proc_spawn(loop, pak_killer_proc, NULL,
		    &opts, &killer), ==, XTC_OK);
	}
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	/* THE REGRESSION: exactly one entry (pre-fix: hundreds), and the
	 * hook's own park completed (pre-fix: it never returned). */
	munit_assert_int(atomic_load(&g_pak_entered), ==, 1);
	munit_assert_int(atomic_load(&g_pak_sleep_rc), ==, XTC_OK);
	munit_assert_int(atomic_load(&g_pak_completed), ==, 1);
	if (killed) {
		/* The kill still lands: masking the hook run must not turn a
		 * delivered kill into a DEFERRED/TIMEOUT report. */
		munit_assert_int(atomic_load(&g_pak_kill_status), ==,
		    XTC_KILL_DELIVERED);
	}

	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

static MunitResult
test_at_exit_park_after_kill(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	return pak_run(/*killed=*/1);
}

static MunitResult
test_at_exit_park_clean_control(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	/* Control: the same parking hook on a clean exit.  This ALWAYS
	 * passed, which is what proved the defect was kill-specific rather
	 * than "a hook may not park". */
	return pak_run(/*killed=*/0);
}

static MunitTest tests[] = {
	{ "/send_recv_basic",   test_send_recv_basic,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/self",              test_self,             NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/userdata",          test_userdata,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/selective_receive", test_selective_receive,NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/monitor",           test_monitor,          NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/spawn_monitor_atomic", test_spawn_monitor_atomic, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/spawn_link_atomic",  test_spawn_link_atomic, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/spawn_rel_needs_proc", test_spawn_rel_needs_proc, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/down_decode_ex",      test_down_decode_ex_exit_vs_signal, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/recv_inf_parks",    test_recv_inf_parks,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/mailbox_stats",     test_mailbox_stats,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/save_queue_cap",    test_save_queue_cap,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/fault_contain",     test_fault_contain,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/fault_early_contain", test_fault_early_contain, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/recovery_registry", test_recovery_registry, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/fault_escalate",    test_fault_escalate,   NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/proc_at_exit",      test_proc_at_exit,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/pid_local_id_ceiling", test_pid_local_id_ceiling, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/cancel_poll",       test_cancel_poll,      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/mask_enter_leave",  test_mask_enter_leave, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/mask_nesting",      test_mask_nesting,     NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/arena_group_discard", test_arena_group_discard, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/arena_group_wedged",  test_arena_group_wedged,  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/exit_pid_deadline", test_exit_pid_deadline, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/exit_pid_deadline_unknown", test_exit_pid_deadline_unknown, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/wait_fd_wake_is_not_timeout", test_wait_fd_wake_is_not_timeout, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/wait_fd_kill_releases_registration", test_wait_fd_kill_releases_registration, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
#if !defined(_WIN32)
	{ "/wait_fd_mailbox_wake_not_lost", test_wait_fd_mailbox_wake_not_lost, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/wait_fd_kill_wake_not_lost", test_wait_fd_kill_wake_not_lost, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/wait_fd_closed_fd_is_inval", test_wait_fd_closed_fd_is_inval, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
#endif
	{ "/at_exit_park_after_kill", test_at_exit_park_after_kill, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/at_exit_park_clean_control", test_at_exit_park_clean_control, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
#if !defined(_WIN32)
	{ "/cross_thread_send_wakes", test_cross_thread_send_wakes, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
#endif
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m8/proc", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
