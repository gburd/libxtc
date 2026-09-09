# ADR-0002: Hegel-c property-based tests as a first-class layer

- **Status:** Accepted, but **DORMANT since 2026-09** -- the upstream API
  this ADR builds on was replaced and the tier cannot run.  See
  "Dormancy" below.  The decision itself (properties are first-class) is
  NOT reversed; only its implementation is stalled.
- **Date:** 2026-05-25 (dormancy recorded 2026-09-09).
- **Supersedes:** --
- **Related:** [`0001-test-first-claim-driven.md`](0001-test-first-claim-driven.md).

## Context

`PLAN.md` (S)7.2 commits the project to property-based tests via
[hegel-c](https://github.com/gburd/hegel-c) (**now deprecated and
read-only** -- see "Dormancy") for every concurrency
primitive: MPSC ordering, mailbox selective receive, deque
linearizability, timer monotonicity, supervisor restart intensity,
future combinators, allocator ownership, RCU, LRLock, and `xtc_cfg`.

Through M0-M3 we wrote munit unit tests but no PBTs.  This ADR
formalises the gap and the remediation.

## Decision

Property-based tests are a **first-class layer** in `make check`,
parallel to the munit suite.  The test tree adds:

```
test/pbt/
|--- pbt_common.h        scaffolding + SKIP-mode stub
|--- pbt_atomic.c        M1 atomics
|--- pbt_alloc.c         M1 allocator
|--- pbt_timer.c         M3 timer subsystem
\--- pbt_run_queue.c     M3 task run queue
```

(That was the M3 shape.  The tree has since grown to 17 suites --
async, chan, chash, cskip, deque, lrlock, lwlock, proc, saga,
sched_shares, scope, sim, slab in addition to the four above -- carrying
36 property definitions in total.)

Each milestone's `M*_CLAIMS.md` table now has explicit PBT-tagged
rows alongside the munit unit-test rows.  The doctrine from ADR-0001
extends: *every property listed in the plan must have a hegel
test that asserts it.*

### Build integration

- `--with-hegel=PATH` configure flag (default: disabled).  When
  enabled, the PBT binaries link against the local hegel-c library
  and the `XTC_HAVE_HEGEL` macro switches their bodies on.
- `--with-hegel-server=CMD` overrides how the test harness launches
  the hegel server (default: `hegel` on PATH).  This accommodates
  setups where `hegel` is invoked via `uv run`, a wrapper script,
  or a containerised service.
- When `--with-hegel` is **off**, every PBT binary still compiles
  (against a stub in `pbt_common.h`) and prints
  `[PBT] <suite> SKIP (--with-hegel was not configured); N
  properties unverified`.  This means the default `make check` is
  green even on machines without hegel installed.
- `dist/configure` includes a probe that locates `libcbor` and
  `libz` even on Nix-style systems where they are not on the
  default linker search path.

### Where PBTs go

| Layer | Properties to express in hegel |
|---|---|
| L0 atomics | linearizability of fetch_add and CAS, store/load round-trip |
| L0 allocator | malloc-writeable, realloc-preserves-contents, hook accounting balanced |
| L1 io | (M5+) backend equivalence under random fd-readiness sequences |
| L2 timer | monotonic fire order, cancel-then-no-fire, exactly-once delivery |
| L2 run queue | each task runs exactly its target number of times |
| L2 wakers | (M5+) cross-thread wake under K writers and one reader |
| L3 channels | (M7+) MPSC send-order preserved, no message loss |
| L3 mailbox | (M8+) BEAM selective-receive ordering with arbitrary skip patterns |
| L3 lock manager | (M13c) any random sequence of acquire/release/promote honors the conflict matrix |
| L3 deadlock detector | (M13c) every cycle in a random waits-for graph is detected; no false positives |
| L4 supervisor | (M10) restart-intensity bound is honored |
| L4 RCU | (M13a) writer never observes a stale reader |

The list is not exhaustive; new primitives must come with new
properties before they can ship.

## Consequences

### Positive

- The plan's PBT commitment becomes mechanically enforced rather
  than aspirational.  Properties drift only when their tests do.
- Bugs that emerge under random load (timing, concurrency, edge
  values) are caught at normal CI tempo, not at soak time.
- Hegel's automatic shrinking gives developers minimal
  counterexamples for free, which keeps the time-to-debug short.
- The SKIP-by-default mode keeps barriers to entry low for
  contributors without hegel installed.

### Negative

- Bigger test runtime: 660 generated examples in M0-M3 take a few
  seconds.  At the lock-manager scale (M13c) this will be minutes.
  Mitigation: split `make check` (fast path) from `make check-pbt`
  (slow path) once the suite gets heavy.
- A second optional dependency (hegel-c library + Python server)
  to maintain.  Mitigation: pin via the Nix flake; keep
  configure's autodetection forgiving.
- Some properties are awkward to express purely in
  `hegel_assume`: spawning threads and joining them inside a
  hegel body is heavyweight.  Mitigation: keep PBT bodies focused;
  the deepest concurrency tests stay in munit + soak.

## Implementation status (M3)

