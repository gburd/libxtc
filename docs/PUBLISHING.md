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
