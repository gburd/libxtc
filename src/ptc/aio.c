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
#include "xtc_fs.h"
#include "xtc_blocking.h"
#include "loop_int.h"
#include "coro_int.h"

#include <errno.h>
#include <string.h>
#if defined(XTC_DIAGNOSTIC)
#  include <stdio.h>
#  include <stdlib.h>
#  include <stdint.h>
extern int __xtc_dio_is_direct(int fd);
#endif
#if defined(_WIN32)
#  include <stdio.h>       /* SEEK_SET */
#  include <io.h>          /* _read/_write/_lseeki64/_commit */
#else
#  include <unistd.h>
#  include <sys/uio.h>     /* preadv/pwritev, struct iovec */
#  include <limits.h>      /* IOV_MAX */
#endif
#include <stdlib.h>        /* getenv */

#ifndef IOV_MAX
#  define IOV_MAX 1024
#endif

/*
 * Force the blocking-pool offload path even where a native completion
 * engine (io_uring / IOCP) exists.  This exists so the portable
 * fallback can be exercised and proven byte-for-byte identical to the
 * native path on a host that also has the native one (e.g. CI on Linux
 * with io_uring): a consumer's "write once, runs as AIO everywhere"
 * expectation is only credible if the non-native path is actually
 * tested where the tests run.  0 = auto (native where available),
 * 1 = always offload.  Resolved from XTC_AIO_FORCE_OFFLOAD on first use
 * and overridable via __xtc_aio_force_offload().
 */
static int g_force_offload = -1;   /* -1 = unresolved */

/* PUBLIC (internal): set by tests / tuning to force the offload path. */
void
__xtc_aio_force_offload(int on)
{
	g_force_offload = on ? 1 : 0;
}

static int
aio_offload_forced(void)
{
	if (g_force_offload < 0) {
		const char *e = getenv("XTC_AIO_FORCE_OFFLOAD");
		g_force_offload = (e != NULL && e[0] == '1') ? 1 : 0;
	}
	return g_force_offload;
}

/* Blocking-pool fallback: run the op on a worker thread.  Portable
 * across the platforms libxtc builds on -- the native (non-blocking)
 * path is io_uring-only anyway, so this is purely the offload case. */
struct aio_blk { int fd; int op; void *buf; uint32_t len; int64_t off;
                 const void *iov; int iovcnt; };

static int
aio_blk_fn(void *arg)
{
	struct aio_blk *b = arg;
#if defined(_WIN32)
	int n;
	if (b->op == XTC_AIO_FSYNC || b->op == XTC_AIO_FDATASYNC)
		return _commit(b->fd) == 0 ? 0 : -EIO;  /* Windows: one durability level */
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
	case XTC_AIO_PREADV:
		n = preadv(b->fd, (const struct iovec *)b->iov, b->iovcnt,
		    (off_t)b->off);                               /* XTC_BLOCKING_OK: offloaded */
		return n < 0 ? -errno : (int)n;
	case XTC_AIO_PWRITEV:
		n = pwritev(b->fd, (const struct iovec *)b->iov, b->iovcnt,
		    (off_t)b->off);                               /* XTC_BLOCKING_OK: offloaded */
		return n < 0 ? -errno : (int)n;
	case XTC_AIO_FSYNC:
		return fsync(b->fd) == 0 ? 0 : -errno;
	case XTC_AIO_FDATASYNC:
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
	memset(&b, 0, sizeof b);
	b.fd = fd; b.op = op; b.buf = buf; b.len = len; b.off = off;
	if (xtc_blocking_run(aio_blk_fn, &b, &rc) != XTC_OK)
		rc = aio_blk_fn(&b);   /* off a loop: run inline */
	return rc;
}

#if !defined(_WIN32)
static int
aio_offload_v(int op, int fd, const struct iovec *iov, int iovcnt,
              int64_t off)
{
	struct aio_blk b;
	int rc;
	memset(&b, 0, sizeof b);
	b.fd = fd; b.op = op; b.off = off; b.iov = iov; b.iovcnt = iovcnt;
	if (xtc_blocking_run(aio_blk_fn, &b, &rc) != XTC_OK)
		rc = aio_blk_fn(&b);
	return rc;
}
#endif

