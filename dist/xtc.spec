# dist/xtc.spec -- RPM spec for libxtc.
#
# Copyright (c) 2026, The XTC Project
# Use of this source code is governed by the ISC License.
#
# Build:
#   rpmbuild -ba dist/xtc.spec \
#       --define "_sourcedir $PWD" \
#       --define "version 1.28.1"
# (or set Version: below and point Source0 at a release tarball).

%global sover 0

Name:           libxtc
Version:        1.29.0
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
* Wed Jul 22 2026 Greg Burd <greg@burd.me> - 1.28.1-1
- fix: xtc_task_waker() names the CURRENT loop, not the stale spawn loop -- a migratable proc's waker previously kept naming its spawn-time loop after being work-stolen, so a wake could target the wrong loop; usually self-healing but a permanent strand under fast shutdown. Confirmed + adversarially-proven regression test. No API change.

* Wed Jul 22 2026 Greg Burd <greg@burd.me> - 1.28.0-1
- exec: xtc_exec_get_service_mode/_get_eager_rebalance getters + a documented policy-knob convention.
- docs: xtc_proc(3) documents xtc_exec_loop_id() as the migration-detection idiom.
- test: closed a DST-coverage gap for eager rebalance (real production steal path now driven under the deterministic simulator).
- REVERTED: fcontext involuntary preemption (v1.26.0) -- intermittent memory corruption under concurrent migratable-proc steal traffic on the fcontext substrate (default on Apple Silicon/musl). ucontext substrate unaffected; fcontext correctly falls back to Phase 1 cooperative preemption as before v1.26.0.
- No breaking API changes.

* Tue Jul 21 2026 Greg Burd <greg@burd.me> - 1.27.0-1
- exec: xtc_exec_set_eager_rebalance (opt-in, off by default) -- makes migratable procs rebalance under a realistic parked-fiber load (a run-queue-empty loop steals before blocking + an idle-peer nudge on migratable-work production). Only migratable tasks move; pinned work unaffected. Unblocks the PostgreSQL work-stealing idle-reclamation case.
- No breaking API changes.

* Tue Jul 21 2026 Greg Burd <greg@burd.me> - 1.26.0-1
- proc: public per-proc userdata (xtc_proc_set_userdata/_userdata) -- opaque void* on the calling proc, survives work-stealing migration; unblocks the PostgreSQL migration case.
- preempt: involuntary preemption (Phase 2b) now works on the fcontext substrate too (not just ucontext) -- available on every Linux target regardless of substrate, incl. musl and forced-fcontext, x86_64 + aarch64.
- fix: macOS/arm64 xtc_dump() from a live fiber no longer risks SIGBUS (fiber-stack-aware walker never falls back to the unbounded backtrace()).
- No breaking API changes.

* Mon Jul 20 2026 Greg Burd <greg@burd.me> - 1.25.0-1
- proc: xtc_proc_opts_t.migratable (opt-in; default pinned/unchanged) -- a migratable proc's coroutine is work-stealable across loops on a multi-loop executor; identity/supervision/recovery/mailbox survive the carrier change, proven under DST (test_sim_migratable). Unblocks work-stealing for supervised backend procs.
- inc: internal __ decls moved out of installed public headers into *_int.h (enforced by a new [C7] gate).
- abi: shared library exports narrowed to the public surface only (xtc_* + macro-backed recovery symbols); no __os_*/__xtc_* internal leak (enforced by a new [C8] gate).
- No breaking API changes.

* Mon Jul 20 2026 Greg Burd <greg@burd.me> - 1.24.0-1
- TLS: expand xtc_tls_* for PostgreSQL adoption -- tri-state verify_peer_mode, cipher_list/ciphersuites_13/groups, crl_file/crl_dir, prefer_server_ciphers, passphrase_cb; server hardening as defaults; post-handshake introspection (version/cipher/bits/ALPN/peer-cert DN+CN+issuer+serial) incl. RFC 5929 tls-server-end-point channel-binding hash. Additive; OpenSSL backend fully implemented, others stubbed.
- OS: dedicated errno abstraction (M1.5) -- __os_errno_map + embedder hook (__os_errno_set_hook/_get_hook), consolidating duplicated per-file errno->XTC_E_ tables.
- No breaking API changes.

* Wed Jul 15 2026 Greg Burd <greg@burd.me> - 1.23.3-1
- Fix (scheduler): drain due timers under a busy run queue -- a never-empty run queue (a busy xtc_yield / RESCHED spin) could starve xtc_proc_sleep / recv-timeout / any deadline indefinitely; timers now fire on the IO-fairness quantum under load. No API change.

