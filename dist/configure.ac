# XTC -- Process this file with autoconf to produce a configure script.
#
# Copyright (c) 2026, The XTC Project
# Use of this source code is governed by the ISC License.

AC_INIT([xtc],
	m4_esyscmd_s([cat ../dist/version.in 2>/dev/null || echo 0.0.0]),
	[https://github.com/gburd/xtc/issues],
	[xtc])
AC_CONFIG_SRCDIR([../src/xtc_version.c])
AC_CONFIG_AUX_DIR([.])
AC_CONFIG_HEADERS([xtc_config.h:../dist/xtc_config.h.in])

AC_LANG([C])
AC_CANONICAL_HOST

# -------------------------------------------------------------------------
# Out-of-source build is mandatory.  See M0_CLAIMS.md [B3,B6].
# -------------------------------------------------------------------------
AC_MSG_CHECKING([for an out-of-source build])
abs_srcdir=`(cd "$srcdir" && pwd)`
abs_projroot=`(cd "$srcdir/.." && pwd)`
abs_builddir=`pwd`
case "$abs_builddir" in
  "$abs_projroot") AC_MSG_ERROR([dnl
xtc must not be configured in the source root.
Create a build directory and run configure from inside it, e.g.:
    mkdir build_unix && cd build_unix && ../dist/configure])
    ;;
  "$abs_srcdir") AC_MSG_ERROR([dnl
xtc must not be configured inside dist/.  Create a sibling
directory such as build_unix/ and run ../dist/configure from there.])
    ;;
esac
AC_MSG_RESULT([$abs_builddir])

# -------------------------------------------------------------------------
# Version components.  Parsed from dist/version.in (single source of truth).
# -------------------------------------------------------------------------
xtc_version_full=`cat "$srcdir/version.in"`
xtc_version_major=`echo "$xtc_version_full" | sed -e 's/^\([[0-9]]*\).*/\1/'`
xtc_version_minor=`echo "$xtc_version_full" | sed -e 's/^[[0-9]]*\.\([[0-9]]*\).*/\1/'`
xtc_version_patch=`echo "$xtc_version_full" | sed -e 's/^[[0-9]]*\.[[0-9]]*\.\([[0-9]]*\).*/\1/'`
AC_SUBST([XTC_VERSION_FULL],  [$xtc_version_full])
AC_SUBST([XTC_VERSION_MAJOR], [$xtc_version_major])
AC_SUBST([XTC_VERSION_MINOR], [$xtc_version_minor])
AC_SUBST([XTC_VERSION_PATCH], [$xtc_version_patch])

AC_DEFINE_UNQUOTED([XTC_VERSION_STRING],
	["$xtc_version_full"], [Library version as SemVer string])
AC_DEFINE_UNQUOTED([XTC_VERSION_MAJOR],
	[$xtc_version_major], [SemVer major])
AC_DEFINE_UNQUOTED([XTC_VERSION_MINOR],
	[$xtc_version_minor], [SemVer minor])
AC_DEFINE_UNQUOTED([XTC_VERSION_PATCH],
	[$xtc_version_patch], [SemVer patch])

# -------------------------------------------------------------------------
# Toolchain.
# -------------------------------------------------------------------------
AC_PROG_CC
AC_PROG_INSTALL
AC_PROG_RANLIB
AC_PROG_AWK

# -------------------------------------------------------------------------
# Optional shared library.  Default off so the historical static-only
# build keeps working unchanged; --enable-shared additionally builds and
# installs libxtc.so with the dist/libxtc.map version script.
# -------------------------------------------------------------------------
AC_ARG_ENABLE([shared],
  [AS_HELP_STRING([--enable-shared],
    [Also build and install a shared libxtc.so (default: no).])],
  [xtc_enable_shared=$enableval], [xtc_enable_shared=no])
AC_MSG_CHECKING([whether to build a shared library])
AC_MSG_RESULT([$xtc_enable_shared])
AC_SUBST([XTC_ENABLE_SHARED], [$xtc_enable_shared])

# Diagnostic build: enables runtime invariant checks that abort on
# violation (e.g. unaligned direct I/O).  Off by default; for
# development and CI hardening, not production.
AC_ARG_ENABLE([diagnostic],
  [AS_HELP_STRING([--enable-diagnostic],
    [Enable runtime DIAGNOSTIC assertions (aborts on violations; default: no).])],
  [xtc_enable_diagnostic=$enableval], [xtc_enable_diagnostic=no])
AC_MSG_CHECKING([whether to enable diagnostic assertions])
AC_MSG_RESULT([$xtc_enable_diagnostic])
AS_IF([test "x$xtc_enable_diagnostic" = xyes],
  [AC_DEFINE([XTC_DIAGNOSTIC], 1,
    [Define to enable runtime diagnostic assertions.])])

# pthread
AC_SEARCH_LIBS([pthread_create], [pthread], [],
  [AC_MSG_ERROR([xtc requires pthreads])])
