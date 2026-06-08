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

## IOCP backend: round-2 native overlapped (COMPILED, NOT RUNTIME-VERIFIED)

`src/io/io_iocp.c` was rewritten from the round-1 WSAEventSelect +
WaitForMultipleObjects readiness emulation (hard-capped at 64 handles)
to a native completion-port design:

  - A single `CreateIoCompletionPort` is the only wait primitive;
    `GetQueuedCompletionStatusEx` dequeues completions in batch.  The
    64-handle cap is GONE -- any number of sockets and file AIOs
    associate with the one port.
  - Socket readiness uses the AFD poll fast path the round-2 plan
    called for: `\Device\Afd` is opened with `NtCreateFile`,
    associated with the port, and one `IOCTL_AFD_POLL`
    (`NtDeviceIoControlFile`) is armed per registered socket and
    re-armed after each completion (level-triggered, matching the
    epoll/kqueue contract).  This is the wepoll/libuv approach.
  - The cross-thread wakeup is `PostQueuedCompletionStatus` with
    completion-key `XTC_IOCP_KEY_WAKEUP`.
  - File AIO (`pread`/`pwrite`) is an overlapped `ReadFile`/`WriteFile`
    whose file HANDLE is associated with the port, so its completion
    is dequeued like any socket event.  `fsync`/`fdatasync` return
    `XTC_E_NOSYS` and are offloaded (FlushFileBuffers is synchronous).
  - OVERLAPPED ownership rule: an OVERLAPPED belongs to the kernel
    from the moment a request is accepted until its completion is
    dequeued; it is never freed or reused while in flight.
    Deregistering an armed socket issues `NtCancelIoFileEx` and defers
    the free to the (canceled) completion's reap.  Registration nodes
    are separately heap-allocated with stable addresses so the
    kernel-held back-pointer never dangles across a table grow or
    swap-remove.

### What is verified on the Linux dev host (this round)

  - **Cross-compiles clean** with mingw-w64 gcc 14.3.0
    (`nix-shell -p pkgsCross.mingwW64.buildPackages.gcc`),
    `-std=c11 -Wall -Wextra`, against `<winsock2.h>` / `<windows.h>` /
    `<winternl.h>` / `<ntstatus.h>` / `<mswsock.h>`.
  - **Links** into a PE32+ binary against `-lntdll -lws2_32`
    (`NtCreateFile`, `NtDeviceIoControlFile`, `NtCancelIoFileEx`,
    `WSAIoctl(SIO_BASE_HANDLE)`, `CreateIoCompletionPort`,
    `GetQueuedCompletionStatusEx`, `PostQueuedCompletionStatus` all
    resolve).  `dist/configure.ac` now adds `-lntdll` to the Windows
    `LIBS`; `dist/build_msvc.bat` links `ntdll.lib`.
  - `io_common.c`, `io_net.c`, and `io_iocp.c` all cross-compile
    together with the IOCP backend selected.
  - The Linux build is **unaffected**: `io_iocp.c` is `#if
    defined(XTC_IO_BACKEND_IOCP)` only, so it compiles to an empty TU
    on Linux; the full C munit suite stays green there.

### What is NOT verified (must run on santorini / a Windows host)

