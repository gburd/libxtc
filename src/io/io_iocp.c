/*-
 * Copyright (c) 2026, The XTC Project
 * Use of this source code is governed by the ISC License.
 *
 * src/io/io_iocp.c
 *	Windows I/O Completion Ports backend (round 2: native overlapped).
 *
 *	Round 1 emulated readiness with WSAEventSelect +
 *	WaitForMultipleObjects, hard-capped at 64 handles.  This round
 *	replaces that entirely with a real completion port and the AFD
 *	poll fast path the Windows port docs call for:
 *
 *	  - CreateIoCompletionPort is the one wait primitive.
 *	    GetQueuedCompletionStatusEx dequeues completions in batch.
 *	    There is no 64-handle cap: any number of sockets and file
 *	    AIOs associate with the single port.
 *	  - Socket readiness comes from the Ancillary Function Driver:
 *	    we open \Device\Afd, associate it with the port, and arm one
 *	    IOCTL_AFD_POLL request per registered socket via
 *	    NtDeviceIoControlFile.  When the socket becomes readable,
 *	    writable, hung up, or errored, the AFD poll completes through
 *	    the port and we re-arm it (level-triggered emulation matching
 *	    the epoll/kqueue contract).  This is the wepoll/libuv design.
 *
 *	    KNOWN AFD BUG (workaround, not a design choice -- see
 *	    docs/KNOWN_ISSUES.md "Windows AFD async data-ready completion"
 *	    for the full diagnosis): a poll armed while the socket is NOT
 *	    yet ready returns STATUS_PENDING, and on this driver/version
 *	    (confirmed via a standalone repro with no libxtc code, Windows
 *	    Server 2022) that pending poll never completes when the socket
 *	    LATER becomes ready -- AFD only ever answers with the CURRENT
 *	    state at arm time, synchronously; it does not track a future
 *	    edge through our handle.  Cancelling a stuck pending poll also
 *	    does not post a port completion (NtCancelIoFileEx finalizes the
 *	    IOSB, STATUS_CANCELLED, synchronously in-process, but nothing
 *	    reaches GetQueuedCompletionStatusEx -- the same "synchronous
 *	    completions are not queued" pattern already fixed for the
 *	    ready-at-arm-time case).  The workaround is
 *	    __xtc_iocp_repoll_sweep: xtc_io_poll re-checks every PENDING
 *	    registration older than XTC_IOCP_REPOLL_NS with a BATCHED,
 *	    zero-timeout, throwaway AFD poll covering up to
 *	    XTC_IOCP_REPOLL_BATCH overdue sockets in ONE syscall (confirmed
 *	    by direct experiment: a second independent poll on a socket
 *	    that already has its own long-pending poll outstanding does not
 *	    disturb it), and only cancels + re-arms the individual
 *	    registrations the probe actually flags ready.  This bounds
 *	    readiness latency to XTC_IOCP_REPOLL_NS instead of hanging
 *	    forever, at O(overdue / XTC_IOCP_REPOLL_BATCH) syscalls per
 *	    sweep rather than O(overdue) -- a per-socket cancel+rearm design
 *	    was tried first and measured 50-98% CPU with 1000-5000 idle
 *	    pending sockets; the batched probe measures near-zero CPU at
 *	    the same scale.  A normal completion -- accept, connect,
 *	    wakeup, AIO -- still short-circuits the workaround immediately.
 *	  - The cross-thread wakeup is a PostQueuedCompletionStatus with
 *	    completion-key XTC_IOCP_KEY_WAKEUP -- no synthetic event, no
 *	    self-pipe.
 *	  - File AIO (pread/pwrite) is a native overlapped
 *	    ReadFile/WriteFile whose file HANDLE is associated with the
 *	    port; its completion is dequeued like any other.  fsync has
 *	    no async form (FlushFileBuffers is synchronous), so
 *	    xtc_io_aio_submit returns XTC_E_NOSYS for it and xtc_aio
 *	    offloads to the blocking pool.
 *
 *	OVERLAPPED OWNERSHIP RULE (the classic IOCP correctness bug is
 *	freeing an OVERLAPPED the kernel still owns; we never do):
 *	  An OVERLAPPED -- the AFD poll's, or a file AIO's -- belongs to
 *	  the kernel from the instant the request is accepted
 *	  (NtDeviceIoControlFile / ReadFile returns STATUS_PENDING or
 *	  ERROR_IO_PENDING) until its completion is dequeued from the
 *	  port by GetQueuedCompletionStatusEx.  While in flight the
 *	  buffer is never freed or reused.  Deregistering a socket whose
 *	  poll is in flight issues NtCancelIoFileEx and moves the
 *	  registration to a "dead" list; its storage is released only
 *	  when the (now canceled) completion is finally reaped.  Each
 *	  OVERLAPPED is the first member of its owning node, so a
 *	  completion's OVERLAPPED_ENTRY.lpOverlapped casts straight back
 *	  to the node.
 *
 *	STATUS: COMPILED, NOT RUNTIME-VERIFIED.  This file cross-compiles
 *	clean with mingw-w64 gcc -Wall -Wextra and links against ntdll +
 *	ws2_32.  It has NOT run on a Windows host: the AFD poll path, the
 *	cancel/re-arm lifetime, and the wakeup ordering must be validated
 *	on santorini (or the windows CI job) before this is called
 *	production quality.  See docs/M_WINDOWS_MATRIX.md and
 *	docs/KNOWN_ISSUES.md for the exact test plan.  This mirrors the
 *	reviewed-but-untested status of src/io/io_aix.c.
 */

#include "xtc_int.h"

#if defined(XTC_IO_BACKEND_IOCP)

#include "io_int.h"

/* winsock2.h MUST precede windows.h on MinGW.  WIN32_NO_STATUS keeps
 * windows.h from defining the STATUS_ macros, then winternl.h supplies
 * the NT types.  We do NOT pull <ntstatus.h>: it redefines dozens of
 * DBG_ and STATUS_ macros that winnt.h / winternl.h already define (65
 * C4005 macro-redefinition warnings under MSVC), and this backend uses
 * only the four NTSTATUS values below -- defined here explicitly. */
#define WIN32_NO_STATUS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>
#include <winioctl.h>     /* CTL_CODE, METHOD_BUFFERED, FILE_ANY_ACCESS (AFD poll IOCTL) */
#include <winternl.h>
#undef WIN32_NO_STATUS
#ifndef STATUS_SUCCESS
#define STATUS_SUCCESS               ((NTSTATUS)0x00000000L)
#endif
#ifndef STATUS_PENDING
#define STATUS_PENDING               ((NTSTATUS)0x00000103L)
#endif
#ifndef STATUS_INVALID_DEVICE_REQUEST
#define STATUS_INVALID_DEVICE_REQUEST ((NTSTATUS)0xC0000010L)
#endif
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XTC_IOCP_KEY_WAKEUP   ((ULONG_PTR)1)
#define XTC_IOCP_KEY_IO       ((ULONG_PTR)0)   /* socket poll + file AIO */

