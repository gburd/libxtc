# Windows build matrix

xtc supports multiple Windows toolchains.  This document records
which compile and test combinations are exercised on the santorini
build host (Windows 11 ARM64, x86_64 emulation layer).

## Tested matrix

| Toolchain   | Version       | Build  | Tests built | Tests pass | Notes |
|-------------|---------------|:------:|:-----------:|:----------:|-------|
| MinGW64 gcc | 16.1.0        | OK     | 50          | 233/233    | Default Windows path; full coverage |
| Clang64     | 22.1.4        | OK     | 50          | 48/48      | LLVM clang with MinGW runtime; 3 POSIX-only tests don't compile |
| MSVC cl.exe | 14.50.35717   | OK     | smoke       | 5/5        | xtc.lib (45 objs incl. ml64 fcontext); standalone smoke test |

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

`C:\\Program Files\\Microsoft Visual Studio\\18\\Community` provides
cl.exe 14.50 and ml64.exe.  libxtc builds with the Microsoft
toolchain via `dist\\build_msvc.bat`, run inside an x64 Native Tools
environment (or after calling `vcvars64.bat`):

    set XTC_SRC=C:\\scratch\\xtc
    call "...\\VC\\Auxiliary\\Build\\vcvars64.bat"
    C:\\scratch\\xtc\\dist\\build_msvc.bat

This assembles `fctx_x86_64_ms_pe.asm` with ml64, compiles every
`src/` translation unit with cl (`/std:c11 /experimental:c11atomics`),
archives `xtc.lib` (45 objects), and builds `test\\msvc\\smoke.c`.
The smoke test passes: version, strerror, the Win32 clocks, a slab
alloc/free round-trip, and an lwlock acquire/release.

What made the MSVC build work:

  * The GAS context-switch asm was ported to MASM
    (`fctx_x86_64_ms_pe.asm`).
  * `__thread` was replaced by the portable `XTC_THREAD_LOCAL`
    (`__declspec(thread)` on MSVC).
  * `__attribute__((format))` and `__attribute__((packed))` were
    wrapped in portable macros (`XTC_PRINTF_FMT`, `XTC_PACK_*`).
  * MSVC lacks winpthreads, so `src/inc/compat/` provides Win32
    shims for the pthread / semaphore / sched / unistd / sys.time
    surface the code uses, plus a hand-authored `xtc_config.h`.
  * `os_time.c` gained a Win32 branch (QueryPerformanceCounter /
    GetSystemTimePreciseAsFileTime).

The munit harness is not used for the MSVC test because its
`MUNIT_ARRAY_PARAM(argc + 1)` expands to a VLA array parameter that
cl rejects; the standalone smoke test covers the Win32-specific
paths instead.  Wiring the full munit suite for MSVC (a
harness-only fix) is the remaining MSVC work.

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
