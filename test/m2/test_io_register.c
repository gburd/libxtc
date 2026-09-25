/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * test/m2/test_io_register.c -- verifies M2_CLAIMS.md R1-R5.
 */

#define _POSIX_C_SOURCE 200809L

#include "io_pipe_compat.h"
#if !defined(_WIN32)
# include <unistd.h>
#endif

#include <string.h>

#include "munit.h"
#include "xtc.h"
#include "xtc_io.h"
#include "os_thread.h"     /* __os_thread_* : portable, no raw pthread */

/* Internal: declared in io_int.h, which pulls in backend-private types.
 * One extern is cleaner here than dragging that header into a test. */
int __xtc_io_defer_del_fd(xtc_io_t *, int);

#if defined(_WIN32)
# define test_pipe_close(fd)            xtc_test_close_pipe((fd), -1)
# define test_pipe_close_pair(p)        xtc_test_close_pipe((p)[0], (p)[1])
#else
# define test_pipe_close(fd)            close(fd)
# define test_pipe_close_pair(p)        do { close((p)[0]); close((p)[1]); } while (0)
#endif

static int
make_pipe(int *r, int *w)
{
	return xtc_test_make_pipe(r, w);
}

/* [R1] */
static MunitResult
test_reg_basic(const MunitParameter p[], void *d)
{
	xtc_io_t *io;
	int r, w, marker = 0;
	(void)p; (void)d;
	munit_assert_int(xtc_io_init(&io), ==, XTC_OK);
	munit_assert_int(make_pipe(&r, &w), ==, 0);
	munit_assert_int(xtc_io_reg_fd(io, r, XTC_IO_READABLE, &marker),
	    ==, XTC_OK);
	munit_assert_int(xtc_io_del_fd(io, r), ==, XTC_OK);
	test_pipe_close(r); test_pipe_close(w);
	munit_assert_int(xtc_io_fini(io), ==, XTC_OK);
	return MUNIT_OK;
}

/* [R2] */
static MunitResult
test_reg_bad_args(const MunitParameter p[], void *d)
{
	xtc_io_t *io;
	(void)p; (void)d;
	munit_assert_int(xtc_io_init(&io), ==, XTC_OK);
	munit_assert_int(xtc_io_reg_fd(NULL, 0, XTC_IO_READABLE, NULL),
	    ==, XTC_E_INVAL);
	munit_assert_int(xtc_io_reg_fd(io, -1, XTC_IO_READABLE, NULL),
	    ==, XTC_E_INVAL);
	munit_assert_int(xtc_io_reg_fd(io,  0, 0,                NULL),
	    ==, XTC_E_INVAL);
	munit_assert_int(xtc_io_fini(io), ==, XTC_OK);
	return MUNIT_OK;
}

