# dist/xtc.spec -- RPM spec for libxtc.
#
# Copyright (c) 2026, The XTC Project
# Use of this source code is governed by the ISC License.
#
# Build:
#   rpmbuild -ba dist/xtc.spec \
#       --define "_sourcedir $PWD" \
#       --define "version 0.5.0"
# (or set Version: below and point Source0 at a release tarball).

%global sover 0

Name:           libxtc
Version:        0.5.0
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
* Mon Jun 08 2026 Greg Burd <greg@burd.me> - 0.5.0-1
- Initial RPM packaging (shared library + -devel subpackage).
