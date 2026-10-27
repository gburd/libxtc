---
title: Zero copy in libxtc
parent: Guide
nav_order: 13
permalink: /guide/13-zero-copy/
lede: >-
  How to move bytes through messages, sockets, TLS, files and devices
  without the library copying them -- and what it still copies, and why.
---

# Zero copy in libxtc

1. TOC
{:toc}

---

A runtime that sits between your code and the kernel has many chances
to copy your data: into a message envelope, out of it again, into a
framing buffer, into a TLS record, into an I/O bounce buffer.  libxtc's
rule is that **used right, it does not force a copy on any data path**.
Where a convenient API copies (because it takes a `const` buffer that
stays yours), there is a sibling that moves or borrows the buffer
instead.  This page lists, path by path, how to move bytes without a
copy, and what the library still copies and why.

"Copy" here means a user-space `memcpy` of the payload.  The kernel's
copy between a user buffer and a socket is the price of a socket; the
only ways around it are kernel TLS + `sendfile` (below) and, for the
socket itself, `MSG_ZEROCOPY`-style APIs that libxtc does not wrap.

## Messages between processes

`xtc_send(to, data, size)` takes a `const` buffer that stays the
caller's, so it must copy.  Small messages (up to 256 bytes) travel
inline in a pooled envelope and are copied out again at `xtc_recv`;
larger ones are copied **once**, into a block that `xtc_recv` hands
over as is.

`xtc_send_owned(to, buf, size)` **moves** a buffer instead: zero copies
at any size, same loop or across threads.  The buffer must come from
libxtc's allocator (`xtc_malloc`, or a block libxtc gave you to free
with `xtc_free`, such as one from `xtc_recv`), because the receiver
frees it with `xtc_free`.  On `XTC_OK` it belongs to the receiver; on
**any** error it is still yours, unchanged -- free it or retry, the
same backpressure contract as `xtc_send`.

{% include snippet.html file="17_zero_copy_msg.c" region="full" %}

Forwarding is free: a block from `xtc_recv` can go straight back out
with `xtc_send_owned`.  A received network frame
(`xtc_net_recv_frame`) is an `xtc_free`-able block too, so a pump that
reads frames and hands them to a process moves them without a copy --
libxtc's own cross-process child pump does exactly that.

Measured (one loop, producer writing every byte, 16-vCPU x86-64 host):
moving instead of copying is 4.2x the message rate at 4 KiB, 10x at
64 KiB and 7.9x at 1 MiB; at 64 bytes the two are the same, so keep
`xtc_send` for small messages.

**Channels** (`xtc_chan_*`) carry `void *`: they never copy.  The
lifetime of what the pointer points at is yours.

**gen_server / gen_statem calls** (`xtc_svr_call`, `xtc_fsm_call`)
copy each direction once: the request (or reply) and its routing
header must become one message, and the payload you pass is `const`.
The frame is then moved, and the caller is handed the very block the
server built.  (Before the zero-copy work a call round trip copied the
payload seven times.)

**Process groups** (`xtc_pg_send`) send one copy per member: a
fan-out of a `const` buffer.  Zero-copy fan-out would need shared,
reference-counted, immutable messages with their own release call --
deliberately not added; if you fan out large payloads, send each member
an `xtc_send_owned` block, or share one immutable buffer by pointer over
a channel and count references yourself.

## Sockets and framing

`xtc_net_send_frame(fd, buf, len)` writes the 4-byte length prefix and
the payload in **one** `sendmsg` from your buffer.  To send a frame
built from several pieces (a header struct, a body that lives
elsewhere, a trailer), use `xtc_net_send_framev(fd, iov, n)`: up to 63
pieces, one frame on the wire, no assembly buffer.

`xtc_net_recv_frame` reads the prefix, allocates the frame once, and
receives the payload straight into it; the block is yours (`xtc_free`)
and can be moved on with `xtc_send_owned`.  UDP (`xtc_net_udp_*`) and
the credentials calls take your buffers as they are.

## TLS

By default, TLS is done in user space: your plaintext is copied into the
TLS library's record buffer, encrypted, and written to the socket.

`xtc_tls_enable_ktls(tls)`, called before the handshake on an fd-mode
connection, asks for **kernel TLS** (Linux `tls.ko`, FreeBSD KTLS;
OpenSSL backend).  After the handshake `xtc_tls_ktls_active(tls)` says
whether the kernel took the transmit (`XTC_TLS_KTLS_TX`) and receive
(`XTC_TLS_KTLS_RX`) sides; if it declined (module not loaded, a cipher
it does not do), the connection is an ordinary user-space TLS
connection and nothing else changes.

