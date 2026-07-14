# Handoff: finish chash tests, implement cskip, whole-library audit, release

Written 2026-07-14 by the lead at ~92% context. This is the
execution-ready plan for the next FRESH session (start it here, read
this file first). Everything below was deliberately NOT started this
session because doing it at 92% context would have forced rushed,
unverified work -- and a release must not sit on a rushed audit.

## STATUS: COMPLETED 2026-07-14 -- v1.21.0 tagged and CI-green.

All four tasks done + extras (each verified, full CI matrix green on
the release commit 9d93ea3, Release workflow succeeded on the tag):
  Task 1 chash coverage -- DONE (DST test_sim_chash + PBT pbt_chash +
    bench_chash; no longer provisional).
  Task 2 xtc_cskip -- DONE (RCU ordered map/skiplist, reviewed against
    ~/ws/skiplist; unit+concurrent+TSan, DST, PBT, bench, man page).
  Task 3 security audit -- DONE (.agent/SECURITY_AUDIT_2026-07.md; no
    remote CVE; 2 hardening fixes: io_net int-cast, tnt arena overflow).
  riscv64 preempt de-flake -- DONE (green first-run on the release CI).
  Task 4 release -- DONE (1.21.0 bumped + tagged v1.21.0 to Codeberg,
    mirrored to GitHub).
  EXTRAS this session (not in the original plan): the PG wake-path
    contention exchange (landed the lock-free __lt_lock fix, then
    root-caused the ACTUAL bottleneck as t->lock per PLAN.md 19.5c,
    replied to the team, deferred that fix to its own session); and
    fixed a pre-existing xtc_rcu double-checked-locking data race found
    by the cskip TSan stress.

Remaining (NOT release blockers, tracked in PLAN.md): 19.5c t->lock
lock-free lookup (the PG bottleneck; needs a resume-vs-exit UAF DST
test), 19.6 lock priority inheritance, proc.c split, blocking-pool UAF,
composition PBT, wake-path option-2 (lock-free mbox_lock).

---

(original plan below, for reference)

Current tip: 6fecbac on main, fully green CI. Version still 1.20.1 in
dist/version.in (the release this plan ends in will bump it).

## Task 1: finish xtc_chash test coverage (it shipped unit-only)

xtc_chash (src/ptc/chash.c, src/inc/xtc_chash.h) is a working
RCU-protected concurrent hash table with 3/3 unit tests
(test/m13/test_chash.c) but NO DST, PBT, or benchmark. Its API:
  int    xtc_chash_create(cmp_fn, hash_fn, size_t initial_cap, out);
  void   xtc_chash_destroy(h);
  int    xtc_chash_get(h, const void *key, void **out_value);
  int    xtc_chash_insert(h, void *key, void *value, void **out_old);
  int    xtc_chash_remove(h, const void *key, void **out_removed);
  size_t xtc_chash_size(h);
Built on xtc_rcu (xtc_rcu_read_lock/_unlock, xtc_rcu_retire,
xtc_rcu_synchronize). Reclamation is via xtc_rcu_retire -- NEVER a
synchronous free of a node/array a reader might still be walking.

