# XTC — Process this file with autoconf to produce a configure script.
#
# Copyright (c) 2026, The XTC Project — All rights reserved.
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

# pthread
AC_SEARCH_LIBS([pthread_create], [pthread], [],
  [AC_MSG_ERROR([xtc requires pthreads])])
# librt for clock_gettime on older glibcs (no-op on modern ones).
AC_SEARCH_LIBS([clock_gettime], [rt], [], [])

# -------------------------------------------------------------------------
# L1 I/O backend selection.
# -------------------------------------------------------------------------
AC_ARG_WITH([io-backend],
  [AS_HELP_STRING([--with-io-backend=auto|poll|epoll|uring|kqueue|iocp|solaris|aix],
    [Pick the L1 I/O backend (default: auto).])],
  [xtc_io_backend=$withval], [xtc_io_backend=auto])

AC_ARG_WITH([liburing],
  [AS_HELP_STRING([--with-liburing=PATH],
    [Path to a liburing install (overrides pkg-config search).])],
  [xtc_with_liburing=$withval], [xtc_with_liburing=auto])

have_epoll=no
have_kqueue=no
have_uring=no
uring_cflags=
uring_libs=
AC_CHECK_HEADER([sys/epoll.h], [have_epoll=yes], [have_epoll=no])
AC_CHECK_HEADER([sys/event.h], [have_kqueue=yes], [have_kqueue=no])
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
    if test "$have_uring" = yes; then xtc_io_backend=uring
    elif test "$have_epoll" = yes; then xtc_io_backend=epoll
    elif test "$have_kqueue" = yes; then xtc_io_backend=kqueue
    else xtc_io_backend=poll
    fi ;;
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
  poll|iocp|solaris|aix) ;;
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
    xtc_extra_libs="-lws2_32"
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
  [aix],     [AC_DEFINE([XTC_IO_BACKEND_AIX],     1, [AIX pollset backend (stub)])])

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

AC_SUBST([CFLAGS])

# -------------------------------------------------------------------------
# Output.
# -------------------------------------------------------------------------
AC_CONFIG_FILES([Makefile:../dist/Makefile.in])
AC_OUTPUT
