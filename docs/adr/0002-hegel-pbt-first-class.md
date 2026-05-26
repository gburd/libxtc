# ADR-0002: Hegel-c property-based tests as a first-class layer

- **Status:** Accepted (M3 retrofit; carried forward from M4 onward).
- **Date:** 2026-05-25.
- **Supersedes:** —
- **Related:** [`0001-test-first-claim-driven.md`](0001-test-first-claim-driven.md).

## Context

`PLAN.md` §7.2 commits the project to property-based tests via
[hegel-c](https://github.com/gburd/hegel-c) for every concurrency
primitive: MPSC ordering, mailbox selective receive, deque
linearizability, timer monotonicity, supervisor restart intensity,
future combinators, allocator ownership, RCU, LRLock, and `xtc_cfg`.

Through M0–M3 we wrote munit unit tests but no PBTs.  This ADR
formalises the gap and the remediation.

## Decision

Property-based tests are a **first-class layer** in `make check`,
parallel to the munit suite.  The test tree adds:

```
test/pbt/
├── pbt_common.h        scaffolding + SKIP-mode stub
├── pbt_atomic.c        M1 atomics
├── pbt_alloc.c         M1 allocator
├── pbt_timer.c         M3 timer subsystem
└── pbt_run_queue.c     M3 task run queue
```

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

- Bigger test runtime: 660 generated examples in M0–M3 take a few
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
- [x] M3 timer: 1 property (`random_timers` — fires once, in order, cancels respected)
- [x] M3 run queue: 1 property (`each_task_runs_target_times`)
- [x] All PBT binaries integrated into `make check` via `tests-pbt`
- [x] Total: 8 hegel properties, 660 generated examples per run

Future milestones must add their PBT rows in the same step as
their unit-test rows, per ADR-0001.

## Notes

This is a retrofit: M0–M3 shipped without it.  The discipline
going forward is that **a milestone is not "complete" until every
PBT-tagged claim has a passing hegel test**.  M4 onward observes
this rule from the start.
