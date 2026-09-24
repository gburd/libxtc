/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/coverage/test_oom_sweep.c -- PLAN 19.27.19.
 *
 *	Allocation-failure sweep over public entry points.  For each target
 *	operation, fail the Nth allocation made through the allocator hook
 *	(__os_alloc_set_hook) for N = 1, 2, ... until the operation completes
 *	with no injected failure, and assert for EVERY N:
 *
 *	  - the call returns an error code, never XTC_OK, when an allocation
 *	    it depends on failed (a success that swallowed the failure would
 *	    hand the caller a half-built object);
 *	  - on XTC_OK the out-param is set (never XTC_OK with NULL);
 *	  - nothing leaks: every allocation made through the hook during the
 *	    step is freed by the time the step's own teardown returns (live
 *	    count through the same hook, the way test/m3/test_timer.c Tm12
 *	    counts);
 *	  - no crash (munit forks each case; an abort or SIGSEGV fails it).
 *
 *	test/coverage/test_fault_inject.c /oom_sweep walks a mixed workload
 *	and relies on ASan to notice a leak; this sweep asserts per target,
 *	per N, in every build.
 *
 *	Process-global caches are warmed before counting: the envelope and
 *	proc slabs (__proc_slabs_ensure) and the per-thread fiber stack pool
 *	are allocated once per process and never returned, and the fiber
 *	stacks themselves are mmap'd, not hooked -- so a WARM-UP run of each
 *	step precedes the sweep, and every step reaps all slab caches before
 *	its snapshot and after its teardown.
 */

#define _GNU_SOURCE

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_cfg.h"
#include "xtc_exec.h"
#include "xtc_loop.h"
#include "xtc_mctx.h"
#include "xtc_proc.h"
#include "xtc_reg.h"
#include "xtc_slab.h"
#include "xtc_int.h"
#include "os_alloc.h"

#if defined(__GLIBC__)
#include <execinfo.h>
#include <unistd.h>
/* OOM_SWEEP_TRACE=1 prints a backtrace at each injected failure, to name
 * the allocation site behind a failing "armed allocation #N". */
static void
trace_site(void)
{
	void *f[24];
	int n;
	if (getenv("OOM_SWEEP_TRACE") == NULL) return;
	n = backtrace(f, 24);
	backtrace_symbols_fd(f, n, 2);
	if (write(2, "--\n", 3) < 0) return;
}
#else
static void trace_site(void) { }
#endif

#define MS (1000LL * 1000)
#define SWEEP_MAX 2000   /* a step needing more is a bug in the step */

/* ---- the counting, failing hook -------------------------------------
 *
 * Allocations are counted only inside an ARMED window (arm() .. disarm()),
 * which a step opens around exactly the target call, so the harness's own
 * setup (a loop to spawn on, a proc to send from) is never the failure
 * point.  Live allocations are counted always. */

static struct __os_alloc_hook g_real;
static _Atomic int  g_armed;
static _Atomic long g_seq;        /* armed allocations this step */
static long         g_fail_n;     /* fail the Nth armed one; 0 = never */
static _Atomic long g_failed;     /* injected failures this step */
static _Atomic long g_live;       /* successful allocs minus frees */

static void arm(void)    { atomic_store(&g_armed, 1); }
static void disarm(void) { atomic_store(&g_armed, 0); }

static int
should_fail(void)
{
	long n;
	if (!atomic_load(&g_armed)) return 0;
	n = atomic_fetch_add(&g_seq, 1) + 1;
	if (g_fail_n > 0 && n == g_fail_n) {
		atomic_fetch_add(&g_failed, 1);
		trace_site();
		return 1;
	}
	return 0;
}

