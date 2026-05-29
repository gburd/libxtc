---
title: libxtc
---

# libxtc documentation

libxtc is a C11 library that provides async concurrency primitives
in the Tokio + Seastar + BEAM tradition.  This site is the rendered
documentation tree.

## Reading order

For a programmer new to libxtc:

  1. [Architecture](ARCHITECTURE.html) -- the layer model and what
     each layer is responsible for.
  2. [Getting started](getting-started.html) -- build, smallest
     working program, walkthrough of the channel and process APIs.
  3. [Locks and synchronization](locks.html) -- which primitive to
     reach for and when.
  4. [API reference](API.html) -- the full public surface.

For a programmer porting code in:

  * [Locks and synchronization](locks.html) -- reading the existing
    code's lock decisions against xtc's primitive set.
  * [Known issues](KNOWN_ISSUES.html) -- workarounds and caveats.
  * [ABI stability](abi-stability.html) -- what is and isn't
    guaranteed to remain unchanged.

For an operator deploying libxtc:

  * [Architecture](ARCHITECTURE.html) -- to understand resource
    consumption.
  * The TLS, libc, and Windows matrices below.

## Topic guides

Lower-level material organized by topic:

  * [BEAM lessons](M_BEAM_LESSONS.html) -- production
    synchronization issues observed in the BEAM/OTP fleet, and
    libxtc's posture against each.
  * [LRLock COW mode](M_LRLOCK_COW.html) -- copy-on-write design
    for left-right locks where the protected data is large.
  * [Multi-headed receive analysis](M_MULTI_HEAD_RECV.html) --
    why libxtc rejects general multi-head match in `xtc_recv` and
    what to use instead.
  * [SQLite hard-fork plan](M_SQLXTC_HARDFORK.html) -- staged plan
    for breaking SQLite's monolithic mutex.
  * [libxtc / PostgreSQL boundary](M_LIBXTC_PG_BOUNDARY.html) --
    layering rules for the eventual PG adapter.

## Build and platform matrices

  * [Windows toolchains](M_WINDOWS_MATRIX.html)
  * [TLS backends](M_TLS_MATRIX.html)
  * [libc implementations](M_LIBC_MATRIX.html)

## Manual pages

The shipping man pages are in the source tree at `man/man3/` and
`man/man7/`.  They are not currently rendered to HTML in this
site; install libxtc and use `man xtc_lwlock` (etc.) on the
target system.

## Source

  * Git repository: <https://codeberg.org/gregburd/libxtc>
  * License: ISC.  See [LICENSE](https://codeberg.org/gregburd/libxtc/src/branch/main/LICENSE).
