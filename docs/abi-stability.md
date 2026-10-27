---
title: ABI stability
parent: Reference
nav_order: 6
lede: >-
  What stays fixed across releases, what is mechanically enforced, and
  what is only a stated intention.
permalink: /reference/abi-stability/
---
This document is the **compatibility contract** for libxtc, and it is
deliberately written in two voices:

- **ENFORCED** -- a gate in `make check` / CI fails if the rule is
  broken.  Every such rule names the gate.
- **POLICY (not mechanically enforced)** -- the maintainer's stated
  intention.  Nothing in the tree checks it.  Treat it as a promise
  backed by review, not by tooling.

An earlier version of this page described symbol-version maps,
capability strings, a prior-release compat suite, and a trace-shape
diff suite as if they existed.  They did not, and do not.  They have
been removed rather than left as false advertising; what remains is
what a packager can verify in the tree.

## Version numbering (POLICY)

Versioning is `MAJOR.MINOR.PATCH`.  The intent:

- **PATCH** (`1.49.x`).  Bug fixes only.  Same ABI, same on-disk
  formats, same trace shape.  Drop-in replacement.
- **MINOR** (`1.x.0`).  New features, new APIs.  Additive only:
  nothing removed, nothing renamed.  New `XTC_E_*` codes only at the
  end of the enum; existing codes never change value.  New lock modes
  never inserted in the middle of an enum.

  **The additive-ABI promise is currently BROKEN for caller-allocated
  option and info structs**, which have grown (and in two cases had
  fields inserted mid-struct) during 1.x.  This is a real defect, not
  a footnote: see
  [Known issues]({{ '/reference/known-issues/' | relative_url }}) for
  the measured sizes, the two severity classes, and the consumer rule.
  Until it is addressed, **recompile consumers against the headers of
  the exact minor whose library they link**.
- **MAJOR** (`x.0.0`).  Breaking changes allowed.  Cadence is
  intentionally slow.

No tooling verifies any of this.  There is no release gate that diffs
the exported symbol set, the struct layouts, or the function
signatures against the previous tag.  If you need that guarantee for a
packaging decision, run `abidiff`/`abi-compliance-checker` yourself
between the two tags you care about; libxtc does not do it for you.

## What the shared library actually exports (ENFORCED)

There is **no per-symbol version map**.  `dist/libxtc.map` is a single
unnamed version node, so every exported symbol is **unversioned**; the
file says so in its own header comment, and defers per-symbol
versioning to a later day.  Consumers link against the SONAME
(`libxtc.so.MAJOR`), and the policy above -- not the linker -- governs
compatibility.

What *is* enforced, by `test/m0/test_symbols.sh` in `make check` and
CI:

- `[C5]` every symbol defined in `libxtc.a` is `xtc_*`, `__xtc_*`, or
  `_`-prefixed.  No stray global names.
- `[C6]` installed public headers do not `#define` standard or bare
  identifiers.
- `[C7]` installed public headers declare no `__`-prefixed internal
  function, except the small allowlist a public *macro* expands to.
- `[C8]` the shared library's dynamic symbol table is exactly `xtc_*`
  plus that same macro-backed allowlist -- so the whole `__os_*`
  substrate and every other internal stays private.  `[C8]` also
  asserts the allowlist agrees with `[C7]`'s.

Per-platform link recipes (in `dist/configure.ac`):

- **ELF (Linux, the BSDs, illumos)**: `-Wl,-soname,libxtc.so.MAJOR`
  plus `-Wl,--version-script=dist/libxtc.map` (export restriction,
  not symbol versioning).
- **macOS**: `-install_name`, `-compatibility_version MAJOR.0`,
  `-current_version FULL`, and `-Wl,-exported_symbols_list,libxtc.exp`
  -- the Mach-O equivalent of the ELF export list, generated from the
  archive by `dist/Makefile.in`.
- **Windows**: the per-commit build is `xtc.lib` (static).  There is no
  hand-curated `xtc.def` and no ordinal stability commitment.

## Man-page and header agreement (ENFORCED)

The documentation side of the contract *is* gated:

- `test/m0/test_man_coverage.sh` -- every `PUBLIC:` function across the
  installed header set is documented in some `man3` page.
- `test/m0/test_man_signatures.sh` -- a function's page mentions every
  parameter name from its declaration and documents the return
  contract.
