# Agent Steering -- xtc

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

   GENERATED-BUT-COMMITTED SOURCE -- allowed, but ONLY these two, and
   they are the complete list as of 2026-07 (do not add more without a
   note here):
     - `dist/configure` -- the autotools-generated configure script,
       committed so a consumer/packager can build WITHOUT autoconf
       installed (the tarball convention; `test/dist/test_install_headers.sh`
       and the macOS no-autoconf path rely on it).
     - `examples/06_sqlxtc/sql_parse_gen.c` -- the Lemon/Lime-generated
       SQL parser, committed like SQLite commits its lemon output
       (regenerating needs the Lime submodule built; the sqlxtc
       Makefile documents it as "the committed generated file").
   These are generated SOURCE, not compiled OUTPUT -- the line the rule
   above draws.  Anything else generated (a `.o`, a lib, a second
   generated `.c`) is an artifact and must be gitignored, not committed.

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

   `.agent/` is gitignored (`.gitignore` line 39).  NEVER `git add -f`
   a file under `.agent/` -- the `-f` overrides the ignore and is
   exactly how six memos leaked into Git in 2026-07 (removed in
   `4969eda`).  Agent tracking notes stay local-only, full stop; if a
   note's content deserves to ship, it becomes consumer-facing docs
   under `docs/` on its own merits, not a force-added `.agent/` file.

## Worktrees and agent scratch space

- **Git worktrees live in `/scratch/libxtc/<worktree>`**, never inside the
  repository or elsewhere under the home directory.  Remove a worktree as
  soon as its branch is merged or abandoned (`git worktree list` should not
  grow without bound).  This repo has a submodule
  (`examples/06_sqlxtc/lime`), so `git worktree remove` refuses an
  initialized worktree: confirm `git status` is clean, then
  `git worktree remove --force --force`.  `git worktree move` cannot move
  such a worktree at all: remove it and `git worktree add` it at the new
  path.
