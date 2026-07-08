---
title: Examples
nav_order: 4
has_children: false
permalink: /examples/
lede: >-
  Complete, buildable programs -- each explained, with its design decisions and trade-offs.
---
{: .no_toc }

1. TOC
{:toc}

---

The [`examples/`](https://codeberg.org/gregburd/libxtc/src/branch/main/examples)
directory ships complete, buildable programs. The small ones isolate a
single idea; the large ones are real servers that use libxtc the way a
production system would. This section explains what each is, the design
decisions behind it, and the trade-offs it accepts. They are built and
run in CI, so they do not rot.

Build them against a configured tree:

```sh
cd build && make examples      # small ones -> ./01_hello, ./02_pingpong, ...
# the larger examples build in their own directories, e.g.
cd examples/05_rexis && make XTC_BUILD=../../build
```

## The teaching programs (01--04)

### 01_hello_async -- the core contract
A ~30-line program that spawns one coroutine, yields once, and awaits
the result. It exists to prove the async/await contract end to end and
to be the first thing a newcomer reads. It is the basis of
[Getting started]({{ '/guide/01-getting-started/' | relative_url }}).

### 02_proc_pingpong -- message passing
Two processes bounce a counter back and forth. It shows that the
`xtc_send` / `xtc_recv` surface is enough to build request/reply RPC,
and it establishes the idiom of encoding the reply-to pid in the
payload (libxtc deliberately does not attach an implicit sender --
[why]({{ '/guide/03-processes-and-messages/' | relative_url }})).

### 03_supervised_app -- an OTP application
An `xtc_app` with a root supervisor (`one_for_all`), two workers, and an
external watcher that can request an orderly shutdown. It demonstrates
that the supervisor stack composes: the app owns the loop, registry, and
root supervisor; the supervisor owns children with a restart policy.
This is the skeleton of a real service.

### 04_lockmgr_demo -- deadlock detection
Two transactions deadlock on a heavyweight lock manager; the detector
finds the circular wait and aborts the younger transaction. It is the
counterpoint to the process model: when you *do* want shared,
lock-ordered state, [the lock manager]({{ '/reference/locks/' | relative_url }}) provides it -- with
deadlock detection, which hand-rolled `pthread_mutex` ordering does not.

## The servers (05--09)

### 05_rexis -- a Redis-compatible server
{: .d-inline-block }

Flagship
{: .label .label-green }

A drop-in server for `redis-cli` with **hard resource budgets**. One
process per connection; a bitcask-style persistent store; metrics. It
demonstrates that a real network server -- protocol parsing,
persistence, back-pressure, observability -- is natural in the process
model, and that libxtc's resource caps
([`xtc_res(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_res.3))
can bound memory under a flood rather than OOM.

- **Design decision:** connection-per-process, not a thread pool. Each
  client's state is private to its process; no shared connection table,
  no locks.
- **Trade-off:** a process per connection costs some memory per client;
  in return there is no shared mutable connection state to protect.

### 06_sqlxtc -- a from-scratch SQL engine
{: .d-inline-block }

Flagship
{: .label .label-green }

A complete SQL engine built on libxtc: a Lime-generated parser, a
vectorized executor, and B-link-tree / buffer-pool / write-ahead-log
storage, served over a small JSON protocol to many concurrent clients.
It is the most demanding use of the library and drove much of its
hardening (the buffer-manager pin races, the cross-thread wakeup fixes).

- **Design decision:** the storage engine uses libxtc's own left-right
  locks, RCU, and lock manager for page concurrency; I/O goes through
  `xtc_aio` so a slow disk parks a fiber instead of a thread.
- **Trade-off:** building storage from scratch (rather than embedding
  SQLite) is more code, but it lets every layer be fiber-aware and
  deterministically testable -- which an off-the-shelf engine with its
  own threading is not.

### 07_kaka -- a Kafka-shaped log broker
Partitioned, append-only logs with credit-based back-pressure. It shows
the log-broker shape -- producers, partitions, consumer offsets -- as a
set of cooperating processes, with a coordinator process owning the
committed offsets lock-free (single-owner, so no lock at all).

### 08_tnt -- the stackless Isolate layer
The canonical echo server for libxtc's Tina-faithful **Isolate** layer
(a supported API: [`xtc_tnt(3)`](https://codeberg.org/gregburd/libxtc/src/branch/main/man/man3/xtc_tnt.3)).
Thread-per-core, shared-nothing, *stackless* state machines that return
transitions rather than owning a fiber stack.

- **Design decision:** this is the explicit-state-machine alternative to
  fibers, offered deliberately for the workloads that want it -- a huge
  number of tiny, uniform state machines where even a kilobyte-scale
  fiber stack each is too much. It maps a *shard* to one long-lived
  process per core, not an isolate to a process.
- **Trade-off:** stackless code is less linear than fiber code (you
  write transitions, not straight-line logic) in exchange for a much
  smaller per-entity footprint. It is the counter-example the guide's
  [not-chosen callouts]({{ '/guide/01-getting-started/' | relative_url }}) point at: fibers are
  the default, but the state-machine style is a first-class option here.

### 09_pgmock -- a mock PostgreSQL backend
A postmaster process accepts connections and spawns one backend process
per client, each speaking a minimal PostgreSQL v3 wire handshake and
`SELECT 1` -- with **zero PostgreSQL source**. It proves the runtime
seam for the future PG adapter: no-fork connection multiplexing, and
`WaitLatchOrSocket` mapped onto `xtc_proc_wait_fd`. See
[the PG adapter design]({{ '/reference/pg-adapter/' | relative_url }}).

## Reading suggestion

Read **01--04** alongside the [Guide]({{ '/guide/' | relative_url }}) -- each maps to a chapter.
Then read **05_rexis** for a complete, approachable server, and
**06_sqlxtc** when you want to see libxtc under real storage-engine
pressure.
