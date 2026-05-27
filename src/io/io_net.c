/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * src/io/io_net.c
 *	Networking helpers built on top of POSIX sockets + a thin
 *	platform fallback for Windows (Winsock).  See xtc_net.h.
 *
 *	M19.1 v1: TCP listen/dial with knobs, UDS listen/dial,
 *	credential passing.  Accept-distribution scaling and DNS
 *	resolution are tracked for v2.
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE        /* SO_REUSEPORT, SCM_CREDENTIALS */
#define _GNU_SOURCE             /* struct ucred on Linux glibc */

#include "xtc_int.h"
#include "xtc_net.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <io.h>
   /* Map errno-style codes to WSA. */
#  define ssize_t intptr_t
#  define close(fd)        closesocket(fd)
#  define SHUT_RDWR        SD_BOTH
#else
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <sys/un.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <unistd.h>
#endif

#ifndef SO_REUSEPORT
#  define SO_REUSEPORT 0
#endif

/* ----- nonblock / cloexec --------------------------------- */

int
xtc_net_setnonblock(int fd)
{
#if defined(_WIN32)
	u_long mode = 1;
	if (ioctlsocket(fd, FIONBIO, &mode) != 0) return XTC_E_INTERNAL;
	return XTC_OK;
#else
	int fl = fcntl(fd, F_GETFL);
	if (fl < 0) return XTC_E_INTERNAL;
	if (fcntl(fd, F_SETFL, fl | O_NONBLOCK) < 0) return XTC_E_INTERNAL;
	fl = fcntl(fd, F_GETFD);
	if (fl < 0) return XTC_E_INTERNAL;
	if (fcntl(fd, F_SETFD, fl | FD_CLOEXEC) < 0) return XTC_E_INTERNAL;
	return XTC_OK;
#endif
}

void
xtc_net_close(int fd) { if (fd >= 0) (void)close(fd); }

/* ----- TCP knobs ----------------------------------------- */

int
xtc_net_apply_tcp_opts(int fd, const xtc_tcp_opts_t *opts)
{
	int v;
	if (opts == NULL) return XTC_OK;

#if !defined(_WIN32)
	if (opts->nodelay) {
		v = 1;
		(void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
		    (const char *)&v, sizeof v);
	}
	if (opts->reuseaddr) {
		v = 1;
		(void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
		    (const char *)&v, sizeof v);
	}
#  if SO_REUSEPORT
	if (opts->reuseport) {
		v = 1;
		(void)setsockopt(fd, SOL_SOCKET, SO_REUSEPORT,
		    (const char *)&v, sizeof v);
	}
#  endif
	if (opts->keepalive) {
		v = 1;
		(void)setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE,
		    (const char *)&v, sizeof v);
#    ifdef TCP_KEEPIDLE
		if (opts->keepidle_s > 0) {
			v = opts->keepidle_s;
			(void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE,
			    &v, sizeof v);
		}
#    endif
#    ifdef TCP_KEEPINTVL
		if (opts->keepintvl_s > 0) {
			v = opts->keepintvl_s;
			(void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL,
			    &v, sizeof v);
		}
#    endif
#    ifdef TCP_KEEPCNT
		if (opts->keepcnt > 0) {
			v = opts->keepcnt;
			(void)setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT,
			    &v, sizeof v);
		}
#    endif
	}
#    ifdef TCP_USER_TIMEOUT
	if (opts->user_timeout_ms > 0) {
		v = opts->user_timeout_ms;
		(void)setsockopt(fd, IPPROTO_TCP, TCP_USER_TIMEOUT,
		    &v, sizeof v);
	}
#    endif
#else
	/* Windows: BSD-style with CHAR* casts; SO_REUSEPORT not portable. */
	if (opts->nodelay) {
		v = 1;
		(void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY,
		    (const char *)&v, sizeof v);
	}
	if (opts->reuseaddr) {
		v = 1;
		(void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR,
		    (const char *)&v, sizeof v);
	}
	if (opts->keepalive) {
		v = 1;
		(void)setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE,
		    (const char *)&v, sizeof v);
	}
