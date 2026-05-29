---
title: "libc matrix"
---

# libc matrix

xtc is C11.  The OS layer (m0/m1) is portable across libc
implementations; the higher layers depend on POSIX-extension surface
area that varies.  This document records what's been verified.

## Tested matrix

Configuration: a fresh out-of-source build per libc, with
`--with-tls=none` and `--without-liburing` (where the dev headers
aren't present in the test shell).

| libc    | Version  | OS layer (m0/m1) | Higher layers | Notes |
|---------|----------|:----------------:|:-------------:|-------|
| glibc   | 2.40     |     33/33        |    266/266    | Default; the canonical Linux build |
| musl    | 1.2.5    |     33/33        |    n/a        | Builds libxtc.a clean.  Higher-layer link fails because musl deliberately omits ucontext (swapcontext / getcontext / makecontext) |
| MSVC    | -        |     untested     |     -         | Windows MinGW ucrt covered by the existing Windows build path; native MSVC not yet attempted |

## glibc

The reference build.  All 279 munit + 23 PBT + 22 shell tests
pass on Linux x86_64.

## musl

`musl-gcc` and the musl runtime build libxtc.a end-to-end with no
warnings on `-Wall -Wextra -Wpedantic`.  The OS layer (`__os_alloc`,
`__os_atomic`, `__os_mutex`, `__os_thread`, `__os_time`, `__os_tls`)
links and tests cleanly:

    test_alloc   8/8
    test_atomic  7/7
    test_mutex   6/6
    test_thread  6/6
    test_time    3/3
    test_tls     3/3
    -- 33/33

Higher-layer link fails:

    coro_uctx.c: undefined reference to swapcontext / getcontext /
                 makecontext

This is by design on musl's part.  The musl maintainers consider
the System-V ucontext API obsolete and bug-prone (see musl FAQ),
and they don't ship implementations.  Glibc, FreeBSD, OpenBSD, and
illumos all do.

xtc detects this in configure.ac:

    AC_CHECK_FUNC([swapcontext],
      [AC_DEFINE([XTC_HAVE_UCONTEXT], 1, ...)],
      [...])

When the symbol is absent, `coro_uctx.c` becomes a no-op
translation unit (preserving `libxtc.a`'s portability) but the
M3+ event-loop code that drives coroutines doesn't have a
substrate to call into, so M4-and-up tests don't link.

### Resolving musl

xtc already has `make_fcontext` / `jump_fcontext` assembly in
`src/os/asm/fctx_x86_64_sysv.S` (and a Windows variant).  The
work to enable musl is to add `src/evt/coro_fctx.c`: a coroutine
substrate that maps the same xtc_task_t -> fiber surface that
`coro_uctx.c` currently provides, but built on the assembly
primitives instead of ucontext.

Estimated effort: 1-2 days.  Tracked in
[PLAN.md](../PLAN.md) under the libc-matrix work item.  Not
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
