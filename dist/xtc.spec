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
Version:        1.50.0
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
* Thu Sep 24 2026 Greg Burd <greg@burd.me> - 1.50.0-1
- Production-readiness release (PLAN.md 19.27).  MINOR: contract changes
  and new APIs.  Behavior changes: a TLS CLIENT verifies by default (opt
  out with XTC_TLS_VERIFY_NONE); xtc_sup_join returns XTC_E_AGAIN for a
  live supervisor; several error codes made consistent (see
  debian/changelog).
- Fixes two lock-manager mutual-exclusion violations (since 1.0), an
  xproc use-after-free and missing reap, fork-without-exec child wedges,
  SIGPIPE killing the host on all TLS backends and in xtc_net, TLS
  host-name checks on GnuTLS/wolfSSL/mbedTLS, supervisor cleanup
  overlap, and an allocator mismatch leaking park timers.
- New: xtc_app_shutdown, xtc_app_drain_on_signal,
  xtc_cfg_get_string_copy, xtc_res_attach_mctx, xtc_res_attach_net.

* Mon Sep 22 2026 Greg Burd <greg@burd.me> - 1.49.5-1
- Fixes three real data races on the fiber-migration path, found by
  auditing ThreadSanitizer on the io_uring backend -- a configuration CI
  had never covered, because the gating TSan job is epoll-only and
  excludes the heavy-migration suites.
- proc: the striped proc table decremented n_used while holding only ONE
  stripe lock, where the allocate path holds ALL of them, so a decrement
  could be lost.  Not merely a statistic: after a table grow the allocate
  path uses n_used as the first fresh slot index, so a drifted count
  either fails a spawn with XTC_E_RESOURCE or targets an occupied slot.
  A defensive bounds check is what kept it from corrupting the table.
- task: park_timer was a plain pointer written both by the parking fiber
  and by the timer-expiry path on the loop thread, which clears it after
  the wake has already made the task runnable.
- proc: the alive flag was a plain int cleared by the exiting fiber and
  read as a liveness gate by foreign threads (wake, send, link/monitor,
  exit-deadline, proc-info).
  All three were benign in observable effect -- both stores wrote the same
  value, or a stale read cost only a harmless no-op -- but each was
  undefined behavior, and a racy read is indistinguishable from one that
  would not be benign.  All three are now _Atomic with relaxed ordering.
- ci: added a non-gating uring heavy-migration TSan job so the next race
  on this path surfaces in CI rather than by hand, and NARROWED the
  suppression list -- a stanza naming the per-fiber entry frame would
  have hidden a brand-new race, and removing it cost no coverage.
- No API or ABI change: 710 exported symbols, none added or removed, and
  every public struct layout and enum value identical to 1.49.4 (the
  changed structs are internal or file-private).

* Mon Sep 22 2026 Greg Burd <greg@burd.me> - 1.49.4-1
- Fixes an io_uring deadlock reported from a PostgreSQL fiber workload: a
  deferred cross-thread fd unregister left its node linked in the io's fd
  list until the owning loop's next poll drained it, so a same-fd
  re-register was rejected as a duplicate (XTC_E_INVAL, surfaced as
  XTC_E_INTERNAL).  Because only the owning thread can drain that queue,
  a caller that retried never returned to its poll and every retry failed
  identically -- a deadlock, not a transient error.  xtc_io_reg_fd now
  applies a queued delete for that fd first.
- io_uring is the only backend that queues deferred deletes; the other
  seven pass straight through, so none was affected.
- xtc_proc_wait_fd now propagates the real registration error
  (XTC_E_INVAL / _NOMEM / _NOSYS / _RESOURCE) instead of flattening every
  cause to XTC_E_INTERNAL, which is now reserved for genuinely unexpected
  returns.  Its documented return set also states that one fd admits one
  waiter per loop, that no non-OK return is a wake to retry, and that the
  auto-unregister holds across migration and cancellation with no
  caller-driven drain or poll.
- No API or ABI change: 710 exported symbols, none added or removed, and
  every public struct layout and enum value identical to 1.49.3.