static void *
h_malloc(size_t n)
{
	void *p;
	if (should_fail()) return NULL;
	p = g_real.malloc(n);
	if (p != NULL) atomic_fetch_add(&g_live, 1);
	return p;
}
static void *
h_calloc(size_t n, size_t sz)
{
	void *p;
	if (should_fail()) return NULL;
	p = g_real.calloc(n, sz);
	if (p != NULL) atomic_fetch_add(&g_live, 1);
	return p;
}
static void *
h_realloc(void *o, size_t sz)
{
	void *p;
	if (should_fail()) return NULL;
	p = g_real.realloc(o, sz);
	if (o == NULL && p != NULL) atomic_fetch_add(&g_live, 1);
	return p;
}
static void
h_free(void *p)
{
	if (p != NULL) atomic_fetch_sub(&g_live, 1);
	g_real.free(p);
}
static void *
h_aligned(size_t a, size_t sz)
{
	void *p;
	if (should_fail()) return NULL;
	p = g_real.aligned(a, sz);
	if (p != NULL) atomic_fetch_add(&g_live, 1);
	return p;
}
static void
h_aligned_free(void *p)
{
	if (p != NULL) atomic_fetch_sub(&g_live, 1);
	g_real.aligned_free(p);
}

/*
 * A step performs one target operation (inside arm/disarm) AND its full
 * teardown, so the live count after it must equal the count before it.
 * It returns the target's rc and sets *ok to 1 when the resulting state
 * is consistent with that rc: on XTC_OK the out-param is set and the
 * object WORKS (so a swallowed allocation failure that left a broken
 * object is caught even though the rc says OK); on an error nothing is
 * left half-registered.
 */
typedef int (*step_fn)(int *ok);

static void
sweep(const char *what, step_fn step)
{
	struct __os_alloc_hook h;
	long n, live0, leaked, total = -1, degraded = 0;
	int rc, ok;

	munit_assert_int(__os_alloc_get_hook(&g_real), ==, XTC_OK);
	h.malloc = h_malloc; h.calloc = h_calloc; h.realloc = h_realloc;
	h.free = h_free; h.aligned = h_aligned; h.aligned_free = h_aligned_free;
	disarm();
	g_fail_n = 0;
	munit_assert_int(__os_alloc_set_hook(&h), ==, XTC_OK);

	/* Warm-up with no failure: populates the process-global caches
	 * (proc/envelope slabs, stack pool) so they are not "leaks". */
	ok = 0;
	munit_assert_int(step(&ok), ==, XTC_OK);
	munit_assert_int(ok, ==, 1);

	for (n = 1; n <= SWEEP_MAX; n++) {
		(void)xtc_slab_reap_all();
		live0 = atomic_load(&g_live);
		atomic_store(&g_seq, 0);
		atomic_store(&g_failed, 0);
		g_fail_n = n;
		ok = 0;
		rc = step(&ok);
		disarm();
		g_fail_n = 0;
		(void)xtc_slab_reap_all();
		leaked = atomic_load(&g_live) - live0;
		if (leaked != 0 || ok != 1)
			munit_logf(MUNIT_LOG_ERROR,
			    "%s: failing armed allocation #%ld: rc=%d ok=%d "
			    "leaked=%ld", what, n, rc, ok, leaked);
		munit_assert_long(leaked, ==, 0);
		munit_assert_int(ok, ==, 1);
		if (atomic_load(&g_failed) == 0) {
			/* Allocation #n was never requested: the target ran to
			 * completion with no injected failure. */
			munit_assert_int(rc, ==, XTC_OK);
			total = n - 1;
			break;
		}
		if (rc == XTC_OK)
			degraded++;   /* failure absorbed; object verified OK */
	}
	munit_assert_int(__os_alloc_set_hook(&g_real), ==, XTC_OK);
	munit_assert_long(total, >, 0);   /* the window saw allocations */
	munit_logf(MUNIT_LOG_INFO, "%s: %ld allocation(s) swept, %ld "
	    "absorbed without an error rc (object still verified)", what,
	    total, degraded);
}

/* ---- loop init / fini ----------------------------------------------- */

static int
step_loop(int *ok)
{
	xtc_loop_t *loop = NULL;
	int rc;
	arm();
	rc = xtc_loop_init(&loop);
	disarm();
	*ok = 1;
	if (rc == XTC_OK) {
		if (loop == NULL) { *ok = 0; return rc; }
		if (xtc_loop_run(loop) != XTC_OK) *ok = 0;
		if (xtc_loop_fini(loop) != XTC_OK) *ok = 0;
	}
	return rc;
}

static MunitResult
test_loop_init(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	sweep("loop_init", step_loop);
	return MUNIT_OK;
}

