# Cross-platform port status

Updated end of M_PORT round 4.

## Per-platform tally (this round)

| Platform | Build | Tests passing |
|---|---|---|
| **Linux x86_64** (epoll/uring/gcc14) | clean, no warnings | **151 / 151** munit + 11 PBT + 22 shell |
| **FreeBSD 15 amd64** (kqueue/clang19) | clean, no warnings | full test suite tracked across rounds |
| **OpenIndiana SPARC** (poll *and* port_*/gcc13) | clean | 132/132 with native event-port backend (full parity) |
| **Windows 11 ARM64 / MinGW-W64 x64** (IOCP/gcc) | clean | 31/36 -- test_fctx now passes (Win64 fcontext asm working); 4 io_* tests blocked on IOCP CRT-fd-vs-HANDLE mismatch (round 3 work); 1 test_proc subtest needs separate fix |
| **Windows 11 ARM64 NATIVE** (clang 20 / MSVC 19.44 & 19.50, real ARM64 host) | clean compile-checks; native fcontext asm RUNTIME-VERIFIED | fcontext round-trip harness PASSES on clang and both MSVC; `io_iocp.c`/`tls_schannel.c`/`xtc_version.c`/`os_alloc.c` all compile `-c` clean (0 warnings) under clang/cl-x2/gcc.  See `docs/M_WINDOWS_MATRIX.md`. |
| **AIX** (pollset/xlc or gcc-aix) | code-complete, untested | `src/io/io_aix.c` implements `pollset_create`/`pollset_ctl`/`pollset_poll`; awaits AIX host for runtime verification |

## Linux (control)

All milestones M0 through M13 pass.  Cumulative this round delivered:

* M10.5: `xtc_app` (root supervisor + lifecycle), one_for_all + rest_for_one + simple_one_for_one strategies (M10.5 final), `xtc_svr` (gen_server), `xtc_reg` (process registry).
* M11: `xtc_mctx` (memory contexts).
* M13a: `xtc_rcu` (epoch reclamation).
* M13b: `xtc_lrlock` (left-right lock).
* M13c: `xtc_lockmgr` (5-mode lock manager + deadlock detector).

## FreeBSD 15 (kqueue)

Bug surfaced and fixed via FreeBSD agent: `struct lock_entry::aborted`
needed `_Atomic int` to satisfy clang 19's strict C11 atomics typing.
Linux glibc was lenient.  After the one-character fix, **63/63** of the
requested runtime tests passed including the full M13b/c stack.

## illumos / OpenIndiana (port_*)

This round implemented `src/io/io_solaris.c` against `port_create` /
`port_associate` / `port_dissociate` / `port_getn` and the side-table
mechanism shared with the kqueue backend.  One bug surfaced and was
fixed:

* `port_getn`'s 4th parameter is `nget`, in/out: input is the **minimum**
  number of events to return, output is how many were actually returned.
  The first cut clobbered it with the buffer size, causing the call to
  block until N events were ready (where N could be 8, 16, 64).  A
  one-fd test would then time out at the user's poll deadline.

After the fix, **132/132** tests pass on illumos with the native event-
port backend.  Full parity with Linux/FreeBSD.

## Windows MinGW (IOCP)

Round 1 of the Windows port shipped an end-to-end working IOCP backend.
Substantial new code:

* `src/evt/coro_winfiber.c` -- Win32 fiber substrate (CreateFiberEx /
  SwitchToFiber / ConvertThreadToFiber).  Mirror of `coro_uctx.c`.
* `src/io/io_iocp.c` -- real IOCP backend.  Wakeup uses
  `PostQueuedCompletionStatus` with completion-key `XTC_IOCP_KEY_WAKEUP`.
  User fds use `WSAEventSelect` + `WaitForMultipleObjects` for
  readiness emulation (round 2 will swap this for AFD/
  `NtDeviceIoControlFile` for native-IOCP performance).
* `src/io/io_common.c` -- Windows path drops the socket-pair self-pipe
  entirely (IOCP wakeup is the post-completion).
* `src/os/os_alloc.c` -- Windows routes ALL allocations through
  `_aligned_malloc` / `_aligned_realloc` / `_aligned_free` (alignment
  16) so the hook surface remains symmetric.
* `src/os/os_cpu.c` -- Windows uses `GetSystemInfo`.
* `dist/configure.ac` -- adds `-lws2_32` to LIBS on `*-mingw*`/
  `*-cygwin*`/`*-msys*` (round 2 adds `-lntdll` for the AFD poll path).
* `src/os/asm/fctx_x86_64_sysv.S` -- guarded with `#if !defined(_WIN32)`
  so the SysV asm stubs out (Win64 ABI is incompatible).

**Results: 30/35 tests passing.**  All pure-C and locked-primitive tests
green: test_atomic, test_alloc, test_time, test_mutex, test_thread,
test_tls, test_mctx, test_rcu, test_lrlock, test_lockmgr.  All
loop+coro tests green: test_loop, test_task, test_async, test_proc,
test_sync, test_sup, test_app, test_svr, test_reg, test_chan,
test_chan_mpmc_bcast, test_exec, test_cross_wake, test_deque,
test_timer, test_waker.

