/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/ptc/aio.c
 *	Async file I/O for fibers (xtc_aio_pread/pwrite/fsync).  See
 *	src/inc/xtc_aio.h.
 *
 *	On a loop with the io_uring backend, the op is submitted natively
 *	(IORING_OP_READ/WRITE/FSYNC); the fiber parks (park_requested +
 *	yield) and the completion event re-enqueues it.  The xtc_aio_t
 *	lives on the parked fiber's stack and is read after it wakes; the
 *	park loop only ever wakes on this op's completion (it arms no
 *	other waker), so the struct is never referenced after the frame
 *	unwinds.  On a readiness-only backend xtc_io_aio_submit returns
 *	XTC_E_NOSYS and the op is offloaded to the blocking pool instead.
 */

#include "xtc_int.h"
#include "xtc_aio.h"
#include "xtc_io.h"
#include "xtc_blocking.h"
#include "loop_int.h"
#include "coro_int.h"

#include <errno.h>
#include <string.h>
#if defined(_WIN32)
#  include <stdio.h>       /* SEEK_SET */
#  include <io.h>          /* _read/_write/_lseeki64/_commit */
#else
#  include <unistd.h>
#endif

/* Blocking-pool fallback: run the op on a worker thread.  Portable
 * across the platforms libxtc builds on -- the native (non-blocking)
 * path is io_uring-only anyway, so this is purely the offload case. */
struct aio_blk { int fd; int op; void *buf; uint32_t len; int64_t off; };

static int
aio_blk_fn(void *arg)
{
	struct aio_blk *b = arg;
#if defined(_WIN32)
	int n;
	if (b->op == XTC_AIO_FSYNC)
		return _commit(b->fd) == 0 ? 0 : -EIO;
	if (_lseeki64(b->fd, (long long)b->off, SEEK_SET) < 0)
		return -EIO;
	n = (b->op == XTC_AIO_PWRITE)
	    ? _write(b->fd, b->buf, b->len)
	    : _read(b->fd, b->buf, b->len);
	return n < 0 ? -EIO : n;
#else
	ssize_t n;
	switch (b->op) {
	case XTC_AIO_PREAD:
		n = pread(b->fd, b->buf, b->len, (off_t)b->off);  /* XTC_BLOCKING_OK: offloaded to the blocking pool */
		return n < 0 ? -errno : (int)n;
	case XTC_AIO_PWRITE:
		n = pwrite(b->fd, b->buf, b->len, (off_t)b->off); /* XTC_BLOCKING_OK: offloaded to the blocking pool */
		return n < 0 ? -errno : (int)n;
	case XTC_AIO_FSYNC:
#if defined(__APPLE__)
		return fsync(b->fd) == 0 ? 0 : -errno;   /* macOS has no fdatasync */
#else
		return fdatasync(b->fd) == 0 ? 0 : -errno;
#endif
	default:
		return -EINVAL;
	}
#endif
}

static int
aio_offload(int op, int fd, void *buf, uint32_t len, int64_t off)
{
	struct aio_blk b;
	int rc;
	b.fd = fd; b.op = op; b.buf = buf; b.len = len; b.off = off;
	if (xtc_blocking_run(aio_blk_fn, &b, &rc) != XTC_OK)
		rc = aio_blk_fn(&b);   /* off a loop: run inline */
	return rc;
}

static int
aio_do(int op, int fd, void *buf, uint32_t len, int64_t off)
{
	xtc_task_t  *t = __xtc_current_task();
	xtc_loop_t  *loop = __xtc_current_loop;
	xtc_aio_t    a;
	int          rc;

	/* Off a loop fiber: there is nothing to park, so run it inline
	 * (the blocking-pool path, which itself runs inline with no loop). */
	if (t == NULL || loop == NULL || loop->io == NULL)
		return aio_offload(op, fd, buf, len, off);

	memset(&a, 0, sizeof a);
	a.fd = fd; a.op = op; a.buf = buf; a.len = len; a.off = off;
	a.tag = t;
	rc = xtc_io_aio_submit(loop->io, &a);
	if (rc != XTC_OK)
		return aio_offload(op, fd, buf, len, off);   /* NOSYS/AGAIN: offload */

	/* Park until THIS op completes.  The completion is the only thing
	 * targeting this task (no other waker is armed), so the loop is
	 * bounded and the on-stack `a` is never touched after we return. */
	while (!a.done) {
		t->park_requested = 1;
		t->wake_revents = 0;
		xtc_yield();
	}
	return a.res;
}

/* PUBLIC: int xtc_aio_pread __P((int, void *, uint32_t, int64_t)); */
int
xtc_aio_pread(int fd, void *buf, uint32_t len, int64_t off)
{
	return aio_do(XTC_AIO_PREAD, fd, buf, len, off);
}

/* PUBLIC: int xtc_aio_pwrite __P((int, const void *, uint32_t, int64_t)); */
int
xtc_aio_pwrite(int fd, const void *buf, uint32_t len, int64_t off)
{
	return aio_do(XTC_AIO_PWRITE, fd, (void *)(uintptr_t)buf, len, off);
}

/* PUBLIC: int xtc_aio_fsync __P((int)); */
int
xtc_aio_fsync(int fd)
{
	return aio_do(XTC_AIO_FSYNC, fd, NULL, 0, 0);
}