# librt for clock_gettime on older glibcs (no-op on modern ones).
AC_SEARCH_LIBS([clock_gettime], [rt], [], [])
# timer_create / timer_settime / timer_delete (the per-thread preemption
# timer in preempt.c) live in librt on FreeBSD and older glibc, in libc
# on modern glibc.  Find them wherever they are so the link does not
# hardcode -lrt (and does not need it where they are in libc).
AC_SEARCH_LIBS([timer_create], [rt], [], [])
# shm_open lives in librt on older glibc, libc on modern glibc / *BSD,
# and libc on macOS (no librt at all).  Find it wherever it is so the
# shm test does not hardcode -lrt.
AC_SEARCH_LIBS([shm_open], [rt], [], [])

# -------------------------------------------------------------------------
# L1 I/O backend selection.
# -------------------------------------------------------------------------
AC_ARG_WITH([io-backend],
  [AS_HELP_STRING([--with-io-backend=auto|poll|epoll|uring|kqueue|iocp|solaris|aix|sim],
    [Pick the L1 I/O backend (default: auto).])],
  [xtc_io_backend=$withval], [xtc_io_backend=auto])

AC_ARG_WITH([liburing],
  [AS_HELP_STRING([--with-liburing=PATH],
    [Path to a liburing install (overrides pkg-config search).])],
  [xtc_with_liburing=$withval], [xtc_with_liburing=auto])

# DPDK: OPTIONAL userspace-networking dependency.  Default OFF -- DPDK
# is a heavyweight L2 poll-mode-driver stack (EAL, hugepages) only worth
# pulling in for a dedicated userspace-networking deployment.  This
# probe just DETECTS libdpdk via pkg-config and defines XTC_HAVE_DPDK so
# a future DPDK I/O backend can be wired; the
# default build neither needs nor links it.  --with-dpdk=yes requires it.
AC_ARG_WITH([dpdk],
  [AS_HELP_STRING([--with-dpdk=auto|yes|no|PATH],
    [Detect DPDK for the optional userspace-networking backend (default: no).])],
  [xtc_with_dpdk=$withval], [xtc_with_dpdk=no])
have_dpdk=no
dpdk_cflags=
dpdk_libs=
AS_IF([test "x$xtc_with_dpdk" != xno],
  [AC_PATH_PROG([PKG_CONFIG], [pkg-config], [no])
   AS_IF([test "x$xtc_with_dpdk" = xauto || test "x$xtc_with_dpdk" = xyes],
     [AS_IF([test "x$PKG_CONFIG" != xno && $PKG_CONFIG --exists libdpdk 2>/dev/null],
        [have_dpdk=yes
         dpdk_cflags=`$PKG_CONFIG --cflags libdpdk`
         dpdk_libs=`$PKG_CONFIG --libs libdpdk`],
        [AS_IF([test "x$xtc_with_dpdk" = xyes],
          [AC_MSG_ERROR([--with-dpdk=yes: libdpdk not found via pkg-config])])])],
     [# Explicit PATH to a DPDK prefix.
      AS_IF([test -f "$xtc_with_dpdk/lib/pkgconfig/libdpdk.pc"],
        [PKG_CONFIG_PATH="$xtc_with_dpdk/lib/pkgconfig:$PKG_CONFIG_PATH"
         export PKG_CONFIG_PATH
         have_dpdk=yes
         dpdk_cflags=`pkg-config --cflags libdpdk 2>/dev/null`
         dpdk_libs=`pkg-config --libs libdpdk 2>/dev/null`],
        [AC_MSG_ERROR([--with-dpdk=$xtc_with_dpdk: libdpdk.pc not found])])])])
AS_IF([test "x$have_dpdk" = xyes],
  [AC_DEFINE([XTC_HAVE_DPDK], 1,
     [Define to 1 if DPDK (libdpdk) is available for the optional
      userspace-networking backend])])
AC_SUBST([XTC_DPDK_CFLAGS], [$dpdk_cflags])
AC_SUBST([XTC_DPDK_LIBS],   [$dpdk_libs])

have_epoll=no
have_kqueue=no
have_uring=no
uring_cflags=
uring_libs=
AC_CHECK_HEADER([sys/epoll.h], [have_epoll=yes], [have_epoll=no])
AC_CHECK_HEADER([sys/event.h], [have_kqueue=yes], [have_kqueue=no])

# execinfo (backtrace/backtrace_symbols_fd) for native stack dumps in
# the panic/abort facility.  glibc, macOS, and the BSDs have it; musl
# does not.  The backtrace seam then falls back to libunwind (below)
# and finally to a no-op stub.
AC_CHECK_HEADER([execinfo.h],
  [AC_CHECK_FUNC([backtrace],
    [AC_DEFINE([XTC_HAVE_EXECINFO], 1,
       [Define to 1 if <execinfo.h> backtrace() is available])
     xtc_have_execinfo=yes])],
  [xtc_have_execinfo=no])

