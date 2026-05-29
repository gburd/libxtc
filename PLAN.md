# XTC -- Design Plan (Revision 4)

**Status:** Draft for review.  Nothing in here is implemented yet.
Revision 4 adds the full lock subsystem ((S)13) -- LRLock from
`~/ws/postgres/lrlck`, LWLock from the same, BDB-style lock manager
+ deadlock detector with all victim policies from `~/ws/libdb`,
incremental waits-for graph from `~/ws/noxu`, BDB nine-mode lock
lattice with intent locks and a promotion ladder, resource caps,
tiered testing/benchmarking/scalability/exhaustion plan -- plus a
worked SQL-query traversal end-to-end through the runtime ((S)14) so
you can see every primitive at work in one place.

Revision 3 incorporated the PostgreSQL multithreading working-group
roadmap (v20 foundation, v21 cutover) -- every item on that list has a
first-class xtc primitive -- plus: rename `ev` -> `evt`, `xtc_ans` ->
`xtc_svr`, the `dist/s_async` prototype-generation tool, explicit
`xtc_yield()` for opt-in cooperative scheduling, RCU and LRLock
additions, a function-call GUC-shaped config API, per-loop logging,
and libc thread-safety wrappers (uselocale, strerror_r, getopt_long,
dlerror, getenv/setenv).  See (S)14 for the explicit mapping from the
PG threading workplan to xtc primitives.

Revision 2 introduced: layer renames, the corrected execution model
(shared-nothing threaded async event loop, *not* "backends as threads"),
the broadened platform matrix matching PostgreSQL 18/19, configure-time-
only backend selection, libumem-inspired memory ownership, coroutine
support via Duff's device, and the answer to the `async()/await()`
syntax question (see (S)12).

`xtc` is a C library that provides Tokio-style asynchronous concurrency
for C, drawing from Seastar (per-CPU shared-nothing reactors) and from
the Erlang BEAM / OTP (lightweight processes, mailboxes, supervision,
selective receive).

The execution model is **a fixed pool of shared-nothing OS threads
(default: one per CPU, configurable up or down) that run an async event
loop**.  Database requests, timers, periodic tasks, and I/O completions
are all *async events* multiplexed onto this thread pool.  Forked
processes exist only for backwards compatibility with PostgreSQL
features that demand process isolation; they host exactly one event
loop apiece.

The ultimate consumer is a threaded PostgreSQL.

---

## 0. Guiding Principles

1. **Layered, with sharp interfaces.**  Each layer can be unit-tested
   in isolation and replaced (e.g. swap io_uring for kqueue, swap the
   work-stealing deque for a lock-free MPMC queue) without rewriting
   anything above it.
2. **Shared-nothing by default, shared-cautiously where required.**
   One event loop per OS thread (Seastar shape).  Each task is owned
   by exactly one event loop.  Cross-loop communication is via
   explicit channels, mailboxes, or shared-buffer handles -- never
   raw shared mutable state.
3. **High speed, low overhead, predictable p99.**  Bounded queues,
   fixed allocation pools per loop, hierarchical timer wheels, no GC,
   no implicit locks on the steady-state path, NUMA-aware steal order.
   The p99 target is the *primary* benchmark; we will reject
   throughput wins that worsen tail latency.
4. **The C programmer's API is synchronous-looking, preemption-tolerant,
   cooperatively *capable*.**  Tasks run preemptively under the event
   loop's scheduler (the loop is free to switch tasks at any await
   point and, when fibers are in use, between fiber switches).  Code
   that wants strict cooperative semantics -- no scheduler interleaving
   between explicit yields -- uses `xtc_yield()` and the
   `XTC_COOP_REGION { ... }` block.  We provide `dispatch + wait`,
   `send + receive`, `future/promise` continuations, **and** a
   `async()`/`await()` macro pair built on stackful fibers (default) or
   Duff's-device coroutines (constrained environments).  See (S)12.
5. **No hidden state in hot paths.**  Errors are returned explicitly
   (BDB/DBSQL convention: `int` return, out-params for results).
   `errno` and Win32 `GetLastError` are wrapped at L0 boundaries; the
   stable `xtc_err` enum is the only error currency above L0.
6. **Conventions match BDB/DBSQL.**  Internal symbols are `__xtc_*`
   or `__os_*`; public symbols are `xtc_*`.  Sources live under
   `src/` organised by subsystem; build automation lives under
   `dist/`; out-of-source builds are enforced.
7. **Configure-time selection of backends, never runtime.**  Whether
   we use io_uring vs epoll vs kqueue vs IOCP vs poll, whether we
   use threads vs protothread-only, whether we have NUMA support
   -- all decided at `./configure` / `meson setup` time, baked into
   the binary as `XTC_HAVE_*` macros.  No vtables on the hot path.
   No surprise behaviour shifts between hosts.
8. **Graceful degradation.**  On the most constrained platform we
   support, xtc must still work -- single OS thread, `poll(2)` only,
   green threads via Duff's-device protothreads (the model from
   `~/ws/pt`).  All higher-layer APIs (mailboxes, supervisors,
   `dispatch`/`reply`) keep working; only the parallelism vanishes.
9. **C dialect = C11**, the floor PG18 raised and PG19 inherits.
   No GNU extensions in public headers; in `.c` files extensions
   are allowed when guarded by feature macros.

---

## 1. Why Tokio + Seastar + BEAM

| Aspect | Tokio (Rust) | Seastar (C++) | BEAM / OTP (Erlang) |
|---|---|---|---|
| Execution model | M:N work-stealing scheduler | Per-core shared-nothing reactor | Per-scheduler queues + work-stealing |
| I/O | `mio` epoll/kqueue/IOCP/io_uring | epoll, io_uring, AIO, DPDK | epoll/kqueue/IOCP, drivers |
| Unit of work | `Future` (poll, state machine) | `future<T>` + continuation | Process (PID, isolated heap, mailbox) |
| Inter-task comms | mpsc/oneshot/broadcast/watch | queue, semaphore, foreign-shard submit | `!`/selective `receive`, gen_server |
| Cancellation | `Drop` of the future | `abort_source`, `gate` | Linked exits, monitors |
| Faults | `Result<T,E>`; panic = death | C++ exceptions in continuations | Supervision, "let it crash" |
| **What we steal** | Future state machines + waker model + channel taxonomy | **Per-CPU reactor + foreign-shard submit + `gate`/`abort_source`** | **Mailbox + selective receive + monitors/links + supervisor** |

Concretely:

- **Scheduler shape**: Seastar (per-thread reactor, owned tasks),
  with a Tokio-style work-stealing escape valve when a reactor is
  idle and has no local work.  Stealing is opt-in and **off** in
  the strict-Seastar build (`--disable-work-stealing`).
- **Task abstraction**: closest to a BEAM process -- identity (PID),
  mailbox, links, monitors, optional supervisor.  Internally
  scheduled cooperatively as a Tokio-like state machine.
- **Channel taxonomy**: Tokio's -- `oneshot`, `mpsc`, `mpmc`,
  `broadcast`, `watch`.
- **wterl `async_nif.h`** is the closest existing C art for the
  dispatch ergonomics: a request goes in, a worker picks it up, a
  reply comes back.  We modernize the queue with a Chase-Lev / ST3
  work-stealing deque.

---

## 2. Layering

Six layers; lower layers know nothing of upper layers.  Each layer has
its own subdirectory, its own internal header, and its own munit +
hegel test binary.

```
+---------------------------------------------------------------------+
| L5  pg/   PostgreSQL adapter (subsumes src/backend/storage/aio,     |
|           latch, signal/CFI, MemoryContext, GUC bridge)             |
|----------------------------------------------------------------------|
| L4  orc/  "Orchestration": supervisors, xtc_svr (gen_server),       |
|           xtc_fsm (gen_statem), xtc_app, xtc_reg                    |
|----------------------------------------------------------------------|
| L3  ptc/  "Processes / Threads / Channels": PIDs, mailboxes (with   |
|           selective receive), links, monitors, channels, futures,   |
|           sync primitives (incl. RCU, LRLock), dispatch()/reply(),  |
|           async()/await(), xtc_yield(), xtc_log, xtc_cfg            |
|----------------------------------------------------------------------|
| L2  evt/  Event loop: per-thread reactor, run queues, work-         |
|           stealing deque, task lifecycle, wakers, timer wheel,      |
|           coroutine substrate (fiber + Duff's-device)               |
|----------------------------------------------------------------------|
| L1  io/   Pollable I/O abstraction: epoll/kqueue/IOCP/io_uring/     |
|           poll wrapper, async file/socket/timer registration        |
|----------------------------------------------------------------------|
| L0  os/   __os_*: threads, processes, shm, mmap, mutex, atomics,    |
|           time, file ops, signals, errno->xtc_err mapping, allocator |
|           hook, TLS, CPU/NUMA topology, dynamic loading, RNG        |
\----------------------------------------------------------------------+
```

Renames vs Revision 1: `rt/` -> `evt/`, `proc/` -> `ptc/`, `otp/` -> `orc/`.
Revision 3 renames: `xtc_ans` -> `xtc_svr` (generic server).

### 2.1 L0 -- `os/` (the `__os_*` layer)

Pure portability shim, modeled on BDB/DBSQL's `src/os/` and `os_ext.h`.
Functional groups, one `os_*.c` file per:

| File | Surface |
|---|---|
| `os_alloc.c` | `__os_malloc`, `__os_calloc`, `__os_realloc`, `__os_free`, `__os_strdup`, `__os_aligned_alloc` |
| `os_atomic.c` | `__os_atomic_load/store/cas/fetch_add/fence`, `__os_pause`, `__os_yield_cpu` |
| `os_thread.c` | `__os_thread_create/join/detach/yield/self`, `__os_setname` |
| `os_tls.c` | `__os_tls_create/get/set/destroy` |
| `os_mutex.c` | `__os_mutex_*`, `__os_rwlock_*`, `__os_cond_*`, `__os_sem_*`, futex/`WaitOnAddress` fast paths |
| `os_proc.c` | `__os_fork`, `__os_spawn`, `__os_waitpid`, `__os_pidfd` |
| `os_shm.c` | `__os_shm_open/unlink/map/unmap`, `__os_mmap`, `__os_madvise` |
| `os_dyn.c` | `__os_dlopen/dlsym/dlclose` |
| `os_time.c` | `__os_clock_mono`, `__os_clock_real`, `__os_jtime`, `__os_sleep`, `__os_sleep_ns` |
| `os_file.c` | `__os_open/close/read/write/pread/pwrite/fsync/falloc/unlink/rename/stat` (synchronous; async wrapper in L1) |
| `os_dir.c` | `__os_mkdir/readdir/rmdir` |
| `os_net.c` | `__os_socket/bind/listen/accept/connect/send/recv/setsockopt` |
| `os_signal.c` | `__os_sig_block/unblock/wait/handler`, `__os_signalfd`, `__os_kqueue_signal` |
| `os_cpu.c` | `__os_ncpus`, `__os_cpu_affinity_set/get`, `__os_numa_node`, `__os_numa_distance`, `__os_cacheline_size` |
| `os_rand.c` | `__os_rand_bytes` |
| `os_errno.c` | `__os_get_errno`, `__os_set_errno`, `__os_strerror_r`, `__os_err_to_xtc`, Win32 `GetLastError` translation |
| `os_globals.c` | Cross-platform wrapper for shared globals and function-local statics -- see (S)4.3 |
| `os_locale.c` | `__os_uselocale`, `__os_newlocale`, `__os_freelocale` -- replaces the bare `setlocale()` PG must escape from |
| `os_env.c` | `__os_getenv`, `__os_setenv` -- guarded; the only place a thread can read the environment safely |
| `os_dl.c` | `__os_dlerror_r` -- thread-safe `dlerror()` replacement |
| `os_getopt.c` | `__os_getopt_long` -- thread-safe `getopt_long` replacement |

Every `__os_*` returns `int` (0 on success, negative `XTC_E_*` on
failure), out-params for results.  The public-internal header is
`src/inc/os_ext.h`, generated by `dist/s_include` from `PUBLIC:`
markers -- exactly the DBSQL pattern.

**libc coverage:** glibc, musl, MSVC's UCRT, Solaris/illumos libc,
AIX libc, *BSD libc, macOS libc, MinGW.  Every `os_*.c` has a
preprocessor-gated implementation per libc family; the `dist/aclocal`
probes pick which to compile.

**Thread-safety wrappers.**  The four most painful libc surfaces
for a threaded server (already on the PG threading workplan) live
in L0 by design:

- `setlocale()` is global; we expose only `__os_uselocale()` /
  `__os_newlocale()` and **forbid** `setlocale` outside
  `os_locale.c` (lint).  Matches Jeff Davis's `uselocale` plan.
- `strerror()` is global; we expose only `__os_strerror_r()`.
  Matches Nathan's `strerror_r` plan.
- `getopt_long()` keeps global state; `__os_getopt_long()` is a
  re-entrant replacement.
- `dlerror()` keeps global state; `__os_dlerror_r()` reads it under
  a coordinator lock and returns a copy into a caller-provided
  buffer.
- `getenv` / `setenv` are global and racy; reads are allowed via
  `__os_getenv()` (returns a copy under a one-shot snapshot lock),
  writes are forbidden after `xtc_app_start()` returns (lint +
  runtime assert).

A `dist/s_signals` lint verifies no source file outside
`os_signal.c` calls `signal()`, `kill()`, `sigaction()`, etc.

### 2.2 L1 -- `io/` (the polling/notification engine, "mio")

Pluggable I/O backends behind a single API.  **Configure-time
selection only** -- exactly one backend is compiled in for a given
binary (or, more precisely, the highest-priority backend probed
positive at configure time becomes `XTC_IO_DEFAULT`, and the
second-highest is compiled as a `XTC_IO_FALLBACK` for runtime
*environment* checks like "io_uring disabled by seccomp" -- but this
is still *no runtime backend selection by user code*).

| File | Backend | Notes |
|---|---|---|
| `io_uring.c` | Linux io_uring | Preferred when liburing present and kernel >= 5.19; subsumes PG's `method_io_uring.c` |
| `io_epoll.c` | Linux epoll + eventfd | Default Linux fallback |
| `io_kqueue.c` | *BSD, macOS, DragonFlyBSD | Native timers + signal events |
| `io_iocp.c` | Windows IOCP | Completion-based (different shape; see vtable) |
| `io_solaris.c` | Solaris/illumos `port_create` | Event ports |
| `io_aix.c` | AIX `pollset` | |
| `io_poll.c` | POSIX `poll(2)` | Last-resort fallback, present always |
| `io_select.c` | POSIX `select(2)` | Older systems where `poll(2)` is broken (rare) |

Vtable (compile-time-resolved; no indirect call on hot paths because
exactly one is compiled in):

```c
struct xtc_io_backend {
    int  (*init)(xtc_io *io, const xtc_io_cfg *cfg);
    void (*fini)(xtc_io *io);
    int  (*reg_fd)(xtc_io *io, int fd, uint32_t interest, void *tag);
    int  (*mod_fd)(xtc_io *io, int fd, uint32_t interest, void *tag);
    int  (*del_fd)(xtc_io *io, int fd);
    int  (*submit)(xtc_io *io, const xtc_io_op *op);     /* completion-style */
    int  (*poll)(xtc_io *io, xtc_io_event *out, int max, int timeout_ns);
    int  (*wakeup)(xtc_io *io);                          /* cross-thread kick */
};
```

The default and fallback are picked by the L1 dispatch macros at
preprocess time so the compiler inlines the chosen backend's
functions directly.  No runtime branch on a function pointer for
`reg_fd`/`poll`/etc. on the steady-state path.

### 2.3 L2 -- `evt/` (event loop / scheduler)

Heart of the runtime.  One **event loop** per OS thread; loops are
collected into an **executor**.

Key types:

- `xtc_task` -- a state-machine continuation.  Function pointer +
  state word + small inline context + waker.  Modeled on Tokio's
  `Harness`.
- `xtc_waker` -- `(loop*, task_id)` capable of being signaled from
  any thread; signaling re-queues the task on its owning loop.
- `xtc_loop` -- owns: an `xtc_io`, a local LIFO slot, a local FIFO
  run queue, a Chase-Lev / ST3 work-stealing **deque**, a
  hierarchical timer wheel, a remote-submission MPSC inbox.
- `xtc_exec` -- owns N loops, the global injector queue, the
  park/unpark coordinator, the shutdown gate.

Scheduling rules (Tokio / Go inspired):

1. Next-task order: LIFO slot -> local FIFO -> global injector
   (every 61st tick) -> steal from another loop's deque (NUMA-near
   first) -> block on `xtc_io.poll()`.
2. **Hierarchical timer wheel** (6 levels x 64 slots, Linux-style)
   drives all timeouts.  No per-task heap; O(1) insert and tick.
3. A task that yields to I/O parks itself by registering its waker
   with the I/O backend; the backend's `poll()` returns ready tags
   and the loop re-queues the corresponding tasks.
4. Cross-loop wake = MPSC push to target loop's inbox + a `wakeup()`
   call to its I/O backend (eventfd / NT keyed event /
   `IORING_OP_MSG_RING`).
5. **Loops are pinned to cores** (Seastar default), with NUMA-aware
   steal ordering (only steal from same-NUMA-node loops first).

Tasks are reference-counted with bit-stealing on the state word
(running/scheduled/complete/cancelled/joinable/notified) -- Tokio's
trick.

#### 2.3.1 Coroutine substrate

L2 also provides three coroutine flavours, each available
independently at configure time:

1. **`evt_fiber.c`** -- stackful fibers via `boost.context`-style asm
   (we ship the small per-arch `make_fcontext`/`jump_fcontext`
   under `src/os/asm/`).  Used for `async()`/`await()` macros.
   Stack size configurable; default 64 KiB with guard page.
2. **`evt_pt.c`** -- Duff's-device protothreads, vendored from
   `~/ws/pt`.  Used on platforms with no fiber asm support and
   for the strict-degraded build.  Locals must be lifted into the
   task struct (the protothread tax).
3. **`evt_ucontext.c`** -- fallback using POSIX `ucontext.h` for
   any architecture our fiber asm doesn't yet cover.  Slow, but
   universal on Unix.

Configure decides which is `XTC_COROUTINE_DEFAULT`.  See (S)12 for
the precise relationship between coroutines and `async()/await()`.

#### 2.3.2 Yielding (cooperative opt-in)

The scheduler is free to interrupt a task at any await point.  Code
that wants strict cooperative semantics -- "do not let any other task
run in this region" -- uses one of:

```c
/* Single yield point -- explicitly invite a reschedule */
xtc_yield();

/* Fenced region -- no preemption between Begin and End even if the
   scheduler would otherwise pick a higher-priority task.  Cheap:
   sets a per-task flag the loop checks at task-boundary points. */
XTC_COOP_REGION {
    /* ... long compute, no await calls allowed by the lint ... */
}
```

`xtc_yield()` is the BEAM `erlang:yield()` analogue: a manual
reduction-counter trip to give other tasks a turn.  It is *not* the
default model -- most code is event-driven and yields naturally at
`await`.  But for tight CPU-bound loops inside a task that should
still cooperate, it is the right primitive.