/* Bounded re-poll interval for the AFD async-completion workaround
 * above: how long a PENDING socket poll is trusted before xtc_io_poll
 * force-refreshes it by cancel + re-arm.  8 ms keeps p99 wakeup
 * latency for a newly-readable socket close to what a real completion
 * would have given, without turning idle sockets into a busy loop (the
 * sweep is O(registrations overdue), and a socket with no traffic is
 * only ever refreshed, never spun on).
 *
 * MEASURED CAVEAT: GetQueuedCompletionStatusEx's millisecond timeout
 * is quantized to the process's current timer resolution, which
 * defaults to ~15.6 ms (the classic Windows 64 Hz system tick) unless
 * something has raised it.  On a default Windows Server 2022 host
 * this constant's actual observed floor was ~15.6 ms, not 8 ms;
 * calling timeBeginPeriod(1) (any process on the system doing so is
 * enough) measured ~3-5 ms instead, tracking this constant closely.
 * libxtc does NOT call timeBeginPeriod itself -- that raises the
 * SYSTEM's timer resolution for every process for as long as this one
 * runs, a global side effect this project does not impose on an
 * embedder's behalf (the same reasoning as not pinning threads to
 * cores).  An application that wants tighter Windows socket-readiness
 * latency than the default tick can call timeBeginPeriod(1) itself. */
#define XTC_IOCP_REPOLL_NS   (8LL * 1000 * 1000)

extern int __xtc_io_drain_wakeup(xtc_io_t *io);

/*
 * The AFD poll interface.  These structures and the IOCTL are not in
 * the public SDK; they are the stable contract wepoll/libuv rely on
 * and are declared here against the documented \Device\Afd ABI.
 */
#define XTC_AFD_POLL_RECEIVE           0x0001
#define XTC_AFD_POLL_RECEIVE_EXPEDITED 0x0002
#define XTC_AFD_POLL_SEND              0x0004
#define XTC_AFD_POLL_DISCONNECT        0x0008  /* graceful peer shutdown */
#define XTC_AFD_POLL_ABORT             0x0010  /* aborted/reset */
#define XTC_AFD_POLL_LOCAL_CLOSE       0x0020
#define XTC_AFD_POLL_ACCEPT            0x0080  /* incoming connection */
#define XTC_AFD_POLL_CONNECT_FAIL      0x0100

/* IOCTL_AFD_POLL: the value wepoll/libuv ship literally (0x00012024).
 * NOTE: this is NOT CTL_CODE(0x12,9,...) = 0x00120024 -- the AFD poll
 * IOCTL uses device type 0x12 in the LOW nibble of the high word the
 * way the AFD driver actually decodes it; the literal is what the
 * kernel accepts (a CTL_CODE(0x12,9,BUFFERED,ANY) is rejected with
 * STATUS_INVALID_DEVICE_REQUEST).  Match the proven wepoll constant. */
#define XTC_IOCTL_AFD_POLL  0x00012024

typedef struct _XTC_AFD_POLL_HANDLE_INFO {
	HANDLE   Handle;
	ULONG    Events;
	NTSTATUS Status;
} XTC_AFD_POLL_HANDLE_INFO;

typedef struct _XTC_AFD_POLL_INFO {
	LARGE_INTEGER            Timeout;
	ULONG                    NumberOfHandles;
	ULONG                    Exclusive;
	XTC_AFD_POLL_HANDLE_INFO Handles[1];
} XTC_AFD_POLL_INFO;

/*
 * The heap node every armed socket poll owns.  OVERLAPPED is first so
 * a completion's lpOverlapped casts back to it; back points at the
 * registration that owns this node.  poll_in is what we submit;
 * poll_out is where AFD writes the result -- they are distinct so the
 * request buffer is never clobbered by the reply while still queued.
 */
struct __xtc_iocp_overlapped {
	OVERLAPPED            ov;       /* MUST be first */
	struct __xtc_iocp_reg *back;
	XTC_AFD_POLL_INFO     poll_in;
	XTC_AFD_POLL_INFO     poll_out;
};

extern NTSTATUS NTAPI NtCreateFile(PHANDLE, ACCESS_MASK, POBJECT_ATTRIBUTES,
    PIO_STATUS_BLOCK, PLARGE_INTEGER, ULONG, ULONG, ULONG, ULONG, PVOID, ULONG);
extern NTSTATUS NTAPI NtDeviceIoControlFile(HANDLE, HANDLE, PIO_APC_ROUTINE,
    PVOID, PIO_STATUS_BLOCK, ULONG, PVOID, ULONG, PVOID, ULONG);
extern NTSTATUS NTAPI NtCancelIoFileEx(HANDLE, PIO_STATUS_BLOCK,
    PIO_STATUS_BLOCK);
extern ULONG NTAPI RtlNtStatusToDosError(NTSTATUS);

/* The AFD device path as a counted UNICODE_STRING. */
static const WCHAR XTC_AFD_DEVICE[] = L"\\Device\\Afd\\Xtc";

/*
 * For an OVERLAPPED-driven NtDeviceIoControlFile the kernel treats the
 * OVERLAPPED's leading Internal/InternalHigh fields as an
 * IO_STATUS_BLOCK and posts a completion whose lpOverlapped equals the
 * IO_STATUS_BLOCK pointer we pass.  So the IO_STATUS_BLOCK pointer is
 * the OVERLAPPED address itself; the cancel request-to-cancel pointer
 * is the same. */
#define XTC_OV_IOSB(o)  ((PIO_STATUS_BLOCK)&(o)->ov)

/* --------------------------------------------------------------- */
/* wakeup                                                          */
/* --------------------------------------------------------------- */

/* Posts a sentinel completion to the IOCP so the next
 * GetQueuedCompletionStatusEx returns immediately.  Called from
 * io_common.c's xtc_io_wakeup on Windows.
 *
 * Coalesced at POST time: at most ONE wakeup completion is ever queued,
 * regardless of how many concurrent xtc_io_wakeup calls happen, so a
 * burst of N wakeups cannot leave N - drained completions sitting in
 * the port for later polls to re-report (the epoll self-pipe and the
 * kqueue EVFILT_USER coalesce the same way).  The poll side clears
 * wakeup_pending when it drains the wakeup event. */
int
__xtc_io_iocp_wakeup_post(xtc_io_t *io)
{
	int expected = 0;
	if (io == NULL || io->iocp == NULL)
		return XTC_E_INVAL;
	/* Only the thread that flips 0 -> 1 posts the (single) completion. */
	if (!atomic_compare_exchange_strong(&io->wakeup_pending, &expected, 1))
		return XTC_OK;            /* a wakeup is already queued; coalesce */
	if (!PostQueuedCompletionStatus((HANDLE)io->iocp, 0,
	    XTC_IOCP_KEY_WAKEUP, NULL)) {
		atomic_store(&io->wakeup_pending, 0);   /* post failed; allow retry */
		return XTC_E_INTERNAL;
	}
	return XTC_OK;
}

