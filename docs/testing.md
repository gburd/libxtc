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

- **92 simulation tests** (`test/sim/`) run on every CI push over the
  sim I/O backend, plus a nightly 100,000-seed swarm across the combined
  fault set. Run the full DST suite locally with `make check-dst` (a
  separate `--with-io-backend=sim` build; not part of plain `make check`
  since it configures and builds a second copy of the library).
- **Every failing seed is pinned.** `test/sim/corpus/seeds.txt` lists
  each seed a sweep has ever failed on, with the fixing commit, and
  `scripts/dst-corpus.sh` replays the whole corpus on every CI push.
- **The right metric is bug-detection latency, not coverage.** A
  bug-injection harness (`scripts/dst-bug-inject.sh`) plants each of
  the known safety bugs listed in `src/inc/xtc_dst_inject.h` (a
  dropped wakeup, a skipped fsync, a conflicting lock grant, and about
  fifty more) and asserts that the simulator *catches* each one, for
  the reason it was planted, within a seed budget (64 by default). A
  planted bug the sweep misses is a hole in DST coverage and fails the
  build. This is
  exactly how FoundationDB and TigerBeetle back the claim "our simulator
  finds real bugs."
- **Determinism is mechanically enforced.** A guard traps any use of the
  real clock, the environment, or the raw thread id from sim-reachable
  code, so a test cannot silently become non-deterministic; every sim
  run proves its own determinism. The RNG is not trapped but
  virtualized: an un-seeded draw takes the run's own seeded stream.

### Simulating your own application

The simulator is a supported (experimental) API, not only libxtc's own
test rig: `xtc_sim.h` is installed, and a library configured
`--with-io-backend=sim` installs as `libxtc-sim.a` with `xtc-sim.pc`, so
a test of your program links it with `pkg-config --static --libs
xtc-sim`.  Spawn your procs on an `xtc_exec`, run
`xtc_sim_exec_run(exec, seed, budget)`, check your invariants, and run
the seed again to prove the replay.  Draw scripted choices from
`xtc_sim_rand` (an application stream that never moves the schedule),
mark decision points with `XTC_SIM_BUGGIFY("name")`, and plant a known
bug behind `XTC_DST_BUG(n)` to measure how fast a sweep catches it.  A
simulated device hands out an fd it owns and makes it ready at a seeded
virtual time with `xtc_sim_fd_ready_at`, so a fiber parked in
`xtc_accel_wait_fence` runs under the simulator unchanged; a completion
a real OS thread produces is given a seeded delivery time with
`xtc_sim_external_begin` / `xtc_sim_external_complete`.  The full
surface is `xtc_sim(3)`.

{% include snippet.html file="20_dst_app.c" region="full" %}

The [jobq example]({{ '/examples/jobq/' | relative_url }}) takes this all
the way, for a job queue with leases and fencing tokens.  Its harness
gives each seed a fault mix and a nemesis that kills processes and cuts
loops off the network.  It checks safety and liveness invariants at
quiescence, measures the fault space each sweep covered, and carries
three planted bugs that each fail within seven seeds.  Its page lists
the steps to adopt DST for your own program and the simulator's limits
to design around.

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
as a first-class testing tool. The PBT suite links
[`libhegel`](https://github.com/hegeldev/hegel-rust), Hegel's in-process
C-ABI library: generation and shrinking run inside the test binary, with
no server. The `test/pbt/` suite states 39 properties in 18 suites
across the layers:

- deque and run queue: a pop/steal race partitions the items, and every
  task runs exactly its target number of times;
- channels: oneshot round trip, MPSC send order preserved;
- processes: every spawn completes, send/recv round trip, FIFO order;
- chash / cskip: equivalence with a model under random operations;
- lrlock / lwlock / atomics / timers / slab / allocator / scope / saga /
  scheduler shares / the simulator's PRNG and clock: their core
  invariants under generated input.

PBT is a build-time option (`--with-hegel`, found through pkg-config,
or `--with-hegel=PREFIX`; `nix develop` provides libhegel). The same
test files compile without it and each suite prints a loud SKIP naming
how many properties went unverified, so a green run without the option
says nothing about them. CI runs the tier with libhegel in its own job,
which gates the workflow. The commitment to PBT is recorded in
[ADR 0002](https://codeberg.org/gregburd/libxtc/src/branch/main/docs/adr/0002-hegel-pbt-first-class.md).

## Sanitizers

Every CI push runs the full `make check` under
**AddressSanitizer** (`detect_leaks=1:abort_on_error=1`) and
**UndefinedBehaviorSanitizer** (`print_stacktrace=1:halt_on_error=1`),
with gcc and with clang, and a separate job runs the core of the DST
tier (eleven tests: scheduler, replay, the determinism guard, faults)
under both.  ASan catches use-after-free, buffer overflow, and leaks; UBSan
catches signed overflow, misaligned access, and the other C undefined
behaviors that compile quietly and fail on a different target. A latent
UBSan misalignment once rotted this project's CI for weeks, which is why
the sanitizer jobs are gating, not advisory.

ThreadSanitizer is run on the cross-thread paths (the wake path is
TSan-clean).  Because the runtime is a cooperative fiber scheduler, TSan
needs each coroutine represented as a TSan *fiber object* (via
`__tsan_create_fiber` / `__tsan_switch_to_fiber` / `__tsan_destroy_fiber`)
so happens-before is tracked per fiber instead of collapsing every
coroutine into one confused thread.  The coro substrates emit those
calls under `-fsanitize=thread` (guarded by `XTC_TSAN_FIBERS`), so a
full `clang -fsanitize=thread` build of libxtc runs the fiber suite
(`test_fctx`, `test_async`, `test_proc`, `test_svr`, `test_chan`) with
zero TSan warnings.  This is **clang-only**: the fiber-identity API is a
clang compiler-rt feature; gcc's `libtsan` does not implement it, so a
gcc TSan build emits no fiber annotations (and the ASan stack-switch
API, a distinct model, is used only under AddressSanitizer).  A
Valgrind job runs memcheck, Helgrind, and DRD over a chosen subset of
the C tests and memcheck over the DST tier; the full munit suite is too
slow under Valgrind to run whole.

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
every CI push on Linux (io_uring, epoll, gcc and clang, glibc and musl,
x86-64 and AArch64), macOS (kqueue), FreeBSD (clang, kqueue, in a VM),
and riscv64 (under qemu-user); Windows CI builds the library with MSVC
and runs a smoke test and about a hundred munit tests on the IOCP
backend.  It is re-verified by hand, outside CI, on illumos (big-endian
sparcv9, event ports) and on Windows with MinGW.  Building on big-endian SPARC
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
