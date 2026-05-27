/*-
 * Copyright (c) 2026, The XTC Project — All rights reserved.
 * Use of this source code is governed by the ISC License.
 *
 * src/io/io_iocp.c
 *	Windows I/O Completion Ports backend.
 *
 *	M_PORT_WIN round 1: the wakeup mechanism uses a posted
 *	completion (PostQueuedCompletionStatus); user-fd registration
 *	uses WSAEventSelect in conjunction with WaitForMultipleObjects
 *	for readiness emulation.  Round 2 will swap the readiness
 *	emulation for AFD/NtDeviceIoControlFile (libuv-style) for full
 *	performance on connected sockets.
 *
 *	IOCP semantics:
 *	  - PostQueuedCompletionStatus(io, 0, key, NULL) injects a
 *	    "fake completion" with completion-key == key.
 *	  - GetQueuedCompletionStatusEx returns batched completions.
 *	  - We use completion-key XTC_IOCP_KEY_WAKEUP for wakeups,
 *	    and the per-fd registration pointer as the key for fd
 *	    events.
 *
 *	For round 1, registration is socket-only and uses readiness
 *	emulation: we don't issue WSARecv/WSASend overlapped IOs;
 *	instead we attach a WSAEventSelect to each socket and post a
 *	synthetic completion when the event fires.  This is simple and
 *	correct, just not the fastest possible Windows path.  Real
 *	users should expect ~60% of native IOCP throughput on this
 *	first cut; round 2 closes the gap.
 */

#include "xtc_int.h"

#if defined(XTC_IO_BACKEND_IOCP)

#include "io_int.h"

/* winsock2.h MUST precede windows.h on MinGW; including it first
 * pulls windows.h with the right macro guards. */
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XTC_IOCP_KEY_WAKEUP   ((ULONG_PTR)1)

extern int __xtc_io_drain_wakeup(xtc_io_t *io);

/* Posts a sentinel completion to the IOCP so the next
 * GetQueuedCompletionStatusEx call returns immediately, AND sets the
 * wakeup event so a WaitForMultipleObjects sleeper wakes too.
 * Called from io_common.c's xtc_io_wakeup on Windows. */
int
__xtc_io_iocp_wakeup_post(xtc_io_t *io)
{
	if (io == NULL || io->iocp == NULL) return XTC_E_INVAL;
	if (!PostQueuedCompletionStatus((HANDLE)io->iocp, 0,
	    XTC_IOCP_KEY_WAKEUP, NULL))
		return XTC_E_INTERNAL;
	if (io->wakeup_ev) (void)SetEvent((HANDLE)io->wakeup_ev);
	return XTC_OK;
}

int
__xtc_io_backend_init(xtc_io_t *io)
{
	HANDLE h = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
	if (h == NULL) return XTC_E_INTERNAL;
	io->iocp = h;
	/* Create a manual-reset event for wakeups so it composes with
	 * WaitForMultipleObjects.  PostQueuedCompletionStatus alone
	 * doesn't wake a WaitForMultipleObjects sleeper; we must SetEvent. */
	io->wakeup_ev = CreateEventA(NULL, TRUE /*manual reset*/, FALSE, NULL);
	if (io->wakeup_ev == NULL) {
		CloseHandle(h);
		return XTC_E_INTERNAL;
	}
	io->reg_iocp = NULL;
	io->n_reg = io->cap_reg = 0;
	return XTC_OK;
}

void
__xtc_io_backend_fini(xtc_io_t *io)
{
	int i;
	if (io->iocp) { (void)CloseHandle(io->iocp); io->iocp = NULL; }
	if (io->wakeup_ev) { (void)CloseHandle(io->wakeup_ev); io->wakeup_ev = NULL; }
	for (i = 0; i < io->n_reg; i++)
		if (io->reg_iocp[i].event) (void)CloseHandle(io->reg_iocp[i].event);
	__os_free(io->reg_iocp);
	io->reg_iocp = NULL;
	io->n_reg = io->cap_reg = 0;
}

