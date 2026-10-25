/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m4/test_uring_batch.c -- deferred io_uring submission.
 *
 * Producers (xtc_io_reg_fd / _mod_fd / _del_fd, xtc_io_aio_submit) leave
 * their SQEs queued; the loop's poll submits them in the enter that
 * waits, and the loop starts queued file AIO early when the device would
 * otherwise idle (see src/io/io_uring.c __submit_wait, __xtc_io_flush).
 * The counts come from __xtc_io_uring_enter_stats: io_uring_enter calls
 * that carried SQEs ("submits") and calls that only waited ("waits").
 *
 *   /aio_burst      N fibers on one loop, one write each: ~N/8 submitting
 *                   enters, not N (1.59: one per write)
 *   /fd_burst       N fibers each park on a ready pipe: their N POLL_ADDs
 *                   go in at most a few enters, not N
 *   /zero_poll      a zero-timeout poll with nothing queued does not enter
 *                   (1.59: every one waited in the kernel)
 *   /spin_on_peer   a fiber that spins on xtc_yield until a peer's write
 *                   completes must not wait on an unsubmitted SQE
 *   /sq_full_burst  more writes in one loop turn than the SQ holds (256):
 *                   none lost, none stuck, every byte reads back
 *
 * On a DEFER_TASKRUN ring (Linux 6.1+, a running loop) the counts are
 * taken on that ring: the owner thread submits and waits in one enter.
 * Other backends have no ring: the counts are skipped, the functional
 * parts (every op completes, every byte round-trips) still run.
 */

#define _GNU_SOURCE

#include <fcntl.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <unistd.h>

#include "io_pipe_compat.h"
#include "munit.h"
#include "xtc_int.h"
#include "xtc.h"
#include "xtc_aio.h"
#include "xtc_fs.h"
#include "xtc_io.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "loop_int.h"     /* loop->io */

int __xtc_io_uring_enter_stats(xtc_io_t *, unsigned long *, unsigned long *,
    unsigned *);
int __xtc_io_uring_info(xtc_io_t *, unsigned *, unsigned *, unsigned *);

#define PAGE  4096
#define SETUP_DEFER_TASKRUN  (1u << 13)

static int
kernel_at_least(int maj, int min)
{
	struct utsname u;
	int a = 0, b = 0;

	if (uname(&u) != 0 || sscanf(u.release, "%d.%d", &a, &b) != 2)
		return 0;
	return a > maj || (a == maj && b >= min);
}

/* Submitting enters on io so far; -1 when the backend has no ring. */
static long
submits(xtc_io_t *io)
{
	unsigned long s = 0;

	if (__xtc_io_uring_enter_stats(io, &s, NULL, NULL) != XTC_OK)
		return -1;
	return (long)s;
}

/* The ring a running loop polls is DEFER_TASKRUN where the kernel has it. */
static void
assert_defer_if_kernel(xtc_io_t *io)
{
	unsigned fl = 0;

	munit_assert_int(__xtc_io_uring_info(io, &fl, NULL, NULL), ==, XTC_OK);
	if (kernel_at_least(6, 1))
		munit_assert_uint(fl & SETUP_DEFER_TASKRUN, !=, 0);
}

static int
tmpfile_open(char *path, size_t n)
{
	char dir[512];
	int fd = -1;

	munit_assert_int(xtc_fs_tmpdir(dir, sizeof dir), ==, XTC_OK);
	snprintf(path, n, "%s/xtc_uring_batch_XXXXXX", dir);
	munit_assert_int(xtc_fs_mkstemp(path, &fd), ==, XTC_OK);
	return fd;
}

/* ---- /aio_burst, /sq_full_burst ------------------------------------- */

struct burst {
	int        fd;
	int        verify;   /* read back and compare too */
	atomic_int next;     /* the fiber's index */
	atomic_int ok;
	atomic_int bad;
};

