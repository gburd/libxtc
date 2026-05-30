# ABI Stability and Deprecation Policy

This document is the **contract** xtc makes with its users about
what changes between releases and what doesn't.  The full rationale
is in [`../PLAN.md`](../PLAN.md) (S)18; this is the operational summary.

## SemVer with explicit ABI promise

Versioning is `MAJOR.MINOR.PATCH`:

- **PATCH** (`1.4.x`).  Bug fixes only.  Same ABI.  Same on-disk
  format.  Same wire format.  Same tracing-span shape.  Same lint
  surface.  Drop-in replacement.
- **MINOR** (`1.x.0`).  New features, new APIs, new hooks.  ABI
  is **additive only**: nothing removed, nothing renamed, no
  behaviour change for code compiled against the prior minor.
  New `XTC_E_*` codes only at the end of the enum; existing
  codes never change value.  New `xtc_cfg` knobs.  New lock modes
  never inserted in the middle of the enum.
- **MAJOR** (`x.0.0`).  Breaking changes allowed.  Cadence is
  intentionally slow -- we target one major every three to five
  years.  An LTS designation on the previous major is committed
  for at least 18 months past the new major's release.  A
  migration guide and an `xtc-migrate-1to2` tool ship with the
  major.

## Symbol versioning

ABI stability is enforced mechanically by symbol versioning:

- **Linux glibc / Solaris / FreeBSD**: `.symver` directives on
  every public symbol.  `dist/s_abi` generates the version map
  from `dist/pubdef.in`.
- **Windows**: stable ordinals + a hand-curated `xtc.def`.
- **macOS**: `-current_version` and `-compatibility_version`
  matched to SemVer; careful curation of exported symbols.

`dist/s_abi` runs on every release tag:

- Reads the previous tag's exported-symbol set.
- Diffs against the current build.
- A removed or renamed symbol on a non-major bump fails the release.
- A changed function signature on a non-major bump fails the release.
- A new symbol on a patch bump fails the release.

## Frozen surfaces (consumer commitments)

Some surfaces are committed frozen ahead of a consumer's release so a
libxtc point release cannot break them.  These are checked by name on
every release tag, not just diffed:

- **The lock layer (frozen for the PostgreSQL adapter, Phase 1).**
  PostgreSQL backs its LWLock / LockManager with these behind
  unchanged PG APIs, so an ABI wobble is expensive.  Frozen as of
  0.4.0, before PG Phase 1 ships:
    - `xtc_lwlock_t` and `xtc_lwlock_mode_t`, and the `xtc_lwlock_*`
      entry points in `xtc_lwlock.h`.
    - `xtc_lrlock_t` and the `xtc_lrlock_*` entry points in
      `xtc_lrlock.h`.
    - `xtc_lockmgr_t`, `xtc_locker_t`, `xtc_lock_mode_t`,
      `xtc_lockmgr_opts_t`, `xtc_lockmgr_stat_t`, `xtc_lock_req_t`,
      and the `xtc_lockmgr_*` / `xtc_lock_*` entry points in
      `xtc_lockmgr.h`.
  Both the function signatures AND the layout of these option/stats
  structs are under the SemVer guarantee above: no change on a
  non-major bump.  New optional fields, if ever needed, go through
  the five-stage deprecation cycle (a new struct / a versioned
  `_ex` entry point), never an in-place layout change.

## Capability bits, not version checks

Applications never hard-code SemVer numbers in their code.  They ask
for capabilities:

```c
if (xtc_have_capability("io_uring"))      { ... }
if (xtc_have_capability("hooks.v2"))      { ... }
if (xtc_have_capability("lock.intent_modes")) { ... }
```

Capabilities are strings declared in `dist/capabilities.in` and
compiled into the binary.  Adding a capability is a **minor** bump;
removing one is a **major** bump.  Capabilities work even when xtc
is loaded as a shared library and swapped under the application
without a recompile -- the precise scenario long-lived servers face.

## Five-stage deprecation lifecycle

Removal of any public API takes at least five minor releases.  Each
stage is one minor release at minimum.

| Stage | Behaviour | Compiler / runtime signal |
|---|---|---|
| 1. Live | Documented, supported. | Nothing. |
| 2. Soft-deprecated | Documented, supported. | `XTC_DEPRECATED_SOFT` attribute -> compiler note.  Doc note. |
| 3. Deprecated | Supported, discouraged. | `XTC_DEPRECATED` attribute -> compiler warning.  `xtc_cfg.warn_deprecated` (default `true`) logs runtime use. |
| 4. Default-off | Compiles only with `-DXTC_ENABLE_DEPRECATED`.  Runtime behaviour unchanged. | Build error without the flag. |
| 5. Removed | Header `#error`'d; symbol absent. | Build error always.  Migration tool referenced in error text. |

Minimum total span: **five minor releases** (~two years at our
intended cadence).  Documented on the wiki per API.  Every removed
function has a documented replacement.

## Compat test suite -- the past keeps working

`test/compat/` contains compiled-and-runnable copies of every
worked example from every prior 1.x release.  CI builds them
against the **current** source.  They must continue to compile and
pass.  This is the strongest possible statement of "we meant it
about ABI stability."

When a major bump happens, the suite forks: `test/compat/1.x/`
freezes; `test/compat/2.x/` starts populating.  The 1.x suite stays
in CI for the duration of the LTS commitment.

## Trace-shape stability

An often-overlooked dimension of "compat": dashboards, alerts, and
runbooks bind to span names and attributes.

- Span names are stable across minors.  Renaming is a major change.
- New attributes may be added in a minor.  Removing or renaming
  attributes is a major change.
- New span kinds (new hooks, new subsystems) may be added in a
  minor.

A `test/trace_compat/` suite captures the span shape from a set of
canonical workloads at each release tag and diffs against the
current build.  Diffs without a `RELEASE.md` entry justifying them
block the release.

## On-disk and wire formats

Though xtc itself doesn't define database formats (PG does), it
defines:

- The flight-recorder dump format (`*.flt`).
- The crash-dump trace format.
- The `xtc_stat_dump` snapshot format.
- The `xtcadmin` admin-socket protocol.

Each has a **format version byte** in its first byte and a documented
per-version layout.  `xtcdump` reads every format version we ever
shipped, forever.  No flag day.

## Long-term support

We commit (informally for now, formally once we hit 1.0):

- Each MAJOR has at least 18 months LTS past the next MAJOR.
- Security fixes are backported to all in-support releases.
- An `xtc-security@` list with a documented embargo policy.
- Coordinated disclosure with PG's security list when an issue
  affects threaded PG.

## See also

- [`../PLAN.md`](../PLAN.md) (S)18 -- full longevity discussion.
- [`adr/`](adr/) -- architecture decision records.
- [`../M0_CLAIMS.md`](../M0_CLAIMS.md) [D3] -- the test that asserts
  this document covers SemVer + the five-stage deprecation cycle.