# Builtin unwinder (_Unwind_Backtrace from <unwind.h>).  This is the
# C++ EH-personality ABI walker provided by libgcc_s / LLVM's unwinder
# and AUTO-LINKED -- no extra -l flag.  It is the same unwinder glibc's
# backtrace() uses internally, but needs no <execinfo.h>, so it covers
# musl (and any execinfo-less target) WITHOUT a third-party libunwind
# dependency.  Probed with a compile+link test; sits ABOVE libunwind in
# priority so libunwind is demoted to an explicit opt-in.
AC_MSG_CHECKING([for _Unwind_Backtrace])
xtc_have_builtin_unwind=no
AC_LINK_IFELSE(
  [AC_LANG_PROGRAM([[#include <unwind.h>
     static _Unwind_Reason_Code cb(struct _Unwind_Context *c, void *a)
     { (void)a; (void)_Unwind_GetIP(c); return _URC_NO_REASON; }]],
    [[(void)_Unwind_Backtrace(cb, 0);]])],
  [xtc_have_builtin_unwind=yes])
AC_MSG_RESULT([$xtc_have_builtin_unwind])
# Note: a third-party libunwind installed at configure time can ship a
# <unwind.h> that omits _Unwind_Backtrace, making this probe report no
# even where the compiler's own <unwind.h> would work.  That is benign:
# on such a system libunwind is present and simply becomes the active
# backend instead.  The case this tier exists for -- a stock musl build
# with NO libunwind -- finds the compiler's <unwind.h> and reports yes.

# libunwind backtrace backend.  OPTIONAL and now DEMOTED to a fallback
# below the builtin _Unwind_Backtrace tier: it is used only when an
# execinfo-less target ALSO lacks a working _Unwind_Backtrace, or when
# the user explicitly asks for it with --with-libunwind=yes.  We still
# probe via pkg-config first (Nix-style split outputs) then a plain link
# test.  dladdr() (libdl / libc) symbolizes the addresses best-effort;
# without it the libunwind path emits raw frame addresses only.
AC_ARG_WITH([libunwind],
  [AS_HELP_STRING([--with-libunwind=auto|yes|no|PATH],
    [Use the third-party libunwind for stack backtraces.  Default auto:
     libunwind is used only when both execinfo and the builtin
     _Unwind_Backtrace are unavailable.  yes forces it above the
     builtin tier (but still below execinfo).])],
  [xtc_with_libunwind=$withval], [xtc_with_libunwind=auto])

have_libunwind=no
libunwind_cflags=
libunwind_libs=
# Under auto, do not even probe libunwind when a higher-priority backend
# (execinfo or the builtin unwinder) is present -- it would never be the
# active backend.  An explicit --with-libunwind=yes|PATH always probes.
xtc_probe_libunwind=no
AS_IF([test "x$xtc_with_libunwind" = xyes],
  [xtc_probe_libunwind=yes],
  [AS_IF([test "x$xtc_with_libunwind" != xno && test "x$xtc_with_libunwind" != xauto],
     [xtc_probe_libunwind=yes],
     [AS_IF([test "x$xtc_with_libunwind" = xauto && test "x$xtc_have_execinfo" != xyes && test "x$xtc_have_builtin_unwind" != xyes],
        [xtc_probe_libunwind=yes])])])
AS_IF([test "x$xtc_probe_libunwind" = xyes],
  [AS_IF([test "x$xtc_with_libunwind" = xauto || test "x$xtc_with_libunwind" = xyes],
     [AC_PATH_PROG([PKG_CONFIG], [pkg-config], [no])
      AS_IF([test "x$PKG_CONFIG" != xno && $PKG_CONFIG --exists libunwind 2>/dev/null],
        [have_libunwind=yes
         libunwind_cflags=`$PKG_CONFIG --cflags libunwind`
         libunwind_libs=`$PKG_CONFIG --libs libunwind`],
        [ac_save_LIBS=$LIBS
         AC_CHECK_HEADER([libunwind.h],
           [AC_CHECK_LIB([unwind], [unw_backtrace],
              [have_libunwind=yes
               libunwind_libs="-lunwind"], [have_libunwind=no])])
         LIBS=$ac_save_LIBS])],
     [# Explicit PATH to a libunwind prefix.
      AS_IF([test -f "$xtc_with_libunwind/lib/pkgconfig/libunwind.pc"],
        [PKG_CONFIG_PATH="$xtc_with_libunwind/lib/pkgconfig:$PKG_CONFIG_PATH"
         export PKG_CONFIG_PATH
         have_libunwind=yes
         libunwind_cflags=`pkg-config --cflags libunwind 2>/dev/null`
         libunwind_libs=`pkg-config --libs libunwind 2>/dev/null`],
        [AS_IF([test -f "$xtc_with_libunwind/include/libunwind.h"],
          [have_libunwind=yes
           libunwind_cflags="-I$xtc_with_libunwind/include"
           libunwind_libs="-L$xtc_with_libunwind/lib -Wl,-rpath,$xtc_with_libunwind/lib -lunwind"],
          [AC_MSG_ERROR([--with-libunwind=$xtc_with_libunwind: libunwind.h not found])])])])])