/* [R3] */
static MunitResult
test_reg_duplicate(const MunitParameter p[], void *d)
{
	xtc_io_t *io;
	int r, w;
	(void)p; (void)d;
	munit_assert_int(xtc_io_init(&io), ==, XTC_OK);
	munit_assert_int(make_pipe(&r, &w), ==, 0);
	munit_assert_int(xtc_io_reg_fd(io, r, XTC_IO_READABLE, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_io_reg_fd(io, r, XTC_IO_READABLE, NULL),
	    ==, XTC_E_INVAL);
	test_pipe_close(r); test_pipe_close(w);
	munit_assert_int(xtc_io_fini(io), ==, XTC_OK);
	return MUNIT_OK;
}

/* [R4] */
static MunitResult
test_mod_fd(const MunitParameter p[], void *d)
{
	xtc_io_t *io;
	int r, w, a = 1, b = 2;
	(void)p; (void)d;
	munit_assert_int(xtc_io_init(&io), ==, XTC_OK);
	munit_assert_int(make_pipe(&r, &w), ==, 0);
	munit_assert_int(xtc_io_reg_fd(io, r, XTC_IO_READABLE, &a),
	    ==, XTC_OK);
	munit_assert_int(xtc_io_mod_fd(io, r, XTC_IO_READABLE, &b),
	    ==, XTC_OK);
	munit_assert_int(xtc_io_mod_fd(io, 9999, XTC_IO_READABLE, NULL),
	    ==, XTC_E_INVAL);     /* not registered */
	test_pipe_close(r); test_pipe_close(w);
	munit_assert_int(xtc_io_fini(io), ==, XTC_OK);
	return MUNIT_OK;
}

/*
 * [R4b] reg -> mod -> del -> POLL.
 *
 * The poll is the whole point.  R4 above modifies and then tears the io
 * down without ever draining, so it never observes what a modified
 * registration does to the reap path -- which is why a real lifetime
 * defect shipped: the io_uring backend used to cancel and re-arm the
 * poll IN PLACE under the same heap node, leaving two submitted poll
 * generations sharing one object.  The following xtc_io_del_fd marked
 * that one object dead, the first terminal CQE freed it, and the second
 * generation's CQE then read and freed it again (valgrind: invalid read
 * + invalid free in xtc_io_poll; glibc aborts the process outright with
 * "double free detected in tcache", so this test does not merely fail on
 * the buggy code, it crashes).
 *
 * Part A must NOT poll between the mod and the del: a poll there drains
 * both generations, and then the delete has nothing in flight to race.
 * That is exactly the hole the shipped R4 fell through, so the ordering
 * here is load-bearing, not incidental.  Part B pins the behavior the
 * fix must not trade away (readiness after a mod reports the NEW tag).
 */
static MunitResult
test_mod_then_del_then_poll(const MunitParameter p[], void *d)
{
	xtc_io_t *io;
	xtc_io_event_t evs[8];
	char buf[8];
	int r, w, n_out, i, j, a = 1, b = 2, c = 3, e = 4, saw_new = 0;
	(void)p; (void)d;
	munit_assert_int(xtc_io_init(&io), ==, XTC_OK);
	munit_assert_int(make_pipe(&r, &w), ==, 0);

	/* --- Part A: the use-after-free / double-free shape. --- */
	munit_assert_int(xtc_io_reg_fd(io, r, XTC_IO_READABLE, &a),
	    ==, XTC_OK);
	munit_assert_int(xtc_io_mod_fd(io, r, XTC_IO_READABLE, &b),
	    ==, XTC_OK);
	/* Make the fd ready so the modified registration has a real
	 * readiness completion in flight, on top of the cancel, when it is
	 * deleted.  No poll before the del -- see the comment above. */
	munit_assert_int(xtc_test_pipe_write(w, "x", 1), ==, 1);
	munit_assert_int(xtc_io_del_fd(io, r), ==, XTC_OK);
	/* Drain past every generation the mod may have left submitted. */
	for (i = 0; i < 5; i++) {
		munit_assert_int(xtc_io_poll(io, evs, 8, 50 * 1000 * 1000LL,
		    &n_out), ==, XTC_OK);
		for (j = 0; j < n_out; j++) {
			/* Deleted: no dispatch for this fd, old tag or new. */
			munit_assert_ptr(evs[j].tag, !=, &a);
			munit_assert_ptr(evs[j].tag, !=, &b);
		}
	}

	/* --- Part B: a mod reports the new tag, and the fd is reusable. ---
	 * The delete above really retired the modified registration, so this
	 * must not report a duplicate (a leaked live node would). */
	munit_assert_int(xtc_test_pipe_read(r, buf, 1), ==, 1);
	munit_assert_int(xtc_io_reg_fd(io, r, XTC_IO_READABLE, &c),
	    ==, XTC_OK);
	munit_assert_int(xtc_io_mod_fd(io, r, XTC_IO_READABLE, &e),
	    ==, XTC_OK);
	munit_assert_int(xtc_test_pipe_write(w, "y", 1), ==, 1);
	for (i = 0; i < 4 && !saw_new; i++) {
		munit_assert_int(xtc_io_poll(io, evs, 8, 200 * 1000 * 1000LL,
		    &n_out), ==, XTC_OK);
		for (j = 0; j < n_out; j++) {
			/* The tag the mod replaced must never be dispatched. */
			munit_assert_ptr(evs[j].tag, !=, &c);
			if (evs[j].tag == &e) saw_new = 1;
		}
	}
	munit_assert_int(saw_new, ==, 1);
	munit_assert_int(xtc_io_del_fd(io, r), ==, XTC_OK);
	munit_assert_int(xtc_io_poll(io, evs, 8, 50 * 1000 * 1000LL, &n_out),
	    ==, XTC_OK);
	test_pipe_close(r); test_pipe_close(w);
	munit_assert_int(xtc_io_fini(io), ==, XTC_OK);
	return MUNIT_OK;
}

/* [R5] */
static MunitResult
test_del_fd(const MunitParameter p[], void *d)
{
	xtc_io_t *io;
	int r, w;
	(void)p; (void)d;
	munit_assert_int(xtc_io_init(&io), ==, XTC_OK);
	munit_assert_int(make_pipe(&r, &w), ==, 0);
	munit_assert_int(xtc_io_reg_fd(io, r, XTC_IO_READABLE, NULL),
	    ==, XTC_OK);
	munit_assert_int(xtc_io_del_fd(io, r), ==, XTC_OK);
	munit_assert_int(xtc_io_del_fd(io, r), ==, XTC_E_INVAL);
	munit_assert_int(xtc_io_mod_fd(io, r, XTC_IO_READABLE, NULL),
	    ==, XTC_E_INVAL);
	test_pipe_close(r); test_pipe_close(w);
	munit_assert_int(xtc_io_fini(io), ==, XTC_OK);
	return MUNIT_OK;
}


/*
 * R4c: a re-register must not be rejected because a delete for that fd is
 * QUEUED but not yet drained.
 *
 * The deferred-unregister queue is drained by the OWNING loop at the top of
 * xtc_io_poll.  Until then the node is still linked in io->fds, so a
 * same-fd re-register saw a live duplicate and got XTC_E_INVAL -- which
 * xtc_proc_wait_fd reported as XTC_E_INTERNAL.  That deadlocks rather than
 * failing transiently: the only thread that can drain the queue is the one
 * receiving the error, so a consumer that retries never polls again and
 * every retry fails identically.  Reported from a PostgreSQL fiber workload
 * (fd 447 rejected with pending_del[0] == 447 on the same io).
 *
 * Fails on the unfixed library with rc == XTC_E_INVAL.
 */
struct defer_arg { xtc_io_t *io; int fd; int rc; };

static void *
__defer_del_thread(void *a)
{
	struct defer_arg *d = a;
	d->rc = __xtc_io_defer_del_fd(d->io, d->fd);
	return NULL;
}

static MunitResult
test_reg_with_pending_del(const MunitParameter p[], void *data)
{
	xtc_io_t *io = NULL;
	xtc_io_event_t evs[4];
	struct defer_arg d;
	__os_thread_t th;
	int pr[2], n = 0, i;
	int r, w;
	void *tag = (void *)(uintptr_t)0xABCD;
	(void)p; (void)data;

	/*
	 * io_uring ONLY.  It is the one backend whose __xtc_io_defer_del_fd
	 * QUEUES the unregister for the owning loop to drain; every other
	 * backend passes straight through to xtc_io_del_fd (verified across
	 * io_epoll/kqueue/poll/select/solaris/aix/iocp), so there is no
	 * pending-delete state for a re-register to collide with and this
	 * scenario cannot arise.  Skipping is honest: asserting the uring
	 * behavior elsewhere would fail for the right reason (the delete
	 * already applied) and teach nothing.
	 */
	if (strcmp(xtc_io_backend_name(), "uring") != 0)
		return MUNIT_SKIP;

	munit_assert_int(xtc_io_init(&io), ==, XTC_OK);
	munit_assert_int(make_pipe(&r, &w), ==, 0);
	pr[0] = r; pr[1] = w;

	/* Become this io's OWNER: owner_tid is recorded by the poller, and
	 * the defer path only QUEUES when the caller is a different thread. */
	for (i = 0; i < 4; i++) evs[i].fd = -1;
	munit_assert_int(xtc_io_poll(io, evs, 4, 0, &n), ==, XTC_OK);

	munit_assert_int(xtc_io_reg_fd(io, r, XTC_IO_READABLE, tag),
	    ==, XTC_OK);

	/* Queue the delete from a foreign thread and do NOT poll after, so
	 * it stays pending -- exactly the reported capture. */
	d.io = io; d.fd = r; d.rc = -999;
	munit_assert_int(__os_thread_create(&th, __defer_del_thread, &d),
	    ==, XTC_OK);
	munit_assert_int(__os_thread_join(&th, NULL), ==, XTC_OK);
	munit_assert_int(d.rc, ==, XTC_OK);

	/* Must succeed: the re-register consumes the queued delete. */
	munit_assert_int(xtc_io_reg_fd(io, r, XTC_IO_READABLE, tag),
	    ==, XTC_OK);

	/* And the fd must still be WATCHABLE.  A fix that merely returned OK
	 * while leaving nothing armed would pass the assert above and still
	 * strand a real waiter, so prove readiness actually dispatches. */
	munit_assert_int(xtc_test_pipe_write(w, "x", 1), ==, 1);
	for (i = 0; i < 4; i++) evs[i].fd = -1;
	n = 0;
	munit_assert_int(xtc_io_poll(io, evs, 4, 500 * 1000000LL, &n),
	    ==, XTC_OK);
	munit_assert_int(n, >, 0);
	/* SCAN, do not assume evs[0]: __xtc_io_defer_del_fd nudges the owner
	 * with xtc_io_wakeup, so this poll legitimately reports the wakeup
	 * event (fd == -1, NULL tag) alongside the fd readiness.  My first
	 * version of this assert checked evs[0] and failed on the wakeup --
	 * a test bug, not a library one. */
	{
		int found = 0;
		for (i = 0; i < n; i++)
			if (evs[i].tag == tag && evs[i].fd == r)
				found = 1;
		munit_assert_int(found, ==, 1);
	}

	(void)xtc_io_del_fd(io, r);
	munit_assert_int(xtc_io_fini(io), ==, XTC_OK);
	test_pipe_close_pair(pr);
	return MUNIT_OK;
}

#if !defined(_WIN32)
/* [R6] An fd closed while still registered must not take the whole poll
 * down.  select(2) fails the entire call with EBADF for ONE dead fd; the
 * select backend used to return XTC_E_INTERNAL for it, and the executor
 * worker then EXITED, stranding every fiber on its loop (the select-backend
 * /m5/exec/Blk6 hang).  Every backend must instead keep polling: the poll
 * returns XTC_OK, and the dead fd may be reported as XTC_IO_ERR (select,
 * poll) or silently dropped by the kernel (epoll, kqueue, io_uring).  A
 * LIVE fd registered beside it must still be reported readable. */
static MunitResult
test_closed_registered_fd(const MunitParameter p[], void *d)
{
	xtc_io_t *io = NULL;
	xtc_io_event_t ev[8];
	int dr, dw, lr, lw, n = 0, i, saw_live = 0, rc;
	char c = 'x';
	(void)p; (void)d;

	munit_assert_int(xtc_io_init(&io), ==, XTC_OK);
	munit_assert_int(make_pipe(&dr, &dw), ==, 0);
	munit_assert_int(make_pipe(&lr, &lw), ==, 0);
	munit_assert_int(xtc_io_reg_fd(io, dr, XTC_IO_READABLE, (void *)1),
	    ==, XTC_OK);
	munit_assert_int(xtc_io_reg_fd(io, lr, XTC_IO_READABLE, (void *)2),
	    ==, XTC_OK);
	(void)close(dr);                    /* registered, now dead */
	(void)close(dw);
	munit_assert_int((int)write(lw, &c, 1), ==, 1);
	/* The first poll may be spent reporting the dead fd; allow two. */
	for (i = 0; i < 2 && !saw_live; i++) {
		int k;
		memset(ev, 0, sizeof ev);
		rc = xtc_io_poll(io, ev, 8, 100LL * 1000 * 1000, &n);
		munit_assert_int(rc, ==, XTC_OK);
		for (k = 0; k < n; k++)
			if (ev[k].tag == (void *)2 &&
			    (ev[k].flags & XTC_IO_READABLE))
				saw_live = 1;
	}
	munit_assert_int(saw_live, ==, 1);
	(void)xtc_io_del_fd(io, lr);
	(void)xtc_io_del_fd(io, dr);        /* may already be gone */
	(void)close(lr);
	(void)close(lw);
	(void)xtc_io_fini(io);
	return MUNIT_OK;
}
#endif

static MunitTest tests[] = {
	{ "/R1_basic",     test_reg_basic,    NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/R2_bad_args",  test_reg_bad_args, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/R3_duplicate", test_reg_duplicate,NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/R4_mod_fd",    test_mod_fd,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/R4c_reg_with_pending_del", test_reg_with_pending_del,
	  NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/R4b_mod_del_poll", test_mod_then_del_then_poll,
	                                      NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
	{ "/R5_del_fd",    test_del_fd,       NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
#if !defined(_WIN32)
	{ "/R6_closed_registered_fd", test_closed_registered_fd, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL },
#endif
	{ NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};
static const MunitSuite suite = { "/m2/io_register", tests, NULL, 1, MUNIT_SUITE_OPTION_NONE };
int main(int argc, char *argv[]) { return munit_suite_main(&suite, NULL, argc, argv); }
