# dist/xtc.spec -- RPM spec for libxtc.
#
# Copyright (c) 2026, The XTC Project
# Use of this source code is governed by the ISC License.
#
# Build:
#   rpmbuild -ba dist/xtc.spec \
#       --define "_sourcedir $PWD" \
#       --define "version 0.9.0"
# (or set Version: below and point Source0 at a release tarball).

%global sover 0

Name:           libxtc
Version:        0.9.0
Release:        1%{?dist}
Summary:        High-performance async/concurrency runtime for C

License:        ISC
URL:            https://codeberg.org/gregburd/libxtc
Source0:        %{name}-%{version}.tar.gz

BuildRequires:  gcc
BuildRequires:  make
BuildRequires:  autoconf
BuildRequires:  pkgconfig
BuildRequires:  openssl-devel
BuildRequires:  liburing-devel

%description
xtc is a C11 async/concurrency runtime: an event loop with pluggable
I/O backends (epoll, io_uring, kqueue, IOCP, event ports), stackful
coroutines, work-stealing executors, channels, an OTP-style process
and supervision model, and synchronisation primitives.

This package contains the shared library.

%package devel
Summary:        Development files for libxtc
Requires:       %{name}%{?_isa} = %{version}-%{release}
Requires:       pkgconfig

%description devel
Headers, the static library, pkg-config metadata, and manual pages
required to build applications against xtc.

%prep
%setup -q

%build
# xtc mandates an out-of-source build driven from dist/configure.
mkdir -p build_rpm
cd build_rpm
../dist/configure \
    --prefix=%{_prefix} \
    --libdir=%{_libdir} \
    --includedir=%{_includedir} \
    --mandir=%{_mandir} \
    --enable-shared \
    --with-tls=auto
%make_build

%install
cd build_rpm
%make_install

%check
cd build_rpm
make check

%files
%license LICENSE
%{_libdir}/libxtc.so.%{sover}
%{_libdir}/libxtc.so.%{version}

%files devel
%{_includedir}/xtc.h
%{_libdir}/libxtc.a
%{_libdir}/libxtc.so
%{_libdir}/pkgconfig/xtc.pc
%{_mandir}/man3/*.3*
%{_mandir}/man7/*.7*

%changelog
* Fri Jul 03 2026 Greg Burd <greg@burd.me> - 0.9.0-1
- DST toward FDB parity (network partition, lock-mgr under sim, bufmgr
  + WAL crash-recovery capstone); preemption hang fix + aarch64
  trampoline; M16.1a mock PG backend; full man-page coverage.

* Wed Jul 01 2026 Greg Burd <greg@burd.me> - 0.8.0-1
- Native preemption facility (docs/M_PREEMPTION.md): per-worker
  CPU-time timer seam; a per-thread async-signal-unsafe-region depth
  counter around the allocator; cooperative-assisted preemption
  (xtc_exec_set_preempt) as the supported mode; signal-context
  involuntary-yield infrastructure + safety gate in place (the resumable
  redirect pending per-arch mcontext work, falls back to cooperative).
* Wed Jul 01 2026 Greg Burd <greg@burd.me> - 0.7.0-1
- DST toward FoundationDB parity: seeded replayable scheduler over the
  real multi-loop executor; 20,000-seed soak clean; simulated I/O
  faults + Buggify + critical-section fault points + latch coverage.
- Concurrent B-link node merge enabled by default (correct under
  concurrent latch-free deletes; churn-gone gate + ASan + 32/32 oracle).
- Cooperative-assisted preemption (xtc_exec_set_preempt + a per-worker
  CPU-time timer); see docs/M_PREEMPTION.md.
- Stackless Isolate layer promoted to a supported API (xtc_tnt_*).
- musl CI qualification (builtin _Unwind_Backtrace, no libunwind);
  optional DPDK (--with-dpdk); O(1) chained-hash process registry.
* Thu Jun 25 2026 Greg Burd <greg@burd.me> - 0.6.0-1
- sqlxtc example is now a fully libxtc-native SQL engine: the vendored
  SQLite (sqlite3.c, VDBE, virtual tables, and the four extension-point
  shims) is removed; the engine runs on a Lime parser, a vectorized
  executor, and an xtc-native B-link/buffer-pool/WAL storage engine.
- io_kqueue: native AIO restricted to FreeBSD/DragonFly (macOS 26 SDK).
* Mon Jun 08 2026 Greg Burd <greg@burd.me> - 0.5.0-1
- Initial RPM packaging (shared library + -devel subpackage).