# Backend selection.  Priority:
#   1. execinfo (when present, always wins -- symbolized, signal-safe).
#   2. libunwind ONLY when forced with --with-libunwind=yes (a user who
#      explicitly wants the richer third-party unwinder over the builtin
#      one), and execinfo is absent.
#   3. builtin _Unwind_Backtrace (the no-dependency musl fallback).
#   4. libunwind as the last resort (auto found it and nothing above is
#      available).
# This demotes libunwind to opt-in: a stock musl build now uses the
# builtin unwinder and needs no -lunwind.
xtc_use_libunwind=no
xtc_use_builtin_unwind=no
AS_IF([test "x$xtc_have_execinfo" = xyes],
  [: # execinfo is the active backend; nothing else to enable.
   libunwind_cflags=
   libunwind_libs=],
  [AS_IF([test "x$xtc_with_libunwind" = xyes && test "x$have_libunwind" = xyes],
     [xtc_use_libunwind=yes],
     [AS_IF([test "x$xtc_have_builtin_unwind" = xyes],
        [xtc_use_builtin_unwind=yes
         libunwind_cflags=
         libunwind_libs=],
        [AS_IF([test "x$have_libunwind" = xyes],
           [xtc_use_libunwind=yes],
           [libunwind_cflags=
            libunwind_libs=])])])])

AS_IF([test "x$xtc_use_libunwind" = xyes],
  [AC_DEFINE([XTC_HAVE_LIBUNWIND], 1,
     [Define to 1 to use the libunwind stack-backtrace backend])])
AS_IF([test "x$xtc_use_builtin_unwind" = xyes],
  [AC_DEFINE([XTC_HAVE_BUILTIN_UNWIND], 1,
     [Define to 1 to use the builtin _Unwind_Backtrace backend])])

# dladdr() for best-effort symbolization on the unwind paths (libunwind
# OR builtin).  It is in libc on glibc/musl and in libdl on some
# systems; only matters when an unwind backend is the active one.
AS_IF([test "x$xtc_use_libunwind" = xyes || test "x$xtc_use_builtin_unwind" = xyes],
  [AC_SEARCH_LIBS([dladdr], [dl],
     [AC_DEFINE([XTC_HAVE_DLADDR], 1,
        [Define to 1 if dladdr() is available for symbolization])], [])])

AC_SUBST([XTC_LIBUNWIND_CFLAGS], [$libunwind_cflags])
AC_SUBST([XTC_LIBUNWIND_LIBS],   [$libunwind_libs])

# musl libc deliberately omits the ucontext.h API (swapcontext et al.)
# which the default coroutine substrate uses.  Detect this so the
# build can fail loudly with a useful message rather than blowing
# up at link time.
AC_CHECK_FUNC([swapcontext],
  [AC_DEFINE([XTC_HAVE_UCONTEXT], 1,
     [Define to 1 if swapcontext / getcontext / makecontext are available])
   xtc_have_ucontext=yes],
  [xtc_have_ucontext=no])
AS_IF([test "x$xtc_with_liburing" != xno],
  [AS_IF([test "x$xtc_with_liburing" = xauto],
     [# Try pkg-config first; it knows about Nix-style split outputs.
      AC_PATH_PROG([PKG_CONFIG], [pkg-config], [no])
      AS_IF([test "x$PKG_CONFIG" != xno && $PKG_CONFIG --exists liburing 2>/dev/null],
        [have_uring=yes
         uring_cflags=`$PKG_CONFIG --cflags liburing`
         uring_libs=`$PKG_CONFIG --libs liburing`],
        [AC_CHECK_HEADER([liburing.h],
           [AC_CHECK_LIB([uring], [io_uring_queue_init],
              [have_uring=yes
               uring_libs="-luring"], [have_uring=no])])])],
     [AS_IF([test -f "$xtc_with_liburing/lib/pkgconfig/liburing.pc"],
        [have_uring=yes
         PKG_CONFIG_PATH="$xtc_with_liburing/lib/pkgconfig:$PKG_CONFIG_PATH"
         export PKG_CONFIG_PATH
         uring_cflags=`pkg-config --cflags liburing 2>/dev/null`
         uring_libs=`pkg-config --libs liburing 2>/dev/null`
         uring_libdir=`pkg-config --variable=libdir liburing 2>/dev/null`
         test -n "$uring_libdir" && uring_libs="$uring_libs -Wl,-rpath,$uring_libdir"],
        [AS_IF([test -f "$xtc_with_liburing/include/liburing.h"],
          [have_uring=yes
           uring_cflags="-I$xtc_with_liburing/include"
           uring_libs="-L$xtc_with_liburing/lib -Wl,-rpath,$xtc_with_liburing/lib -luring"],
          [AC_MSG_ERROR([--with-liburing=$xtc_with_liburing: liburing.h not found])])])])])

