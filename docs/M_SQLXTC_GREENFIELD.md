# SQLite reimagined on libxtc: a clean-slate design

The companion document `M_SQLXTC_HARDFORK.md` asks how to break the
*existing* SQLite into concurrent pieces.  This document asks the
inverse, and more interesting, question:

> If the people who wrote SQLite had started with libxtc, and liked
> its ideas, what would they have built?

SQLite's actual architecture is a product of its constraints in the
year 2000: a single-file, zero-dependency, embedded engine that had
to run anywhere a C compiler reached, with no threads it could rely
on.  Those constraints produced a brilliant design that is also,
deliberately, single-threaded at its core: one big serialized mutex,
a synchronous VFS, and a bytecode interpreter that runs to
completion on the calling thread.

libxtc removes those constraints.  A libxtc-native SQL engine would
keep SQLite's best ideas -- the bytecode VM, the B-tree, the pager,
the VFS abstraction -- but arrange them as cooperating processes
instead of one call stack behind one lock.  This is the design the
`examples/06_sqlxtc` example is evolving toward.

## SQLite's layers, as the starting material

SQLite's component layering is public and well understood:

  * **Tokenizer + parser** -- SQL text to a parse tree.
  * **Code generator** -- parse tree to VDBE bytecode.
  * **VDBE** -- a register-based virtual machine that executes the
    bytecode; the heart of query execution.
  * **B-tree** -- ordered key/value storage over database pages.
  * **Pager** -- the page cache plus the transaction/journal/WAL
    machinery that gives atomicity and durability.
  * **OS interface (VFS)** -- the pluggable layer that does actual
    file I/O and locking.

In stock SQLite these are linked into one library and, in serialized
mode, run one-at-a-time behind a connection (or database) mutex.  The
layering is clean; the concurrency is not, because it was never meant
to be.

## The libxtc-native arrangement

Start from libxtc's organizing idea: a unit of mutable state plus the
code that owns it is an `xtc_proc`, and everything else talks to it by
message.  Apply that to the layers above.

| Engine role | libxtc form | Why |
|-------------|-------------|-----|
| Connection / session | one `xtc_proc` per client | The session *is* the proc: its prepared statements, transaction state, and per-connection settings are the proc's private state.  No shared session table, no session mutex. |
| Parser + code generator | a pure function called inside the session proc | Parsing has no shared state; it stays on the calling proc's stack.  (sqlxtc already does this with the Lime-generated parser.) |
| VDBE execution | runs inside the session proc, yielding at I/O points | The VM steps bytecode; when an opcode needs a page that isn't resident, it does not block the thread -- it awaits a message from the pager proc and yields, so other sessions on the same loop run. |
| Page cache (buffer pool) | an `xtc_lrlock` over the page table | Readers find a resident page wait-free; the pager proc is the sole writer that installs and evicts pages.  This is the single highest-value substitution -- the page cache is the hottest read structure in any database. |
| Pager / WAL writer | one `xtc_proc` | Durability is inherently serial: the WAL is an append-only log with one writer.  Modeling the pager as a proc makes that serialization explicit and lock-free, exactly as kaka models a partition. |
| B-tree | fine-grained locks via `xtc_lockmgr`, keyed by page id | Concurrent readers and writers on different parts of the tree proceed in parallel; the lock manager's deadlock detector handles lock-order cycles that a hand-rolled scheme could not. |
| VFS / file I/O | `xtc_io` async submission | A page fault becomes an async read submitted to the loop's backend (io_uring / IOCP / kqueue); the faulting session awaits completion instead of blocking the thread. |
| Checkpointer, vacuum, stats | supervised background `xtc_proc`s | Long-running maintenance runs as its own proc under the engine's supervisor, restarted on fault, never blocking foreground work. |
| Resource limits | `xtc_res` | Bounded cache size, bounded in-flight I/O, bounded connections, with high-water callbacks -- the backpressure SQLite leaves to the embedding application. |
| Metrics | `xtc_stats` | Per-operation latency histograms and cache hit/miss counters, sharded per core. |