/* --------------------------------------------------------------- */
/* lifecycle                                                       */
/* --------------------------------------------------------------- */

/* Open \Device\Afd and associate it with the completion port so AFD
 * poll completions land on the same queue as everything else. */
static int
__open_afd(xtc_io_t *io)
{
	HANDLE            afd = INVALID_HANDLE_VALUE;
	UNICODE_STRING    name;
	OBJECT_ATTRIBUTES oa;
	IO_STATUS_BLOCK   iosb;
	NTSTATUS          st;

	name.Buffer = (WCHAR *)XTC_AFD_DEVICE;
	name.Length = (USHORT)((sizeof XTC_AFD_DEVICE) - sizeof(WCHAR));
	name.MaximumLength = (USHORT)(sizeof XTC_AFD_DEVICE);
	InitializeObjectAttributes(&oa, &name, 0, NULL, NULL);
	memset(&iosb, 0, sizeof iosb);

	st = NtCreateFile(&afd, SYNCHRONIZE, &oa, &iosb, NULL, 0,
	    FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_OPEN, 0, NULL, 0);
	if (st != STATUS_SUCCESS)
		return XTC_E_INTERNAL;
	if (CreateIoCompletionPort(afd, (HANDLE)io->iocp,
	    XTC_IOCP_KEY_IO, 0) == NULL) {
		(void)CloseHandle(afd);
		return XTC_E_INTERNAL;
	}
	/* Route ALL AFD completions (synchronous and async) to the port.
	 * Do NOT set FILE_SKIP_SET_EVENT_ON_HANDLE here: on this AFD handle
	 * it suppressed the async (STATUS_PENDING) poll completion from
	 * being queued when the socket later became ready, so a data-ready
	 * notification never arrived (the loopback echo hung).  We reap
	 * every completion from the port uniformly. */
	io->afd = afd;
	return XTC_OK;
}

int
__xtc_io_backend_init(xtc_io_t *io)
{
	HANDLE h;
	int rc;

	h = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
	if (h == NULL)
		return XTC_E_INTERNAL;
	io->iocp = h;
	io->afd = INVALID_HANDLE_VALUE;
	io->reg_iocp = NULL;
	io->n_reg = io->cap_reg = 0;
	io->dead_iocp = NULL;
	io->n_dead = io->cap_dead = 0;
	io->aio_pend = NULL;
	io->n_aio = io->cap_aio = 0;

	if ((rc = __open_afd(io)) != XTC_OK) {
		(void)CloseHandle(h);
		io->iocp = NULL;
		return rc;
	}
	return XTC_OK;
}

void
__xtc_io_backend_fini(xtc_io_t *io)
{
	int i;

	/*
	 * Cancel every armed poll, then drain the port so the kernel
	 * releases its hold on each OVERLAPPED before we free it -- the
	 * ownership rule applies just as hard at teardown.
	 */
	if (io->afd != INVALID_HANDLE_VALUE) {
		for (i = 0; i < io->n_reg; i++) {
			if (io->reg_iocp[i]->pending) {
				struct __xtc_iocp_overlapped *o =
				    (struct __xtc_iocp_overlapped *)
				        io->reg_iocp[i]->ovp;
				IO_STATUS_BLOCK c;
				memset(&c, 0, sizeof c);
				(void)NtCancelIoFileEx(io->afd,
				    XTC_OV_IOSB(o), &c);
			}
		}
		for (i = 0; i < io->n_dead; i++) {
			struct __xtc_iocp_overlapped *o =
			    (struct __xtc_iocp_overlapped *)
			        io->dead_iocp[i]->ovp;
			IO_STATUS_BLOCK c;
			memset(&c, 0, sizeof c);
			(void)NtCancelIoFileEx(io->afd, XTC_OV_IOSB(o), &c);
		}
	}
	/*
	 * Drain any still-queued completions so no kernel reference is
	 * outstanding, THEN close the AFD and port handles.  Closing the
	 * AFD handle aborts and completes any polls still pending after
	 * the drain, and closing both handles drops every kernel
	 * reference, so the OVERLAPPED buffers are ours to free below.
	 */
	if (io->iocp != NULL) {
		OVERLAPPED_ENTRY drained[64];
		ULONG n = 0;
		while (GetQueuedCompletionStatusEx((HANDLE)io->iocp, drained,
		    64, &n, 0, FALSE) && n > 0)
			; /* spin until the queue is empty */
	}
	if (io->afd != INVALID_HANDLE_VALUE) {
		(void)CloseHandle(io->afd);
		io->afd = INVALID_HANDLE_VALUE;
	}
	if (io->iocp != NULL) {
		(void)CloseHandle((HANDLE)io->iocp);
		io->iocp = NULL;
	}
	for (i = 0; i < io->n_reg; i++) {
		__os_free(io->reg_iocp[i]->ovp);
		__os_free(io->reg_iocp[i]);
	}
	__os_free(io->reg_iocp);
	io->reg_iocp = NULL;
	io->n_reg = io->cap_reg = 0;
	for (i = 0; i < io->n_dead; i++) {
		__os_free(io->dead_iocp[i]->ovp);
		__os_free(io->dead_iocp[i]);
	}
	__os_free(io->dead_iocp);
	io->dead_iocp = NULL;
	io->n_dead = io->cap_dead = 0;
	for (i = 0; i < io->n_aio; i++)
		__os_free(io->aio_pend[i].ov);
	__os_free(io->aio_pend);
	io->aio_pend = NULL;
	io->n_aio = io->cap_aio = 0;
}

int
__xtc_io_register_wakeup(xtc_io_t *io, int fd)
{
	/* The wakeup channel is PostQueuedCompletionStatus, not an fd.
	 * Just confirm the port is alive. */
	(void)fd;
	if (io->iocp == NULL)
		return XTC_E_INTERNAL;
	return XTC_OK;
}

/* --------------------------------------------------------------- */
/* registration table                                             */
/* --------------------------------------------------------------- */

static int
__find_reg(xtc_io_t *io, int fd)
{
	int i;
	for (i = 0; i < io->n_reg; i++)
		if (io->reg_iocp[i]->fd == fd)
			return i;
	return -1;
}