/* ---- proc spawn ------------------------------------------------------ */

static _Atomic int g_ran;
static void p_mark(void *a) { (void)a; atomic_fetch_add(&g_ran, 1); }

static int
step_spawn(int *ok)
{
	xtc_loop_t *loop = NULL;
	xtc_pid_t pid = XTC_PID_NONE;
	int rc;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	atomic_store(&g_ran, 0);
	arm();
	rc = xtc_proc_spawn(loop, p_mark, NULL, NULL, &pid);
	disarm();
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	/* A spawn that reported success must have produced a pid AND a
	 * proc that ran; a failed one must not have run anything. */
	if (rc == XTC_OK)
		*ok = !xtc_pid_is_none(pid) && atomic_load(&g_ran) == 1;
	else
		*ok = atomic_load(&g_ran) == 0;
	return rc;
}

static MunitResult
test_proc_spawn(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	sweep("proc_spawn", step_spawn);
	return MUNIT_OK;
}

/* ---- mailbox send / recv -------------------------------------------- */

struct mbox_st {
	size_t len;
	int send_rc, recv_rc, ok;
};

static void
mbox_body(void *a)
{
	struct mbox_st *s = a;
	char data[512];
	void *m = NULL;
	size_t n = 0;
	memset(data, 'x', sizeof data);
	arm();
	s->send_rc = xtc_send(xtc_self(), data, s->len);
	s->recv_rc = (s->send_rc == XTC_OK) ? xtc_recv(&m, &n, 0) : s->send_rc;
	disarm();
	if (s->send_rc != XTC_OK) {
		/* Nothing was queued: a non-blocking recv finds nothing. */
		s->ok = (xtc_recv(&m, &n, 0) == XTC_E_AGAIN && m == NULL);
	} else if (s->recv_rc == XTC_OK) {
		s->ok = (m != NULL && n == s->len && memcmp(m, data, n) == 0);
		xtc_free(m);
	} else {
		/* recv failed (NOMEM on the copy-out): the message must have
		 * been kept, and a retry delivers it intact. */
		s->ok = (m == NULL && xtc_recv(&m, &n, 0) == XTC_OK &&
		    m != NULL && n == s->len && memcmp(m, data, n) == 0);
		if (m != NULL) xtc_free(m);
	}
}

static size_t g_mbox_len;

static int
step_mbox(int *ok)
{
	xtc_loop_t *loop = NULL;
	struct mbox_st s;
	memset(&s, 0, sizeof s);
	s.len = g_mbox_len;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, mbox_body, &s, NULL, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	*ok = s.ok;
	return s.send_rc != XTC_OK ? s.send_rc : s.recv_rc;
}

static MunitResult
test_mbox_small(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	g_mbox_len = 16;      /* envelope-slab path */
	sweep("send_recv_small", step_mbox);
	return MUNIT_OK;
}

static MunitResult
test_mbox_large(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	g_mbox_len = 512;     /* __os_malloc envelope path */
	sweep("send_recv_large", step_mbox);
	return MUNIT_OK;
}

/* ---- timer set ------------------------------------------------------ */

static _Atomic int g_fired;
static void t_mark(void *u) { (void)u; atomic_fetch_add(&g_fired, 1); }

static int
step_timer(int *ok)
{
	xtc_loop_t *loop = NULL;
	xtc_timer_t *tm = NULL;
	int rc;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	atomic_store(&g_fired, 0);
	arm();
	rc = xtc_timer_set(loop, 1000, t_mark, NULL, &tm);
	disarm();
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	if (rc == XTC_OK)
		*ok = (tm != NULL && atomic_load(&g_fired) == 1);
	else
		*ok = (atomic_load(&g_fired) == 0);
	return rc;
}

static MunitResult
test_timer_set(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	sweep("timer_set", step_timer);
	return MUNIT_OK;
}

/* ---- cfg register / set / session set ------------------------------- */

static const char *g_cfg_op;   /* which cfg call is inside the window */