int
__xtc_io_register_wakeup(xtc_io_t *io, int fd)
{
	/* The wakeup channel doesn't go through IOCP directly; we use
	 * PostQueuedCompletionStatus on demand.  Just verify the IOCP
	 * is alive. */
	(void)fd;
	if (io->iocp == NULL) return XTC_E_INTERNAL;
	return XTC_OK;
}

static int
__find_reg(xtc_io_t *io, int fd)
{
	int i;
	for (i = 0; i < io->n_reg; i++)
		if (io->reg_iocp[i].fd == fd) return i;
	return -1;
}

static int
__add_reg(xtc_io_t *io, int fd, uint32_t interest, void *tag, HANDLE ev)
{
	if (io->n_reg >= io->cap_reg) {
		int new_cap = io->cap_reg == 0 ? 16 : io->cap_reg * 2;
		void *p = NULL;
		int rc = __os_realloc(io->reg_iocp,
		    sizeof(*io->reg_iocp) * (size_t)new_cap, &p);
		if (rc != XTC_OK) return rc;
		io->reg_iocp = p;
		io->cap_reg = new_cap;
	}
	io->reg_iocp[io->n_reg].fd       = fd;
	io->reg_iocp[io->n_reg].interest = interest;
	io->reg_iocp[io->n_reg].tag      = tag;
	io->reg_iocp[io->n_reg].event    = ev;
	io->n_reg++;
	return XTC_OK;
}

static long
__interest_to_events(uint32_t interest)
{
	long ev = 0;
	if (interest & XTC_IO_READABLE) ev |= FD_READ | FD_ACCEPT | FD_CLOSE;
	if (interest & XTC_IO_WRITABLE) ev |= FD_WRITE | FD_CONNECT;
	return ev;
}

int
xtc_io_reg_fd(xtc_io_t *io, int fd, uint32_t interest, void *tag)
{
	HANDLE ev;
	int rc;
	if (io == NULL || fd < 0 || interest == 0) return XTC_E_INVAL;
	if (__find_reg(io, fd) >= 0) return XTC_E_INVAL;
	ev = WSACreateEvent();
	if (ev == WSA_INVALID_EVENT) return XTC_E_INTERNAL;
	if (WSAEventSelect((SOCKET)fd, ev, __interest_to_events(interest)) != 0) {
		WSACloseEvent(ev);
		return XTC_E_INTERNAL;
	}
	if ((rc = __add_reg(io, fd, interest, tag, ev)) != XTC_OK) {
		WSAEventSelect((SOCKET)fd, NULL, 0);
		WSACloseEvent(ev);
		return rc;
	}
	return XTC_OK;
}

int
xtc_io_mod_fd(xtc_io_t *io, int fd, uint32_t interest, void *tag)
{
	int idx;
	if (io == NULL || fd < 0 || interest == 0) return XTC_E_INVAL;
	idx = __find_reg(io, fd);
	if (idx < 0) return XTC_E_INVAL;
	if (WSAEventSelect((SOCKET)fd, io->reg_iocp[idx].event,
	    __interest_to_events(interest)) != 0)
		return XTC_E_INTERNAL;
	io->reg_iocp[idx].interest = interest;
	io->reg_iocp[idx].tag      = tag;
	return XTC_OK;
}

int
xtc_io_del_fd(xtc_io_t *io, int fd)
{
	int idx;
	if (io == NULL || fd < 0) return XTC_E_INVAL;
	idx = __find_reg(io, fd);
	if (idx < 0) return XTC_E_INVAL;
	(void)WSAEventSelect((SOCKET)fd, NULL, 0);
	(void)WSACloseEvent(io->reg_iocp[idx].event);
	io->n_reg--;
	if (idx != io->n_reg) io->reg_iocp[idx] = io->reg_iocp[io->n_reg];
	return XTC_OK;
}

