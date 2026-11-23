# Documentation publishing

The `docs/` tree is a Jekyll site, plus a Doxygen API reference built
from the public headers and copied into the site under `/api`.  The
Pages workflow runs `docs/gen_api_reference.sh` (the man pages rendered
with mandoc into `docs/reference/api/`), builds the Jekyll site, builds
Doxygen, and checks links with lychee, on every push to main that
touches `src/inc/`, `docs/`, `man/`, or `README.md`:

  * GitHub Pages: `.github/workflows/pages.yml`, deployed with
    `actions/deploy-pages` (Pages enabled in the repository settings,
    source GitHub Actions).  Broken links block the deploy.  This is
    the workflow that runs.

  * Codeberg Pages: `.forgejo/workflows/pages.yml` does the same and
    deploys with the `codeberg.org/git-pages/action@v2` action.  It is
    kept but does not run: Codeberg disabled hosted Forgejo Actions for
    this repository on 2026-10-30.

For the Doxygen part the repository README is the main page, the
markdown files listed in `docs/Doxyfile` are additional pages, and the
public headers under `src/inc/` provide the API reference.

## Local preview

    sh docs/gen_api_reference.sh
    cd docs && bundle install && bundle exec jekyll serve

The Doxygen part alone:

    cd docs
    doxygen Doxyfile
    xdg-open api/html/index.html        # or: python3 -m http.server -d api/html

The generated `docs/api/` tree and `doxygen-warnings.txt` are
gitignored; only the inputs (markdown, headers, Doxyfile) are
tracked.

## What gets published

The Jekyll site publishes every page under `docs/` that carries front
matter (`title`, `parent`, `permalink`).  `docs/Doxyfile` lists the
markdown it also feeds to Doxygen explicitly in `INPUT`, and excludes
the internal-only header `src/inc/xtc_int.h` from API extraction.

When adding a new user-facing page:

  1. Write it as `docs/<name>.md` with Jekyll front matter, as the
     other pages do.
  2. Cross-link other pages with `relative_url` links
     (`[architecture]({{ '/reference/architecture/' | relative_url }})`).
  3. Add it to the Doxyfile's `INPUT` only if it belongs in the
     Doxygen pages too.

## Warning backlog

The Doxyfile sets `WARN_AS_ERROR = NO` because the public headers do
not yet carry full Doxygen doc comments, so the build emits
undocumented-symbol warnings.  The workflows print the count in their
logs.  When the headers are fully documented, flip
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

  * `PFX/lib/libxtc.a`: the static library (always).
  * `PFX/lib/libxtc.so.X.Y.Z`: the shared library (only with
    `--enable-shared`), plus the SONAME symlink `libxtc.so.MAJOR` and
    the bare-name link `libxtc.so`.
  * `PFX/lib/pkgconfig/xtc.pc`: pkg-config metadata.
  * `PFX/include/*.h`: the public headers (`LIB_HDRS_PUBLIC` in
    `dist/Makefile.in`) and the generated `xtc_version.h`.
  * `PFX/share/man/man3/*.3` and `man7/*.7`: the manual pages (not
    the internal `__os_*.3` pages, since 2.1.3).

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

`Version` is the release name: 2.1.6 for an LTS release, 2026.11.24
for a development release.  pkg-config compares a CalVer name as a
version, so it sorts above every 2.x (`xtc >= 2.3` holds and `xtc < 3`
does not); test XTC_VERSION_MAJOR/_MINOR for the ABI level.

Verified on Linux x86_64 (gcc, autoconf build): `--modversion`,
`--cflags`, `--libs`, and `--static --libs` all resolve against a
temp install prefix, and a sample program links and runs against the
installed shared object.

## Shared library and symbol versioning

The shared library is OPTIONAL (`--enable-shared`); the default build
is static-only, preserving the historical behaviour.  When built, the
ld version script `dist/libxtc.map` controls the exported ABI:

  * global: `xtc_*` (the documented public API) and the
    `__xtc_proc_recovery_slot` / `__xtc_recovery_*` symbols that the
    `xtc_proc_recovery_arm()` macro in `src/inc/xtc_proc.h` expands
    to.  No other `__xtc_*` symbol is exported.
  * local: everything else, notably the internal `__os_*`
    OS-abstraction layer.

Every export carries a symbol version: `XTC_2.0` for the 2.0 set, and
`XTC_2.2` and `XTC_2.3` for the functions those minors added (`nm -D`
shows `xtc_foo@@XTC_2.0`; checked by `test/m0/test_symbols.sh`
[C8]/[C9]), and no `__os_*` symbol leaks.  The
SONAME carries only the ABI level's major (a development release's
CalVer name never reaches it); `docs/abi-stability.md` gives the
policy for adding `XTC_2.N` nodes in later minors.  At release time,
`scripts/abi-check.sh PREV_TAG` runs `abidiff` between the previous
release and the candidate.

## Release branches and tags

`main` carries the development releases (vYYYY.MM.DD, published as
pre-releases).  The LTS line has its own branch, `lts/MAJOR.MINOR`; the
current one is `lts/2.1`, cut from the v2.1.1 tag:

    git branch lts/2.1 v2.1.1
    git push origin lts/2.1

