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

5. **Internal design/status memos.**  Agent working notes -- design
   explorations, milestone claim logs, status/readiness reviews,
   host-bring-up scratch, per-feature investigation write-ups -- are
   NOT shipped documentation.  They live in `.agent/` (gitignored),
   never in `docs/` or anywhere else in Git.  `docs/` holds ONLY
   consumer-facing documentation: the ones an API user or packager
   reads (API.md, ARCHITECTURE.md, KNOWN_ISSUES.md, abi-stability.md,
   getting-started.md, guide/, locks.md, index.md, PUBLISHING.md, the
   ADRs, Doxyfile) plus reference matrices a shipped man page / README
   legitimately cites.  A file named `M_*.md` is the tell of an agent
   memo: it belongs in `.agent/`.  When moving one out of `docs/`,
   fix or drop every shipped reference to it (README, man pages, source
   header comments, index.md) rather than leaving a dead link.

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
backend code to a coverage number UNDER DST -- that is dishonest.

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

## API discipline (merge gate: test/dist/test_api_discipline.sh, in make check + CI)

Four rules, enforced by the gate.  A rare, justified exception on a
single line carries a `/* XTC_RAW_OK: reason */` (or the existing
`XTC_BLOCKING_OK`) marker.

1. LIBRARY code uses the `__os_*` wrappers, never the raw wrapped
   primitives (malloc/calloc/realloc/free/strdup, pthread_create,
   clock_gettime/gettimeofday/nanosleep/usleep).  The wrappers add the
   allocator hook (embedder accounting), the async-signal-unsafe
   bracket, and portability; raw calls bypass all three (and a raw
   malloc handed to a caller who frees it with xtc_free/__os_free is a
   mismatched free / heap corruption under a custom allocator).  The
   ONLY place the raw primitive is allowed is inside the wrapper's own
   implementation (src/os/*) or a marked platform shim.

2. Never `(void)__os_malloc(...)` (or _calloc/_realloc); always check
   `!= XTC_OK`.  And do NOT double-check the rc AND the pointer
   `== NULL`: `__os_malloc/_calloc/_realloc` guarantee a NON-NULL
   pointer on XTC_OK (even for size 0), so the NULL half is dead code.

3. CONSUMERS use only the public `xtc_*` API, never the internal
   `__os_*` / `__xtc_*` surface.  The examples are the consumer
   exemplar and must stay clean -- if a consumer needs a primitive, add
   the public `xtc_*` for it (this is why xtc_malloc/_calloc/_realloc/
   _aligned_alloc/_aligned_free, xtc_clock_mono/_real, xtc_sleep_ns,
   xtc_atomic_i64_load/_add exist).  Inside a fiber use xtc_proc_sleep,
   not xtc_sleep_ns (which blocks the OS thread).

4. No NAKED block-scoped variables: a bare `{ ... }` introduced only to
   scope a variable is forbidden.  Block-scoped declarations are fine in
   if/while/for/do/switch bodies; otherwise declare at the nearest real
   enclosing block (usually the function top).

Before cutting a release, run the gate (it is in make check, and CI runs
it); it must print `[api] OK`.

## Documentation site (docs/, Jekyll, both Pages)

The docs/ tree is a Jekyll site that builds and deploys to BOTH GitHub
Pages and Codeberg Pages.  It reads as a BerkeleyDB-reference-manual +
O'Reilly Getting-Started hybrid: narrative that walks a newcomer into
the code AND the philosophy, with many worked snippets (short to long,
easy to complex), demonstrating benefits and calling out the
alternatives deliberately NOT chosen.  It cross-links to the man pages
and into the source.  A separate "Examples" section documents each
./examples/* program (what it is, the design decisions, the trade-offs).

Every code snippet in the docs MUST compile and run -- doc code is a
RELEASE GATE.  Snippets live as real files under docs/_includes/snippets/ and are
built+run by test/docs/test_doc_snippets.sh (wired into make check and
CI).  A snippet that will not build or a doc that promises behavior the
snippet does not demonstrate is a release blocker, exactly like the API
discipline gate.  Never paste code into a doc page that is not backed by
a tested snippet file.

DEFERRED (revisit later, do NOT attempt now): the in-page live,
editable, re-runnable WASM playground, and making WASM a tested libxtc
target.  Recorded 2026-07; explicitly out of scope for the current docs
work.  When revisited it is its own phase (emscripten + Asyncify + the
sim backend + a wasm CI job) BEFORE any in-browser editor.

## Code Style

BSD KNF as encoded in `.clang-format`.  ASCII-only in source, docs,
comments, and commit messages.

See .agent-steering-domains.md for domain-specific steering (local).