static int
step_cfg(int *ok)
{
	xtc_cfg_spec_t s;
	xtc_cfg_session_t *ss = NULL, *prev;
	const char *v = NULL;
	int rc = XTC_OK, reg_rc;
	memset(&s, 0, sizeof s);
	s.name = "oom.str"; s.short_desc = "an oom-sweep knob";
	s.kind = XTC_CFG_STRING; s.dflt.d_string = "dflt";
	*ok = 1;

	if (strcmp(g_cfg_op, "register") == 0) arm();
	reg_rc = xtc_cfg_register(&s);
	disarm();
	if (reg_rc != XTC_OK) {
		/* A failed register leaves the name unregistered. */
		*ok = (xtc_cfg_get_string("oom.str", &v) != XTC_OK);
		return reg_rc;
	}
	/* The registered default reads back exactly as given. */
	if (xtc_cfg_get_string("oom.str", &v) != XTC_OK || v == NULL ||
	    strcmp(v, "dflt") != 0)
		*ok = 0;

	if (strcmp(g_cfg_op, "set") == 0) {
		arm();
		rc = xtc_cfg_set_string("oom.str", "a-new-value");
		disarm();
		if (xtc_cfg_get_string("oom.str", &v) != XTC_OK || v == NULL ||
		    strcmp(v, rc == XTC_OK ? "a-new-value" : "dflt") != 0)
			*ok = 0;
	}
	if (strcmp(g_cfg_op, "session") == 0) {
		arm();
		rc = xtc_cfg_session_create(&ss);
		if (rc == XTC_OK)
			rc = xtc_cfg_ssn_set_string(ss, "oom.str", "sess",
			    XTC_CFG_SRC_SESSION);
		disarm();
		if (ss != NULL) {
			prev = xtc_cfg_session_bind(ss);
			if (xtc_cfg_get_string("oom.str", &v) != XTC_OK ||
			    v == NULL ||
			    strcmp(v, rc == XTC_OK ? "sess" : "dflt") != 0)
				*ok = 0;
			(void)xtc_cfg_session_bind(prev);
			xtc_cfg_session_destroy(ss);
		} else if (rc == XTC_OK) {
			*ok = 0;
		}
	}
	munit_assert_int(xtc_cfg_unregister("oom.str"), ==, XTC_OK);
	return rc;
}

static MunitResult
test_cfg_register(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	g_cfg_op = "register";
	sweep("cfg_register", step_cfg);
	return MUNIT_OK;
}

static MunitResult
test_cfg_set(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	g_cfg_op = "set";
	sweep("cfg_set_string", step_cfg);
	return MUNIT_OK;
}

static MunitResult
test_cfg_session(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	g_cfg_op = "session";
	sweep("cfg_session_set", step_cfg);
	return MUNIT_OK;
}

/* ---- reg create / register ------------------------------------------ */

static int
step_reg(int *ok)
{
	xtc_reg_t *r = NULL;
	xtc_pid_t pid = { 1, 2, 3 }, got = XTC_PID_NONE;
	int rc;
	arm();
	rc = xtc_reg_create(&r);
	if (rc == XTC_OK)
		rc = xtc_reg_register(r, "oom.name", pid);
	disarm();
	*ok = 1;
	if (r == NULL)
		return rc;
	if (rc == XTC_OK)
		*ok = (xtc_reg_whereis(r, "oom.name", &got) == XTC_OK &&
		    xtc_pid_eq(got, pid) && xtc_reg_count(r) == 1);
	else
		*ok = (xtc_reg_count(r) == 0 &&
		    xtc_reg_whereis(r, "oom.name", &got) != XTC_OK);
	xtc_reg_destroy(r);
	return rc;
}

static MunitResult
test_reg(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	sweep("reg_create_register", step_reg);
	return MUNIT_OK;
}

/* ---- exec init / fini ---------------------------------------------- */

static int
step_exec(int *ok)
{
	xtc_exec_t *e = NULL;
	int rc;
	arm();
	rc = xtc_exec_init(&e, 3);
	disarm();
	*ok = 1;
	if (rc == XTC_OK) {
		*ok = (e != NULL && xtc_exec_n_loops(e) == 3 &&
		    xtc_exec_loop(e, 2) != NULL);
		if (e != NULL && xtc_exec_fini(e) != XTC_OK) *ok = 0;
	}
	return rc;
}

static MunitResult
test_exec_init(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	sweep("exec_init", step_exec);
	return MUNIT_OK;
}

