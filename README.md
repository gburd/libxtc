# xtc

**A concurrency runtime for serious C programs.**

[![CI](https://github.com/gburd/libxtc/actions/workflows/ci.yml/badge.svg)](https://github.com/gburd/libxtc/actions/workflows/ci.yml)
[![Docs](https://github.com/gburd/libxtc/actions/workflows/pages.yml/badge.svg)](https://gburd.github.io/libxtc/)
[![Sanitizers](https://img.shields.io/badge/tested-ASan%20%2B%20UBSan-brightgreen)](https://github.com/gburd/libxtc/actions/workflows/ci.yml)
[![License: ISC](https://img.shields.io/badge/license-ISC-blue.svg)](LICENSE)
[![Release](https://img.shields.io/badge/release-v1.51.0-informational)](https://codeberg.org/gregburd/libxtc/releases)
[![C11](https://img.shields.io/badge/C-11-blue.svg)](https://en.wikipedia.org/wiki/C11_(C_standard_revision))

**[Read the documentation &rarr;](https://gburd.github.io/libxtc/)**
(mirror: <https://gregburd.codeberg.page/libxtc/>)

xtc gives C the same asynchronous, fault-tolerant, predictable-latency
foundation that Tokio gives Rust, that the BEAM gives Erlang, and that
Seastar gives C++.  It is a single library you can link against to write
network servers, databases, queues, schedulers, and any other long-lived
service that needs to handle thousands of connections, recover from
faults, and stay inside a fixed resource budget on commodity hardware.

```
+-------------------------------------------------------+
|                    your program                       |
+-------------------------------------------------------+
|  orchestration:  supervisor  app  registry  gen_server|
+-------------------------------------------------------+
|  primitives:  proc  channel  lwlock  lrlock  lockmgr  |
|               rcu   slab     mctx    res     log      |
+-------------------------------------------------------+
|  event runtime:  loop  task  timer  fiber  executor   |
+-------------------------------------------------------+
|  I/O substrate:  io_uring  epoll  kqueue  IOCP  poll  |
+-------------------------------------------------------+
|  OS substrate:  alloc atomic time thread tls mutex    |
+-------------------------------------------------------+
```

## Why you might want this

* **You are writing a server in C** and you want async I/O without
  building it from scratch.  xtc gives you a backend-pluggable event
  loop (io_uring, epoll, kqueue, IOCP, poll, select) under a uniform
  API, with non-blocking task scheduling and fiber-based coroutines.

* **You need fault tolerance.**  xtc has Erlang-style processes with
  links and monitors, supervisors with the four canonical strategies
  (`one_for_one`, `one_for_all`, `rest_for_one`, `simple_one_for_one`),
  and a `gen_server`-shaped server abstraction.  Crashes are caught
  and restarted by a tree, not by your shell script.

* **You care about tail latency.**  xtc has resource accountants
  (`xtc_res`) with high-water alert callbacks.  Metering is opt-in and
  covers exactly what you attach: slab chunks and `xtc_mctx` arenas
  (`MEM_BYTES`), sockets `xtc_net` opens (`FDS`), and tasks you charge.
  It does not see plain `malloc`, or descriptors you `accept(2)`
  yourself -- see `xtc_res(3)` for the full list.  Mailboxes and
  channels are bounded, so backpressure is built in.

* **You want portable source.**  The same source builds and passes
  its tests on Linux (glibc and musl), FreeBSD, illumos, and Windows.
  Linux is exercised on every commit by CI (gcc and clang, plus
  AddressSanitizer, UBSan, and the forced-fcontext substrate that
  musl uses), as are macOS (Apple Silicon, the kqueue backend +
  ucontext substrate, full C munit suite) and Windows/MSVC (xtc.lib +
  smoke test).  FreeBSD 15 (clang, kqueue) was re-verified against the
  current tree (full gmake check passes, including the native kqueue
  file-AIO path).  illumos (SunOS 5.11, UltraSPARC v9 / big-endian
  sparcv9, gcc, the event-ports backend) was re-verified against the
  current tree (full gmake check, with OpenSSL 3).
  Windows builds
  with all three of MinGW, Clang64, and MSVC.  The IOCP runtime (AFD
  socket poll, cross-thread wakeup, file AIO) was runtime-verified on a
  Windows host with MinGW (loop/task/timer/waker/net + file-AIO); the
  per-commit Windows CI remains an MSVC xtc.lib + smoke build.  macOS
  has an OS-layer port.  (An AIX/ppc64 OS-layer port exists in-tree and
  compiles, but AIX is NOT a supported or maintained target -- it is
  unverified and off the roadmap; see docs/KNOWN_ISSUES.md.)  See
  `docs/M_WINDOWS_MATRIX.md`,
  `docs/M_LIBC_MATRIX.md`, and PLAN.md for the per-platform status.

* **You want to stay close to the metal.**  No GC, no STW pauses.
  Memory comes from cache-line-padded slab caches with optional
  shared-memory mode (a BDB-style `roff_t` pointer-into-region works
  across processes).  Small messages (payload up to 256 bytes) are
  served from a per-thread envelope pool, so the common send path
  takes no allocator round-trip; larger messages fall back to malloc.
  Reads on the read-mostly primitive (`xtc_lrlock`) are wait-free.

## A 30-second taste

```c
#include <stdio.h>

#include <xtc.h>        /* XTC_OK, xtc_free */
#include <xtc_loop.h>   /* the event loop */
#include <xtc_proc.h>   /* procs, mailboxes, send/recv */

/* The worker runs on a fiber.  It is handed the pid to reply to. */
static void
worker(void *arg)
{
    xtc_pid_t parent = *(xtc_pid_t *)arg;

    (void)xtc_send(parent, "hello", 5);
}

/* The parent spawns the worker from INSIDE a proc, so it has a pid to be
 * replied to and a mailbox to receive in. */
static void
parent(void *arg)
{
    xtc_loop_t *loop = arg;
    xtc_pid_t   self = xtc_self();   /* a real pid: we are on a proc */
    void       *msg;
    size_t      sz;

    if (xtc_proc_spawn(loop, worker, &self, NULL, NULL) != XTC_OK)
        return;

    /* Wait up to one second for the worker's message. */
    if (xtc_recv(&msg, &sz, 1000LL * 1000 * 1000) == XTC_OK) {
        printf("got %zu bytes from worker\n", sz);
        xtc_free(msg);           /* xtc_free, not free(3): libxtc may
                                  * use its own allocator */
    }
}

int
main(void)
{
    xtc_loop_t *loop;

    if (xtc_loop_init(&loop) != XTC_OK)
        return 1;
    if (xtc_proc_spawn(loop, parent, loop, NULL, NULL) != XTC_OK)
        return 1;
    (void)xtc_loop_run(loop);        /* runs both procs to completion */
    (void)xtc_loop_fini(loop);
    return 0;
}
```

Compile and run:
```sh
cc my.c -lxtc -lpthread -o my && ./my
got 5 bytes from worker
```

That's a one-process actor system in about 50 lines.  Note the shape:
`xtc_self()` and `xtc_recv()` are **proc-context** calls -- off a proc
`xtc_self()` returns `XTC_PID_NONE` and `xtc_recv()` rejects with
`XTC_E_INVAL`, so the receive lives inside a proc and `main()` only
spawns and runs the loop.  This exact program is
[`docs/_includes/snippets/00_readme_taste.c`](docs/_includes/snippets/00_readme_taste.c),
compiled and run by `make check` -- doc code is a release gate here.

## Where it shines

`examples/05_rexis/` is a working Redis-protocol server in ~4,800 LOC
(command handling, storage, connections, expiry, metrics; excludes
tests). It uses every major xtc subsystem and stays inside hard
`--max-memory`,
`--max-keys`, `--max-clients`, `--max-iops`, and `--cores` caps under
load.  Run it with:

```sh
cd examples/05_rexis && make
./rexis-server-xtc -p 6379 --max-memory=$((100*1024*1024)) --max-clients=10000
```

Then talk to it with `redis-cli` like any Redis server.

Other examples in `examples/` (the full list, with per-example design
notes, is [`examples/README.md`](examples/README.md)):

| Example | What it shows | Gated by |
|---|---|---|
| `01_hello_async.c` | A single async task with a timer | `make check` (runs) |
| `02_proc_pingpong.c` | Two BEAM processes bouncing messages | `make check` (runs, asserts) |
| `03_supervised_app.c` | Crash a worker, watch the supervisor restart it | `make check` (runs) |
| `04_lockmgr_demo.c` | The 9-mode transactional lock manager, with a pluggable (randomized) deadlock-victim policy | `make check` (runs) |
| `05_rexis/` | Networked, budgeted, multi-command Redis-compat server | CI `make check-rexis` (RESP, loopback, measured budgets) |
| `06_sqlxtc/` | A from-scratch SQL engine (parser, vectorized executor, B-link + buffer pool + WAL) | CI (in-process tests, differential oracle vs sqlite3) |
| `07_kaka/` | Kafka-shaped partitioned log broker with credit backpressure | CI (in-process tests) |
| `08_tnt/` | The Isolate layer: thread-per-core stackless state machines, TCP echo | `make check` (built, `--help` lint) |
| `09_pgmock/` | A mock PostgreSQL backend on the xtc scheduler -- zero PG source | CI (built, `--help` lint only -- not run) |
| `10_circuit_breaker.c` | The circuit-breaker pattern as an `xtc_fsm` (gen_statem) | `make check` (runs, asserts) |
| `11_lorb/` | A price-time limit order book / matching engine, with benchmarks | CI (in-process tests) |

## Built on three traditions

xtc owes a lot to three runtimes that came before:

* **Tokio (Rust)** -- the work-stealing executor model, futures,
  channels, and the principle that single-threaded primitives are
  faster than locks when you can get away with them.
* **The BEAM (Erlang/Elixir)** -- processes, mailboxes, selective
  receive, links, monitors, supervisors, and the philosophy that
  "let it crash" is a feature when the supervisor tree is well-designed.
* **Seastar (C++)** -- thread-per-core, share-nothing reactors,
  cache-line awareness, and the discipline that the runtime must
  not allocate on the hot path.

> You either start with BEAM or you build it over the years in your
> stack and in your infra -- as detailed in "You Built an Erlang":
> <https://vereis.com/posts/you_built_an_erlang>

Where these conflict, xtc picks the choice that's most idiomatic in C
and explains why in `PLAN.md`.  Read that file when you want to
understand the *why*; read the man pages and headers when you want
the *what*.

## Status and stability

xtc is **1.0**.  The public API surface is stable in the sense that no
documented `xtc_*` function has been removed, renamed, or had its
signature changed during 1.x.

**One caveat a packager must read before mixing versions:** several
caller-allocated option and info structs have GROWN during 1.x, and two
had fields inserted mid-struct rather than appended.  Source
compatibility is intact; **binary** compatibility across minors is not.
Recompile consumers against the headers of the exact minor whose library
they link, and do not mix.  The measured sizes and the affected structs
are in [docs/KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md); the compatibility
policy, and an explicit split between what is mechanically enforced and
what is only intention, is in `docs/abi-stability.md`.  There is **no
automated ABI or signature diff** between release tags -- run `abidiff`
yourself if you need that guarantee.

What's working today:

| Layer | Status |
|---|---|
| L0 OS substrate | Linux, FreeBSD, illumos runtime-verified; Windows (MinGW/Clang64/MSVC) and macOS OS-layer ports build.  (An AIX/ppc64 OS-layer port compiles in-tree but AIX is NOT supported/maintained -- unverified, off the roadmap.) |
| L1 I/O | io_uring, epoll, kqueue, poll, select, and illumos event-ports (port_*) runtime-verified (the last on big-endian sparcv9, including its native SIGEV_PORT file-AIO path).  IOCP (Windows) runtime-verified on a host with MinGW (loop/task/timer/wakeup/socket-poll/file-AIO); AIX pollset COMPILES and is code-reviewed but not yet runtime-verified.  Per-commit CI runs Linux, macOS, FreeBSD, and riscv64 at runtime; Windows CI is a build-only smoke. |
| L2 event runtime | Done.  Single + multi-loop, work stealing, hand-written `fcontext` asm for 7 CPU families (x86_64, aarch64, arm, ppc64le, riscv64, s390x, sparc64; 12 `.S` + 2 MASM variants covering the SysV / MS-PE / Mach-O ABIs) + ucontext fallback + Win32 fibers.  The bare `__xtc_jump_fcontext` swap is ~7.6 ns on x86_64; a full `xtc_yield` -- the consumer-visible cost, including run-queue turn and per-fiber TLS -- measures ~455 ns/op in `bench_micro` on this workstation. |
| L3 primitives | Done.  Channels, processes, sync, RCU, lwlock, lrlock, lockmgr, slab, resource caps, observability. |
| L4 orchestration | Done.  Supervisors (4 strategies), gen_server, registry, app bringup, hierarchical mctx. |
| Process groups | Done.  `xtc_pg` (Erlang `:pg`): named, single-node pid groups with join/leave/broadcast; tested in `test/m10/test_pg.c` and under DST.  Cross-node groups await the unbuilt distributed module. |
| TLS | OpenSSL, GnuTLS, wolfSSL, Mbed TLS, and BoringSSL backends build and pass the m18 suite in CI (`docs/M_TLS_MATRIX.md`); SChannel (Windows) is compile-only.  Since 1.50 a CLIENT verifies the server certificate AND host name by default on the four non-Windows backends, a write to a dead peer is an error rather than a process-killing `SIGPIPE`, and a zeroed `xtc_tls_opts_t` is secure.  SChannel applies the same verify default since 1.51 but does not yet check the host name. |

Test coverage today, measured against this tree (v1.49.1 plus the
allocator / cancellation / accounting regression tests that landed after
it): **644 munit test cases across 109 munit binaries on Linux**, of
which 2 skip here (a macOS-only preemption-timer case and one slab
pressure case), clean under AddressSanitizer and UBSan in CI, plus 34
shell gates, 7 standalone C harnesses, the 68-file
deterministic-simulation tier (a separate `--with-io-backend=sim` build,
`make check-dst`), and **36 hegel properties across 17 suites** that all
pass when the tier is enabled with `--with-hegel`.  Recount rather than
trust these numbers if you are citing them: a full `make tests-c` run of
this tree reports 642 successful + 2 skipped, and
`for t in $(...TESTS_C...); do ./$t --list; done | grep -c '^/'` gives
the same 644.

The property tier is opt-in because it needs `libhegel`
([hegeldev/hegel-rust](https://github.com/hegeldev/hegel-rust)), an
in-process C-ABI shared library.  `nix develop` provides it, so
`configure --with-hegel` finds it via pkg-config with no other setup.
Without it, `make check` prints a loud per-suite SKIP and a count of
unverified properties rather than passing silently -- a green run without
`--with-hegel` does NOT mean those properties hold.  See
[ADR-0002](docs/adr/0002-hegel-pbt-first-class.md).
GitHub CI runs, on every push and pull request: gcc and clang `make
check`, AddressSanitizer, UndefinedBehaviorSanitizer, ThreadSanitizer,
Valgrind, a forced-fcontext (musl coroutine path) build, the DST sim
tier, the five TLS backends, the examples, **macOS** (Apple Silicon:
kqueue + ucontext + GCD dispatch semaphores, full C munit suite),
**FreeBSD** in a VM (clang + kqueue, `gmake check` -- this job gates
again since the 1.48.0 strand fix), **riscv64** under qemu-user (C +
property suites), and an **MSVC** `xtc.lib` + smoke build on
**Windows**.  The property tier's own job is advisory
(`continue-on-error`) pending a runner-specific failure; see
KNOWN_ISSUES.

NOT in per-commit CI, and therefore verified by hand against this tree
rather than continuously: illumos (SunOS 5.11, UltraSPARC v9 /
big-endian sparcv9, gcc -- full `gmake check`, OpenSSL 3, native
event-port file-AIO) and the Windows IOCP runtime (runtime-verified on a
host with MinGW: loop/task/timer/waker/net + file AIO).  The Windows
MinGW and Clang64 munit numbers quoted in older notes (~233 munit under
MinGW, 48/48 buildable binaries under Clang64) are from earlier manual
runs and have not been re-measured against this tree -- treat them as
historical.

Honest gaps and known issues live in [docs/KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md).
The original design plan (a historical bring-up document) is in
[PLAN.md](PLAN.md); it predates the implementation and does not track
current status -- the live status is this table plus KNOWN_ISSUES.

## Building

xtc is BSD-style C11.  No external deps beyond libc, pthreads, and
optionally `liburing`/`OpenSSL`.

```sh
<!-- M0_CLAIMS:B1_BEGIN -->
cd dist && autoreconf -i && cd ..
mkdir -p build_unix && cd build_unix
../dist/configure                      # autodetects io backend + tls
make -j$(nproc)
<!-- M0_CLAIMS:B1_END -->
make check                              # full test suite
sudo make install                       # libxtc.a + headers + man pages
```

`make check` builds and runs the unit/PBT/shell-gate suite (fast, minutes).
It does NOT include the DST suite -- that needs a separate
`--with-io-backend=sim` build.  Run it explicitly:

```sh
make check-dst                          # deterministic-simulation tests
                                         # (test/sim/); the project's
                                         # strongest correctness tier
```

```sh
<!-- M0_CLAIMS:B2_BEGIN -->
meson setup build_meson -Dtls=openssl
meson compile -C build_meson
<!-- M0_CLAIMS:B2_END -->
meson test -C build_meson
```

Configure flags worth knowing:

| Flag | What it does |
|---|---|
| `--with-io-backend=AUTO` | Pick io_uring, epoll, kqueue, IOCP, poll, select; defaults are sensible per-OS |
| `--with-tls=auto|openssl|libressl|boringssl|gnutls|wolfssl|mbedtls|schannel|none` | TLS backend (default `auto`) |
| `--with-liburing=PATH` | Use a specific liburing install |
| `--with-hegel[=PREFIX]` | Property-based tests via `libhegel` (in-process C ABI).  With no PREFIX, found by pkg-config; `nix develop` supplies it |

The meson build (`meson.build` + `meson_options.txt`) is at parity with
the autotools build: it compiles the full static (and, with
`-Dshared=true`, shared) library from the same source list, with the
same io-backend / coroutine / TLS selection, and `meson test` registers
111 of the same C test binaries (measured: `meson test --list | wc -l`)
against `make check`'s 118-entry C tier.  The exported
`xtc_*` / `__xtc_*` symbol set is identical to the autotools
`libxtc.a` at the same optimization level (verified for this release:
885 defined symbols each, zero difference).  Its options mirror
the `./configure` flags:  `-Dio-backend=` (auto/poll/epoll/uring/kqueue/
iocp/solaris/aix/select/sim), `-Dtls=` (auto/openssl/libressl/boringssl/
mbedtls/gnutls/wolfssl/schannel/none), `-Daccel=` (auto/yes/no),
`-Dliburing=PATH`, `-Dshared=`, and `-Ddiagnostic=`.  configure.ac /
dist/Makefile.in remain the reference for WHAT is built; meson tracks
them.  Not yet wired here: the shell-gate tests (`TESTS_SH`), the
property-based tier (`TESTS_PBT`, needs `--with-hegel`), and the
deterministic-simulation tier (`make check-dst`) still run via the
autotools `make check`; and meson is not yet exercised in CI (a
follow-up).

## Documentation

* `examples/` -- start here.  Five working programs from "hello async" to "Redis-compat server with budgets".
* `docs/getting-started.md` -- step-by-step beginner walkthrough, from "just cloned" to an async TCP server.
* `man/man3/` and `man/man7/` -- per-API reference.  Every public `xtc_*` symbol has a man page (coverage is gate-enforced in `make check`).
* `PLAN.md` -- the full design rationale.  Long but exhaustive.
* `docs/ARCHITECTURE.md` -- the layer diagram, the principles, the why.
* `docs/abi-stability.md` -- the compatibility contract, split into what
  is mechanically enforced and what is only stated policy.
* `docs/KNOWN_ISSUES.md` -- everything I know about that's not perfect.

## License

ISC.  See [LICENSE](LICENSE).

## Contributing

Issues and patches welcome.  Code style is BSD KNF as encoded in
`.clang-format`.  All contributions must be ASCII-only in source,
docs, comments, and commit messages.  Run `dist/s_async` and
`dist/s_cfg` lints before submitting.  Property-based tests
(via hegel-c) are encouraged for any new primitive.

## Inspiration

* Tokio: <https://tokio.rs>
* The BEAM book: <https://blog.stenmans.org/theBeamBook/>
* Seastar: <http://seastar.io>
* Glommio: <https://github.com/DataDog/glommio> -- thread-per-core +
  io_uring, and the proportional-share scheduler libxtc's optional
  weighted-fair scheduling is modeled on
* FoundationDB and TigerBeetle -- the deterministic-simulation-first
  testing discipline libxtc holds itself to
* PostgreSQL's pluggable buffer manager / aio work
* Jon Gjengset's left-right concurrency technique:
  <https://github.com/jonhoo/left-right> -- the basis for `xtc_lrlock`
  (a C implementation of it)
