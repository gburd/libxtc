# xtc

**A concurrency runtime for serious C programs.**

[![CI](https://github.com/gburd/libxtc/actions/workflows/ci.yml/badge.svg)](https://github.com/gburd/libxtc/actions/workflows/ci.yml)
[![Docs](https://github.com/gburd/libxtc/actions/workflows/pages.yml/badge.svg)](https://gburd.github.io/libxtc/)
[![Sanitizers](https://img.shields.io/badge/tested-ASan%20%2B%20UBSan-brightgreen)](https://github.com/gburd/libxtc/actions/workflows/ci.yml)
[![License: ISC](https://img.shields.io/badge/license-ISC-blue.svg)](LICENSE)
[![Release](https://img.shields.io/badge/release-v1.25.0-informational)](https://codeberg.org/gregburd/libxtc/releases)
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
  (`xtc_res`) with high-water alert callbacks so you can hold
  bounded RSS, file descriptors, in-flight tasks, and bandwidth
  under stress.  Backpressure is built in; OOM-spirals are not.

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
  per-commit Windows CI remains an MSVC xtc.lib + smoke build.  macOS and
  AIX have OS-layer ports; AIX awaits a test host.  See
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
#include <xtc.h>
#include <xtc_loop.h>
#include <xtc_proc.h>

static void
worker(void *arg)
{
    xtc_pid_t parent = *(xtc_pid_t *)arg;
    xtc_send(parent, "hello", 5);
}

int
main(void)
{
    xtc_loop_t *loop;
    xtc_pid_t   self, child;
    void       *msg; size_t sz;

    xtc_loop_init(&loop);
    self = xtc_self();

    xtc_proc_spawn(loop, worker, &self, NULL, &child);

    /* Wait for "hello" with a 1-second timeout. */
    if (xtc_recv(&msg, &sz, 1000LL * 1000 * 1000) == XTC_OK) {
        printf("got %zu bytes from worker\n", sz);
        free(msg);
    }
    xtc_loop_run(loop);
    xtc_loop_fini(loop);
    return 0;
}
```

Compile:
```sh
cc my.c -lxtc -lpthread -o my
```

That's a one-process actor system in 25 lines.

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

Other examples in `examples/`:

| Example | What it shows |
|---|---|
| `01_hello_async/` | A single async task with a timer |
| `02_proc_pingpong/` | Two BEAM processes bouncing messages |
| `03_supervised_app/` | Crash a worker, watch the supervisor restart it |
| `04_lockmgr_demo/` | The 9-mode transactional lock manager |
| `05_rexis/` | Networked, budgeted, multi-command Redis-compat server |

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

xtc is **1.0**.  The public API surface is stable and semver applies
from here: no breaking change to a documented public API without a major
version bump.  The semver / deprecation policy is documented in
`docs/abi-stability.md`.

What's working today:

| Layer | Status |
|---|---|
| L0 OS substrate | Linux, FreeBSD, illumos runtime-verified; Windows (MinGW/Clang64/MSVC) and macOS OS-layer ports build; AIX builds, awaits a test host. |
| L1 I/O | io_uring, epoll, kqueue, poll, select, and illumos event-ports (port_*) runtime-verified (the last on big-endian sparcv9, including its native SIGEV_PORT file-AIO path).  IOCP (Windows) runtime-verified on a host with MinGW (loop/task/timer/wakeup/socket-poll/file-AIO); AIX pollset COMPILES and is code-reviewed but not yet runtime-verified.  Per-commit CI runs Linux + macOS at runtime; Windows CI is a build-only smoke. |
| L2 event runtime | Done.  Single + multi-loop, work stealing, hand-written x86_64 fcontext (~7.6 ns/swap) + 7 more arches + ucontext fallback. |
| L3 primitives | Done.  Channels, processes, sync, RCU, lwlock, lrlock, lockmgr, slab, resource caps, observability. |
| L4 orchestration | Done.  Supervisors (4 strategies), gen_server, registry, app bringup, hierarchical mctx. |
| L5 PG adapter | Designed; not yet implemented. |
| TLS | OpenSSL, GnuTLS, wolfSSL, Mbed TLS, and BoringSSL backends build and pass the m18 suite in CI (`docs/M_TLS_MATRIX.md`); SChannel (Windows) is compile-only. |

Test coverage today: **480+ munit test cases + 23 hegel-c property
tests on Linux** (the munit total spans the L0-L4 suites plus the 35
OTP/gen_server cases), clean under AddressSanitizer and UBSan in CI.
GitHub CI also runs the full C munit suite on **macOS** (Apple Silicon:
kqueue + ucontext + GCD dispatch semaphores) and an **MSVC** xtc.lib +
smoke build on **Windows** every commit.  FreeBSD 15 (clang, kqueue)
was re-verified against the current tree (full gmake check passes,
including the native kqueue file-AIO path); the Windows IOCP runtime
was runtime-verified on a host with MinGW.  illumos (SunOS 5.11,
UltraSPARC v9 / big-endian sparcv9, gcc) was also re-verified against
the current tree (full gmake check, OpenSSL 3, native event-port
file-AIO).  None of
FreeBSD/illumos/Windows-runtime is in per-commit CI yet.  Windows also
passes ~233 munit under MinGW and 48/48 of the buildable binaries
under Clang64 in prior runs.

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
make check-dst                          # 52+ deterministic-simulation tests
                                         # (test/sim/); the project's
                                         # strongest correctness tier
```

```sh
<!-- M0_CLAIMS:B2_BEGIN -->
meson setup build_meson
meson compile -C build_meson
<!-- M0_CLAIMS:B2_END -->
meson test -C build_meson
```

Configure flags worth knowing:

| Flag | What it does |
|---|---|
| `--with-io-backend=AUTO` | Pick io_uring, epoll, kqueue, IOCP, poll, select; defaults are sensible per-OS |
| `--with-tls=openssl|none|auto` | Build TLS support (OpenSSL only today) |
| `--with-liburing=PATH` | Use a specific liburing install |
| `--with-hegel=PATH` | Enable property-based tests via the hegel-c framework |

A meson build is also provided (`meson.build`), but it currently
compiles only the M0 subset of the library (the OS/portability layer),
not the full runtime -- it is a work in progress.  For a complete
libxtc build use the autotools path above; meson is not yet at parity
and is not exercised in CI.

## Documentation

* `examples/` -- start here.  Five working programs from "hello async" to "Redis-compat server with budgets".
* `docs/getting-started.md` -- step-by-step beginner walkthrough, from "just cloned" to an async TCP server.
* `man/man3/` and `man/man7/` -- per-API reference.  Every public `xtc_*` symbol has a man page (coverage is gate-enforced in `make check`).
* `PLAN.md` -- the full design rationale.  Long but exhaustive.
* `docs/ARCHITECTURE.md` -- the layer diagram, the principles, the why.
* `docs/abi-stability.md` -- semver and deprecation policy.
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
* PostgreSQL's pluggable buffer manager / aio work
* libumem, BDB, DBSQL -- where the BDB/DBSQL build conventions come from
* The lrlck PostgreSQL branch -- where xtc_lrlock and xtc_lwlock come from