/* Resolve the base (non-LSP) socket handle AFD must poll on. */
static int
__base_socket(int fd, HANDLE *out)
{
	SOCKET base = INVALID_SOCKET;
	DWORD  got = 0;
	/* The handle to hand IOCTL_AFD_POLL is the socket's BASE service
	 * provider handle.  On a plain socket SIO_BASE_HANDLE gives it, but
	 * on an ACCEPTED or layered socket (LSP/WFP present) the poll must
	 * target SIO_BSP_HANDLE_POLL -- SIO_BASE_HANDLE can return a handle
	 * whose AFD poll never completes on data arrival (observed: the
	 * loopback echo's accepted socket armed PENDING and was never woken).
	 * wepoll's resolution order: BSP_HANDLE_POLL, then BASE_HANDLE, then
	 * the fd itself. */
#ifndef SIO_BSP_HANDLE_POLL
#define SIO_BSP_HANDLE_POLL 0x4800001DUL   /* _WSAIORW(IOC_WS2, 29) */
#endif
	if (WSAIoctl((SOCKET)fd, SIO_BSP_HANDLE_POLL, NULL, 0, &base,
	    sizeof base, &got, NULL, NULL) == 0 && base != INVALID_SOCKET) {
		*out = (HANDLE)base;
		return XTC_OK;
	}
	base = INVALID_SOCKET; got = 0;
	if (WSAIoctl((SOCKET)fd, SIO_BASE_HANDLE, NULL, 0, &base,
	    sizeof base, &got, NULL, NULL) == 0 && base != INVALID_SOCKET) {
		*out = (HANDLE)base;
		return XTC_OK;
	}
	/* Last resort: poll the fd directly (non-layered stacks). */
	*out = (HANDLE)(SOCKET)fd;
	return XTC_OK;
}

static ULONG
__interest_to_afd(uint32_t interest)
{
	ULONG e = 0;
	/* AFD_POLL_LOCAL_CLOSE, ABORT and CONNECT_FAIL always report so
	 * the loop sees a hung-up/errored peer even when the caller only
	 * asked for one direction. */
	e |= XTC_AFD_POLL_LOCAL_CLOSE | XTC_AFD_POLL_ABORT |
	     XTC_AFD_POLL_CONNECT_FAIL;
	if (interest & XTC_IO_READABLE)
		e |= XTC_AFD_POLL_RECEIVE | XTC_AFD_POLL_RECEIVE_EXPEDITED |
		     XTC_AFD_POLL_ACCEPT | XTC_AFD_POLL_DISCONNECT;
	if (interest & XTC_IO_WRITABLE)
		e |= XTC_AFD_POLL_SEND;
	return e;
}

static uint32_t
__afd_to_flags(ULONG afd_events)
{
	uint32_t f = 0;
	if (afd_events & (XTC_AFD_POLL_RECEIVE |
	    XTC_AFD_POLL_RECEIVE_EXPEDITED | XTC_AFD_POLL_ACCEPT))
		f |= XTC_IO_READABLE;
	if (afd_events & XTC_AFD_POLL_SEND)
		f |= XTC_IO_WRITABLE;
	if (afd_events & XTC_AFD_POLL_DISCONNECT)
		f |= XTC_IO_HUP;
	if (afd_events & (XTC_AFD_POLL_ABORT | XTC_AFD_POLL_LOCAL_CLOSE))
		f |= XTC_IO_HUP;
	if (afd_events & XTC_AFD_POLL_CONNECT_FAIL)
		f |= XTC_IO_ERR;
	return f;
}

/*
 * Arm (or re-arm) the AFD poll for one registration.  Submits an
 * IOCTL_AFD_POLL whose OVERLAPPED is reaped from the port.  After this
 * returns XTC_OK the kernel owns reg->ovp until the completion is
 * dequeued (the ownership rule).
 */
static int
__arm_poll(xtc_io_t *io, struct __xtc_iocp_reg *reg)
{
	struct __xtc_iocp_overlapped *o =
	    (struct __xtc_iocp_overlapped *)reg->ovp;
	NTSTATUS st;

	memset(&o->ov, 0, sizeof o->ov);
	o->back = reg;
	o->poll_in.Timeout.QuadPart = INT64_MAX;  /* no AFD-side timeout */
	o->poll_in.NumberOfHandles = 1;
	o->poll_in.Exclusive = FALSE;
	o->poll_in.Handles[0].Handle = (HANDLE)reg->base;
	o->poll_in.Handles[0].Status = 0;
	o->poll_in.Handles[0].Events = __interest_to_afd(reg->interest);
	memset(&o->poll_out, 0, sizeof o->poll_out);

	st = NtDeviceIoControlFile(io->afd, NULL, NULL, NULL, XTC_OV_IOSB(o),
	    XTC_IOCTL_AFD_POLL, &o->poll_in, sizeof o->poll_in,
	    &o->poll_out, sizeof o->poll_out);
	if (st == STATUS_SUCCESS) {
		/* Synchronous completion: the fd was already ready and AFD
		 * filled poll_out now, but does NOT queue a port completion for
		 * the synchronous case (observed on Windows Server 2022).  Post
		 * one ourselves against this reg's OVERLAPPED so the reap loop
		 * processes it uniformly.  reg->pending gates the reap so a
		 * (theoretical) second completion for the same OVERLAPPED is a
		 * no-op. */
		reg->pending = 1;
		(void)__os_clock_mono(&reg->armed_at_ns);
		(void)PostQueuedCompletionStatus((HANDLE)io->iocp, 0,
		    XTC_IOCP_KEY_IO, &o->ov);
		return XTC_OK;
	}
	if (st == STATUS_PENDING) {
		reg->pending = 1;
		(void)__os_clock_mono(&reg->armed_at_ns);
		return XTC_OK;
	}
	reg->pending = 0;
	return XTC_E_INTERNAL;
}

/*
 * Workaround for the AFD async-completion bug documented at the top of
 * this file: a poll that has been PENDING for at least
 * XTC_IOCP_REPOLL_NS is force-refreshed by a THROWAWAY, ZERO-TIMEOUT,
 * BATCHED probe (one NtDeviceIoControlFile call covering up to
 * XTC_IOCP_REPOLL_BATCH overdue registrations' sockets at once,
 * confirmed by direct experiment to work correctly and NOT disturb
 * each socket's own individually-armed pending poll) -- only the
 * sockets the probe actually reports ready are canceled and re-armed
 * on their real registration OVERLAPPED.  This is O(batches) syscalls
 * per sweep, not O(overdue registrations): the earlier per-socket
 * cancel+rearm design measured 50-98% CPU with 1000-5000 idle pending
 * sockets (every syscall pair repeated every interval for every
 * outstanding registration, whether or not it had anything to report);
 * the batched probe measured near-zero CPU at the same scale because a
 * probe that finds nothing ready costs one syscall for up to 64
 * sockets, not one syscall pair PER socket.
 *
 * Cheap by construction even when many sockets are pending: a probe
 * that finds nothing ready is one cheap syscall per XTC_IOCP_REPOLL_BATCH
 * registrations; only registrations the probe actually flags ready
 * pay the individual cancel+rearm cost.  Called once per xtc_io_poll
 * before waiting on the port, so a real completion that arrives
 * promptly (all the paths this bug does NOT affect) is unaffected --
 * this only tops up the one broken case.
 */
