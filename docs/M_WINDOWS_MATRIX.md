---
title: "Windows build matrix"
---

# Windows build matrix

xtc supports multiple Windows toolchains.  This document records
which compile and test combinations are exercised on the santorini
build host (Windows 11 ARM64, x86_64 emulation layer).

## Tested matrix

| Toolchain   | Version       | Build  | Tests built | Tests pass | Notes |
|-------------|---------------|:------:|:-----------:|:----------:|-------|
| MinGW64 gcc | 16.1.0        | OK     | 50          | 233/233    | Default Windows path; full coverage |
| Clang64     | 22.1.4        | OK     | 50          | 48/48      | LLVM clang with MinGW runtime; 3 POSIX-only tests don't compile |
| MSVC cl.exe | 14.44.35207   | -      | -           | -          | Build system port required; see below |

## MinGW64 (msys2 mingw64)

The reference Windows toolchain.  Configure auto-detects the IOCP
backend; no extra flags required.

    export PATH=/mingw64/bin:/usr/bin:$PATH
    cd /c/scratch/xtc
    cd dist && autoreconf -i && cd ..
    mkdir -p build_mingw && cd build_mingw
    ../dist/configure --with-tls=none
    make -j4

All 50 test binaries build.  The two TLS-handshake tests SKIP
because TLS is disabled in this configuration.  The remaining 48
report 233 munit assertions, 0 failures.

## Clang64 (msys2 clang64)

The clang frontend with the MinGW runtime.  Same configure path as
MinGW64 with `CC=clang`:

    export PATH=/c/msys64/clang64/bin:/usr/bin:$PATH
    pacman -S --needed mingw-w64-clang-x86_64-clang
    cd /c/scratch/xtc/build_clang64
    CC=clang ../dist/configure --with-tls=none
    make -j4

47 of 50 test binaries compile.  The three that fail to link are:

  * `test_net_udp` -- uses POSIX-only `nanosleep` semantics.
  * `test_proc_wait_fd` -- exercises a Linux-specific eventfd path
    that hasn't been ported to Windows IOCP.
  * `test_slab_shm` -- uses `mmap(MAP_SHARED)` cross-process state
    that has no Windows equivalent in xtc yet.

The 47 that build pass 48/48 (the 2 TLS handshake cases SKIP, same
reason as MinGW64).  Tracking the three Windows ports in
[PLAN.md](../PLAN.md).

## MSVC cl.exe

`C:\Program Files\Microsoft Visual Studio\18\Community\VC\Tools\MSVC\14.44.35207`
ships a working cl.exe (HostArm64 + Hostx64 toolchains, x64 / x86 /
arm64 targets).  Native MSVC cannot yet build xtc.  The blockers
are:

  * **Assembly syntax.**  `src/os/asm/fctx_x86_64_*.S` use GNU
    assembler (GAS) syntax; MSVC expects MASM (.asm) syntax through
    ml64.exe.  The fcontext primitives need translation.

  * **Build system.**  The autoconf path uses MinGW-style flags
    (`-fPIC -pthread -std=c11 ...`) that cl.exe doesn't accept.
    The meson path is currently a stub that builds only the
    public-API translation units; the M3+ event loop, M7+
    channels, and M8+ processes aren't compiled there.

`clang-cl.exe` (LLVM's MSVC-compatible driver, installed at
`C:\Program Files\LLVM\bin\clang-cl.exe`) accepts the GCC
intrinsics that the source uses (`__builtin_expect`,
`__builtin_ctzll`, `__builtin_clzll` -- already wrapped via
`XTC_LIKELY` / `XTC_CTZLL` / `XTC_CLZLL` macros) but still hits the
asm-syntax blocker.

### Path to MSVC support

The work is straightforward but boxed:

  1. Port `fctx_x86_64_sysv.S` and `fctx_x86_64_ms_pe.S` to MASM
     equivalents (`fctx_x86_64_sysv.asm`, `fctx_x86_64_ms_pe.asm`).
     Each is ~100 lines of standard register save / restore.
  2. Extend `meson.build` to enumerate every translation unit
     under `src/{os,io,evt,ptc,orc}/` and link against the
     produced .obj from ml64.exe.
  3. Add MSVC-specific cflags (`/W3 /std:c11 /experimental:c11atomics`).
  4. Run the test suite and adjust as MSVC-specific issues
     surface.

Estimate: 1-2 days of focused work.  Tracked in PLAN.md.

## Reproducing on santorini

Push a tarball (gitignored junk excluded):

    cd $XTC_SRC_ROOT
    git ls-files | tar cz --files-from=- > /tmp/xtc-snap.tgz
    scp /tmp/xtc-snap.tgz santorini:xtc-snap.tgz

Land it under c:\scratch and extract:

    ssh santorini 'cmd /c "move %USERPROFILE%\xtc-snap.tgz c:\scratch\"'
    ssh santorini 'cmd /c "C:\msys64\usr\bin\bash.exe -lc \
        \"mkdir -p /c/scratch/xtc && cd /c/scratch/xtc && \
          tar xzf /c/scratch/xtc-snap.tgz\""'

Run the matrix script (see `dist/santorini-matrix.sh`).
