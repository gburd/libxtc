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

- **ENFORCED** -- a gate in `make check` / CI (or the named release
  script) fails if the rule is broken.  Every such rule names the gate.
- **POLICY (not mechanically enforced)** -- the maintainer's stated
  intention.  Nothing in the tree checks it.  Treat it as a promise
  backed by review, not by tooling.

An earlier version of this page described capability strings, a
prior-release compat suite, and a trace-shape diff suite as if they
existed.  They did not, and do not; what remains is what a packager can
verify in the tree.

## Version numbering (POLICY)

Versioning is `MAJOR.MINOR.PATCH`.  The intent:

- **PATCH** (`2.0.x`).  Bug fixes only.  Same ABI, same on-disk
  formats, same trace shape.  Drop-in replacement.
- **MINOR** (`2.x.0`).  New features, new APIs.  Additive only:
  nothing removed, nothing renamed, no field of a public struct moved or
  resized.  New `XTC_E_*` codes only at the end of the enum; existing
  codes never change value.  New lock modes never inserted in the middle
  of an enum.  A minor may APPEND a field to a size-first struct (below):
  that is binary compatible in both directions.
- **MAJOR** (`x.0.0`).  Breaking changes allowed: new SONAME, new base
  symbol version.  Cadence is intentionally slow.

