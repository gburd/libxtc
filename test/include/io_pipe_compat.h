/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the PostgreSQL License.
 *
 * test/include/io_pipe_compat.h
 *	Cross-platform pipe-pair helper for the L1 I/O tests.
 *
 *	On POSIX:    pipe(2) — read end at fd[0], write end at fd[1].
 *	On Windows:  loopback TCP socketpair via WSA*.  Anonymous pipes
 *	             on Windows can't compose with IOCP (different
 *	             completion model than overlapped sockets); using
 *	             sockets keeps the readiness-based xtc_io contract
 *	             working in tests.  The "fd" returned is the SOCKET
 *	             cast to int — same convention the io_iocp backend
 *	             uses internally.
 */

#ifndef XTC_TEST_IO_PIPE_COMPAT_H
#define XTC_TEST_IO_PIPE_COMPAT_H

#if defined(_WIN32)
# include <winsock2.h>
# include <ws2tcpip.h>
# include <stdio.h>
# include <string.h>

static inline int
xtc_test_make_pipe(int *r, int *w)
{
	SOCKET listener, s1, s2;
	struct sockaddr_in addr;
	int addr_len;
	WSADATA wsa;
	static int wsa_inited = 0;

	if (!wsa_inited) {
		(void)WSAStartup(MAKEWORD(2, 2), &wsa);
		wsa_inited = 1;
	}

	listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (listener == INVALID_SOCKET) return -1;
	memset(&addr, 0, sizeof addr);
	addr.sin_family      = AF_INET;
	addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	addr.sin_port        = 0;
	if (bind(listener, (struct sockaddr *)&addr, sizeof addr) != 0) {
		closesocket(listener); return -1;
	}
	addr_len = sizeof addr;
	if (getsockname(listener, (struct sockaddr *)&addr, &addr_len) != 0) {
		closesocket(listener); return -1;
	}
	if (listen(listener, 1) != 0) { closesocket(listener); return -1; }

	s1 = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s1 == INVALID_SOCKET) { closesocket(listener); return -1; }
	if (connect(s1, (struct sockaddr *)&addr, sizeof addr) != 0) {
		closesocket(s1); closesocket(listener); return -1;
	}
	s2 = accept(listener, NULL, NULL);
	closesocket(listener);
	if (s2 == INVALID_SOCKET) { closesocket(s1); return -1; }

	*r = (int)s2;       /* read side */
	*w = (int)s1;       /* write side */
	return 0;
}

static inline void
xtc_test_close_pipe(int r, int w)
{
	if (r >= 0) closesocket((SOCKET)r);
	if (w >= 0) closesocket((SOCKET)w);
}

static inline int
xtc_test_pipe_write(int w, const void *buf, int len)
{
	return send((SOCKET)w, (const char *)buf, len, 0);
}

static inline int
xtc_test_pipe_read(int r, void *buf, int len)
{
	return recv((SOCKET)r, (char *)buf, len, 0);
}

#else /* !_WIN32 */
# include <unistd.h>

static inline int
xtc_test_make_pipe(int *r, int *w)
{
	int p[2];
	if (pipe(p) != 0) return -1;
	*r = p[0]; *w = p[1];
	return 0;
}

static inline void
xtc_test_close_pipe(int r, int w)
{
	if (r >= 0) close(r);
	if (w >= 0) close(w);
}

static inline int
xtc_test_pipe_write(int w, const void *buf, int len)
{
	return (int)write(w, buf, (size_t)len);
}

static inline int
xtc_test_pipe_read(int r, void *buf, int len)
{
	return (int)read(r, buf, (size_t)len);
}

#endif /* _WIN32 */

#endif /* XTC_TEST_IO_PIPE_COMPAT_H */