AC_MSG_CHECKING([for L1 I/O backend])
case "$xtc_io_backend" in
  auto)
    case "$host_os" in
      *mingw*|*cygwin*|*msys*) xtc_io_backend=iocp ;;
      *solaris*|*illumos*)     xtc_io_backend=solaris ;;
      *aix*)                   xtc_io_backend=aix ;;
      *)
        if test "$have_uring" = yes; then xtc_io_backend=uring
        elif test "$have_epoll" = yes; then xtc_io_backend=epoll
        elif test "$have_kqueue" = yes; then xtc_io_backend=kqueue
        else xtc_io_backend=poll
        fi
        ;;
    esac ;;
  uring)
    if test "$have_uring" != yes; then
      AC_MSG_ERROR([--with-io-backend=uring requires liburing (try --with-liburing=PATH)])
    fi ;;
  epoll)
    if test "$have_epoll" != yes; then
      AC_MSG_ERROR([--with-io-backend=epoll requires <sys/epoll.h>])
    fi ;;
  kqueue)
    if test "$have_kqueue" != yes; then
      AC_MSG_ERROR([--with-io-backend=kqueue requires <sys/event.h>])
    fi ;;
  poll|iocp|solaris|aix|select) ;;
  sim) ;;   # deterministic-simulation backend (DST); test-only, no platform deps
  *) AC_MSG_ERROR([invalid --with-io-backend=$xtc_io_backend]) ;;
esac
AC_MSG_RESULT([$xtc_io_backend])
AC_SUBST([XTC_IO_BACKEND], [$xtc_io_backend])
AC_SUBST([XTC_URING_CFLAGS], [$uring_cflags])
AC_SUBST([XTC_URING_LIBS],   [$uring_libs])

# Windows: the IOCP backend needs ws2_32 (Winsock).  We add it to LIBS
# unconditionally on mingw/cygwin since other backends won't be picked
# anyway.
xtc_extra_libs=
case "$host_os" in
  *mingw*|*cygwin*|*msys*)
    # ws2_32 for sockets, ntdll for the AFD poll fast path
    # (NtCreateFile / NtDeviceIoControlFile / NtCancelIoFileEx) in the
    # IOCP backend, and dbghelp for the DbgHelp backtrace backend
    # (CaptureStackBackTrace + SymFromAddr).
    xtc_extra_libs="-lws2_32 -lntdll -ldbghelp"
    ;;
  *solaris*|*illumos*)
    xtc_extra_libs="-lsocket -lnsl"
    ;;
esac
AC_SUBST([XTC_PLATFORM_LIBS], [$xtc_extra_libs])

AS_CASE([$xtc_io_backend],
  [epoll],   [AC_DEFINE([XTC_IO_BACKEND_EPOLL],   1, [Linux epoll backend])],
  [poll],    [AC_DEFINE([XTC_IO_BACKEND_POLL],    1, [POSIX poll backend])],
  [uring],   [AC_DEFINE([XTC_IO_BACKEND_URING],   1, [Linux io_uring backend])],
  [kqueue],  [AC_DEFINE([XTC_IO_BACKEND_KQUEUE],  1, [BSD/macOS kqueue backend])],
  [iocp],    [AC_DEFINE([XTC_IO_BACKEND_IOCP],    1, [Windows IOCP backend (stub)])],
  [solaris], [AC_DEFINE([XTC_IO_BACKEND_SOLARIS], 1, [Solaris event ports backend (stub)])],
  [aix],     [AC_DEFINE([XTC_IO_BACKEND_AIX],     1, [AIX pollset backend (stub)])],
  [select],  [AC_DEFINE([XTC_IO_BACKEND_SELECT],  1, [POSIX select(2) backend (fallback)])],
  [sim],     [AC_DEFINE([XTC_IO_BACKEND_SIM],     1, [Deterministic-simulation backend (DST; test-only)])])

# -------------------------------------------------------------------------
# TLS backend selection.
#
# One tls_<backend>.c is compiled per build.  Each backend fronts the
# same internal seam (the public xtc_tls_* API); the rest of the library
# is backend-agnostic.  --with-tls picks the backend at configure time,
# the same discipline as --with-io-backend.  Detection prefers
# pkg-config (Nix split outputs, custom prefixes) and falls back to a
# header/lib probe.
# -------------------------------------------------------------------------
AC_ARG_WITH([tls],
  [AS_HELP_STRING([--with-tls=openssl|libressl|boringssl|mbedtls|gnutls|wolfssl|schannel|none|auto],
    [Select the TLS backend (default: auto).])],
  [xtc_tls=$withval], [xtc_tls=auto])

xtc_tls_backend=none
XTC_TLS_CFLAGS=
XTC_TLS_LIBS=

AC_PATH_PROG([PKG_CONFIG], [pkg-config], [no])