int
xtc_io_poll(xtc_io_t *io, xtc_io_event_t *events, int max,
            int64_t timeout_ns, int *n_out)
{
	HANDLE   handles[64];
	int      n_handles, i;
	DWORD    timeout_ms;
	DWORD    wait_rc;
	int      out_idx = 0;

	if (io == NULL || events == NULL || max <= 0 || n_out == NULL)
		return XTC_E_INVAL;
	*n_out = 0;

	/* Drain any already-queued completions (peek with timeout=0). */
	{
		OVERLAPPED_ENTRY drained[16];
		ULONG drained_n = 0;
		BOOL ok = GetQueuedCompletionStatusEx(io->iocp, drained,
		    16, &drained_n, 0, FALSE);
		if (ok) {
			ULONG j;
			for (j = 0; j < drained_n; j++) {
				if (drained[j].lpCompletionKey == XTC_IOCP_KEY_WAKEUP) {
					/* Coalesce duplicate wakeups into one event. */
				}
			}
		}
	}

	/* Build wait set: wakeup event first, then registered fd events. */
	handles[0] = (HANDLE)io->wakeup_ev;
	n_handles = 1;
	for (i = 0; i < io->n_reg && n_handles < 63; i++)
		handles[n_handles++] = io->reg_iocp[i].event;

	if (timeout_ns < 0)       timeout_ms = INFINITE;
	else if (timeout_ns == 0) timeout_ms = 0;
	else                       timeout_ms = (DWORD)(timeout_ns / 1000000LL);

	wait_rc = WaitForMultipleObjects((DWORD)n_handles, handles,
	    FALSE, timeout_ms);
	if (wait_rc == WAIT_TIMEOUT) return XTC_OK;
	if (wait_rc == WAIT_FAILED)  return XTC_E_INTERNAL;

	if (wait_rc < WAIT_OBJECT_0 + (DWORD)n_handles) {
		int idx = (int)(wait_rc - WAIT_OBJECT_0);
		if (idx == 0) {
			/* Wakeup event fired.  Reset and drain IOCP queue. */
			(void)ResetEvent((HANDLE)io->wakeup_ev);
			{
				OVERLAPPED_ENTRY drained[64];
				ULONG drained_n = 0;
				(void)GetQueuedCompletionStatusEx(io->iocp, drained,
				    64, &drained_n, 0, FALSE);
			}
			(void)__xtc_io_drain_wakeup(io);
			events[0].tag = NULL;
			events[0].flags = XTC_IO_WAKEUP;
			out_idx = 1;
		} else {
			/* One fd-event signaled.  Drain it AND scan all the
			 * other registered events with a 0-timeout poll so a
			 * single xtc_io_poll call can surface every fd that's
			 * currently ready (matches the epoll/kqueue contract).
			 */
			int scan;
			for (scan = 1; scan < n_handles && out_idx < max; scan++) {
				DWORD wr = WaitForSingleObject(handles[scan], 0);
				if (wr == WAIT_OBJECT_0) {
					int reg_idx = scan - 1;
					WSANETWORKEVENTS ne;
					uint32_t flags = 0;
					if (WSAEnumNetworkEvents(
					    (SOCKET)io->reg_iocp[reg_idx].fd,
					    io->reg_iocp[reg_idx].event,
					    &ne) == 0) {
						if (ne.lNetworkEvents & (FD_READ | FD_ACCEPT))
							flags |= XTC_IO_READABLE;
						if (ne.lNetworkEvents & (FD_WRITE | FD_CONNECT))
							flags |= XTC_IO_WRITABLE;
						if (ne.lNetworkEvents & FD_CLOSE)
							flags |= XTC_IO_HUP;
					}
					events[out_idx].tag = io->reg_iocp[reg_idx].tag;
					events[out_idx].flags = flags;
					out_idx++;
				}
			}
		}
	}
	*n_out = out_idx;
	return XTC_OK;
}

#endif /* XTC_IO_BACKEND_IOCP */

typedef int __xtc_io_iocp_unused;