* Mon Sep 22 2026 Greg Burd <greg@burd.me> - 1.49.3-1
- Fixes the two MSVC regressions that made 1.49.2's Windows gate red, plus
  pre-existing signed-overflow UB in the DST tests, and corrects the
  xtc_exit_pid_deadline status contract.  No API/ABI change: 710 exported
  symbols, none added or removed, and every public struct layout identical
  to 1.49.2 (the one new enum value is appended).
- slab: the Windows chunk shim used plain malloc, which guarantees only
  max_align_t, where POSIX mmap is page aligned -- so a 64-byte-aligned
  redzone object came back 16-mod-64 and /m11/slab/redzone_alignment failed
  on MSVC while passing on Linux.  The POSIX-side redzone fix in 1.49.2 was
  therefore only half a fix.  Now _aligned_malloc at page granularity, with
  the matching _aligned_free.
- test: removed a POSIX truncate(2) call that does not exist on MSVC, where
  /WX promoted the implicit declaration to a build error.  It was redundant
  -- the following fopen(..., "w") already truncates.
- test/sim: fixed signed integer overflow (undefined behavior) in the
  order-sensitive hash fold used by 30 simulation tests.  Pre-existing since
  at least 1.48.0 and only visible once a sanitized DST job existed; the fix
  is value-preserving, so every pinned replay hash is unchanged.
- proc: xtc_exit_pid_deadline's XTC_KILL_DELIVERED no longer claims that
  at-exit hooks have COMPLETED.  The exit path clears `alive` before running
  them, so a parked hook left cleanup in progress while the caller was told
  it had finished.  DEFERRED/TIMEOUT/unknown-pid qualifications documented
  too.  Documentation-only; no behavior change.
- proc, reg: a dropped DOWN/EXIT notification or registry monitor enrollment
  is no longer silent -- both emit XTC_TAIL_LIFECYCLE_DROP.  The bounded
  mailbox is correct backpressure; a lifecycle event losing that race being
  invisible was not.  Makes the loss diagnosable, not impossible.
- msvc: the build gate discarded both compiler and test output, so a failure
  on the one platform with no local host to reproduce on named only the
  binary.  Both are now printed.

* Mon Sep 22 2026 Greg Burd <greg@burd.me> - 1.49.2-1
- Bug-fix release from an external code review.  Seventeen reproduced
  defects across the I/O registration lifetime, cancellation/park paths,
  config sessions, arena groups, the allocator/slab/resource accounting,
  and the deterministic-simulation oracles.  No API or ABI change: the
  public symbol set (710) and every public struct layout are byte-identical
  to 1.49.1, verified mechanically.
- Highlights: an io_uring use-after-free/double-free on
  register-modify-delete-poll; cfg sessions were per-OS-thread rather than
  per-fiber, so two fibers on one carrier read each other's settings; arena
  group discard could reset memory while a member was still live; five
  cancellation defects including a killed waiter leaking its fd
  registration and an at-exit hook looping forever after a kill; an
  aligned-allocation size overflow; slab destructors running twice; and a
  resource-cap overflow that admitted past the cap.
- Also: several test oracles that could pass without testing anything
  (a partition that cut no traffic, a corruption check that could not
  fail, a planted-bug gate that credited any nonzero exit) now fail when
  the property they name is broken, and release publication is gated on
  the tested revision.

* Tue Sep 16 2026 Greg Burd <greg@burd.me> - 1.49.1-1
- doc: man-page coverage for the 1.49.0 additions (the man-coverage gate
  requires every PUBLIC function be documented; v1.49.0's tag landed on
  the commit before that fix, so this supersedes it).

* Tue Sep 16 2026 Greg Burd <greg@burd.me> - 1.49.0-1
- proc: xtc_mask_enter/xtc_mask_leave -- paired (callback-free) form of the
  cancellation mask, for macro-pair bridges like START/END_CRIT_SECTION.
- cfg: per-session scoping with a transactional override stack (SET LOCAL /
  rollback / nesting) and source precedence, so xtc_cfg can back GUCs.
