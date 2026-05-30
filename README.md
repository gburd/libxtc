# xtc

**A concurrency runtime for serious C programs.**

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
  smoke test).  FreeBSD 15 (clang, kqueue) and illumos (SunOS 5.11,
  gcc, the solaris port backend) are verified against the current
  tree -- 283/283 and a clean C suite respectively.  Windows builds
  with all three of MinGW, Clang64, and MSVC.  macOS and AIX have
  OS-layer ports awaiting a test host.  See `docs/M_WINDOWS_MATRIX.md`,
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

`examples/05_rexis/` is a working Redis-protocol server in ~2,000 LOC.
It uses every major xtc subsystem and stays inside hard `--max-memory`,
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

Where these conflict, xtc picks the choice that's most idiomatic in C
and explains why in `PLAN.md`.  Read that file when you want to
understand the *why*; read the man pages and headers when you want
the *what*.

## Status and stability

xtc is **pre-1.0**.  The public API surface is stable in shape but
specific signatures may shift before v1.0.  The semver / deprecation
policy is documented in `docs/abi-stability.md`.

What's working today:

| Layer | Status |
|---|---|
| L0 OS substrate | Done.  Linux, FreeBSD, illumos, Windows (MinGW, Clang64, MSVC); macOS + AIX OS-layer ports await a test host. |
| L1 I/O | Done.  io_uring, epoll, kqueue, IOCP, poll, select, illumos port_*, AIX pollset (untested). |
| L2 event runtime | Done.  Single + multi-loop, work stealing, hand-written x86_64 fcontext (~7.6 ns/swap). |
| L3 primitives | Done.  Channels, processes, sync, RCU, lwlock, lrlock, lockmgr, slab, resource caps, observability. |
| L4 orchestration | Done.  Supervisors (4 strategies), gen_server, registry, app bringup, hierarchical mctx. |
| L5 PG adapter | Designed (`docs/M16_PG_ADAPTER.md`); not yet implemented. |
| TLS | OpenSSL backend done (also builds + mostly passes on LibreSSL, see `docs/M_TLS_MATRIX.md`); GnuTLS/wolfSSL/Mbed TLS designed (`docs/M_TLS.md`). |

Test coverage today: **280 munit + 23 hegel-c property tests on
Linux**, clean under AddressSanitizer and UBSan in CI.  GitHub CI also
runs the full C munit suite on **macOS** (Apple Silicon: kqueue +
ucontext + GCD dispatch semaphores) and an **MSVC** xtc.lib + smoke
build on **Windows** every commit.  FreeBSD and illumos have been
verified at matching numbers in prior runs (a re-verify against the
current tree is pending); Windows also passes ~233 munit under MinGW
and 48/48 of the buildable binaries under Clang64.

Honest gaps and known issues live in [docs/KNOWN_ISSUES.md](docs/KNOWN_ISSUES.md).
The full milestone roadmap is in [PLAN.md](PLAN.md).

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

A meson build is also provided (`meson.build`), with the same
options and behaviour.

## Documentation

* `examples/` -- start here.  Five working programs from "hello async" to "Redis-compat server with budgets".
* `docs/getting-started.md` -- step-by-step beginner walkthrough.  TODO: the document currently lives only as fragments inside the examples; a unified guide is in flight.
* `man/man3/` and `man/man7/` -- per-API reference.  Coverage is partial; see `docs/MAN_TODO.md` for the gap list.
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
