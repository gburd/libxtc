/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * src/io/io_common.c
 *	Lifecycle code shared by every L1 backend: alloc the io struct,
 *	create the cross-thread wakeup self-pipe, register the read end
 *	with the backend, expose the backend name.
 */

#define _POSIX_C_SOURCE 200809L

#include "xtc_int.h"
#include "io_int.h"
#include "xtc_inject.h"

#include <errno.h>

#if defined(_WIN32)
# include <winsock2.h>
# include <ws2tcpip.h>
#else
# include <fcntl.h>
# include <unistd.h>
#endif

/* Set FD_CLOEXEC and O_NONBLOCK on a file descriptor.  POSIX only;
 * the Windows path uses ioctlsocket(FIONBIO) below. */
#if !defined(_WIN32)
static int
__set_cloexec_nonblock(int fd)
{
	int flags = fcntl(fd, F_GETFD);
	if (flags == -1) return XTC_E_INTERNAL;
	if (fcntl(fd, F_SETFD, flags | FD_CLOEXEC) == -1) return XTC_E_INTERNAL;

	flags = fcntl(fd, F_GETFL);
	if (flags == -1) return XTC_E_INTERNAL;
	if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) return XTC_E_INTERNAL;
	return XTC_OK;
}
#endif

/* Forward-declared; each backend file provides one. */
int  __xtc_io_backend_init(xtc_io_t *io);
void __xtc_io_backend_fini(xtc_io_t *io);
int  __xtc_io_register_wakeup(xtc_io_t *io, int fd);

/*
 * PUBLIC: int xtc_io_init __P((xtc_io_t **));
 */
int
xtc_io_init(xtc_io_t **out)
{
	xtc_io_t *io;
	int rc;

	if (out == NULL)
		return XTC_E_INVAL;
	if (xtc_inject_check("io.init.calloc_fail")) {
		xtc_inject_trigger("io.init.calloc_fail");
		return XTC_E_NOMEM;
	}
	if ((rc = __os_calloc(1, sizeof(*io), (void **)&io)) != XTC_OK)
		return rc;

	io->wakeup_rfd = -1;
	io->wakeup_wfd = -1;

#if defined(_WIN32)
	/* Windows IOCP wakeup uses PostQueuedCompletionStatus directly
	 * — no socket pair needed.  __xtc_io_register_wakeup is a no-op
	 * but still called for symmetry. */
#else
	{
		int p[2];
		int pipe_rc;
		/* The wakeup self-pipe.  Reads on rfd, writes on wfd. */
		if (xtc_inject_check("io.init.pipe_fail")) {
			xtc_inject_trigger("io.init.pipe_fail");
			pipe_rc = -1;
		} else {
			pipe_rc = pipe(p);
		}
		if (pipe_rc != 0) {
			__os_free(io);
			return XTC_E_INTERNAL;
		}
		if (xtc_inject_check("io.init.fcntl_fail")) {
			xtc_inject_trigger("io.init.fcntl_fail");
			(void)close(p[0]); (void)close(p[1]);
			__os_free(io);
			return XTC_E_INTERNAL;
		}
		if ((rc = __set_cloexec_nonblock(p[0])) != XTC_OK ||
		    (rc = __set_cloexec_nonblock(p[1])) != XTC_OK) {
			(void)close(p[0]); (void)close(p[1]);
			__os_free(io);
			return rc;
		}
		io->wakeup_rfd = p[0];
		io->wakeup_wfd = p[1];
	}
#endif

	if (xtc_inject_check("io.init.backend_fail")) {
		xtc_inject_trigger("io.init.backend_fail");
		rc = XTC_E_INTERNAL;
	} else {
		rc = __xtc_io_backend_init(io);
	}
	if (rc != XTC_OK) {
#if !defined(_WIN32)
		(void)close(io->wakeup_rfd);
		(void)close(io->wakeup_wfd);
#endif
		__os_free(io);
		return rc;
	}

	if ((rc = __xtc_io_register_wakeup(io, io->wakeup_rfd)) != XTC_OK) {
		__xtc_io_backend_fini(io);
#if !defined(_WIN32)
		(void)close(io->wakeup_rfd);
		(void)close(io->wakeup_wfd);
#endif
		__os_free(io);
		return rc;
	}

	*out = io;
	return XTC_OK;
}

/*
 * PUBLIC: int xtc_io_fini __P((xtc_io_t *));
 */
int
xtc_io_fini(xtc_io_t *io)
{
	if (io == NULL)
		return XTC_E_INVAL;
	__xtc_io_backend_fini(io);
#if !defined(_WIN32)
	if (io->wakeup_rfd >= 0) (void)close(io->wakeup_rfd);
	if (io->wakeup_wfd >= 0) (void)close(io->wakeup_wfd);
#endif
	__os_free(io);
	return XTC_OK;
}

#if defined(_WIN32)
/* Forward decl for the IOCP backend's wakeup post (defined in
 * io_iocp.c).  We can't include windows.h here cleanly without
 * include-order pain, so the actual PostQueuedCompletionStatus call
 * lives in the backend file. */
int __xtc_io_iocp_wakeup_post(xtc_io_t *io);
#endif

/*
 * PUBLIC: int xtc_io_wakeup __P((xtc_io_t *));
 */
int
xtc_io_wakeup(xtc_io_t *io)
{
	unsigned char b = 1;
	if (io == NULL)
		return XTC_E_INVAL;
#if defined(_WIN32)
	return __xtc_io_iocp_wakeup_post(io);
#else
	for (;;) {
		ssize_t n = write(io->wakeup_wfd, &b, 1);
		if (n == 1)
			return XTC_OK;
		if (n == -1 && errno == EINTR)
			continue;
		if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return XTC_OK;   /* already pending; coalesced */
		return XTC_E_INTERNAL;
	}
	(void)b;
#endif
}

/*
 * Drain the wakeup pipe.  Called by each backend when its poll
 * surfaces readiness on wakeup_rfd.
 */
int
__xtc_io_drain_wakeup(xtc_io_t *io)
{
#if defined(_WIN32)
	/* IOCP wakeup is consumed by GetQueuedCompletionStatusEx;
	 * nothing to drain here. */
	(void)io;
	return XTC_OK;
#else
	unsigned char buf[64];
	for (;;) {
		ssize_t n = read(io->wakeup_rfd, buf, sizeof buf);
		if (n > 0) continue;
		if (n == -1 && errno == EINTR) continue;
		if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
			return XTC_OK;
		if (n == 0) return XTC_OK;          /* EOF: pipe closed */
		return XTC_E_INTERNAL;
	}
#endif
}

/*
 * PUBLIC: const char *xtc_io_backend_name __P((void));
 */
const char *
xtc_io_backend_name(void)
{
#if defined(XTC_IO_BACKEND_EPOLL)
	return "epoll";
#elif defined(XTC_IO_BACKEND_URING)
	return "uring";
#elif defined(XTC_IO_BACKEND_KQUEUE)
	return "kqueue";
#elif defined(XTC_IO_BACKEND_IOCP)
	return "iocp";
#elif defined(XTC_IO_BACKEND_SOLARIS)
	return "solaris";
#elif defined(XTC_IO_BACKEND_AIX)
	return "aix";
#elif defined(XTC_IO_BACKEND_POLL)
	return "poll";
#else
	return "unknown";
#endif
}