### 2.4 L3 -- `ptc/` (Processes / Threads / Channels)

Where the C programmer lives.  Three intertwined abstractions:

#### 2.4.1 Futures and promises

```c
xtc_future_t  *fut;
xtc_promise_t *prom;
xtc_future_new_pair(&prom, &fut);
xtc_promise_set(prom, value, status);
int rc = xtc_future_await(fut, &out);    /* in a task: cooperative */
int rc = xtc_future_wait (fut, &out, timeout_ns); /* on main thread */
```

Combinators: `xtc_future_then`, `xtc_future_map`, `xtc_future_when_all`,
`xtc_future_when_any`, `xtc_future_with_timeout`.

#### 2.4.2 Processes (BEAM-style)

```c
xtc_pid_t pid;
xtc_proc_spawn(&pid, my_entry, arg, &opts);  /* opts: link, monitor, mailbox cap */
xtc_send(pid, msg, len);                     /* async */
xtc_recv(&msg, &len, timeout_ns);            /* selective via match fn */
xtc_link(pid);
xtc_monitor_t m; xtc_monitor(pid, &m);
xtc_exit(pid, reason);
```

A process here is a *task with identity*: PID, mailbox (unbounded
MPSC of opaque envelopes), optional name in `xtc_reg`.  Tasks that
don't need a mailbox use the future API at zero overhead.

**Selective receive** is the BEAM-distinguishing semantics: receiver
supplies a match function; non-matching envelopes are skipped (kept
in arrival order in a save queue) until a match arrives or the
timeout fires.  Implemented with the BEAM's save-queue optimization
so successive receives don't re-scan from the head.

#### 2.4.3 Channels (Tokio taxonomy)

| Channel | Senders | Receivers | Buffering |
|---|---|---|---|
| `xtc_oneshot` | 1 | 1 | 1 slot |
| `xtc_mpsc` | many | 1 | bounded or unbounded |
| `xtc_mpmc` | many | many | bounded |
| `xtc_broadcast` | many | many (each gets every msg) | ring, lossy on slow consumer |
| `xtc_watch` | many | many (each only sees latest) | 1 slot |

#### 2.4.4 Synchronization

- `xtc_mutex`, `xtc_rwlock` -- async, fair, parking.
- `xtc_semaphore` -- counting; backpressure currency.
- `xtc_notify` -- Tokio `Notify` (one-shot wake of any waiter).
- `xtc_barrier` -- N-task rendezvous.
- `xtc_gate` -- Seastar `gate`: tracks outstanding ops for drain.
- `xtc_abort_source` / `xtc_abort_token` -- Seastar structured
  cancellation; replaces ad-hoc "stop" booleans.
- `xtc_rcu` -- read-copy-update for read-mostly graph data; matches
  the PG `pg_rcu.h` proposal (relcache/syscache invalidation as the
  first user).  Epoch-based reclamation; readers are wait-free.
- `xtc_lrlock` -- left-right lock per Greg Burd's RFC: wait-free
  reads via two-copy publish/swap.  Designed for ProcArray-style
  snapshot, replication-slot xmin array, buffer-mapping hash table.
  Writer side uses an inner `xtc_mutex` (per the RFC's revision
  to use LWLock-style instead of spinlock); oplog is pre-allocated
  at construction; drain-wait uses `xtc_notify` with proper wait
  events.
- `xtc_lwlock` -- partition-friendly counting lock, the LWLock
  analogue.  Tranches and partition arrays are first-class.
  Re-benchmarked specifically under threaded-mode contention
  before promoting any PG subsystem onto it.

#### 2.4.5 Logging (`xtc_log`)

A per-loop ring-buffered logger that drains to a single writer
task.  No global lock on the hot path; no torn lines under high
concurrency.  Per-task `errcontext` chain (matches the PG
requirement that `errcontext` go per-thread).  Pluggable sinks:
stderr, file, syslog, journald, Windows event log, PG `elog`
bridge.

#### 2.4.6 Configuration (`xtc_cfg` -- function-call GUC API)

A function-call config API designed for the threaded-PG
"GUC table = address of a global" replacement:

```c
bool        b   = xtc_cfg_get_bool   ("work_mem_warn_on_full");
int64_t     n   = xtc_cfg_get_int64  ("work_mem");
double      d   = xtc_cfg_get_double ("random_page_cost");
const char *s   = xtc_cfg_get_string ("timezone");
int         e   = xtc_cfg_get_enum   ("track_activity", &enum_meta);

xtc_cfg_set_int64("work_mem", new_value, XTC_CFG_SOURCE_SESSION);
```

Type-dispatched backing store (matching the PG plan):

- **Booleans** -> `sparsemap` (we already have one in `~/ws/sparsemap`).
- **Integers / enums** -> indexed array keyed by registered ID.
- **Strings** -> hash table; values owned by per-loop arena.
- **Doubles** -> indexed array, atomic 64-bit load/store.

Hot-path readers (`xtc_cfg_get_int64("work_mem")` in an inner
planner loop) compile to a single load via a per-call-site cached
ID -- see (S)6.4 for the prototype-generation tool that emits the IDs.

Sources are typed (`SOURCE_DEFAULT`, `SOURCE_FILE`, `SOURCE_SESSION`,
`SOURCE_OVERRIDE`) so PG's GUC source-precedence semantics survive
the migration.

#### 2.4.7 Dispatch / reply (lowercase, the new spelling)

For C callers that don't want to think about futures, the wterl-style
sugar:

```c
dispatch(my_op, args, {
    /* worker body -- runs on whichever loop picks this up */
    int r = do_work(args);
    reply(0, r);
});
/* caller side: */
int rc = xtc_call(my_op, &args, &result, timeout_ns);
```

Under the hood `dispatch()` declares a one-shot process whose
mailbox is replaced by a `xtc_oneshot`; `xtc_call` is
`xtc_future_wait`.  See (S)12 for how `dispatch()` and
`async()`/`await()` relate.

### 2.5 L4 -- `orc/` (orchestration)

OTP's actually-novel contribution -- supervised process trees with
declared restart strategies.

| Symbol | Erlang/OTP analogue | Purpose |
|---|---|---|
| `xtc_supervisor` | `supervisor` | Children + strategy + restart intensity |
| `xtc_svr` | `gen_server` ("generic **s**er**v**e**r**") | Callback module pattern for stateful services |
| `xtc_fsm` | `gen_statem` ("finite **s**tate **m**achine") | State-machine variant |
| `xtc_app`  | `application` | Root supervisor + lifecycle hooks |
| `xtc_reg`  | process registry | Name -> PID lookup |

Strategies: `one_for_one`, `one_for_all`, `rest_for_one`,
`simple_one_for_one`.  Restart intensity = max-restarts-per-window
before the supervisor itself exits up the tree.

`xtc_svr` is a struct of function pointers (`init`, `handle_call`,
`handle_cast`, `handle_info`, `terminate`) plus opaque state.
`xtc_fsm` adds state IDs and per-state handler tables.

`xtc_reg` is a per-executor concurrent hash table with epoch-based
reclamation, so steady-state `whereis` is lock-free.

### 2.6 L5 -- `pg/` (PostgreSQL adapter)

The end-game.  This layer **subsumes** several PG subsystems:

| PG subsystem | xtc replacement |
|---|---|
| `src/backend/storage/aio/` (whole tree) | `xtc_io` + `xtc_future` per-IO; `pgaio_io_acquire` -> `xtc_io_acquire`; `PgAioWaitRef` -> `xtc_future_t *`; `method_io_uring.c`, `method_worker.c`, `method_sync.c` -> already covered by L1 |
| `src/backend/storage/ipc/latch.c` | `xtc_notify` |
| `CHECK_FOR_INTERRUPTS()` | abort-token check at every `await` point |
| `MemoryContext` / palloc | `xtc_alloc` ownership domains (see (S)5) |
| `src/backend/storage/ipc/shm_mq.c` | `xtc_chan_*` over shared-buffer handles |
| `src/backend/storage/ipc/procsignal.c` | `xtc_signal` -> mailbox messages |
| `src/backend/postmaster/`'s ad-hoc fork-and-exec | `xtc_proc_spawn` (threaded) + opt-in `xtc_proc_fork_spawn` |
| Startup / GUC tunables | Bridge to `xtc_cfg_*` |

Forked-backend compatibility: a forked process hosts exactly one
event loop.  Cross-process messaging reuses PG's existing
shared-memory channels under the same `xtc_send` API; the dispatch
implementation is selected at process boundary.

L5 is the *only* PG-specific code; L0-L4 are general-purpose.

---

## 3. Platform matrix (matches PG18/19 supported-platforms)

xtc must build and pass its full test suite on every platform PG
supports, with identical semantics, **gracefully degrading** to a
single-thread, `poll(2)`-based, protothread-coroutine build on the
most constrained host.

### 3.1 Operating systems (from PG installation.sgml)

- **Linux** (glibc + musl + uclibc): Tier 1
- **Windows** (MSVC UCRT, MinGW-w64): Tier 1
- **FreeBSD, OpenBSD, NetBSD, DragonFlyBSD**: Tier 1
- **macOS** (recent Xcode): Tier 1
- **Solaris, illumos**: Tier 1
- **AIX**: Tier 2 (build, smoke tests; soak optional)

### 3.2 CPU architectures

x86-64, x86, ARM64, ARM (32), PowerPC (BE/LE, 32/64),
S/390x, SPARC (v9), MIPS (BE/LE), RISC-V 64.  Big-endian and
little-endian, 32-bit and 64-bit variants.

The fiber assembly (`src/os/asm/`) ships hand-written
`make_fcontext`/`jump_fcontext` for: x86-64-sysv, x86-64-ms,
x86-sysv, x86-ms, aarch64, arm-eabi, ppc64le, ppc64-elfv1,
s390x, sparc64, mips64, riscv64.  Architectures we don't have
asm for fall through to `os_ucontext.c`; if even that's missing,
the build automatically uses the protothread coroutine model.

### 3.3 Compilers

- gcc >= 11
- clang >= 13
- MSVC >= 2019 (cl.exe + clang-cl)
- IBM XLC / OpenXLC (AIX)
- Sun/Oracle Studio cc (Solaris)
- Intel ICC (best-effort)

Every C11 atomic, alignment, and threading feature has a fallback
keyed by `__STDC_NO_ATOMICS__` / `_MSC_VER` / etc.

### 3.4 libc families

glibc, musl, UCRT (MSVC), MinGW UCRT, Solaris/illumos libc, AIX
libc, FreeBSD libc, NetBSD libc, OpenBSD libc, DragonFlyBSD libc,
macOS libc (Apple).  Each `os_*.c` is preprocessor-gated per family
where surfaces differ (`getrandom` vs `arc4random` vs
`RtlGenRandom`; `signalfd` vs `kqueue EVFILT_SIGNAL` vs
`SetConsoleCtrlHandler`; `pthread_setname_np` flavours; etc.).

### 3.5 Buildfarm parity

CI must include containers/VMs that mirror as many PG buildfarm
animals as we can practically host: the Linux glibc/musl matrix,
FreeBSD/OpenBSD/NetBSD images, an illumos image (OmniOS), Windows
MSVC, Windows MinGW, macOS GitHub runner, and an AIX image when
available.  A subset (Linux x86-64 glibc, Linux aarch64 musl,
FreeBSD amd64, Windows MSVC, macOS arm64) is the *gate* on every
PR; the rest run nightly.

### 3.6 Degradation ladder

Build-time decision tree (executed by `dist/configure` and `s_meson`):

```
1. Have C11 atomics + threads?       no -> bail with clear error
2. Have a fiber asm for this arch?   yes -> XTC_COROUTINE = fiber
                                     no  -> step 3
3. Have working ucontext.h?          yes -> XTC_COROUTINE = ucontext
                                     no  -> step 4
4. Always available                       XTC_COROUTINE = pt (Duff)
5. Have io_uring?                    yes -> XTC_IO = uring
6. Have epoll?                       yes -> XTC_IO = epoll
7. Have kqueue?                      yes -> XTC_IO = kqueue
8. Have IOCP?                        yes -> XTC_IO = iocp
9. Have Solaris event ports?         yes -> XTC_IO = solaris
10. Have AIX pollset?                yes -> XTC_IO = aix
11. Always available                       XTC_IO = poll
12. Have pthreads or Win32 threads?  yes -> XTC_THREADS = on
                                     no  -> XTC_THREADS = off
                                           (single-loop, pt-coroutines only)
```

The most-degraded build is: single OS thread, `poll(2)`,
Duff's-device protothreads.  Every public API still works; only
parallelism is gone.  This is the floor we promise.

---

## 4. Memory: ownership-aware, libumem-inspired

Memory management combines three ideas:

1. **PG-style memory contexts** -- hierarchical scopes, "free the
   whole subtree" semantics.  Already a model PG developers know.
2. **libumem slabs** -- per-CPU magazines, object caches with
   constructor/destructor callbacks, debug auditing
   (`UMEM_DEBUG=audit,contents,guards`), per-thread caches (PTC).
3. **Ownership domains** -- every allocation has an *owner*: a task,
   a process, a context, or a shared-buffer handle.  Cross-domain
   transfers are explicit (`xtc_buf_transfer(&buf, new_owner)`),
   like Rust's move but enforced at runtime by an ownership tag in
   debug builds.

### 4.1 The allocation API

```c
xtc_alloc_ctx_t *ctx = xtc_alloc_ctx_new(parent, "request-12345");
void *p  = xtc_palloc(ctx, sz);             /* PG-shape: free with ctx */
void *p2 = xtc_pcalloc(ctx, n, sz);
void *p3 = xtc_palloc_aligned(ctx, sz, 64); /* cacheline-aligned */
xtc_pfree(p);                               /* explicit early free */
xtc_alloc_ctx_reset(ctx);                   /* recycle children, keep ctx */
xtc_alloc_ctx_delete(ctx);                  /* free everything */

/* Slab-cached fixed-size objects (libumem shape) */
xtc_cache_t *c = xtc_cache_create(
    "task", sizeof(xtc_task), alignof(xtc_task),
    task_ctor, task_dtor, /*flags=*/XTC_CACHE_PERCPU);
xtc_task *t = xtc_cache_alloc(c);
xtc_cache_free(c, t);

/* Ownership transfer -- explicit, debuggable */
xtc_buf_t *b = xtc_buf_alloc(ctx_a, 4096);
xtc_buf_transfer(b, ctx_b);   /* now owned by ctx_b; ctx_a can't free */
```

### 4.2 Domains

| Domain | Owner | Lifetime |
|---|---|---|
| Per-loop arena | one `xtc_loop` (the `evt_loop`) | forever (recycled per task) |
| Per-task arena | one `xtc_task` | until task completes |
| Per-process context | one `xtc_proc` | until process exits |
| Request context | a `dispatch()` | until `reply()` |
| Shared buffer | refcounted | until last ref drops |
| Global | none | program lifetime |

PG's `MemoryContext` maps directly to `xtc_alloc_ctx_t`; PG's
`ResourceOwner` maps to per-task arena.  Inside a PG backend the
xtc allocator delegates to palloc; outside, it uses
`__os_malloc` / the slab cache.

### 4.3 Shared globals and function-local statics

This is genuinely tricky in a multi-threaded model and we will
provide a *stylized* mechanism rather than letting people scatter
`static` keywords:

- `XTC_GLOBAL(type, name)` -- declares a per-process global.
  Internally a TLS pointer to a heap-allocated cell, initialized
  on first access via a one-shot.  Visible to all loops.
  *Caller is responsible for synchronization*; we provide
  `XTC_GLOBAL_RWLOCK` and `XTC_GLOBAL_ATOMIC` variants.
- `XTC_PERLOOP(type, name)` -- per-event-loop shadow of a global.
  Identical syntax, but each loop sees its own copy.  This is the
  Seastar-style "shard local" pattern; eliminates synchronization
  for loop-local caches/counters.
- `XTC_FN_STATIC(type, name, init)` -- function-local static
  alternative.  Declares a one-shot-initialized static keyed by
  `(__FILE__, __LINE__)`.  Pure replacement for `static T name = init;`
  inside a function, but uses an explicit initialization
  primitive (so init is observably ordered with respect to the
  C11 memory model) and integrates with `xtc_app` shutdown.
- `XTC_PERLOOP_FN_STATIC(...)` -- same but per-loop.
- `XTC_TLS(type, name)` -- bare per-thread storage when neither
  per-loop nor per-process is the right scope.  Wraps
  `_Thread_local` / `__declspec(thread)` / `pthread_key_t` per
  platform.

Discipline: in xtc code itself, **bare `static` and `_Thread_local`
are forbidden outside `os/`**.  All shared/static state goes
through one of the macros above.  A `dist/s_globals` checker
(grep + AST-lite) enforces this in CI.

### 4.4 Debug instrumentation

Same flags libumem uses (`audit`, `contents`, `guards`,
`firewall`), exposed via:

```
XTC_DEBUG_MEM=audit,guards ./your_app
```

Our `os_alloc.c` thin-wraps a vendored libumem build when
`--with-libumem` is passed; otherwise uses jemalloc, mimalloc, or
plain `malloc()` based on what configure finds.  The debug flags
work the same way regardless of the underlying allocator, because
the auditing lives in `xtc_palloc` / `xtc_buf_alloc`, not the OS
allocator.

---

## 5. Cancellation, errors, faults

Three distinct mechanisms, kept distinct:

1. **Cooperative cancellation** -- `xtc_abort_source` raises a flag;
   tasks observe it at `await` points (and at every
   `XTC_CHECK_CANCEL()` macro inside dispatch handlers -- the xtc
   analogue of `CHECK_FOR_INTERRUPTS()`) and unwind cleanly with
   `XTC_E_CANCELLED`.
2. **Errors** -- every public function returns `int` (`0` or
   `XTC_E_*`).  No `errno` reliance.  Error code table in
   `inc/xtc_err.h`.  L0 `__os_errno.c` translates errno/Win32
   `GetLastError` into stable `XTC_E_*` codes; the conversion
   table is the single source of truth.
3. **Process faults** -- a process can `xtc_exit(reason)` or be
   killed by a `SIGSEGV`-class fault caught via a per-loop
   handler running on a `sigaltstack`.  Linked processes get
   `{'EXIT', pid, reason}` envelopes; monitors get
   `{'DOWN', ref, pid, reason}`.  Supervisors decide whether to
   restart.

We do **not** use C++ exceptions, longjmp-from-signal-handler, or
pthread cancellation.  We *do* use `setjmp/longjmp` inside a
single loop thread for the "let it crash" path of supervised
processes -- bounded and well-defined.

### 5.1 Signals

`os_signal.c` provides the only legal way for xtc code to interact
with signals:

- Signals are blocked on every loop thread except a single
  dedicated **signal-handler thread** (pattern PG uses already).
- That thread translates each signal into a `xtc_send` to a
  registered receiver process.  Backends thus see signals as
  ordinary mailbox messages; no async-signal-safety constraints
  apply to handlers.