#define XTC_IOCP_REPOLL_BATCH  64

struct __xtc_iocp_probe_batch {
	LARGE_INTEGER            Timeout;
	ULONG                    NumberOfHandles;
	ULONG                    Exclusive;
	XTC_AFD_POLL_HANDLE_INFO Handles[XTC_IOCP_REPOLL_BATCH];
};

static void
__xtc_iocp_repoll_sweep(xtc_io_t *io)
{
	struct __xtc_iocp_probe_batch probe_in, probe_out;
	struct __xtc_iocp_reg *overdue[XTC_IOCP_REPOLL_BATCH];
	int64_t now_ns = 0;
	int i, n_overdue;
	OVERLAPPED throwaway_ov;
	NTSTATUS st;

	if (io->n_reg == 0)
		return;
	(void)__os_clock_mono(&now_ns);

	n_overdue = 0;
	for (i = 0; i < io->n_reg && n_overdue < XTC_IOCP_REPOLL_BATCH; i++) {
		struct __xtc_iocp_reg *reg = io->reg_iocp[i];

		if (!reg->pending || reg->dead)
			continue;
		if (now_ns - reg->armed_at_ns < XTC_IOCP_REPOLL_NS)
			continue;
		overdue[n_overdue++] = reg;
	}
	if (n_overdue == 0)
		return;

	memset(&throwaway_ov, 0, sizeof throwaway_ov);
	memset(&probe_in, 0, sizeof probe_in);
	memset(&probe_out, 0, sizeof probe_out);
	probe_in.Timeout.QuadPart = 0;      /* immediate: report current state only */
	probe_in.NumberOfHandles = (ULONG)n_overdue;
	probe_in.Exclusive = FALSE;
	for (i = 0; i < n_overdue; i++) {
		probe_in.Handles[i].Handle = (HANDLE)overdue[i]->base;
		probe_in.Handles[i].Events = __interest_to_afd(overdue[i]->interest);
	}

	st = NtDeviceIoControlFile(io->afd, NULL, NULL, NULL,
	    (PIO_STATUS_BLOCK)&throwaway_ov, XTC_IOCTL_AFD_POLL,
	    &probe_in,
	    (ULONG)(sizeof(LARGE_INTEGER) + 2 * sizeof(ULONG) +
	        (size_t)n_overdue * sizeof(XTC_AFD_POLL_HANDLE_INFO)),
	    &probe_out,
	    (ULONG)(sizeof(LARGE_INTEGER) + 2 * sizeof(ULONG) +
	        (size_t)n_overdue * sizeof(XTC_AFD_POLL_HANDLE_INFO)));

	/* Any registration the sweep decided to probe gets a fresh
	 * armed_at_ns regardless of outcome, so a socket that stays
	 * unready keeps waiting its full interval again rather than being
	 * re-probed on every subsequent poll call. */
	for (i = 0; i < n_overdue; i++)
		(void)__os_clock_mono(&overdue[i]->armed_at_ns);

	/* A Timeout=0 poll is documented and observed (this repo's own
	 * standalone AFD experiments) to always resolve SYNCHRONOUSLY --
	 * there is nothing to wait for at zero timeout.  Guard the
	 * unexpected case anyway: throwaway_ov is stack-allocated, and if
	 * AFD ever returned STATUS_PENDING the kernel would hold a pointer
	 * into this stack frame past the return -- cancel it immediately
	 * (synchronous per the same NtCancelIoFileEx contract this file
	 * relies on elsewhere) rather than ever leave it outstanding. */
	if (st == STATUS_PENDING) {
		IO_STATUS_BLOCK c;
		memset(&c, 0, sizeof c);
		(void)NtCancelIoFileEx(io->afd, (PIO_STATUS_BLOCK)&throwaway_ov, &c);
		return;
	}
	if (st != STATUS_SUCCESS)
		return;         /* nothing ready (or a transient error); try again later */

	/* Only the sockets AFD actually flagged get canceled + re-armed on
	 * their real registration OVERLAPPED, which self-posts the ready
	 * event through the existing synchronous-arm path in __arm_poll --
	 * this throwaway probe's own OVERLAPPED needed no port association
	 * (it was never armed asynchronously; it only ever returns
	 * synchronously or is dropped) and needs no cleanup. */
	for (i = 0; i < (int)probe_out.NumberOfHandles; i++) {
		HANDLE ready_handle = probe_out.Handles[i].Handle;
		int j;

		for (j = 0; j < n_overdue; j++) {
			struct __xtc_iocp_reg *reg = overdue[j];
			struct __xtc_iocp_overlapped *o;
			IO_STATUS_BLOCK c;

			if ((HANDLE)reg->base != ready_handle)
				continue;
			o = (struct __xtc_iocp_overlapped *)reg->ovp;
			memset(&c, 0, sizeof c);
			(void)NtCancelIoFileEx(io->afd, XTC_OV_IOSB(o), &c);
			reg->pending = 0;
			(void)__arm_poll(io, reg);
			break;
		}
	}
}

static int
__push_dead(xtc_io_t *io, struct __xtc_iocp_reg *reg)
{
	if (io->n_dead >= io->cap_dead) {
		int nc = io->cap_dead == 0 ? 8 : io->cap_dead * 2;
		void *p = NULL;
		int rc = __os_realloc(io->dead_iocp,
		    sizeof(*io->dead_iocp) * (size_t)nc, &p);
		if (rc != XTC_OK)
			return rc;
		io->dead_iocp = p;
		io->cap_dead = nc;
	}
	io->dead_iocp[io->n_dead++] = reg;
	return XTC_OK;
}

/* PUBLIC: int xtc_io_reg_fd __P((xtc_io_t *, int, uint32_t, void *)); */
int
xtc_io_reg_fd(xtc_io_t *io, int fd, uint32_t interest, void *tag)
{
	struct __xtc_iocp_reg *reg = NULL;
	HANDLE base = INVALID_HANDLE_VALUE;
	int rc;

	if (io == NULL || fd < 0 || interest == 0)
		return XTC_E_INVAL;
	if (__find_reg(io, fd) >= 0)
		return XTC_E_INVAL;
	if ((rc = __base_socket(fd, &base)) != XTC_OK)
		return rc;

	/* Grow the live pointer array if needed (it holds pointers, so a
	 * realloc never moves the stable nodes themselves). */
	if (io->n_reg >= io->cap_reg) {
		int nc = io->cap_reg == 0 ? 16 : io->cap_reg * 2;
		void *p = NULL;
		if ((rc = __os_realloc(io->reg_iocp,
		    sizeof(*io->reg_iocp) * (size_t)nc, &p)) != XTC_OK)
			return rc;
		io->reg_iocp = p;
		io->cap_reg = nc;
	}

	if ((rc = __os_calloc(1, sizeof *reg, (void **)&reg)) != XTC_OK)
		return rc;
	reg->fd = fd;
	reg->base = base;
	reg->interest = interest;
	reg->tag = tag;
	reg->pending = 0;
	reg->dead = 0;
	if ((rc = __os_calloc(1, sizeof(struct __xtc_iocp_overlapped),
	    (void **)&reg->ovp)) != XTC_OK) {
		__os_free(reg);
		return rc;
	}

	if ((rc = __arm_poll(io, reg)) != XTC_OK) {
		__os_free(reg->ovp);
		__os_free(reg);
		return rc;
	}
	io->reg_iocp[io->n_reg++] = reg;
	return XTC_OK;
}

