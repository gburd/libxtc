# DST failing-seed regression corpus

This directory pins specific `(sim test, seed)` pairs as permanent
regression tests, the way libFuzzer / AFL keep a corpus: once a bug is
fixed (or a planted bug is confirmed caught), the exact seed that
reproduced the failure is recorded here and re-run on every future
build, so the fix cannot silently regress.

It closes gap #2 of `.agent/DST_MATURITY_2026-07.md`: previously a
failing seed was fixed and forgotten; now it is a pinned replay.

## Format

Plain-text `*.txt`, one pinned case per line:

```
<sim_test_name> <seed> <count> <short-description>
```

  - `<sim_test_name>` -- a `test/sim/test_sim_*` basename (no `.c`).
    It must link against ONLY the sim library (`-pthread -ldl -lm`);
    tests that need the sqlxtc engine objects (test_sim_compose,
    test_sim_compose_crash, test_sim_crash_recover) are intentionally
    NOT in the corpus runner (they are exercised by run_sim_tests.sh
    and dst-bug-inject.sh instead -- keeping the corpus runner a thin,
    dependency-free gate).
  - `<seed>` -- the seed to pin.  For a test that takes `<base> <count>`
    on argv (test_sim_res, test_sim_sup_strategy, test_sim_swarm) this
    is the exact base seed the runner passes.  For a test with built-in
    seeds and no argv (test_sim_credit, test_sim_chan, test_sim_reg,
    test_sim_saga, test_sim_wake_park, test_sim_pingpong,
    test_sim_lockmgr, test_sim_torn) use `-`: the runner invokes the
    test with no arguments and the test replays its own fixed seeds.
  - `<count>` -- how many seeds from `<seed>` to sweep (argv tests only;
    ignored, use `-`, for the built-in-seed tests).
  - `<short-description>` -- free text to the end of line: why this
    seed is pinned (which bug it guards, or "known-good smoke").

Blank lines and lines beginning with `#` are comments.

## What is pinned, and why

Two kinds of entry (both must PASS on the current clean build):

1. **Planted-bug regression guards.**  Each entry names a seed the
   catching test in `scripts/dst-bug-inject.sh` runs.  With the planted
   bug active (`-DXTC_DST_INJECT_BUG=N`) that seed FAILS (the harness
   proves it); with the bug "fixed" (a normal build, which is what the
   corpus runner uses) the SAME seed must PASS.  That is the
   fixed-bug-cannot-regress guarantee: if someone reintroduces the bug
   (or weakens the invariant so the bug slips through), the pinned seed
   goes red here.

2. **Known-good smoke seeds.**  A couple of ordinary seeds per
   argv-driven test, so the corpus runner is also a fast, seed-explicit
   smoke of the invariant checkers themselves.

## Running

```sh
scripts/dst-corpus.sh          # from the repo root
```

Builds the sim-backend library once, compiles each distinct pinned test
once, runs every pinned case, and fails if any pinned (test, seed) does
not pass on the current clean build.  It is a separate target (not in
the default `make check`) and is wired into the `sim-dst` CI job next to
`dst-bug-inject.sh` -- the FAIL direction (bug active -> caught) and the
PASS direction (bug fixed -> stays fixed) gated side by side.

## Adding a case

When the swarm/soak or a CI run finds a failing seed, fix the bug, then
add a line here with the seed and a one-line description.  When you add
a planted-bug case to `dst-bug-inject.sh`, add the matching regression
seed here too.