- **`.agent/` holds notes, not build output.**  Keep reports, scripts and
  small result files; do not leave build trees, binaries, object files,
  git bundles, cores or raw multi-hundred-MB driver logs there (compact a
  raw log to its counts, or delete it once its numbers are in a report).
  A git bundle of history taken before a rewrite still contains what the
  rewrite removed: delete it once the rewrite is confirmed on the remote.

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
   sim-reachable real clock / env read / raw thread id)
   plus xtc_sim_exec_run refusing XTC_OK if a run hit one -- so every
   sim test proves its own determinism.  Any new sim-reachable
   nondeterministic primitive MUST call the guard.  Never regress this.

   The guard is hooked at exactly these primitives (test/sim/
   test_sim_determinism_guard.c asserts each one still fires, so this
   list cannot silently rot):
     - real/monotonic clock  -- src/os/os_time.c (__os_clock_mono's
       non-virtualized fallbacks, and __os_clock_real on BOTH the POSIX
       and Windows branches; the WALL clock is never virtualized)
     - environment read      -- src/os/os_env.c (__os_env_get)
     - raw OS thread id      -- src/os/os_thread.c (__os_thread_self)

   The RNG is not on the list: it is VIRTUALIZED instead (2.0).  An
   un-seeded __os_rand_u64 / xtc_rand_u64 draw under the simulator
   takes the run's XTC_SIM_RNG_RAND stream (src/os/os_rand.c), so it
   replays; the guard test asserts it does not trip and does replay.

   CONSUMER-VISIBLE COROLLARY: xtc_clock_mono is virtualized by the
   simulator and is the clock to use on any sim-reachable path;
   xtc_clock_real is NOT virtualized.  A value DERIVED from real time
   before a sim run must not reach the run either -- and the guard
   cannot see that class, because the real read happened outside the
   run.  The 1.60 version of hlc_phys_ms (examples/06_sqlxtc/xstore.c)
   was exactly that: xtc_clock_mono plus a wall-clock offset sampled
   once, off the simulator, so every commit stamp of a run carried the
   real time of the warm-up, and two runs of one seed minted different
   stamps (about 5% of test_sim_sqlxtc_ckpt replays failed once the
   B-tree's separators depended on the stamp bytes).  The rule: under
   the virtual clock (xtc_sim_clock_active()) a wall-clock-like value
   is the virtual time plus a FIXED constant, never an offset sampled
   from real time; off the simulator it may anchor to the wall clock.
   hlc_phys_ms and vxf_now_ms ('now') now do that, and the state a run
   starts from must be a function of the seed too (a commit clock left
   by an earlier run or an off-sim commit carries its time forward:
   reset it, xstore_clock_reset, or make every commit of the seed run
   under the virtual clock).  The sqlxtc DST tiers hash every commit
   stamp into their replay check, and `test_sim_sqlxtc_ckpt 1 6 sleep`
   lets real time pass between the runs, so a leak fails every time.

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

5. PIN EVERY FAILING SEED.  A seed that failed once is a REPRODUCER,
   and it is the only thing that proves the fix still holds next year.
   Random sweeps roll fresh seeds nightly, so an unrecorded failing
   seed is GONE.  When any sweep fails, add a row to
   test/sim/corpus/seeds.txt: <test> <seed> <count> <description>, with
   the FIXING COMMIT in the description.  scripts/dst-corpus.sh re-runs
   the whole corpus on every push and fails if a pinned seed regresses.
   The swarm prints a paste-ready row on failure -- use it.

   Verify a pin BOTH ways before trusting it: the offset must reproduce
   the named seed AND FAIL on the pre-fix build.  A pinned row that
   passes without the fix is decoration, not a guard.  (The swarm's
   reported seed is DERIVED, 0x9E3779B97F4A7C15 * (base+s+1), not an
   argv the test takes, so the pin is the base OFFSET.)

   Pinned seeds and volume sweeps are COMPLEMENTARY, not substitutes:
   the corpus proves old bugs stay fixed, fresh seeds find new ones.
   Never delete a corpus row to make a sweep green.

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

## Cutting a release -- what green does NOT cover

`make check` passing is NOT a qualified release.  Three tiers are
invisible to it, and each has shipped a real defect through a fully
green run:

1. **`make check-dst` needs its own build** (`--with-io-backend=sim`),
   so a sim-only regression hides behind a green native run.  v1.49.2
   did exactly this: a cyclic slab free list that hung
   `xtc_slab_destroy` forever was invisible to `make check` on both
   epoll and io_uring, and only `test_sim_lockmgr` caught it.  RUN
   `make check-dst`.

2. **A TAG DOES NOT RUN THE NIGHTLY TIERS.**  `ci.yml`'s push filter is
   `branches: [main]`; the 100k-seed swarm and CBMC gate on
   `schedule`/`workflow_dispatch`.  A tag arrives as a `push`, so those
   jobs SKIP -- and a skip does not fail the pipeline.  RUN THE 100k
   SWARM BY HAND at release time:

   ```sh
   # 4 disjoint shards x 25k, the same split CI uses
   for base in 0 25000 50000 75000; do
       ./build_sim/test_sim_swarm 25000 "$base" > "swarm-$base.log" 2>&1 &
   done; wait; grep -l FAIL swarm-*.log
   ```

   At v1.49.2 this found 3 failing seeds on a freshly-armed oracle that
   nothing else would have surfaced before publication.  Pin whatever it
   finds (see the corpus rule in the DST yardsticks above).

3. **MULTIPLE BACKENDS, not just the default.**  Run at least one
   readiness backend AND one completion backend.  When no BSD/Windows
   host is available, `poll` and `select` are the best available proxy
   for kqueue: they keep a USERSPACE fd registry, which is where the
   historical kqueue multi-registration bug lived, so they exercise that
   hazard class for anything touching fd-registration or cancellation
   cleanup.  They are NOT a substitute for a real kqueue/IOCP run --
   state plainly which platforms were not tested.

State the qualification precisely and name what was NOT run.  "Green"
without the tier list is how a release claims more than it tested.

4. **PERFORMANCE, against the previous release.**  `make check` cannot
   see a slowdown.  Run `scripts/perf-gate.sh vPREV HEAD OUTDIR` on one
   dedicated, measurement-grade host (deep CPU idle states off: the
   script refuses otherwise).  It builds both releases and A/Bs 19 hot
   path cells; a cell fails when its median is worse by more than its
   threshold AND the ranges do not overlap.  A regression that ships on
   purpose is passed by name (`PERF_GATE_ACCEPT=cell`) and named in the
   release notes.  See bench/PERF_GATE.md.  Never accept an agent's
   speedup measured on one host without re-running the cell on the
   gate host: a +70% on 16 vCPUs was -45% on 64 in 1.59.

5. **io_uring use**, with `scripts/uring-gate.sh` (below).

## io_uring use is a release gate (uringscope)

libxtc's Linux performance rests on io_uring, and io_uring fails quietly:
a request that falls off the fast path onto an io-wq worker thread, a
CQ that overflows, a request that never completes, or two in-flight
requests on one buffer all return success.  `strace` cannot see inside
the rings.  uringscope (in the nix dev shell; github.com/rch0wdhury/
uringscope) traces the kernel's io_uring tracepoints and names these.

- **During development**, run the test or example you are changing
  under it whenever the change touches src/io/, src/ptc/aio.c, the
  loop's poll path, sqlxtc's WAL or page I/O, or an example's socket
  handling:

  ```sh
  sudo uringscope -f --json=r.json -- ./test_x
  jq -r '.findings[] | "\(.severity) \(.tag): \(.message)"' r.json
  ```

  Use `-f` (libxtc runs one ring per loop thread), an absolute path
  (sudo's PATH drops `.`), and a run long enough to observe:
  uringscope polls every 100 ms and a test that ends sooner reports
  NODATA, which is not a pass.  `--check` adds buffer-hazard detection
  (overlapping in-flight buffers, registered-buffer lifetime, unmap
  while in flight); run it over the io tests the way you would ASan.

- **As a release gate**, `sudo scripts/uring-gate.sh BUILD_DIR OUTDIR`
  (CHECK=1 for the hazard pass) runs the aio, exec, WAL, crash-oracle
  and rexis workloads.  A WARN/CRIT finding fails it unless the
  workload's row in the script lists the tag as understood (and says
  why, with a control measured on plain liburing).  NODATA fails a
  workload that should use io_uring.  INFO findings never fail; BATCH
  (SQEs per io_uring_enter) is reported so a release can show it
  moving.  Quote its table in the release qualification.

- **Adding an allowance is a decision, not a fix.**  Before allowing a
  PUNT, reproduce it with plain liburing (no libxtc) on the same kernel
  and filesystem; if liburing does not punt, libxtc is doing something
  wrong.  The current allowances (FSYNC, buffered writes contending on
  one inode from many rings, page-file extension) were each measured
  that way.

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