- mctx: arena groups (xtc_arena_group_*) -- discard a cohort's shared state
  wholesale on kill, two-phase (all members gone, THEN reset the arena).

* Wed Sep 16 2026 Greg Burd <greg@burd.me> - 1.48.1-1
- proc: xtc_proc_sleep had the same wrong-proc bug as xtc_proc_wait_fd; it
  cancelled and re-armed ANOTHER fiber's park timer, stranding that fiber
  with no timer at all.  Both now share one __proc_reanchor helper.

* Wed Sep 16 2026 Greg Burd <greg@burd.me> - 1.48.0-1
- proc: fix a wrong-proc bug in xtc_proc_wait_fd that stranded fibers on
  kqueue backends (FreeBSD/macOS) under migration + blocking offload.
- io: xtc_io_event_t carries the fd an event is for; task->park_fd is
  atomic and its unregister is claimed exactly once.
- tools: xtc-stranded flags an fd park on a CQ-overflowed ring.
- test: the doc-snippet gate links -lrt when present (FreeBSD).

* Tue Sep 16 2026 Greg Burd <greg@burd.me> - 1.47.0-1
- alloc: __os_alloc_set_hook COPIES the vtable and requires all six
  callbacks (aligned_free was unchecked); fixes a stack-use-after-return
  when a caller registered a stack local.
- proc: xtc_proc_info reports the MAILBOX park (it always reported NONE,
  making a healthy xtc_recv park indistinguishable from a lost wake).
- proc: new xtc_exit_pid_deadline reports DELIVERED/DEFERRED/TIMEOUT, and
  xtc_proc_info exposes the cancellation-mask state.
- doc: xtc_send's wake guarantee is now stated; xtc_proc(3) documents the
  stance on killing a fiber that mutates shared state.

* Sat Sep 13 2026 Greg Burd <greg@burd.me> - 1.45.0-1
- tail: xtc_tail is now a dial9-class microscope with dial9-GUI interop.
  xtc_tail_dump_dial9 emits the dial9 trace wire format (TRC\\0 v1) with the
  scheduler events mapped to dial9's built-in schema names (PollStartEvent,
  TaskSpawnEvent, WakeEventEvent, ...) so a libxtc trace opens in the dial9
  viewer with a native timeline; libxtc-specific events (the io_uring chain)
  ride as custom events.  xtc_tail_from_env enables recording from
  XTC_TAIL_ENABLE with no code change; xtc_tail_spill_dial9 writes a segment
  file for a sidecar to ship off a deployed box.  New guide chapter
  (Observing a running application) documents the deployed + GUI workflow.
* Fri Sep 11 2026 Greg Burd <greg@burd.me> - 1.44.1-1
- aio: FIX the cross-loop lost wake -- a migratable fiber's async-file
  completion lands on the ring of the loop it submitted on, and if the fiber
  then migrates away that loop could fail to poll its own ring, stranding the
  completion (and the fiber) indefinitely.  The aio waiter now nudges the
  submitting loop (xtc_loop_wake, lost-wake-free) whenever it resumes on a
  different loop with the op still pending.  DST regression test
  test_sim_aio_migrate (24 seeds, migratable fibers, cross-loop completions).
* Thu Sep 10 2026 Greg Burd <greg@burd.me> - 1.44.0-1
- tail: complete the lost-wake instrument chain -- XTC_TAIL_PARK_TASK (9),
  XTC_TAIL_REAP (10), XTC_TAIL_SUBMIT (11), XTC_TAIL_SUBMIT_FAIL (12), so a
  fiber that parks and never runs can be localized to exactly one step.
- tail: detect SHORT io_uring submits (the return is a COUNT, not just an
  errno), which previously read as success while an SQE never reached the
  kernel.
- tools: xtc-tail.py --strands classifies every parked task; xtc-rings gains
  an ovf column because unreaped saturates and cannot see the kernel overflow
  list; xtc-tail-dropped reads the ring from a hung process.
