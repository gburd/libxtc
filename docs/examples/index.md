---
title: Examples
nav_order: 6
has_children: true
permalink: /examples/
lede: >-
  Complete, buildable programs -- each explained, with the software that inspired it, how it differs, and what libxtc made easy or hard.
---

1. TOC
{:toc}

---

The [`examples/`](https://codeberg.org/gregburd/libxtc/src/branch/main/examples)
directory ships complete, buildable programs. The small ones isolate a
single idea; the large ones are real servers modelled on well-known
systems -- Redis, SQLite, Kafka, Seastar/Tina, PostgreSQL -- rebuilt on
libxtc. Each server has its own page below covering **the software that
inspired it**, how the libxtc version is **similar and different**, **how
it works**, and the **advantages and challenges** of building it on
libxtc. Every example is built and run in CI, so none of it rots.

Build them against a configured tree:

```sh
cd build && make examples      # small ones -> ./01_hello, ./02_pingpong, ...
# the larger examples build in their own directories, e.g.
cd examples/05_rexis && make XTC_BUILD=../../build
```

## The server examples

| Example | Modelled on | What it shows |
|---|---|---|
| [rexis]({{ '/examples/rexis/' | relative_url }}) | Redis / Valkey | A drop-in RESP server with hard resource budgets; connection-per-process. |
| [sqlxtc]({{ '/examples/sqlxtc/' | relative_url }}) | SQLite | A from-scratch SQL engine: parser, vectorized executor, B-link/buffer-pool/WAL storage. |
| [kaka]({{ '/examples/kaka/' | relative_url }}) | Apache Kafka / Redpanda | A partitioned append-only log broker with credit-based backpressure. |
| [tnt]({{ '/examples/tnt/' | relative_url }}) | Seastar / Tina | The stackless Isolate layer -- the deliberate alternative to fibers. |
| [pgmock]({{ '/examples/pgmock/' | relative_url }}) | PostgreSQL | A mock PG backend proving the no-fork runtime seam, with zero PG source. |

## The teaching programs (01--04)

These four are small and map directly onto the [Guide]({{ '/guide/' | relative_url }})
chapters; read them side by side.

### 01_hello_async -- the core contract

A ~30-line program that spawns one coroutine, yields once, and awaits
the result. It exists to prove the async/await contract end to end and
to be the first thing a newcomer reads. It is the basis of
[Getting started]({{ '/guide/01-getting-started/' | relative_url }}).
Source:
[`examples/01_hello_async.c`](https://codeberg.org/gregburd/libxtc/src/branch/main/examples/01_hello_async.c).

### 02_proc_pingpong -- message passing

Two processes bounce a counter back and forth. It shows that the
`xtc_send` / `xtc_recv` surface is enough to build request/reply RPC,
and it establishes the idiom of encoding the reply-to pid in the payload
(libxtc deliberately does not attach an implicit sender --
[why]({{ '/guide/03-processes-and-messages/' | relative_url }})). Source:
[`examples/02_proc_pingpong.c`](https://codeberg.org/gregburd/libxtc/src/branch/main/examples/02_proc_pingpong.c).

### 03_supervised_app -- an OTP application

An `xtc_app` with a root supervisor (`one_for_all`), two workers, and an
external watcher that can request an orderly shutdown. It demonstrates
that the supervisor stack composes: the app owns the loop, registry, and
root supervisor; the supervisor owns children with a restart policy.
This is the skeleton of a real service. Source:
[`examples/03_supervised_app.c`](https://codeberg.org/gregburd/libxtc/src/branch/main/examples/03_supervised_app.c).

### 04_lockmgr_demo -- deadlock detection

Two transactions deadlock on a heavyweight lock manager; the detector
finds the circular wait and aborts the younger transaction. It is the
counterpoint to the process model: when you *do* want shared,
lock-ordered state, [the lock manager]({{ '/reference/locks/' | relative_url }})
provides it -- with deadlock detection, which hand-rolled
`pthread_mutex` ordering does not. Source:
[`examples/04_lockmgr_demo.c`](https://codeberg.org/gregburd/libxtc/src/branch/main/examples/04_lockmgr_demo.c).

## Reading suggestion

Read **01--04** alongside the [Guide]({{ '/guide/' | relative_url }}) --
each maps to a chapter. Then read [rexis]({{ '/examples/rexis/' | relative_url }})
for a complete, approachable server, and
[sqlxtc]({{ '/examples/sqlxtc/' | relative_url }}) when you want to see
libxtc under real storage-engine pressure.