/* PUBLIC: int xtc_io_mod_fd __P((xtc_io_t *, int, uint32_t, void *)); */
int
xtc_io_mod_fd(xtc_io_t *io, int fd, uint32_t interest, void *tag)
{
	struct __xtc_iocp_reg *reg;
	int idx;

	if (io == NULL || fd < 0 || interest == 0)
		return XTC_E_INVAL;
	idx = __find_reg(io, fd);
	if (idx < 0)
		return XTC_E_INVAL;
	reg = io->reg_iocp[idx];
	reg->interest = interest;
	reg->tag = tag;
	/*
	 * Cancel the in-flight poll (if any) and re-arm immediately with
	 * the new interest mask.  NtCancelIoFileEx is confirmed
	 * synchronous here (it finalizes the IRP's IOSB in-process,
	 * STATUS_CANCELLED, before returning) but -- like every other
	 * synchronous AFD/cancel outcome on this driver -- does NOT queue
	 * a port completion, so waiting for one (the original design)
	 * would leave the OLD interest mask armed until the
	 * __xtc_iocp_repoll_sweep workaround eventually notices.  Re-arm
	 * right here instead: cheaper and gives the new mask effect
	 * immediately rather than up to XTC_IOCP_REPOLL_NS later.
	 */
	if (reg->pending) {
		IO_STATUS_BLOCK c;
		memset(&c, 0, sizeof c);
		(void)NtCancelIoFileEx(io->afd,
		    XTC_OV_IOSB((struct __xtc_iocp_overlapped *)reg->ovp), &c);
		reg->pending = 0;
	}
	return __arm_poll(io, reg);
}

/* PUBLIC: int xtc_io_del_fd __P((xtc_io_t *, int)); */
int
xtc_io_del_fd(xtc_io_t *io, int fd)
{
	struct __xtc_iocp_reg *reg;
	int idx, last;

	if (io == NULL || fd < 0)
		return XTC_E_INVAL;
	idx = __find_reg(io, fd);
	if (idx < 0)
		return XTC_E_INVAL;
	reg = io->reg_iocp[idx];

	/* Swap-remove the node POINTER from the live array.  The node
	 * itself keeps its stable address regardless. */
	last = --io->n_reg;
	if (idx != last)
		io->reg_iocp[idx] = io->reg_iocp[last];

	if (reg->pending) {
		/*
		 * The poll OVERLAPPED is still owned by the kernel.  We must
		 * NOT free the node now (the classic IOCP use-after-free):
		 * o->back still points at this node and a completion is still
		 * in flight.  Cancel the request and move the node to the
		 * dead list; xtc_io_poll frees it when the (canceled)
		 * completion is reaped.
		 */
		IO_STATUS_BLOCK c;
		int rc;
		reg->dead = 1;
		if ((rc = __push_dead(io, reg)) != XTC_OK) {
			/* Cannot defer-free: restore to the live array so the
			 * node is never leaked nor freed-while-armed. */
			reg->dead = 0;
			io->reg_iocp[io->n_reg++] = reg;
			return rc;
		}
		memset(&c, 0, sizeof c);
		(void)NtCancelIoFileEx(io->afd,
		    XTC_OV_IOSB((struct __xtc_iocp_overlapped *)reg->ovp), &c);
	} else {
		__os_free(reg->ovp);
		__os_free(reg);
	}
	return XTC_OK;
}

/* Non-io_uring backends: reg/del are kernel-synchronized (epoll) or
 * this backend keeps its own registry; the cross-loop deferred-
 * unregister that io_uring needs is a no-op passthrough here (the
 * caller is xtc_proc_wait_fd cleanup after a migration).  Provided so
 * the single caller links on every backend. */
int
__xtc_io_defer_del_fd(xtc_io_t *io, int fd)
{
	return xtc_io_del_fd(io, fd);
}

/* --------------------------------------------------------------- */
/* file AIO                                                        */
/* --------------------------------------------------------------- */

/* PUBLIC: int xtc_io_aio_submit __P((xtc_io_t *, xtc_aio_t *)); */
/*
 * Native Windows file AIO.  A pread/pwrite is an overlapped
 * ReadFile/WriteFile whose file HANDLE is associated with the
 * completion port, so its completion is dequeued by
 * GetQueuedCompletionStatusEx like every socket event -- no pool
 * thread, no hEvent.  fsync has no async form (FlushFileBuffers is
 * synchronous) so it returns XTC_E_NOSYS and xtc_aio offloads it.  An
 * op that completes synchronously (cached data) still posts a
 * completion to the port (we do not skip it), so reaping is uniform.
 */
