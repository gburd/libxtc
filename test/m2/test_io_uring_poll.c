/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m2/test_io_uring_poll.c -- io_uring readiness without POLL_REMOVE.
 *
 * A poll cancel (IORING_OP_POLL_REMOVE) walks the kernel's poll-cancel
 * hash bucket: 16 buckets on the default ring, so O(armed polls / 16)
 * per cancel -- 169 us at 100k armed polls on Linux 6.12, and an
 * idle-connection server, where every connection's fiber wakes on its
 * idle timeout once a second and re-parks, spent 96% of its loop in it.
 * The backend now avoids the cancel on both re-park paths:
 *
 *   /ready_no_cancel   readiness wake: one-shot polls end when they fire,
 *                      so the dispatcher's delete needs no cancel
 *   /idle_repark       timeout wake + re-park of the same socket: the
 *                      armed poll lingers and is adopted, no SQE at all
 *                      (from the second timeout on: the first one finds
 *                      a busy fd's registration, which skips the fstat
 *                      that identifies the socket, and cancels)
 *   /linger_fires      the socket becomes ready while its poll lingers
 *                      (nobody parked): the next park still sees it
 *   /linger_reused_fd  close + a new socket on the same fd number while
 *                      the old poll lingers: the new socket is watched,
 *                      the old poll is not adopted (identity is the inode)
 *   /linger_expires    an unadopted linger is cancelled by the next poll
 *   /level_kept        a registration that is NOT deleted keeps reporting
 *                      while the fd stays ready (level, as epoll)
 *   /reg_past_table    the first registration of an fd number beyond the
 *                      fd table (the linger lookup ran before the table
 *                      grew: an out-of-bounds read, a sqlxtc-server crash)
 *   /idle_many         4,096 idle sockets parked with a timeout, many
 *                      timeout rounds: 0 cancels, every wake on time
 *
 * Other backends: skip (the counters are io_uring's).
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include "munit.h"
#include "xtc_int.h"
#include "xtc.h"
#include "xtc_io.h"
#include "xtc_loop.h"
#include "xtc_proc.h"
#include "loop_int.h"     /* loop->io */

int __xtc_io_uring_poll_stats(xtc_io_t *, unsigned long *, unsigned long *);
int __xtc_io_reg_stats(xtc_io_t *, int *, int *, int *);

static int
is_uring(void)
{
	return strcmp(xtc_io_backend_name(), "uring") == 0;
}

static unsigned long
cancels(xtc_io_t *io)
{
	unsigned long c = 0;

	munit_assert_int(__xtc_io_uring_poll_stats(io, &c, NULL), ==, XTC_OK);
	return c;
}

static unsigned long
adopted(xtc_io_t *io)
{
	unsigned long a = 0;

	munit_assert_int(__xtc_io_uring_poll_stats(io, NULL, &a), ==, XTC_OK);
	return a;
}

static void
sockpair(int sp[2])
{
	munit_assert_int(socketpair(AF_UNIX, SOCK_STREAM, 0, sp), ==, 0);
	munit_assert_int(fcntl(sp[0], F_SETFL, O_NONBLOCK), ==, 0);
}

/* Poll io until an event for tag arrives (or 2 s); returns its flags. */
static uint32_t
poll_for(xtc_io_t *io, void *tag)
{
	xtc_io_event_t ev[16];
	int i, k, n;

	for (k = 0; k < 40; k++) {
		n = 0;
		munit_assert_int(xtc_io_poll(io, ev, 16, 50LL * 1000000, &n),
		    ==, XTC_OK);
		for (i = 0; i < n; i++)
			if (ev[i].tag == tag)
				return ev[i].flags;
	}
	return 0;
}

/* Poll io once, non-blocking; 1 if an event for tag is among the results. */
static int
poll_has(xtc_io_t *io, void *tag, int64_t ns)
{
	xtc_io_event_t ev[16];
	int i, n = 0;

	munit_assert_int(xtc_io_poll(io, ev, 16, ns, &n), ==, XTC_OK);
	for (i = 0; i < n; i++)
		if (ev[i].tag == tag)
			return 1;
	return 0;
}

/* One timed-out park: afterwards the fd counts as idle, so its next
 * registration records the socket's identity and may linger. */
static void
prime(xtc_io_t *io, int fd, void *tag)
{
	munit_assert_int(xtc_io_reg_fd(io, fd, XTC_IO_READABLE, tag), ==,
	    XTC_OK);
	munit_assert_int(poll_has(io, tag, 1000000), ==, 0);
	munit_assert_int(xtc_io_del_fd(io, fd), ==, XTC_OK);
	(void)poll_has(io, tag, 0);   /* the cancel's CQE */
}

static MunitResult
test_ready_no_cancel(const MunitParameter p[], void *d)
{
	xtc_io_t *io = NULL;
	int sp[2], k, tag = 1;
	char c;
	(void)p; (void)d;

	if (!is_uring())
		return MUNIT_SKIP;
	munit_assert_int(xtc_io_init(&io), ==, XTC_OK);
	sockpair(sp);
	for (k = 0; k < 100; k++) {
		munit_assert_int(xtc_io_reg_fd(io, sp[0], XTC_IO_READABLE, &tag),
		    ==, XTC_OK);
		munit_assert_int((int)write(sp[1], "x", 1), ==, 1);
		munit_assert_uint(poll_for(io, &tag) & XTC_IO_READABLE, ==,
		    XTC_IO_READABLE);
		/* What the dispatcher does on a readiness wake. */
		munit_assert_int(xtc_io_del_fd(io, sp[0]), ==, XTC_OK);
		munit_assert_int((int)read(sp[0], &c, 1), ==, 1);
	}
	munit_assert_ulong(cancels(io), ==, 0);
	close(sp[0]);
	close(sp[1]);
	munit_assert_int(xtc_io_fini(io), ==, XTC_OK);
	return MUNIT_OK;
}

static MunitResult
test_idle_repark(const MunitParameter p[], void *d)
{
	xtc_io_t *io = NULL;
	int sp[2], k, tag = 1;
	(void)p; (void)d;

	if (!is_uring())
		return MUNIT_SKIP;
	munit_assert_int(xtc_io_init(&io), ==, XTC_OK);
	sockpair(sp);
	for (k = 0; k < 100; k++) {
		munit_assert_int(xtc_io_reg_fd(io, sp[0], XTC_IO_READABLE, &tag),
		    ==, XTC_OK);
		munit_assert_int(poll_has(io, &tag, 1000000), ==, 0); /* timeout */
		/* The waiter's cleanup, then its next park, as xtc_proc_wait_fd
		 * does on a timeout: del + reg with no poll in between. */
		munit_assert_int(xtc_io_del_fd(io, sp[0]), ==, XTC_OK);
	}
	munit_assert_int(xtc_io_reg_fd(io, sp[0], XTC_IO_READABLE, &tag), ==,
	    XTC_OK);
	munit_assert_ulong(cancels(io), ==, 1);
	munit_assert_ulong(adopted(io), ==, 99);
	/* And the adopted poll still works. */
	munit_assert_int((int)write(sp[1], "x", 1), ==, 1);
	munit_assert_uint(poll_for(io, &tag) & XTC_IO_READABLE, ==,
	    XTC_IO_READABLE);
	close(sp[0]);
	close(sp[1]);
	munit_assert_int(xtc_io_fini(io), ==, XTC_OK);
	return MUNIT_OK;
}

static MunitResult
test_linger_fires(const MunitParameter p[], void *d)
{
	xtc_io_t *io = NULL;
	int sp[2], tag = 1, tag2 = 2;
	(void)p; (void)d;

	if (!is_uring())
		return MUNIT_SKIP;
	munit_assert_int(xtc_io_init(&io), ==, XTC_OK);
	sockpair(sp);
	prime(io, sp[0], &tag);
	munit_assert_int(xtc_io_reg_fd(io, sp[0], XTC_IO_READABLE, &tag), ==,
	    XTC_OK);
	munit_assert_int(poll_has(io, &tag, 1000000), ==, 0);
	munit_assert_int(xtc_io_del_fd(io, sp[0]), ==, XTC_OK);   /* lingers */
	munit_assert_ulong(cancels(io), ==, 1);                    /* prime's */
	/* Ready while nobody is parked: the lingering poll fires and is
	 * consumed, and must not be reported to anyone. */
	munit_assert_int((int)write(sp[1], "x", 1), ==, 1);
	munit_assert_int(poll_has(io, &tag, 50LL * 1000000), ==, 0);
	munit_assert_int(poll_has(io, &tag, 0), ==, 0);
	/* A later park still sees the byte (level-checked at arm). */
	munit_assert_int(xtc_io_reg_fd(io, sp[0], XTC_IO_READABLE, &tag2), ==,
	    XTC_OK);
	munit_assert_uint(poll_for(io, &tag2) & XTC_IO_READABLE, ==,
	    XTC_IO_READABLE);
	munit_assert_int(xtc_io_del_fd(io, sp[0]), ==, XTC_OK);
	close(sp[0]);
	close(sp[1]);
	munit_assert_int(xtc_io_fini(io), ==, XTC_OK);
	return MUNIT_OK;
}

static MunitResult
test_linger_reused_fd(const MunitParameter p[], void *d)
{
	xtc_io_t *io = NULL;
	int a[2], b[2], fd, tag = 1, tag2 = 2;
	(void)p; (void)d;

	if (!is_uring())
		return MUNIT_SKIP;
	munit_assert_int(xtc_io_init(&io), ==, XTC_OK);
	sockpair(a);
	fd = a[0];
	prime(io, fd, &tag);
	munit_assert_int(xtc_io_reg_fd(io, fd, XTC_IO_READABLE, &tag), ==,
	    XTC_OK);
	munit_assert_int(poll_has(io, &tag, 1000000), ==, 0);
	munit_assert_int(xtc_io_del_fd(io, fd), ==, XTC_OK);      /* lingers */
	/* Close it and open a new socket on the same number. */
	close(a[0]);
	sockpair(b);
	if (b[0] != fd) {
		munit_assert_int(dup2(b[0], fd), ==, fd);
		close(b[0]);
		b[0] = fd;
		munit_assert_int(fcntl(fd, F_SETFL, O_NONBLOCK), ==, 0);
	}
	munit_assert_int(xtc_io_reg_fd(io, fd, XTC_IO_READABLE, &tag2), ==,
	    XTC_OK);
	munit_assert_ulong(adopted(io), ==, 0);   /* a different socket */
	/* The old socket is closed: its peer's write fails (EPIPE), and
	 * whatever the old poll does must not wake the new waiter... */
	(void)send(a[1], "x", 1, MSG_NOSIGNAL);
	munit_assert_int(poll_has(io, &tag2, 50LL * 1000000), ==, 0);
	/* ...the new one's must. */
	munit_assert_int((int)write(b[1], "y", 1), ==, 1);
	munit_assert_uint(poll_for(io, &tag2) & XTC_IO_READABLE, ==,
	    XTC_IO_READABLE);
	munit_assert_int(xtc_io_del_fd(io, fd), ==, XTC_OK);
	close(a[1]);
	close(b[0]);
	close(b[1]);
	munit_assert_int(xtc_io_fini(io), ==, XTC_OK);
	return MUNIT_OK;
}

static MunitResult
test_linger_expires(const MunitParameter p[], void *d)
{
	xtc_io_t *io = NULL;
	int sp[2], tag = 1, live = -1, zomb = -1, pend = -1, k;
	(void)p; (void)d;

	if (!is_uring())
		return MUNIT_SKIP;
	munit_assert_int(xtc_io_init(&io), ==, XTC_OK);
	sockpair(sp);
	prime(io, sp[0], &tag);
	munit_assert_int(xtc_io_reg_fd(io, sp[0], XTC_IO_READABLE, &tag), ==,
	    XTC_OK);
	munit_assert_int(poll_has(io, &tag, 1000000), ==, 0);
	munit_assert_int(xtc_io_del_fd(io, sp[0]), ==, XTC_OK);   /* lingers */
	munit_assert_int(__xtc_io_reg_stats(io, &live, &zomb, &pend), ==,
	    XTC_OK);
	munit_assert_int(zomb, ==, 1);
	/* Nobody re-registers: the next poll cancels it, and its terminal
	 * CQE frees it -- the socket's file is not held past that. */
	for (k = 0; k < 20 && zomb != 0; k++) {
		(void)poll_has(io, &tag, 10LL * 1000000);
		munit_assert_int(__xtc_io_reg_stats(io, &live, &zomb, &pend),
		    ==, XTC_OK);
	}
	munit_assert_int(zomb, ==, 0);
	munit_assert_int(live, ==, 0);
	munit_assert_ulong(cancels(io), ==, 2);   /* prime's + the expiry */
	close(sp[0]);
	close(sp[1]);
	munit_assert_int(xtc_io_fini(io), ==, XTC_OK);
	return MUNIT_OK;
}

static MunitResult
test_level_kept(const MunitParameter p[], void *d)
{
	xtc_io_t *io = NULL;
	int sp[2], tag = 1, k;
	(void)p; (void)d;

	if (!is_uring())
		return MUNIT_SKIP;
	munit_assert_int(xtc_io_init(&io), ==, XTC_OK);
	sockpair(sp);
	munit_assert_int(xtc_io_reg_fd(io, sp[0], XTC_IO_READABLE, &tag), ==,
	    XTC_OK);
	munit_assert_int((int)write(sp[1], "x", 1), ==, 1);
	/* Never read, never delete: every poll reports it again. */
	for (k = 0; k < 5; k++)
		munit_assert_uint(poll_for(io, &tag) & XTC_IO_READABLE, ==,
		    XTC_IO_READABLE);
	munit_assert_int(xtc_io_del_fd(io, sp[0]), ==, XTC_OK);
	close(sp[0]);
	close(sp[1]);
	munit_assert_int(xtc_io_fini(io), ==, XTC_OK);
	return MUNIT_OK;
}

static MunitResult
test_reg_past_table(const MunitParameter p[], void *d)
{
	xtc_io_t *io = NULL;
	struct rlimit rl;
	int sp[2], fd, tag = 1;
	(void)p; (void)d;

	if (!is_uring())
		return MUNIT_SKIP;
	munit_assert_int(getrlimit(RLIMIT_NOFILE, &rl), ==, 0);
	if (rl.rlim_cur < 4096) {
		rl.rlim_cur = rl.rlim_max == RLIM_INFINITY ||
		    rl.rlim_max >= 4096 ? 4096 : rl.rlim_max;
		(void)setrlimit(RLIMIT_NOFILE, &rl);
		if (getrlimit(RLIMIT_NOFILE, &rl) != 0 || rl.rlim_cur < 4096)
			return MUNIT_SKIP;
	}
	munit_assert_int(xtc_io_init(&io), ==, XTC_OK);
	sockpair(sp);
	/* Far past the initial table, and past any table a grown one
	 * would have: the lookup must not index it before it grows. */
	fd = 4000;
	munit_assert_int(dup2(sp[0], fd), ==, fd);
	munit_assert_int(xtc_io_reg_fd(io, fd, XTC_IO_READABLE, &tag), ==,
	    XTC_OK);
	munit_assert_int((int)write(sp[1], "x", 1), ==, 1);
	munit_assert_uint(poll_for(io, &tag) & XTC_IO_READABLE, ==,
	    XTC_IO_READABLE);
	munit_assert_int(xtc_io_del_fd(io, fd), ==, XTC_OK);
	close(fd);
	close(sp[0]);
	close(sp[1]);
	munit_assert_int(xtc_io_fini(io), ==, XTC_OK);
	return MUNIT_OK;
}

/* ---- many idle fibers -------------------------------------------- */

#define NIDLE   4096
#define ROUNDS  10

static int        g_sp[NIDLE][2];
static atomic_int g_timeouts;
static atomic_int g_bad;

static void
idle_fiber(void *arg)
{
	int i = (int)(intptr_t)arg, r, k;
	uint32_t rev;

	for (k = 0; k < ROUNDS; k++) {
		rev = 0;
		r = xtc_proc_wait_fd(g_sp[i][0], XTC_IO_READABLE,
		    20LL * 1000000, &rev);
		if (r == XTC_E_AGAIN && (rev & XTC_WAIT_TIMEOUT) &&
		    !(rev & XTC_IO_READABLE))
			atomic_fetch_add(&g_timeouts, 1);
		else
			atomic_fetch_add(&g_bad, 1);
	}
}

static MunitResult
test_idle_many(const MunitParameter p[], void *d)
{
	xtc_loop_t *loop = NULL;
	struct rlimit rl;
	int i;
	(void)p; (void)d;

	if (!is_uring())
		return MUNIT_SKIP;
	munit_assert_int(getrlimit(RLIMIT_NOFILE, &rl), ==, 0);
	if (rl.rlim_cur < 2 * NIDLE + 64) {
		rl.rlim_cur = rl.rlim_max == RLIM_INFINITY ||
		    rl.rlim_max >= 2 * NIDLE + 64 ? 2 * NIDLE + 64 : rl.rlim_max;
		(void)setrlimit(RLIMIT_NOFILE, &rl);
		if (getrlimit(RLIMIT_NOFILE, &rl) != 0 ||
		    rl.rlim_cur < 2 * NIDLE + 64)
			return MUNIT_SKIP;
	}
	for (i = 0; i < NIDLE; i++)
		sockpair(g_sp[i]);
	atomic_store(&g_timeouts, 0);
	atomic_store(&g_bad, 0);
	munit_assert_int(xtc_loop_init(&loop), ==, XTC_OK);
	for (i = 0; i < NIDLE; i++)
		munit_assert_int(xtc_proc_spawn(loop, idle_fiber,
		    (void *)(intptr_t)i, NULL, NULL), ==, XTC_OK);
	munit_assert_int(xtc_loop_run(loop), ==, XTC_OK);
	munit_assert_int(atomic_load(&g_bad), ==, 0);
	munit_assert_int(atomic_load(&g_timeouts), ==, NIDLE * ROUNDS);
	/* Per fiber: the first timeout cancels (a fresh fd records no
	 * identity), the second re-park lingers, every later one is adopted,
	 * and the last park's linger is cancelled after the fiber exits: 2
	 * cancels and ROUNDS-2 adoptions, whatever ROUNDS is.  A re-park
	 * that misses its linger (the loop polled between the cleanup and
	 * the next park) costs one cancel more.  Before: one cancel per
	 * timeout, NIDLE * ROUNDS. */
	munit_logf(MUNIT_LOG_WARNING, "cancels %lu adopted %lu (timeouts %d)",
	    cancels(loop->io), adopted(loop->io), NIDLE * ROUNDS);
	munit_assert_ulong(cancels(loop->io), <=, 2 * NIDLE + NIDLE / 8);
	munit_assert_ulong(adopted(loop->io), >=,
	    (unsigned long)NIDLE * (ROUNDS - 2) * 7 / 8);
	(void)xtc_loop_fini(loop);
	for (i = 0; i < NIDLE; i++) {
		close(g_sp[i][0]);
		close(g_sp[i][1]);
	}
	return MUNIT_OK;
}

static MunitTest tests[] = {
	{ "/ready_no_cancel", test_ready_no_cancel, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/idle_repark", test_idle_repark, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/linger_fires", test_linger_fires, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/linger_reused_fd", test_linger_reused_fd, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/linger_expires", test_linger_expires, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/level_kept", test_level_kept, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/reg_past_table", test_reg_past_table, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ "/idle_many", test_idle_many, NULL, NULL,
	    MUNIT_TEST_OPTION_NONE, NULL },
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m2/io_uring_poll", tests, NULL, 1,
    MUNIT_SUITE_OPTION_NONE };

int
main(int argc, char *argv[])
{
	return munit_suite_main(&suite, NULL, argc, argv);
}
