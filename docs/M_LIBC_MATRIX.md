---
title: libc matrix
parent: Reference
nav_order: 13
lede: >-
  Supported libc implementations and their quirks.
permalink: /reference/libc-matrix/
---
xtc is C11.  The OS layer (m0/m1) is portable across libc
implementations; the higher layers depend on POSIX-extension surface
area that varies.  This document records what's been verified.

## Tested matrix

Configuration: a fresh out-of-source build per libc / platform.

| libc / platform | Version | OS layer | Full suite | Notes |
|-----------------|---------|:--------:|:----------:|-------|
| glibc (Linux x86_64) | 2.40 | pass | pass | Canonical build; runtime-verified every commit in CI (gcc + clang, ASan, UBSan) |
| musl (Linux x86_64) | 1.2.5 | pass | pass (forced-fcontext CI) | musl omits ucontext; the hand-written fcontext substrate covers it and the forced-fcontext CI job runs the suite every commit |
| FreeBSD libc (15.0, clang, amd64) | 15.0 | pass | pass (gmake check) | Native kqueue file-AIO exercised |
| illumos libc (SunOS 5.11, gcc, UltraSPARC v9) | illumos-31d3d510d0 | pass | pass (gmake check) | Big-endian sparcv9 -- validates byte order in codecs/hashing/atomics; event-ports backend, OpenSSL 3 |
| macOS libSystem (Apple Silicon, clang) | -- | pass | pass | kqueue + ucontext; runtime-verified every commit in CI |
| Windows ucrt (MinGW gcc 13.2) | UCRT | pass | IOCP runtime-verified on a host | loop/task/timer/wakeup/socket-poll/file-AIO; per-commit Windows CI is an MSVC xtc.lib + smoke build |
| Windows MSVC (cl) | 14.x | builds (xtc.lib) | smoke | The munit harness uses GCC-isms cl rejects; a standalone smoke test stands in |

## glibc

The reference build.  All 279 munit + 23 PBT + 22 shell tests
pass on Linux x86_64.

## musl

`musl-gcc` and the musl runtime build libxtc.a end-to-end with no
warnings on `-Wall -Wextra -Wpedantic`, and the full stack -- OS
layer through coroutines, processes, supervisors, and gen_server --
links and runs.

The OS layer (`__os_alloc`, `__os_atomic`, `__os_mutex`,
`__os_thread`, `__os_time`, `__os_tls`) passes 33/33.  The
coroutine-dependent suites (async, proc, proc_wait_fd, sync, sup,
svr, gen_server, supervisor, exec, steal, channels, the locks, slab)
all pass as well.  The only tests that do not build on musl are the
two that use POSIX/Linux-specific surface unrelated to the runtime
(test_net_udp, test_slab_shm) and the TLS handshake tests (built
only with a TLS backend).

### How musl is supported: the fcontext substrate

musl deliberately omits the System-V ucontext API (swapcontext /
getcontext / makecontext); the musl maintainers consider it obsolete
and bug-prone.  Glibc, FreeBSD, OpenBSD, and illumos all ship it.
configure.ac probes for it:

    AC_CHECK_FUNC([swapcontext],
      [AC_DEFINE([XTC_HAVE_UCONTEXT], 1, ...)],
      [...])

The coroutine substrate has two interchangeable implementations,
selected at compile time, presenting an identical surface
(`xtc_async`, `__xtc_coro_step`, `xtc_await`, `xtc_yield`,
`xtc_stack_size`):

  * `src/evt/coro_uctx.c` -- the ucontext implementation, used when
    `XTC_HAVE_UCONTEXT` is defined (glibc, the BSDs, illumos).
  * `src/evt/coro_fctx.c` -- built on the hand-written
    `make_fcontext` / `jump_fcontext` assembly in
    `src/os/asm/fctx_x86_64_sysv.S`, used when ucontext is absent
    (musl).  A coroutine's resume point is a single saved stack
    pointer; the scheduler's return point is a per-thread cursor.

Exactly one of the two compiles to live code; the other becomes an
empty translation unit.  Both are always in the source list, so no
build-system branching is needed beyond the configure probe.

The fcontext path can be forced on a glibc host with
`-DXTC_CORO_FORCE_FCTX`, which is how it is exercised under
AddressSanitizer without a musl toolchain: the full 283-assertion
suite passes and is ASan/UBSan clean on the forced fcontext build,
matching the ucontext build exactly.

Estimated effort: 1-2 days.  Tracked in
[PLAN.md](https://codeberg.org/gregburd/libxtc/src/branch/main/PLAN.md) under the libc-matrix work item.  Not
blocking any user shipping libxtc on glibc / FreeBSD / illumos
hosts (the entire production target set so far).

## Reproducing

glibc (default):

    cd build_unix
    /home/gburd/ws/xtc/dist/configure --with-tls=auto
    make -j$(nproc)
    make check

musl:

    mkdir -p /tmp/xtc_musl_build && cd /tmp/xtc_musl_build
    nix-shell -p musl --command \
        "CC=musl-gcc CFLAGS='-O2 -g' LDFLAGS='-static' \
         /home/gburd/ws/xtc/dist/configure --with-tls=none && \
         make -j4 libxtc.a"
    # libxtc.a builds clean; OS-layer tests link and run:
    nix-shell -p musl --command \
        "make test_alloc test_atomic test_mutex test_thread test_time test_tls && \
         for t in ./test_alloc ./test_atomic ./test_mutex ./test_thread ./test_time ./test_tls; do \$t; done"