* Wed Jul 15 2026 Greg Burd <greg@burd.me> - 1.23.2-1
- Fix (macOS/arm64): unify the fcontext-default substrate guard across all four sites (coro_fctx.c, coro_uctx.c, coro_int.h include + struct member) so struct xtc_coro's layout cannot drift between translation units; the 1.23.1 flip updated only two sites. Non-Apple targets + amalgamation byte-unchanged.

* Wed Jul 15 2026 Greg Burd <greg@burd.me> - 1.23.1-1
- macOS/Apple-Silicon now defaults to the Mach-O arm64 fcontext coroutine substrate (was ucontext), removing the per-switch sigprocmask syscall; override with -DXTC_CORO_FORCE_UCONTEXT. Amalgamation + macOS x86-64 keep ucontext.

* Wed Jul 15 2026 Greg Burd <greg@burd.me> - 1.23.0-1
- macOS/Apple-Silicon: --enable-shared builds a Mach-O dylib (Darwin branch, configure-selected); opt-in Mach-O arm64 fcontext substrate; fixed the intermittent xtc_dump() SIGBUS (fiber-stack-bounded backtrace). Packaging: Debian -dev ships all headers; shell gates skip without autoconf; BSD-make-parseable Makefile; deeper macOS CI (make check + -Werror + install/shared smoke).

* Wed Jul 15 2026 Greg Burd <greg@burd.me> - 1.22.1-1
- Fixed a pre-existing cross-thread data race on the receive waker (recv_waker read outside mbox_lock in __mbox_deliver, racing the receiver re-arm); the wake now copies the waker under the lock. Added the proc-table stress test to the tsan-fibers CI gate + fault-injection coverage for the xtc_svr_reply OOM path.

* Tue Jul 14 2026 Greg Burd <greg@burd.me> - 1.22.0-1
- Striped per-loop proc-table lock (PG fiber-per-session bottleneck, 19.5c); fixed two lazy-slab-init DCL data races (rcu + proc pools); compositional DST test + proc-table stress; backend-portable cross-thread-wake guard (kqueue coverage); MSVC munit subset (16 tests) promoted to a hard CI gate.

* Tue Jul 14 2026 Greg Burd <greg@burd.me> - 1.21.0-1
- New xtc_cskip (RCU ordered map/skiplist, lock-free readers, min/floor); xtc_chash DST/PBT/bench (no longer provisional); lock-free cross-thread wake resolver; rcu.c lazy-slab-init data-race fix; pre-release security audit + 2 hardening fixes; riscv64/QEMU preempt de-flake.

* Mon Jul 13 2026 Greg Burd <greg@burd.me> - 1.20.1-1
- Zero-warning build on all targets (gcc/clang/musl/MSVC/sanitizers) + -Werror//WX enforcement.

* Sun Jul 12 2026 Greg Burd <greg@burd.me> - 1.20.0-1
- Windows IOCP AFD poll: IOCTL-code + stale-errno + sync-completion fixes; FlsAlloc slab magazine validated on MSVC.

* Sun Jul 12 2026 Greg Burd <greg@burd.me> - 1.19.0-1
- Layering fix + s_layer gate; removed L5 PG adapter from the plan; Windows FlsAlloc slab magazine; FreeBSD + RISC-V CI.

* Sun Jul 12 2026 Greg Burd <greg@burd.me> - 1.18.0-1
- Code-quality cleanup: coro-substrate dedup (coro_common.h), park-timer helper, library free-discipline consistency.

* Sun Jul 12 2026 Greg Burd <greg@burd.me> - 1.17.0-1
- TSan fiber-identity annotations (clang) for the coro substrates; io_common/svr coverage tests.

* Sun Jul 12 2026 Greg Burd <greg@burd.me> - 1.16.0-1
- Windows xproc deadlock fix + pg_threads.h portability layer (rwlock split, call_once, thread_atexit, static lock initializers).

* Sat Jul 11 2026 Greg Burd <greg@burd.me> - 1.15.0-1
- Windows xtc_xproc control channel via a dedicated reader thread (IOCP socket-readiness workaround); benchmark placement-artifact note.

* Sat Jul 11 2026 Greg Burd <greg@burd.me> - 1.14.0-1
- Fiber-stack pool (spawn mprotect elimination), x86_64-Windows MSVC validation of xtc_xproc, test_sim_pg DST, bench_xproc_fanout.

* Sat Jul 11 2026 Greg Burd <greg@burd.me> - 1.13.0-1
- Proc-teardown refcount (UAF race class fixed), sanitizer fiber-switch annotations, Windows xtc_xproc port, xtc_xlink, xtc_tail compact-portable format + MSG source + offline viewer.

* Fri Jul 10 2026 Greg Burd <greg@burd.me> - 1.12.0-1
- Crash-aware registry (reaper + register_mon + svr_call_name), cross-fork xtc_xproc, xtc_tail microscope (phase 1).

