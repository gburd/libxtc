/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m2/test_io_uring_setup.c -- io_uring ring setup and ownership.
 *
 * xtc_io_init builds the ring with COOP_TASKRUN (Linux 5.19+) or no
 * optional flag.  When a thread starts RUNNING the loop, the ring is
 * rebuilt on that thread with DEFER_TASKRUN + SINGLE_ISSUER (6.1+): such
 * a ring is bound to one thread, which is the only one that may submit to
 * it or reap it, and a loop's thread can change, so a later run on
 * another thread rebuilds it again.  (Waiting for the run also keeps
 * loops that are created and destroyed without running off DEFER, whose
 * kernel-side free costs an RCU grace period.)
 *
 *   /tiers            init: COOP; after a loop run: DEFER (by kernel)
 *   /fallback_coop    DEFER refused ("io.uring.no_defer"): a run stays COOP
 *   /fallback_plain   every optional flag refused ("io.uring.setup_einval"):
 *                     a plain ring, still working
 *   /creator_no_submit xtc_io_init submits nothing (a poll's completion
 *                     work runs on its SUBMITTER); the first poll arms the
 *                     wakeup, and a wakeup posted before it is delivered
 *   /loop_other_thread a loop created on one thread and run on another
 *   /exec_run_twice   xtc_exec_run twice (new worker threads each time)
 *                     with file AIO and fd waits in both runs
 *   /aio_across_stop  an AIO in flight when the loop's thread stops is
 *                     reaped by that thread and completes on the next run
 *                     on a different thread
 *   /ready_across_move readiness that arrives while no thread runs the
 *                     loop is reported by the next owner's (rebuilt) ring
 *
 * Other backends: the uring-only checks are skips; the ownership cases
 * run everywhere (they are plain API contracts).
 */

#define _GNU_SOURCE

#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "munit.h"
#include "xtc_int.h"
#include "xtc.h"
#include "xtc_io.h"
#include "xtc_aio.h"
#include "xtc_exec.h"
#include "xtc_fs.h"
#include "xtc_inject.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "os_thread.h"
#include "loop_int.h"     /* loop->io: the ring a loop polls */

int __xtc_io_uring_info(xtc_io_t *, unsigned *, unsigned *, unsigned *);

/* Kernel UAPI values (stable), spelled here so the test builds without
 * liburing on the non-uring backends. */
#define SETUP_COOP_TASKRUN   (1u << 8)
#define SETUP_TASKRUN_FLAG   (1u << 9)
#define SETUP_SINGLE_ISSUER  (1u << 12)
#define SETUP_DEFER_TASKRUN  (1u << 13)
#define COOP (SETUP_COOP_TASKRUN | SETUP_TASKRUN_FLAG)
#define DEFER (SETUP_SINGLE_ISSUER | SETUP_DEFER_TASKRUN | SETUP_TASKRUN_FLAG)
#define ALLOPT (COOP | DEFER)

static int
is_uring(void)
{
	return strcmp(xtc_io_backend_name(), "uring") == 0;
}

/* Running kernel at least maj.min? */
static int
kernel_at_least(int maj, int min)
{
	struct utsname u;
	int a = 0, b = 0;

	if (uname(&u) != 0 || sscanf(u.release, "%d.%d", &a, &b) != 2)
		return 0;
	return a > maj || (a == maj && b >= min);
}

static unsigned
ring_flags(xtc_io_t *io)
{
	unsigned fl = 0;

	munit_assert_int(__xtc_io_uring_info(io, &fl, NULL, NULL), ==, XTC_OK);
	return fl & ALLOPT;
}

static unsigned
rebinds(xtc_io_t *io)
{
	unsigned r = 0;

	if (!is_uring())
		return 0;
	munit_assert_int(__xtc_io_uring_info(io, NULL, NULL, &r), ==, XTC_OK);
	return r;
}

/* Wakeup round trip on io: post, then a bounded poll must see it. */
static void
assert_wakes(xtc_io_t *io)
{
	xtc_io_event_t evs[8];
	int n = 0;

	munit_assert_int(xtc_io_wakeup(io), ==, XTC_OK);
	munit_assert_int(xtc_io_poll(io, evs, 8, 2000LL * 1000000, &n), ==,
	    XTC_OK);
	munit_assert_int(n, ==, 1);
	munit_assert_uint(evs[0].flags & XTC_IO_WAKEUP, ==, XTC_IO_WAKEUP);
}

/* A loop run with one fiber that does nothing: binds loop->io. */
static void
nop_fiber(void *arg)
{
	(void)arg;
}