- [x] `--with-hegel` configure flag with libcbor/libz autodetection
- [x] `pbt_common.h` SKIP-mode stub
- [x] M1 atomics: 3 properties (`fetch_add_sum`, `cas_loop_sum`, `store_load_roundtrip`)
- [x] M1 allocator: 3 properties (`malloc_writeable`, `realloc_preserves`, `hook_balanced`)
- [x] M3 timer: 1 property (`random_timers` -- fires once, in order, cancels respected)
- [x] M3 run queue: 1 property (`each_task_runs_target_times`)
- [x] All PBT binaries integrated into `make check` via `tests-pbt`
- [x] Total: 36 property definitions across 17 suites (measured 2026-09-09
      by summing what each binary prints at runtime).  **All 36 are
      currently UNVERIFIED** -- see "Dormancy" below.  Earlier revisions of
      this ADR and of `README.md` quoted 8 and 23; both were stale counts
      that grew without being re-measured.
- [x] `pbt_saga` wired into `TESTS_PBT` (2026-09-09).  It had been written
      in `2f05478` but never added to the build list, so its 1 property was
      neither compiled nor counted -- an orphaned suite is indistinguishable
      from a passing one, which is the same failure mode as a silently
      skipped property.

## Dormancy (recorded 2026-09-09)

The tier does not run, and cannot, as written.  Recorded here because an
ADR that keeps asserting a dead mechanism works is worse than no ADR.

**What broke.**  This ADR is built on
[gburd/hegel-c](https://github.com/gburd/hegel-c), whose model is a
*client that forks a server*: `hegel_session_new()` pipes to a `hegel`
subprocess (located via `HEGEL_SERVER_COMMAND` or `PATH`), then drives
properties with `hegel_run_test()` / `hegel_test_fn` /
`HEGEL_DEFAULT_SETTINGS`.  That repository is now deprecated and
read-only, and its socket protocol was removed upstream.  Worse, the two
halves never actually interoperated: the client speaks the removed socket
protocol while `hegel-core`'s `hegel` binary speaks stdio, so every PBT
skipped at runtime even on a machine that had both installed.  Verified
directly on 2026-09-09: with `--with-hegel` pointed at a locally built
hegel-c, `configure` accepts it and the suites *link*, then fail at
`hegel: handshake failed (error -2)` -> `cannot start hegel session`.  So
the SKIP was never merely "hegel not installed"; the mechanism was
non-functional.

**The replacement.**  [hegeldev/hegel-rust](https://github.com/hegeldev/hegel-rust)
ships `libhegel`, a pure **in-process C-ABI shared library**: generation,
shrinking, and the example database all live inside the `.so`.  There is
no subprocess, no socket, and nothing to put on `PATH`.  Its shape is a
context/run model rather than a session/test-fn model:
`hegel_context_new` -> `hegel_run_start` -> `hegel_next_test_case` ->
`hegel_mark_complete` -> `hegel_run_result`, with explicit paired frees
for every handle.

**What reviving it costs.**  Not a flake edit -- a harness rewrite:

1. Rewrite `test/pbt/pbt_common.h` from the session/server model to the
   context/run model.  This is the bulk of the work; the 36 property
   bodies themselves are mostly portable since they are ordinary C using
   `hegel_assume`, but the driver, settings, and result reporting all
   change shape.
2. Change `dist/configure.ac`: `--with-hegel=PATH` currently expects a
   hegel-c *source root* containing `include/hegel/hegel.h` and
   `build/libhegel.{a,so}`, and probes for `libcbor`/`libz`.  libhegel
   needs neither of those transitive deps and installs a flat
   `include/hegel.h` + `lib/libhegel.so`.  `--with-hegel-server=CMD`
   becomes meaningless and should be retired.
3. Add the library to `flake.nix`.  **A working model already exists**:
   `~/ws/lime/flake.nix` consumes libhegel **0.36.5** as pinned prebuilt
   release artifacts (`libhegel-<os>-<arch>` plus `hegel.h`), one
   `fetchurl` hash per platform, with `patchelf` fixing the soname and a
   generated `hegel.pc` so meson's `dependency('hegel', required: false)`
   resolves.  It deliberately avoids `rustPlatform.buildRustPackage`
   because that fetches ~140 crates at build time and breaks offline
   `nix develop`.  Copy that derivation rather than re-deriving it.

**Interim posture.**  The SKIP is deliberately LOUD and stays that way:
each suite prints `N properties unverified` and `tests-pbt` prints a
summary warning that a green `make check` does not mean the properties
hold.  That is the honest state.  `README.md` was corrected on 2026-09-09
to stop counting the properties as coverage; it had reported them as
delivered test coverage, which was the actual integrity problem -- a
skipped property that is *counted* is worse than one that is absent,
because it inflates confidence rather than merely failing to add to it.

Future milestones must add their PBT rows in the same step as
their unit-test rows, per ADR-0001.

## Notes

This is a retrofit: M0-M3 shipped without it.  The discipline
going forward is that **a milestone is not "complete" until every
PBT-tagged claim has a passing hegel test**.  M4 onward observes
this rule from the start.