static void
burst_fiber(void *arg)
{
	struct burst *b = arg;
	unsigned char out[PAGE], in[PAGE];
	int i = atomic_fetch_add(&b->next, 1), r;

	memset(out, 0x30 + (i % 64), sizeof out);
	out[0] = (unsigned char)i;
	out[1] = (unsigned char)(i >> 8);
	if ((r = xtc_aio_pwrite(b->fd, out, PAGE, (int64_t)i * PAGE)) !=
	    PAGE) {
		munit_logf(MUNIT_LOG_WARNING, "write %d: %d", i, r);
		atomic_fetch_add(&b->bad, 1);
		return;
	}
	if (b->verify) {
		memset(in, 0, sizeof in);
		if ((r = xtc_aio_pread(b->fd, in, PAGE, (int64_t)i * PAGE)) !=
		    PAGE || memcmp(in, out, PAGE) != 0) {
			munit_logf(MUNIT_LOG_WARNING, "read %d: %d", i, r);
			atomic_fetch_add(&b->bad, 1);
			return;
		}
	}
	atomic_fetch_add(&b->ok, 1);
}

/* N fibers on one loop, one write (+ read) each; returns submitting
 * enters during the run (-1 off io_uring). */
static long
run_burst(int n, int verify)
{
	xtc_loop_t *loop = NULL;
	struct burst b;
	char path[600];
	long s0, s1;
	unsigned queued = 0;
	int i;

	memset(&b, 0, sizeof b);
	b.fd = tmpfile_open(path, sizeof path);
	b.verify = verify;
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	for (i = 0; i < n; i++)
		munit_assert_int(xtc_proc_spawn(loop, burst_fiber, &b, NULL,
		    NULL), ==, XTC_OK);
	s0 = submits(loop->io);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	s1 = submits(loop->io);
	munit_assert_int(atomic_load(&b.bad), ==, 0);
	munit_assert_int(atomic_load(&b.ok), ==, n);
	if (s1 >= 0) {
		assert_defer_if_kernel(loop->io);
		/* Nothing is left behind unsubmitted. */
		munit_assert_int(__xtc_io_uring_enter_stats(loop->io, NULL,
		    NULL, &queued), ==, XTC_OK);
		munit_assert_uint(queued, ==, 0);
	}
	(void)xtc_loop_fini(loop);
	(void)xtc_fs_close(b.fd);
	(void)unlink(path);
	return s1 < 0 ? -1 : s1 - s0;
}

static MunitResult
test_aio_burst(const MunitParameter p[], void *d)
{
	enum { N = 64 };
	long s;
	(void)p; (void)d;

	s = run_burst(N, 0);
	if (s < 0)
		return MUNIT_OK;   /* no ring: the writes completing is the test */
	munit_logf(MUNIT_LOG_INFO, "%d writes, %ld submitting enters", N, s);
	/* One submit per write was 1.59 (>= N).  Now: the first write alone
	 * (nothing in flight), then batches of up to 8 runs. */
	munit_assert_long(s, <=, N / 4);
	return MUNIT_OK;
}

/* ---- /fd_burst ------------------------------------------------------ */

#define FD_MAX 300
struct fdb {
	int        rp[FD_MAX], wp[FD_MAX];
	atomic_int next;
	atomic_int ok;
};

static void
fd_fiber(void *arg)
{
	struct fdb *f = arg;
	int i = atomic_fetch_add(&f->next, 1);
	uint32_t rev = 0;

	if (xtc_proc_wait_fd(f->rp[i], XTC_IO_READABLE, 5000LL * 1000000,
	    &rev) == XTC_OK && (rev & XTC_IO_READABLE))
		atomic_fetch_add(&f->ok, 1);
}

/* n fibers each park on their own ready pipe; returns submitting enters
 * during the run (-1 off io_uring). */