- On Linux, we use `signalfd` and feed it through L1 directly so
  the signal-handler thread is unnecessary in the common case.
- On *BSD/macOS, `kqueue EVFILT_SIGNAL` does the same job.
- On Windows, `SetConsoleCtrlHandler` and async procedure calls
  drive equivalent mailbox sends.
- `SIGSEGV`/`SIGBUS`/`SIGFPE` are exceptions (handled per-loop
  on `sigaltstack`; trigger the fault path above).

This subsumes PG's `procsignal.c` cleanly.

---

## 6. Build system

Two parallel build systems, single source of truth.  Out-of-source
enforced.  Same shape as BDB/DBSQL.

### 6.1 Repository layout

```
xtc/
|--- README.md
|--- PLAN.md                           <- this document
|--- LICENSE                           <- ISC license (proposed)
|--- dist/                             <- build apparatus
|   |--- configure.ac
|   |--- aclocal/                      <- .m4 probes (io_uring, kqueue, ucontext,
|   |                                   numa, atomics, fiber-asm-arch, libc-family)
|   |--- config.guess, config.sub, install-sh, ltmain.sh
|   |--- Makefile.in
|   |--- xtc.pc.in
|   |--- meson.build.in                <- rendered by s_meson
|   |--- meson_options.txt.in
|   |--- srcfiles.in                   <- single source of truth for file list
|   |--- pubdef.in                     <- public API symbols
|   |--- platforms.in                  <- platform-config matrix
|   |--- s_all                         <- runs all generators
|   |--- s_config                      <- regen autoconf (autoreconf -i)
|   |--- s_meson                       <- regen meson.build from srcfiles.in
|   |--- s_include                     <- regen src/inc/*_ext.h
|   |--- s_async                       <- scan xtc_async() calls, gen prototypes ((S)6.4)
|   |--- s_cfg                         <- scan xtc_cfg_get/set, gen ID table ((S)6.4)
|   |--- s_globals                     <- lint: forbid bare static/_Thread_local
|   |--- s_signals                     <- lint: forbid raw signal/kill outside os_signal.c
|   |--- s_perm
|   |--- s_symlink
|   |--- s_tags
|   |--- s_test                        <- regen test driver lists
|   |--- gen_inc.awk
|   \--- RELEASE
|--- build_unix/                       <- user-created
|--- build_meson/                      <- user-created
|--- src/
|   |--- inc/
|   |   |--- xtc.h                     <- public umbrella
|   |   |--- xtc_int.h                 <- internal umbrella
|   |   |--- xtc_err.h
|   |   |--- xtc_async.h               <- async()/await() macros (see (S)12)
|   |   |--- xtc_async_decls.h         <- GENERATED by dist/s_async (see (S)6.4)
|   |   |--- xtc_cfg_ids.h             <- GENERATED by dist/s_cfg
|   |   |--- os_ext.h, io_ext.h, evt_ext.h, ptc_ext.h, orc_ext.h
|   |   |--- queue.h, hash.h, list.h   <- BSD intrusive containers (vendored)
|   |   \--- pt.h, lc.h                <- protothreads (vendored from ../pt)
|   |--- os/
|   |   |--- os_alloc.c, os_atomic.c, os_thread.c, os_tls.c,
|   |   |   os_mutex.c, os_proc.c, os_shm.c, os_dyn.c, os_time.c,
|   |   |   os_file.c, os_dir.c, os_net.c, os_signal.c, os_cpu.c,
|   |   |   os_rand.c, os_errno.c, os_globals.c
|   |   \--- asm/                      <- fiber make/jump per arch
|   |       |--- fctx_x86_64_sysv.S, fctx_x86_64_ms.asm,
|   |       |   fctx_aarch64.S, fctx_arm_eabi.S,
|   |       |   fctx_ppc64le.S, fctx_s390x.S, fctx_riscv64.S, ...
|   |--- io/
|   |   |--- io.c, io_uring.c, io_epoll.c, io_kqueue.c,
|   |   |   io_iocp.c, io_solaris.c, io_aix.c, io_poll.c, io_select.c
|   |--- evt/
|   |   |--- evt_task.c, evt_waker.c, evt_deque.c, evt_loop.c,
|   |   |   evt_exec.c, evt_timer.c, evt_park.c, evt_yield.c,
|   |   |   evt_fiber.c, evt_pt.c, evt_ucontext.c
|   |--- ptc/
|   |   |--- proc.c, proc_mailbox.c, proc_recv.c, proc_link.c,
|   |   |   proc_monitor.c,
|   |   |   chan_oneshot.c, chan_mpsc.c, chan_mpmc.c,
|   |   |   chan_broadcast.c, chan_watch.c,
|   |   |   sync_mutex.c, sync_rwlock.c, sync_sem.c,
|   |   |   sync_notify.c, sync_barrier.c,
|   |   |   sync_rcu.c,
|   |   |   gate.c, abort.c,
|   |   |   future.c, future_combinators.c,
|   |   |   dispatch.c,               <- dispatch()/reply() macro support
|   |   |   log.c, cfg.c
|   |   \--- lock/                     <- (S)13: full lock subsystem
|   |       |--- lr.c                  <- xtc_lrlock_*  (from lrlck)
|   |       |--- lw.c                  <- xtc_lwlock_*  (from lrlck)
|   |       |--- mgr.c                 <- xtc_lock_mgr_* (from libdb + noxu)
|   |       |--- mgr_alloc.c           <- slab caches, pre-allocated reservoirs
|   |       |--- mgr_list.c            <- per-locker holding lists
|   |       |--- mgr_method.c          <- pluggable lock methods (mode tables)
|   |       |--- conflict.h            <- the 9x9 matrix
|   |       |--- dd.c                  <- deadlock detector (incremental + periodic)
|   |       |--- dd_policy.c           <- victim selection policies
|   |       |--- timer.c               <- lock-wait timeouts
|   |       |--- failchk.c             <- dead-locker GC after a crash
|   |       |--- stat.c                <- per-locker / per-shard counters
|   |       \--- vec.c                 <- atomic-batch "lock vector"
|   |--- orc/
|   |   |--- supervisor.c, svr.c, fsm.c, app.c, reg.c
|   \--- pg/                           <- optional, --with-postgres
|       |--- pg_aio.c                  <- subsumes src/backend/storage/aio
|       |--- pg_latch.c
|       |--- pg_palloc.c
|       |--- pg_signal.c
|       \--- pg_backend.c
|--- test/
|   |--- munit.c, munit.h
|   |--- common.c, common.h
|   |--- test_os.c, test_io.c, test_evt.c, test_ptc.c, test_orc.c
|   |--- hegel/
|   |   |--- pbt_deque.c, pbt_mpsc.c, pbt_timer.c,
|   |   |   pbt_supervisor.c, pbt_recv.c, pbt_alloc_ownership.c,
|   |   |   pbt_rcu.c, pbt_lrlock.c, pbt_cfg.c
|   |--- soak/
|   |   |--- soak_chan.c, soak_loop.c
|   \--- ex/
|       |--- ex_async_await.c          <- the (S)12 example
|       |--- ex_pingpong.c
|       \--- ex_supervisor.c
\--- docs/
    |--- architecture.md
    |--- porting.md
    |--- pg-integration.md
    |--- async-await.md                <- deep-dive on (S)12
    \--- platforms.md                  <- per-platform notes
```

### 6.2 Autoconf (BDB/DBSQL style)

```
cd build_unix
../dist/configure --prefix=/opt/xtc \
      --enable-io-uring --enable-numa \
      --with-postgres --with-libumem
make && make check && make soak
```

Out-of-source enforced.  `make analyze` runs cppcheck/clang-analyzer/
splint.  `make coverage` runs lcov.

### 6.3 Meson / Ninja

```
meson setup build_meson \
      -Dbuildtype=debugoptimized \
      -Dio_uring=enabled -Dnuma=auto \
      -Dpostgres=enabled -Dlibumem=enabled
meson compile -C build_meson
meson test    -C build_meson
```

Same options surface as configure; `dist/srcfiles.in` is the
single source of truth, both build files are regenerated from it
by `dist/s_*` scripts.  CI runs both for every PR.

### 6.4  `dist/s_async` -- prototype generation for `xtc_async()`

(Answers your "could we have a tool that finds calls to `xtc_async()`
and creates function prototypes" idea.  Yes -- and it pays off in
two more places besides type-checking.)

**Problem.**  In Strategy C of (S)12 (the always-portable explicit-thunk
form) we want:

```c
t = xtc_async(bar, ARGS(int, a));
c = xtc_await_int(t);
```

But `xtc_async` is variadic -- the compiler can't tell that `bar`
actually takes `int` and returns `int`, so passing the wrong types
in `ARGS(...)` blows up at runtime, not at compile time.

**Solution.**  A dist tool, run as part of `s_all`, that:

1. Greps the source tree for occurrences of `xtc_async(IDENT, ...)`
   and `XTC_ASYNC_DECL(IDENT, ...)`.