static int
aio_do(int op, int fd, void *buf, uint32_t len, int64_t off)
{
	xtc_task_t  *t = __xtc_current_task();
	xtc_loop_t  *loop = __xtc_current_loop;
	xtc_aio_t    a;
	int          rc;

#if defined(XTC_DIAGNOSTIC)
	/* Direct I/O requires the buffer address, file offset and transfer
	 * length to meet the device's alignment.  In a DIAGNOSTIC build,
	 * abort loudly on a violation rather than letting the kernel return
	 * a confusing EINVAL at runtime. */
	if ((op == XTC_AIO_PREAD || op == XTC_AIO_PWRITE) &&
	    __xtc_dio_is_direct(fd)) {
		size_t ma = 4096, oa = 4096, la = 4096;
		(void)xtc_fs_dio_align(fd, &ma, &oa, &la);
		if ((ma && ((uintptr_t)buf & (ma - 1))) ||
		    (oa && ((uint64_t)off & (oa - 1))) ||
		    (la && ((uint64_t)len & (la - 1)))) {
			fprintf(stderr,
			    "XTC DIAGNOSTIC: unaligned direct I/O on fd %d: "
			    "buf=%p off=%lld len=%u "
			    "(require mem%%%zu off%%%zu len%%%zu)\n",
			    fd, buf, (long long)off, (unsigned)len, ma, oa, la);
			abort();
		}
	}
#endif

	/* Off a loop fiber: there is nothing to park, so run it inline
	 * (the blocking-pool path, which itself runs inline with no loop). */
	if (t == NULL || loop == NULL || loop->io == NULL)
		return aio_offload(op, fd, buf, len, off);

	/* Forced offload (test / tuning): take the portable thread-pool path
	 * even though a native engine is present.  Still parks the fiber and
	 * keeps the loop live -- same observable behavior as native AIO. */
	if (aio_offload_forced())
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

/* PUBLIC: int xtc_aio_fdatasync __P((int)); */
int
xtc_aio_fdatasync(int fd)
{
	return aio_do(XTC_AIO_FDATASYNC, fd, NULL, 0, 0);
}

#if !defined(_WIN32)
/*
 * Vectored submit path: parallels aio_do but carries an iovec array.
 * Native io_uring (IORING_OP_READV/WRITEV) when on a loop with the
 * uring backend; otherwise the blocking-pool preadv/pwritev fallback.
 */
static int
aio_do_v(int op, int fd, const struct iovec *iov, int iovcnt, int64_t off)
{
	xtc_task_t  *t = __xtc_current_task();
	xtc_loop_t  *loop = __xtc_current_loop;
	xtc_aio_t    a;
	int          rc;

	if (iov == NULL || iovcnt < 1 || iovcnt > IOV_MAX)
		return -EINVAL;   /* bounded submission; negative-errno contract */

#if defined(XTC_DIAGNOSTIC)
	/* Direct I/O: every iovec base must meet the memory alignment, the
	 * offset the offset alignment, and the TOTAL length the length
	 * alignment -- else the kernel returns a confusing EINVAL. */
	if (__xtc_dio_is_direct(fd)) {
		size_t ma = 4096, oa = 4096, la = 4096;
		uint64_t total = 0;
		int i;
		(void)xtc_fs_dio_align(fd, &ma, &oa, &la);
		for (i = 0; i < iovcnt; i++) {
			if (ma && ((uintptr_t)iov[i].iov_base & (ma - 1))) {
				fprintf(stderr, "XTC DIAGNOSTIC: unaligned "
				    "direct vectored I/O on fd %d: iov[%d].base"
				    "=%p (require mem%%%zu)\n", fd, i,
				    iov[i].iov_base, ma);
				abort();
			}
			total += (uint64_t)iov[i].iov_len;
		}
		if ((oa && ((uint64_t)off & (oa - 1))) ||
		    (la && (total & (la - 1)))) {
			fprintf(stderr, "XTC DIAGNOSTIC: unaligned direct "
			    "vectored I/O on fd %d: off=%lld total=%llu "
			    "(require off%%%zu len%%%zu)\n", fd,
			    (long long)off, (unsigned long long)total, oa, la);
			abort();
		}
	}
#endif

	if (t == NULL || loop == NULL || loop->io == NULL)
		return aio_offload_v(op, fd, iov, iovcnt, off);
	if (aio_offload_forced())
		return aio_offload_v(op, fd, iov, iovcnt, off);

	memset(&a, 0, sizeof a);
	a.fd = fd; a.op = op; a.off = off;
	a.iov = (void *)(uintptr_t)iov; a.iovcnt = iovcnt;
	a.tag = t;
	rc = xtc_io_aio_submit(loop->io, &a);
	if (rc != XTC_OK)
		return aio_offload_v(op, fd, iov, iovcnt, off);

	while (!a.done) {
		t->park_requested = 1;
		t->wake_revents = 0;
		xtc_yield();
	}
	return a.res;
}

/* PUBLIC: int xtc_aio_preadv __P((int, const struct iovec *, int, int64_t)); */
int
xtc_aio_preadv(int fd, const struct iovec *iov, int iovcnt, int64_t off)
{
	return aio_do_v(XTC_AIO_PREADV, fd, iov, iovcnt, off);
}

/* PUBLIC: int xtc_aio_pwritev __P((int, const struct iovec *, int, int64_t)); */
int
xtc_aio_pwritev(int fd, const struct iovec *iov, int iovcnt, int64_t off)
{
	return aio_do_v(XTC_AIO_PWRITEV, fd, iov, iovcnt, off);
}
#endif /* !_WIN32 */
