/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * src/io/io_net.c
 *	Networking helpers built on top of POSIX sockets + a thin
 *	platform fallback for Windows (Winsock).  See xtc_net.h.
 *
 *	Supported families: XTC_NET_INET (IPv4), XTC_NET_INET6 (IPv6).
 *	DNS resolution uses getaddrinfo for hostname lookup.
 *	TCP listen/dial with tunable options, UDS listen/dial (Unix),
 *	and credential passing (Linux SO_PEERCRED, BSD LOCAL_PEERCRED).
 */

#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE        /* SO_REUSEPORT, SCM_CREDENTIALS */
#define _GNU_SOURCE             /* struct ucred on Linux glibc */

#include "xtc_int.h"
#include "xtc_net.h"

#include <errno.h>
#include <stdatomic.h>
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
#  include <netdb.h>
#  include <fcntl.h>
#  include <unistd.h>
#endif

#ifndef SO_REUSEPORT
#  define SO_REUSEPORT 0
#endif

/* ----- nonblock / cloexec --------------------------------- */

#if defined(_WIN32)
/* Initialize Winsock on first call.  Idempotent. */
static void
__win_wsa_init(void)
{
	static _Atomic int done = 0;
	int expect = 0;
	if (atomic_compare_exchange_strong(&done, &expect, 1)) {
		WSADATA wsa;
		(void)WSAStartup(MAKEWORD(2, 2), &wsa);
		/* No WSACleanup paired -- process-wide one-shot. */
	}
}
#  define WSA_INIT_ONCE() __win_wsa_init()
#else
#  define WSA_INIT_ONCE() ((void)0)
#endif

