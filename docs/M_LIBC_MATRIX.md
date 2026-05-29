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