Add:
- test/sim/test_sim_chash.c -- model on test/sim/test_sim_rcu.c (and
  test_sim_slab.c / test_sim_pdict.c for the "DST-test a data
  structure" shape). Under a seeded pessimal multi-loop schedule,
  concurrent insert/remove/get must show: no lost update, no double-
  free, no torn/UAF read (a get that overlaps a remove either sees the
  old value or misses -- never a freed pointer), the final contents
  equal a sequential reference model of the same op sequence, and the
  run replays byte-identically from its seed. This is the tier chash
  is currently MISSING and the reason it was flagged "provisional" --
  its RCU-reclamation UAF-safety is the exact thing DST must prove.
- test/pbt/pbt_chash.c -- model on test/pbt/pbt_slab.c / pbt_deque.c
  (pbt_common.h harness). Property: for any random op sequence, final
  contents == a reference std-map-equivalent model; no key lost or
  duplicated; size() exact.
- bench/bench_chash.c -- model on an existing bench_*.c. ops/sec for
  get (read-mostly) and insert/remove at a few thread counts; ideally
  an A/B vs a plain-mutex-protected map to quantify the RCU read-side
  win.
- Wire all three into dist/Makefile.in (TESTS_C/TESTS_PBT + build
  rules, and test/sim/run_sim_tests.sh's for-loop for the DST one --
  see how test_sim_rcu is wired there; chash needs the chash.o object
  which the sim lib already contains since chash.c is in LIB_SRCS now).

## Task 2: implement xtc_cskip (concurrent skiplist) -- NOT STARTED

The other half of PLAN.md 19.18. A concurrent, RCU-protected, ORDERED
map (that's the differentiator from chash: ordered iteration). Design
from Pugh's probabilistic skiplist first principles -- do NOT try to
vendor "~/ws/skiplist", it does not exist in this environment.
- New src/ptc/cskip.c, src/inc/xtc_cskip.h (PUBLIC: markers), built on
  xtc_rcu exactly as chash is (retire, never free inline).
- API shape (mirror chash + ordered ops):
  xtc_cskip_create(cmp_fn, out) / _destroy / _insert / _remove / _find
  / _first / _next (ordered traversal under an RCU read section) /
  _size. Concurrent writers AND lock-free-ish RCU readers.
- man/man3/xtc_cskip.3 (model on xtc_rcu.3 / xtc_chan.3).
- Full test tiers, same as chash: test/m13/test_cskip.c (unit incl. a
  concurrent stress test, ASan+TSan clean), test/sim/test_sim_cskip.c
  (DST: ordered-invariant + no-UAF + replay-identical),
  test/pbt/pbt_cskip.c (property vs an ordered reference model incl.
  ordered-iteration correctness), bench/bench_cskip.c.
- Wire into dist/Makefile.in (LIB_SRCS + LIB_OBJS + compile rule +
  LIB_HDRS_PUBLIC + TESTS_C/TESTS_PBT + run_sim_tests.sh) and
  regenerate ptc_ext.h via dist/s_include. meson.build: NOT needed
  (it builds only the M0 subset -- confirmed this session).
- APPLY THE SIZE-ARITHMETIC LESSON FROM THIS SESSION'S AUDIT: any
  level-array or node allocation sized from a caller value or a
  random level must have an overflow/hang guard (see the chash
  __chash_next_pow2 fix in 6fecbac for the pattern -- a caller-
  supplied huge value must never hang a loop or overflow a size_t
  multiply). The security-sweep gate (test_alloc_overflow_sweep.sh)
  will catch unguarded alloc arithmetic; run it.

DISPATCH NOTE: if delegating cskip to a subagent, this session learned
the hard way (3 lost/failed agents) that large from-scratch tasks lose
all work if they run out of budget. Either do cskip directly, or give
the subagent a generous max_turns AND instruct it to commit its
skiplist skeleton to a file EARLY (before deep test work) so a lost
session leaves something reusable.

## Task 3: whole-library security/CVE audit (the big one, own budget)

This session audited ONLY the new code (found + fixed 3 defense-in-
depth integer-overflow/hang hazards in os_cpu/saga/chash, 6fecbac; no
CVE-class remote issue in new code). The WHOLE-LIBRARY audit is still
owed. Focus areas, in rough priority:
1. Network-facing parse/dispatch paths -- the actual remote attack
   surface: src/io/io_net.c (framing, xtc_net_recv_frame's length
   handling -- there is a max_len OOM guard already, verify it),
   src/orc/svr.c, src/orc/stream.c, the TLS backends' record handling.
   These are where a real CVE would live (remote input -> memory
   unsafety). Read them for: unchecked length -> alloc/memcpy,
   integer overflow in size math, off-by-one in framing, use of a
   freed conn/proc across a yield.
2. xtc_send / mailbox / envelope path (src/ptc/proc.c) -- A2 in
   docs/M_CRITICAL_REVIEW.md already added send-path overflow guards;
   re-verify they still hold and cover xtc_saga/xtc_chash/xtc_cskip's
   new call sites.
3. The allocator wrappers + slab (src/os/os_alloc.c, src/ptc/slab.c)
   -- alignment, double-free, magazine races.
4. AFD/IOCP (src/io/io_iocp.c) -- the OVERLAPPED ownership rule and
   the new repoll-sweep batched probe (throwaway_ov stack lifetime --
   this session already added a STATUS_PENDING guard there, verify).
5. Fault-containment (src/ptc/preempt.c signal handlers) -- async-
   signal-safety of anything reachable from the SIGSEGV/BUS/etc.
   handler.
6. Run the existing gates as part of the audit: test_alloc_overflow_
   sweep.sh, test_api_discipline.sh, and a full ASan + UBSan +
   TSan(clang) make check, plus valgrind. Consider running the
   DST swarm at higher seed count (test_sim_swarm) and the planted-
   bug injection (scripts/dst-bug-inject.sh) as part of qualification.
Fix any real issue; add a defense-in-depth guard where cheap and
regression-free. Document findings (even "audited, clean") in
docs/KNOWN_ISSUES.md or a new docs section so the audit is a citable
artifact, the way FDB/TigerBeetle publish theirs.

## Task 4: qualify + tag + cut the release (ONLY after 1-3 are done+green)

- Version bump: this is a feature release (sagas, crypto, cgroup,
  tuning, chash/cskip, Windows DLL export, MSVC munit, io-models doc,
  NOALLOC lint) -> minor bump 1.20.1 -> 1.21.0. Procedure (from prior
  releases): echo 1.21.0 > dist/version.in; clear dist/autom4te.cache;
  rm dist/configure; autoreconf -i; verify PACKAGE_VERSION='1.21.0';
  bump the README version badge; add debian/changelog + dist/xtc.spec
  changelog entries.
- Qualify: full make check + make check-dst + -Werror gcc/clang +
  ASan + UBSan + clang-TSan + valgrind + amalgamation, all green
  locally; then push and WAIT for CI green on ALL jobs (the 19+ job
  matrix incl. windows-msvc, freebsd, riscv64-qemu, macos, tls-
  backends) -- per AGENTS.md, never tag without CI green, especially
  a release.
- Tag: git tag -a v1.21.0 -m "1.21.0: <one-line summary>"; push to
  origin (Codeberg) AND the GitHub mirror; then push the tag to both.
  Watch the release CI after tagging.
- Update the changelog cadence line in this repo's own tracking.

## What is DELIBERATELY still deferred after this (do NOT let the
## release block on these):
- Lock manager priority inheritance (3 failed subagent attempts;
  lead-implement directly using the design in TEAM_DISPATCH_2026-07-13.md).
- proc.c split by concern (lost twice; dispatch as 3 smaller per-file
  agents, lead pre-enumerates the function map).
- Blocking-pool cross-thread wake UAF (diagnosis started, lost;
  reproducer-first, tighten existing refcount, don't invent new sync).
- Composition PBT suite (test/hegel/composition/).
- WASM-at-Isolate and the VOPR GUI viewer (both parked by the user).
These can ship in a later release; 1.21.0 does not need them.

## KNOWN FLAKE to address before/during release qualification

test_preempt.c:87 (arm_ticks: `xtc_preempt_tick_pending() == 0`)
flakes intermittently on the riscv64-qemu CI job ONLY -- a Phase-2
involuntary-preemption CPU-time-tick timing assertion that is jittery
under QEMU user-mode emulation (the timer fires or doesn't within the
test's window depending on emulation speed). Observed multiple times
this session on unrelated commits; always passes on re-run
(`gh run rerun <id> --repo gburd/libxtc --failed`). The security-
hardening commit 6fecbac hit it (my 3 new tests test_saga/test_chash/
test_cpu_cgroup ALL passed on riscv64 -- the failure was purely this
preexisting unrelated test).

This flaky test is a real (small) quality debt: a genuinely flaky test
erodes the "CI must be green" discipline. Before cutting the release,
FIX the flake properly -- either make the arm_ticks assertion tolerant
of emulation timing (poll for the tick with a bounded retry/timeout
instead of asserting an exact immediate state), or gate that specific
assertion off under QEMU/slow-emulation detection. Do NOT just keep
re-running it. A green release CI should be green on the first run.
