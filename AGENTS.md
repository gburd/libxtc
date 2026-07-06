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

## DST is the point -- measure it by the RIGHT yardsticks

libxtc's north star is to be as trustworthy as FoundationDB and
TigerBeetle: DST-first, 100% deterministic simulation.  Do NOT judge
that by code-coverage percentage.  Coverage is a hygiene FLOOR for the
DST-reachable code, not the goal, and it undercounts on purpose (the
thread pool, cross-process mmap, native AIO backends, and real TLS wire
are deliberately NOT DST-reachable and are covered by other test tiers
-- m9 threads, m11 shm, m4 native AIO, m18 TLS).  The real yardsticks,
in priority order:

1. DETERMINISM: 100%, ENFORCED (binary, not a percent).  Same seed +
   same config => byte-identical execution, every time.  Enforced by
   the determinism guard (__xtc_sim_nondeterminism traps any
   sim-reachable real clock / unseeded RNG / env read / raw thread id)
   plus xtc_sim_exec_run refusing XTC_OK if a run hit one -- so every
   sim test proves its own determinism.  Any new sim-reachable
   nondeterministic primitive MUST call the guard.  Never regress this.

2. VOLUME SURVIVED: seeds x fault-classes run with ZERO invariant
   violation.  This is the trust-building number and it grows over
   time -- the swarm/soak sweeps report it; run them at scale nightly.
   "N seeds across M fault classes, no durability/safety violation" is
   the honest confidence statement, not a coverage figure.

3. FAULT-SPACE COVERAGE: which faults / buggify sites the sweep
   actually ACTIVATED (not which lines executed).  The question is
   "did we inject a partition WHILE a commit was in flight WHILE the
   disk was slow," not "did we run this line."  xtc_sim_buggify_site /
   _reached_count expose it; the swarm reports it; a sweep that does
   not hit enough is a gap to fix.

4. BUG-DETECTION LATENCY (the killer metric): plant a KNOWN
   durability/safety bug behind a compile flag and prove DST catches
   it within K seeds, deterministically, with a replayable trace.
   "If you break it, the simulator finds it fast and hands you the
   exact seed" is what lets us truthfully claim FDB/TigerBeetle-grade
   testing.  See test/sim/test_sim_bug_inject.c and
   scripts/dst-bug-inject.sh.  When you add a new safety invariant,
   add a planted-bug case that DST must catch.

Line coverage of the DST-REACHABLE public-API code is still a useful
floor (catches "this whole branch is never exercised"); measure and
track it, but state the claim precisely as ">85% of the DST-reachable
public surface," and never force thread-only / real-kernel / native-
backend code to a coverage number UNDER DST -- that is dishonest.  See
docs/M_DST.md for the measured baseline and the reachability breakdown.

## Continuous integration -- always check after pushing

The repository lives on Codeberg (`origin`,
`ssh://git@codeberg.org/gregburd/libxtc.git`) and is mirrored to
GitHub (`gburd/libxtc`), where the Actions CI runs: gcc and clang
`make check`, AddressSanitizer, UndefinedBehaviorSanitizer, and a
forced-fcontext (musl coroutine path) job.

**After every push -- and without exception after pushing a tagged
release -- verify CI went green.**  CI can fail on something the
local `make check` does not catch (a sanitizer not enabled locally,
a different compiler, a backend the local build did not select).  A
red pipeline that nobody looks at silently rots: this project once
accumulated dozens of consecutive failing runs from one latent UBSan
misalignment because pushes were not being checked.

```sh
# wait for the mirror to sync (~30-60s), then:
gh run list --repo gburd/libxtc --limit 5
# inspect a failure:
gh run view <run-id> --repo gburd/libxtc --log-failed | \
    grep -iE 'runtime error|FAIL|error:|Assertion'
```

Fix CI failures promptly and treat them as first-class -- the README
claims the library is CI-tested under sanitizers, so the badge must
be true.  The local sanitizer build that mirrors CI most closely:

```sh
B=$(mktemp -d)
( cd "$B" && CFLAGS='-fsanitize=undefined -fno-omit-frame-pointer -g -O1' \
    LDFLAGS='-fsanitize=undefined' /path/to/dist/configure --with-tls=auto && \
    UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 make check )
```

## Allocation alignment

Any struct with an over-aligned member (`_Alignas(XTC_CACHE_LINE)`,
stricter than `max_align_t`) MUST be heap-allocated with
`__os_aligned_alloc` and released with `__os_aligned_free` -- never
`__os_calloc`/`__os_free`, which only guarantee `max_align_t` and
trip UBSan's alignment check (and can fault on stricter targets or
corrupt the heap on Windows).  This applies to the struct itself and
to arrays whose element type is over-aligned (the array base must be
aligned or element 0 is misaligned).  `aligned()` and `aligned_free()`
are a matched pair in the allocator vtable.

## Code Style

BSD KNF as encoded in `.clang-format`.  ASCII-only in source, docs,
comments, and commit messages.

See .agent-steering-domains.md for domain-specific steering (local).
