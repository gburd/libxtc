# xtc — high-performance async/concurrency runtime for C

`xtc` is the foundational concurrency runtime for a future threaded
PostgreSQL.  It is a layered C library that provides Tokio-style
async tasks, BEAM-style processes/mailboxes/supervisors, and Seastar-
style per-CPU shared-nothing reactors over a pluggable I/O substrate
(io_uring / epoll / kqueue / IOCP / poll).

The full design is in [PLAN.md](PLAN.md).  The current milestone
(M0 — repo skeleton) is documented in [M0_CLAIMS.md](M0_CLAIMS.md);
every claim there has a corresponding test.

## Status

**Pre-1.0, M0.**  The build system, the public-header skeleton, the
documentation discipline, and the `dist/s_*` generator framework are
in place.  Real concurrency primitives land in M1+.

## Project layout

```
PLAN.md             design plan (all 21 sections)
M0_CLAIMS.md        the claims of milestone M0 and the tests for them
README.md           you are here
LICENSE             PostgreSQL license

dist/               build apparatus (BDB / DBSQL convention)
  configure.ac        autoconf source (run autoreconf -i to produce configure)
  Makefile.in         template for the autoconf build's Makefile
  xtc_config.h.in     template for the autoconf-generated config header
  version.in          single source of truth for the SemVer string
  s_all               run every code generator
  s_include           PUBLIC: → prototype headers (M0_CLAIMS.md [T2])
  s_perm              chmod +x every s_* (M0_CLAIMS.md [T5])
  gen_inc.awk         the awk used by s_include
  meson.build         meson build entry point
  meson_options.txt

src/                library source
  inc/                public + internal headers
    xtc.h             single public header (M0_CLAIMS.md [C4])
  xtc_version.c       xtc_version_string + xtc_version_components
  xtc_strerror.c      xtc_strerror

test/               test suite
  m0/                 munit + shell tests for M0 claims
  m1/                 munit + shell tests for M1 (L0 os/) claims
  m2/                 munit + shell tests for M2 (L1 io/) claims
  m3/                 munit + shell tests for M3 (L2 evt/) claims
  pbt/                hegel-c property-based tests across all layers
  dist/               tests for the dist/s_* generators

man/                man pages (mdoc/man-style)
  man3/               function-level pages
  man7/               overview pages

docs/               long-form documentation
  ARCHITECTURE.md     layered architecture overview
  API.md              public API reference (generated section + handwritten)
  abi-stability.md    SemVer + symbol-versioning + deprecation policy
  adr/                architecture decision records
```

## Build

### Autoconf path (Tier 1, primary)

<!-- M0_CLAIMS:B1_BEGIN -->
```sh
cd dist && autoreconf -i && cd ..
mkdir -p build_unix && cd build_unix
../dist/configure
make
```
<!-- M0_CLAIMS:B1_END -->

Then `make check` runs the test suite (see [M0_CLAIMS.md](M0_CLAIMS.md)).

### Meson path (Tier 1, parallel)

<!-- M0_CLAIMS:B2_BEGIN -->
```sh
meson setup build_meson
meson compile -C build_meson
```
<!-- M0_CLAIMS:B2_END -->

Then `meson test -C build_meson` runs the suite.

Both paths are tested in CI.  See [M0_CLAIMS.md](M0_CLAIMS.md) [B1, B2].

### Property-based tests (hegel-c, optional)

Property-based tests live under `test/pbt/`.  They link against the
[hegel-c](https://github.com/gburd/hegel-c) library and assert
generative properties (linearizability, ordering invariants,
allocator balance, etc.).  Configure with `--with-hegel`:

```sh
../dist/configure --with-hegel=/path/to/hegel-c \
                  --with-hegel-server=/path/to/hegel-wrapper
make check          # runs munit + PBT + shell tests
```

Without `--with-hegel`, the PBT binaries print SKIP and the rest of
`make check` runs unchanged.  See [docs/adr/0002-hegel-pbt-first-class.md](docs/adr/0002-hegel-pbt-first-class.md).

### Out-of-source is required

Configuring inside the source root or inside `dist/` is rejected with
a clear error.  See [M0_CLAIMS.md](M0_CLAIMS.md) [B3, B6].

### Reproducible build environment (Nix)

```sh
nix develop          # provides autoconf, meson, ninja, mandoc, etc.
```

## Documentation

- **`PLAN.md`** — the full design (≈2600 lines, 21 sections).  Read
  this to understand intent.
- **`docs/ARCHITECTURE.md`** — the layered architecture, condensed.
- **`docs/API.md`** — public API reference (generated from headers
  for the per-function detail; handwritten introduction).
- **`docs/abi-stability.md`** — what we promise across versions, how
  we deprecate, what symbol versioning we use.
- **`man/man3/*.3`** — Unix-style function reference (one page per
  public function; required by [M0_CLAIMS.md](M0_CLAIMS.md) [D4]).
- **`man/man7/xtc.7`** — overview man page; what to read first.
- **`docs/adr/`** — architecture decision records, one per Q-decision
  in `PLAN.md` §10.

## Adding to the project

The discipline:

1. **State the claim** in the appropriate `M*_CLAIMS.md` (or open a
   new one for a new milestone).
2. **Write the test** before the implementation.  See `test/m0/` for
   the established style.
3. **Implement** until the test passes.
4. **Update documentation** (man page, API.md, ARCHITECTURE.md as
   relevant) in the same commit.
5. **Run the full check**: `make check && (cd ../build_meson && meson test)`.

This discipline is what we trade for the right to call this a
foundation.  See [PLAN.md §18](PLAN.md) for the longevity contract.

## License

PostgreSQL license.  See [LICENSE](LICENSE).
