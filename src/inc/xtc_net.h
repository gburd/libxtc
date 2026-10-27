/*-
 * Copyright (c) 2026, The XTC Project
 * @stability stable
 * Use of this source code is governed by the ISC License,
 * a copy of which is in the file LICENSE in the top-level directory
 * of this distribution.
 *
 * src/inc/xtc_net.h
 *	Networking helpers built on top of xtc_io.  These are the
 *	first slice of M19.1: TCP knobs + Unix domain sockets +
 *	cred-passing.  Future passes will add accept scaling
 *	(SO_REUSEPORT distribution), TCP_USER_TIMEOUT, congestion
 *	algorithm selection, etc.
 *
 *	The socket family is identified by xtc_net_family_t; xtc owns
 *	the fd and exposes it for direct use with xtc_io_reg_fd /
 *	xtc_io_poll.  None of these calls block -- caller-driven I/O
 *	is the contract.
 */

#ifndef XTC_NET_H
#define XTC_NET_H

#include "xtc_export.h"

#include <stddef.h>
#include <stdint.h>

#include "xtc.h"

typedef enum xtc_net_family {
	XTC_NET_INET     = 0,
	XTC_NET_INET6    = 1,
	XTC_NET_UNIX     = 2
} xtc_net_family_t;

/* TCP-level knobs.  Defaults pick reasonable server-side values:
 * TCP_NODELAY=1, SO_REUSEADDR=1, KEEPALIVE off (caller opts in). */
typedef struct xtc_tcp_opts {
	size_t size;           /* sizeof(xtc_tcp_opts_t): XTC_TCP_OPTS_DEFAULT */
	int  nodelay;          /* TCP_NODELAY: 1 = disable Nagle */
	int  reuseaddr;        /* SO_REUSEADDR */
	int  reuseport;        /* SO_REUSEPORT (Linux/BSD; falls back to no-op) */
	int  keepalive;        /* SO_KEEPALIVE */
	int  keepidle_s;       /* TCP_KEEPIDLE in seconds (Linux); 0 = OS default */
	int  keepintvl_s;      /* TCP_KEEPINTVL in seconds (Linux); 0 = OS default */
	int  keepcnt;          /* TCP_KEEPCNT (Linux); 0 = OS default */
	int  user_timeout_ms;  /* TCP_USER_TIMEOUT in ms (Linux); 0 = OS default */
} xtc_tcp_opts_t;

#define XTC_TCP_OPTS_DEFAULT { \
	.size = sizeof(xtc_tcp_opts_t), \
	.nodelay = 1, .reuseaddr = 1, .reuseport = 0, .keepalive = 0, \
	.keepidle_s = 0, .keepintvl_s = 0, .keepcnt = 0, .user_timeout_ms = 0 \
}

/*
 * PUBLIC: int xtc_net_listen __P((xtc_net_family_t, const char *, int, const xtc_tcp_opts_t *, int *));
 * PUBLIC: int xtc_net_listen_backlog __P((xtc_net_family_t, const char *, int, const xtc_tcp_opts_t *, int, int *));
 * PUBLIC: int xtc_net_dial __P((xtc_net_family_t, const char *, int, const xtc_tcp_opts_t *, int *));
 * PUBLIC: int xtc_net_accept __P((int, int *));
 * PUBLIC: int xtc_net_apply_tcp_opts __P((int, const xtc_tcp_opts_t *));
 * PUBLIC: int xtc_net_setnonblock __P((int));
 * PUBLIC: void xtc_net_close __P((int));
 *
 * PUBLIC: int xtc_net_unix_listen __P((const char *, int *));
 * PUBLIC: int xtc_net_unix_dial __P((const char *, int *));
 * PUBLIC: int xtc_net_unix_send_creds __P((int, const void *, size_t));
 * PUBLIC: int xtc_net_unix_recv_creds __P((int, void *, size_t, uint32_t *, uint32_t *, size_t *));
 *
 * PUBLIC: int xtc_net_udp_socket __P((xtc_net_family_t, const char *, int, int *));
 * PUBLIC: int xtc_net_udp_sendto __P((int, const void *, size_t, const char *, int));
 * PUBLIC: int xtc_net_udp_recvfrom __P((int, void *, size_t, char *, size_t, int *, size_t *));
 *
 * PUBLIC: int xtc_dns_resolve __P((const char *, int, xtc_net_family_t, char *, size_t));
 */

/* TCP listen socket.  `host` may be NULL/"" for any-address.  Returns
 * the listening fd; caller registers it with xtc_io_reg_fd for accept
 * readiness.  The accept-queue depth is the default: SOMAXCONN, or 4096
 * if that is smaller, clamped by the kernel to its own limit
 * (net.core.somaxconn on Linux).  It was a fixed 128 through 1.55.0. */
XTC_API int xtc_net_listen(xtc_net_family_t fam, const char *host, int port,
                           const xtc_tcp_opts_t *opts, int *out_fd);

/* xtc_net_listen with an explicit accept-queue depth.  backlog 0 means
 * the default above; negative is XTC_E_INVAL.  The kernel clamps it. */
XTC_API int xtc_net_listen_backlog(xtc_net_family_t fam, const char *host,
                                   int port, const xtc_tcp_opts_t *opts,
                                   int backlog, int *out_fd);

/* TCP connect to host:port.  Returns the fd in out_fd.  The connect
 * is non-blocking; caller polls for writability to detect completion. */
/* Accept one pending connection on listen_fd as a non-blocking, CLOEXEC
 * fd.  XTC_E_AGAIN when none is pending.  Charged against XTC_RES_FDS when
 * xtc_res_attach_net is in effect (XTC_E_RESOURCE at the cap; the
 * connection stays in the backlog); xtc_net_close refunds it. */
