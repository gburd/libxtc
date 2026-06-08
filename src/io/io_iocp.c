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
 * windows.h from defining the STATUS_* macros that ntstatus.h owns,
 * then we pull ntstatus.h for the full set. */
#define WIN32_NO_STATUS
#include <winsock2.h>
#include <ws2tcpip.h>
#include <mswsock.h>
#include <windows.h>
#include <winternl.h>
#undef WIN32_NO_STATUS
#include <ntstatus.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define XTC_IOCP_KEY_WAKEUP   ((ULONG_PTR)1)
#define XTC_IOCP_KEY_IO       ((ULONG_PTR)0)   /* socket poll + file AIO */

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

/* IOCTL_AFD_POLL = FSCTL_AFD_BASE(0x12) function 9, METHOD_BUFFERED. */
#define XTC_IOCTL_AFD_POLL \
	CTL_CODE(0x12, 9, METHOD_BUFFERED, FILE_ANY_ACCESS)

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
 * io_common.c's xtc_io_wakeup on Windows. */
int
__xtc_io_iocp_wakeup_post(xtc_io_t *io)
{
	if (io == NULL || io->iocp == NULL)
		return XTC_E_INVAL;
	if (!PostQueuedCompletionStatus((HANDLE)io->iocp, 0,
	    XTC_IOCP_KEY_WAKEUP, NULL))
		return XTC_E_INTERNAL;
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
	/* Skip the redundant queued completion when the AFD poll
	 * finishes synchronously: we always reap from the port. */
	(void)SetFileCompletionNotificationModes(afd,
	    FILE_SKIP_SET_EVENT_ON_HANDLE);
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
	if (WSAIoctl((SOCKET)fd, SIO_BASE_HANDLE, NULL, 0, &base,
	    sizeof base, &got, NULL, NULL) != 0)
		return XTC_E_INTERNAL;
	if (base == INVALID_SOCKET)
		return XTC_E_INTERNAL;
	*out = (HANDLE)base;
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
	if (st == STATUS_SUCCESS || st == STATUS_PENDING) {
		reg->pending = 1;
		return XTC_OK;
	}
	reg->pending = 0;
	return XTC_E_INTERNAL;
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
	 * Cancel the in-flight poll (if any); the canceled completion is
	 * reaped in xtc_io_poll and triggers the re-arm with the new
	 * interest mask.  We cannot re-submit on the same OVERLAPPED while
	 * it is still owned by the kernel, so the re-arm waits for the
	 * cancel's completion.  If nothing was armed, arm now.
	 */
	if (reg->pending) {
		IO_STATUS_BLOCK c;
		memset(&c, 0, sizeof c);
		(void)NtCancelIoFileEx(io->afd,
		    XTC_OV_IOSB((struct __xtc_iocp_overlapped *)reg->ovp), &c);
		return XTC_OK;
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

	if (!ok) {
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
	 * Whether ok==TRUE (synchronous) or ERROR_IO_PENDING, a
	 * completion is posted to the port (we did not set
	 * FILE_SKIP_COMPLETION_PORT_ON_SUCCESS on the file handle).  Track
	 * the op so xtc_io_poll reaps it.  ov is now owned by the kernel.
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

	ok = GetQueuedCompletionStatusEx((HANDLE)io->iocp, batch,
	    (ULONG)batch_max, &n_done, timeout_ms, FALSE);
	if (!ok) {
		DWORD e = GetLastError();
		if (e == WAIT_TIMEOUT)
			return XTC_OK;
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
		 * OVERLAPPED and the wakeup key.  Coalesce duplicates. */
		if (ce->lpCompletionKey == XTC_IOCP_KEY_WAKEUP || ov == NULL) {
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

	*n_out = out_idx;
	return XTC_OK;
}

#endif /* XTC_IO_BACKEND_IOCP */

typedef int __xtc_io_iocp_unused;