int
xtc_io_aio_submit(xtc_io_t *io, xtc_aio_t *a)
{
	HANDLE fh;
	OVERLAPPED *ov = NULL;
	BOOL ok;
	DWORD err;

	if (io == NULL || a == NULL)
		return XTC_E_INVAL;
	if (a->op != XTC_AIO_PREAD && a->op != XTC_AIO_PWRITE)
		return XTC_E_NOSYS;             /* fsync/fdatasync: offload */

	fh = (HANDLE)_get_osfhandle(a->fd);
	if (fh == INVALID_HANDLE_VALUE)
		return XTC_E_INVAL;

	/*
	 * Associate the file with the port once.  CreateIoCompletionPort
	 * on an already-associated handle returns NULL with
	 * ERROR_INVALID_PARAMETER, which we treat as "already done".
	 */
	if (CreateIoCompletionPort(fh, (HANDLE)io->iocp,
	    XTC_IOCP_KEY_IO, 0) == NULL) {
		err = GetLastError();
		if (err != ERROR_INVALID_PARAMETER)
			return XTC_E_AGAIN;     /* cannot associate: offload */
	} else {
		/* Newly associated: ask the kernel to NOT post a port
		 * completion when the op completes synchronously (the call
		 * returns TRUE).  We finish those inline below.  This keeps a
		 * synchronous completion from being double-counted, and it is
		 * the only correct behavior for a non-overlapped handle (one
		 * opened by _open / CreateFile without FILE_FLAG_OVERLAPPED),
		 * which NEVER posts a port completion -- without this, such a
		 * handle's op would park the fiber forever. */
		(void)SetFileCompletionNotificationModes(fh,
		    FILE_SKIP_COMPLETION_PORT_ON_SUCCESS);
	}

	if (__os_calloc(1, sizeof *ov, (void **)&ov) != XTC_OK)
		return XTC_E_AGAIN;             /* offload */
	ov->Offset     = (DWORD)((uint64_t)a->off & 0xFFFFFFFFu);
	ov->OffsetHigh = (DWORD)((uint64_t)a->off >> 32);

	a->done = 0;
	a->res = 0;
	if (a->op == XTC_AIO_PREAD)
		ok = ReadFile(fh, a->buf, (DWORD)a->len, NULL, ov);
	else
		ok = WriteFile(fh, a->buf, (DWORD)a->len, NULL, ov);

	if (ok) {
		/* Synchronous completion.  With
		 * FILE_SKIP_COMPLETION_PORT_ON_SUCCESS set above (and always,
		 * for a non-overlapped handle), NO completion will arrive on
		 * the port, so finish the op inline rather than parking the
		 * fiber on a completion that never comes.  The transferred
		 * count is in the OVERLAPPED's InternalHigh. */
		DWORD nb = 0;
		(void)GetOverlappedResult(fh, ov, &nb, FALSE);
		a->res = (int)nb;
		a->done = 1;
		__os_free(ov);
		return XTC_OK;
	}

	{
		err = GetLastError();
		if (err == ERROR_HANDLE_EOF) {
			/* Read started at/after EOF: the request did not begin,
			 * so no completion will arrive on the port.  Finish it
			 * inline as a zero-byte read. */
			a->res = 0;
			a->done = 1;
			__os_free(ov);
			return XTC_OK;
		}
		if (err != ERROR_IO_PENDING) {
			__os_free(ov);
			return XTC_E_AGAIN;     /* hard failure: offload */
		}
	}
	/*
	 * ERROR_IO_PENDING: the op is genuinely asynchronous (an
	 * overlapped handle), so its completion WILL be posted to the
	 * port.  Track it so xtc_io_poll reaps it; ov is now owned by the
	 * kernel until that completion is dequeued.
	 */
	if (io->n_aio >= io->cap_aio) {
		int nc = io->cap_aio == 0 ? 8 : io->cap_aio * 2;
		void *p = NULL;
		if (__os_realloc(io->aio_pend,
		    sizeof(*io->aio_pend) * (size_t)nc, &p) != XTC_OK) {
			(void)CancelIoEx(fh, ov);
			/* Leave ov tracked-as-leaked rather than free a kernel-
			 * owned buffer; but we have nowhere to track it, so wait
			 * for the cancel completion synchronously. */
			{
				DWORD nb = 0;
				(void)GetOverlappedResult(fh, ov, &nb, TRUE);
			}
			__os_free(ov);
			return XTC_E_AGAIN;
		}
		io->aio_pend = p;
		io->cap_aio = nc;
	}
	io->aio_pend[io->n_aio].ov  = ov;
	io->aio_pend[io->n_aio].aio = a;
	io->aio_pend[io->n_aio].fh  = fh;
	io->n_aio++;
	return XTC_OK;
}

/* Find and remove the file-AIO record whose OVERLAPPED matches ov. */
static struct __xtc_iocp_aio *
__take_aio(xtc_io_t *io, OVERLAPPED *ov, struct __xtc_iocp_aio *slot)
{
	int i;
	for (i = 0; i < io->n_aio; i++) {
		if ((OVERLAPPED *)io->aio_pend[i].ov == ov) {
			*slot = io->aio_pend[i];
			io->n_aio--;
			if (i != io->n_aio)
				io->aio_pend[i] = io->aio_pend[io->n_aio];
			return slot;
		}
	}
	return NULL;
}

/* Find the dead-list node whose OVERLAPPED matches ov, removing it. */
static struct __xtc_iocp_reg *
__take_dead(xtc_io_t *io, OVERLAPPED *ov)
{
	int i;
	for (i = 0; i < io->n_dead; i++) {
		struct __xtc_iocp_overlapped *o =
		    (struct __xtc_iocp_overlapped *)io->dead_iocp[i]->ovp;
		if (&o->ov == ov) {
			struct __xtc_iocp_reg *node = io->dead_iocp[i];
			io->n_dead--;
			if (i != io->n_dead)
				io->dead_iocp[i] = io->dead_iocp[io->n_dead];
			return node;
		}
	}
	return NULL;
}

/* Is reg still a live registration (not on the dead list)? */
static int
__reg_is_live(xtc_io_t *io, struct __xtc_iocp_reg *reg)
{
	int i;
	for (i = 0; i < io->n_reg; i++)
		if (io->reg_iocp[i] == reg)
			return 1;
	return 0;
}

/* --------------------------------------------------------------- */
/* poll                                                            */
/* --------------------------------------------------------------- */

