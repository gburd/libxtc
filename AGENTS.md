# Agent Steering — xtc

Project-specific rules for AI agents (Claude Code, Kiro CLI, Pi, Maki).

## Never Commit

1. **Core dump files** (`core`, `core.*`, `*.core`, `vgcore.*`).
   These are gitignored but if one slips through, remove it immediately.
   They are large, useless outside the original debug session, and
   pollute git history.

2. **Private keys, certificates, or secrets** -- even test-only ones.
   GitHub's secret scanner rejects pushes containing PEM private keys
   regardless of whether they protect anything real.  Instead:
   - Generate test certs at runtime via `openssl req -x509 ...` in
     test setup functions.
   - Clean them up in test teardown.
   - See `test/m18/test_tls_server.c` for the canonical pattern.

3. **API keys, tokens, passwords, .env files** -- standard rule, no
   exceptions.

4. **Build artifacts.** Compiled binaries, object files, static/shared
   libraries, and language-toolchain build trees (Rust `target/`,
   Cargo `*.rlib`/`*.rmeta`, etc.).  Each example and bench directory
   that produces a binary must carry a `.gitignore` for it.  Vendored
   source (e.g. the SQLite amalgamation) is allowed; compiled output
   never is.

## Recovering from a bad commit (history rewrite)

If a core dump, build artifact, or secret reaches the remote, it must
be purged from history -- GitHub's push rules and secret scanner will
reject or flag the repository, and a plain `git rm` only removes it
from the tip, not from history.

Procedure (used 2026-05 to purge `test/m99/core.*`, an 11 MB Rust
`tokio/target/` tree, and a stale `sqlite.zip`):

1. **Back up first.**  `git bundle create /tmp/xtc-backup-$(date
   +%Y%m%d-%H%M%S).bundle --all`.  Keep it until the rewrite is
   confirmed good on the remote.

2. **List what you are purging** so the action is auditable:
   `git rev-list --all --objects | grep -E '<pattern>'`.

3. **Rewrite with git-filter-repo** (available via
   `nix-shell -p git-filter-repo`).  Put the paths in a file and run
   `git filter-repo --force --invert-paths --paths-from-file FILE`.
   Globs use the `glob:` prefix, e.g.
   `glob:bench/conformance/*/tokio/target/**`.

4. **Re-add the remote.**  filter-repo removes `origin` by design;
   `git remote add origin ssh://git@codeberg.org/gregburd/libxtc.git`.

5. **Verify** the blobs are gone from *all* history
   (`git rev-list --all --objects | grep -E '<pattern>'` returns
   nothing) and that the working tree still has every source file.

6. **Force-push requires explicit human approval.**  The agent
   harness blocks `git push --force` and even `--force-with-lease`.
   This is intentional.  A human runs the final push after
   confirming the local rewrite:

   ```sh
   git push --force-with-lease=main:<old-remote-sha> origin main
   ```

   where `<old-remote-sha>` is the remote tip before the rewrite, so
   the push refuses if anyone else pushed in the interim.  Any tags
   that pointed at rewritten commits must be re-pushed too.


## TLS Test Pattern

When writing tests that need certificates:

```c
static int
generate_cert(const char *cert_path, const char *key_path, const char *cn)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "openssl req -x509 -newkey rsa:2048 -nodes -days 1 "
             "-keyout %s -out %s -subj /CN=%s 2>/dev/null",
             key_path, cert_path, cn);
    return system(cmd);
}
```

Call in `suite_setup`, `unlink()` in `suite_teardown`.

## Build & Test

```sh
cd build_unix && make -j$(nproc) && make check
```

## Code Style

BSD KNF as encoded in `.clang-format`.  ASCII-only in source, docs,
comments, and commit messages.