The backend has NOT executed on Windows.  The following must be
validated before calling it production quality (mirrors the
reviewed-but-untested status of `src/io/io_aix.c`):

  1. **AFD poll correctness.**  That `IOCTL_AFD_POLL` with the
     `XTC_AFD_POLL_*` event mask reports FD_READ/FD_WRITE/FD_ACCEPT/
     FD_CLOSE-equivalent readiness for connected TCP, listening, and
     UDP sockets, and that `SIO_BASE_HANDLE` yields the right base
     socket through any layered service providers.
  2. **Re-arm/level-triggered semantics.**  That re-arming after each
     completion does not busy-loop on a persistently-ready socket and
     does not drop edges, i.e. that the m2 `io_events` contract
     (E1..E7) passes through this backend exactly as it does on epoll.
  3. **Cancel/lifetime under churn.**  `mod_fd` and `del_fd` on an
     armed poll: that the canceled completion is reaped exactly once,
     the dead node is freed exactly once, and no OVERLAPPED is freed
     while the kernel still owns it (run under Application Verifier /
     Dr. Memory if available).
  4. **Wakeup ordering.**  That `PostQueuedCompletionStatus` wakeups
     coalesce into one `XTC_IO_WAKEUP` event per poll (m2 W3) and do
     not race the AFD completions -- this is the suspected home of the
     `test_proc::selective_receive` flake (see KNOWN_ISSUES.md).
  5. **File AIO round-trip** through the port (the MSVC smoke test
     already exercised the round-1 hEvent variant; it must be re-run
     against the port-associated variant).
  6. The full m2 suite (`test_io_register`, `test_io_events`,
     `test_io_wakeup`, `test_io_integration`) over a tcp-socketpair
     (`test/include/io_pipe_compat.h`), since IOCP/AFD work on sockets,
     not anonymous CRT pipes.

Run on santorini via `dist/santorini-matrix.sh`; there is no
non-interactive Windows CI job yet, so this is a hand-run gate.

### Cross-compile commands used (Linux dev host)

    nix-shell -p pkgsCross.mingwW64.buildPackages.gcc
    x86_64-w64-mingw32-gcc -c -std=c11 -DXTC_IO_BACKEND_IOCP=1 \
        -I<config-dir-with-XTC_IO_BACKEND_IOCP> -Isrc/inc \
        -Wall -Wextra -o io_iocp.o src/io/io_iocp.c
    # link check (against a stub allocator + drain_wakeup):
    x86_64-w64-mingw32-gcc -o drv.exe drv.c io_iocp.o -lntdll -lws2_32

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
cl.exe 14.50 and ml64.exe; `...\\17\\Community` provides the VS2022
cl.  libxtc builds with either via `dist\\build_msvc.bat`, run inside
an x64 Native Tools environment (or after calling `vcvars64.bat`):

    set XTC_SRC=C:\\scratch\\xtc
    call "...\\VC\\Auxiliary\\Build\\vcvars64.bat"
    C:\\scratch\\xtc\\dist\\build_msvc.bat

This assembles `fctx_x86_64_ms_pe.asm` with ml64, compiles every
`src/` translation unit with cl (`/std:c11 /experimental:c11atomics`),
archives `xtc.lib`, and builds `test\\msvc\\smoke.c`.  The smoke test
passes on both VS2022 (17) and VS2026 (18): version, strerror, the
Win32 clocks, a slab alloc/free round-trip, an lwlock acquire/release,
SEH fault containment, and native IOCP file AIO (an overlapped
ReadFile/WriteFile round-trip through xtc_aio on an OVERLAPPED handle).

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

## Unix domain sockets on Windows (COMPILED, NOT RUNTIME-VERIFIED)

Windows 10 (build 17063+) supports `AF_UNIX` with `SOCK_STREAM` via
`<afunix.h>`, so `src/io/io_net.c` no longer returns `XTC_E_NOSYS`
for the four UDS entry points on Windows.  `xtc_net_unix_listen` /
`xtc_net_unix_dial` mirror the POSIX path with `DeleteFileA` standing
in for the `unlink` of a stale socket path and `WSA_INIT_ONCE()` for
Winsock startup.

The credential path differs by design: Windows `AF_UNIX` has NO
ancillary credential channel (no `SO_PEERCRED`/`LOCAL_PEERCRED`
analogue), so `xtc_net_unix_send_creds` is a plain `send` and
`xtc_net_unix_recv_creds` returns the received bytes with `uid`/`gid`
reported as 0.  Callers that need peer identity on Windows must use a
different mechanism (e.g. a named-pipe `GetNamedPipeClientProcessId`
path, not yet wired).

Verified on the Linux dev host only: cross-compiles clean with
mingw-w64 against `<afunix.h>` (`-Wall -Wextra`).  Not yet run on a
Windows host -- the listen/dial round-trip and the no-creds contract
must be exercised on santorini, and a `test_net_unix` Windows case
added, before this is trusted.

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