AC_MSG_CHECKING([for TLS backend])
case "$xtc_tls" in
  auto)
    # Try OpenSSL (or its API-compatible drop-in: LibreSSL).  Both
    # ship libssl.so + libcrypto.so + openssl/ssl.h, so the same
    # detection works for either.  No other backend is auto-selected:
    # they must be requested explicitly so a build stays reproducible.
    ac_save_LIBS=$LIBS
    AC_CHECK_LIB([ssl], [SSL_CTX_new],
      [ac_have_ssl_lib=yes], [ac_have_ssl_lib=no], [-lcrypto])
    LIBS=$ac_save_LIBS
    AC_CHECK_HEADER([openssl/ssl.h],
      [ac_have_ssl_hdr=yes], [ac_have_ssl_hdr=no])
    AS_IF([test "x$ac_have_ssl_lib" = xyes && test "x$ac_have_ssl_hdr" = xyes],
      [xtc_tls_backend=openssl
       XTC_TLS_LIBS="-lssl -lcrypto"],
      [xtc_tls_backend=none])
    ;;
  openssl|libressl)
    # Explicit request -- fail loudly if not present.  The two share
    # an ABI; we keep both names so users can document which they
    # actually built against.
    ac_save_LIBS=$LIBS
    AC_CHECK_LIB([ssl], [SSL_CTX_new], [],
      [AC_MSG_ERROR([--with-tls=$xtc_tls: libssl not found])], [-lcrypto])
    LIBS=$ac_save_LIBS
    AC_CHECK_HEADER([openssl/ssl.h], [],
      [AC_MSG_ERROR([--with-tls=$xtc_tls: openssl/ssl.h not found])])
    xtc_tls_backend=openssl
    XTC_TLS_LIBS="-lssl -lcrypto"
    AS_IF([test "x$xtc_tls" = xlibressl],
      [xtc_tls_flavor=libressl], [xtc_tls_flavor=openssl])
    ;;
  boringssl)
    # BoringSSL is OpenSSL-API compatible, so it uses the same
    # tls_openssl.c backend; tls_openssl.c gates the few API gaps
    # (it lacks SSL_read_ex/SSL_write_ex) on OPENSSL_IS_BORINGSSL,
    # which BoringSSL's openssl/base.h defines.  BoringSSL is rarely
    # installed system-wide, so the user typically points at a build
    # tree via CPPFLAGS=-I.../include and LDFLAGS=-L.../lib; we honor
    # pkg-config (some distros ship a boringssl.pc) but fall back to
    # the plain header + -lssl -lcrypto search.
    AS_IF([test "x$PKG_CONFIG" != xno && $PKG_CONFIG --exists openssl 2>/dev/null && $PKG_CONFIG --variable=enginesdir openssl 2>/dev/null | grep -qi boring],
      [XTC_TLS_CFLAGS=`$PKG_CONFIG --cflags openssl`
       XTC_TLS_LIBS=`$PKG_CONFIG --libs openssl`], [:])
    ac_save_CPPFLAGS=$CPPFLAGS
    CPPFLAGS="$CPPFLAGS $XTC_TLS_CFLAGS"
    AC_CHECK_HEADER([openssl/ssl.h], [],
      [AC_MSG_ERROR([--with-tls=boringssl: openssl/ssl.h not found; set CPPFLAGS=-I<boringssl>/include])])
    AC_MSG_CHECKING([whether openssl/ssl.h is BoringSSL])
    AC_COMPILE_IFELSE(
      [AC_LANG_PROGRAM([[#include <openssl/ssl.h>]],
         [[#ifndef OPENSSL_IS_BORINGSSL
           #error not boringssl
           #endif]])],
      [AC_MSG_RESULT([yes])],
      [AC_MSG_RESULT([no])
       AC_MSG_ERROR([--with-tls=boringssl: the openssl/ssl.h on the include path is not BoringSSL (OPENSSL_IS_BORINGSSL undefined); fix CPPFLAGS])])
    CPPFLAGS=$ac_save_CPPFLAGS
    xtc_tls_backend=openssl
    xtc_tls_flavor=boringssl
    AS_IF([test -z "$XTC_TLS_LIBS"], [XTC_TLS_LIBS="-lssl -lcrypto"])
    ;;
  mbedtls)
    # mbedTLS splits into libmbedtls (TLS) + libmbedx509 + libmbedcrypto.
    # Its mbedtls.pc emits only -lmbedtls (the shared object carries the
    # other two as NEEDED), but linking our static libxtc.a needs all
    # three named explicitly, so append the lower layers after the
    # pkg-config output.
    AS_IF([test "x$PKG_CONFIG" != xno && $PKG_CONFIG --exists mbedtls 2>/dev/null],
      [XTC_TLS_CFLAGS=`$PKG_CONFIG --cflags mbedtls`
       XTC_TLS_LIBS="`$PKG_CONFIG --libs mbedtls` -lmbedx509 -lmbedcrypto"],
      [AC_CHECK_HEADER([mbedtls/ssl.h], [],
         [AC_MSG_ERROR([--with-tls=mbedtls: mbedtls/ssl.h not found])])
       XTC_TLS_LIBS="-lmbedtls -lmbedx509 -lmbedcrypto"])
    xtc_tls_backend=mbedtls
    ;;
  gnutls)
    AS_IF([test "x$PKG_CONFIG" != xno && $PKG_CONFIG --exists gnutls 2>/dev/null],
      [XTC_TLS_CFLAGS=`$PKG_CONFIG --cflags gnutls`
       XTC_TLS_LIBS=`$PKG_CONFIG --libs gnutls`],
      [AC_CHECK_HEADER([gnutls/gnutls.h], [],
         [AC_MSG_ERROR([--with-tls=gnutls: gnutls/gnutls.h not found])])
       XTC_TLS_LIBS="-lgnutls"])
    xtc_tls_backend=gnutls
    ;;
  wolfssl)
    AS_IF([test "x$PKG_CONFIG" != xno && $PKG_CONFIG --exists wolfssl 2>/dev/null],
      [XTC_TLS_CFLAGS=`$PKG_CONFIG --cflags wolfssl`
       XTC_TLS_LIBS=`$PKG_CONFIG --libs wolfssl`],
      [AC_CHECK_HEADER([wolfssl/ssl.h], [],
         [AC_MSG_ERROR([--with-tls=wolfssl: wolfssl/ssl.h not found])])
       XTC_TLS_LIBS="-lwolfssl"])
    xtc_tls_backend=wolfssl
    ;;
  schannel)
    # Windows-native SSPI/SChannel.  Only valid on a Windows host; the
    # backend is compile-verified there and links against secur32 +
    # crypt32.  Reject on non-Windows so the misconfiguration is loud.
    case "$host_os" in
      *mingw*|*cygwin*|*msys*|*windows*) ;;
      *) AC_MSG_ERROR([--with-tls=schannel is Windows-only (host_os=$host_os)]) ;;
    esac
    xtc_tls_backend=schannel
    XTC_TLS_LIBS="-lsecur32 -lcrypt32"
    ;;
  none)
    xtc_tls_backend=none
    ;;
  *)
    AC_MSG_ERROR([invalid --with-tls=$xtc_tls; use openssl, libressl, boringssl, mbedtls, gnutls, wolfssl, schannel, none, or auto])
    ;;
