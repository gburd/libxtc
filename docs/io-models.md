---
title: IO models
parent: Reference
nav_order: 9
lede: >-
  The same xtc_io_op pattern under epoll, io_uring, kqueue, and IOCP,
  side by side, grounded in the four real backend files.
permalink: /reference/io-models/
---
Layer L1 (`src/io/`) presents ONE contract to L2 and above:

    int xtc_io_reg_fd (xtc_io_t *, int fd, uint32_t interest, void *tag);
    int xtc_io_mod_fd (xtc_io_t *, int fd, uint32_t interest, void *tag);
    int xtc_io_del_fd (xtc_io_t *, int fd);
    int xtc_io_poll   (xtc_io_t *, xtc_io_event_t *events, int max,
                       int64_t timeout_ns, int *n_out);

Exactly one backend is compiled in per binary, chosen at configure
time (never at runtime -- see `docs/ARCHITECTURE.md`).  Every backend
implements the same four calls, but the kernel facility underneath is
readiness-based (epoll, kqueue), completion-based (io_uring, IOCP), or
a hybrid the backend emulates into one or the other shape.  This page
is a translation table between the four real backend files under
`src/io/`, each showing the SAME worked example: register interest in
a socket becoming readable, wait, reap the result, read one buffer,
and be ready to see it again (level-triggered re-arm).

The four files are `src/io/io_epoll.c`, `src/io/io_uring.c`,
`src/io/io_kqueue.c`, and `src/io/io_iocp.c`.  Every function name and
code fragment below is copied from (or is a close paraphrase of) that
file, not a textbook description of the underlying kernel API.