2. For each `IDENT`, parses its prototype from a `PUBLIC:` /
   `XTC_ASYNC:` marker comment in the same translation unit (the
   same regex pattern DBSQL's `s_include` already uses).
3. Emits `src/inc/xtc_async_decls.h` containing one strongly-typed
   wrapper macro per discovered async target:

   ```c
   /* generated from: int bar(int);  // XTC_ASYNC: */
   #define XTC_ASYNC_PROTO_bar(ARG_a)                                  \
       _Static_assert(__builtin_types_compatible_p(                    \
           __typeof__(ARG_a), int), "xtc_async(bar): arg 1 must be int"); \
       extern int bar(int);

   /* a typed wrapper that the compiler can fully check */
   static inline xtc_task_t *__xtc_async_call_bar(int a) {             \
       struct __args { int a; } __a__ = { .a = a };                     \
       return __xtc_async_spawn(__xtc_thunk_bar, &__a__,                \
                                sizeof __a__, sizeof(int));            \
   }
   ```

4. Rewrites `xtc_async(bar, ARGS(int, a))` (via a `#define
   xtc_async(F, ...) __xtc_async_call_##F(__VA_ARGS__)`) so the
   compiler sees a real call to `__xtc_async_call_bar(a)` -- type
   errors surface at compile time, including wrong arity.

**Side benefits.**  The same scan emits:

- A registry `xtc_async_registry.c` with `(name, fn, arg_size,
  ret_size)` so `xtc_async_by_name("bar", ...)` can dispatch by
  string for tracing/RPC scenarios.
- `xtc_async_decls.gv` -- a callgraph in graphviz of who calls what
  asynchronously, useful for documentation and dependency review.
- A duplicate-detection pass: if two TUs declare different
  prototypes for the same name we fail the build.

**Same trick for `xtc_cfg`.**  `dist/s_cfg` scans for `xtc_cfg_get_*`
/ `xtc_cfg_set_*` calls, emits a registered-ID table
(`xtc_cfg_ids.h`) so each call site uses an integer ID instead of
a string lookup at runtime, and verifies that all GUC names
referenced anywhere in the tree are declared in `dist/cfg.in`.
This matches the PG GUC-API requirement: types and names checked
at build time, not at startup or first use.

The parsing in both tools is the same kind of half-AST regex
pass DBSQL's existing `s_include` and `gen_inc.awk` use.  No
C parser dependency -- just bash + awk + a tiny `dist/cscan.awk`.

---

## 7. Testing strategy

### 7.1 Unit (munit)

One `test_<layer>.c` per layer.  Same `__diag` /
`munit_assert_*` style as sparsemap/skiplist.  Vendor `munit.{c,h}`.

### 7.2 Property-based (hegel-c)

Per-primitive PBTs under `test/hegel/`:

- **MPSC**: send-order preserved; no message lost; for N producers
  and M each, receiver sees NxM.
- **Mailbox**: selective receive preserves skipped-message order;
  saved messages delivered in original arrival order when match
  predicate becomes true.
- **Work-stealing deque (ST3)**: 1-owner-K-thieves invariant:
  multiset returned == multiset pushed; no double-take; no loss.
- **Timer wheel**: a timer set for `t_now + d` never fires before
  `t_now + d`; cancellation observable.
- **Supervisor**: <= N restarts per W seconds -> supervisor exits.
- **Future combinators**: `when_all`/`when_any`/timeout semantics.
- **Allocator ownership**: every allocation freed by exactly its
  owner (or transferred); no double-free; no leak across
  `xtc_alloc_ctx_delete`.

### 7.3 Soak / stress

`make soak` -- long-running workloads under
ASan/UBSan/TSan/Valgrind/Helgrind.  Required green for tags.

### 7.4 Cross-runtime conformance

Echo / ping-pong / supervisor tests also implemented against
vanilla Tokio (Rust) and Erlang to keep semantics honest.

### 7.5 Buildfarm matrix

Per PR: Linux x86-64 glibc, Linux aarch64 musl, FreeBSD amd64,
Windows MSVC, macOS arm64 (gating).  Nightly: full PG-buildfarm
mirror (illumos, AIX, Windows MinGW, Linux ppc64le, NetBSD,
OpenBSD, DragonFlyBSD).

---

## 8. Naming and style

- Files: `<layer>_<topic>.c`, lowercase snake_case.
- Public API: `xtc_<noun>_<verb>` -- e.g. `xtc_chan_send`,
  `xtc_proc_spawn`, `xtc_future_await`.
- Internal cross-file: `__xtc_<topic>_<verb>` (double underscore).
- OS layer: `__os_*`, never `xtc_os_*`.
- Types: `xtc_<noun>_t` for opaque handles, `struct xtc_<noun>` for
  the actual struct (definition in internal headers only).
- Errors: `XTC_E_*`.
- Macros: `XTC_<TOPIC>_<NAME>`.
- Lowercase-keyword macros (the new exceptions): `dispatch`,
  `reply`, `async`, `await` -- these are the user-facing
  ergonomics; everything else stays `XTC_*`.
- `PUBLIC:` markers parsed by `dist/s_include`, exactly like DBSQL.
- C11.  No GNU extensions in headers; in `.c` files extensions
  are guarded.
- Indentation: 8-column tabs, BDB style.

---

## 9. Milestones (revised)

| # | Milestone | Deliverable |
|---|---|---|
| **M0** | Repo skeleton | `dist/`, `src/inc/`, both build systems wired, `make check` runs zero tests successfully on Linux + macOS + Windows. |
| **M1** | L0 `os/` | All `__os_*` groups including `os_globals.c`; munit + hegel for `os_atomic`. |
| **M2** | L1 minimal | `io_poll` + `io_epoll` backends; cross-thread wakeup. |
| **M3** | L2 single-loop | Single-thread event loop, run queue, timer wheel, `xtc_task_spawn` of state-machine tasks. |
| **M4** | L2 coroutines | `evt_pt` (Duff's-device) + `evt_fiber` (asm) + `async()`/`await()` macros ((S)12) + `xtc_yield()` + `XTC_COOP_REGION`. |
| **M5** | L2 multi-loop | ST3 work-stealing deque, executor with N loops, cross-loop wake.  PBT for deque. |
| **M6** | L1 io_uring + kqueue + iocp | All major I/O backends. |
| **M7** | L3 channels + futures | All channel types; future/promise + combinators. |
| **M8** | L3 processes + mailboxes | `xtc_proc_*`, selective receive, links, monitors, `xtc_exit`.  PBT. |
| **M9** | L3 sync + dispatch | All sync primitives; `dispatch()`/`reply()` sugar. |
| **M10** | L4 orc | Supervisor (4 strategies), `xtc_svr`, `xtc_fsm`, `xtc_app`, `xtc_reg`.  PBT. |
| **M11** | Memory model | `xtc_alloc_ctx`, slab caches, ownership transfer, libumem-debug flags. |
| **M12** | Shared globals | `XTC_GLOBAL`/`XTC_PERLOOP`/`XTC_FN_STATIC`/`XTC_TLS` macros + `dist/s_globals` + `dist/s_signals` lints. |
| **M13a** | RCU primitive | `xtc_rcu` epoch-based reclamation; PBT for read-side wait-freedom; basic benchmarks. |
| **M13b** | LRLock + LWLock | Port from `~/ws/postgres/lrlck` to xtc style; PBT (lin-checker); micro/macro benchmarks vs `xtc_rwlock`. |
| **M13c** | Lock manager | Port from `~/ws/libdb` + `~/ws/noxu`; 9-mode matrix, intent locks, promotion, sharded tables, incremental + periodic deadlock detector with all victim policies, resource caps from (S)13.5; full (S)13.8 test/bench/scale/exhaust suite. |
| **M14** | `xtc_cfg` + `xtc_log` + `dist/s_async` + `dist/s_cfg` | Function-call config API, per-loop logging, prototype-generation tools. |
| **M15** | L1 Solaris + AIX + select | Tier-2 platform completeness. |
| **M16** | L5 PG adapter | Subsumes `src/backend/storage/aio`; latch/signal/CFI/MemoryContext bridges; example threaded backend. |
| **M17** | Conformance + benchmarks | Cross-runtime conformance vs Tokio + Erlang; published p99 numbers. |

---

## 10. Open design questions (revised)

These remain for your sign-off.  Defaults given are my recommendation.

| # | Question | Default |
|---|---|---|
| Q1 | Stackless or stackful coroutines? | **Both**: stackful (fiber asm) is the default; protothreads automatic on platforms without fiber asm; ucontext bridge in between. |
| Q2 | How shared-nothing? | **Soft Seastar**: per-loop default; cross-loop access only via channels/mailboxes/shared-buffer handles. |
| Q3 | Mailbox = MPSC or BEAM-with-save-queue? | **BEAM with save queue.**  Bounded variant available via channels. |
| Q4 | Cancellation propagation | **`xtc_abort_source` (cooperative) + `xtc_exit` (kill).** No implicit structured concurrency. |
| Q5 | Allocator | **Pluggable + per-task arena + slab caches + ownership domains** (libumem-shaped). |
| Q6 | OTP scope | Yes: supervisor, svr, fsm, app, reg.  No (for now): distribution, hot reload, ETS. |
| Q7 | Multi-process from L0 | **L0 has the primitives, L1-L4 don't use them in v1.** PG L5 is the only multi-process consumer. |
| Q8 | License | **ISC license.** |
| Q9 (new) | Configure-time vs runtime backend selection | **Configure-time only.** No vtable on hot path. |
| Q10 (new) | Default loop count | **`__os_ncpus()`**, configurable down to 1 for the strict-degraded build. |
| Q11 (new) | Coroutine stack size default | **64 KiB with guard page**, configurable per-spawn. |
| Q12 (new) | Default scheduling discipline | **Preemptive at await/yield boundaries**, with `XTC_COOP_REGION` for strict cooperative blocks.  `xtc_yield()` available as the explicit reduction-trip. |
| Q13 (new) | Where do RCU/LRLock/LWLock live | **L3 (`ptc/sync_rcu.c` for RCU; `ptc/lock/` for the lock subsystem)**.  Each can be disabled at configure time on platforms with quirky atomics. |
| Q14 (new) | `xtc_cfg` source-precedence semantics | **Match PG GUC** (`SOURCE_DEFAULT < FILE < SESSION < OVERRIDE`), so the migration is mechanical. |
| Q15 (new) | Default deadlock detector | **`XTC_DD_INCREMENTAL`** for everyday workloads; **`XTC_DD_HYBRID`** recommended after M13c benchmarking; periodic-only for legacy parity. |
| Q16 (new) | Default victim policy | **`XTC_DD_OLDEST`** (PG/Oracle convention).  `XTC_DD_SMALLCOST` recommended once user code starts setting per-locker abort costs. |

---

## 11. Risks (revised)

1. **Platform matrix is huge.**  AIX + illumos + Windows MinGW are
   labour-intensive.  Mitigation: tier the matrix; only Tier 1
   gates PRs.
2. **io_uring kernel-version sensitivity.**  Mitigation: probe at
   configure, fall through to epoll cleanly.
3. **Fiber asm per arch.**  Boost.context already has them; we
   reimplement under our license, but it's still 12 small `.S`
   files to maintain.  Mitigation: write a CI test that does
   `make_fcontext`/`jump_fcontext` round-trip on every arch.
4. **MSVC + C11 atomics edge cases.**  Mitigation: `__os_atomic`
   shim hides the compiler.
5. **Selective receive O(N^2) trap.**  BEAM's save-queue + scan-mark
   trick; implement on day 1.
6. **Two build systems drifting.**  `dist/srcfiles.in` is the SoT;
   CI runs both.
7. **PG AIO subsumption is contentious.**  PG just landed
   `method_io_uring.c`; replacing it with xtc requires careful
   API matching.  Mitigation: in M16 we *wrap* PG's AIO API
   surface around xtc internals first, ship that, and only later
   propose replacing the underlying `method_*.c`.
8. **Threaded PG is a multi-year upstream effort.**  Mitigation:
   xtc must be useful standalone for greenfield C servers.  But also:
   the v20->v21 PG roadmap ((S)14) is concrete and short enough that
   xtc *as-designed* matches every primitive on that list.  We
   should pitch xtc not as a research project but as the toolbox
   v20 is going to need anyway.
9. **Predictable p99 is hard.**  Even one slow path (e.g. an
   accidental kernel call in the steady-state) wrecks tail
   latency.  Mitigation: latency-histogram tests in `make soak`
   that fail the build if p99 regresses.
10. **LRLock under threaded mode may behave differently than under
   processes.**  The 8-core data was inconclusive; threaded mode
   amplifies cache-line bouncing because there's no kernel
   boundary.  Mitigation: re-benchmark `xtc_lrlock` and
   `xtc_lwlock` specifically under simulated threaded-PG
   contention in M13c *before* promoting them as the recommended
   primitive for ProcArray-style use cases.

---

## 12.  `async()` / `await()` in C -- yes, with caveats

The user asked: can we get to this?

```c
int bar(int a)
{
    return a ^ 2;
}

int foo(int a, char *b)
{
    int c;
    xtc_task_t *t;

    t = async(bar(a));     /* spawn, do not wait */
    /* ... do something else ... */
    c = await(t);          /* cooperatively wait, return value */

    return c;
}
```

**Short answer: yes, with three implementation strategies, two of
which give us *exactly* this syntax and one of which requires a
small syntactic compromise on portability-only platforms.**

The challenge is that `async(bar(a))` looks like a function call
on `bar(a)`, but we want to *defer* `bar(a)` and return a task
handle.  In C without compiler help, you cannot pass an
"unevaluated expression" as an argument.  We need either:

- a macro that captures `bar` and `a` separately (lose the
  literal syntax but stay portable), **or**
- a compiler extension that supports nested functions / blocks /
  statement expressions (keep the literal syntax on most
  compilers), **or**
- a stackful fiber that runs `bar(a)` to completion in a separate
  context (keep the literal syntax everywhere, at the cost of a
  fiber switch).

We will offer **all three**, selected at configure time and
documented as such.

### 12.1 Strategy A -- fiber-based (default, fully portable to the literal syntax)

`async(EXPR)` is a macro that:

1. Allocates a small fiber stack from the per-loop slab (`xtc_cache`).
2. Captures `EXPR` as an *expression to evaluate inside the fiber*
   by emitting a one-shot trampoline function that closes over
   the calling frame via a stack copy.
3. Returns a `xtc_task_t *` whose result slot is the fiber's
   return value.

```c
/*
 * Sketch of src/inc/xtc_async.h
 *
 * Requires: stackful fiber substrate (evt_fiber.c) -- available on
 * every arch we ship asm for, plus the ucontext fallback.  Works
 * with gcc, clang, MSVC.
 */

#define async(EXPR) \
    __xtc_async_capture(__FILE__, __LINE__, \
        __XTC_ASYNC_TRAMPOLINE_BEGIN \
            __typeof__(EXPR) __xtc_result__ = (EXPR); \
            __xtc_fiber_return(&__xtc_result__, sizeof __xtc_result__); \
        __XTC_ASYNC_TRAMPOLINE_END)

#define await(T) \
    ({ \
        __typeof__(*(T)->__result_hint__) __xtc_out__; \
        (void)__xtc_task_await((T), &__xtc_out__, sizeof __xtc_out__); \
        __xtc_out__; \
    })
```

The trick is `__XTC_ASYNC_TRAMPOLINE_*`: on GCC/Clang this expands
to a statement expression `({ ... })` that creates the trampoline
inline.  On MSVC (which lacks statement expressions and nested
functions both), it expands to a numbered top-of-translation-unit
trampoline declaration plus a call site that invokes
`__xtc_fiber_spawn` with a captured-arg blob.

For a function call like `bar(a)` the trampoline is:

```c
/* what async(bar(a)) expands to, fiber-mode, GCC/Clang */
xtc_task_t *t = ({
    /* capture the lexical environment by value */
    struct __xtc_env__ { int a; } __env__ = { .a = a };
    /* trampoline runs in the new fiber */
    static int __xtc_trampoline__(void *_env, void **out_result) {
        struct __xtc_env__ *e = _env;
        int r = bar(e->a);
        *out_result = (void*)(intptr_t)r;
        return 0;
    }
    __xtc_async_spawn(__xtc_trampoline__, &__env__, sizeof __env__,
                      sizeof(int));
});
```

`await(t)` then performs `xtc_future_await` (cooperative; if `foo`
itself is running on a fiber, this just parks the fiber until
the inner one completes).

**Caveats and what this buys:**

- Works for any C expression `EXPR`, not just function calls:
  `async(a*b + c[d])`, `async(big_compute(p, q, &out))`, etc.
- Local-variable capture is by value, copied at the moment of
  `async()` -- exactly what a Rust `move ||` closure does.  No
  surprise mutation.
- Cost: one fiber stack (cached/recycled), one heap copy of the
  captured frame (typically 0-64 bytes), one fiber switch on
  await.  Fiber switches are ~30 ns on modern x86-64 with our
  ASM; significantly cheaper than a syscall.
- The enclosing function `foo()` itself does NOT need to be
  declared specially -- `await()` works because `foo` is *also*
  running on a fiber (every xtc task is).  This is the key
  property that makes the syntax look like Tokio: there is no
  `async fn foo` keyword; **every entry point into xtc is
  automatically on a fiber.**
- On compilers without nested functions (clang, MSVC) the
  trampoline is hoisted to file scope and a per-call-site
  numbered name is used (`__xtc_tramp_<file>_<line>`).  We
  generate these via a `dist/s_async` preprocessing step or via
  a smaller variant of the macro that requires the user to write:

  ```c
  /* clang/MSVC fallback if -DXTC_NO_NESTED_FN */
  XTC_ASYNC_FN(int, my_bar_call, (int a)) { return bar(a); }
  /* ... */
  t = async_call(my_bar_call, a);    /* slight syntax compromise */
  ```

  We hide this behind one macro: `async_e(EXPR)` is the GCC-only
  literal form; `async(F, ...)` is the portable form; both
  return `xtc_task_t *`.

### 12.2 Strategy B -- protothread-based (graceful-degradation mode)

When the build has only Duff's-device coroutines (no fiber asm,
no ucontext -- embedded, or constrained illumos/AIX), `async()`
and `await()` still work, but with the protothread tax: locals
in the *enclosing* function must be in a state struct, and the
function must be wrapped:

```c
XTC_TASK(int, foo, (int a, char *b))   /* wraps as a protothread */
{
    XTC_LOCALS(int c; xtc_task_t *t;)  /* lift locals into pt frame */
    XTC_BEGIN();

    XTC_LOCAL(t) = async(bar, a);
    /* ... do something else (no syscalls -- must be cooperative) ... */
    XTC_LOCAL(c) = await_int(XTC_LOCAL(t));

    XTC_RETURN(XTC_LOCAL(c));
    XTC_END();
}
```

Same logical behaviour, uglier syntax.  This is the floor of
graceful degradation -- the *literal* `async(bar(a))` syntax is
not achievable here without a fiber, because Duff's device
cannot resume across a yield without lifting locals.

### 12.3 Strategy C -- explicit thunk (always portable, always cheap)

This is a forever-fallback for programmers who can't or don't
want fibers and don't want the protothread macros either:

```c
int foo(int a, char *b)
{
    int c;
    xtc_task_t *t;

    /* portable everywhere; cheapest possible spawn */
    t = xtc_async(bar, ARGS(int, a));
    /* ... do something else ... */
    c = xtc_await_int(t);
    return c;
}
```

Here `xtc_async()` is a variadic macro that builds the args
struct from `ARGS(type, val, ...)` and finds (or generates via
`XTC_ASYNC_DECL(bar, int, (int))`) a thunk with a known
signature.  Zero magic.  Slightly more typing.  Always works.

### 12.4 Recommendation

- The **default** build on every Tier 1 platform supports
  `async(bar(a))` literally -- fiber-based, GCC/Clang/MSVC.
- The **constrained** build (no fiber asm, no ucontext) requires
  the protothread `XTC_TASK` macros or the explicit-thunk form.
- Our *examples and docs* always show the literal-syntax form
  first, with a `#ifdef XTC_HAVE_FIBER` note pointing at the
  fallbacks.

So the answer to "could this work in C with our new code?" is
**yes**, with the literal syntax on every platform that has fiber
asm or ucontext (which is every Tier 1 platform), and a
documented graceful-degradation path for the rest.

A worked example will live at `test/ex/ex_async_await.c` and run
in CI, exactly as the user wrote it.

---

## 13. Lock manager (`ptc/lock/`)

This is large enough to deserve its own section.  We are pulling in
working implementations from three places, renaming to xtc style,
and adding the missing pieces (lock modes / intent locks / promotion /
deadlock detection / resolution / resource caps) that none of the
three have all of.

### 13.1 Sources

| Code we pull in | From | Becomes |
|---|---|---|
| LRLock implementation (`lrlock.c`/`lrlock.h`, ~1.2k LoC) | `~/ws/postgres/lrlck/src/{backend/storage/lmgr,include/storage}/lrlock.{c,h}` | `src/ptc/lock/lr.c` + `src/inc/lock_lr.h` -- `xtc_lrlock_*` |
| LWLock implementation (`lwlock.c`/`lwlock.h`/`lwlocklist.h`, ~2.1k LoC) | `~/ws/postgres/lrlck/src/{backend/storage/lmgr,include/storage}/lwlock*.{c,h}` | `src/ptc/lock/lw.c` + `src/inc/lock_lw.h` -- `xtc_lwlock_*` |
| Lock-mode + intent-lock matrix (NG/READ/WRITE/IREAD/IWRITE/IWR/READ_UNCOMMITTED/WWRITE, conflict matrix, lockers, lock-list management) | `~/ws/libdb/src/lock/{lock,lock_alloc,lock_id,lock_list,lock_method,lock_region,lock_util}.c` (~5k LoC) | `src/ptc/lock/mgr.c` + `src/inc/lock_mgr.h` -- `xtc_lock_mgr_*` |
| Deadlock detection: build waits-for graph, find cycle, victim policies (DEFAULT/EXPIRE/MAXLOCKS/MAXWRITE/MINLOCKS/MINWRITE/OLDEST/RANDOM/YOUNGEST), abort + rollback signal | `~/ws/libdb/src/lock/lock_deadlock.c` (~1k LoC) | `src/ptc/lock/dd.c` -- `xtc_lock_dd_*` |
| Lock timer / expiry / fail-check | `~/ws/libdb/src/lock/{lock_timer,lock_failchk}.c` | `src/ptc/lock/timer.c`, `src/ptc/lock/failchk.c` |
| Sharded lock tables, **incremental** waits-for graph (O(1) cycle check on the wait path), thread-locker share groups, configurable `lock_timeout_ms`, statistics | `~/ws/noxu/crates/noxu-txn/src/{lock_manager,deadlock_detector,lock_info}.rs` (~2k LoC, Rust -> ported to C) | merged into `src/ptc/lock/mgr.c` and `src/ptc/lock/dd.c` |

Nothing is copy-pasted as-is.  Each source is **rewritten to xtc
conventions** (`__xtc_*` / `xtc_*` naming, `int` returns with
out-params, `XTC_E_*` errors, `__os_*` for syscalls, BDB-style
file/function comment block, `PUBLIC:` markers parsed by
`dist/s_include`).  The algorithms are unchanged -- we are buying
the correctness work, not the prose.  Each port carries the
upstream copyright notice in the file header.

### 13.2 Layered API

```
+------------------------------------------------------------------+
| Lock manager (`xtc_lock_mgr`):                                      |
|   - sharded hash table of lock objects (key -> lock)                |
|   - lockers (xtc_locker_t = an xtc_proc + transaction ID)          |
|   - 9-mode conflict matrix (BDB)                                   |
|   - intent locks + promotion                                       |
|   - per-locker timeouts                                            |
|   - deadlock detector callbacks                                    |
|   - resource caps ((S)13.5)                                          |
|-------------------------------------------------------------------|
| Deadlock detector (`xtc_lock_dd`):                                  |
|   - incremental waits-for graph maintained on the wait path        |
|   - on-demand cycle scan with multiple victim policies             |
|   - victim notification via xtc_send (kill envelope)               |
|-------------------------------------------------------------------|
| Latches: low-level cooperative locks the lock manager itself uses  |
|   - `xtc_mutex` (parking)        - `xtc_rwlock` (parking, fair)    |
|   - `xtc_lwlock` (LWLock-shape)  - `xtc_lrlock` (left-right)       |
|   - `xtc_spinlock` (debug-only; bare CPU spin under TSan)          |
|-------------------------------------------------------------------|
| Atomic primitives (`__os_atomic_*`): C11 atomics + futex/Wait      |
\-------------------------------------------------------------------+
```

The sync primitives (`sync_mutex.c` etc.) stay above the lock
manager: they are the *latches* the manager itself uses to
protect its shards.  They are themselves implemented with
`__os_atomic_*` directly, never with the lock manager (that
would be a circular dependency).

### 13.3 Lock modes and conflict matrix (BDB)

We adopt BDB's nine-mode lattice verbatim because it is the most
complete lock matrix in the open-source database tradition.

| Mode | xtc constant | Meaning |
|---|---|---|
| 0 | `XTC_LOCK_NG` | Not granted (sentinel) |
| 1 | `XTC_LOCK_R` | Shared / read |
| 2 | `XTC_LOCK_W` | Exclusive / write |
| 3 | `XTC_LOCK_WAIT` | Wait-for-event placeholder |
| 4 | `XTC_LOCK_IW` | Intent exclusive / intent-write |
| 5 | `XTC_LOCK_IR` | Intent shared / intent-read |
| 6 | `XTC_LOCK_IWR` | Intent read+write |
| 7 | `XTC_LOCK_RU` | Read-uncommitted (degree-1 isolation) |
| 8 | `XTC_LOCK_WW` | Was-written (degraded retain) |

The conflict matrix is a 9x9 byte table compiled into
`src/ptc/lock/conflict.h`, with a unit test that verifies the
IW/IR/IWR rows against the canonical Gray/Lorie/Putzolu/Traiger
paper.

### 13.4 Lock promotion

A locker that holds a weaker mode (e.g. `XTC_LOCK_R`) and requests
a stronger mode (`XTC_LOCK_W`) on the same object goes through a
*promotion path*:

1. The request is enqueued **ahead** of any other waiters not
   already holding a lock on this object (avoids the convoy
   starvation problem).
2. If the upgrade conflicts with another current holder, the
   detector immediately checks for a deadlock cycle that
   includes this new edge.  Promotion is a frequent source of
   deadlocks that does not show up in vanilla wait-for graphs
   unless the detector explicitly models the locker's existing
   holdings.
3. If a cycle is found, the upgrade caller is the natural
   victim (smallest "abort cost" -- it is already holding a
   lock so its abort is cheap to roll back).
4. Upgrade requests do not deadlock-with-self: a locker can
   upgrade its own lock without going through the detector.

The promotion ladder is fixed: `R` -> `IW`/`IWR` -> `W`.  Other
transitions return `XTC_E_LOCK_BAD_PROMOTE`.

### 13.5 Resource governance

Three exhaustion vectors get explicit caps and reservoirs.
Nothing in the lock manager allocates from the heap on the
wait path -- every datum is pre-allocated at
`xtc_lock_mgr_init` time.

| Resource | Cap knob | Mechanism | What happens at the cap |
|---|---|---|---|
| Lock objects | `max_locks` | Slab cache pre-allocated; per-shard reservoir | `XTC_E_LOCK_NOMEM`; detector picks a victim |
| Locker descriptors | `max_lockers` | Slab cache | Same |
| Lock-list cells per locker | `max_objects` | Slab cache | Same |
| Waiter notify pairs | `max_waiters` | Pool, recycled | Same |
| Wait queue depth per object | `max_wait_depth` | Bounded queue with backpressure | New waiters return `XTC_E_LOCK_TOOBUSY` immediately; lock manager has *not* gone into the wait state |
| Stack per `xtc_proc` fiber | `proc_stack_size` (64 KiB) + guard page | Guard-page fault -> `sigaltstack` handler | Supervisor restarts the offending proc, no others affected |
| CPU per task | `task_reductions` | Reduction counter; preempts at await/yield | Task moves to back of run queue (BEAM model) |
| Memory per `xtc_proc` | `proc_alloc_cap` | Per-proc arena ceiling | `xtc_palloc` returns `XTC_E_NOMEM`; trips `XTC_CHECK_CANCEL()` |
| Memory per executor | `executor_alloc_cap` | Sum-of-procs ceiling | Spawns return `XTC_E_RESOURCE`; supervisor backpressure |
| Open file descriptors | `max_fds` | Counted at `__os_open`/`__os_socket` | `XTC_E_NFILE`; reaper task tries idle fds first |
| io_uring SQEs in flight | `uring_depth` | Bounded SQ ring | Submitter parks via `xtc_notify` |
| Wakers in flight (cross-loop) | `inbox_depth` | Bounded MPSC | Configurable: park / drop / error |

Every cap is exposed as an `xtc_cfg` knob with a default,
min, max, and a hot-reload behaviour: caps that *grow* take
effect immediately by enlarging slab caches; caps that
*shrink* log a warning and take effect on next executor restart.

### 13.6 Deadlock detection and resolution

We implement *both* the BDB on-demand detector and the Noxu
incremental detector and let configuration pick which is
active.  They are not redundant -- they have different cost
profiles:

- **Incremental (`XTC_DD_INCREMENTAL`, default).**  When a
  locker enters the wait path, an edge `(waiter -> owner)`
  is added to a process-wide `xtc_lock_dd_graph` (a small
  `xtc_lrlock`-protected map).  Before sleeping, the
  detector performs a depth-first cycle search starting
  at the new waiter; if a cycle exists it includes the new
  edge.  Cost is O(cycle length) on the wait path; zero
  cost on the no-wait fast path.  Noxu's algorithm.
- **Periodic (`XTC_DD_PERIODIC`).**  A dedicated detector
  `xtc_proc` wakes every `dd_interval_ms` (default 250 ms)
  and rebuilds the full waits-for graph by scanning all
  shards.  More expensive but catches subtle cycles missed
  by the incremental detector when wakeups race with new
  waits.  BDB's algorithm.  We always run periodic on a
  slow timer (`dd_periodic_full_ms`, default 30 s) even when
  incremental is the primary, as a safety net.
- **Hybrid (`XTC_DD_HYBRID`, recommended after M13
  benchmarking).**  Incremental on every wait, full periodic
  every 30 s, and a *triggered* full scan when any locker
  has been blocked for more than `dd_suspicious_ms`
  (default 1 s).

When a cycle is detected the detector runs a **victim
selection policy**.  Full BDB menu plus two new ones:

| Policy | xtc constant | Selects |
|---|---|---|
| Default | `XTC_DD_DEFAULT` | Same as `OLDEST` |
| Expire | `XTC_DD_EXPIRE` | Anyone whose lock has expired |
| Max-locks | `XTC_DD_MAXLOCKS` | Locker holding most locks |
| Max-write | `XTC_DD_MAXWRITE` | Locker holding most write locks |
| Min-locks | `XTC_DD_MINLOCKS` | Locker holding fewest locks (cheapest rollback) |
| Min-write | `XTC_DD_MINWRITE` | Locker holding fewest write locks |
| Oldest | `XTC_DD_OLDEST` | Oldest transaction (highest seniority survives -- PG/Oracle convention) |
| Random | `XTC_DD_RANDOM` | Random victim (good for testing fairness) |
| Youngest | `XTC_DD_YOUNGEST` | Newest transaction (least invested) |
| **Lowest-priority** | `XTC_DD_LOWPRIO` | Locker with smallest `xtc_proc.priority` (xtc-new) |
| **Smallest-cost** | `XTC_DD_SMALLCOST` | Locker reporting smallest abort-cost via `xtc_locker_set_cost()` (xtc-new) |

Resolution path: the chosen victim's `xtc_proc` receives a
special `{'$xtc_dd_abort', cycle, victim}` mailbox message at
the **head** of its mailbox (jumps the queue).  The locker
observes this at its next `await`/`xtc_yield`/`XTC_CHECK_CANCEL()`
and unwinds with `XTC_E_DEADLOCK`.  All locks held by that
locker are released as part of `xtc_proc` cleanup.  No
longjmp-from-inside-the-detector -- the detector only signals;
the victim chooses *when* to die at a known-safe point.

Retry policy is the caller's: most users wrap an entire
transaction in `xtc_txn_run(...)` which catches
`XTC_E_DEADLOCK` and re-runs the closure with exponential
backoff and a retry cap.

### 13.7 Layered API surface (selected)

```c
/* --- LRLock --- */
xtc_lrlock_t *xtc_lrlock_create   (size_t data_size, xtc_lr_apply_fn,
                                   xtc_lr_sync_fn, const char *name);
const void   *xtc_lrlock_read_begin (xtc_lrlock_t *);
void          xtc_lrlock_read_end   (xtc_lrlock_t *);
void         *xtc_lrlock_write_begin(xtc_lrlock_t *);
void          xtc_lrlock_apply_op   (xtc_lrlock_t *, const void *op, size_t);
void          xtc_lrlock_publish    (xtc_lrlock_t *);
void          xtc_lrlock_write_end  (xtc_lrlock_t *);

/* --- LWLock --- */
int  xtc_lwlock_init     (xtc_lwlock_t *, const char *tranche);
int  xtc_lwlock_acquire  (xtc_lwlock_t *, xtc_lw_mode_t);   /* SHARED/EXCL */
int  xtc_lwlock_try      (xtc_lwlock_t *, xtc_lw_mode_t);
void xtc_lwlock_release  (xtc_lwlock_t *);

/* --- Heavyweight lock manager --- */
xtc_lock_mgr_t *xtc_lock_mgr_create(const xtc_lock_cfg_t *);
int  xtc_locker_create (xtc_lock_mgr_t *, xtc_proc_t *owner, xtc_locker_t **);
int  xtc_lock_get      (xtc_locker_t *, const xtc_lock_key_t *,
                        xtc_lock_mode_t, uint32_t flags,
                        xtc_lock_h_t *out);              /* may park */
int  xtc_lock_get_async(xtc_locker_t *, ..., xtc_future_t **out); /* awaitable */
int  xtc_lock_put      (xtc_locker_t *, xtc_lock_h_t);
int  xtc_lock_promote  (xtc_locker_t *, xtc_lock_h_t, xtc_lock_mode_t);
int  xtc_lock_vec      (xtc_locker_t *, const xtc_lock_req_t *, int n);
                       /* atomic batch acquire -- BDB-style "lock vector" */

/* --- Deadlock detector --- */
int  xtc_lock_dd_set_policy(xtc_lock_mgr_t *, xtc_dd_policy_t);
int  xtc_lock_dd_run_now   (xtc_lock_mgr_t *, int *victims_aborted);
int  xtc_lock_dd_get_graph (xtc_lock_mgr_t *, xtc_dd_graph_t *out);
```

`xtc_lock_get_async` is the loop-friendly variant: returns a
`xtc_future_t` so a request can be `await`ed without parking
the fiber on a kernel primitive.  This is the version PG-on-xtc
will use -- it lets thousands of waiters coexist on a few OS
threads.

### 13.8 Testing & benchmarking strategy

*The standalone testing strategy ((S)7) covers basic primitives.
Locks need a more aggressive program because they are the
hottest code in any database.*

#### 13.8.1 Correctness testing

- **Unit (munit).**  Per-mode acquire/release; conflict-matrix
  table-driven test (every cell of the 9x9 matrix verified);
  promotion ladder; intent-lock semantics.
- **Property-based (hegel).**  Per-primitive invariants:
  - `pbt_lr.c`: writers never see a stale read; epoch counter
    monotone; oplog never grows beyond `oplog_capacity`.
  - `pbt_lw.c`: at most one writer; readers see a consistent
    pre/post snapshot; release matches acquire.
  - `pbt_mgr.c`: for any sequence of acquire/release/promote,
    the conflict matrix is honoured at every point.
  - `pbt_dd.c`: every cycle in a randomly-generated waits-for
    graph is detected; no false positives; victim selection
    matches the policy.
  - `pbt_promote.c`: every promotion that should deadlock does;
    every promotion that shouldn't, doesn't.
- **Linearizability harness (vendored, ~300 LoC).**  Lin-checker
  for `xtc_lwlock` and `xtc_lrlock`: enumerates short
  interleavings (3 threads x 6 ops) and verifies a valid
  linearization exists in the conflict matrix.
- **Sanitizers.**  Every test runs under TSan, ASan, UBSan,
  Helgrind, DRD.  TSan-clean is a release-blocker.
- **Loom-style model checker (optional, `--with-loom`).**  Tiny
  C port of Rust's `loom` for the wait-free LRLock read path --
  exhaustive interleaving search up to N=4 threads.  Only way
  to *prove* the wait-free path is correct under all memory
  orderings.
- **Conformance.**  Same bank-account-transfer 2PL stress run
  against xtc, BDB, vanilla pthread, and noxu Rust LM -- all
  four must produce identical commit/abort histograms
  (modulo randomized victim policies).

#### 13.8.2 Macro benchmarks

Under `bench/lock/`, with a `bench_runner` driver that emits CSV
plus latency histograms (HdrHistogram-shaped):

| Bench | What it measures |
|---|---|
| `bm_read_only` | LRLock vs LWLock-shared vs `xtc_rwlock` read scalability, 1...256 threads |
| `bm_write_only` | Single-writer throughput, contention p50/p95/p99/p999 |
| `bm_read_write_mix` | 80/20, 95/5, 99/1 read/write mixes |
| `bm_promotion_storm` | All threads upgrade R->W concurrently |
| `bm_deadlock_storm` | Constructed cycles at increasing rate -- detector latency p99 |
| `bm_proc_array` | The PG ProcArray-snapshot scenario from the LRLock RFC |

Results auto-published to `docs/bench/`.  CI fails the build if
**p99** regresses by more than 5% vs the previous tagged release
on Linux x86-64 glibc.

#### 13.8.3 Micro benchmarks

Under `bench/lock/micro/`, with cycle-count targets:

- `u_acquire_uncontended` -- LRLock read <= 6 ns; LWLock shared <= 12 ns; mgr fast path <= 80 ns.
- `u_cas_pingpong` -- baseline two-thread CAS cost.
- `u_dd_cycle_check` -- incremental detector at graph sizes 8 / 64 / 512 / 4096.  Linear in cycle length.
- `u_promote_cas` -- fastest possible R->W promotion (no contention).

#### 13.8.4 Scalability testing

- **Thread count sweep.**  1, 2, 4, 8, 16, 32, 64, 128, 256
  threads on the largest box we have (community AWS metal
  `m6i.metal` and `r8i.metal-96xl` per the
  `pg-numa-benchmark` skill).  Plot throughput and p99 vs
  thread count for each macro bench.
- **NUMA sweep.**  Same workloads with reactors pinned
  per-NUMA-node and across nodes; cross-node contention
  curves.
- **CPU-budget sweep.**  Under cgroup CPU limits at 50%, 75%,
  100% -- we want graceful degradation, not cliffs.
- **Cache-line behavior.**  `perf c2c` runs as part of CI on
  the contention benches; false-sharing regressions fail
  the build.

#### 13.8.5 Resource-exhaustion testing

Dedicated suite under `test/exhaust/` that *intentionally*
starves each cap in 13.5 to verify the documented behaviour:

| Test | Starves | Expected outcome |
|---|---|---|
| `ex_max_locks` | `max_locks + 1` | `XTC_E_LOCK_NOMEM` and detector picks a victim |
| `ex_wait_depth` | One object's wait queue | `XTC_E_LOCK_TOOBUSY` immediately |
| `ex_proc_alloc` | A proc's arena | `XTC_E_NOMEM`; clean unwind |
| `ex_stack_overflow` | Recurse to guard page | Supervisor restarts the offending proc, no others affected |
| `ex_fd_exhaustion` | `max_fds` | Reaper task recovers idle fds; else `XTC_E_NFILE` |
| `ex_uring_full` | Oversubmit io_uring SQEs | Submitter parks |
| `ex_inbox_full` | Cross-loop MPSC | Configured policy (park / drop / error) |
| `ex_dd_cycle_long` | Construct a 1024-deep cycle | Detector still completes in bounded time; victim selected |

#### 13.8.6 Soak

`make soak-lock` runs the workloads from 13.8.2 for 1 hour each
under TSan and Helgrind.  Required green for any tag.

### 13.9 Where this lands in milestones

The original M13 ("RCU + LRLock + LWLock") splits into three
milestones -- see the revised table in (S)9.

---

## 14. SQL query traversal -- a worked example

The pitch is concrete only when we trace what actually happens.
Below we follow one ordinary OLTP query end-to-end through xtc.
This is hypothetical -- the PG adapter is M16 -- but every step is
backed by a primitive already designed.

### 14.1 The query

```sql
-- Two-table join with an aggregate, ORDER BY, LIMIT.
-- Realistic mid-complexity OLTP shape; I/O on three relations,
-- a syscache hit on a third, and a write of one heap row at commit.
BEGIN;
  SELECT  c.region,
          SUM(o.amount) AS total
  FROM    orders o
  JOIN    customers c ON c.id = o.customer_id
  WHERE   o.created_at > now() - interval '7 days'
          AND c.tier = 'gold'
  GROUP BY c.region
  ORDER BY total DESC
  LIMIT 10;

  UPDATE customers SET last_seen = now()
          WHERE id IN (SELECT customer_id FROM recent_logins);
COMMIT;
```

Assume:

- `orders` is large (TB-scale, partitioned by month, on direct
  io_uring reads).
- `customers` and `recent_logins` are smaller (in shared
  buffers, mostly).
- This is one session out of ~= 8 000 active sessions.
- The xtc executor has 32 reactors pinned to 32 cores.

### 14.2 Connection-level: the session is an `xtc_proc`

The TCP listener is a single `xtc_proc` (call it `listener_pid`)
running on reactor 0, awaiting accept events from `xtc_io`
(io_uring `IORING_OP_ACCEPT`).  When a new connection arrives:

```c
/* simplified pg_backend.c sketch */
xtc_pid_t backend;
xtc_proc_spawn(&backend, pg_backend_main, conn_fd,
               &(xtc_proc_opts_t){
                   .name        = "backend",
                   .mailbox_cap = XTC_MAILBOX_UNBOUNDED,
                   .stack_size  = 128 * 1024,
                   .reactor     = XTC_REACTOR_LEAST_LOADED,
               });
```

The new backend is now *the* session struct: every formerly-global
PG variable for this session lives in `pg_backend_state` (a field
on the `xtc_proc` user data).  No TLS, no `__thread`, no
`MyProcPid`.  `xtc_self()->pid` is the session ID.

### 14.3 BEGIN: just a state transition

The backend reads the `BEGIN` from its socket:

```c
xtc_io_op_t op = { .kind = XTC_IO_RECV, .fd = conn_fd, ... };
int nread = await(xtc_io_submit(&op));   /* fiber parks until io_uring CQE */
```

`await` parks the fiber on its waker; reactor 0 keeps running
other sessions in the meantime.  When the bytes arrive, the
io_uring CQE delivers a tag pointing to this fiber's waker; the
reactor reschedules it.  The backend transitions to `IN_TXN`,
allocates an `xtc_alloc_ctx_t` rooted at the proc:

```c
bk->txn_ctx = xtc_alloc_ctx_new(bk->proc_ctx, "txn");
```

All memory allocated for this transaction lives under `bk->txn_ctx`
and is released atomically on COMMIT or ROLLBACK.

### 14.4 Parse / plan / execute: where the async actually starts

The SELECT is parsed and planned.  Planning is mostly CPU-bound
and in-memory -- no awaits.  When planning needs the syscache (e.g.
to look up `customers` and `orders` metadata), we hit the first
interesting xtc primitive: **`xtc_rcu`**.

```c
/* relcache.c style, on xtc */
const pg_class_t *rel = NULL;
xtc_rcu_read_begin(&relcache);
    rel = relcache_lookup(rel_oid);    /* wait-free read */
    if (rel == NULL) rel = miss;        /* caller will fault in */
xtc_rcu_read_end(&relcache);
```

No wait, no fiber switch.  If invalidation arrives concurrently
from another backend, the writer side of the RCU swaps in a new
snapshot at the next safe epoch.

If the relcache misses, we have to load the catalog tuple, which
is an I/O.  That's a `dispatch()`:

```c
pg_class_t out;
int rc = xtc_call(syscache_load,
                  &(syscache_req_t){.oid = rel_oid},
                  &out, /*timeout_ns=*/250 * 1000 * 1000);
```

`xtc_call` is the wterl-style sugar: it's a `xtc_oneshot` channel
backed by an internal worker pool that handles syscache I/O.
While we wait, the reactor runs other sessions.

### 14.5 The big read: io_uring batched, dependent reads chained

Now the executor walks the plan.  On `orders`, the seqscan needs
N heap pages.  Old PG would issue `read()` per page (or use the
new AIO with `pgaio_io_acquire` / `PgAioWaitRef`).  On xtc the
seqscan declares an **`xtc_io_stream`** (analogue of PG's
`read_stream.c`):

```c
xtc_io_stream_t *s = xtc_io_stream_open(orders_smgr, blkno_lo, blkno_hi,
                                        XTC_IO_DIRECT | XTC_IO_PREFETCH);
for (;;) {
    xtc_io_buf_t *buf;
    int rc = await(xtc_io_stream_next(s, &buf)); /* parks if no buf ready */
    if (rc == XTC_E_EOF) break;
    process_page(buf);
    xtc_io_buf_put(buf);                          /* return to pool */
}
xtc_io_stream_close(s);
```

Under the hood:

- The stream maintains a sliding window of `prefetch_depth`
  outstanding io_uring SQEs.
- Each `xtc_io_stream_next` is `await`-only when the window is
  drained -- most calls return immediately because the next
  buffer is already complete.
- On a 32-reactor box, eight other sessions on the same reactor
  can be making the same calls; their fibers interleave
  *naturally* on this reactor as their CQEs arrive.  No thread
  context switch: just fiber switches at ~30 ns each.
- If io_uring is unavailable (configure-time), the same code
  compiles unchanged but `xtc_io_submit` posts to a worker pool
  that does blocking pread -- the PG `method_worker.c` shape,
  running as ordinary `xtc_proc`s.

### 14.6 Locking the rows we're going to read

For the FROM clause we need shared (predicate) locks on the heap
pages we read.  Each page lock goes through the lock manager:

```c
xtc_lock_h_t h;
int rc = await(xtc_lock_get_async(
                bk->locker,
                &(xtc_lock_key_t){.oid = orders_oid, .page = blkno},
                XTC_LOCK_R,
                XTC_LOCK_F_NOWAIT, &h));
```

In the common case (no contention) `xtc_lock_get_async` completes
synchronously; `await` returns without parking.  Under contention
the request enters the wait list, the incremental deadlock
detector adds the edge `(bk->locker -> holder)`, and if the cycle
search is clean the fiber parks on a `xtc_notify` attached to the
lock.  When the holder releases, the notify fires, the reactor
reschedules the waiting fiber, and `await` returns.  If the cycle
search found a deadlock, the policy picks a victim; the chosen
victim receives a mailbox kill envelope and unwinds at its next
`XTC_CHECK_CANCEL()`, releasing all its locks; this backend then
acquires.  If we *are* the victim, `await` returns
`XTC_E_DEADLOCK` and the txn-runner retries the whole transaction.

Aggregation, sort, limit -- all CPU.  `xtc_yield()` is sprinkled at
reduction-counter checkpoints inside the executor so a long
aggregation cooperates with other backends on the same reactor.

### 14.7 Sub-query for the UPDATE: an `async()` value

The UPDATE's `IN (SELECT customer_id FROM recent_logins)` doesn't
depend on the outer plan, so the planner emits it as a speculative
async subquery:

```c
xtc_task_t *t_subq = async(exec_subquery_recent_logins(plan->subq));
/* ... outer setup, scanning customers table head ... */
int64_t *ids; size_t nids;
await_into(t_subq, &ids, &nids);   /* await with out-params */
```

With fiber-mode `async()` ((S)12.1) this looks just like the
user's sketch: a value we kick off, do other work, then await.
The subquery runs as its own fiber on the same reactor; the
executor never blocks on it.

### 14.8 The UPDATE: write locks, WAL, fsync

For each row to update the executor takes `XTC_LOCK_W` on the
heap page.  If we already have `XTC_LOCK_R` on that page from
the SELECT scan above, we **promote**:

```c
rc = xtc_lock_promote(bk->locker, h, XTC_LOCK_W);
```

The promotion may deadlock with another backend that also holds R
and wants to promote.  The detector catches this immediately on
the wait edge insertion ((S)13.6).

WAL writes use `xtc_io_uring`'s linked SQE feature (or its
emulation on epoll/IOCP): one SQE for the WAL write, a linked SQE
for the fsync, with the completion not delivered until both
retire:

```c
xtc_io_op_t ops[2] = {
    { .kind = XTC_IO_PWRITE, .fd = wal_fd, .buf = wal_rec, ... },
    { .kind = XTC_IO_FSYNC,  .fd = wal_fd, .link_to_prev = true },
};
int64_t lsn;
await(xtc_io_submit_chain(ops, 2, &lsn));
```

While this fiber is parked waiting for the fsync, the same
reactor handles I/O completions for *every other backend on that
reactor.*  This is the entire point.

### 14.9 COMMIT: arena drop, lock release, reply

```c
/* commit: drop the txn arena; release all locks; reply OK */
xtc_lock_release_all(bk->locker);   /* fast path: walk per-locker list */
xtc_alloc_ctx_delete(bk->txn_ctx);  /* one call frees the entire txn's memory */
await(xtc_io_submit(&send_ok_op));
```

At this point the backend mailbox is empty, the proc is idle,
fiber stack is recycled to the per-reactor slab, txn arena is
recycled, lock-list cells are recycled.  No allocator calls
except slab pops.

### 14.10 What if a piece of the world is down?

Three scenarios show how the orchestration layer earns its keep.

1. **Backend crashes** (NULL deref in a UDF, or guard-page stack
   overflow).  `SIGSEGV` is caught on the per-reactor
   `sigaltstack`.  The supervisor for the backend gets a
   `{'EXIT', backend_pid, segv}` message, applies its
   `simple_one_for_one` strategy, and a new clean `xtc_proc`
   accepts the next connection.  No other session affected
   because no global state was corrupted -- every per-session
   datum lived inside the dead proc's arena.
