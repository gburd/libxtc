# Completion Program -- sqlxtc + libxtc

This document tracks the multi-track effort to take sqlxtc and libxtc
from "demonstrated" to "production-complete", and records honestly what
is verified versus written-but-unverified on the development host.

The development host is Linux/x86_64.  Some targets (AIX, ppc64le,
sparc64, Windows) cannot be runtime-verified here; those are built
and/or cross-run under QEMU where possible and otherwise marked
"written, not runtime-verified" -- the same discipline src/io/io_aix.c
already follows.

## Tracks

| # | Track | Owner | State |
| - | ----- | ----- | ----- |
| 1 | sqlxtc: B-tree leaf merge / page reclaim on delete | agent+lead | DONE ea8a2f9 |
| 2 | libxtc: real backtrace (execinfo / libunwind / DbgHelp) | agent | DONE 29ebd3c |
| 3 | libxtc: OS packaging (pkg-config, .so+map, deb, rpm, flake) | agent | DONE 7d7db64 |
| 4 | libxtc: fctx fiber asm for all PG archs (qemu-verified where possible) | agent+lead | DONE 6660077 |
| 5 | libxtc: TLS backends (mbedTLS, GnuTLS, wolfSSL, SChannel) | agent+lead | DONE 8f4b423 |
| 6 | libxtc: Windows IOCP to production (IOCP port + AFD poll) | agent | DONE 8b3e125 |
| 7 | sqlxtc: recovery completion (in-place redo, NTA SMO, fuzzy checkpoint) | agent | in progress |
| 8 | sqlxtc: isolation levels + savepoints + nested txn | lead | DONE 7d26093 |
| 9 | docs: truth-up every stale doc against code | lead + agents | in progress |

## Honesty ledger (what is runtime-verified vs not)

- **Backtrace**: glibc execinfo path verified on this host; libunwind
  (musl) path compiles + configure-detected; Windows DbgHelp written,
  not runtime-verified (no Windows host).
- **Packaging**: pkg-config xtc.pc, shared lib + symbol map, and the
  Nix flake build verified on this host; deb/rpm control files
  written, dpkg-buildpackage / rpmbuild not run here (tools absent).
- **fctx asm**: x86_64 native + aarch64 (glibc AND musl-static),
  ppc64le, riscv64, s390x ALL runtime-verified under qemu-user with a
  callee-saved int+FP round-trip harness.  arm32 has a known
  round-trip register-preservation bug (NOT trustworthy yet); sparc64
  written but no cross toolchain here to assemble/run.
- **TLS**: OpenSSL, LibreSSL, mbedTLS, GnuTLS, wolfSSL all pass the
  m18 server+client tests on this host.  SChannel compile-only.
- **Windows IOCP**: native overlapped redesign written; NOT
  runtime-verified -- needs the windows CI job / a Windows host.
- **B-tree delete merge**: single-threaded reclaim verified + ASan/
  UBSan clean; concurrent merge has a known structural race so it is
  DISABLED by default (concurrent deletes stay correct, just no
  reclaim).
- **Isolation/savepoints/nested-txn**: verified on this host
  (test_isolation, test_savepoint) + ASan/UBSan clean.
- **Recovery completion**: see track 7 (in progress).

## Completion gates (apply to every track)

- Test-first; a new or extended test proves the feature.
- Builds -Wall -Wextra -Wpedantic clean; ASCII-only; BSD KNF.
- ASan + UBSan clean on any concurrent path touched.
- Both CI workflows updated when a test is added.
- Docs state what the CODE does, not aspirations.  No claim of
  runtime verification that was not actually performed on real or
  emulated hardware.

## Honesty ledger (filled in as tracks land)

(Each track appends: what is runtime-verified, what is emulated under
QEMU, what is compiled-only, what is reviewed-only.)
