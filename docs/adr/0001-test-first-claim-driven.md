# ADR-0001: Test-first claim-driven development

- **Status:** Accepted (M0).
- **Date:** 2026-05-25.
- **Supersedes:** —

## Context

xtc is intended as a foundational library for a threaded
PostgreSQL.  PostgreSQL is twenty-eight years old and counting; a
foundation for it has to last on the order of decades.

A library used at that horizon is shaped less by what it can do
on day one than by what is true about it that *cannot drift*.
Claims in documentation drift away from claims in code; claims
in tests drift more slowly because they execute in CI; claims
in code without tests drift quickest of all.

## Decision

Every claim made in xtc code or documentation must have a test
that asserts it.

The mechanism:

1. Each milestone Mn ships a `Mn_CLAIMS.md` with a numbered table
   of claims, each tagged with a unique ID
   (e.g. `[C1]`, `[B3]`, `[T2]`, `[D5]`).
2. Each claim ID points to a test under `test/Mn/` or `test/dist/`.
3. Documentation that asserts a claim references the claim ID.
4. CI runs every test in every `M*_CLAIMS.md` for every milestone
   that has been merged.
5. Adding a new public-facing behaviour means: add the claim,
   write the test (and watch it fail), implement until the test
   passes, update the documentation in the same commit.

## Consequences

### Positive

- Documentation cannot say things the code does not do.
- Behaviour we depend on cannot drift away silently.
- A reviewer can audit a claim by running its test.
- The discipline composes: every later milestone gets the
  observability, hooks, and longevity tests of the earlier ones
  for free.

### Negative

- Up-front cost is higher than "implement, hope, document later."
- Some claims are awkward to test (e.g. "the API is intuitive");
  we make those non-claims and don't promise them.
- Shell tests for build/doc claims add toolchain prerequisites
  (`shellcheck`, `mandoc`); we make these `SKIP` cleanly when
  absent rather than block the build.

## Alternatives considered

- **Tests-as-needed.** The default in most C libraries.  Rejected
  because by the time you need the test, the bug has already
  shipped and the fix is the regression test.
- **Documentation-first without enforcement** (e.g.  RFCs that
  describe behaviour and aren't directly executed).  Rejected
  because there is no detector for drift.
- **Hand-curated changelog** instead of mechanical claim
  tracking.  Rejected because changelogs describe deltas, not
  the standing contract.

## Notes

- This ADR is itself part of the contract: changing the
  test-first discipline requires a successor ADR with status
  `Supersedes ADR-0001`.
- See [`abi-stability.md`](../abi-stability.md) for how this
  doctrine combines with SemVer to produce the longevity
  promise.