2. **A reactor's I/O backend stalls** (e.g. an io_uring CQE storm
   we can't drain in time).  Other reactors steal tasks from its
   run queue (Tokio shape).  If the stall exceeds
   `dd_suspicious_ms` the deadlock detector triggers a periodic
   sweep -- it won't be a deadlock, but the sweep surfaces the slow
   reactor in `xtc_log` for ops to see.
3. **The whole executor wedges.**  The two-process supervisor at
   the very top notices the executor heartbeat stop, kills the
   child, restarts it.  Connections drop; clients reconnect; we
   carry on.

### 14.11 What changed vs the process-per-backend world

For the same SQL query:

| Aspect | Process-per-backend PG | xtc-on-PG |
|---|---|---|
| Cost of a session | one fork, private memory map, ~1 MB resident | one `xtc_proc` (fiber + small struct), ~64 KiB resident |
| Cost of an `await` | not applicable (block in syscall) | ~30 ns fiber switch |
| Sessions per box at p99 SLA | hundreds | tens of thousands (target) |
| Where session state lives | scattered global variables | one `xtc_proc` user-data struct |
| How `BEGIN` allocates | `MemoryContext` | `xtc_alloc_ctx` (delegates to MemoryContext if PG-embedded) |
| ProcArray snapshot | LWLock-protected scan | `xtc_lrlock` wait-free read |
| Relcache lookup | LWLock + invalidation | `xtc_rcu` epoch read |
| Heap I/O | `pgaio_io_acquire` + `PgAioWaitRef` wait | `xtc_io_submit` + `await` |
| Page lock | LWLock (no DD) or heavyweight LM (own DD) | `xtc_lock_get_async` + incremental DD |
| WAL fsync | sync syscall | linked io_uring SQE chain, fiber parks |
| Deadlock detection | PG heavyweight LM, periodic | incremental (ns-scale) + periodic safety net |
| Cancellation | `CHECK_FOR_INTERRUPTS()` peppered in code | `XTC_CHECK_CANCEL()` at every await + abort-source for fast-track |
| Crash recovery | postmaster forks a new backend | `xtc_supervisor` restarts the proc |
| Memory cleanup on crash | `MemoryContextDelete(TopMemoryContext)` | proc arena drop -- same idea, smaller blast radius |

---

## 15. Observability, tracing, statistics

A database foundation that you can't see into is a database
foundation you can't operate.  The threaded model makes this
*harder*, not easier, than the process-per-backend world: you can
no longer attach `strace` to a backend and see one session's
syscalls.  We compensate by building observability into every
layer from day one.

Four things, all designed together so they share data structures:

### 15.1 Tracing (W3C Trace Context, OpenTelemetry-compatible)

Every `xtc_task` carries a 20-byte trace context (16-byte trace
ID + 8-byte span ID + 1-byte flags).  Context propagates
automatically:

- `async(EXPR)` and `xtc_proc_spawn` copy the parent's context
  into the child task and start a child span.
- `xtc_send`, `xtc_call`, `dispatch()` carry the context as a
  reserved envelope header.
- `xtc_io_submit` records an I/O span linked to the calling
  task's span, with start/end timestamps.
- Lock-manager waits, deadlock-detector cycles, supervisor
  restarts, and abort-source firings all become spans with
  the relevant attributes (`xtc.lock.mode`, `xtc.dd.victim`,
  `xtc.proc.exit_reason`).

The trace API is small:

```c
xtc_span_t s;
xtc_span_begin(&s, "plan.relcache_lookup",
               XTC_SPAN_KIND_INTERNAL, NULL);
xtc_span_set_attr_int (&s, "oid", oid);
xtc_span_set_attr_str (&s, "relname", relname);
xtc_span_end_ok (&s);
/* or: xtc_span_end_err(&s, XTC_E_NOTFOUND); */
```

Under the hood spans are small (cacheline-sized) records ring-
buffered per-loop and drained to an exporter task.  Sampling is
head-based by default (decide-once at the root) but tail-based
sampling is a configure-time option for environments that want
to keep traces only for slow or error queries.

**Pluggable exporters**, configure-time picked:

- `xtc_export_otlp` -- OTLP/gRPC or OTLP/HTTP to any
  OpenTelemetry collector.
- `xtc_export_jaeger` -- native Jaeger Thrift.
- `xtc_export_zipkin` -- Zipkin v2 JSON.
- `xtc_export_log` -- spans serialized to `xtc_log` for
  development.
- `xtc_export_null` -- spans built and discarded (still useful
  for the per-task histograms in 15.2 because span timings feed
  them).

### 15.2 Per-task latency histograms

Every task class has a name (`xtc_proc_opts_t.name`).  The
runtime maintains an HdrHistogram per `(task_class, span_name)`
pair, recording wall-clock duration of every span.  The
resolution is microseconds, range 1 us ... 1 hour, three
significant digits -- ~2 KB per histogram, recycled to a slab.

Queryable live:

```c
xtc_lat_snapshot_t snap;
xtc_lat_snapshot_take("backend.", &snap);
/* iterate snap.entries: name, count, p50, p95, p99, p99.9, max */
```

Exposed by:

- `xtc_stat_dump` (15.3) for one-shot inspection.
- The OTLP exporter (15.1) as histograms (`OTLP_HISTOGRAM`).
- A built-in Prometheus textfile exporter for ops who want it
  scraped (`xtc_export_prom`, configure-time).
- A built-in JSON-over-Unix-socket admin endpoint at
  `$XTC_RUNTIME_DIR/stat.sock` so `xtcadmin stat` works without
  network access.

### 15.3 `xtc_stat_*` -- the live introspection table

This is the `pg_stat_activity` analogue.  A single function:

```c
int xtc_stat_dump(xtc_stat_filter_t *f, xtc_stat_view_t **out);
void xtc_stat_view_free(xtc_stat_view_t *);
```

Returns a snapshot containing:

- Every `xtc_proc`: pid, name, state (running/runnable/parked/
  receiving/blocked-on-io/blocked-on-lock), current span, mailbox
  depth, memory consumption, reactor assignment.
- Every `xtc_loop`: id, run-queue length, deque length, recent
  steal counts, last poll latency, current task.
- Every `xtc_lock_mgr` shard: depth, waiter count, oldest
  waiter age, hottest object.
- The full `xtc_lock_dd_graph` (waits-for graph) at the moment
  of the snapshot.
- Top-N tasks by allocated memory, by spans-per-second, by CPU
  time consumed in the last `stat_window_ms`.
- Active supervisor children with restart counters.
- Channel depth and lag for every named channel.

The snapshot is a *consistent point-in-time view* assembled by
broadcasting a `STAT_DUMP_REQ` to every loop, awaiting all
responses, and merging.  Latency is bounded by the slowest
loop's poll cycle (sub-millisecond in steady state).  Read
cannot stall a loop -- each loop fills its slice from local
state without a global lock.

A companion command-line tool `xtcadmin` understands the snapshot
format and prints it as a `top`-style live view, an inspector for
one specific PID, or a wait-graph diagram.  This is the first
tool a production operator reaches for; treat it as part of the
product.

### 15.4 Flight recorder

Each loop maintains an always-on ring buffer of the last
`flight_recorder_events` events (default 64 Ki):

- Task scheduled / unscheduled / completed.
- I/O submission and completion.
- `xtc_send` and `xtc_recv`.
- Lock acquired / released / waited.
- Cancellation observed.
- Span begin / end.
- Allocation > `flight_alloc_threshold`.

Events are 32 bytes each (timestamp + event class + tagged
union).  ~2 MiB per loop at default size.  In a core dump or
live `xtc_stat_dump(..., .flight_recorder = true)`, the recorder
is the single most useful artefact for postmortem.

A crash dumper (registered as a SIGSEGV/SIGBUS/SIGFPE last-
resort handler on the alt-stack) writes the flight recorder of
every loop to `$XTC_RUNTIME_DIR/crash-<pid>.flt` before
re-raising.  Companion tool `xtcdump` prints, filters,
time-sorts.

### 15.5 USDT / eBPF probes

Every hot-path event has a USDT probe.  We ship a `xtc.d`
DTrace script and a `xtc.bt` bpftrace script as examples.
Probe points (selected):

- `xtc:::task_schedule(pid, loop_id, span_id)`
- `xtc:::task_complete(pid, loop_id, duration_ns)`
- `xtc:::lock_wait_begin(locker, key, mode)`
- `xtc:::lock_wait_end(locker, key, granted, wait_ns)`
- `xtc:::dd_cycle_detected(victim, cycle_len)`
- `xtc:::reactor_stall(loop_id, stall_ns, current_task)`
- `xtc:::io_submit(op, fd, span_id)` /
  `xtc:::io_complete(op, result, latency_ns)`
- `xtc:::send(from_pid, to_pid, msg_kind)`
- `xtc:::sup_restart(supervisor_pid, child_pid, reason)`

Probes are zero-cost when no consumer is attached (compiled to
NOPs via libstapsdt / SystemTap).  Required-on for Tier 1 builds.

### 15.6 Logging (`xtc_log`) revisited

(S)2.4.5 already specified `xtc_log` as a per-loop ring buffer with
a single drain task and a per-task `errcontext` chain.  Two
upgrades for production use:

- **Structured.** Every log record is a CBOR map; sinks decide
  whether to render as JSON, plain text, or journald native.
  No printf-string-search regressions.
- **Trace-correlated.** Every record carries the current
  trace/span ID.  In Grafana / Jaeger / your-tool-here you can
  jump from a span to its log lines and back.

Log levels: `DEBUG5` ... `DEBUG1`, `INFO`, `NOTICE`, `WARNING`,
`ERROR`, `FATAL`, `PANIC` (matches PG `elog`/`ereport`
severity).  Per-module levels at runtime via `xtc_cfg`.

### 15.7 Where this lives

```
src/ptc/obs/
|--- trace.c, trace_ctx.c           <- spans + W3C context
|--- export_otlp.c, export_jaeger.c, export_zipkin.c,
|   export_log.c, export_null.c, export_prom.c
|--- lat.c                          <- per-task HdrHistograms
|--- stat.c                         <- live snapshot
|--- flight.c                       <- flight recorder
|--- probes.h                       <- USDT probe declarations
\--- admin.c                        <- unix-socket admin endpoint

src/inc/xtc_obs.h                  <- public API
bin/xtcadmin/                      <- CLI
bin/xtcdump/                       <- crash-dump pretty-printer
```

Observability is a first-class subsystem on the same footing as
the lock manager.  It lands in **M3** (basic spans + `xtc_log`),
**M5** (cross-loop correlation), **M9** (`xtc_stat_dump`),
**M11** (HdrHistograms + flight recorder), **M14** (full
exporters + admin tools).

---

## 16. The blocking-call contract

The most dangerous bug in any reactor system is a task that
blocks the reactor.  One `getaddrinfo()`, one `pthread_mutex_lock`
on a heavily-contended pthread mutex, one `read()` from an NFS
mount that hangs -- and every other task on that reactor stalls.
In process-per-backend PG this never matters; one slow backend
doesn't affect the others.  In threaded PG it is *the* failure
mode.

We address it on three fronts: a documented contract, a runtime
escape hatch, and a stall detector with teeth.

### 16.1 The contract

> Code running on an `xtc_loop` must not call any blocking
> syscall, blocking library function, or non-cooperating lock.
> If a task must perform such an operation, it must enclose it
> in `xtc_block_in_place { ... }` or use `xtc_spawn_blocking`.

This is enforced three ways:

1. **Lint** (`dist/s_blocking`).  Greps for raw uses of
   `pthread_mutex_lock`, `pthread_rwlock_*`, `pthread_cond_*`,
   `sem_wait`, `usleep`, `sleep`, `nanosleep`, `select`,
   `poll`, `epoll_wait`, `recv`, `send`, `read`, `write`,
   `pread`, `pwrite`, `fsync`, `getaddrinfo`, `gethostbyname`,
   `dlopen`, `flock`, `fcntl(F_SETLKW)`, etc., outside `os/`,
   `compat/`, and explicitly tagged escape blocks.  Failures
   block the build.
2. **Annotation.**  Functions known to block carry
   `XTC_BLOCKING` in their declaration; the lint understands
   this and demands an escape block at every call site.
3. **Runtime detection** -- the reactor-stall detector (16.4).

### 16.2 `xtc_block_in_place` -- in-place escape

For short blocking sections inside a task that's mostly
cooperative:

```c
int result;
XTC_BLOCK_IN_PLACE {
    /* we are now on the blocking pool, NOT the reactor */
    result = legacy_sync_call(args);
}
/* back on the reactor */
use(result);
```

Mechanics: the macro saves the calling fiber's reactor
assignment, hands the fiber off to a *blocking-pool worker
thread*, executes the block, then re-pins the fiber back to its
original reactor (or another, by least-loaded policy).  The
original reactor continues running other tasks while the block
is in flight.  Cost: two fiber switches plus a context-handoff;
on the order of microseconds.  The block must not call any
`xtc_*` API itself except `xtc_log` and `xtc_alloc_*` -- this is
linted.

### 16.3 `xtc_spawn_blocking` -- dedicated blocking task

For longer-running blocking work (compression of a large blob,
parsing of a multi-MB SQL string, calling into an FDW that does
blocking RPC):

```c
xtc_task_t *t = xtc_spawn_blocking(do_legacy_work, &args);
/* ... do something else on the reactor ... */
int rc = await(t);
```

Under the hood the new task lives on the blocking pool from
creation to completion.  `await(t)` from the reactor side is
`xtc_future_await` and parks the calling fiber as usual; no
special coordination.

### 16.4 The blocking pool

A distinct pool of OS threads (default `min(__os_ncpus() * 2,
128)`, configurable via `blocking_pool_size`).  Threads are
real pthreads with full kernel scheduling -- unlike reactor
threads they may block, hold pthread mutexes, do synchronous
syscalls.

Governance:

- A bounded global queue feeds the pool; full queue makes
  `xtc_spawn_blocking` either park the caller (default) or
  return `XTC_E_RESOURCE` (`block_pool_full=error`).
- Pool threads are themselves observable via `xtc_stat_dump`
  (one row per worker, current task, current call site).
- A separate `priority_blocking_pool` for latency-critical
  blocking work (WAL fsync, etc.) so a noisy FDW can't starve
  durability operations.

### 16.5 Reactor-stall detector

Every reactor's main loop records a wall-clock timestamp before
calling each task's continuation and again after.  A *watchdog
task* on a separate pthread (the same one that drives
`os_signalfd`) wakes every `stall_check_ms` (default 50 ms) and
compares each loop's last-update timestamp against now.  If a
loop has been in one task for more than `stall_threshold_ms`
(default 100 ms) the watchdog:

1. Emits a `reactor_stall` USDT probe (15.5).
2. Logs a WARNING with the loop ID, the offending task name,
   the current span, and a backtrace (libbacktrace) of the
   stalled fiber's stack.
3. Increments `xtc_stat.reactor.stalls_total`.
4. If `stall_action=panic` (default off), aborts the executor
   so a supervisor restart provides clean state.
5. If `stall_action=steal` (default on), other reactors
   immediately start aggressive stealing from the stalled
   loop's deque so the *other* tasks on that loop continue
   running on healthy reactors.

This is what catches the bugs the lint missed.  In production
the alert ("reactor stall") is a P1 page; the log line names
the culprit.

### 16.6 `xtc_compat_pthread.h` -- transparent compat for legacy code

An opt-in header that `#define`s pthread primitives to xtc-aware
shims:

- `pthread_mutex_lock(m)` -> try-fast-path; if blocked, switch
  to the blocking pool, lock, switch back.
- `pthread_cond_wait(c, m)` -> same; the wait is on the blocking
  pool with the reactor freed.
- `usleep(us)` -> `xtc_sleep_us(us)` (timer-wheel based).
- `read/write/recv/send/...` on a fd we recognize -> route
  through `xtc_io_submit` instead of blocking.

Including this header in legacy code (a contrib module, an
FDW, glue from a vendored library) makes most of it
*automatically* cooperate with the reactor without a rewrite.
The edge cases that don't (e.g. code that does `pthread_create`
itself, or that takes a pthread mutex in a destructor) still
need manual conversion, but the bulk is mechanical.

This header is the single biggest win for porting existing code
bases.  It deserves its own `docs/compat-pthread.md`.

### 16.7 Where this lives, milestone

```
src/ptc/block/
|--- in_place.c                    <- XTC_BLOCK_IN_PLACE
|--- spawn_blocking.c              <- xtc_spawn_blocking
|--- pool.c                        <- blocking pool worker threads
|--- stall.c                       <- reactor-stall detector
\--- compat_pthread.c              <- the shim implementation

src/inc/xtc_block.h, xtc_compat_pthread.h
dist/s_blocking                   <- the lint
```