Nothing is committed to an LTS branch directly except a backport that
could not be cherry-picked.  What a patch release may change is the LTS
contract in `docs/abi-stability.md` ("Long-term support and release
lines"): fixes only, no ABI, format, default or dependency change.

## Patch releases on an LTS branch

1. Land the fix on `main` with its test (fail-before / pass-after), as
   for any change.
2. Backport, naming the source commit:

       git switch lts/2.1
       git cherry-pick -x <main-commit>

   If it does not apply, rewrite it on the branch with the same test and
   say why in the message.  Never backport a commit that changes
   test/abi/layout.txt, dist/libxtc.map's symbol set, a format version,
   or a documented default: that is a minor-release change.
3. Bump the patch number (`dist/version.in`, `dist/configure` regenerated
   and committed, the man-page dates if they changed), and add the
   release notes: the fixes, with the `main` commit of each, any
   security advisory (SECURITY.md), and any accepted perf-gate cell.
4. Qualify on the branch exactly as a minor is qualified (AGENTS.md
   "Cutting a release"): `make check` on a readiness and a completion
   backend, `make check-dst` in a sim build, the 100k swarm by hand,
   `scripts/perf-gate.sh vPREV HEAD` against the previous patch release,
   and the io_uring gate.  The compat CI job must pass: the N-1 consumer
   built against the previous release's headers runs against the patch
   release's library.
5. Tag from the branch, signed, and push the tag:

       git tag -s v2.1.N -m "libxtc 2.1.N"
       git push origin lts/2.1 v2.1.N

6. Check CI on the tag and on the branch (a tag does not run the nightly
   tiers; see AGENTS.md).

A security fix follows SECURITY.md's embargo: it is prepared on private
branches, and `main` and every supported `lts/*` branch are tagged
together on the disclosure day.

## Development releases from main

A development release is named by the day it is cut, vYYYY.MM.DD
(vYYYY.MM.DD.2 for a second one that day), and carries the ABI level in
`dist/version.in` (2.3.0 for the first).  `dist/release_name.sh` says
how the two versions reach the build.

1. Set the ABI level in `dist/version.in` by the rules in
   `docs/abi-stability.md` ("Version numbering"): the minor moves when
   API or symbols were added since the last release (a new XTC_2.N node
   in `dist/libxtc.map`), the patch when only fixes were.  Regenerate
   and commit `dist/configure`, and add the ABI level's entry to
   `debian/changelog` and `dist/xtc.spec` if it has none.  Those entries
   name the ABI level, not the date, because the packages are built
   only for LTS releases.
2. Write the name into dist/release.in and the release body into
   dist/release-notes.md, in the same commit as anything else the
   release needs:

       echo 2026.11.24 > dist/release.in
       $EDITOR dist/release-notes.md
       git add -f dist/release.in dist/release-notes.md

   (`-f` because a global gitignore may cover `dist/`.)  Set
   XTC_VERSION_STRING in `src/inc/compat/xtc_config.h`, the MSVC build's
   hand-kept header, to "2026.11.24" too; `make check` [B7] fails until
   it agrees.  The release
   workflow publishes dist/release-notes.md as the release body, word
   for word, and refuses a CalVer tag whose commit has none.  The build
   then reports `xtc_version_string()` = "2026.11.24" and pkg-config Version
   2026.11.24, while the SONAME stays libxtc.so.2 and the file
   libxtc.so.2.3.0.
3. Qualify exactly as an LTS patch (step 4 above), with perf-gate
   against the previous development release (or v2.2.0 for the first).
   The compat job picks the previous development release, or v2.2.0,
   by itself.
4. Tag `main`, signed, and push the tag:

       git tag -s v2026.11.24 -m "libxtc 2026.11.24"
       git push origin main v2026.11.24

   The release workflow checks that the tag is v plus the release name,
   publishes a pre-release (never "Latest") with the source tarball,
   the amalgamation and the man-page bundle, and builds no distro
   packages.  On Codeberg, create the release by hand as a pre-release
   (`tea releases create --prerelease`) with the same three files and
   dist/release-notes.md as the body.
5. Remove dist/release.in and dist/release-notes.md in the next commit
   on `main`, and set the MSVC header's XTC_VERSION_STRING back to the
   ABI level, so a tree between releases is named by its ABI level and
   cannot be mistaken for the release.
6. Check CI on the tag, as for an LTS release, and check that the
   newest v2.1.N is still "Latest" on both forges.

## Cutting a new LTS line

A new LTS line is cut from `main` when the development features chosen
to keep are there: delete dist/release.in, set `dist/version.in` to the
line's first SemVer version (for example 2.4.0), tag it v2.4.0, and cut
the branch from the tag (`git branch lts/2.4 v2.4.0`).  Its patch
releases follow "Patch releases on an LTS branch".  Update the
"Long-term support and release lines" section of
`docs/abi-stability.md` and SECURITY.md's table when it is cut: that
is when the previous line's end date is fixed.

## Debian / Ubuntu

These packages are built for LTS releases only.  A development release
from `main` ships source artefacts only, so it never replaces an LTS
install.

`debian/` builds two binary packages:

  * `libxtc2`: the shared library (the package name carries the
    SONAME major).
  * `libxtc-dev`: headers, static library, pkg-config file, and man
    pages.

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
(it resolves to `libxtc-<version>-1` and `libxtc-devel-<version>-1`,
the version in the spec).  A full
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