The shape that emerges: **sessions are processes, the pager is a
process, the page cache is a wait-free read structure, and the B-tree
is protected by a real lock manager.**  The big serialized mutex
disappears -- not by careful lock-splitting of existing code, but
because the state was never shared in the first place.

## What the message flows look like

A read query in the libxtc-native engine:

  1. The session proc parses + codegens (pure, on its own stack).
  2. The VDBE steps bytecode.  An opcode needs page P.
  3. It reads the buffer pool through the `xtc_lrlock`: if P is
     resident, it gets the page wait-free and continues -- no
     message, no yield.  This is the common case and it is fast.
  4. On a miss, it sends a `fault(P)` message to the pager proc and
     awaits the reply.  The session yields; other sessions run.
  5. The pager proc submits an async read via `xtc_io`, and when the
     completion arrives, installs P into the buffer pool (the single
     writer side of the lrlock) and replies to the waiting session.
  6. The session resumes where it yielded, page in hand.

A write transaction adds: the session acquires the relevant B-tree
page locks through `xtc_lockmgr` (deadlock-detected), modifies pages
in its private scratch, and at commit sends the dirty set to the
pager proc, which appends to the WAL and acknowledges.  The
checkpointer proc later folds the WAL back into the main file.

Nothing here holds an OS mutex across a yield; every wait is a
cooperative await that keeps the loop busy.

## How this differs from the stock engine

  * **No connection mutex.**  Sessions share nothing, so there is
    nothing to lock between them.
  * **Reads do not block on writes** at the page-cache layer.  The
    lrlock gives readers a stable snapshot of the page table while
    the pager installs new pages on the other copy.
  * **I/O does not block a thread.**  A page fault is an async
    submission; the faulting session yields rather than stalling its
    OS thread, so one thread serves thousands of sessions.
  * **Lock order is enforced, not assumed.**  The B-tree's
    fine-grained locks go through `xtc_lockmgr`, whose deadlock
    detector aborts a victim on a cycle instead of hanging.
  * **Background work is supervised.**  Checkpoint, vacuum, and
    analyze are processes under a supervisor, not callbacks the
    application must remember to drive.

## Why SQLite did not do this (and why that was right)

None of the above was available or appropriate for SQLite's mission.
An embedded engine that must link into a phone app cannot assume an
event loop, cannot spawn background threads the host did not ask for,
and must behave identically whether or not threads exist.  SQLite's
serialized core is the correct answer to *its* problem.  The point of
this exercise is not that SQLite is wrong, but that a *server-class*
SQL engine -- one that owns its process and wants to serve thousands
of concurrent connections with low tail latency -- would be built
very differently if libxtc existed when it started.  That server-class
engine is what sqlxtc is exploring.

## Mapping onto the sqlxtc example today

`examples/06_sqlxtc` currently embeds stock SQLite behind one
`xtc_lwlock`, with one session proc per connection and the Lime
parser doing pre-parse validation.  It has the session-as-proc shape
already.  The path from here to the libxtc-native design, in order of
value:

  1. **Page cache behind `xtc_lrlock`.**  Replace SQLite's pcache
     with a custom `sqlite3_pcache_methods2` implementation backed by
     an `xtc_lrlock` page table.  This is a supported SQLite
     extension point, so it needs no fork -- the single highest-value
     step, and the one that proves the read-concurrency claim.
  2. **Async VFS via `xtc_io`.**  Implement `sqlite3_vfs` over
     `xtc_io` so page reads submit to the loop and the session awaits
     completion.  Also a supported extension point.
  3. **Pager as a proc.**  Route durability through a single pager
     proc so the WAL writer is an explicit owner.
  4. **Fine-grained locks via `xtc_lockmgr`** -- the deep step that
     does require forking SQLite's B-tree, covered in
     `M_SQLXTC_HARDFORK.md`.

Steps 1 and 2 are reachable without forking SQLite at all, because
the pcache and VFS are designed to be replaced.  They are the natural
next phase for the sqlxtc example and would demonstrate the
read-concurrency and async-I/O claims on a real SQL workload.

## Status

Design note.  The sqlxtc example implements the session-as-proc and
parser-as-pure-function pieces today; the page-cache-on-lrlock and
async-VFS steps are the proposed next phase, tracked in PLAN.md.