/* ---- proc sleep (the park-timer path) -------------------------------- */

static int g_sleep_rc;
static int64_t g_slept_ns;

#define SLEEP_NS (20 * MS)

static void
sleeper(void *a)
{
	int64_t t0 = xtc_clock_mono(), t1;
	(void)a;
	arm();
	g_sleep_rc = xtc_proc_sleep(SLEEP_NS);
	disarm();
	t1 = xtc_clock_mono();
	g_slept_ns = t1 - t0;
}

static int
step_sleep(int *ok)
{
	xtc_loop_t *loop = NULL;
	g_sleep_rc = 12345;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, sleeper, NULL, NULL, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	/* XTC_OK means "slept at least ns" (xtc_proc.h); an error means it
	 * could not.  OK with a short sleep is the swallowed-failure bug. */
	*ok = (g_sleep_rc != 12345) &&
	    (g_sleep_rc != XTC_OK || g_slept_ns >= SLEEP_NS);
	if (!*ok)
		munit_logf(MUNIT_LOG_ERROR, "proc_sleep rc=%d slept %lld ns "
		    "of %lld", g_sleep_rc, (long long)g_slept_ns,
		    (long long)SLEEP_NS);
	return g_sleep_rc;
}

static MunitResult
test_proc_sleep(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	sweep("proc_sleep", step_sleep);
	return MUNIT_OK;
}

/* ---- spawn_monitor: the DOWN must arrive iff the spawn succeeded ----- */

struct mon_st {
	xtc_loop_t *loop;
	int spawn_rc, got_down;
};

static void mon_child(void *a) { (void)a; }

static void
mon_parent(void *a)
{
	struct mon_st *s = a;
	xtc_pid_t c = XTC_PID_NONE;
	uint64_t ref = 0;
	void *m = NULL;
	size_t n = 0;
	xtc_down_info_t di;
	arm();
	s->spawn_rc = xtc_proc_spawn_monitor(s->loop, mon_child, NULL,
	    NULL, &c, &ref);
	disarm();
	if (s->spawn_rc != XTC_OK)
		return;
	if (xtc_recv(&m, &n, 2000 * MS) == XTC_OK && m != NULL &&
	    xtc_down_decode_ex(m, n, &di) == XTC_OK && di.ref == ref &&
	    xtc_pid_eq(di.pid, c))
		s->got_down = 1;
	if (m != NULL) xtc_free(m);
}