esac
AC_MSG_RESULT([$xtc_tls_backend])

# Exactly one XTC_TLS_BACKEND_* macro is defined; configure also exports
# XTC_TLS_BACKEND so the Makefile compiles the matching tls_<backend>.c.
AS_CASE([$xtc_tls_backend],
  [openssl],  [AC_DEFINE([XTC_TLS_BACKEND_OPENSSL],  1, [OpenSSL/LibreSSL TLS backend])],
  [mbedtls],  [AC_DEFINE([XTC_TLS_BACKEND_MBEDTLS],  1, [mbedTLS TLS backend])],
  [gnutls],   [AC_DEFINE([XTC_TLS_BACKEND_GNUTLS],   1, [GnuTLS TLS backend])],
  [wolfssl],  [AC_DEFINE([XTC_TLS_BACKEND_WOLFSSL],  1, [wolfSSL TLS backend])],
  [schannel], [AC_DEFINE([XTC_TLS_BACKEND_SCHANNEL], 1, [Windows SChannel TLS backend])],
  [none],     [AC_DEFINE([XTC_TLS_BACKEND_NONE],     1, [No TLS backend; NOSYS stubs])])

AC_SUBST([XTC_TLS_BACKEND], [$xtc_tls_backend])
AC_SUBST([XTC_TLS_CFLAGS])
AC_SUBST([XTC_TLS_LIBS])

# -------------------------------------------------------------------------
# Optional: hegel-c property-based testing.
# When enabled, links the PBT suite against the local hegel-c library
# and forwards HEGEL_SERVER_COMMAND through `make check` so the
# subprocess can find the hegel server.
# -------------------------------------------------------------------------
AC_ARG_WITH([hegel],
  [AS_HELP_STRING([--with-hegel=PATH],
    [Enable hegel-c property-based tests; PATH is the hegel-c source root that contains include/hegel/hegel.h and build/libhegel.{a,so}.])],
  [xtc_with_hegel=$withval], [xtc_with_hegel=no])