static unsigned
flags_after_run(xtc_loop_t *loop)
{
	munit_assert_int(xtc_proc_spawn(loop, nop_fiber, NULL, NULL, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	return ring_flags(loop->io);
}

static MunitResult
test_tiers(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	unsigned fl;
	(void)p; (void)d;

	if (!is_uring())
		return MUNIT_SKIP;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	fl = ring_flags(loop->io);
	munit_assert_true(fl == COOP || fl == 0);
	if (kernel_at_least(5, 19))
		munit_assert_uint(fl, ==, COOP);
	assert_wakes(loop->io);
	fl = flags_after_run(loop);
	munit_assert_true(fl == DEFER || fl == COOP || fl == 0);
	if (kernel_at_least(6, 1))
		munit_assert_uint(fl, ==, DEFER);
	munit_logf(MUNIT_LOG_INFO, "running ring flags 0x%x (%s)", fl,
	    fl == DEFER ? "defer_taskrun" : fl == COOP ? "coop_taskrun" :
	    "plain");
	assert_wakes(loop->io);
	/* Same thread again: no rebuild. */
	munit_assert_uint(flags_after_run(loop), ==, fl);
	munit_assert_uint(rebinds(loop->io), ==, fl == DEFER ? 1 : 0);
	(void)xtc_loop_fini(loop);
	return MUNIT_OK;
}

static void
inj_nop(const char *name, void *user)
{
	(void)name; (void)user;
}

static MunitResult
fallback(const char *inj, unsigned want)
{
	xtc_loop_t *loop = NULL;
	int rc;

	if (!is_uring())
		return MUNIT_SKIP;
	if (want == COOP && !kernel_at_least(5, 19))
		return MUNIT_SKIP;
	munit_assert_int(xtc_inject_attach(inj, inj_nop, NULL), ==, XTC_OK);
	rc = xtc_loop_init(&loop);
	if (rc == XTC_OK)
		munit_assert_uint(flags_after_run(loop), ==, want);
	(void)xtc_inject_detach(inj);
	munit_assert_int(rc, ==, XTC_OK);
	assert_wakes(loop->io);
	(void)xtc_loop_fini(loop);
	return MUNIT_OK;
}

static MunitResult
test_fallback_coop(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	return fallback("io.uring.no_defer", COOP);
}

static MunitResult
test_fallback_plain(const MunitParameter p[], void *d)
{
	(void)p; (void)d;
	return fallback("io.uring.setup_einval", 0);
}

static MunitResult
test_creator_no_submit(const MunitParameter p[], void *d)
{
	xtc_io_t *io = NULL;
	xtc_io_event_t evs[8];
	unsigned head = ~0u;
	int n = -1;
	(void)p; (void)d;

	if (!is_uring())
		return MUNIT_SKIP;
	munit_assert_int(xtc_io_init(&io), ==, XTC_OK);
	munit_assert_int(__xtc_io_uring_info(io, NULL, &head, NULL), ==,
	    XTC_OK);
	munit_assert_uint(head, ==, 0);   /* nothing reached the kernel */
	/* Written before any arm: the first poll's arm must still see it. */
	munit_assert_int(xtc_io_wakeup(io), ==, XTC_OK);
	munit_assert_int(xtc_io_poll(io, evs, 8, 2000LL * 1000000, &n), ==,
	    XTC_OK);
	munit_assert_int(n, ==, 1);
	munit_assert_uint(evs[0].flags & XTC_IO_WAKEUP, ==, XTC_IO_WAKEUP);
	munit_assert_int(__xtc_io_uring_info(io, NULL, &head, NULL), ==,
	    XTC_OK);
	munit_assert_uint(head, >=, 1);
	/* And it stays armed. */
	assert_wakes(io);
	munit_assert_int(xtc_io_poll(io, evs, 8, 0, &n), ==, XTC_OK);
	munit_assert_int(n, ==, 0);
	munit_assert_int(xtc_io_fini(io), ==, XTC_OK);
	return MUNIT_OK;
}

/* ---- fiber work: file AIO round trip + an fd wait ------------------- */

#define PAGE 4096

struct work {
	int         fd;       /* temp file */
	int         rp, wp;   /* pipe: the fiber waits on rp */
	int         round;
	atomic_int  ok;       /* fibers that finished correctly */
	atomic_int  bad;
};

static void
work_fiber(void *arg)
{
	struct work *w = arg;
	unsigned char out[PAGE], in[PAGE];
	uint32_t rev = 0;
	char c;
	int i, r;

	for (i = 0; i < PAGE; i++)
		out[i] = (unsigned char)(i * 31 + w->round);
	if ((r = xtc_aio_pwrite(w->fd, out, PAGE, (int64_t)w->round * PAGE)) !=
	    PAGE || (r = xtc_aio_fdatasync(w->fd)) != 0) {
		munit_logf(MUNIT_LOG_WARNING, "write/sync r=%d", r);
		atomic_fetch_add(&w->bad, 1);
		return;
	}
	memset(in, 0, sizeof in);
	if ((r = xtc_aio_pread(w->fd, in, PAGE, (int64_t)w->round * PAGE)) !=
	    PAGE || memcmp(in, out, PAGE) != 0) {
		munit_logf(MUNIT_LOG_WARNING, "read r=%d", r);
		atomic_fetch_add(&w->bad, 1);
		return;
	}
	/* An fd wait that completes: the byte is written before the park. */
	if (write(w->wp, "x", 1) != 1) {
		atomic_fetch_add(&w->bad, 1);
		return;
	}
	r = xtc_proc_wait_fd(w->rp, XTC_IO_READABLE, 2000LL * 1000000, &rev);
	if (r != XTC_OK || read(w->rp, &c, 1) != 1) {
		munit_logf(MUNIT_LOG_WARNING, "wait r=%d", r);
		atomic_fetch_add(&w->bad, 1);
		return;
	}
	atomic_fetch_add(&w->ok, 1);
}

static void
work_open(struct work *w, char *path, size_t n)
{
	char dir[512];
	int p[2];

	memset(w, 0, sizeof *w);
	munit_assert_int(xtc_fs_tmpdir(dir, sizeof dir), ==, XTC_OK);
	snprintf(path, n, "%s/xtc_uring_own_XXXXXX", dir);
	w->fd = mkstemp(path);
	munit_assert_int(w->fd, >=, 0);
	munit_assert_int(pipe(p), ==, 0);
	munit_assert_int(fcntl(p[0], F_SETFL, O_NONBLOCK), ==, 0);
	w->rp = p[0];
	w->wp = p[1];
}

static void
work_close(struct work *w, const char *path)
{
	close(w->fd);
	close(w->rp);
	close(w->wp);
	unlink(path);
}

/* ---- a loop created on one thread and run on another ---------------- */

struct run_arg {
	xtc_loop_t *loop;
	int         rc;
};

static void *
run_thread(void *arg)
{
	struct run_arg *ra = arg;

	ra->rc = xtc_loop_run(ra->loop);
	return NULL;
}

static void
run_on_thread(xtc_loop_t *loop)
{
	__os_thread_t th;
	struct run_arg ra;

	ra.loop = loop;
	ra.rc = -999;
	munit_assert_int(__os_thread_create(&th, run_thread, &ra), ==, XTC_OK);
	munit_assert_int(__os_thread_join(&th, NULL), ==, XTC_OK);
	munit_assert_int(ra.rc, ==, XTC_OK);
}

static MunitResult
test_loop_other_thread(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	struct work w;
	char path[600];
	int k;
	(void)p; (void)d;

	work_open(&w, path, sizeof path);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	/* Three runs, each on a fresh thread: DEFER ring built on each. */
	for (k = 0; k < 3; k++) {
		w.round = k;
		munit_assert_int(xtc_proc_spawn(loop, work_fiber, &w, NULL,
		    NULL), ==, XTC_OK);
		run_on_thread(loop);
		munit_assert_int(atomic_load(&w.bad), ==, 0);
		munit_assert_int(atomic_load(&w.ok), ==, k + 1);
	}
	/* And once on this (creating) thread. */
	w.round = 3;
	munit_assert_int(xtc_proc_spawn(loop, work_fiber, &w, NULL, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(atomic_load(&w.ok), ==, 4);
	munit_assert_int(atomic_load(&w.bad), ==, 0);
	if (is_uring() && (ring_flags(loop->io) & SETUP_SINGLE_ISSUER))
		munit_assert_uint(rebinds(loop->io), ==, 4);   /* 1 per run */
	(void)xtc_loop_fini(loop);
	work_close(&w, path);
	return MUNIT_OK;
}

/* ---- xtc_exec_run twice -------------------------------------------- */

#define NLOOPS 4

static MunitResult
test_exec_run_twice(const MunitParameter p[], void *d)
{
	xtc_exec_t *e = NULL;
	struct work w[NLOOPS];
	char path[NLOOPS][600];
	int run, i;
	(void)p; (void)d;

	for (i = 0; i < NLOOPS; i++)
		work_open(&w[i], path[i], sizeof path[i]);
	munit_assert_int(xtc_exec_init(&e, NLOOPS), ==, XTC_OK);
	for (run = 0; run < 3; run++) {
		for (i = 0; i < NLOOPS; i++) {
			w[i].round = run;
			munit_assert_int(xtc_proc_spawn(xtc_exec_loop(e, i),
			    work_fiber, &w[i], NULL, NULL), ==, XTC_OK);
		}
		munit_assert_int(xtc_exec_run(e), ==, XTC_OK);
		for (i = 0; i < NLOOPS; i++) {
			munit_assert_int(atomic_load(&w[i].bad), ==, 0);
			munit_assert_int(atomic_load(&w[i].ok), ==, run + 1);
		}
	}
	(void)xtc_exec_fini(e);
	for (i = 0; i < NLOOPS; i++)
		work_close(&w[i], path[i]);
	return MUNIT_OK;
}

/* ---- an AIO in flight when the loop's thread stops ------------------ */

struct stop_ctx {
	xtc_loop_t *loop;
	int         fd;
	atomic_int  started;
	atomic_int  res;
};

static void
stop_fiber(void *arg)
{
	struct stop_ctx *c = arg;
	static unsigned char buf[1 << 20];

	atomic_store(&c->started, 1);
	/* Stop the loop while this write is in the kernel: the run returns
	 * with the fiber parked on it. */
	(void)xtc_loop_stop(c->loop);
	atomic_store(&c->res, xtc_aio_pwrite(c->fd, buf, sizeof buf, 0));
}

static MunitResult
test_aio_across_stop(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	struct stop_ctx c;
	char dir[512], path[600];
	(void)p; (void)d;

	munit_assert_int(xtc_fs_tmpdir(dir, sizeof dir), ==, XTC_OK);
	snprintf(path, sizeof path, "%s/xtc_uring_stop_XXXXXX", dir);
	memset(&c, 0, sizeof c);
	c.fd = mkstemp(path);
	munit_assert_int(c.fd, >=, 0);
	atomic_store(&c.res, -12345);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	c.loop = loop;
	munit_assert_int(xtc_proc_spawn(loop, stop_fiber, &c, NULL, NULL), ==,
	    XTC_OK);
	run_on_thread(loop);           /* stops with the write parked */
	munit_assert_int(atomic_load(&c.started), ==, 1);
	/* A different thread finishes the run: the fiber must resume with
	 * the write's result, not hang (the old ring's issuer is gone). */
	run_on_thread(loop);
	munit_assert_int(atomic_load(&c.res), ==, 1 << 20);
	(void)xtc_loop_fini(loop);
	close(c.fd);
	unlink(path);
	return MUNIT_OK;
}

/* ---- readiness that arrives while nobody runs the loop -------------- */

struct ready_ctx {
	xtc_loop_t *loop;
	int         rp, wp;
	atomic_int  phase;
	atomic_int  res;
	uint32_t    rev;
};

static void
ready_fiber(void *arg)
{
	struct ready_ctx *c = arg;

	atomic_store(&c->phase, 1);
	(void)xtc_loop_stop(c->loop);
	atomic_store(&c->res, xtc_proc_wait_fd(c->rp, XTC_IO_READABLE,
	    5000LL * 1000000, &c->rev));
	atomic_store(&c->phase, 2);
}

static MunitResult
test_ready_across_move(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	struct ready_ctx c;
	int pp[2];
	(void)p; (void)d;

	memset(&c, 0, sizeof c);
	munit_assert_int(pipe(pp), ==, 0);
	munit_assert_int(fcntl(pp[0], F_SETFL, O_NONBLOCK), ==, 0);
	c.rp = pp[0];
	c.wp = pp[1];
	atomic_store(&c.res, -12345);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	c.loop = loop;
	munit_assert_int(xtc_proc_spawn(loop, ready_fiber, &c, NULL, NULL), ==,
	    XTC_OK);
	run_on_thread(loop);    /* returns with the fiber parked on rp */
	munit_assert_int(atomic_load(&c.phase), ==, 1);
	/* The fd becomes ready while no thread runs the loop. */
	munit_assert_int((int)write(c.wp, "x", 1), ==, 1);
	run_on_thread(loop);
	munit_assert_int(atomic_load(&c.phase), ==, 2);
	munit_assert_int(atomic_load(&c.res), ==, XTC_OK);
	munit_assert_uint(c.rev & XTC_IO_READABLE, ==, XTC_IO_READABLE);
	(void)xtc_loop_fini(loop);
	close(pp[0]);
	close(pp[1]);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/tiers", test_tiers, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/fallback_coop", test_fallback_coop, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/fallback_plain", test_fallback_plain, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/creator_no_submit", test_creator_no_submit, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/loop_other_thread", test_loop_other_thread, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/exec_run_twice", test_exec_run_twice, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/aio_across_stop", test_aio_across_stop, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/ready_across_move", test_ready_across_move, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m2/io_uring_setup", tests, NULL, 1,
    MUNIT_SUITE_OPTION_NONE };

int
main(int argc, char *argv[])
{
	return munit_suite_main(&suite, NULL, argc, argv);
}
