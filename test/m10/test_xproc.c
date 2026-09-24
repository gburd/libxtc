/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m10/test_xproc.c
 *	Cross-fork spawn / send / monitor (src/orc/xproc.c).  A parent
 *	forks a child running an xtc runtime, sends it a message, and
 *	monitors it so the child's exit surfaces as a normal xtc DOWN with
 *	the exit code decoded from the child's waitpid status.
 *
 *	POSIX only (fork); the whole suite SKIPs on Windows.
 */

#include <stdint.h>
#include <string.h>
#include <stdatomic.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "xtc_xproc.h"
#include "xtc_int.h"

#if defined(_WIN32)

static MunitResult
test_xproc_skip(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	return MUNIT_SKIP;   /* fork/socketpair have no Windows equivalent */
}
static MunitTest tests[] = {
	{ "/skip", test_xproc_skip, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

#else /* POSIX */

#include <pthread.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>

/* ---- child root proc: recv one message (a target exit code), exit with
 * it, so the parent's monitor sees exactly that reason. ---- */
static void
child_root(void *arg)
{
	int want_code = 7;   /* default if no arg */
	if (arg != NULL)
		memcpy(&want_code, arg, sizeof(int));
	{
		/* Wait for the parent's xtc_xsend, then exit with the code it
		 * carries (or want_code if none arrives in time). */
		void *msg = NULL; size_t n = 0;
		if (xtc_recv(&msg, &n, 1000LL * 1000 * 1000) == XTC_OK &&
		    n == sizeof(int)) {
			memcpy(&want_code, msg, sizeof(int));
		}
		if (msg) xtc_free(msg);
	}
	xtc_exit_self(want_code);
}

/* The monitoring fiber: xspawn a child, xsend it an exit code, xmonitor,
 * then recv the DOWN and record the decoded reason. */
struct mon_ctx {
	xtc_loop_t *loop;
	int    use_entry;        /* 1 = xtc_xspawn_entry, 0 = xtc_xspawn */
	int    down_seen;
	int    down_reason;
	int    xspawn_rc;
	long   child_os_pid;
};

static void
monitor_fiber(void *a)
{
	struct mon_ctx *m = a;
	xtc_loop_t *loop = m->loop;
	xtc_xproc_t *child = NULL;
	int init_code = 3;       /* child's default before our xsend */
	int exit_code = 42;      /* what we tell the child to exit with */
	uint64_t ref = 0;
	void *msg = NULL; size_t n = 0;

	m->xspawn_rc = m->use_entry
	    ? xtc_xspawn_entry(loop, "xchild", "root",
	        &init_code, sizeof init_code, &child)
	    : xtc_xspawn(loop, "xchild", child_root,
	        &init_code, sizeof init_code, &child);
	if (m->xspawn_rc != XTC_OK)
		return;

	/* The child's OS pid is a real, positive pid on POSIX. */
	m->child_os_pid = xtc_xproc_os_pid(child);
	/* Monitor first, then tell the child to exit with 42. */
	if (xtc_xmonitor(child, &ref) != XTC_OK) {
		xtc_xproc_destroy(child);
		return;
	}
	(void)xtc_xsend(child, &exit_code, sizeof exit_code);

	/* Wait for the DOWN the shadow proc delivers on the child's exit. */
	if (xtc_recv(&msg, &n, 3000LL * 1000 * 1000) == XTC_OK) {
		xtc_down_info_t di;
		if (xtc_down_decode_ex(msg, n, &di) == XTC_OK) {
			m->down_seen = 1;
			m->down_reason = di.reason;
		}
	}
	if (msg) xtc_free(msg);
	xtc_xproc_destroy(child);
}

static MunitResult
test_xproc_monitor_exit(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	struct mon_ctx m;
	(void)p; (void)d;

	memset(&m, 0, sizeof m);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	m.loop = loop;
	munit_assert_int(xtc_proc_spawn(loop, monitor_fiber, &m, NULL, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	if (m.xspawn_rc == XTC_E_NOSYS)
		return MUNIT_SKIP;   /* no fork on this build */
	munit_assert_int(m.xspawn_rc, ==, XTC_OK);
	munit_assert_int(m.down_seen, ==, 1);        /* got the DOWN */
	munit_assert_int(m.down_reason, ==, 42);     /* the code we sent */
	munit_assert_int64(m.child_os_pid, >, 0);    /* a real child pid */

	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* Entry-registry path (portable form; the only form that works on
 * Windows).  Registers child_root under a name, then spawns via
 * xtc_xspawn_entry.  On POSIX this forks + resolves the name; the
 * monitored child exit surfaces as the same DOWN. */
static MunitResult
test_xproc_entry(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	struct mon_ctx m;
	(void)p; (void)d;

	munit_assert_int(xtc_xproc_register_entry("root", child_root), ==,
	    XTC_OK);
	/* An unknown entry is rejected. */
	munit_assert_int(xtc_xspawn_entry(loop, "x", "nope", NULL, 0, NULL),
	    ==, XTC_E_INVAL);   /* NULL loop/out caught first */

	memset(&m, 0, sizeof m);
	m.use_entry = 1;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	m.loop = loop;
	munit_assert_int(xtc_proc_spawn(loop, monitor_fiber, &m, NULL, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);

	if (m.xspawn_rc == XTC_E_NOSYS)
		return MUNIT_SKIP;
	munit_assert_int(m.xspawn_rc, ==, XTC_OK);
	munit_assert_int(m.down_seen, ==, 1);
	munit_assert_int(m.down_reason, ==, 42);

	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* Link path: the linking fiber binds its fate to the child via
 * xtc_xlink; the child's exit delivers an EXIT signal (link semantics),
 * which the linker receives and decodes. */
struct link_ctx { xtc_loop_t *loop; int exit_seen; int exit_reason; int rc; };

static void
linker_fiber(void *a)
{
	struct link_ctx *m = a;
	xtc_xproc_t *child = NULL;
	int init_code = 3, exit_code = 55;
	void *msg = NULL; size_t n = 0;

	m->rc = xtc_xspawn_entry(m->loop, "lk", "root", &init_code,
	    sizeof init_code, &child);
	if (m->rc != XTC_OK) return;
	if (xtc_xlink(child) != XTC_OK) { xtc_xproc_destroy(child); return; }
	(void)xtc_xsend(child, &exit_code, sizeof exit_code);
	/* A link EXIT arrives as a decodable signal in our mailbox. */
	if (xtc_recv(&msg, &n, 3000LL * 1000 * 1000) == XTC_OK) {
		xtc_down_info_t di;
		if (xtc_down_decode_ex(msg, n, &di) == XTC_OK) {
			m->exit_seen = 1;
			m->exit_reason = di.reason;
		}
	}
	if (msg) xtc_free(msg);
	xtc_xproc_destroy(child);
}

static MunitResult
test_xproc_link(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	struct link_ctx m;
	(void)p; (void)d;
	munit_assert_int(xtc_xproc_register_entry("root", child_root), ==,
	    XTC_OK);
	memset(&m, 0, sizeof m);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	m.loop = loop;
	munit_assert_int(xtc_proc_spawn(loop, linker_fiber, &m, NULL, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	if (m.rc == XTC_E_NOSYS) return MUNIT_SKIP;
	munit_assert_int(m.rc, ==, XTC_OK);
	munit_assert_int(m.exit_seen, ==, 1);       /* link EXIT delivered */
	munit_assert_int(m.exit_reason, ==, 55);    /* the code we sent */
	munit_assert_int(xtc_loop_fini(loop), ==, XTC_OK);
	return MUNIT_OK;
}

/* ---- PLAN 19.27.1 / 19.27.2: destroy while the child is still running --
 *
 * A child that simply sleeps (it would outlive the test on its own).  The
 * parent monitors it -- which starts a SHADOW proc parked in
 * xtc_osproc_wait on the child's osproc -- and then destroys the handle
 * while the child is alive.
 *
 * Before the fix, destroy freed the osproc under the parked shadow (a
 * heap-use-after-free, visible under ASan) and did not terminate the
 * child at all: it stayed alive, orphaned, and later became a zombie.
 * After it, destroy terminates + reaps, and the shadow still delivers a
 * DOWN because it holds its own reference to the osproc.  Under ASan this
 * test is the UAF regression; without ASan it is the reaping regression.
 */
static void
sleeper_root(void *arg)
{
	(void)arg;
	for (;;)
		(void)xtc_proc_sleep(100LL * 1000 * 1000);
}

struct live_destroy {
	xtc_loop_t *loop;
	long child_pid;
	int  down_seen;
	int  child_still_alive;   /* after destroy returned */
	int  zombie;              /* waitpid could still collect it */
};

static void
live_destroy_fiber(void *a)
{
	struct live_destroy *ld = a;
	xtc_xproc_t *child = NULL;
	uint64_t ref = 0;
	void *msg = NULL;
	size_t n = 0;
	int st = 0;

	if (xtc_xspawn(ld->loop, "sleeper", sleeper_root,
	    NULL, 0, &child) != XTC_OK)
		return;
	ld->child_pid = xtc_xproc_os_pid(child);
	if (xtc_xmonitor(child, &ref) != XTC_OK) {
		xtc_xproc_destroy(child);
		return;
	}
	(void)xtc_proc_sleep(20LL * 1000 * 1000);   /* shadow is now parked */
	xtc_xproc_destroy(child);                   /* child STILL RUNNING */

	/* Destroy must have terminated AND reaped it: no process, and
	 * nothing left for waitpid to collect. */
	ld->child_still_alive = (kill((pid_t)ld->child_pid, 0) == 0);
	ld->zombie = (waitpid((pid_t)ld->child_pid, &st, WNOHANG) > 0);

	/* The shadow held its own reference, so it survives the destroy and
	 * still reports the child's death to the monitor. */
	if (xtc_recv(&msg, &n, 3000LL * 1000 * 1000) == XTC_OK)
		ld->down_seen = 1;
	if (msg != NULL)
		xtc_free(msg);
	if (ld->child_still_alive) {               /* never leak it */
		(void)kill((pid_t)ld->child_pid, SIGKILL);
		(void)waitpid((pid_t)ld->child_pid, &st, 0);
	}
}

/* ---- PLAN 19.27.3: a child's death is reported with the RIGHT KIND ----
 *
 * One child per scenario; the monitor must see:
 *   exit(0)        -> XTC_DOWN_KIND_CLEAN
 *   exit(1)        -> XTC_DOWN_KIND_EXIT,   exit_code 1
 *   exit(11)       -> XTC_DOWN_KIND_EXIT,   exit_code 11
 *   killed SIGSEGV -> XTC_DOWN_KIND_SIGNAL, signal 11
 * The last two carry the same number, which is exactly the case that used
 * to collapse: every signal death arrived as kind=EXIT with the signal
 * number as the exit code, so a supervisor could not tell a crash from
 * exit(11).  The child dies by raise(SIGSEGV) with the default action
 * restored, i.e. a real OS signal death -- not the in-process containment
 * path, which never reaches waitpid.
 */
struct kind_case { int mode; int got_kind; int got_signal; int got_code; int seen; };

static void
kind_child_root(void *arg)
{
	int mode = 0;
	if (arg != NULL)
		memcpy(&mode, arg, sizeof mode);
	if (mode == 3) {
		signal(SIGSEGV, SIG_DFL);
		(void)raise(SIGSEGV);
	}
	(void)xtc_exit_self(mode == 0 ? 0 : (mode == 1 ? 1 : 11));
}

struct kind_ctx { xtc_loop_t *loop; struct kind_case *c; };

static void
kind_fiber(void *a)
{
	struct kind_ctx *k = a;
	xtc_xproc_t *child = NULL;
	uint64_t ref = 0;
	void *msg = NULL;
	size_t n = 0;
	xtc_down_info_t di;

	if (xtc_xspawn(k->loop, "kind", kind_child_root, &k->c->mode,
	    sizeof k->c->mode, &child) != XTC_OK)
		return;
	if (xtc_xmonitor(child, &ref) == XTC_OK &&
	    xtc_recv(&msg, &n, 5000LL * 1000 * 1000) == XTC_OK &&
	    xtc_down_decode_ex(msg, n, &di) == XTC_OK) {
		k->c->seen = 1;
		k->c->got_kind = (int)di.kind;
		k->c->got_signal = di.signal;
		k->c->got_code = di.exit_code;
	}
	if (msg != NULL)
		xtc_free(msg);
	xtc_xproc_destroy(child);
}

static MunitResult
test_xproc_down_kind(const MunitParameter p[], void *d)
{
	struct kind_case c[4] = {
		{ 0, -1, -1, -1, 0 }, { 1, -1, -1, -1, 0 },
		{ 2, -1, -1, -1, 0 }, { 3, -1, -1, -1, 0 },
	};
	int i;
	(void)p; (void)d;

	for (i = 0; i < 4; i++) {
		xtc_loop_t *loop = NULL;
		struct kind_ctx k;
		munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
		k.loop = loop;
		k.c = &c[i];
		munit_assert_int(xtc_proc_spawn(loop, kind_fiber, &k, NULL,
		    NULL), ==, XTC_OK);
		munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
		(void)xtc_loop_fini(loop);
		munit_assert_int(c[i].seen, ==, 1);
	}
	munit_assert_int(c[0].got_kind, ==, XTC_DOWN_KIND_CLEAN);
	munit_assert_int(c[1].got_kind, ==, XTC_DOWN_KIND_EXIT);
	munit_assert_int(c[1].got_code, ==, 1);
	munit_assert_int(c[2].got_kind, ==, XTC_DOWN_KIND_EXIT);
	munit_assert_int(c[2].got_code, ==, 11);
	/* The discriminating case: same number as c[2], different fate. */
	munit_assert_int(c[3].got_kind, ==, XTC_DOWN_KIND_SIGNAL);
	munit_assert_int(c[3].got_signal, ==, SIGSEGV);
	return MUNIT_OK;
}

static MunitResult
test_xproc_destroy_live_child(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	struct live_destroy ld;
	(void)p; (void)d;

	memset(&ld, 0, sizeof ld);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	ld.loop = loop;
	munit_assert_int(xtc_proc_spawn(loop, live_destroy_fiber, &ld, NULL,
	    NULL), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	(void)xtc_loop_fini(loop);

	munit_assert_long(ld.child_pid, >, 0);
	munit_assert_int(ld.child_still_alive, ==, 0);  /* terminated */
	munit_assert_int(ld.zombie, ==, 0);             /* and reaped */
	munit_assert_int(ld.down_seen, ==, 1);          /* shadow survived */
	return MUNIT_OK;
}

/* ---- PLAN 19.27.9: xtc_xspawn_entry from a multithreaded parent ----
 *
 * Other threads continuously create and tear down loops (each takes the
 * process-global proc-table lock, __lt_lock) while a fiber spawns
 * FH_SPAWNS entry children in a row.  When the entry path forked
 * straight into a child runtime, a child forked while a churn thread held
 * __lt_lock blocked on it forever in its first xtc_proc_spawn --
 * reproduced about once per 200 spawns with 8 churn threads.  Re-exec
 * gives the child a fresh image, so every child must exit promptly.
 * A child that does not exit within 10 s counts as WEDGED (it is then
 * killed by xtc_xproc_destroy, so the test itself cannot hang). */
#define FH_SPAWNS  400
#define FH_THREADS 8
static _Atomic int g_fh_stop;
static void fh_noop(void *a) { (void)a; }
static void fh_root(void *a) { (void)a; (void)xtc_exit_self(0); }

static void *
fh_churn(void *a)
{
	(void)a;
	while (!atomic_load(&g_fh_stop)) {
		xtc_loop_t *l = NULL;
		if (xtc_loop_init(&l) == XTC_OK) {
			(void)xtc_proc_spawn(l, fh_noop, NULL, NULL, NULL);
			(void)xtc_loop_fini(l);   /* no run: hold time maximal */
		}
	}
	return NULL;
}

struct fh_ctx { xtc_loop_t *loop; int clean, wedged, other; };

static void
fh_fiber(void *a)
{
	struct fh_ctx *c = a;
	int i;
	for (i = 0; i < FH_SPAWNS; i++) {
		xtc_xproc_t *ch = NULL;
		uint64_t ref = 0;
		void *m = NULL;
		size_t n = 0;
		xtc_down_info_t di;
		if (xtc_xspawn_entry(c->loop, "fh", "fh_root", NULL, 0,
		    &ch) != XTC_OK) {
			c->other++;
			continue;
		}
		(void)xtc_xmonitor(ch, &ref);
		if (xtc_recv(&m, &n, 10LL * 1000 * 1000 * 1000) != XTC_OK)
			c->wedged++;
		else if (xtc_down_decode_ex(m, n, &di) == XTC_OK &&
		    di.kind == XTC_DOWN_KIND_CLEAN)
			c->clean++;
		else
			c->other++;
		if (m != NULL)
			xtc_free(m);
		xtc_xproc_destroy(ch);
	}
	atomic_store(&g_fh_stop, 1);
}

static MunitResult
test_xproc_entry_mt_parent(const MunitParameter p[], void *d)
{
	pthread_t t[FH_THREADS];
	xtc_loop_t *loop = NULL;
	struct fh_ctx c;
	int i;
	(void)p; (void)d;

	memset(&c, 0, sizeof c);
	atomic_store(&g_fh_stop, 0);
	munit_assert_int(xtc_xproc_register_entry("fh_root", fh_root), ==,
	    XTC_OK);
	for (i = 0; i < FH_THREADS; i++)
		munit_assert_int(pthread_create(&t[i], NULL, fh_churn, NULL),
		    ==, 0);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	c.loop = loop;
	munit_assert_int(xtc_proc_spawn(loop, fh_fiber, &c, NULL, NULL), ==,
	    XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	(void)xtc_loop_fini(loop);
	atomic_store(&g_fh_stop, 1);
	for (i = 0; i < FH_THREADS; i++)
		(void)pthread_join(t[i], NULL);
	munit_logf(MUNIT_LOG_INFO, "entry spawns: clean=%d wedged=%d other=%d",
	    c.clean, c.wedged, c.other);
	/* The property is "no child wedges".  Every child must also report
	 * its real fate; on releases before 1.50 a short-lived child's DOWN
	 * was misclassified (0f007ca), so this second assertion is only the
	 * wedge test's sanity check that every spawn really completed. */
	munit_assert_int(c.wedged, ==, 0);
	munit_assert_int(c.clean + c.other, ==, FH_SPAWNS);
	munit_assert_int(c.clean, ==, FH_SPAWNS);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/monitor_exit", test_xproc_monitor_exit, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/destroy_live_child", test_xproc_destroy_live_child, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/down_kind",          test_xproc_down_kind,          NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/entry_mt_parent",    test_xproc_entry_mt_parent,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/entry",        test_xproc_entry,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/link",         test_xproc_link,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

#endif /* _WIN32 */

static const MunitSuite suite = { "/m10.10/xproc", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int
main(int argc, char *argv[])
{
#if !defined(_WIN32)
	/* xtc_xspawn_entry re-execs THIS binary; the child must resolve the
	 * same names, so register them BEFORE the child hook, which runs the
	 * entry and _exit()s when this is a child launch. */
	(void)xtc_xproc_register_entry("root", child_root);
	(void)xtc_xproc_register_entry("fh_root", fh_root);
	(void)xtc_xproc_win_child_maybe(argc, argv);
#endif
	return munit_suite_main(&suite, NULL, argc, argv);
}