### Remaining Windows gaps (all round-2 deliverables)

1. **Anonymous pipes vs IOCP** (4 test failures: test_io_register,
   test_io_wakeup, test_io_events, test_io_integration).  MinGW's
   `_pipe()` creates non-OVERLAPPED HANDLEs that IOCP can't bind.
   Fix: a `make_overlapped_pipe()` helper using `CreateNamedPipe(...
   FILE_FLAG_OVERLAPPED)` or a tcp-socketpair shim.  Test-side
   change; the IOCP backend itself is correct (it correctly rejects
   non-overlapped HANDLEs).

2. **Win64 fcontext asm** (1 build failure: test_fctx).  The SysV asm
   uses RDI/RSI/RDX as arg registers; Win64 uses RCX/RDX/R8/R9.
   Different callee-saved set (RDI/RSI become callee-saved; XMM6-15
   too).  Different stack alignment.  Need
   `src/os/asm/fctx_x86_64_ms_pe.S` selected by configure when
   `host_os` matches `*mingw*`.  Boost.Context's
   `make_x86_64_ms_pe_gas.asm` is a known-good template.

   UPDATE (2026-06): the AArch64 Windows fcontext is now done and
   RUNTIME-VERIFIED on a real Windows 11 ARM64 host.  The ELF
   `fctx_aarch64_aapcs.S` is `!_WIN32`-guarded (empty on Windows), so
   two siblings were added: `fctx_aarch64_ms_pe.S` (clang/GAS) and
   `fctx_aarch64_ms_pe.asm` (armasm64/MSVC), both guarded
   `defined(__aarch64__) && defined(_WIN32)`.  They keep x18 (the
   Windows reserved TEB register) untouched and drop the ELF-only
   directives.  A round-trip harness (transfer threading, x19-x28 +
   d8-d15 preservation, multi-resume) passes natively under clang 20
   and both MSVC (19.44 / 19.50), including the two cross-link cases
   that prove the `.S` and `.asm` objects are ABI-interchangeable.
   NOTE: `coro_fctx.c` is `!_WIN32`-guarded, so the live Windows coro
   substrate is still Win32 fibers (`coro_winfiber.c`); the asm is
   ready but not on the active Windows coroutine path.

Neither blocker affects the L0/L2/L3/L4 surface; they're isolated
to the L1 readiness-on-Windows story and the optional fast-fiber-
switch.  The **default** fiber path (Win32 fibers) is fully working.

### IOCP round 2: native overlapped (COMPILED, NOT RUNTIME-VERIFIED)

`src/io/io_iocp.c` was taken from the round-1 readiness emulation
(WSAEventSelect + WaitForMultipleObjects, hard-capped at 64 handles,
~60% of native IOCP throughput) to a native completion-port design:

* The single `CreateIoCompletionPort` / `GetQueuedCompletionStatusEx`
  pair is the only wait primitive -- the **64-handle cap is removed**.
* Socket readiness uses the **AFD poll fast path** the round-2 plan
  called for: `\Device\Afd` opened with `NtCreateFile`, associated
  with the port, one `IOCTL_AFD_POLL` (`NtDeviceIoControlFile`) armed
  per socket and re-armed after each completion (level-triggered).
  This is the wepoll/libuv design.
* Wakeup is `PostQueuedCompletionStatus`; file AIO is overlapped
  `ReadFile`/`WriteFile` reaped from the same port.
* The OVERLAPPED-ownership rule is enforced: kernel owns the
  OVERLAPPED from accept to completion-dequeue; deregistering an
  armed socket cancels (`NtCancelIoFileEx`) and defers the free to the
  reap.  Registration nodes are separately heap-allocated with stable
  addresses so the kernel-held back-pointer never dangles.

**Verified on the Linux dev host:** cross-compiles clean with
mingw-w64 gcc 14.3.0 `-std=c11 -Wall -Wextra` and links into a PE32+
binary against `-lntdll -lws2_32`.  The Linux build is unaffected
(`io_iocp.c` is `XTC_IO_BACKEND_IOCP`-only; it is an empty TU
everywhere else, and the full C munit suite stays green on epoll/
uring).

**NOT runtime-verified.**  The backend has not executed on Windows.
The AFD poll correctness, the level-triggered re-arm, the cancel/
lifetime under churn, and the wakeup ordering must be validated on
santorini before this is production quality -- same reviewed-but-
untested status as `src/io/io_aix.c`.  The exact test plan is in
`docs/M_WINDOWS_MATRIX.md`.

**Native ARM64 compile-check (2026-06):** `io_iocp.c` and
`tls_schannel.c` now COMPILE `-c` cleanly (0 warnings at `/W4` and
`-Wall -Wextra`) on a real Windows 11 ARM64 host under clang 20, MSVC
19.44 (VS17), MSVC 19.50 (VS18), and MinGW gcc 13.2 (x64).  MSVC needs
`/std:c11 /experimental:c11atomics`.  No portability bugs surfaced;
neither file needed changes.  They remain COMPILED-not-RUN: no runtime
execution was attempted (no full library link / configure run).