- `test/m0/test_man_lint.sh` -- mdoc lints clean.
- `test/m0/test_docs_abi.sh` -- every tool, path, and gate script this
  page cites exists in the tree, and the page does not claim machinery
  that is absent.  (That gate was added because this page previously
  described five nonexistent files and it passed anyway.)
- `test/docs/test_doc_snippets.sh` -- every code snippet in the docs
  compiles and runs against the freshly built library.

## Frozen surfaces (POLICY)

The lock layer is the widest-consumed surface, so it is committed
frozen: `xtc_lwlock_t` / `xtc_lwlock_mode_t` and the `xtc_lwlock_*`
entry points; `xtc_lrlock_t` and `xtc_lrlock_*`; `xtc_lockmgr_t`,
`xtc_locker_t`, `xtc_lock_mode_t`, `xtc_lockmgr_opts_t`,
`xtc_lockmgr_stat_t`, `xtc_lock_req_t` and the `xtc_lockmgr_*` /
`xtc_lock_*` entry points.  Both the signatures and the layout of
those option/stat structs are meant to hold across 1.x.

Verified by hand for this release: none of those structs has changed
size or field order since v1.0.0.  But **no gate checks it**, and the
struct-growth defect above shows that hand review is not sufficient on
its own.  If a new optional field is ever needed there, it must go
through a new struct or a versioned `_ex` entry point, never an
in-place layout change.

## Deprecation lifecycle (POLICY -- machinery NOT present)

The intended lifecycle for removing a public API, one minor release
per stage at minimum:

| Stage | Behaviour | Intended signal |
|---|---|---|
| 1. Live | Documented, supported. | Nothing. |
| 2. Soft-deprecated | Documented, supported. | Doc note. |
| 3. Deprecated | Supported, discouraged. | Compiler warning. |
| 4. Default-off | Opt-in build flag required. | Build error without the flag. |
| 5. Removed | Header `#error`'d; symbol absent. | Build error always. |

**None of the compiler/runtime machinery exists yet.**  There is no
`XTC_DEPRECATED_SOFT` or `XTC_DEPRECATED` attribute macro in the
headers, no `xtc_cfg.warn_deprecated` knob, and no
`-DXTC_ENABLE_DEPRECATED` build mode.  Nothing has been deprecated in
1.x, so the machinery has not been needed; it will be added with the
first real deprecation.  Until then, stages 2-4 are documentation
notes only.

## No capability query API

Applications sometimes want to ask the library what it can do rather
than hard-coding a version.  **libxtc has no such API today** -- there
is no `xtc_have_capability()` and no capability-string table, in any
form.  (An earlier version of this page showed example calls to one.
They never compiled.)

What you can query today, from the public API:

- `xtc_version_string()` / the `XTC_VERSION_*` macros -- the version.
- `xtc_runtime_info()` -- loop count, CPU counts, NUMA node count.
- The configure-time `xtc_config.h` defines (`XTC_IO_BACKEND_*`,
  `XTC_HAVE_*`) -- which backend a given build selected.

Because the I/O backend and coroutine substrate are chosen at
**configure** time, not runtime, "which backend am I on" is a
compile-time fact for a statically linked consumer.  A consumer that
swaps a shared libxtc underneath itself has no runtime way to ask, and
should pin the minor.

## On-disk and wire formats

libxtc defines no database format.  It does emit:

- The `xtc_tail` event-trace dump (`xtc_tail_dump`), a 24-byte header
  of magic + version + flags + count + base timestamp, read by
  `tools/xtc-tail.py`.
- The dial9 wire form of the same trace (`xtc_tail_dump_dial9`),
  magic `TRC\0` + version.
- `xtc_dump(fd)` -- a human-readable diagnostic dump, not a parsed
  format, with no compatibility commitment.

Both trace formats carry a version field, so a reader can reject or
adapt.  **POLICY:** a format change bumps that field rather than
silently reinterpreting bytes.  There is no `xtcdump` or `xtcadmin`
tool, no flight-recorder `*.flt` format, and no admin-socket protocol
in this tree; earlier claims about them were removed.

## Trace shape (POLICY)

Dashboards and runbooks bind to event names.  Intended: event names
stable across minors, new event kinds may be added in a minor,
renaming or removing one is a major change.  **No suite captures or
diffs trace shape**, so this rests on review.

## Long-term support and the 2.0 LTS (POLICY)

This section is the support contract for the first long-term-support
release.  It is POLICY: the dates are commitments, but nothing in the
tree enforces them; the compat job in CI (below) is the one mechanical
piece.  The ABI rules a 2.x release must keep, and the tiers that say
which headers they cover, are in the sections above.