#endif
	return XTC_OK;
}

/* ----- TCP listen / dial ---------------------------------- */

static int
__sockaddr_inet(const char *host, int port, struct sockaddr_in *out)
{
	memset(out, 0, sizeof *out);
	out->sin_family = AF_INET;
	out->sin_port = htons((uint16_t)port);
	if (host == NULL || host[0] == '\0') {
		out->sin_addr.s_addr = htonl(INADDR_ANY);
	} else {
		if (inet_pton(AF_INET, host, &out->sin_addr) != 1)
			return XTC_E_INVAL;
	}
	return XTC_OK;
}

int
xtc_net_listen(xtc_net_family_t fam, const char *host, int port,
               const xtc_tcp_opts_t *opts, int *out_fd)
{
	int fd, rc;
	if (out_fd == NULL || port <= 0 || port > 65535)
		return XTC_E_INVAL;
	if (fam != XTC_NET_INET) return XTC_E_NOSYS;   /* v1 INET only */

	fd = (int)socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return XTC_E_INTERNAL;

	if ((rc = xtc_net_setnonblock(fd)) != XTC_OK) {
		(void)close(fd); return rc;
	}
	(void)xtc_net_apply_tcp_opts(fd, opts);

	{
		struct sockaddr_in sa;
		if ((rc = __sockaddr_inet(host, port, &sa)) != XTC_OK) {
			(void)close(fd); return rc;
		}
		if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
			(void)close(fd); return XTC_E_INTERNAL;
		}
	}
	if (listen(fd, 128) != 0) {
		(void)close(fd); return XTC_E_INTERNAL;
	}
	*out_fd = fd;
	return XTC_OK;
}

int
xtc_net_dial(xtc_net_family_t fam, const char *host, int port,
             const xtc_tcp_opts_t *opts, int *out_fd)
{
	int fd, rc;
	if (out_fd == NULL || host == NULL || port <= 0 || port > 65535)
		return XTC_E_INVAL;
	if (fam != XTC_NET_INET) return XTC_E_NOSYS;

	fd = (int)socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) return XTC_E_INTERNAL;
	if ((rc = xtc_net_setnonblock(fd)) != XTC_OK) {
		(void)close(fd); return rc;
	}
	(void)xtc_net_apply_tcp_opts(fd, opts);
	{
		struct sockaddr_in sa;
		if ((rc = __sockaddr_inet(host, port, &sa)) != XTC_OK) {
			(void)close(fd); return rc;
		}
		if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
#if defined(_WIN32)
			int we = WSAGetLastError();
			if (we != WSAEWOULDBLOCK && we != WSAEINPROGRESS) {
				(void)close(fd); return XTC_E_INTERNAL;
			}
#else
			if (errno != EINPROGRESS && errno != EAGAIN) {
				(void)close(fd); return XTC_E_INTERNAL;
			}
#endif
		}
	}
	*out_fd = fd;
	return XTC_OK;
}

/* ----- Unix domain sockets ------------------------------- */

#if !defined(_WIN32)
int
xtc_net_unix_listen(const char *path, int *out_fd)
{
	int fd, rc;
	struct sockaddr_un sa;
	if (out_fd == NULL || path == NULL) return XTC_E_INVAL;
	if (strlen(path) >= sizeof sa.sun_path) return XTC_E_INVAL;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) return XTC_E_INTERNAL;
	if ((rc = xtc_net_setnonblock(fd)) != XTC_OK) {
		(void)close(fd); return rc;
	}
	memset(&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX;
	strncpy(sa.sun_path, path, sizeof sa.sun_path - 1);
	(void)unlink(path);
	if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
		(void)close(fd); return XTC_E_INTERNAL;
	}
	if (listen(fd, 64) != 0) {
		(void)close(fd); return XTC_E_INTERNAL;
	}
	*out_fd = fd;
	return XTC_OK;
}