* Fri Jul 10 2026 Greg Burd <greg@burd.me> - 1.11.0-1
- OTP behaviours R5-R10: registry dup-keys, xtc_pg, xtc_pool, circuit-breaker example, xtc_chan_demand, xtc_stream.
- Plus xtc_reg_drop_pid, xtc_credit sliding-window regulator; rexis Pub/Sub on xtc_pg; kaka credit self-test on xtc_credit.

* Fri Jul 10 2026 Greg Burd <greg@burd.me> - 1.10.0-1
- OTP behaviours: xtc_fsm (gen_statem), bounded supervisor pool + handle_continue.

* Thu Jul 09 2026 Greg Burd <greg@burd.me> - 1.9.0-1
- Resolve signal-mask, exec_fini-leak, and epoll wake residuals; new xtc_env/rand/str
  API; [C6] header-hygiene gate.

* Wed Jul 08 2026 Greg Burd <greg@burd.me> - 1.8.0-1
- xtc_proc_wake + cross-thread prepare/park wake-miss fix; ssize_t namespace fix;
  Valgrind CI + tag-triggered release workflows.

* Wed Jul 08 2026 Greg Burd <greg@burd.me> - 1.7.0-1
- Migrate PBT to the official hegel-c; fix the primary xtc_exec_fini teardown leak.

* Wed Jul 08 2026 Greg Burd <greg@burd.me> - 1.6.0-1
- Fiber-aware left-right lock (xtc_alrlock_create/_ex); large docs-site expansion
  (in-site API reference, Testing + Benchmarking sections, more diagrams).

* Wed Jul 08 2026 Greg Burd <greg@burd.me> - 1.5.0-1
- Documentation release: Jekyll docs site (GitHub + Codeberg Pages), tested-snippet
  release gate, CI link checker, hero styling + mermaid diagrams.  No code change.

* Tue Jul 07 2026 Greg Burd <greg@burd.me> - 1.4.2-1
- Lost-wakeup + signal-mask + allocator-discipline fixes; public alloc/time/atomic
  API; API-discipline gate; DST additions (stale-data, ENOSPC, bug-injection).

* Mon Jul 06 2026 Greg Burd <greg@burd.me> - 1.4.1-1
- DST bug-injection harness + right-yardsticks steering; M17 both-framings
  fairness; measured DST coverage baseline.

* Mon Jul 06 2026 Greg Burd <greg@burd.me> - 1.4.0-1
- DST-first: determinism enforcement + FDB-parity fault waves (clock skew,
  ENOSPC, stale-data, reboot/incarnation, coverage + consistency checks);
  portable block-device layer; distributed design doc.

* Mon Jul 06 2026 Greg Burd <greg@burd.me> - 1.3.0-1
- Atomic spawn_link/spawn_monitor, self-describing DOWN (xtc_down_decode_ex),
  DST adversarial upgrades (+lockmgr bad-free fix), wake_revents race fix.

* Sun Jul 05 2026 Greg Burd <greg@burd.me> - 1.2.1-1
- Portability (riscv64/FreeBSD/illumos/Win11-ARM64) + carrier-reported
  monitor-DOWN ambiguity and early-fault containment fixes.

* Sun Jul 05 2026 Greg Burd <greg@burd.me> - 1.2.0-1
- Preemption Phase 3 (xtc_launch) + Lever S1 (madvise-on-park); vectored
  scatter/gather AIO (xtc_aio_preadv/pwritev); public xtc_free packaging
  fix; sqlxtc fuzzy checkpoint; conformance W5.  40-test DST sim suite.

* Sat Jul 04 2026 Greg Burd <greg@burd.me> - 1.1.0-1
- DST reach extended toward FoundationDB parity: tnt actor layer
  (incl. cross-shard + timers), L4 supervision/app, resource governance,
  OS-subprocess lifecycle (FDB actor pattern), and crash recovery under
  a multi-primitive composition.  38-test sim suite.

* Sat Jul 04 2026 Greg Burd <greg@burd.me> - 1.0.0-1
- First stable release: FDB-class DST (all concurrency primitives under
  simulation), native preemption, sqlxtc STEAL, and the threaded-PG
  runtime seam.

* Fri Jul 03 2026 Greg Burd <greg@burd.me> - 0.9.0-1
- DST toward FDB parity (network partition, lock-mgr under sim, bufmgr
  + WAL crash-recovery capstone); preemption hang fix + aarch64
  trampoline; M16.1a mock PG backend; full man-page coverage.

* Wed Jul 01 2026 Greg Burd <greg@burd.me> - 0.8.0-1
- Native preemption facility: per-worker
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
  CPU-time timer).
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
