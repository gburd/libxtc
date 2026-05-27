/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
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
 *	xtc_io_poll.  None of these calls block — caller-driven I/O
 *	is the contract.
 */

#ifndef XTC_NET_H
#define XTC_NET_H

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
	.nodelay = 1, .reuseaddr = 1, .reuseport = 0, .keepalive = 0, \
	.keepidle_s = 0, .keepintvl_s = 0, .keepcnt = 0, .user_timeout_ms = 0 \
}

/*
 * PUBLIC: int xtc_net_listen __P((xtc_net_family_t, const char *, int, const xtc_tcp_opts_t *, int *));
 * PUBLIC: int xtc_net_dial __P((xtc_net_family_t, const char *, int, const xtc_tcp_opts_t *, int *));
 * PUBLIC: int xtc_net_apply_tcp_opts __P((int, const xtc_tcp_opts_t *));
 * PUBLIC: int xtc_net_setnonblock __P((int));
 * PUBLIC: void xtc_net_close __P((int));
 *
 * PUBLIC: int xtc_net_unix_listen __P((const char *, int *));
 * PUBLIC: int xtc_net_unix_dial __P((const char *, int *));
 * PUBLIC: int xtc_net_unix_send_creds __P((int, const void *, size_t));
 * PUBLIC: int xtc_net_unix_recv_creds __P((int, void *, size_t, uint32_t *, uint32_t *, size_t *));
 */

/* TCP listen socket.  `host` may be NULL/"" for any-address.  Returns
 * the listening fd; caller registers it with xtc_io_reg_fd for accept
 * readiness.  Backlog defaults to 128 if 0. */
int xtc_net_listen(xtc_net_family_t fam, const char *host, int port,
                   const xtc_tcp_opts_t *opts, int *out_fd);

/* TCP connect to host:port.  Returns the fd in out_fd.  The connect
 * is non-blocking; caller polls for writability to detect completion. */
int xtc_net_dial(xtc_net_family_t fam, const char *host, int port,
                 const xtc_tcp_opts_t *opts, int *out_fd);

/* Apply TCP knobs to an already-open socket. */
int xtc_net_apply_tcp_opts(int fd, const xtc_tcp_opts_t *opts);

/* Set O_NONBLOCK + FD_CLOEXEC. */
int xtc_net_setnonblock(int fd);

/* Close (always succeeds). */
void xtc_net_close(int fd);

/* Unix domain sockets — SOCK_STREAM. */
int xtc_net_unix_listen(const char *path, int *out_fd);
int xtc_net_unix_dial  (const char *path, int *out_fd);

/* Send a message + the sender's credentials (uid/gid/pid) over a
 * UNIX socket via SCM_CREDENTIALS / LOCAL_PEERCRED.  The receiver
 * extracts the credentials with xtc_net_unix_recv_creds. */
int xtc_net_unix_send_creds(int fd, const void *buf, size_t buflen);
int xtc_net_unix_recv_creds(int fd, void *buf, size_t buflen,
                            uint32_t *out_uid, uint32_t *out_gid,
                            size_t *out_n);

#endif /* XTC_NET_H */