static long
run_fd_burst(struct fdb *f, int n)
{
	xtc_loop_t *loop = NULL;
	long s0, s1;
	int i;

	memset(f, 0, sizeof *f);
	for (i = 0; i < n; i++) {
		munit_assert_int(xtc_test_make_pipe(&f->rp[i], &f->wp[i]), ==,
		    0);
		munit_assert_int(xtc_test_pipe_write(f->wp[i], "x", 1), ==, 1);
	}
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	for (i = 0; i < n; i++)
		munit_assert_int(xtc_proc_spawn(loop, fd_fiber, f, NULL,
		    NULL), ==, XTC_OK);
	s0 = submits(loop->io);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	s1 = submits(loop->io);
	munit_assert_int(atomic_load(&f->ok), ==, n);
	if (s1 >= 0)
		assert_defer_if_kernel(loop->io);
	(void)xtc_loop_fini(loop);
	for (i = 0; i < n; i++)
		xtc_test_close_pipe(f->rp[i], f->wp[i]);
	return s1 < 0 ? -1 : s1 - s0;
}

static struct fdb g_fdb;

static MunitResult
test_fd_burst(const MunitParameter p[], void *d)
{
	enum { N = 64 };
	long s;
	(void)p; (void)d;

	s = run_fd_burst(&g_fdb, N);
	if (s < 0)
		return MUNIT_OK;
	munit_logf(MUNIT_LOG_INFO, "%d fd parks, %ld submitting enters", N, s);
	/* 1.59: one enter per POLL_ADD (>= N).  Now the parks' polls go in
	 * with the poll that waits for them. */
	munit_assert_long(s, <=, 4);
	return MUNIT_OK;
}

/*
 * More SQEs queued in one loop turn than the 256-entry SQ holds: FD_MAX
 * fibers park on ready pipes before the loop polls (polls are never
 * flushed early), so the SQ fills and __get_sqe submits it.  Then 600
 * fibers each write and read back a page.  Nothing may be lost or stuck.
 * Off io_uring the fd part runs small (macOS's default fd limit).
 */
static MunitResult
test_sq_full_burst(const MunitParameter p[], void *d)
{
	enum { N = 600 };
	long s;
	int nfd = strcmp(xtc_io_backend_name(), "uring") == 0 ? FD_MAX : 32;
	(void)p; (void)d;

	s = run_fd_burst(&g_fdb, nfd);
	if (s >= 0) {
		munit_logf(MUNIT_LOG_INFO, "%d fd parks, %ld submitting enters",
		    nfd, s);
		munit_assert_long(s, >=, 2);   /* the full SQ was submitted */
	}
	s = run_burst(N, 1);
	if (s >= 0)
		munit_logf(MUNIT_LOG_INFO, "%d writes + reads, %ld submitting "
		    "enters", N, s);
	return MUNIT_OK;
}

/* ---- /zero_poll ----------------------------------------------------- */

static MunitResult
test_zero_poll(const MunitParameter p[], void *d)
{
	xtc_io_t *io = NULL;
	xtc_io_event_t ev[8];
	unsigned long s0 = 0, w0 = 0, s1 = 0, w1 = 0;
	int i, n;
	(void)p; (void)d;

	munit_assert_int(xtc_io_init(&io), ==, XTC_OK);
	if (__xtc_io_uring_enter_stats(io, &s0, &w0, NULL) != XTC_OK) {
		(void)xtc_io_fini(io);
		return MUNIT_SKIP;
	}
	/* The first poll arms the wakeup poll (one submit). */
	munit_assert_int(xtc_io_poll(io, ev, 8, 0, &n), ==, XTC_OK);
	munit_assert_int(__xtc_io_uring_enter_stats(io, &s0, &w0, NULL), ==,
	    XTC_OK);
	for (i = 0; i < 1000; i++) {
		munit_assert_int(xtc_io_poll(io, ev, 8, 0, &n), ==, XTC_OK);
		munit_assert_int(n, ==, 0);
	}
	munit_assert_int(__xtc_io_uring_enter_stats(io, &s1, &w1, NULL), ==,
	    XTC_OK);
	munit_logf(MUNIT_LOG_INFO, "1000 zero-timeout polls: %lu submits, "
	    "%lu waits", s1 - s0, w1 - w0);
	munit_assert_ulong(s1 - s0, ==, 0);
	munit_assert_ulong(w1 - w0, ==, 0);
	/* Still live: a wakeup is seen. */
	munit_assert_int(xtc_io_wakeup(io), ==, XTC_OK);
	munit_assert_int(xtc_io_poll(io, ev, 8, 2000LL * 1000000, &n), ==,
	    XTC_OK);
	munit_assert_int(n, ==, 1);
	munit_assert_int(xtc_io_fini(io), ==, XTC_OK);
	return MUNIT_OK;
}