int
xtc_net_unix_dial(const char *path, int *out_fd)
{
	int fd, rc;
	struct sockaddr_un sa;
	if (out_fd == NULL || path == NULL) return XTC_E_INVAL;
	if (strlen(path) >= sizeof sa.sun_path) return XTC_E_INVAL;
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) return XTC_E_INTERNAL;
	if ((rc = xtc_net_setnonblock(fd)) != XTC_OK) {
		(void)close(fd); return rc;
	}
	memset(&sa, 0, sizeof sa);
	sa.sun_family = AF_UNIX;
	strncpy(sa.sun_path, path, sizeof sa.sun_path - 1);
	if (connect(fd, (struct sockaddr *)&sa, sizeof sa) != 0) {
		if (errno != EINPROGRESS && errno != EAGAIN) {
			(void)close(fd); return XTC_E_INTERNAL;
		}
	}
	*out_fd = fd;
	return XTC_OK;
}

/* Credential passing: send/recv helper.  Linux uses SCM_CREDENTIALS;
 * BSD/macOS use LOCAL_PEERCRED via getsockopt — simpler.  We expose a
 * unified API: send routes plain bytes; recv extracts uid/gid from
 * the peer at the time of receipt. */

int
xtc_net_unix_send_creds(int fd, const void *buf, size_t buflen)
{
	ssize_t n = send(fd, buf, buflen, 0);
	if (n < 0) return XTC_E_INTERNAL;
	if ((size_t)n != buflen) return XTC_E_AGAIN;
	return XTC_OK;
}

int
xtc_net_unix_recv_creds(int fd, void *buf, size_t buflen,
                        uint32_t *out_uid, uint32_t *out_gid,
                        size_t *out_n)
{
	ssize_t n;
	if (out_n == NULL) return XTC_E_INVAL;
	n = recv(fd, buf, buflen, 0);
	if (n < 0) {
		if (errno == EAGAIN || errno == EWOULDBLOCK) return XTC_E_AGAIN;
		return XTC_E_INTERNAL;
	}
	*out_n = (size_t)n;

#if defined(__linux__) && defined(SO_PEERCRED)
	{
		struct ucred uc;
		socklen_t ulen = sizeof uc;
		if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &uc, &ulen) == 0) {
			if (out_uid) *out_uid = (uint32_t)uc.uid;
			if (out_gid) *out_gid = (uint32_t)uc.gid;
			return XTC_OK;
		}
	}
#elif defined(LOCAL_PEERCRED) && defined(__BSD_VISIBLE)
	/* FreeBSD/macOS path; struct xucred lives in <sys/ucred.h>. */
#  include <sys/ucred.h>
	{
		struct xucred uc;
		socklen_t ulen = sizeof uc;
		if (getsockopt(fd, 0, LOCAL_PEERCRED, &uc, &ulen) == 0) {
			if (out_uid) *out_uid = (uint32_t)uc.cr_uid;
			if (out_gid) *out_gid = (uint32_t)uc.cr_groups[0];
			return XTC_OK;
		}
	}
#endif
	if (out_uid) *out_uid = 0;
	if (out_gid) *out_gid = 0;
	return XTC_OK;
}
#else /* _WIN32 — UDS not yet supported on Windows */
int xtc_net_unix_listen(const char *p, int *o)
{ (void)p; (void)o; return XTC_E_NOSYS; }
int xtc_net_unix_dial(const char *p, int *o)
{ (void)p; (void)o; return XTC_E_NOSYS; }
int xtc_net_unix_send_creds(int fd, const void *b, size_t l)
{ (void)fd; (void)b; (void)l; return XTC_E_NOSYS; }
int xtc_net_unix_recv_creds(int fd, void *b, size_t l, uint32_t *u, uint32_t *g, size_t *n)
{ (void)fd; (void)b; (void)l; (void)u; (void)g; (void)n; return XTC_E_NOSYS; }
#endif