int
xtc_io_poll(xtc_io_t *io, xtc_io_event_t *events, int max,
            int64_t timeout_ns, int *n_out)
{
	OVERLAPPED_ENTRY batch[64];
	ULONG    n_done = 0;
	DWORD    timeout_ms;
	BOOL     ok;
	int      out_idx = 0;
	int      wakeup_emitted = 0;
	int      batch_max;
	ULONG    j;

	if (io == NULL || events == NULL || max <= 0 || n_out == NULL)
		return XTC_E_INVAL;
	*n_out = 0;

	batch_max = max < (int)(sizeof batch / sizeof batch[0])
	    ? max : (int)(sizeof batch / sizeof batch[0]);

	if (timeout_ns < 0)        timeout_ms = INFINITE;
	else if (timeout_ns == 0)  timeout_ms = 0;
	else                       timeout_ms =
	    (DWORD)((timeout_ns + 999999) / 1000000);

	/*
	 * AFD async-completion workaround (see the file header + the
	 * XTC_IOCP_REPOLL_NS note): a poll armed while the socket was NOT
	 * yet ready never completes on its own when the socket LATER
	 * becomes ready, so a single GetQueuedCompletionStatusEx slice can
	 * return zero events even though data has arrived.  To honour the
	 * caller's timeout contract ("return the event if it becomes ready
	 * within timeout_ns"), cap each wait slice at the repoll interval
	 * and LOOP -- sweeping (cancel + re-arm every overdue poll) between
	 * slices -- until an event is emitted, a wakeup fires, or the
	 * caller's real deadline elapses.  A zero timeout still does
	 * exactly one slice; a negative (INFINITE) timeout loops until an
	 * event arrives.  On epoll/kqueue one slice suffices; this loop is
	 * only load-bearing on the IOCP/AFD path.
	 */
	{
		int64_t deadline_ns = 0;
		int have_deadline = (timeout_ns > 0);
		if (have_deadline) {
			int64_t now_ns = 0;
			(void)__os_clock_mono(&now_ns);
			deadline_ns = now_ns + timeout_ns;
		}

		for (;;) {
			DWORD slice_ms = timeout_ms;

			/* Cap the actual wait to the repoll interval whenever any
			 * socket is registered: with the AFD async-completion bug a
			 * caller-requested INFINITE (or merely long) wait could
			 * otherwise block forever on a pending poll that will never
			 * complete on its own.  Bounds every registered socket's
			 * worst-case readiness latency to XTC_IOCP_REPOLL_NS. */
			if (io->n_reg > 0) {
				DWORD repoll_ms = (DWORD)(XTC_IOCP_REPOLL_NS / 1000000);
				if (slice_ms > repoll_ms)
					slice_ms = repoll_ms;
			}

			out_idx = 0;
			wakeup_emitted = 0;
			n_done = 0;
			ok = GetQueuedCompletionStatusEx((HANDLE)io->iocp, batch,
			    (ULONG)batch_max, &n_done, slice_ms, FALSE);
			if (!ok) {
				DWORD e = GetLastError();
				/* An empty dequeue is a benign timeout.  GetLastError is
				 * only meaningful when a wait actually failed; after a
				 * 0 ms poll of an empty port it can carry a STALE code
				 * from an earlier call, so keying strictly on
				 * WAIT_TIMEOUT wrongly reports XTC_E_INTERNAL.  Treat
				 * n_done == 0 as the timeout it is. */
				if (e == WAIT_TIMEOUT || n_done == 0) {
					goto slice_done;
				}
				return XTC_E_INTERNAL;
			}

	/*
	 * Process EVERY dequeued completion -- the kernel already removed
	 * them from the port, so dropping one silently loses an event or
	 * leaks an OVERLAPPED.  Only the EMISSION into the caller's buffer
	 * is gated on out_idx < max; a ready fd we cannot report this
	 * round is still re-armed (level-triggered), so the next poll sees
	 * it again.
	 */
	for (j = 0; j < n_done; j++) {
		OVERLAPPED_ENTRY *ce = &batch[j];
		OVERLAPPED       *ov = ce->lpOverlapped;
		struct __xtc_iocp_overlapped *o;
		struct __xtc_iocp_reg        *reg;
		struct __xtc_iocp_aio         aslot;
		struct __xtc_iocp_aio        *ar;

		/* Wakeup: a PostQueuedCompletionStatus with a NULL
		 * OVERLAPPED and the wakeup key.  At most one is ever queued
		 * (coalesced at post time), so clearing wakeup_pending here
		 * re-arms the post side; emit a single XTC_IO_WAKEUP event. */
		if (ce->lpCompletionKey == XTC_IOCP_KEY_WAKEUP || ov == NULL) {
			atomic_store(&io->wakeup_pending, 0);
			if (!wakeup_emitted && out_idx < max) {
				(void)__xtc_io_drain_wakeup(io);
				events[out_idx].tag = NULL;
				events[out_idx].flags = XTC_IO_WAKEUP;
				out_idx++;
				wakeup_emitted = 1;
			}
			continue;
		}

		/* A file-AIO completion? */
		ar = __take_aio(io, ov, &aslot);
		if (ar != NULL) {
			xtc_aio_t *a = (xtc_aio_t *)ar->aio;
			DWORD nbytes = 0;
			if (GetOverlappedResult((HANDLE)ar->fh, ov, &nbytes, FALSE))
				a->res = (int)nbytes;
			else if (GetLastError() == ERROR_HANDLE_EOF)
				a->res = (int)nbytes;        /* short read at EOF */
			else
				a->res = -5;                 /* I/O error (~ -EIO) */
			a->done = 1;
			if (out_idx < max) {
				events[out_idx].tag = a->tag;
				events[out_idx].flags = XTC_IO_AIO;
				out_idx++;
			}
			__os_free(ov);                   /* kernel released it */
			continue;
		}

		/*
		 * Otherwise it is an AFD poll completion: ov is the first
		 * member of a __xtc_iocp_overlapped, so cast back to find
		 * the owning (stable-address) registration node.
		 */
		o = (struct __xtc_iocp_overlapped *)ov;
		reg = o->back;

		/* Deregistered while in flight?  Free its node now that the
		 * kernel has released the (canceled) OVERLAPPED -- this is the
		 * deferred free the ownership rule mandates. */
		if (reg->dead) {
			struct __xtc_iocp_reg *node = __take_dead(io, ov);
			if (node != NULL) {
				__os_free(node->ovp);
				__os_free(node);
			}
			continue;
		}

		/* Safety net: a completion for a node that is neither live
		 * nor on the dead list cannot happen under the ownership rule
		 * (a node is freed only after its terminal completion is
		 * reaped here).  Skip it rather than risk a double free. */
		if (!__reg_is_live(io, reg))
			continue;

		/* A completion for a reg whose poll is not outstanding
		 * (pending already cleared) is a duplicate -- e.g. our
		 * synchronous-success manual post racing an AFD-queued one.
		 * Skip it; the live re-arm keeps the fd watched. */
		if (!reg->pending)
			continue;
		reg->pending = 0;

		/*
		 * Emit readiness from the AFD result.  poll_out.Handles[0]
		 * carries the events that fired; NumberOfHandles == 0 means
		 * the poll completed with nothing (e.g. canceled by mod_fd).
		 */
		if (o->poll_out.NumberOfHandles >= 1 && out_idx < max) {
			ULONG afd = o->poll_out.Handles[0].Events;
			uint32_t flags = __afd_to_flags(afd);
			if (flags != 0) {
				events[out_idx].tag = reg->tag;
				events[out_idx].flags = flags;
				out_idx++;
			}
		}
		/* Re-arm for level-triggered behaviour (also re-arms after a
		 * mod_fd cancel, picking up the new interest mask).  Done even
		 * when the event buffer is full, so the fd stays watched. */
		(void)__arm_poll(io, reg);
	}

slice_done:
			/* One slice processed.  Return as soon as we have
			 * something for the caller (a readiness/AIO event or a
			 * wakeup), or the caller did not want to block (0 ms), or
			 * the caller's real deadline has elapsed.  Otherwise sweep
			 * the AFD workaround and take another slice. */
			if (out_idx > 0 || wakeup_emitted || timeout_ms == 0) {
				*n_out = out_idx;
				__xtc_iocp_repoll_sweep(io);
				return XTC_OK;
			}
			if (have_deadline) {
				int64_t now_ns = 0;
				(void)__os_clock_mono(&now_ns);
				if (now_ns >= deadline_ns) {
					*n_out = 0;
					__xtc_iocp_repoll_sweep(io);
					return XTC_OK;
				}
			}
			__xtc_iocp_repoll_sweep(io);
		}
	}
}

#endif /* XTC_IO_BACKEND_IOCP */

typedef int __xtc_io_iocp_unused;