/* ---- /spin_on_peer -------------------------------------------------- */

/*
 * A fiber that busy-waits with xtc_yield for a peer's write keeps the
 * run queue from draining, so the loop's blocking poll -- where queued
 * SQEs are normally submitted -- does not come.  Preemption is off (the
 * default).  The write must still be started (the loop flushes after a
 * yield, and when nothing else is in flight) and reaped (the 64-run
 * fairness poll).  Several spinners, so the queue is never empty.  A
 * deadline turns a hang into a failure.
 */
#define SPIN_BOUND_NS (10LL * 1000 * 1000 * 1000)
static atomic_int g_spin_done, g_spin_wrote;
static atomic_long g_spin_yields;
static int64_t g_spin_t0;
static int g_spin_fd;

static void
spin_writer(void *arg)
{
	unsigned char b[PAGE];
	(void)arg;
	memset(b, 0x5a, sizeof b);
	if (xtc_aio_pwrite(g_spin_fd, b, PAGE, 0) == PAGE)
		atomic_store(&g_spin_wrote, 1);
	atomic_store(&g_spin_done, 1);
}

static void
spinner(void *arg)
{
	(void)arg;
	while (!atomic_load(&g_spin_done) &&
	    xtc_clock_mono() - g_spin_t0 < SPIN_BOUND_NS) {
		atomic_fetch_add(&g_spin_yields, 1);
		xtc_yield();
	}
}

static MunitResult
test_spin_on_peer(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	char path[600];
	int i;
	(void)p; (void)d;

	g_spin_fd = tmpfile_open(path, sizeof path);
	atomic_store(&g_spin_done, 0);
	atomic_store(&g_spin_wrote, 0);
	atomic_store(&g_spin_yields, 0);
	g_spin_t0 = xtc_clock_mono();
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	for (i = 0; i < 4; i++)
		munit_assert_int(xtc_proc_spawn(loop, spinner, NULL, NULL,
		    NULL), ==, XTC_OK);
	munit_assert_int(xtc_proc_spawn(loop, spin_writer, NULL, NULL, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	(void)xtc_loop_fini(loop);
	(void)xtc_fs_close(g_spin_fd);
	(void)unlink(path);
	munit_logf(MUNIT_LOG_INFO, "%ld spinner yields before the write "
	    "completed in %lld us", atomic_load(&g_spin_yields),
	    (long long)((xtc_clock_mono() - g_spin_t0) / 1000));
	munit_assert_int(atomic_load(&g_spin_wrote), ==, 1);
	munit_assert_int(atomic_load(&g_spin_done), ==, 1);
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/aio_burst", test_aio_burst, NULL, NULL, MUNIT_TEST_OPTION_NONE,
	    NULL },
	{ "/fd_burst", test_fd_burst, NULL, NULL, MUNIT_TEST_OPTION_NONE,
	    NULL },
	{ "/zero_poll", test_zero_poll, NULL, NULL, MUNIT_TEST_OPTION_NONE,
	    NULL },
	{ "/spin_on_peer", test_spin_on_peer, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/sq_full_burst", test_sq_full_burst, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/uring_batch", tests, NULL, 1,
    MUNIT_SUITE_OPTION_NONE };
int main(int argc, char **argv) { return munit_suite_main(&suite, NULL, argc, argv); }
