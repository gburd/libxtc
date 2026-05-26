# Cross-platform port status

Updated end of M_PORT round 4.

## Per-platform tally (this round)

| Platform | Build | Tests passing |
|---|---|---|
| **Linux x86_64** (epoll/uring/gcc14) | clean, no warnings | **151 / 151** munit + 11 PBT + 22 shell |
| **FreeBSD 15 amd64** (kqueue/clang19) | clean, no warnings | full test suite tracked across rounds |
| **OpenIndiana SPARC** (poll *and* port_*/gcc13) | clean | 132/132 with native event-port backend (full parity) |
| **Windows 11 ARM64 / MinGW-W64 x64** (IOCP/gcc) | clean | 31/36 — test_fctx now passes (Win64 fcontext asm working); 4 io_* tests blocked on IOCP CRT-fd-vs-HANDLE mismatch (round 3 work); 1 test_proc subtest needs separate fix |
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

* `src/evt/coro_winfiber.c` — Win32 fiber substrate (CreateFiberEx /
  SwitchToFiber / ConvertThreadToFiber).  Mirror of `coro_uctx.c`.
* `src/io/io_iocp.c` — real IOCP backend.  Wakeup uses
  `PostQueuedCompletionStatus` with completion-key `XTC_IOCP_KEY_WAKEUP`.
  User fds use `WSAEventSelect` + `WaitForMultipleObjects` for
  readiness emulation (round 2 will swap this for AFD/
  `NtDeviceIoControlFile` for native-IOCP performance).
* `src/io/io_common.c` — Windows path drops the socket-pair self-pipe
  entirely (IOCP wakeup is the post-completion).
* `src/os/os_alloc.c` — Windows routes ALL allocations through
  `_aligned_malloc` / `_aligned_realloc` / `_aligned_free` (alignment
  16) so the hook surface remains symmetric.
* `src/os/os_cpu.c` — Windows uses `GetSystemInfo`.
* `dist/configure.ac` — adds `-lws2_32` to LIBS on `*-mingw*`/
  `*-cygwin*`/`*-msys*`.
* `src/os/asm/fctx_x86_64_sysv.S` — guarded with `#if !defined(_WIN32)`
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

Neither blocker affects the L0/L2/L3/L4 surface; they're isolated
to the L1 readiness-on-Windows story and the optional fast-fiber-
switch.  The **default** fiber path (Win32 fibers) is fully working.

## Future port matrix items

* AIX `pollset_*` — same pattern as illumos's `port_*`, simpler
  semantics.  ~150 LOC.
* macOS — should mostly work via kqueue + Mach-O fcontext asm
  variant.
* CMake-on-Windows alternative configure path so MSYS2-pacman or
  Strawberry Perl users without autoconf can build.
* Native ARM64 Windows (replace x64-emulated MinGW with llvm-mingw
  aarch64).