The round-1 test-side gap (item 1 above) is unchanged: the m2 io_*
tests still need a tcp-socketpair shim because IOCP/AFD operate on
sockets, not anonymous CRT pipes -- this is orthogonal to the backend
rewrite.

### Unix domain sockets on Windows (COMPILED, NOT RUNTIME-VERIFIED)

`src/io/io_net.c`'s four UDS entry points are implemented on Windows
via `AF_UNIX` (`<afunix.h>`, Windows 10 build 17063+) instead of
returning `XTC_E_NOSYS`.  Listen/dial mirror POSIX; Windows `AF_UNIX`
has no peer-credential channel, so the creds API returns the bytes
with uid/gid 0.  Cross-compiles clean with mingw-w64; not yet run on
a Windows host (details in `docs/M_WINDOWS_MATRIX.md`).

## Native stack backtrace (xtc_dump / panic handler)

The backtrace seam (`src/os/os_backtrace.c`, header
`src/inc/os_backtrace.h`) selects a backend at configure time in this
priority order.  Per-platform symbolization status:

| Backend | Platforms | Output | Verified |
|---|---|---|---|
| execinfo | glibc, macOS, FreeBSD/NetBSD/OpenBSD | symbolized frames (function name + offset + module) | RUNTIME-VERIFIED on Linux glibc x86_64: `test_backtrace` green, full panic/dump path exercised |
| builtin `_Unwind_Backtrace` + dladdr | musl and any libc without `<execinfo.h>` whose toolchain provides the EH-ABI unwinder (libgcc_s / LLVM) -- effectively every GCC/Clang target | symbolized frames best-effort (dladdr names + module) | RUNTIME-VERIFIED on REAL musl (x86_64-unknown-linux-musl gcc 14.3): `os_backtrace.o` references only `_Unwind_Backtrace`/`_Unwind_GetIP` (auto-linked from libgcc_s, NO `unw_*` / NO `-lunwind`), the walk produces a real trace.  Also forced+verified on glibc. |
| libunwind + dladdr | OPT-IN (`--with-libunwind=yes`) or last resort where neither execinfo nor a working `_Unwind_Backtrace` exists | symbolized frames best-effort (dladdr names + module) | RUNTIME-VERIFIED on Linux glibc x86_64 by forcing the backend and linking against the system libunwind: produced a real symbolized trace and passed `test_backtrace`. |
| libunwind, no dladdr | as above, dladdr absent | frame ADDRESSES only (no names) | RUNTIME-VERIFIED on Linux glibc x86_64 with dladdr disabled: `test_backtrace` falls back to asserting the frame count and passes |
| DbgHelp | Windows (`_WIN32`, MinGW/MSVC) | symbolized frames via `CaptureStackBackTrace` + `SymFromAddr` | COMPILED-NOT-RUNTIME-VERIFIED.  Cross-compiles clean with mingw-w64 `-Wall -Wextra -Wpedantic` and links with `-ldbghelp`, but no Windows host in the CI matrix runs it.  Reviewed against the Win32 DbgHelp API docs; same status as `src/io/io_aix.c`. |
| stub | any platform with none of the above | empty backtrace (length 0) | the dump degrades to proc/loop/mailbox state with the note "no backtrace backend" |

Notes:

* Priority: execinfo > builtin `_Unwind_Backtrace` > libunwind > stub.
  execinfo wins wherever present (symbolized, signal-safe).  On an
  execinfo-less libc (musl) the BUILTIN unwinder is now the default --
  it needs no third-party library, only the compiler's own EH-ABI
  unwinder (libgcc_s / LLVM), auto-linked with no `-l` flag.  This
  DEMOTES libunwind to opt-in: `--with-libunwind=yes` forces it above
  the builtin tier (still below execinfo); `--with-libunwind=auto`
  (default) uses it only when both execinfo and the builtin unwinder
  are unavailable.  A stock musl build no longer links libunwind.
* The execinfo, builtin-unwind, and libunwind emit paths are async-
  signal-safe (the first via `backtrace_symbols_fd`, the latter two via
  a hand-rolled formatter feeding `write(2)`; dladdr name lookup is
  best-effort, not formally signal-safe, and is only an enrichment on
  top of the raw
  address).  The DbgHelp `Sym*` family is single-threaded and is
  serialized under a critical section; it is intended for the
  panic/abort path, not arbitrary in-flight signals.

## Future port matrix items

* AIX `pollset_*` -- same pattern as illumos's `port_*`, simpler
  semantics.  ~150 LOC.
* macOS -- should mostly work via kqueue + Mach-O fcontext asm
  variant.
* CMake-on-Windows alternative configure path so MSYS2-pacman or
  Strawberry Perl users without autoconf can build.
* Native ARM64 Windows (replace x64-emulated MinGW with llvm-mingw
  aarch64).  PARTIALLY DONE (2026-06): the fcontext asm is
  runtime-verified native ARM64 (clang 20 + both MSVC) and the IOCP/
  SChannel/core TUs compile-check native ARM64 clean; the remaining
  work is a full native-ARM64 library build + the m2 runtime suite.