**The 2.0 LTS line.**  2.0.0 is the first LTS release.  The line is
2.0.0, 2.0.1, 2.0.2, ...; it is supported until the LATER of

- 18 months after 3.0.0 is released, and
- 2.0.0's release date plus 3 years.

"Supported" means security fixes and fixes for data loss, crashes,
hangs and wrong results, released as 2.0.x.  The end date is printed in
SECURITY.md's table once 3.0.0 has a date.  Minors after 2.0 (2.1, 2.2,
...) are not LTS: each is supported until the next minor ships, and a
consumer moves forward within 2.x without a rebuild (the 2.x ABI
promise above).  A later major may name its own LTS; the next one is
announced with the major, never retroactively.

**What a 2.0.x patch may change.**  Fixes only.  A patch release:

- adds, removes or renames no exported symbol and changes no function
  signature, enum value, macro value, or struct layout (the layout
  gate's table is unchanged, and the release abidiff, abi-check.sh,
  against the previous 2.0.x reports status 0);
- keeps every on-disk, wire, trace and probe format byte for byte
  (below);
- changes no documented default, no configuration name, and no error
  code a function returns for a given condition -- unless the old
  behaviour was the bug, and then the release notes say so;
- adds no dependency and raises no minimum compiler, kernel or TLS
  library version;
- may change behaviour that contradicts the documentation, close a
  security hole, fix a leak, a race or a hang, and change performance
  when the fix needs it (the release notes name any such cell from
  scripts/perf-gate.sh).

A fix that needs more than this ships in the next minor, and only the
minimal safe part (if any) is backported.

**Backports.**  The 2.0 line lives on the branch `lts/2.0`, cut from the
2.0.0 tag.  Every fix lands on `main` first, then is cherry-picked
(`git cherry-pick -x`, so the `main` commit is named) to `lts/2.0`; a
fix that does not apply cleanly is rewritten on the branch and its
message says why.  Tags `v2.0.N` are cut from `lts/2.0` only.  The
release procedure for a patch release is in `docs/PUBLISHING.md`
("Patch releases on an LTS branch").

**Downgrades.**  A downgrade -- running an older release against data or
state a newer one has written (for example a sqlxtc store) -- is
supported only between LTS releases, and only to the immediately
previous LTS.  Within the 2.0 line every 2.0.x reads and writes the same
formats, so moving between any two 2.0.x is supported in both
directions.  Upgrades are supported: a release reads what earlier
releases of the same major wrote, unless its notes say otherwise.
Before 2.0.0 there is no LTS release, so no downgrade is supported: a
format may change in any 1.x minor or 2.0 pre-release, and the release
that changes it says so in KNOWN_ISSUES.

**Format, trace and probe versions.**  Every format libxtc writes
carries a version: the `xtc_tail` dump header, its dial9 form, and the
USDT probe set (its provider and argument lists).  The rule:

- a 2.0.x patch never changes a format's version or its bytes;
- a 2.x minor may ADD to a format only in a way an older reader of the
  same major skips (a new event kind, a trailing field), and bumps the
  minor part of the version where the format has one; it may add a new
  probe, never change an existing probe's arguments;
- removing or reinterpreting anything is a major change;
- a reader in any 2.x release accepts every version a 2.x release
  wrote.

**The compat job.**  CI's `compat` job (`scripts/compat-check.sh`)
takes the N-1 consumer from the PREVIOUS 2.x release tag, compiles it
against that release's installed headers, and runs it against the
current `libxtc.a` and `libxtc.so` -- a consumer compiled for 2.0.0 must
keep working against every later 2.x.  It skips, saying why, until a
2.x tag exists (a 1.x consumer is not expected to work against 2.0:
1.x -> 2.0 is the major break).  With the layout gate it is the
mechanical half of this contract; the rest is review.

**Security fixes** follow the process in SECURITY.md: private report,
fix on every supported line, coordinated release.

## See also

- [SECURITY.md](https://codeberg.org/gregburd/libxtc/src/branch/main/SECURITY.md)
  -- supported versions and how to report a vulnerability.

- [Known issues]({{ '/reference/known-issues/' | relative_url }}) --
  the struct-growth ABI defect, in detail, with measurements.
- [`adr/`](adr/) -- architecture decision records.
- [Testing]({{ '/testing/' | relative_url }}) -- how the enforced gates
  are run.