Lands in **M5** (basic stall detector with the multi-loop
executor), **M9** (`xtc_block_in_place` + `xtc_spawn_blocking` +
blocking pool), **M14** (`compat_pthread.h` + the lint at full
strength).

---

## 17. Hooks framework

Extensions are how a database stays alive across decades.
PostgreSQL's success owes as much to its hook surface (executor
hooks, planner hooks, `ProcessUtility_hook`, `ClientAuthentication_hook`,
~30 others) as to its core engine.  The current design is
gloriously simple: a global function pointer, set by an
extension's `_PG_init`, called by the core.  In a threaded
world this design is incorrect at every level: globals
race, ordering between extensions is undefined, an extension's
crash takes everything down.

We replace it with a typed, versioned, threadsafe hook framework
that is *the* mechanism by which the runtime is extended over
time -- see also (S)18 (Longevity) for why this matters for
change management.

### 17.1 Concept

A **hook** is a named, typed extension point with a defined
signature, a defined ordering policy, and a defined termination
policy.  Extensions register handlers; the runtime calls the
chain.  Examples (for L5 PG):

- `pg.executor.run` -- called per query, decorate-style (each
  handler may modify the running state, all are called).
- `pg.client.auth` -- called per connection, stop-on-first
  (first handler that returns `XTC_AUTH_OK` or `XTC_AUTH_DENY`
  decides).
- `pg.utility.run` -- called per `ProcessUtility`, replace-style
  (a handler may consume the call, stopping the chain).
- `pg.planner.optimize` -- decorate-style.
- `xtc.proc.spawn` -- called when any `xtc_proc` is spawned;
  observe-only (handlers cannot modify, used by tracing).

Extensions use the same machinery for their own internal hooks.

### 17.2 Public API

```c
/* Define a hook.  Done once at runtime startup or by an extension. */
int xtc_hook_define(const char *name,
                    const xtc_hook_sig_t *sig,
                    xtc_hook_policy_t policy,
                    xtc_hook_h_t *out);

/* Register a handler.  priority orders handlers; lower runs earlier. */
int xtc_hook_register(xtc_hook_h_t hk,
                      const char *owner,         /* extension name */
                      uint16_t version,          /* extension SemVer minor */
                      int32_t priority,          /* 0 = default */
                      xtc_hook_fn_t fn,
                      void *user_data,
                      xtc_hook_reg_t *out);

/* Unregister.  Safe under contention; the chain uses RCU. */
int xtc_hook_unregister(xtc_hook_reg_t reg);

/* Invoke the chain.  args/result types must match the hook signature. */
int xtc_hook_call(xtc_hook_h_t hk, void *args, void *result);

/* Introspection. */
int xtc_hook_list(xtc_hook_h_t hk, xtc_hook_info_t **handlers, int *n);
```

Policies:

- `XTC_HOOK_DECORATE` -- all handlers run, in priority order; each
  may mutate `args` and `result`.
- `XTC_HOOK_REPLACE` -- handlers run until one returns
  `XTC_HOOK_CONSUMED`; the rest are skipped.
- `XTC_HOOK_OBSERVE` -- all handlers run; mutations to `args`
  /`result` are dropped by the framework (defensive copy in
  debug builds).
- `XTC_HOOK_FIRST_OK` -- handlers run until one returns
  `XTC_OK`; result is that handler's result.

### 17.3 Hook chains use RCU

The handler list is an `xtc_rcu`-protected array.  Calls are
wait-free; registration/unregistration costs an epoch.  This
is a primary consumer of `xtc_rcu` ((S)2.4.4 / M13a) and one of
the reasons RCU is mandatory rather than optional.

### 17.4 Crash isolation

Handler invocation is wrapped in:

- An `xtc_abort_source` so a misbehaving handler can be
  cancelled if the call exceeds `hook_handler_timeout_ms`
  (default 5 s).
- A `setjmp`/`longjmp` perimeter for `XTC_HOOK_OBSERVE`
  handlers -- a `longjmp` out of an observe handler is caught
  and turned into a logged error rather than corrupting the
  caller.
- A try/catch for `SIGSEGV` etc. when `--with-handler-fault-
  isolation` is enabled (Linux only, uses signalfd +
  process-wide handler).  An observe handler that segfaults
  is *unregistered* and the call continues; a decorate or
  replace handler that segfaults aborts the call with
  `XTC_E_HOOK_FAULT`.  In either case the supervisor sees a
  WARNING, not a crash of the executor.

### 17.5 Versioning and capability negotiation

Each hook signature is versioned:

```c
/* xtc/inc/hooks/pg_executor_run.h -- generated */
#define PG_EXECUTOR_RUN_HOOK_NAME    "pg.executor.run"
#define PG_EXECUTOR_RUN_HOOK_SIG_V1  { ... arg/return shapes ... }
#define PG_EXECUTOR_RUN_HOOK_SIG_V2  { ... v2 with extra field ... }

/* runtime declares which versions it understands */
static const xtc_hook_sig_set_t pg_executor_run_supported = {
    .versions  = { PG_EXECUTOR_RUN_HOOK_SIG_V1, PG_EXECUTOR_RUN_HOOK_SIG_V2 },
    .preferred = 2,
};
```

An extension built against v1 still registers cleanly against
a runtime that supports v2; the framework presents the v1 view
to the handler, fills in defaults for the new fields.  When the
runtime drops v1 (per the deprecation policy in (S)18) the
extension fails to register with a clear error pointing at the
rebuild instructions.

### 17.6 Per-extension accounting

`xtc_hook_register` takes the extension name; the framework
attributes:

- CPU time spent in this extension's handlers (via the span
  framework, 15.1).
- Memory allocated by this extension (via a per-extension
  `xtc_alloc_ctx`).
- Number of hook faults / timeouts.

Queryable via `xtc_stat_extensions()`.  Lets ops disable a
slow extension based on data, not folklore.

### 17.7 Bridging to PG hooks

For every PG hook (`ExecutorRun_hook`, `planner_hook`, etc.) the
L5 adapter:

1. Defines an `xtc_hook_h_t` with a typed signature.
2. Implements the legacy `_hook` global as a thin shim that
   calls `xtc_hook_call` with the right policy.
3. Provides `XTC_HOOK_TO_PG_*` macros so existing extensions
   that set the global pointer keep working through a
   compatibility shim until they are migrated.

This is one of the two killer features of L5 (the other being
the AIO subsumption from (S)2.6).  It lets us deliver the
hardening (priority, ordering, isolation, accounting) *without*
breaking every existing extension on day one.

### 17.8 Where this lives, milestone

```
src/ptc/hook/
|--- hook.c                        <- define / register / call / unregister
|--- hook_chain.c                  <- RCU-protected handler arrays
|--- hook_isolate.c                <- timeout + abort-source + fault catch
|--- hook_version.c                <- signature negotiation
\--- hook_account.c                <- per-extension accounting

src/inc/xtc_hook.h
src/inc/hooks/                    <- typed signature headers, generated
dist/s_hooks                      <- generates hooks/*.h from *.in declarations
```

Lands in **M9** (basic API + decorate/replace/observe policies)
and **M13a** (RCU integration + isolation + versioning).  The PG
hook bridging is **M16**.

---

## 18. Longevity and change management

The library has to live for decades.  Postgres is 28 years old
and counting; xtc as its underpinning needs the same horizon.
This section is the *contract* the project makes with its users
about how it will evolve, and the mechanism by which we honour
it.

Four pillars: **stable surface**, **mechanical change**,
**ratchets that only tighten**, **deprecation with deadlines**.

### 18.1 SemVer and ABI promise

Versioning is `MAJOR.MINOR.PATCH`:

- **PATCH** (`1.4.x`).  Bug fixes only.  Same ABI.  Same on-
  disk format.  Same wire format.  Same tracing-span shape.
  Same lint surface.  Drop-in.
- **MINOR** (`1.x.0`).  New features, new APIs, new hooks.
  ABI is *additive only*: nothing removed, nothing renamed,
  nothing changes in behaviour for code compiled against the
  prior minor.  New `XTC_E_*` codes only at the end of the
  enum; existing codes never change value.  New `xtc_cfg`
  knobs.  New lock modes never inserted in the middle of the
  enum.
- **MAJOR** (`x.0.0`).  Breaking changes allowed.  Cadence is
  intentionally slow -- we target one major every 3-5 years.
  An LTS designation on the previous major is committed for
  at least 18 months past the new major's release.  Migration
  guide and `xtc-migrate-1to2` tool ship with the major.

ABI stability is enforced by **symbol versioning** (`.symver`)
on platforms that support it (Linux glibc, Solaris, FreeBSD).
On Windows, by stable ordinals + a never-rewritten `xtc.def`.
On macOS, by careful curation of exported symbols + a
`-current_version` / `-compatibility_version` matched to SemVer.

A `dist/s_abi` checker runs on every release tag:

- Reads the previous tag's exported-symbol set.
- Diffs against the current build.
- Removed or renamed symbols on a non-major bump fails the
  release.
- Changed function signatures on a non-major bump fails the
  release.
- New symbols on a patch bump fails the release.

This catches the entire class of "oops we broke ABI in a
patch" mistakes that have plagued every C library ever.

### 18.2 Capability bits and feature flags

Applications never check version numbers.  They check
*capabilities*:

```c
if (xtc_have_capability("io_uring")) { ... }
if (xtc_have_capability("hooks.v2")) { ... }
if (xtc_have_capability("lock.intent_modes")) { ... }
```

Capabilities are strings declared in `dist/capabilities.in` and
compiled into the binary.  Adding a capability is a **minor**
bump; removing one is a **major** bump.  Capabilities are
exposed to extensions through the hook-registration return
value and to operators through `xtc_stat_dump`.

Why capabilities and not feature macros?  Macros are
compile-time and assume the library you ship against is the
library you run against.  Capabilities work even when xtc is a
shared library swapped under the application without a
recompile -- the precise scenario for long-lived servers.

### 18.3 Mechanical change as doctrine

Structural change to the codebase happens through `dist/`
tools, not by hand.  This is the BDB / DBSQL discipline turned
up to eleven.  The full set of generators:

| Tool | Generates / enforces | Why mechanical |
|---|---|---|
| `dist/s_include` | `inc/*_ext.h` from `PUBLIC:` markers | Function prototypes can't drift from definitions |
| `dist/s_async`   | `xtc_async_decls.h` typed wrappers | Async dispatch is type-checked at compile time |
| `dist/s_cfg`     | `xtc_cfg_ids.h`, GUC name/type registry | Config is registered, validated, enumerable |
| `dist/s_hooks`   | `hooks/*.h` typed signature headers | Hooks are versioned, can't be redefined silently |
| `dist/s_globals` | Lint: forbid bare `static`/`_Thread_local` outside `os/` | No new shared globals slip in |
| `dist/s_signals` | Lint: forbid raw signal API outside `os_signal.c` | Signal discipline is enforced |
| `dist/s_blocking`| Lint: forbid raw blocking syscalls outside `os/`+`compat/` | The blocking-call contract ((S)16) is enforced |
| `dist/s_noalloc` | Lint: forbid allocations in `XTC_NOALLOC` files | Hot path stays allocation-free |
| `dist/s_meson`   | `meson.build` from `srcfiles.in` | Build files can't diverge between autoconf and meson |
| `dist/s_abi`     | `xtc.symver` from `pubdef.in`; ABI diff at release | ABI promise ((S)18.1) is mechanically enforced |
| `dist/s_doc`     | Reference docs from source | Docs can't drift from code |
| `dist/s_migrate` | Migration scripts when a major bumps | Rewrites in user code are scripted |
| `dist/s_tags`    | ctags/etags | Editor navigation works |

**The doctrine:** if you find yourself making the same change
in twelve files, write a `dist/s_*` tool instead.  Every script
is documented at the top with its inputs, outputs, and
correctness criterion.  Every script has a unit test in
`test/dist/`.  A change to a generator is a structural change
to the codebase reviewed with the same care as a change to the
event loop.

### 18.4 Ratchets that only tighten

Four **ratchet files** in `dist/ratchets/`:

- **`globals.txt`** -- the set of legacy bare `static`/
  `_Thread_local` declarations grandfathered in.  Lints fail if
  any new ones appear; PRs that *remove* lines are encouraged.
  Goal: file size monotonically decreases to zero.
- **`coverage.json`** -- line and branch coverage per file.
  CI fails if a file's coverage drops below the recorded
  number.  The recorded number is updated only when a PR
  *raises* coverage.
- **`p99.json`** -- per-bench p99 latency.  CI fails if any
  bench regresses by more than `regression_budget` (default
  5%).  Recorded numbers update on tagged releases only.
- **`alloc.txt`** -- the maximum allocations per task in each
  worked-example test.  Ratchet down only.

These files are committed.  They are *the* mechanism that
prevents slow rot.  In ten years, the globals ratchet will
either be empty (we won) or have a clear list of legacy
shame (we know what to fix).

### 18.5 Deprecation lifecycle

A five-stage lifecycle for any API we want to remove.  Each
stage is one minor release at minimum:

| Stage | Behaviour | Compiler/runtime signal |
|---|---|---|
| 1. Live | Documented, supported. | Nothing. |
| 2. Soft-deprecated | Documented, supported. | `XTC_DEPRECATED_SOFT` attribute -> compiler note. Doc note. |
| 3. Deprecated | Supported, discouraged. | `XTC_DEPRECATED` attribute -> compiler warning.  `xtc_cfg.warn_deprecated` (default `true`) logs runtime use. |
| 4. Default-off | Compiles only with `-DXTC_ENABLE_DEPRECATED`.  Runtime behaviour unchanged. | Build error without the flag. |
| 5. Removed | Header `#error`'d; symbol absent. | Build error always.  Migration tool referenced in error text. |

Minimum total span: 5 minor releases (~2 years at our intended
cadence).  Documented on the wiki per API.  Every removed
function has a documented replacement.

### 18.6 Compat test suite -- the past keeps working

`test/compat/` contains compiled-and-runnable copies of every
worked example from every prior 1.x release.  CI builds them
against the *current* source.  They must continue to compile
and pass.  This is the strongest possible statement of "we
meant it about ABI stability."

When a major bump happens, the suite forks: `test/compat/1.x/`
freezes; `test/compat/2.x/` starts populating.  The 1.x suite
stays in CI for the duration of the LTS commitment.

### 18.7 Trace-shape stability

An often-overlooked dimension of "compat": dashboards,
alerts, and runbooks bind to span names and attributes.  We
commit to:

- Span names are stable across minors.  Renaming is a major
  change.
- New attributes may be added in a minor.  Removing or
  renaming attributes is a major change.
- New span kinds (new hooks, new subsystems) may be added in
  a minor.

A `test/trace_compat/` suite captures the span shape from a
set of canonical workloads at each release tag and diffs
against the current build.  Diffs without a `RELEASE.md`
entry justifying them block the release.

### 18.8 On-disk and wire formats

Though xtc itself doesn't define on-disk database formats (PG
does), it defines:

- The flight-recorder dump format (`*.flt`).
- The crash-dump trace format.
- The `xtc_stat_dump` snapshot format.
- The `xtcadmin` admin-socket protocol.

Each has a **format version byte** in its first byte and a
documented per-version layout.  The `xtcdump` tool reads every
format version we ever shipped, forever.  No flag day.

### 18.9 Documentation that cannot drift

- Reference manual generated by `dist/s_doc` from `PUBLIC:`
  comment blocks (BDB style).
- Every public function's doc comment carries an
  `XTC_AVAILABLE_SINCE: 1.x` tag.  `s_doc` builds a
  "what's new in 1.x" page automatically.
- ADRs (`docs/adr/`) capture every Q-decision (Q1-Q16 of
  (S)10 plus future ones).  Format: rationale, alternatives,
  consequences, status.  Immutable once accepted; superseded
  by a new ADR that links back.
- Every removal in stage 5 ((S)18.5) has its ADR updated to
  status `Superseded`.

### 18.10 Long-term support and the maintainer model

We commit (informally for now, formally once we hit 1.0) to:

- Each MAJOR has at least 18 months LTS past the next MAJOR.
- Security fixes are backported to all in-support releases.
- An `xtc-security@` list with a documented embargo policy.
- Coordinated disclosure with PG's security list when an issue
  affects threaded PG.

This is what makes a serious downstream consumer (say,
PostgreSQL itself) willing to depend on xtc.  Without it the
dependency is a liability.

### 18.11 Where this lives, milestone

```
dist/
|--- ratchets/
|   |--- globals.txt, coverage.json, p99.json, alloc.txt
|--- capabilities.in
|--- pubdef.in                    <- single source of API symbols
|--- s_abi, s_doc, s_migrate, ...
docs/
|--- adr/                         <- architecture decision records
|--- deprecation.md
|--- abi-stability.md
\--- compat-pthread.md
test/
|--- compat/                      <- frozen examples per release
\--- trace_compat/                <- span-shape diff tests
```

The machinery lands gradually, but the *commitments* are
made in **M0** (the README and `docs/abi-stability.md` ship
with the empty repo) so users know what they're buying into
before the first release.

---

## 19. Known gaps and roadmap

Things the design has identified but not fully specified.
Each has a target milestone; none blocks M0-M3.  This list
lives in the plan rather than the issue tracker because the
gaps are *architectural*: how we close them affects the
shape of the surrounding code.

### 19.1 Networking depth (M7 / M9 / M14)

- **TLS / mTLS** via configure-time-picked OpenSSL /
  BoringSSL / wolfSSL / SChannel / macOS Network framework.
  An `xtc_tls_t` over `xtc_io`.  M9.
- **DNS resolver.**  Sync `__os_getaddrinfo` (in the blocking
  pool) plus a cancellable async resolver (c-ares-shaped).
  M9.
- **Connection pool** (`xtc_pool`) for backend-to-backend.
  M14.
- **Unix domain sockets** with credential passing
  (`SO_PASSCRED`, `getpeereid`, Windows pipe equivalent).
  M7.
- **Accept scaling.**  `SO_REUSEPORT` accept distribution,
  BSD accept filters, Linux `SO_INCOMING_CPU` to pin
  accepted connections to the accepting reactor.  M7.
- **TCP knobs.**  `TCP_NODELAY`, `TCP_CORK`, keepalive,
  `TCP_USER_TIMEOUT`, congestion algorithm selection.
  Documented in `os_net.c`.

### 19.2 Cryptography building blocks (M14)

`__os_crypto_*` shim over:

- AES-GCM, ChaCha20-Poly1305 (auth-encrypt for TDE).
- SHA-2 family, SHA-3, BLAKE3 (hashing for checksums and
  WAL).
- HMAC, HKDF.
- A `__os_csprng_t` per-loop ChaCha20 streaming RNG seeded
  from `__os_rand_bytes` and reseeded per-cycle.

Backed by libsodium / OpenSSL / BoringSSL / SChannel /
CommonCrypto.

### 19.3 Time virtualization for tests (M3)