## Contents

  1. [The readiness/completion axis](#the-readinesscompletion-axis)
  2. [Worked example: epoll](#worked-example-epoll)
  3. [Worked example: io_uring](#worked-example-io_uring)
  4. [Worked example: kqueue](#worked-example-kqueue)
  5. [Worked example: IOCP](#worked-example-iocp)
  6. [Translation table](#translation-table)
  7. [See also](#see-also)

---

## The readiness/completion axis

  * **Readiness backends** (epoll, kqueue) tell you "this fd CAN be
    read/written now"; you still make the syscall yourself.  A
    readiness backend is naturally level-triggered if you ask it to
    be, so re-arming is either automatic (epoll: nothing to do,
    `EPOLLIN` fires again next `epoll_wait` if unread data remains) or
    a one-line re-register (kqueue's `EV_CLEAR` edge-triggers, so xtc
    re-registers on every `xtc_io_reg_fd` / `xtc_io_mod_fd` call, not
    per-event).

  * **Completion backends** (io_uring's `IORING_OP_POLL_ADD`, IOCP's
    AFD poll) tell you "an operation you submitted has finished."
    xtc's io_uring backend actually submits a POLL, not a read -- it
    still reports readiness, using `POLL_MULTISHOT` so one submission
    keeps delivering CQEs (no re-arm needed until the fd is removed).
    IOCP has no multishot poll primitive at all, so xtc's backend
    re-arms one AFD poll per completion (true completion semantics,
    one-shot by construction).

The xtc_io_op pattern is the same regardless: **register, wait, reap,
read, re-arm (if the backend needs it)**.  What differs is which step
the kernel does for you.

---

## Worked example: epoll

Scenario: a socket `fd` may have data.  Register interest, block until
readable, read one buffer, loop (epoll is level-triggered here because
xtc registers plain `EPOLLIN`, not `EPOLLET`, so a partially-drained
socket fires again next poll with zero extra code).

Register (`src/io/io_epoll.c`, `xtc_io_reg_fd`):

```c
struct epoll_event ev;
ev.events = EPOLLIN;             /* __interest_to_events(XTC_IO_READABLE) */
ev.data.ptr = tag;                /* caller's opaque tag, returned verbatim */
epoll_ctl(io->epfd, EPOLL_CTL_ADD, fd, &ev);
```

Wait + reap (`xtc_io_poll`):

```c
struct epoll_event evs[64];
int got = epoll_wait(io->epfd, evs, batch, timeout_ms);
for (i = 0; i < got; i++) {
	events[out_idx].tag   = evs[i].data.ptr;
	events[out_idx].flags = __epoll_to_flags(evs[i].events);
	out_idx++;
}
```

Read one buffer -- this is application/L2+ code, outside `src/io/`,
using the plain `read(2)` syscall on `fd` once `xtc_io_poll` reports
`XTC_IO_READABLE` for that tag.

Re-arm: nothing to do.  `xtc_io_reg_fd` used `EPOLLIN` (level-
triggered), so if the read did not drain the socket, the next
`epoll_wait` call reports `EPOLLIN` again with no further `epoll_ctl`
call.  A caller that wants to change the interest mask (say, add
`EPOLLOUT` because a write is now pending) calls `xtc_io_mod_fd`,
which is `EPOLL_CTL_MOD`.

---

## Worked example: io_uring

Scenario: the same socket-readable-then-read-one-buffer case.
io_uring has no native "readiness" op, so xtc's backend submits an
`IORING_OP_POLL_ADD` (a completion-style wrapper around the same
readiness question) with `POLL_MULTISHOT` so a single submission keeps
producing CQEs -- the re-arm epoll does implicitly, io_uring does via
one flag at registration time instead of per-poll.

Register (`src/io/io_uring.c`, `xtc_io_reg_fd` -> `__submit_poll_add`):

```c
struct io_uring_sqe *sqe = io_uring_get_sqe(&io->ring);
io_uring_prep_poll_multishot(sqe, uf->fd,
    __interest_to_pollmask(uf->interest));   /* POLLIN for XTC_IO_READABLE */
io_uring_sqe_set_data(sqe, uf);               /* uf carries the tag */
io_uring_submit(&io->ring);
```

Wait + reap (`xtc_io_poll`):

```c
struct io_uring_cqe *cqe;
io_uring_wait_cqe_timeout(&io->ring, &cqe, tsp);
/* ... for each CQE: */
uf = (struct __xtc_uring_fd *)io_uring_cqe_get_data(cqe);
events[got].tag   = uf->tag;
events[got].flags = __pollres_to_flags((uint32_t)cqe->res);
io_uring_cqe_seen(&io->ring, cqe);
```

Read one buffer: same as epoll -- a plain `read(2)` in application
code, once `xtc_io_poll` reports `XTC_IO_READABLE`.

Re-arm: because the poll was submitted `MULTISHOT`, the SAME
submission keeps delivering a CQE every time the fd becomes readable
again; the backend does not resubmit per-event.  It only resubmits
(`__submit_poll_add` again) on an explicit `xtc_io_mod_fd` (interest
change: cancel via `__submit_poll_remove` then re-submit) or when a
multishot auto-disarms.  Deregistration (`xtc_io_del_fd`) issues
`IORING_OP_POLL_REMOVE` and moves the registration to a zombie list
until its terminal (non-`IORING_CQE_F_MORE`) CQE is drained, because
the kernel may still have CQEs in flight referencing it -- freeing
early is a use-after-free the epoll backend simply has no equivalent
danger for (epoll's `epoll_ctl(EPOLL_CTL_DEL)` is synchronous).

---

## Worked example: kqueue

Scenario: the same case again.  kqueue is readiness-based like epoll,
but its clearing semantics are edge-triggered by default, so xtc
always passes `EV_CLEAR` and treats every `xtc_io_reg_fd` as the
one-time "arm" and every `xtc_io_mod_fd` as an explicit re-arm with a
new mask (there is no implicit level-triggered re-fire the way plain
`EPOLLIN` gives epoll).

Register (`src/io/io_kqueue.c`, `xtc_io_reg_fd` -> `__kev_register`):

```c
struct kevent kev;
EV_SET(&kev, fd, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, tag);
kevent(io->epfd, &kev, 1, NULL, 0, NULL);
```

Wait + reap (`xtc_io_poll`):

```c
struct kevent evs[64];
int got = kevent(io->epfd, NULL, 0, evs, batch, tsp);
for (i = 0; i < got; i++) {
	uint32_t f = 0;
	if (evs[i].filter == EVFILT_READ)  f |= XTC_IO_READABLE;
	if (evs[i].flags & EV_EOF)         f |= XTC_IO_HUP;
	events[out_idx].tag   = evs[i].udata;
	events[out_idx].flags = f;
}
```

Read one buffer: same as the other two, a plain `read(2)` once
`XTC_IO_READABLE` is reported.

Re-arm: because the filter was registered with `EV_CLEAR`, the kernel
delivers the event once per edge and then clears it; it fires AGAIN
the next time NEW data arrives on an already-registered fd without any
further xtc call, because `kevent(EV_ADD)` on an existing filter just
updates the udata/flags and the kernel still tracks the fd's readable
state under the hood -- from xtc's contract this behaves exactly like
epoll's level-triggered re-fire for the common "keep reading until
EAGAIN" idiom.  An explicit interest-mask change goes through
`xtc_io_mod_fd`, which clears BOTH filters first (`__kev_register`
with `del=1`) then re-registers only the requested ones, avoiding a
stale write-interest left over from a prior mask.

---

## Worked example: IOCP

Scenario: the same case on Windows.  There is no native socket-
readiness primitive in the completion-port API, so xtc's backend opens
`\Device\Afd` and issues an `IOCTL_AFD_POLL` per registered socket --
the AFD poll IS the completion whose arrival IS the readiness
notification, and because AFD polls are inherently one-shot, the
backend re-arms explicitly after every single completion (there is no
multishot analog on this backend the way io_uring has one).

Register (`src/io/io_iocp.c`, `xtc_io_reg_fd` -> `__arm_poll`):

```c
o->poll_in.NumberOfHandles = 1;
o->poll_in.Handles[0].Handle = (HANDLE)reg->base;   /* base socket handle */
o->poll_in.Handles[0].Events = __interest_to_afd(reg->interest);  /* AFD_POLL_RECEIVE, etc */
NtDeviceIoControlFile(io->afd, NULL, NULL, NULL, XTC_OV_IOSB(o),
    XTC_IOCTL_AFD_POLL, &o->poll_in, sizeof o->poll_in,
    &o->poll_out, sizeof o->poll_out);
/* STATUS_PENDING: the kernel now owns o->ov until dequeued. */
```

Wait + reap (`xtc_io_poll`):

```c
OVERLAPPED_ENTRY batch[64];
GetQueuedCompletionStatusEx((HANDLE)io->iocp, batch, batch_max,
    &n_done, timeout_ms, FALSE);
/* ... for each completion: */
o = (struct __xtc_iocp_overlapped *)ce->lpOverlapped;
reg = o->back;
events[out_idx].tag   = reg->tag;
events[out_idx].flags = __afd_to_flags(o->poll_out.Handles[0].Events);
```

Read one buffer: same as the other three -- once `XTC_IO_READABLE` is
reported, application code issues its own read (or, for a genuinely
async file read rather than a socket, `xtc_io_aio_submit` with
`XTC_AIO_PREAD`, a SEPARATE completion path also reaped by this same
`xtc_io_poll` loop; see the AIO section of `xtc_aio(3)`).

Re-arm: mandatory and explicit, unlike the other three backends.  The
last line of the AFD-completion branch in `xtc_io_poll` is:

```c
(void)__arm_poll(io, reg);   /* re-arm for level-triggered behaviour */
```

This is done even when the caller's event buffer is already full for
this poll call, so a socket that stays ready is never silently
dropped.  `src/io/io_iocp.c` documents a real driver bug this file
works around (a `STATUS_PENDING` poll armed while a socket is NOT yet
ready never completes when it later becomes ready, on the tested
Windows Server 2022 driver): `__xtc_iocp_repoll_sweep` force-refreshes
any poll that has been pending longer than `XTC_IOCP_REPOLL_NS` (8 ms)
with a single batched, zero-timeout probe covering up to 64
registrations, and only cancels + re-arms the ones the probe actually
flags ready.  No other backend on this page needs anything like it;
it is called out here because it is the sharpest illustration of "the
completion model is a leaky abstraction over readiness when the OS
doesn't actually offer a native readiness completion."

---

## Translation table

| Step in the xtc_io_op pattern | epoll (`io_epoll.c`) | io_uring (`io_uring.c`) | kqueue (`io_kqueue.c`) | IOCP (`io_iocp.c`) |
|---|---|---|---|---|
| Register | `epoll_ctl(EPOLL_CTL_ADD)`, tag in `epoll_data.ptr` | `IORING_OP_POLL_ADD` with `POLL_MULTISHOT`, tag in SQE `user_data` | `kevent(EV_ADD\|EV_CLEAR)`, tag in `kevent.udata` | `IOCTL_AFD_POLL` via `NtDeviceIoControlFile`, tag on the reg node the OVERLAPPED points back to |
| Wait | `epoll_wait` | `io_uring_wait_cqe_timeout` + `io_uring_peek_cqe` to drain the rest | `kevent` with an output array | `GetQueuedCompletionStatusEx` |
| Reap | walk `struct epoll_event[]`, tag = `data.ptr` | walk `struct io_uring_cqe`, tag = `io_uring_cqe_get_data(cqe)` | walk `struct kevent[]`, tag = `udata` | walk `OVERLAPPED_ENTRY[]`, tag = `((iocp_overlapped *)lpOverlapped)->back->tag` |
| Change interest | `epoll_ctl(EPOLL_CTL_MOD)` | cancel (`IORING_OP_POLL_REMOVE`) then re-submit `POLL_ADD` | clear both filters then re-register the requested one(s) | cancel (`NtCancelIoFileEx`) then `__arm_poll` again |
| Deregister | `epoll_ctl(EPOLL_CTL_DEL)`, synchronous, safe to free immediately | `IORING_OP_POLL_REMOVE`, then park on a zombie list until the terminal CQE drains | `EV_DELETE`, synchronous, safe to free immediately | cancel, then park on a dead list until the (canceled) completion drains |
| Level-triggered re-arm | automatic (plain `EPOLLIN`, no `EPOLLET`) | automatic while `POLL_MULTISHOT` stays armed | automatic between edges on an already-`EV_ADD`ed filter; explicit only on a mask change | explicit, EVERY completion (`__arm_poll` called again at the bottom of the reap loop), plus a periodic force-refresh sweep for a driver bug |
| Cross-thread wakeup | write end of a self-pipe registered with `EPOLLIN` | single-shot `POLL_ADD` on the wakeup pipe, re-armed before draining (not after -- closes a lost-wakeup race) | `EVFILT_USER` with `NOTE_TRIGGER`, no pipe/fd at all | `PostQueuedCompletionStatus` with a sentinel key, no pipe/fd at all |
| Async file I/O | not supported (`XTC_E_NOSYS`; offload to the blocking pool) | `IORING_OP_READ`/`WRITE`/`READV`/`WRITEV`/`FSYNC`, tag's low bit set to distinguish from a poll CQE | `POSIX aio_read`/`aio_write` completing via `SIGEV_KEVENT` as `EVFILT_AIO` (FreeBSD/DragonFly only; macOS offloads) | native overlapped `ReadFile`/`WriteFile` on the same port; `FlushFileBuffers` has no async form so fsync offloads |

All four backends report through the SAME `xtc_io_event_t { void *tag;
uint32_t flags; }` shape (`src/inc/xtc_io.h`), and `flags` is always
one of `XTC_IO_READABLE`, `XTC_IO_WRITABLE`, `XTC_IO_HUP`,
`XTC_IO_ERR`, `XTC_IO_WAKEUP`, or `XTC_IO_AIO` -- the whole point of
L1 is that L2 (`src/evt/loop.c`) never branches on which backend is
compiled in.

---

## See also

  * [`xtc_io(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_io.3) -- the L1 API manual page.
  * [`xtc_aio(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_aio.3) -- async file I/O across the same four backends.
  * `docs/ARCHITECTURE.md` -- the layer model and the configure-time
    backend-selection rule.
  * `docs/KNOWN_ISSUES.md` -- the AFD async-completion driver bug this
    page's IOCP section summarizes, in full.