XTC_API int xtc_net_accept(int listen_fd, int *out_fd);
XTC_API int xtc_net_dial(xtc_net_family_t fam, const char *host, int port,
                         const xtc_tcp_opts_t *opts, int *out_fd);

/* Apply TCP knobs to an already-open socket. */
XTC_API int xtc_net_apply_tcp_opts(int fd, const xtc_tcp_opts_t *opts);

/* Set O_NONBLOCK + FD_CLOEXEC. */
XTC_API int xtc_net_setnonblock(int fd);

/* Close (always succeeds). */
XTC_API void xtc_net_close(int fd);

/* Unix domain sockets -- SOCK_STREAM.  The listener's backlog is the
 * same default as xtc_net_listen (it was 64 through 1.55.0); to change
 * it, call listen(2) again on the returned fd with the depth you want. */
XTC_API int xtc_net_unix_listen(const char *path, int *out_fd);
XTC_API int xtc_net_unix_dial  (const char *path, int *out_fd);

/* Send a message + the sender's credentials (uid/gid/pid) over a
 * UNIX socket via SCM_CREDENTIALS / LOCAL_PEERCRED.  The receiver
 * extracts the credentials with xtc_net_unix_recv_creds. */
XTC_API int xtc_net_unix_send_creds(int fd, const void *buf, size_t buflen);
XTC_API int xtc_net_unix_recv_creds(int fd, void *buf, size_t buflen,
                                    uint32_t *out_uid, uint32_t *out_gid,
                                    size_t *out_n);

/* ---- UDP ----------------------------------------------------- */

/* Open a UDP datagram socket bound to (host, port).  host=NULL
 * means INADDR_ANY / in6addr_any.  port=0 means kernel-assigned.
 * Returns the fd in non-blocking + cloexec mode. */
XTC_API int xtc_net_udp_socket(xtc_net_family_t fam, const char *host,
                               int port, int *out_fd);

/* Send a single datagram to host:port.  Returns XTC_OK on full
 * send, XTC_E_AGAIN if the kernel buffer is full (caller should
 * poll for writability). */
XTC_API int xtc_net_udp_sendto(int fd, const void *buf, size_t len,
                               const char *host, int port);

/* Receive a single datagram.  out_host/out_host_size receive the
 * peer's address as a printable string.  out_port and out_n are
 * filled in.  Returns XTC_OK or XTC_E_AGAIN. */
XTC_API int xtc_net_udp_recvfrom(int fd, void *buf, size_t buflen,
                                 char *out_host, size_t out_host_size,
                                 int *out_port, size_t *out_n);

/* ---- DNS resolution ------------------------------------------
 *
 * xtc_dns_resolve resolves a hostname to a printable address
 * string.  The current implementation uses getaddrinfo(3) on a
 * blocking thread; the calling proc / fiber blocks during the
 * lookup.  For genuine async resolution -- and to avoid blocking
 * the loop -- run the call from a dedicated lookup proc whose
 * thread is allowed to block.  A future revision will integrate
 * a c-ares-style fully async resolver.
 */
XTC_API int xtc_dns_resolve(const char *hostname, int port,
                            xtc_net_family_t fam,
                            char *out_addr, size_t out_addr_size);

/* ---- length-framed transport --------------------------------
 *
 * A 4-byte big-endian length prefix + payload.  On a non-blocking fd
 * these yield the calling fiber (via the loop) on partial I/O instead
 * of blocking, so many connections share one loop; off a loop they
 * fall back to poll(2).  Set the fd non-blocking (xtc_net_setnonblock)
 * for the loop-friendly behaviour.
 *
 * xtc_net_recv_frame allocates the frame with the xtc allocator (free
 * it with xtc_free) and rejects a claimed length above max_len
 * (max_len 0 = no cap) with XTC_E_RANGE -- so a peer cannot force an
 * unbounded allocation.  Returns XTC_E_AGAIN on timeout, XTC_E_INVAL
 * if the peer closed mid-frame.  A zero-length frame returns XTC_OK
 * with *out == NULL.
 *
 * Neither copies the payload in user space: send_frame hands the
 * caller's buffer to the kernel together with the 4-byte prefix in one
 * sendmsg(2) (one TCP segment for a small frame), and recv_frame reads
 * straight into the frame it returns.
 */
int xtc_net_send_frame(int fd, const void *buf, size_t len);
int xtc_net_recv_frame(int fd, void **out, size_t *out_len,
                       size_t max_len, int64_t timeout_ns);

#if !defined(_WIN32)
#include <sys/uio.h>    /* struct iovec */
/*
 * PUBLIC: int xtc_net_send_framev __P((int, const struct iovec *, int));
 *
 * Scatter-gather send_frame: one frame whose payload is the
 * concatenation of iov[0..iovcnt), sent straight from those buffers
 * (no assembly copy into a contiguous message).  Use it for a header
 * plus a body that live apart -- a reply header on the stack and a
 * value in a cache, a record batch in several slabs.  The receiver sees
 * an ordinary frame (xtc_net_recv_frame).  Zero-length pieces are
 * skipped; iovcnt 0 sends an empty frame.  The caller's iovec array is
 * not modified.  iovcnt is at most 63 (XTC_E_INVAL above that); the
 * total at most 0xFFFFFFFF bytes.  Same waiting and return codes as
 * xtc_net_send_frame.  POSIX only.
 */
XTC_API int xtc_net_send_framev(int fd, const struct iovec *iov, int iovcnt);
#endif

#endif /* XTC_NET_H */
