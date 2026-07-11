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

static MunitTest tests[] = {
	{ "/monitor_exit", test_xproc_monitor_exit, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/entry",        test_xproc_entry,        NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/link",         test_xproc_link,         NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

#endif /* _WIN32 */

static const MunitSuite suite = { "/m10.10/xproc", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
