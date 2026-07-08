---
title: Testing
nav_order: 4
has_children: false
permalink: /testing/
lede: >-
  Why you should trust a piece of software this foundational -- and the specific methods that earn that trust.
---

1. TOC
{:toc}

---

libxtc is the kind of code you build *on*: a runtime, a scheduler, locks,
crash recovery. A bug here is not a bug in one feature -- it is a bug
under everything you write. Trust in software like this is not asserted,
it is **earned**, method by method, and the honest thing to do is show
the methods rather than the adjectives. This page is that accounting.

## Deterministic simulation testing (DST)

The centerpiece. Concurrency bugs -- races, lost wakeups, ordering
violations, torn writes across a crash -- are the ones that "pass on my
machine" and fail in production, because they depend on an interleaving
you did not happen to hit. A stress test finds the interleavings that
occur on the box you ran it on; it is not reproducible and it does not
search adversarially.

DST removes the luck. libxtc can replace the real scheduler and real I/O
with a **deterministic simulation** driven by a seed: the same process
code runs, but scheduling, timers, message ordering, and injected faults
(partitions, latency, disk errors, `ENOSPC`, torn writes, fsync loss,
clock skew, machine death and reboot) all unfold reproducibly from that
seed. A failing seed reproduces the *exact* failure, every time, on any
machine -- so a race becomes an ordinary, debuggable, fixable bug.

This is the discipline
[FoundationDB](https://apple.github.io/foundationdb/testing.html) proved
out and [TigerBeetle](https://tigerbeetle.com/) made a headline feature;
[Tina](https://github.com/pmbanugo/tina) applies the same idea to a
stackless runtime. libxtc treats it as the primary defense, not an
extra:

- **46 simulation tests** (`test/sim/`) run every commit in CI over the
  sim I/O backend, plus a nightly 100,000-seed swarm across the combined
  fault set.
- **The right metric is bug-detection latency, not coverage.** A
  bug-injection harness (`scripts/dst-bug-inject.sh`) plants a known
  safety bug -- a dropped wakeup, a skipped fsync, a lock-conflict error
  -- and asserts the simulator *catches* it from a seed. A planted bug
  the sweep misses is a hole in DST coverage and fails the build. This is
  exactly how FoundationDB and TigerBeetle back the claim "our simulator
  finds real bugs."
- **Determinism is mechanically enforced.** A guard traps any use of the
  real clock, real RNG, environment, or thread id from sim-reachable
  code, so a test cannot silently become non-deterministic; every sim
  run proves its own determinism.

DST has already found and fixed real library bugs -- a lock-manager
bad-free reachable only when a lock was held at destroy, a cross-thread
wake race, buffer-manager pin races in the sqlxtc example. Those are in
[Known issues]({{ '/reference/known-issues/' | relative_url }}), documented
rather than buried.

## Property-based testing (PBT)

Where DST hunts *interleavings*, property-based testing hunts *inputs*.
Instead of a handful of hand-picked cases, you state a property that must
hold for all inputs -- a round-trip, an invariant, a contract -- and the
framework generates hundreds of randomized cases and **shrinks** any
failure to a minimal reproducer.

libxtc uses [**Hegel**](https://hegel.dev), a property-based-testing
library based on [Hypothesis](https://github.com/hypothesisworks/hypothesis),
as a first-class testing tool. The PBT suite links against the official
C binding, [`hegel-c`](https://github.com/hegeldev/hegel-c), driven by
the `hegel` protocol server. The `test/pbt/` suite states properties
across the layers:

- deque and run-queue: never lose or duplicate an item under random
  push/steal sequences;
- channels: MPSC send order preserved, no message loss;
- mailbox: BEAM selective-receive ordering under arbitrary skip patterns;
- lock manager: any random acquire/release/promote sequence honors the
  conflict matrix;
- deadlock detector: every cycle in a random waits-for graph is detected,
  with no false positives;
- lrlock / lwlock / atomics / timers / slab / allocator: their core
  invariants under generated load.

PBT is a build-time option (`--with-hegel=PATH`, pointing at a built
[`hegel-c`](https://github.com/hegeldev/hegel-c) tree; it links
`libhegel` plus its `libcbor` and `zlib` dependencies and runs against
the `hegel` server). The same test files compile
to no-ops without it, so the suite is always present and turns on where
Hegel is available. The commitment to PBT is recorded in
[ADR 0002](https://codeberg.org/gregburd/libxtc/src/branch/main/docs/adr/0002-hegel-pbt-first-class.md).

## Sanitizers

Every commit runs the full C test suite under
**AddressSanitizer** (`detect_leaks=1:abort_on_error=1`) and
**UndefinedBehaviorSanitizer** (`print_stacktrace=1:halt_on_error=1`) in
CI. ASan catches use-after-free, buffer overflow, and leaks; UBSan
catches signed overflow, misaligned access, and the other C undefined
behaviors that compile quietly and fail on a different target. A latent
UBSan misalignment once rotted this project's CI for weeks, which is why
the sanitizer jobs are gating, not advisory.

ThreadSanitizer is run on the cross-thread paths (the wake path is
TSan-clean); Valgrind memcheck is a documented gap on the roadmap, not a
claim we make today.

## Claim-driven, test-first

Every claim in the code or the documentation is meant to be backed by a
test -- the discipline recorded in
[ADR 0001](https://codeberg.org/gregburd/libxtc/src/branch/main/docs/adr/0001-test-first-claim-driven.md).
That extends to this manual: every code snippet in the
[Guide]({{ '/guide/' | relative_url }}) is a real file that is compiled
and run in `make check` (a
[release gate](https://codeberg.org/gregburd/libxtc/src/branch/main/test/docs/test_doc_snippets.sh)),
and every public function must have a man page or the build fails.

## Cross-platform runtime verification

A concurrency runtime that only works on the developer's laptop is not
trustworthy. The suite is **runtime-verified** (not merely compiled) on
Linux (epoll + io_uring) and macOS (kqueue) every commit in CI, and
re-verified out-of-CI on FreeBSD (clang, kqueue), illumos (big-endian
sparcv9, event ports), and Windows (IOCP). Building on big-endian SPARC
in particular validates byte-order assumptions in the codecs, hashing,
and atomics that a little-endian-only test would never exercise. The
honest per-platform status -- what runs versus what only compiles -- is
in [Known issues]({{ '/reference/known-issues/' | relative_url }}).

## The stance

The [ABI]({{ '/reference/abi-stability/' | relative_url }}) page says what
we promise to keep stable; [Known issues]({{ '/reference/known-issues/' | relative_url }})
says what is imperfect. Between them and the methods above, the goal is
that you can verify our claims rather than take them on faith -- which is
the only basis on which foundational software deserves to be trusted.

---

Next: [Benchmarking]({{ '/benchmarking/' | relative_url }}) &rarr;