With kernel TX active, `xtc_tls_sendfile(tls, fd, off, len, &n)` sends a
file region encrypted by the kernel; the file's pages never enter user
space.  Without it the call returns `XTC_E_NOSYS`, which is your cue to
`pread` + `xtc_tls_write`:

{% include snippet.html file="18_tls_sendfile.c" region="full" %}

Measured on a 32-vCPU x86-64 host (Linux 6.12, OpenSSL 3.5.7, AES-GCM,
loopback, sender and receiver on two pinned cores, 256 KiB writes, 5
runs interleaved, median), serving a file:

| mode | MiB/s | sender CPU s/GiB |
|---|---|---|
| user-space TLS, `pread` + `xtc_tls_write` | 1837 | 0.556 |
| kernel TLS, `pread` + `xtc_tls_write` | 1593 | 0.639 |
| kernel TLS, `xtc_tls_sendfile` | 2106 | 0.486 |

With kernel TX active, `xtc_tls_write` hands your whole buffer to the
socket in one `write(2)` and the kernel cuts it into records (before
2.0 it went through OpenSSL, which writes one 16 KiB record per system
call: 1329 MiB/s here).  Kernel TLS for plain writes is still below
user-space TLS on this host: a `write(2)` of the same plaintext to the
kernel-TLS socket without libxtc or OpenSSL measures the same, so the
rest is the kernel's copy-and-encrypt path.  Enable kernel TLS for
connections that mostly serve files with `xtc_tls_sendfile`, not as a
blanket default, and measure on your hardware.

Two rules for a kernel-TLS connection.  Close its socket with
`xtc_net_close`, not `close(2)`: on Linux the kernel flushes a queued
record at close and, if the peer has gone, raises SIGPIPE in the
closing thread; `xtc_net_close` holds that off.  And a write to a peer
that has gone returns `XTC_E_IO` even though the kernel itself reports
the bytes as accepted.

A connection on a custom transport (`xtc_tls_create_transport`) has no
socket to hand the kernel, so it always does TLS in user space.

## Files and block devices

`xtc_aio_pread / pwrite / preadv / pwritev` pass your buffer straight
to the kernel (an io_uring SQE, a syscall on the blocking pool, or the
simulator's inline call): no bounce buffer.  For `O_DIRECT`, the rules
for your buffer's address, file offset and length come from
`xtc_fs_dio_align(fd, &mem, &off, &len)`, and `xtc_fs_dio_alloc` gives
you a buffer that meets them.  `xtc_bdev_pread / pwrite` likewise.

libxtc does not (yet) expose io_uring registered buffers or
`SEND_ZC`; the payload is never copied by libxtc either way.

## Accelerators

`xtc_batch` stores only the `void *` you submit; `xtc_accel_wait_fence`
waits on a fence or `sync_file` fd; `xtc_accel_run_blocking` runs your
function off the loop.  libxtc never touches device memory, so how
bytes reach a GPU (pinned host memory, mapped or unified memory,
DMA-BUF) is entirely the driver's and your choice.  The batching
examples show both styles: one reads results back with a copy
(`clEnqueueReadBuffer`), another maps the buffer.

## What libxtc still copies, and why

| what | copies | why |
|---|---|---|
| `xtc_send` <= 256 B | 2 (in, out) | inline pooled envelope; at this size the copy costs about the same as moving (measured) |
| `xtc_send` > 256 B | 1 | the buffer is `const` and stays yours; use `xtc_send_owned` |
| `xtc_svr_call` / `xtc_fsm_call` | 1 per direction | header + `const` payload become one message |
| `xtc_pg_send` | 1 per member | fan-out of a `const` buffer |
| `xtc_log_write` | 1 | formatting is the copy |
| user-space TLS | record copy + crypto | use kernel TLS + `xtc_tls_sendfile` where it pays |

The examples follow the same rules.  `examples/07_kaka` moves produced
records into the partition with `xtc_send_owned`.  `examples/08_tnt`
echoes with `xtc_tnt_recv_take` (keep the reactor's receive buffer)
and `xtc_tnt_io_send_owned` (send it, the runtime frees it after), so
the echoed bytes are never copied into the Isolate.  The copies left in
the examples are their own choices: rexis copies a value into its reply
buffer so the reply stays valid after the read lock is dropped; the
sqlxtc WAL copies records into its ring because the ring *is* the group
commit (many committers' records leave in one write).

## Alternatives not chosen

- **Reference-counted messages for every send.**  It would make fan-out
  free but add an atomic per message and a release call that is not
  `xtc_free` to every receiver -- a new receive contract for one use.
- **Receive into a caller buffer** (`xtc_recv_into`).  The envelope for
  a large message already *is* the receiver's buffer; a receive-into
  would add the copy back.
- **Kernel TLS on by default.**  Measured slower for plain writes on a
  host whose CPU has fast user-space AES (0.87x); it pays for `sendfile`.
