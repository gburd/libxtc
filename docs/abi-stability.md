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

## Long-term support (POLICY)

Informal, and stated as intent rather than commitment: security fixes
go to the current release; an LTS window on a previous major will be
defined if and when there is a second major.  There is no
`xtc-security@` list, no published embargo policy, and no backport
branch in this tree yet.

## See also

- [Known issues]({{ '/reference/known-issues/' | relative_url }}) --
  the struct-growth ABI defect, in detail, with measurements.
- [`adr/`](adr/) -- architecture decision records.
- [Testing]({{ '/testing/' | relative_url }}) -- how the enforced gates
  are run.
