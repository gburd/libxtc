# libxtc documentation

libxtc is a C11 library that provides async concurrency primitives
in the Tokio + Seastar + BEAM tradition.  This site is the rendered
documentation tree.

## Reading order

For a programmer new to libxtc:

  1. [Architecture](ARCHITECTURE.md) -- the layer model and what
     each layer is responsible for.
  2. [Getting started](getting-started.md) -- build, smallest
     working program, walkthrough of the channel and process APIs.
  3. [Thinking in libxtc](guide/transitioning.md) -- the mental
     shifts a C/C++/Rust programmer must make, and the anti-patterns
     that bite if they do not.  Read this before writing real code.
  4. [Locks and synchronization](locks.md) -- which primitive to
     reach for and when.
  5. [Debugging and observing](guide/debugging.md) -- finding bugs
     in the message-passing model with GDB/LLDB and the runtime's
     introspection.
  6. [API reference](API.md) -- the full public surface.

For a programmer porting code in:

  * [Locks and synchronization](locks.md) -- reading the existing
    code's lock decisions against xtc's primitive set.
  * [Known issues](KNOWN_ISSUES.md) -- workarounds and caveats.
  * [ABI stability](abi-stability.md) -- what is and isn't
    guaranteed to remain unchanged.

For an operator deploying libxtc:

  * [Architecture](ARCHITECTURE.md) -- to understand resource
    consumption.
  * The TLS, libc, and Windows matrices below.

## Topic guides

Lower-level material organized by topic:

  * [Thinking in libxtc](guide/transitioning.md) -- mental-model
    transition for C/C++/Rust programmers and the anti-patterns to
    avoid.
  * [Debugging and observing](guide/debugging.md) -- task-oriented
    recipes with the GDB/LLDB tools (`tools/`) and the runtime
    introspection APIs.
## Build and platform matrices

  * [Windows toolchains](M_WINDOWS_MATRIX.md)
  * [TLS backends](M_TLS_MATRIX.md)
  * [libc implementations](M_LIBC_MATRIX.md)

## Manual pages

The shipping man pages are in the source tree at `man/man3/` and
`man/man7/`.  They are not currently rendered to HTML in this
site; install libxtc and use `man xtc_lwlock` (etc.) on the
target system.

## Source

  * Git repository: <https://codeberg.org/gregburd/libxtc>
  * License: ISC.  See [LICENSE](https://codeberg.org/gregburd/libxtc/src/branch/main/LICENSE).