- orc: FIX the supervisor's spawn-then-monitor window (reported): a child
  that faulted inside it was misreported as the "benign" XTC_DOWN_NOPROC
  rather than a signal, and TRANSIENT children were restarted after a clean
  exit.  __spawn_child now uses xtc_proc_spawn_monitor; NOPROC no longer
  counts as abnormal.  Enforced by test_api_discipline.sh RULE 5.
- test: new gates test/tools/test_xtc_tail_strands.sh and
  test/tools/test_gdb_cqes.sh; de-flaked the sqlxtc differential oracle
  (fixed sleep -> bounded connect retry).
* Tue Sep 08 2026 Greg Burd <greg@burd.me> - 1.43.0-1
- tail: XTC_TAIL_WAKE is now EMITTED (was declared-but-unused since Phase
  1) from the completion-dispatch hook, with detail = the task pointer as
  a join key.  PARK/WAKE/RUN together localize a lost wakeup to either
  side of dispatch.  Behaviour change for anyone filtering kind 2.
- tail: new public xtc_tail_dropped() -- records evicted by ring wrap, so
  a real zero is distinguishable from an overwritten one.
- tail: XTC_TAIL_LOOP_POLL is now emitted only for an IDLE poll, and from
  both the loop and executor poll sites.  It was 87-93% of the ring at 32
  loops and evicted the events it exists to explain.
- tools: xtc-tail.py renders kind 8; xtc-procs prints the task pointer.

* Mon Sep 08 2026 Greg Burd <greg@burd.me> - 1.42.0-1
- tail: new XTC_TAIL_LOOP_POLL event (SCHED source) -- per-loop I/O poll
  liveness, so "a fiber's park has no matching RUN because its loop
  stopped" is observable rather than inferred.  Additive enum value at
  the end (8); existing values unchanged.

* Sun Sep 07 2026 Greg Burd <greg@burd.me> - 1.41.1-1
- fix(sched): do not report a loop idle while it holds runnable work
  (n_alive counts HOMED tasks, so a foreign-homed task in this loop's
  queue made it claim idle and enter the backoff sleep; 165,733
  false-idle decisions measured in one 8-loop run).
- fix(sched): re-check wake_pending after publishing PARKED, closing a
  window where a waker latching between the consume and the state store
  left a fiber PARKED with nobody left to consume the latch.
- io(uring): DIAGNOSTIC assertion that all SQ submits come from the
  ring's owner thread.
- tail: record the aio park/resume, so a lost completion wake is visible
  as a PARK with no matching RUN.
- tools: new xtc-stranded and xtc-rings debugger commands.

* Fri Sep 05 2026 Greg Burd <greg@burd.me> - 1.41.0-1
- New public API: xtc_ncpus, xtc_numa_nnodes, xtc_numa_node_of_cpu,
  xtc_numa_current_node (minor bump).
- fix(sched): consult the RUNNING loop's preempt ring, not the fiber's
  home loop -- a work-stolen fiber mutated a peer loop's io_uring ring.
- fix(evt): latch wake_pending on the same-loop wake path too, closing a
  dropped-wake window measured at ~1 in 7200 wakes.
- fix(evt): plug an exec->loop_node leak on the xtc_exec_init unwind.
- Security/robustness: cap pid local ids at 65536 (uint16_t truncation),
  bracket chash RCU reads, hook all four determinism-guard classes.
- io(uring): attach the preempt ring's io-wq to the loop's main ring.

* Mon Sep 01 2026 Greg Burd <greg@burd.me> - 1.40.7-1
- fix(io): record each loop's io-backend owner thread eagerly at worker startup (__xtc_io_set_owner) so a work-stolen fiber's xtc_proc_wait_fd cleanup always defers the cross-loop fd-unregister to the owning thread. A home loop that had all its fibers work-stolen kept io->owner_set==0 (it never blocked in xtc_io_poll), so foreign cleanups deleted its fds inline and two concurrent dels corrupted the single-owner io->fds list into a cycle -- an infinite loop in xtc_io_del_fd stranding a fiber holding a lock, plus an occasional fini double-free. The 6th cross-loop-migration surface. No API change.
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