static int
step_monitor(int *ok)
{
	xtc_loop_t *loop = NULL;
	struct mon_st s;
	memset(&s, 0, sizeof s);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	s.loop = loop;
	munit_assert_int(xtc_proc_spawn(loop, mon_parent, &s, NULL, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	*ok = (s.spawn_rc == XTC_OK) ? s.got_down : 1;
	return s.spawn_rc;
}

static MunitResult
test_spawn_monitor(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	sweep("proc_spawn_monitor", step_monitor);
	return MUNIT_OK;
}

/* ---- spawn_link: the EXIT must arrive iff the spawn succeeded ------- */

struct lnk_st {
	xtc_loop_t *loop;
	int spawn_rc, got_exit;
};

static void lnk_child(void *a) { (void)a; xtc_exit_self(3); }

static void
lnk_parent(void *a)
{
	struct lnk_st *s = a;
	xtc_pid_t c = XTC_PID_NONE;
	void *m = NULL;
	size_t n = 0;
	xtc_down_info_t di;
	arm();
	s->spawn_rc = xtc_proc_spawn_link(s->loop, lnk_child, NULL, NULL, &c);
	disarm();
	if (s->spawn_rc != XTC_OK)
		return;
	if (xtc_recv(&m, &n, 2000 * MS) == XTC_OK && m != NULL &&
	    xtc_down_decode_ex(m, n, &di) == XTC_OK && xtc_pid_eq(di.pid, c))
		s->got_exit = 1;
	if (m != NULL) xtc_free(m);
}

static int
step_link(int *ok)
{
	xtc_loop_t *loop = NULL;
	struct lnk_st s;
	memset(&s, 0, sizeof s);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	s.loop = loop;
	munit_assert_int(xtc_proc_spawn(loop, lnk_parent, &s, NULL, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	*ok = (s.spawn_rc == XTC_OK) ? s.got_exit : 1;
	return s.spawn_rc;
}

static MunitResult
test_spawn_link(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	sweep("proc_spawn_link", step_link);
	return MUNIT_OK;
}

/* ---- mctx create / alloc / destroy ---------------------------------- */

static int
step_mctx(int *ok)
{
	xtc_mctx_t *root = NULL, *kid = NULL;
	char *str = NULL;
	int rc;
	*ok = 1;
	arm();
	rc = xtc_mctx_create(NULL, "oom.root", 0, &root);
	if (rc == XTC_OK)
		rc = xtc_mctx_create(root, "oom.kid", 0, &kid);
	if (rc == XTC_OK) {
		str = xtc_mctx_strdup(kid, "hello");
		if (str == NULL) rc = XTC_E_NOMEM;
	}
	disarm();
	if (rc == XTC_OK)
		*ok = (strcmp(str, "hello") == 0 &&
		    strcmp(xtc_mctx_name(kid), "oom.kid") == 0);
	if (root != NULL)
		xtc_mctx_destroy(root);   /* recursively frees kid */
	return rc;
}

static MunitResult
test_mctx(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	sweep("mctx_create_strdup", step_mctx);
	return MUNIT_OK;
}

/* ---- slab create / alloc / destroy ---------------------------------- */

static int
step_slab(int *ok)
{
	xtc_slab_opts_t o = XTC_SLAB_OPTS_DEFAULT;
	xtc_slab_t *sl = NULL;
	void *obj = NULL;
	int rc;
	o.name = "oom.slab"; o.obj_size = 48;
	*ok = 1;
	arm();
	rc = xtc_slab_create(&o, &sl);
	if (rc == XTC_OK) {
		obj = xtc_slab_alloc(sl);
		if (obj == NULL) rc = XTC_E_NOMEM;
	}
	disarm();
	if (rc == XTC_OK)
		memset(obj, 0xab, 48);          /* usable */
	if (obj != NULL) xtc_slab_free(sl, obj);
	if (sl != NULL) xtc_slab_destroy(sl);
	else if (rc == XTC_OK) *ok = 0;
	return rc;
}

static MunitResult
test_slab(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	sweep("slab_create_alloc", step_slab);
	return MUNIT_OK;
}

#define T(name, fn) { name, fn, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
/* KNOWN BUG, reported, not yet fixed: munit TODO passes while the case
 * FAILS and errors ("marked TODO, but was successful") the moment the fix
 * lands -- so whoever fixes it must drop the mark, and the make check
 * SKIP SUMMARY lists every TODO case as not-passing. */
#define TODO(name, fn) { name, fn, NULL, NULL, MUNIT_TEST_OPTION_TODO, NULL }
static MunitTest tests[] = {
	T("/loop_init_fini",   test_loop_init),
	T("/proc_spawn",       test_proc_spawn),
	T("/send_recv_small",  test_mbox_small),
	T("/send_recv_large",  test_mbox_large),
	T("/timer_set",        test_timer_set),
	/* OOM-1: xtc_cfg_register voids the default-string strdup rc, so
	 * it returns XTC_OK with a NULL value (src/ptc/cfg.c). */
	TODO("/cfg_register",  test_cfg_register),
	T("/cfg_set_string",   test_cfg_set),
	T("/cfg_session_set",  test_cfg_session),
	T("/reg",              test_reg),
	T("/exec_init_fini",   test_exec_init),
	T("/proc_sleep",       test_proc_sleep),
	/* OOM-2/3/4: spawn_link / spawn_monitor return XTC_OK with no
	 * link / monitor when an entry alloc fails, and spawn_monitor leaks
	 * the child-side entry when the coro alloc fails (src/ptc/proc.c
	 * __proc_spawn_core). */
	TODO("/spawn_monitor", test_spawn_monitor),
	TODO("/spawn_link",    test_spawn_link),
	T("/mctx",             test_mctx),
	T("/slab",             test_slab),
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

static const MunitSuite suite = {
	"/coverage/oom_sweep", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE
};

int
main(int argc, char *argv[])
{
	return munit_suite_main(&suite, NULL, argc, argv);
}
