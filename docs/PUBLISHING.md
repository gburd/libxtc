# Documentation publishing

The `docs/` tree is rendered by Doxygen and published to:

  * Codeberg Pages (canonical):
    <https://gregburd.codeberg.page/libxtc/>.  Built and deployed
    by `.forgejo/workflows/pages.yml` on every push to main that
    touches `docs/`, `src/inc/`, or `README.md`.  The workflow runs
    Doxygen, stages the HTML, and deploys with the
    `codeberg.org/git-pages/action@v2` action.

  * GitHub Pages (optional mirror):
    `.github/workflows/pages.yml` validates that the Doxygen build
    succeeds on every push but does not deploy by default.  To turn
    on GitHub Pages, follow the commented instructions at the top of
    that workflow.

The setup mirrors the lime project's: the repository README is the
Doxygen main page, the user-facing markdown guides under `docs/` are
additional pages, and the public headers under `src/inc/` provide
the API reference.

## Local preview

    cd docs
    doxygen Doxyfile
    xdg-open api/html/index.html        # or: python3 -m http.server -d api/html

The generated `docs/api/` tree and `doxygen-warnings.txt` are
gitignored; only the inputs (markdown, headers, Doxyfile) are
tracked.

## What gets published

`docs/Doxyfile` lists the inputs explicitly in `INPUT`.  User-facing
guides are included; internal material (claims sheets, KVM runbooks,
milestone notes, the man-page TODO) is intentionally omitted.  The
internal-only header `src/inc/xtc_int.h` is excluded from API
extraction.

When adding a new user-facing guide:

  1. Write it as `docs/<name>.md` with a leading `# Heading`.
  2. Add the path to the `INPUT` list in `docs/Doxyfile`.
  3. Cross-link other guides with relative `.md` paths
     (`[architecture](ARCHITECTURE.md)`); Doxygen rewrites these to
     the generated page.

## Warning backlog

The Doxyfile sets `WARN_AS_ERROR = NO` because the public headers do
not yet carry full Doxygen doc comments; the build currently emits
~260 undocumented-symbol warnings.  The workflows surface the count
in CI logs.  When the headers are fully documented, flip
`WARN_AS_ERROR` to `YES` so doc-rot fails the build.

## Voice

The documentation voice is concise, declarative, and free of
marketing language.  See `AGENTS.md` for the style rules.

# Package publishing

Beyond the documentation site, xtc ships recipes so it can be
installed via the major package managers and consumed through
`pkg-config`.  Each recipe drives the project's own out-of-source
autoconf build (the build that mandates `dist/configure` be run from
a separate build directory).

## What an install produces

`./dist/configure --prefix=PFX [--enable-shared] && make && make install`
installs:

  * `PFX/lib/libxtc.a`                  -- static library (always).
  * `PFX/lib/libxtc.so.X.Y.Z`           -- shared library (only with
    `--enable-shared`), plus the SONAME symlink `libxtc.so.MAJOR` and
    the bare-name link `libxtc.so`.
  * `PFX/lib/pkgconfig/xtc.pc`          -- pkg-config metadata.
  * `PFX/include/xtc.h`                 -- the single public header.
  * `PFX/share/man/man3/*.3`, `man7/*.7`-- manual pages.

`make uninstall` removes the same set.

## pkg-config

`dist/xtc.pc.in` is substituted at configure time
(prefix/libdir/includedir/version and the private link
dependencies) into `xtc.pc`.  Consumers use the usual incantation:

    cc $(pkg-config --cflags xtc) app.c $(pkg-config --libs xtc)
    # static link (pulls in -pthread, -luring, -lssl, -lcrypto, ...):
    cc app.c $(pkg-config --static --libs xtc) -static

The meson build emits an equivalent `xtc.pc` via
`pkgconfig.generate()`.

Verified on Linux x86_64 (gcc, autoconf build): `--modversion`,
`--cflags`, `--libs`, and `--static --libs` all resolve against a
temp install prefix, and a sample program links and runs against the
installed shared object.

## Shared library and symbol versioning

The shared library is OPTIONAL (`--enable-shared`); the default build
is static-only, preserving the historical behaviour.  When built, the
ld version script `dist/libxtc.map` controls the exported ABI:

  * global: `xtc_*` (the documented public API) and `__xtc_*` (internal
    seams that PUBLIC macros in the installed header call through, e.g.
    the proc recovery / fcontext hooks in `src/inc/xtc_proc.h`).
  * local: everything else, notably the internal `__os_*`
    OS-abstraction layer.

Verified with `nm -D`: the built `libxtc.so` exports 401 symbols, all
`xtc_*`/`__xtc_*`, with zero `__os_*` symbols leaking.  The SONAME
carries only the SemVer major; the SemVer policy in
`docs/abi-stability.md` governs compatibility.

## Debian / Ubuntu

`debian/` builds two binary packages:

  * `libxtc0`     -- the shared library.
  * `libxtc-dev`  -- header, static lib, pkg-config file, man pages.

`debian/rules` is debhelper-13 (`dh`) and overrides the `auto_*`
steps to drive the out-of-source autoconf build with
`--enable-shared` into the multiarch libdir.  Build with:

    dpkg-buildpackage -us -uc -b

Status: the `changelog` parses with `dpkg-parsechangelog`, the
`control` stanzas are well-formed, and every `*.install` glob was
validated against a real `--enable-shared` DESTDIR install.  A full
`dpkg-buildpackage` / `lintian` run was NOT performed on the
development host (no `debhelper`/`lintian` available there); run it
on a Debian or Ubuntu machine to confirm.

## RPM (Fedora / RHEL / openSUSE)

`dist/xtc.spec` builds `libxtc` (shared library) and `libxtc-devel`
(header, static lib, pkg-config, man pages), tags `License: ISC`, and
runs the test suite in `%check`.  Build from a release tarball with:

    rpmbuild -ba dist/xtc.spec

Status: validated with `rpmspec --parse` and `rpmspec --query`
(resolves to `libxtc-0.7.0-1` and `libxtc-devel-0.7.0-1`).  A full
`rpmbuild -ba` / `rpmlint` run was NOT performed on the development
host (no RPM build root / rpmlint there); run it on a Fedora/RHEL/
openSUSE machine, adjusting the `BuildRequires` package names if your
distro spells them differently.

## Nix

`flake.nix` exposes:

  * `packages.<system>.default` (alias `packages.<system>.xtc`) -- a
    derivation that configures with `--enable-shared`, builds, and
    installs into `$out`.  Build with `nix build`.  (The full
    `make check` is not run in the sandboxed package build because its
    shell meta-tests need autoreconf and host env vars; run it from the
    devShell or CI.)
  * `devShells.default` -- the full toolchain for hacking on xtc.

Verified that the flake evaluates and the derivation is well-formed
on this host; `nix build` requires network access to fetch nixpkgs.