AC_MSG_CHECKING([whether to build hegel-c property tests])
AS_IF([test "x$xtc_with_hegel" = xno],
  [AC_MSG_RESULT([no])
   xtc_hegel_cflags=
   xtc_hegel_libs=
   xtc_hegel_pbt=no],
  [test -f "$xtc_with_hegel/include/hegel/hegel.h"],
  [AC_MSG_RESULT([yes (using $xtc_with_hegel)])
   xtc_hegel_cflags="-I$xtc_with_hegel/include"
   xtc_hegel_pbt=yes

   # Find libcbor and libz.  We try in order:
   #   1) the dirs already on the linker search path (system install);
   #   2) the directory the prebuilt libhegel.so was linked against
   #      (read off ldd output);
   #   3) failure with a clear error.
   xtc_extra_l=
   xtc_extra_L=
   for dep in cbor z; do
     if echo 'int main(void){return 0;}' | $CC -x c - -l$dep -o /dev/null 2>/dev/null; then
       :
     else
       # 1) try ldd of libhegel.so (catches libcbor)
       lib=`ldd "$xtc_with_hegel/build/libhegel.so" 2>/dev/null \
          | $AWK -v d=lib$dep '$1 == d ".so" || $1 ~ "^" d "\\.so\\." {print $3; exit}'`
       if test -z "$lib" -o ! -f "$lib"; then
         # 2) try ldd of a known-good binary (catches libz on nix)
         for probe in /run/current-system/sw/bin/git \
                      /run/current-system/sw/bin/curl \
                      /usr/bin/git /usr/bin/curl ; do
           if test -x "$probe"; then
             lib=`ldd "$probe" 2>/dev/null \
                | $AWK -v d=lib$dep '$1 == d ".so" || $1 ~ "^" d "\\.so\\." {print $3; exit}'`
             if test -n "$lib" -a -f "$lib"; then break; fi
           fi
         done
       fi
       if test -n "$lib" -a -f "$lib"; then
         libdir=`dirname "$lib"`
         xtc_extra_L="$xtc_extra_L -L$libdir -Wl,-rpath,$libdir"
       else
         AC_MSG_WARN([could not find lib$dep; --with-hegel build may fail])
       fi
     fi
     xtc_extra_l="$xtc_extra_l -l$dep"
   done
   xtc_hegel_libs="-L$xtc_with_hegel/build -Wl,-rpath,$xtc_with_hegel/build -lhegel $xtc_extra_L $xtc_extra_l"
   AC_DEFINE([XTC_HAVE_HEGEL], 1, [hegel-c property tests enabled])],
  [AC_MSG_ERROR([--with-hegel=$xtc_with_hegel: hegel.h not found at $xtc_with_hegel/include/hegel/hegel.h])])
AC_SUBST([XTC_HEGEL_CFLAGS], [$xtc_hegel_cflags])
AC_SUBST([XTC_HEGEL_LIBS],   [$xtc_hegel_libs])
AC_SUBST([XTC_HEGEL_PBT],    [$xtc_hegel_pbt])

AC_ARG_WITH([hegel-server],
  [AS_HELP_STRING([--with-hegel-server=CMD],
    [Override how the test harness launches the hegel server (default: "hegel" on PATH).])],
  [xtc_hegel_server=$withval], [xtc_hegel_server=hegel])
AC_SUBST([XTC_HEGEL_SERVER_COMMAND], [$xtc_hegel_server])

# Require C11.  PG18 raised the floor; PG19 inherits.
AC_MSG_CHECKING([whether the C compiler accepts -std=c11])
saved_CFLAGS=$CFLAGS
CFLAGS="$CFLAGS -std=c11"
AC_COMPILE_IFELSE([AC_LANG_PROGRAM([[]], [[
#if __STDC_VERSION__ < 201112L
#error "C11 required"
#endif
]])],
  [AC_MSG_RESULT([yes])],
  [CFLAGS=$saved_CFLAGS
   AC_MSG_RESULT([no])
   AC_MSG_ERROR([xtc requires a C11 compiler])])

# Under -std=c11 the libc exposes only the ISO C namespace by default;
# POSIX functions (nanosleep, fdopen, etc.) need a feature-test macro.
# The library TUs get _DEFAULT_SOURCE via xtc_int.h, but the test
# programs include only the public <xtc.h> (which must not leak feature
# macros), so they would miss POSIX declarations on a strict libc like
# musl (glibc is laxer and happens to expose them anyway).  Set
# _DEFAULT_SOURCE globally: it is the right base feature level for a
# POSIX program and is a no-op where the libc already exposes POSIX.
CFLAGS="$CFLAGS -D_DEFAULT_SOURCE"

AC_SUBST([CFLAGS])

# -------------------------------------------------------------------------
# pkg-config private link flags.  These are the transitive dependencies a
# static consumer must add on the link line; pkg-config emits them under
# Libs.private and `pkg-config --static --libs xtc` surfaces them.
# -------------------------------------------------------------------------
xtc_pc_libs_private="-pthread $LIBS $uring_libs $xtc_extra_libs $XTC_TLS_LIBS $libunwind_libs"
AC_SUBST([XTC_PC_LIBS_PRIVATE], [$xtc_pc_libs_private])

# -------------------------------------------------------------------------
# Output.
# -------------------------------------------------------------------------
AC_CONFIG_FILES([Makefile:../dist/Makefile.in])
AC_CONFIG_FILES([xtc.pc:../dist/xtc.pc.in])
AC_OUTPUT
