# Documentation publishing

The `docs/` tree is rendered by Jekyll and published to:

  * GitHub Pages: <https://gregburd.github.io/libxtc/> (when the
    repo is mirrored to GitHub).  Built and deployed by
    `.github/workflows/pages.yml` on every push to main that
    touches `docs/`.
  * Codeberg Pages: <https://gregburd.codeberg.page/libxtc/>
    (when configured).  Built by `.forgejo/workflows/pages.yml`,
    which pushes rendered HTML to the `pages` branch; Codeberg
    Pages serves that branch.

## Local preview

    cd docs
    bundle install                          # one-time, if Gemfile present
    jekyll serve                            # http://localhost:4000

If `jekyll` is not installed:

    gem install --user-install jekyll \
                kramdown-parser-gfm \
                rouge \
                jekyll-theme-cayman

## Codeberg setup

The first deploy requires:

  1. A repository secret named `PAGES_PUSH_TOKEN` containing a
     personal access token with `repository:write` scope on this
     repo.
  2. Enabling Pages in repo settings (Settings -> Pages) with
     branch set to `pages`.

The Forgejo Actions workflow runs in a Docker container based on
`ruby:3.3`, installs Jekyll, builds the site, and force-pushes to
the `pages` branch.  Codeberg Pages picks the branch up and serves
the rendered HTML.

## Authoring conventions

Every page in `docs/` is rendered.  Files matching the patterns
in `_config.yml`'s `exclude` list (internal claims sheets, KVM
runbooks) are kept private to the source tree.

For new top-level pages:

  * Use `.md` (kramdown / GFM).
  * Add a YAML front-matter block at the top with at least a
    `title:` field.
  * Cross-link with relative paths and the `.html` extension that
    Jekyll produces, not the `.md` extension that the source tree
    uses.  For example, `[architecture](ARCHITECTURE.html)`, not
    `[architecture](ARCHITECTURE.md)`.

For consistency, the documentation voice is concise, declarative,
and free of marketing language.  See `AGENTS.md` for the style
rules in detail.