The promise covers headers in the **stable** tier only (see
[Stability tiers](#stability-tiers-enforced)); an experimental header
may change in any minor.

## Caller-allocated structs: size first (ENFORCED)

Every public struct that a caller allocates and the library reads or
writes -- option, spec, callback, info and stats structs -- starts with
`size_t size`, which the caller sets to `sizeof` its own view of the
struct.  Each struct has an initializer that does it
(`XTC_PROC_OPTS_INIT`, `XTC_SUP_OPTS_DEFAULT`, `XTC_TLS_OPTS_INIT`, ...),
or a designated initializer can: `{ .size = sizeof(xtc_proc_opts_t),
.name = "w" }`.

- **Input structs.**  The library copies the caller's first `size`
  bytes over its own defaults, so a field the caller's (older) header
  does not have takes its default -- never the bytes that follow the
  caller's object.  A larger (newer) struct whose unknown tail is all
  zero is accepted; a nonzero tail -- the caller set an option this
  library does not know -- is `XTC_E_VERSION`.  A `size` of 0, or one
  that does not cover the struct's required fields, is `XTC_E_INVAL`.
- **Output structs.**  The library writes at most `size` bytes, and
  zeroes any part of a newer caller's struct beyond what it knows.
- **Structs the library hands out** (an `xtc_proc_info_t` passed to an
  `xtc_inspect_proc_fn`) carry the library's size; check
  `XTC_FIELD_OK(p, field)` (`xtc.h`) before reading a field newer than
  the oldest library you support.
- **Arrays** of size-first structs (`xtc_sup_start`'s children) stride
  by element 0's `size`, and every element must carry the same `size`.
- **`xtc_aio_t`** is read and written in place for its whole flight, so
  its `size` must cover the whole 2.0 layout; later fields are read only
  when `size` covers them.

This is what makes an append-only minor release safe for a consumer
built against an older or newer minor.  `test/abi/test_abi_sized.c`
(in `make check`) plays an N-1 consumer: each struct cut to an earlier
prefix, heap-allocated at exactly that size, so an AddressSanitizer
build turns any read or write past `size` into a failure; plus a newer
struct with a tail, and `size` 0.

Not size-first, deliberately: element types of arrays the library
strides at its own `sizeof` (`xtc_io_event_t`, `xtc_accel_dev_t`,
`xtc_msg_t`, the batch item/result and scheduler lane types) and plain
value types (`xtc_pid_t`, `xtc_pt_t`, `xtc_waker_t`, the trace and tail
record types).  Their layout is frozen by the layout gate; growing one
means a new type and a new function.  The lock layer is frozen too
(below).  The experimental batch and scheduler-policy structs keep their
own `uint32_t size, version` pair with an exact-size check.

## Struct layout gate (ENFORCED)

`test/abi/layout.txt` records, for every public struct in the installed
headers, its `sizeof`, alignment, and each field's offset and size.  It
is generated, not hand-written -- `test/abi/struct_fields.awk` lists the
structs and their members, a C probe prints the numbers -- and
`test/dist/test_abi_layout.sh` (in `make check`) fails if the headers no
longer match it.  A layout change is therefore a conscious act: rerun
the gate with `XTC_ABI_LAYOUT_UPDATE=1` and commit the new table with
the struct change.  For a stable-tier struct only an APPEND to a
size-first struct is acceptable in a minor; reviewers read the table
diff.  The committed table is the Linux x86-64 one; on any other target
the gate reports SKIP (`pthread_mutex_t` inside `xtc_lwlock_t` differs
by libc and OS).

Release-time, not in `make check` (it needs the previous build):
`scripts/abi-check.sh BASE_TAG [CAND_REF]` builds `libxtc.so` at both
refs and runs libabigail's `abidiff`, limited to the installed headers.
Its exit status is abidiff's: bit 4 means an ABI change (additive is
fine in a minor), bit 8 an INCOMPATIBLE change, which blocks a minor or
patch release and is expected in a major.  Measured at this change:
v1.59.0 to the 2.0 tree reports status 4 (struct size changes on
`xtc_app_create`, `xtc_proc_spawn`, `xtc_inspect_procs`, one added
function) with the 1.x SONAME, because the major bump is a separate
release commit.

## Symbol versions and the export set (ENFORCED)

`dist/libxtc.map` is an ELF version script with one named node,
`XTC_2.0`, covering every exported `xtc_*` symbol (`nm -D` shows
`xtc_foo@@XTC_2.0`).  A consumer linked against 2.x records the node it
needs, so the dynamic loader refuses a library that lacks it instead of
failing at the first missing function.

POLICY for later releases, written into the map's header:

- A minor release 2.N that adds functions lists them BY NAME in a new
  node `XTC_2.N { global: xtc_new_a; } XTC_2.(N-1);` placed above
  `XTC_2.0` (an explicit name binds before the `xtc_*` glob).
- A symbol never moves out of the node it shipped in, and a released
  node is never edited.  A changed signature is a new name (or a major).
- A major release starts over with one `XTC_<MAJOR>.0` node and a new
  SONAME (`libxtc.so.MAJOR`).

Enforced by `test/m0/test_symbols.sh` in `make check` and CI:

- `[C5]` every symbol defined in `libxtc.a` is `xtc_*`, `__xtc_*`, or
  `_`-prefixed.  No stray global names.
- `[C6]` installed public headers do not `#define` standard or bare
  identifiers.
- `[C7]` installed public headers declare no `__`-prefixed internal
  function, except the small allowlist a public *macro* expands to.
- `[C8]` the shared library's dynamic symbol table is exactly `xtc_*`
  plus that same macro-backed allowlist -- so the whole `__os_*`
  substrate and every other internal stays private.
- `[C9]` every exported symbol carries an `XTC_<MAJOR>.N` version, none
  is unversioned, and the map's base node is `XTC_<MAJOR>.0` for the
  major in `dist/version.in`.  (While `dist/version.in` still says 1.x --
  the 2.0.0 version bump is its own release commit -- that last
  comparison reports SKIP; everything else in `[C9]` is checked.)

Per-platform link recipes (in `dist/configure.ac`):

- **ELF (Linux, the BSDs, illumos)**: `-Wl,-soname,libxtc.so.MAJOR`
  plus `-Wl,--version-script=dist/libxtc.map` (export restriction AND
  symbol versions).
- **macOS**: `-install_name`, `-compatibility_version MAJOR.0`,
  `-current_version FULL`, and `-Wl,-exported_symbols_list,libxtc.exp`
  -- the same export SET, generated from the archive by
  `dist/Makefile.in`.  Mach-O has no symbol versions.
- **Windows**: the per-commit build is `xtc.lib` (static).  There is no
  hand-curated `xtc.def` and no ordinal stability commitment.

## Stability tiers (ENFORCED)

Every installed header declares exactly one tier, as a
`@stability stable` or `@stability experimental` line in its opening
comment:

- **stable** -- covered by the promise above: no incompatible change
  within a major, and an LTS release keeps it for the LTS window.
- **experimental** -- may change, or be removed, in any minor release.
  Use it, pin the minor, and read the release notes.

`test/dist/test_stability_tiers.sh` (in `make check`) fails if a header
has no tier or more than one, and if the table below is out of date with
the headers; it regenerates the table with `XTC_TIERS_UPDATE=1`.

Why each experimental header is experimental: `xtc_tnt.h` (the
stackless isolate layer, young, no `XTC_API` on its entry points),
`xtc_accel.h` (attached-compute bridge, one backend, scope still
moving), `xtc_batch.h` and `xtc_sched.h` (continuous batching and the
scheduler policy hooks; both already carry a size/version pair because
the contract is still moving), `xtc_dio_sched.h` and `xtc_iosched.h`
(self-tuning write schedulers), `xtc_inject.h` (test-only injection
points), `xtc_pkey.h` (one-platform memory-protection keys),
`xtc_tailcall.h` (compiler-dependent build aid),
`xtc_preempt_involuntary.h` (involuntary preemption: Linux x86-64 and
aarch64 only, and its safe-point contract is new in 2.0).
`xtc_preempt.h` (the preemption timer and cooperative preemption) is
stable.

<!-- tiers:begin (generated by test/dist/test_stability_tiers.sh) -->
| header | tier |
|---|---|
| `xtc.h` | stable |
| `xtc_accel.h` | experimental |
| `xtc_aio.h` | stable |
| `xtc_alloc_audit.h` | stable |
| `xtc_app.h` | stable |
| `xtc_async.h` | stable |
| `xtc_batch.h` | experimental |
| `xtc_bdev.h` | stable |
| `xtc_blocking.h` | stable |
| `xtc_cfg.h` | stable |
| `xtc_chan.h` | stable |
| `xtc_chash.h` | stable |
| `xtc_credit.h` | stable |
| `xtc_cskip.h` | stable |
| `xtc_dio_sched.h` | experimental |
| `xtc_dispatch.h` | stable |
| `xtc_dump.h` | stable |
| `xtc_exec.h` | stable |
| `xtc_export.h` | stable |
| `xtc_fs.h` | stable |
| `xtc_fsm.h` | stable |
| `xtc_future.h` | stable |
| `xtc_inject.h` | experimental |
| `xtc_inspect.h` | stable |
| `xtc_io.h` | stable |
| `xtc_iosched.h` | experimental |
| `xtc_launch.h` | stable |
| `xtc_lockmgr.h` | stable |
| `xtc_log.h` | stable |
| `xtc_loop.h` | stable |
| `xtc_lrlock.h` | stable |
| `xtc_lwlock.h` | stable |
| `xtc_mctx.h` | stable |
| `xtc_net.h` | stable |
| `xtc_orc.h` | stable |
| `xtc_osproc.h` | stable |
| `xtc_pdict.h` | stable |
| `xtc_pg.h` | stable |
| `xtc_pkey.h` | experimental |
| `xtc_pool.h` | stable |
| `xtc_preempt.h` | stable |
| `xtc_preempt_involuntary.h` | experimental |
| `xtc_prob.h` | stable |
| `xtc_proc.h` | stable |
| `xtc_rcu.h` | stable |
| `xtc_reg.h` | stable |
| `xtc_res.h` | stable |
| `xtc_runtime.h` | stable |
| `xtc_saga.h` | stable |
| `xtc_sched.h` | experimental |
| `xtc_slab.h` | stable |
| `xtc_stats.h` | stable |
| `xtc_stream.h` | stable |
| `xtc_svr.h` | stable |
| `xtc_sync.h` | stable |
| `xtc_tail.h` | stable |
| `xtc_tailcall.h` | experimental |
| `xtc_tls.h` | stable |
| `xtc_tnt.h` | experimental |
| `xtc_trace.h` | stable |
| `xtc_xproc.h` | stable |
<!-- tiers:end -->

## Man-page and header agreement (ENFORCED)

The documentation side of the contract is gated:

- `test/m0/test_man_coverage.sh` -- every `PUBLIC:` function across the
  installed header set is documented in some `man3` page.
- `test/m0/test_man_signatures.sh` -- a function's page mentions every
  parameter name from its declaration and documents the return
  contract.
- `test/m0/test_man_lint.sh` -- mdoc lints clean.
- `test/m0/test_docs_abi.sh` -- every tool, path, and gate script this
  page cites exists in the tree, and the page does not claim machinery
  that is absent.
- `test/docs/test_doc_snippets.sh` -- every code snippet in the docs
  compiles and runs against the freshly built library.

## Frozen surfaces (POLICY, layout ENFORCED)

The lock layer is the widest-consumed surface, so it is committed
frozen: `xtc_lwlock_t` / `xtc_lwlock_mode_t` and the `xtc_lwlock_*`
entry points; `xtc_lrlock_t` and `xtc_lrlock_*`; `xtc_lockmgr_t`,
`xtc_locker_t`, `xtc_lock_mode_t`, `xtc_lockmgr_opts_t`,
`xtc_lockmgr_stat_t`, `xtc_lock_req_t` and the `xtc_lockmgr_*` /
`xtc_lock_*` entry points.  Their option and stat structs are therefore
NOT size-first: their layout is held by the layout gate instead, and has
not changed since v1.0.0.  If a new optional field is ever needed there,
it goes through a new struct and a new `_ex` entry point, never an
in-place layout change.

## Deprecation (ENFORCED that it compiles; lifecycle is POLICY)

`XTC_DEPRECATED(msg)` (`src/inc/xtc_export.h`) marks a declaration:

```c
XTC_DEPRECATED("use xtc_foo_ex") XTC_API int xtc_foo(int);
```

It expands to `__attribute__((deprecated(msg)))` on gcc and clang,
`[[deprecated(msg)]]` in C++14 and later, and
`__declspec(deprecated(msg))` on MSVC; defining
`XTC_NO_DEPRECATION_WARNINGS` before including any header silences it.
`test/dist/test_deprecated.sh` (in `make check`) proves a use compiles,
warns with the message in C and C++, and can be silenced.

The lifecycle:

| Stage | Behaviour | Signal |
|---|---|---|
| 1. Live | Documented, supported. | Nothing. |
| 2. Soft-deprecated | Documented, supported; the replacement exists. | Doc note in the man page and release notes. |
| 3. Deprecated | Supported, discouraged.  Marked in minor N. | `XTC_DEPRECATED` compiler warning. |
| 4. Default-off | Not planned: a stable symbol stays callable for the rest of its major. | -- |
| 5. Removed | No earlier than the next MAJOR after the minor that deprecated it. | Symbol absent; build error. |

An experimental-tier symbol may skip straight from live to removed in
any minor.  Nothing is deprecated as of 2.0.

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
  the 1.x struct-growth defect (FIXED in 2.0) and the CHANGED (2.0)
  list of every struct that changed.
- [`adr/`](adr/) -- architecture decision records.
- [Testing]({{ '/testing/' | relative_url }}) -- how the enforced gates
  are run.