int
xtc_net_setnonblock(int fd)
{
	WSA_INIT_ONCE();
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

/* Resolve hostname/port to sockaddr using getaddrinfo.
 * Returns the first matching address for the requested family.
 * For listen, host=NULL binds to all interfaces. */
static int
__resolve_addr(xtc_net_family_t fam, const char *host, int port,
               struct sockaddr_storage *out, socklen_t *outlen)
{
	struct addrinfo hints, *res, *rp;
	char portbuf[8];
	int rc;

	memset(&hints, 0, sizeof hints);
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_flags = AI_PASSIVE;  /* for listen with NULL host */

	if (fam == XTC_NET_INET)
		hints.ai_family = AF_INET;
	else if (fam == XTC_NET_INET6)
		hints.ai_family = AF_INET6;
	else
		return XTC_E_INVAL;

	snprintf(portbuf, sizeof portbuf, "%d", port);
	rc = getaddrinfo(host, portbuf, &hints, &res);
	if (rc != 0 || res == NULL)
		return XTC_E_INVAL;

	/* Use first result that matches. */
	for (rp = res; rp != NULL; rp = rp->ai_next) {
		if ((fam == XTC_NET_INET && rp->ai_family == AF_INET) ||
		    (fam == XTC_NET_INET6 && rp->ai_family == AF_INET6)) {
			memcpy(out, rp->ai_addr, rp->ai_addrlen);
			*outlen = (socklen_t)rp->ai_addrlen;
			freeaddrinfo(res);
			return XTC_OK;
		}
	}
	freeaddrinfo(res);
	return XTC_E_INVAL;
}

int
xtc_net_listen(xtc_net_family_t fam, const char *host, int port,
               const xtc_tcp_opts_t *opts, int *out_fd)
{
	int fd, rc, v;
	struct sockaddr_storage sa;
	socklen_t salen;
	int af;

	WSA_INIT_ONCE();
	if (out_fd == NULL || port <= 0 || port > 65535)
		return XTC_E_INVAL;
	if (fam != XTC_NET_INET && fam != XTC_NET_INET6)
		return XTC_E_INVAL;

	af = (fam == XTC_NET_INET6) ? AF_INET6 : AF_INET;
	fd = (int)socket(af, SOCK_STREAM, 0);
	if (fd < 0) return XTC_E_INTERNAL;

	if ((rc = xtc_net_setnonblock(fd)) != XTC_OK) {
		(void)close(fd); return rc;
	}
	(void)xtc_net_apply_tcp_opts(fd, opts);

	/* For IPv6 sockets, set IPV6_V6ONLY to avoid dual-stack issues.
	 * Cast to const char * for portability: Winsock requires a char *
	 * argument, BSD/Linux accept void *. */
	if (fam == XTC_NET_INET6) {
		v = 1;
		(void)setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY,
		    (const char *)&v, sizeof v);
	}

	if ((rc = __resolve_addr(fam, host, port, &sa, &salen)) != XTC_OK) {
		(void)close(fd); return rc;
	}
	if (bind(fd, (struct sockaddr *)&sa, salen) != 0) {
		(void)close(fd); return XTC_E_INTERNAL;
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
	struct sockaddr_storage sa;
	socklen_t salen;
	int af;

	WSA_INIT_ONCE();
	if (out_fd == NULL || host == NULL || port <= 0 || port > 65535)
		return XTC_E_INVAL;
	if (fam != XTC_NET_INET && fam != XTC_NET_INET6)
		return XTC_E_INVAL;

	if ((rc = __resolve_addr(fam, host, port, &sa, &salen)) != XTC_OK)
		return rc;

	af = (fam == XTC_NET_INET6) ? AF_INET6 : AF_INET;
	fd = (int)socket(af, SOCK_STREAM, 0);
	if (fd < 0) return XTC_E_INTERNAL;
	if ((rc = xtc_net_setnonblock(fd)) != XTC_OK) {
		(void)close(fd); return rc;
	}
	(void)xtc_net_apply_tcp_opts(fd, opts);

	if (connect(fd, (struct sockaddr *)&sa, salen) != 0) {
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
 * BSD/macOS use LOCAL_PEERCRED via getsockopt -- simpler.  We expose a
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
#else /* _WIN32 -- UDS not yet supported on Windows */
int xtc_net_unix_listen(const char *p, int *o)
{ (void)p; (void)o; return XTC_E_NOSYS; }
int xtc_net_unix_dial(const char *p, int *o)
{ (void)p; (void)o; return XTC_E_NOSYS; }
int xtc_net_unix_send_creds(int fd, const void *b, size_t l)
{ (void)fd; (void)b; (void)l; return XTC_E_NOSYS; }
int xtc_net_unix_recv_creds(int fd, void *b, size_t l, uint32_t *u, uint32_t *g, size_t *n)
{ (void)fd; (void)b; (void)l; (void)u; (void)g; (void)n; return XTC_E_NOSYS; }
#endif

/* ---- UDP -------------------------------------------------------- */

int
xtc_net_udp_socket(xtc_net_family_t fam, const char *host, int port,
                   int *out_fd)
{
	int fd, rc;
	int af;
	WSA_INIT_ONCE();
	if (out_fd == NULL) return XTC_E_INVAL;

	switch (fam) {
	case XTC_NET_INET:  af = AF_INET;  break;
	case XTC_NET_INET6: af = AF_INET6; break;
	default:            return XTC_E_NOSYS;
	}
	fd = (int)socket(af, SOCK_DGRAM, 0);
	if (fd < 0) return XTC_E_INTERNAL;
	if ((rc = xtc_net_setnonblock(fd)) != XTC_OK) {
		(void)close(fd); return rc;
	}
	if (host != NULL || port != 0) {
		struct sockaddr_storage ss;
		socklen_t slen = 0;
		memset(&ss, 0, sizeof ss);
		if (af == AF_INET) {
			struct sockaddr_in *sa = (struct sockaddr_in *)&ss;
			sa->sin_family = AF_INET;
			sa->sin_port = htons((uint16_t)port);
			if (host == NULL || host[0] == '\0' ||
			    strcmp(host, "0.0.0.0") == 0)
				sa->sin_addr.s_addr = htonl(INADDR_ANY);
			else if (inet_pton(AF_INET, host, &sa->sin_addr) != 1) {
				(void)close(fd); return XTC_E_INVAL;
			}
			slen = sizeof *sa;
		} else {
			struct sockaddr_in6 *sa = (struct sockaddr_in6 *)&ss;
			sa->sin6_family = AF_INET6;
			sa->sin6_port = htons((uint16_t)port);
			if (host == NULL || host[0] == '\0' ||
			    strcmp(host, "::") == 0)
				sa->sin6_addr = in6addr_any;
			else if (inet_pton(AF_INET6, host, &sa->sin6_addr) != 1) {
				(void)close(fd); return XTC_E_INVAL;
			}
			slen = sizeof *sa;
		}
		if (bind(fd, (struct sockaddr *)&ss, slen) != 0) {
			(void)close(fd); return XTC_E_INTERNAL;
		}
	}
	*out_fd = fd;
	return XTC_OK;
}

int
xtc_net_udp_sendto(int fd, const void *buf, size_t len,
                   const char *host, int port)
{
	struct addrinfo hints, *res = NULL;
	char portbuf[16];
	int rc;
	ssize_t n;
	if (buf == NULL || host == NULL) return XTC_E_INVAL;
	memset(&hints, 0, sizeof hints);
	hints.ai_socktype = SOCK_DGRAM;
	hints.ai_family   = AF_UNSPEC;
	snprintf(portbuf, sizeof portbuf, "%d", port);
	rc = getaddrinfo(host, portbuf, &hints, &res);
	if (rc != 0 || res == NULL) return XTC_E_INVAL;
	n = sendto(fd, buf, len, 0, res->ai_addr, res->ai_addrlen);
	freeaddrinfo(res);
	if (n < 0) {
#if defined(_WIN32)
		int we = WSAGetLastError();
		if (we == WSAEWOULDBLOCK) return XTC_E_AGAIN;
#else
		if (errno == EAGAIN || errno == EWOULDBLOCK) return XTC_E_AGAIN;
#endif
		return XTC_E_INTERNAL;
	}
	return ((size_t)n == len) ? XTC_OK : XTC_E_AGAIN;
}

int
xtc_net_udp_recvfrom(int fd, void *buf, size_t buflen,
                     char *out_host, size_t out_host_size,
                     int *out_port, size_t *out_n)
{
	struct sockaddr_storage ss;
	socklen_t slen = sizeof ss;
	ssize_t n;
	if (buf == NULL || out_n == NULL) return XTC_E_INVAL;
	n = recvfrom(fd, buf, buflen, 0, (struct sockaddr *)&ss, &slen);
	if (n < 0) {
#if defined(_WIN32)
		int we = WSAGetLastError();
		if (we == WSAEWOULDBLOCK) return XTC_E_AGAIN;
#else
		if (errno == EAGAIN || errno == EWOULDBLOCK) return XTC_E_AGAIN;
#endif
		return XTC_E_INTERNAL;
	}
	*out_n = (size_t)n;
	if (out_host != NULL && out_host_size > 0) {
		out_host[0] = '\0';
		if (ss.ss_family == AF_INET) {
			struct sockaddr_in *sa = (struct sockaddr_in *)&ss;
			(void)inet_ntop(AF_INET, &sa->sin_addr, out_host,
			    (socklen_t)out_host_size);
			if (out_port) *out_port = ntohs(sa->sin_port);
		} else if (ss.ss_family == AF_INET6) {
			struct sockaddr_in6 *sa = (struct sockaddr_in6 *)&ss;
			(void)inet_ntop(AF_INET6, &sa->sin6_addr, out_host,
			    (socklen_t)out_host_size);
			if (out_port) *out_port = ntohs(sa->sin6_port);
		}
	}
	return XTC_OK;
}

/* ---- DNS -------------------------------------------------------- */

int
xtc_dns_resolve(const char *hostname, int port,
                xtc_net_family_t fam,
                char *out_addr, size_t out_addr_size)
{
	struct addrinfo hints, *res = NULL;
	char portbuf[16];
	int rc;
	int af;
	if (hostname == NULL || out_addr == NULL || out_addr_size == 0)
		return XTC_E_INVAL;
	switch (fam) {
	case XTC_NET_INET:  af = AF_INET;  break;
	case XTC_NET_INET6: af = AF_INET6; break;
	default:            af = AF_UNSPEC; break;
	}
	memset(&hints, 0, sizeof hints);
	hints.ai_family   = af;
	hints.ai_socktype = SOCK_STREAM;
	snprintf(portbuf, sizeof portbuf, "%d", port);
	rc = getaddrinfo(hostname, portbuf, &hints, &res);
	if (rc != 0 || res == NULL) return XTC_E_INVAL;
	out_addr[0] = '\0';
	if (res->ai_family == AF_INET) {
		struct sockaddr_in *sa = (struct sockaddr_in *)res->ai_addr;
		(void)inet_ntop(AF_INET, &sa->sin_addr, out_addr,
		    (socklen_t)out_addr_size);
	} else if (res->ai_family == AF_INET6) {
		struct sockaddr_in6 *sa = (struct sockaddr_in6 *)res->ai_addr;
		(void)inet_ntop(AF_INET6, &sa->sin6_addr, out_addr,
		    (socklen_t)out_addr_size);
	}
	freeaddrinfo(res);
	return XTC_OK;
}

/* ---- length-framed transport (R2) -------------------------------
 *
 * A 4-byte big-endian length prefix + payload, loop-aware: on a
 * non-blocking fd these yield the calling fiber (via xtc_proc_wait_fd)
 * instead of blocking the loop, so several connections share one loop
 * cleanly.  recv_frame caps the claimed length so a peer cannot force
 * an unbounded allocation.  Three consumers (the PG satellite bridge,
 * kaka, sqlxtc) previously hand-rolled this.
 *
 * Layering note: these live in the net module for API discoverability
 * but call up into the proc layer for the cooperative wait.  Off a
 * loop they fall back to poll(2) (POSIX).
 */
#include "xtc_proc.h"
#include "xtc_io.h"
#include "os_time.h"
#if !defined(_WIN32)
#include <poll.h>
#endif

static int
__net_wait_fd(int fd, uint32_t interest, int64_t deadline_ns)
{
	int64_t to = -1;
	if (deadline_ns >= 0) {
		int64_t now = 0;
		(void)__os_clock_mono(&now);
		to = deadline_ns - now;
		if (to < 0) return -1;          /* timed out */
	}
	if (!xtc_pid_is_none(xtc_self())) {
		uint32_t revents = 0;
		return xtc_proc_wait_fd(fd, interest, to, &revents) == XTC_OK
		    ? 0 : -1;
	}
#if !defined(_WIN32)
	{
		struct pollfd p;
		int ms = (to < 0) ? -1 : (int)(to / 1000000);
		int r;
		p.fd = fd;
		p.events = ((interest & XTC_IO_READABLE) ? POLLIN : 0) |
		           ((interest & XTC_IO_WRITABLE) ? POLLOUT : 0);
		do { r = poll(&p, 1, ms); } while (r < 0 && errno == EINTR);
		return r > 0 ? 0 : -1;
	}
#else
	return -1;   /* off-loop wait on a non-blocking fd: unsupported */
#endif
}

/* PUBLIC: int xtc_net_send_frame __P((int, const void *, size_t)); */
int
xtc_net_send_frame(int fd, const void *buf, size_t len)
{
	uint8_t hdr[4];
	const uint8_t *p = (const uint8_t *)buf;
	size_t off;
	if (fd < 0 || (len > 0 && buf == NULL) || len > 0xFFFFFFFFu)
		return XTC_E_INVAL;
	hdr[0] = (uint8_t)(len >> 24); hdr[1] = (uint8_t)(len >> 16);
	hdr[2] = (uint8_t)(len >> 8);  hdr[3] = (uint8_t)(len);
	for (off = 0; off < 4; ) {
		ssize_t w = send(fd, (const char *)hdr + off, 4 - off, 0);
		if (w > 0) { off += (size_t)w; continue; }
		if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			if (__net_wait_fd(fd, XTC_IO_WRITABLE, -1) != 0)
				return XTC_E_INTERNAL;
			continue;
		}
		if (w < 0 && errno == EINTR) continue;
		return XTC_E_INTERNAL;
	}
	for (off = 0; off < len; ) {
		ssize_t w = send(fd, (const char *)p + off, len - off, 0);
		if (w > 0) { off += (size_t)w; continue; }
		if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			if (__net_wait_fd(fd, XTC_IO_WRITABLE, -1) != 0)
				return XTC_E_INTERNAL;
			continue;
		}
		if (w < 0 && errno == EINTR) continue;
		return XTC_E_INTERNAL;
	}
	return XTC_OK;
}

/* PUBLIC: int xtc_net_recv_frame __P((int, void **, size_t *, size_t, int64_t)); */
int
xtc_net_recv_frame(int fd, void **out, size_t *out_len, size_t max_len,
                   int64_t timeout_ns)
{
	uint8_t hdr[4];
	size_t off, len;
	uint8_t *frame;
	int64_t deadline = -1;
	if (fd < 0 || out == NULL || out_len == NULL) return XTC_E_INVAL;
	*out = NULL; *out_len = 0;
	if (timeout_ns >= 0) {
		int64_t now = 0;
		(void)__os_clock_mono(&now);
		deadline = now + timeout_ns;
	}
	for (off = 0; off < 4; ) {
		ssize_t r = recv(fd, (char *)hdr + off, 4 - off, 0);
		if (r > 0) { off += (size_t)r; continue; }
		if (r == 0) return XTC_E_INVAL;        /* peer closed */
		if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			if (__net_wait_fd(fd, XTC_IO_READABLE, deadline) != 0)
				return XTC_E_AGAIN;
			continue;
		}
		if (r < 0 && errno == EINTR) continue;
		return XTC_E_INTERNAL;
	}
	len = ((size_t)hdr[0] << 24) | ((size_t)hdr[1] << 16) |
	      ((size_t)hdr[2] << 8) | (size_t)hdr[3];
	/* OOM guard: a peer must not be able to claim an unbounded frame. */
	if (max_len > 0 && len > max_len) return XTC_E_RANGE;
	if (len == 0) return XTC_OK;               /* valid empty frame */
	if (__os_malloc(len, (void **)&frame) != XTC_OK) return XTC_E_NOMEM;
	for (off = 0; off < len; ) {
		ssize_t r = recv(fd, (char *)frame + off, len - off, 0);
		if (r > 0) { off += (size_t)r; continue; }
		if (r == 0) { __os_free(frame); return XTC_E_INVAL; }
		if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			if (__net_wait_fd(fd, XTC_IO_READABLE, deadline) != 0) {
				__os_free(frame);
				return XTC_E_AGAIN;
			}
			continue;
		}
		if (r < 0 && errno == EINTR) continue;
		__os_free(frame);
		return XTC_E_INTERNAL;
	}
	*out = frame;
	*out_len = len;
	return XTC_OK;
}