A `xtc_clock_t` opaque object.  In production the singleton
`__xtc_real_clock` reads `__os_clock_mono`; in tests a
`xtc_test_clock` lets the test advance time deterministically
(`xtc_test_clock_advance(ns)`).  Every timer-using primitive
(timer wheel, deadlock-detector intervals, retry backoff,
stall detector) reads through the injected clock.  This
lands with M3 because every later milestone's tests benefit.

### 19.4 Fault injection framework (M5)

`xtc_fi_*`:

- Failure points named by `dist/s_fi` like `dist/s_async`
  parses calls.
- Per-test activation: "fail allocation N at site X with
  probability p".
- Latency injection: "delay every io_uring CQE by U(0, 100
  us)".
- Drop injection: "drop every Nth message on channel C".

Enabled by configure flag `--enable-fault-injection`; compiled
out of release builds by default.  Lands in M5 because the
multi-loop work is the first thing that *needs* it.

### 19.5 Sagas / multi-step coordination (M14)

A small library on top of `xtc_future`:

```c
xtc_saga_t s; xtc_saga_init(&s);
xtc_saga_step(&s, do_step1, compensate_step1, ctx);
xtc_saga_step(&s, do_step2, compensate_step2, ctx);
xtc_saga_step(&s, do_step3, compensate_step3, ctx);
int rc = await(xtc_saga_run(&s));   /* runs forward; on failure, runs compensations in reverse */
```

Not strictly needed for v1; right abstraction for replication
coordination, distributed transactions, complex extension flows.
M14.

### 19.6 Priority inheritance through the lock manager (M13c)

When a low-priority locker holds a lock that a high-priority
locker needs, the holder temporarily inherits the requester's
priority for the duration.  The `xtc_lock_dd` graph is the
structure we need; the priority is on the `xtc_proc`.  Wire it
through `xtc_lock_get_async`'s wait path.  M13c.

### 19.7 Backpressure protocol (M9)

Beyond bounded channels:

- High/low watermarks on every bounded queue.
- Reactive-streams `request(n)` credit protocol on
  `xtc_chan_*`.
- Admission control at the connection-accept layer:
  `xtc_admission_t` that the listener consults; rejects with a
  documented error code and a Retry-After-style hint.
- Per-class CPU shares for the data plane vs admin plane.

### 19.8 LLVM JIT (M16)

PG uses LLVM for expression JIT.  `LLVMContext` is *not*
thread-safe; IR modules can't move between contexts.  L5
provides `xtc_jit_ctx` per-loop with IR caching keyed on the
plan tree shape.  Cache eviction is per-loop LRU.  Cross-loop
migration of a JIT-using task forces re-JIT (or, optionally,
bouncing to a JIT-pinned reactor).  M16.

### 19.9 Replication (walsender/walreceiver, logical, slots) (M16)

Each is a long-running supervised `xtc_proc`:

- `walsender` per replica connection.
- `walreceiver` for standby ingestion.
- Logical decoding as an `xtc_io_stream` over WAL.
- Synchronous-standby commit coordination via `xtc_barrier`.
- Slot management as an `xtc_lrlock`-protected catalog.

### 19.10 Parallel query workers (M16)

Leader-worker cohort: leader is the user `xtc_proc`, workers
are children spawned by `xtc_proc_spawn` with shared
`xtc_alloc_ctx` for tuple-store handoff.  Tuple queues are
`xtc_chan_mpsc`.  Crash of a worker is observed via monitor;
leader cancels the rest via `xtc_abort_source`.  This warrants
its own (S)14-style worked example in `docs/parallel-query.md`
at M16.

### 19.11 `pgstat` / activity monitoring (M16)

An `xtc_svr` (gen_server) consumer of stat-delta messages from
backends.  Snapshots produced by `xtc_stat_dump` at the xtc
layer; PG-specific stats layered on top.  Maps the existing
`pg_stat_*` views to xtc snapshot output.  M16.

### 19.12 Autovacuum (M16)

A supervisor with a pool of vacuum-worker `xtc_proc`s, each
holding a worktable lock for an extended period.  Excellent
stress test for the lock manager and the priority-inheritance
implementation.  M16.

### 19.13 Foreign data wrappers (M16)

The canonical "makes blocking C calls into other databases"
case.  FDWs *must* live in the blocking pool ((S)16) or yield
through a documented async API.  An `xtc_fdw_t` interface that
makes the choice explicit in the FDW author's contract.  M16.

### 19.14 TOAST / large object streaming (M16)

Streaming tuple support; touches memory contexts, the I/O
streamer, the lock manager, and backpressure.  Not viable as
"all-at-once" in a threaded server with thousands of sessions.
M16.

### 19.15 Container / cgroup awareness (M0)

`__os_ncpus()` reads cgroup v2 `cpu.max` first, falling back to
`/proc/cpuinfo`.  `__os_mem_max()` reads `memory.max`.  OOM
cooperativity reads `memory.events`.  Default reactor count and
default per-proc memory cap are computed from these.  Without
this, defaults are wrong on every Kubernetes deployment.  Ships
in **M0** because it affects every test run on CI containers.

### 19.16 Internationalization (M14)

ICU collation through L0; `gettext` for error messages
(extracted from `XTC_E_*` tables); IANA `tzdata` access via
`__os_tz_*`.  M14.

### 19.17 Sandboxing and security (M14)

seccomp profile generator (auto-derived from the syscalls our
`os_*.c` actually issues, plus a per-extension delta).
Landlock on Linux, pledge/unveil on OpenBSD, AppArmor/SELinux
context support, capability dropping helpers.  Address-sanitization
at process boundary (CFI, shadow stacks, Intel CET) where the
toolchain supports it.  M14.

### 19.18 Hash tables and other concurrent data structures (M13a/b)

- RCU-protected concurrent hash table (`xtc_chash`) --
  primary RCU consumer.  M13a.
- Concurrent skiplist (`xtc_cskip`, vendoring `~/ws/skiplist`).
  M13b.
- Bloom filter, HyperLogLog (small library; mostly numerical).
  M14.

### 19.19 Compositional property tests (M11)

Existing PBT plan covers per-primitive invariants.  Add a
`test/hegel/composition/` suite that draws random sequences of
`(spawn, send, recv, link, monitor, exit, send-during-recv,
lock-during-send, ...)` and checks system-wide invariants:
no orphaned procs, no dangling links, every monitor fires,
supervisor restart counts are correct.  M11.

### 19.20 Graceful shutdown protocol (M9)

Documented multi-stage drain orchestrated by `xtc_app_shutdown`:

1. Stop accepting new connections.
2. Send `'$xtc_shutdown'` to every `xtc_proc`.
3. Wait for `drain_deadline_ms` (default 30 s) for procs to
   exit.
4. Force-cancel via `xtc_abort_source` with `force_deadline_ms`
   (default 5 s).
5. Force-kill any survivors; supervisor logs them.
6. Flush WAL, close fds, exit.

Responds correctly to SIGTERM (graceful), SIGINT (graceful with
shorter deadline), SIGQUIT (immediate).  M9.

### 19.21 Power / kernel-tuning advisor (M14)

`xtc_app_start` runs a battery of cheap probes:

- `cpufreq` set to `performance`?
- `intel_pstate=disable`?
- `transparent_hugepage=madvise`?
- `vm.swappiness` low?
- `kernel.sched_autogroup_enabled` off (for latency)?
- io_uring not blocked by seccomp?

Each "off" emits a NOTICE with a recommendation.  Operators
don't want to hunt through tuning guides to learn their kernel
is fighting them.  M14.

### 19.22 IOCP-vs-readiness worked example (M6)

A documented translation table in `docs/io-models.md` showing
the same `xtc_io_op` pattern under epoll, io_uring, kqueue, and
IOCP, side by side, with a worked example per backend.  Lands
with M6 (the milestone that adds the non-Linux backends).

### 19.23 The `XTC_NOALLOC` discipline (M14)

Annotation on files where the hot path runs.  `dist/s_noalloc`
lint forbids any allocation symbol reference (malloc, palloc,
xtc_palloc, slab alloc) inside such files.  Files initially
annotated: `evt_loop.c`, `evt_deque.c`, `evt_timer.c`,
`io_uring.c`, `lock/lr.c`, `lock/lw.c`'s fast paths.

---

## 20. PostgreSQL multithreading roadmap -> xtc primitive map

This is the single most important section of the document for the
pitch.  Every item from the v20->v21 PG threading workplan has a
first-class xtc primitive ready (or planned).  The column on the
right is what xtc offers; the column in the middle is what the PG
workplan calls for.

| PG workplan item | What it asks for | xtc primitive |
|---|---|---|
| Thread per backend / socket / session | Per-connection async dispatch on a fixed thread pool | `xtc_proc_spawn` per connection; one process per session, mailbox-driven |
| `multithreading` GUC (on/off, runtime) | Build that supports both modes; switch at startup | `xtc_app` config flag (configure-time) + per-session adapter (runtime); see (S)2.6 |
| Cutover to threaded-only by v21 | Underpinning that is *production* by v20 | xtc M16 (PG adapter) targets v20 dev cycle; M13a/b/c (RCU + LRLock + lock manager) lands the new primitives in time |
| Heikki's TLS branch | Find globals via `__thread` annotations as verification harness | `XTC_TLS(type, name)` ((S)4.3); used as the verification harness, then converted to `XTC_PERLOOP` or moved into `xtc_proc` state |
| Session struct (long-term) | One struct holding all per-session state | `xtc_proc` already *is* the session struct: per-process arena, mailbox, name in `xtc_reg`.  Each PG subsystem's globals become fields on the `xtc_proc` user-data struct. |
| Function-static memory audit | Find/redact every `static T x = init;` in a function | `XTC_FN_STATIC` macro ((S)4.3) + `dist/s_globals` lint forbidding bare `static`/`_Thread_local` outside `os/` |
| Interrupt / signals / timers rewrite | Replace ad-hoc signals, get to latch-based interrupt model | (S)5.1: signals -> mailbox messages, `xtc_notify` is the latch, hierarchical timer wheel in `evt/` for all timers; `dist/s_signals` lint forbids raw signal use |
| Thread primitives abstraction (`port/pg_threads.h`) | Thin wrapper over C11/pthreads/Win32 for create/join/mutex/cond | L0 `os_thread.c` + `os_mutex.c` + `os_tls.c` -- *exactly* this |
| GUCs: function-call API replacing `&global` | `GetGUC<Type>`/`SetGUC<Type>` with type-dispatched store | `xtc_cfg` ((S)2.4.6): bool->sparsemap, int/enum->indexed array, string->hash; IDs cached at compile time by `dist/s_cfg` |
| LWLock review for threaded mode | Tranche IDs and partition arrays survive threaded contention | `xtc_lwlock` ((S)13); re-benchmarked in M13b/c under threaded-mode contention before promoting |
| RCU primitive (`pg_rcu.h`) | Read-mostly graph data, relcache/syscache | `xtc_rcu` ((S)2.4.4), epoch-based reclamation, wait-free readers |
| LRLock primitive | Wait-free reads via two-copy publish/swap | `xtc_lrlock` ((S)2.4.4), per the RFC: writer uses inner `xtc_mutex` (not spinlock), oplog pre-allocated, drain-wait via `xtc_notify` |
| Memory: thread-exit cleanup hook | Per-thread `proc_exit` equivalent; on fault, supervisor restarts | `xtc_proc` cleanup runs on normal exit; faults bubble up to `xtc_supervisor` which restarts per strategy.  *Exactly* the requested model. |
| palloc / contexts review | Memory contexts that work in threaded mode | `xtc_alloc_ctx` ((S)4) wraps palloc inside a PG backend, falls back to `__os_malloc` outside |
| Logging (`/var/log`) thread-safe | `errcontext` per-thread, no torn lines | `xtc_log` ((S)2.4.5): per-loop ring buffer, single drain task, per-task `errcontext` chain |
| Proc number / thread id | 32-bit thread id + backwards-compat pid column | `xtc_pid_t` is already 32-bit `(loop_id << 24) \| local_id`; trivial to expose as both `pid` and `tid` |
| `RegisterBackgroundThread()` API | Parallel to `RegisterBackgroundWorker` for in-tree users | `xtc_proc_spawn` with options is *the* worker API; `xtc_supervisor` is `RegisterBackgroundThread` for restartable workers |
| Two-process supervisor | Thin parent restarts the multithreaded child on crash | Map: parent = `xtc_app` host process; child = a single `xtc_supervisor` rooted on the executor.  Connection acceptance moves into the child as an `xtc_proc`. |
| `setlocale` -> `uselocale` | Thread-safe locale handling | `os_locale.c` (`__os_uselocale`); bare `setlocale` lint-forbidden |
| `strerror_r` | Thread-safe errno strings | `os_errno.c` (`__os_strerror_r`); bare `strerror` lint-forbidden |
| `getopt_long` replacement | Re-entrant CLI parsing | `os_getopt.c` (`__os_getopt_long`) |
| `dlerror` review | Thread-safe dynamic-load error path | `os_dl.c` (`__os_dlerror_r`) |
| `getenv`/`setenv` audit | No racy environment writes | `os_env.c`: read-only after `xtc_app_start()`, lint-enforced |
| `PG_THREADSAFE_EXTENSION` macro | Self-declaration of thread safety | `XTC_THREADSAFE_MODULE` macro on the L5 boundary; `_PG_thread_init`/`_PG_thread_fini` map to `xtc_proc` per-spawn hooks |
| `xtc_supervisor` ((S)2.5) | `xtc_lock_dd_*` victim signal goes via `xtc_send`; supervisors handle the abort-and-retry envelope just like any other crash |
| Tooling: lints for new globals / statics / signals / sync | CI hard-fail on regressions | `dist/s_globals`, `dist/s_signals`, `dist/s_async`, `dist/s_cfg` -- all in (S)6.4 / (S)6.1 |
| Buildfarm threaded animals | Linux x86_64, Linux aarch64, macOS, threaded mode | xtc CI matrix in (S)3.5 -- Linux x86_64 glibc, Linux aarch64 musl, FreeBSD amd64, Windows MSVC, macOS arm64 gating; full PG-buildfarm mirror nightly |
| TSan in CI from CF1 | Threaded mode TSan-clean | `--enable-sanitize=thread` build profile; gating on every PR per (S)6.2 |
| LTO build profile | Link-time optimization | `--enable-lto` / `-Db_lto=true`; in (S)6.4 toolchain matrix |
| `THREADING.md` for core contributors | One doc on annotations, lints, GUC API, latches | `docs/threading.md` (xtc's; suitable to be lifted into `src/backend/THREADING.md`) |
| Migration guide for extension authors | Wiki page + blog post | `docs/extensions.md` covers the L5 boundary contract |

**Read this table as the deliverable list.**  Each row is something
xtc will do for the PG threading effort.  If a row looks weak when
we get to implementation, that is a re-design trigger for xtc, not
for PG.

---

## 21. Active build-matrix work items

The build-and-test matrix as it stands today.  Items marked `WIP`
are in progress; `BLOCKED` items list their gating dependency.

### Operating systems

| OS              | Status      | Notes |
|-----------------|-------------|-------|
| Linux glibc     | full pass   | Reference target.  279 munit + 23 PBT + 22 shell |
| Linux musl      | OS layer    | libxtc.a builds; coro_uctx absent (musl drops ucontext); needs coro_fctx.c substrate |
| FreeBSD 15      | full pass   | Verified through M13c (last full run; re-verify after recent changes) |
| illumos         | partial     | OpenSSL link drift in last attempt; pin to `--with-tls=none` to retest |
| macOS           | not yet     | Awaiting host; KVM runbook in `docs/M_MACOS_KVM.md` |
| AIX             | not yet     | Awaiting host; KVM runbook in `docs/M_AIX_KVM.md` |

### Windows toolchains (santorini host)

Full writeup in `docs/M_WINDOWS_MATRIX.md`.

| Toolchain       | Status      | Pass count        | Gating |
|-----------------|-------------|-------------------|--------|
| MinGW64 gcc 16.1.0 | full pass | ~233 munit / 50 binaries | -- |
| Clang64 22.1.4  | partial     | 48/48 of binaries that link; 3 don't compile (POSIX-only) | Port test_net_udp / test_proc_wait_fd / test_slab_shm to Win32 |
| MSVC cl 14.44   | BLOCKED     | --                | Asm port: fctx_x86_64_*.S -> MASM equivalents (~1-2 days); meson.build expansion to all source files; clang-cl as a stepping-stone driver works through the same path |

### TLS backends

Full writeup in `docs/M_TLS_MATRIX.md`.

| Backend            | Status      | Notes |
|--------------------|-------------|-------|
| OpenSSL 3.0.10     | full pass   | 16/16 |
| LibreSSL 4.2.1     | partial     | Builds clean against OpenSSL backend; tls_basic + tls_server pass (14/16); tls_client handshake fails -- LibreSSL 4.x cipher policy interaction in non-blocking poll loop, deferred |
| GnuTLS / mbedTLS / wolfSSL | not yet | Each requires a separate `tls_<backend>.c` (~1 person-day each) |

### libc matrix

Full writeup in `docs/M_LIBC_MATRIX.md`.

| libc       | Status      |
|------------|-------------|
| glibc 2.40 | full pass   |
| musl 1.2.5 | OS layer; needs coro_fctx.c |
| MSVC UCRT  | rolled into Windows matrix above |

---

## 22. What I want from you before we start coding

1. Sign off (or push back) on **Q1-Q16** in (S)10.  Defaults given.
2. Confirm the **layer renames** (`evt`, `ptc`, `orc`) and the
   sub-renames (`xtc_svr`, `xtc_fsm`, `xtc_app`, `xtc_reg`,
   lowercase `dispatch`/`reply`/`async`/`await`/`xtc_yield`).
3. Confirm the **platform matrix** in (S)3 -- particularly whether
   AIX is Tier 2 or can drop to "best-effort".
4. Confirm **C11** as the dialect.
5. Confirm **ISC license**.
6. Confirm the **(S)12 strategy** for `async()/await()` -- fiber as
   default, protothread fallback, explicit-thunk escape, with
   `dist/s_async` ((S)6.4) generating typed prototypes.
7. Confirm the **milestone order** -- particularly that M13a/b/c
   (RCU / LRLock / LWLock / lock manager benchmarking under
   threaded contention) lands *before* M16 (PG adapter) so we
   don't bake in a primitive that loses to the alternative
   under real PG-shaped contention.
8. Confirm the **(S)20 mapping** -- that we are explicitly framing xtc
   as the toolbox the PG v20/v21 threading effort is going to need,
   and that any gap in the table is a redesign trigger for xtc.
9. Anything missing?  Other PG subsystems on the threading plan I
   haven't mapped?

Once those are settled I'll create:

- the `dist/` skeleton with both build systems wired,
- the `src/inc/` umbrella headers,
- the `os/os_alloc.c` + `os/os_atomic.c` + `os/os_thread.c`
  starter trio,
- the `os/asm/fctx_x86_64_sysv.S` for fiber bring-up,
- the munit + hegel test harness skeleton,
- a `make check && meson test` that runs and passes against
  a stub library on Linux/macOS/Windows CI,

...and we iterate from there.
