---
title: Home
layout: default
nav_order: 1
---

# libxtc
{: .fs-9 }

Asynchronous concurrency for C, in the tradition of Tokio, Seastar, and
the BEAM: fibers, an event loop, lightweight processes with links and
monitors, supervisors, and deterministic simulation testing.
{: .fs-6 .fw-300 }

[Get started](guide/01-getting-started){: .btn .btn-primary .fs-5 .mb-4 .mb-md-0 .mr-2 }
[API reference](https://codeberg.org/gregburd/libxtc/src/branch/main/man){: .btn .fs-5 .mb-4 .mb-md-0 }

---

libxtc gives a C program the concurrency model that made Go, Rust/Tokio,
Seastar, and Erlang/OTP productive -- without asking you to leave C.
You write ordinary straight-line functions; libxtc runs them on fibers
over an event loop, so a call that would block instead *yields*, and one
OS thread drives thousands of concurrent activities. On top of that sit
Erlang-style *processes* that own their state and communicate only by
message, with links, monitors, and supervisors for failure handling.

This manual is written to be read front to back the first time -- like
an O'Reilly guide -- and used as a reference afterward, like the
BerkeleyDB manual. Every code block on these pages is a real file under
[`docs/_includes/snippets/`](https://codeberg.org/gregburd/libxtc/src/branch/main/docs/snippets)
that is compiled and run as part of the test suite, so nothing you copy
from here can silently rot against the API.

## The book

### Part I -- Guide (read in order)

1. [Getting started](guide/01-getting-started) -- install, build, and
   run your first coroutine; the anatomy of a libxtc program.
2. [Fibers and the event loop](guide/02-fibers-and-the-loop) -- how
   `xtc_async`, `xtc_yield`, and `xtc_await` actually work, and why
   a fiber is not a thread.
3. [Processes and messages](guide/03-processes-and-messages) -- spawn,
   send, receive; the shared-nothing discipline.
4. [Links, monitors, and supervisors](guide/04-supervision) -- letting
   things crash, and cleaning up when they do.
5. [Blocking work and I/O](guide/05-blocking-and-io) -- files, sockets,
   timers, and how to call a blocking C API without stalling the loop.
6. [Thinking in libxtc](guide/transitioning) -- the mental shifts for a
   C/C++/Rust programmer, and the anti-patterns that bite.

### Part II -- Reference

- [Architecture and the layer model](ARCHITECTURE) -- L0 through L5 and
  what each layer owns.
- [The public API](API) -- the shape of the `xtc_*` surface.
- [Locks and synchronization](locks) -- which primitive to reach for.
- [Manual pages](reference/man-pages) -- every `xtc_*` function, by
  section.
- [Debugging and observing](guide/debugging) -- GDB/LLDB recipes and
  runtime introspection.
- [ABI stability](abi-stability) -- what stays fixed across releases.
- [Known issues](KNOWN_ISSUES) -- honest caveats and workarounds.

### Part III -- The example programs

The [`examples/`](examples/) directory ships whole programs -- a Redis
work-alike, a Kafka-shaped log broker, a from-scratch SQL engine -- each
built on libxtc. The [Examples](examples/) section explains what each
one is, the design decisions behind it, and the trade-offs it makes.

### Part IV -- Philosophy

- [Why libxtc exists](philosophy/why) -- the problem, and the shape of
  the answer.
- [Choices and roads not taken](philosophy/choices) -- the alternatives
  we deliberately did not choose, and why.

## Build and platform matrices

- [Windows toolchains](M_WINDOWS_MATRIX)
- [TLS backends](M_TLS_MATRIX)
- [libc implementations](M_LIBC_MATRIX)

## Source and license

- Git: <https://codeberg.org/gregburd/libxtc> (mirror:
  <https://github.com/gburd/libxtc>)
- License: ISC. See
  [LICENSE](https://codeberg.org/gregburd/libxtc/src/branch/main/LICENSE).
