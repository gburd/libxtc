# sqlxtc -- an xtc-native storage engine

> **Note.** This is a forward-looking plan; some sections describe
> mechanisms that were considered but not shipped.  Where the buffer
> manager is concerned, the engine addresses pages **by page id**
> through a striped-lock page table, with a cooling-stage CLOCK
> evictor and scan resistance (2Q probation).  It does not swizzle
> pointers.

This document specifies the storage engine that replaces SQLite's
btree / pager / buffer-pool triad for the `examples/06_sqlxtc`
example.  It is a design and planning document.  It draws on two
reference engines -- LeanStore's cooling-stage buffer manager and the
threadskv B-link tree -- adapted onto xtc's concurrency primitives,
behind the `sx_` facade that `engine.h` already publishes.

The companion documents frame the work.  `M_SQLXTC_HARDFORK.md`
describes breaking the *existing* SQLite into concurrent xtc_procs in
five stages.  `M_SQLXTC_GREENFIELD.md` describes what a SQL engine
would look like if it had been written on xtc from the start.  This
document is the concrete engineering plan for the storage layer that
both of those imagine: a from-scratch key/value store that owns its
pages, its cache, and its tree, and that the SQL layer drives through
a narrow seam.

## 1. Overview and goals

The current example embeds stock SQLite.  `pcache.c` supplies an
`xtc_slab`-backed `sqlite3_pcache_methods2`; `vfs.c` supplies an
instrumented `"sqlxtc"` VFS whose reads, writes, and syncs are
offloaded with `xtc_blocking_run`.  These are the two officially
sanctioned SQLite extension points, and they prove out the slab and
the blocking-offload mechanisms.  They do not, and cannot, change how
SQLite locks the database: the pager still serializes, the btree still
runs on the calling thread under the connection mutex.

The engine specified here replaces the btree, the pager, and the
buffer pool wholesale.  It keeps SQLite's record format and the VDBE's
cursor contract at the seam so the SQL layer is undisturbed, but
below the cursor everything is ours.

What "fully uses xtc" means, concretely:

  * The **page provider** -- the background thread that LeanStore runs
    to keep free frames available -- is an `xtc_proc`, supervised, not
    a detached pthread.
  * **Page I/O** is asynchronous.  A page fault submits through
    `xtc_io` on a loop, or is offloaded via `xtc_blocking_run` where
    the backend cannot poll a regular file; the faulting process
    parks (it does not block the loop thread).
  * **Page latches** are xtc atomics: an optimistic version counter in
    the common case, an `xtc_lwlock` for the contended fallback.
  * The **page table** that maps page id to frame is read through an
    `xtc_lrlock` so lookups are wait-free while the provider installs
    and evicts on the single writer side.
  * **Old versions** of structures freed during a structure
    modification are reclaimed with `xtc_rcu`.
  * **Transaction locks** at table granularity, with deadlock
    detection, come from `xtc_lockmgr`.
  * **Frames and nodes** are allocated from `xtc_slab` caches.
  * Every hot path increments an `xtc_stats` counter, gauge, or
    histogram, surfaced on the same metrics line the VFS and pcache
    already feed (`sqlxtc.*`).

The non-goals are equally explicit.  This is not MVCC.  There is no
per-row version chain and no snapshot isolation.  Concurrency comes
from fine-grained page latching plus the B-link structure, which is
real and substantial but bounded -- see section 4.

## 2. The buffer manager

The buffer manager owns a fixed DRAM pool of frames and a backing file
of pages.

### 2.1 Page-id resolution

A reference to a page -- a B-tree child pointer, a sibling right-link,
the root -- is a stable on-disk **page id** (`bm_pid_t`).  `bm_fix_pid`
resolves an id to a resident, pinned frame: it hashes the id to a
bucket in the page table, takes that bucket's stripe lock, walks the
short chain, and pins the frame if present.  A miss faults the page in
through `xtc_io` into a free frame (evicting a cooling-stage victim if
the pool is full) and installs it in the table.  `bm_alloc_pid`
allocates a fresh id and returns a pinned, zeroed frame.  Frames carry
a per-frame state (HOT when actively used, COOL once an eviction
candidate); a fix that finds a resident COOL frame promotes it to HOT.

Page ids -- not in-memory pointers -- are what the tree serializes into
its node cells, what the WAL records name, and what the superblock
stores; they are also the only page reference that remains valid when
the same pool is mapped at different addresses in different processes,
which a shared multi-process pool requires.

### 2.2 The frame -- header plus page

A frame is a header followed by a 512-byte-aligned page.  The header
carries the latch and the bookkeeping the evictor and provider need;
the page carries the persisted bytes.  The fields:

  * `STATE state` -- `FREE`, `HOT`, `COOL`, or `LOADED`.
  * `HybridLatch latch` -- the optimistic version latch (section 4).
  * `std::atomic<bool> is_being_written_back` -- set while the page is
    in flight to disk, so the evictor skips it.
  * `LID last_written_plsn` versus `page.PLSN` -- `isDirty()` is
    exactly `page.PLSN != header.last_written_plsn`.  This is the only
    dirty test; there is no separate dirty flag to keep in sync.
  * `PID pid` -- the on-disk page id.
  * `BufferFrame* next_free_bf` -- the free-list link.
  * `DTID dt_id` (on the page) -- which data structure (which table or
    index B-tree) the page belongs to.

In the xtc port the frame pool is one large mmap region carved by an
`xtc_slab` cache sized to `sizeof(frame)`; all frames are one size
class, so the slab never fragments -- the same property `pcache.c`
already relies on.  `keep_in_memory` pins the catalog root and other
must-stay pages.

### 2.3 The page table and the free list

Two structures index the pool:

  * The **page table** maps a resident page id to its frame; an id
    not in the table is not resident.  Every page reference goes
    through it (there are no direct frame pointers to short-circuit),
    so it is on the hot path: it is a fixed-modulo open-chained hash
    (`pid % nbucket`) with 256 cache-line-isolated stripe locks, so
    fixes of pages in different stripes proceed in parallel
    (section 4.2).
  * The **free list** is a stack of `FREE` frames threaded through
    `next_free`.  `bm_alloc_pid` pops one and returns it pinned and
    marked `HOT`; reclaim pushes one back.

### 2.4 Cooling-stage eviction, and how cooling drives writeback

Eviction does not write a hot page out synchronously on the fault
path.  A CLOCK sweep (`evict_one`) and a background trickler
(`xtc_proc`) cooperate in three steps.

**Phase 1 -- cool.**  When the free count drops below the cool target,
the sweep advances a clock cursor over the frames and flips a HOT,
unpinned frame whose `ref` second-chance bit is clear to `COOL`.  The
page is now in the **cooling stage**: still resident, still correct to
read, but marked as a future eviction candidate.  No data has moved
and nothing has been written.

**Phase 2 -- write dirty cooling pages ahead of eviction.**  The
trickler walks the cooling candidates.  For each:

  * If the page is **clean**, it can be evicted now: the sweep
    reserves the frame (pin 0 -> -1 so no fixer can re-pin it),
    removes its page-table entry, and returns the frame to the free
    list.
  * If the page is **dirty**, eviction prefers to skip it (leaving a
    clean victim for the foreground) and the trickler writes it out
    ahead of demand, while it is cooling, not at the moment a fault
    needs the frame.

This is the mechanism: cooling informs the buffer
manager which pages are about to be evicted, and the provider
proactively flushes those dirty cooling pages so that by the time a
frame is actually needed, its disk image is already current and it can
be reclaimed without a synchronous write on the fault path.

**Phase 3 -- harvest completions and evict.**  The provider polls the
async write buffer.  For each completed write it records
`last_written_plsn` (the page is now clean) and clears
`is_being_written_back`; then, if the page is still COOL and clean, it
evicts it as in phase 1.  Freed frames are batched back onto the
partition free list.

The mapping to xtc primitives:

  | LeanStore piece                 | xtc primitive                       |
  |---------------------------------|-------------------------------------|
  | page-provider thread            | `xtc_proc` under the supervisor     |
  | async write buffer / read       | `xtc_io` submit + poll; `xtc_blocking` fallback |
  | per-frame `HybridLatch`          | optimistic version + `xtc_lwlock`   |
  | partition free list / frame pool | `xtc_slab` cache                    |
  | page table                      | `xtc_lrlock` COW                    |
  | `PPCounters` (touched, evicted, flushed) | `xtc_stats` counters       |

The provider proc never holds a latch across a yield except the brief
exclusive windows phase 1 and phase 3 require, and those do not yield;
the I/O wait is on the async buffer, which parks the proc, not the
loop.

### 2.5 Scan resistance -- the cooling stage as a probationary FIFO

A large sequential scan -- a `VACUUM`, an analytical table scan, a bulk
load -- touches many pages exactly once.  A naive manager that admits
every faulted page as HOT lets such a scan promote its pages over the
working set and evict everything that was hot, so the OLTP traffic that
resumes after (or runs alongside) the scan faults all of its pages back
in.  This is the classic buffer-pool pollution problem.

The cooling-stage evictor keeps **no per-access bookkeeping** beyond a
single `ref` bit, so scan resistance cannot be bought with LRU-K (that
would put a write and a list splice on every page hit).  It comes from
the **cooling stage itself**, used as a probationary FIFO.  This is the
2Q insight (Johnson and Shasha, "2Q: A Low Overhead High Performance
Buffer Management Replacement Algorithm", VLDB 1994): a page seen once
and a page seen repeatedly belong in different queues.

The engine maps 2Q onto the frame states with one rule, and no new
hot-path work:

  * **Probationary admission.**  A demand-LOADED page is admitted to
    the COOL stage, not HOT (`scan_resist`, on by default).  A page
    becomes HOT only on a SECOND access -- the rescue that
    `ht_lookup_pin` performs (COOL -> HOT).  A scan touches each page
    once, so its pages stay COOL and never enter the hot set.  Freshly
    ALLOCATED pages are still born HOT: they are new, dirty, and
    certainly in use, not a scan artifact.

  * **COOL-first eviction.**  `evict_one` reclaims an already-COOL
    frame in preference to cooling a HOT one; it cools a HOT frame only
    when a full sweep finds no COOL victim (the `force_cool` retry,
    which also guarantees progress).  The page provider cools HOT
    frames only to refill the cool budget toward `cool_target`,
    counting the clean COOL frames it already has.  So an abundant
    supply of probationary scan pages becomes the eviction victims and
    the hot working set is never cooled to make room for the scan.

Together these make the scan recycle its own COOL pages while the hot
set stays HOT and resident.  `test_scan_resist` demonstrates it
directly: after warming a 16-page hot set and then scanning 3000 cold
pages through a 32-frame pool, re-touching the hot set reloads **0 of
16** pages with scan resistance on, versus **16 of 16** with it off.

This is the right fix for a scan that runs *alongside* a hot workload
(the OLTP-during-vacuum case): the hot set survives.  It is distinct
from the *cost of the scan itself* -- a full-tree vacuum still has to
read the whole larger-than-RAM tree from disk -- which is why version
reclamation on the write path uses incremental, HOT-style inline
pruning (`xstore_autovacuum`, section in docs/M_SQLXTC_MVCC_SQL.md and
the benchmark in bench/sqlxtc/ENGINE_AB.md) rather than a periodic
stop-the-world full scan.

The correctness subtlety the change exposed: a frame admitted COOL on
a demand load must be PINNED before it is published in the page table,
or it is briefly reachable-and-evictable with `pin == 0` and a
concurrent evictor can reclaim it under the loader.  `bm_fix_pid`
pins the frame before installing it under the bucket's stripe lock,
and re-checks for a racing publisher after `ht_remove`, so the load
and publish never expose an unpinned COOL frame.

## 3. The B-tree

The on-disk index is a B-link tree (Lehman and Yao) with a
prefix-compressed slotted node layout.  threadskv
(`threadskv10g.c`) is the structural reference.

### 3.1 B-link structure

Every node carries a pointer to its right sibling at the same level.
In threadskv this is `BtPage_.right` (and `left` for reverse cursors).
The right pointer is what makes the tree safe to traverse without
holding a parent latch: if a descent arrives at a node whose high
fence is below the search key -- because a concurrent split moved the
key rightward -- the searcher simply follows `right` to the new home.
`bt_loadpage` does exactly this:

```
  if (keycmp(fenceptr(page), key, len) < 0)
      if (page_no = page->right)
          continue;          // slide right into the split-off page
```

The split is "half-split then post": a split installs the new right
sibling and fixes the old node's `right` pointer atomically at the
leaf, and only afterward posts the separator key up to the parent.
Between those two steps the tree is still correct because the right
pointer bridges the gap.  This is the property that lets writers to
different parts of the tree proceed without a tree-global lock.

### 3.2 The slotted node with prefix compression

The node layout is LeanStore's `BTreeNode`: a `BTreeNodeHeader`
followed by a growing array of fixed `Slot` records from the front and
variable-length key/value bytes from the back (`data_offset` walks
down as `slot[]` walks up).  The header holds two fence keys --
`lower_fence` and `upper_fence`, each a `{offset, length}` `FenceKey`
-- that bound every key on the page.

Prefix compression falls out of the fences.  `setFences` computes
`prefix_length` as the longest common prefix of the lower and upper
fence keys:

```
  for (prefix_length = 0;
       prefix_length < min(lowerLen, upperLen)
         && lowerKey[prefix_length] == upperKey[prefix_length];
       prefix_length++) ;
```

That common prefix is stored exactly once (it is the head of the lower
fence key).  Every slot stores only the key bytes *after* the prefix:
`storeKeyValue` does `key += prefix_length; key_len -= prefix_length`
before copying.  `spaceNeeded` charges `key_len - prefix_length` per
key, so a page of keys sharing a long prefix holds far more entries
than a naive layout.  `getFullKeyLen` and `copyFullKey` reconstitute
the whole key by prepending the prefix when the SQL layer needs it.

Each `Slot` also caches a 4-byte `head` -- the first four post-prefix
bytes, byte-swapped to big-endian so integer comparison matches
lexicographic order (`BTreeNode::head`).  `lowerBound` compares the
search key's head against `slot[mid].head` first and only falls back
to a full `memcmp` when the heads tie and the key is longer than four
bytes.  For the common case of short distinct keys this turns the
binary search into a sequence of single-word integer compares.  An
optional `hint[hint_count]` array of evenly spaced heads narrows the
search bounds before the binary search begins.

`findSep` chooses a split point that maximizes the resulting common
prefix (it searches a window around the midpoint for the slot where
the prefix length changes), and tries to truncate the separator to the
shortest distinguishing key.  This keeps inner nodes dense and their
prefixes long.

### 3.3 Insert and split with lock coupling

Descent uses lock coupling, threadskv-style.  Each `BtLatchSet`
carries four independent reader/writer locks -- `readwr`, `access`,
`parent`, `link` -- plus a `modify` mutex, so the different phases of a
restructure do not block each other unnecessarily:

  * The descent takes `BtLockAccess` (intent) on the child before
    releasing the parent, then `BtLockRead` on the child, then
    releases the parent's `access`.  This is the coupling: at most two
    levels are latched at once, and the parent is released as soon as
    the child is secured.
  * An insert that fits takes `BtLockWrite` on the target leaf only.
  * A split takes `BtLockWrite` on the splitting node, allocates the
    right sibling, redistributes via `copyKeyValueRange`, fixes the
    `right` pointer, then takes `BtLockParent` to post the separator.
    If the parent itself must split, the same procedure recurses
    upward.  Lock acquisition order is page-id ascending where two
    locks are ever held together, which is the order the deadlock
    avoidance depends on.

Because of the B-link right pointer, a reader or another writer that
races the split is never wrong -- it either sees the old node (and
slides right) or the new one.

### 3.4 Range cursors

A scan opens at a start key with `bt_loadpage(..., BtLockRead)` to
locate the first slot (`bt_startkey`), then walks slots and follows
`right` across page boundaries (`bt_nextkey`), re-pinning each
successive page and dropping the previous one.  The cursor holds at
most one leaf latched at a time, in read mode, so scans run concurrent
with point writes elsewhere in the tree.  This is the cursor the VDBE
sits on top of at the seam.

### 3.5 Advanced features

**Prefetching.**  During a forward scan the cursor knows the next page
from the current page's `right` pointer before it needs it.  When that
sibling is not resident, the cursor submits an `xtc_io` read for it while
still consuming the current page, so the fault is already in flight by
the time the scan crosses the boundary.  The same readahead applies to
inner-node descent: an index range scan can prefetch the child pages
of the slots it is about to visit.  Prefetch depth is bounded by an
`xtc_res` budget so a large scan cannot evict the rest of the working
set.  Prefetch hits and wasted prefetches are `xtc_stats` counters.

**Skip-scan.**  A multi-column index `(a, b)` queried with a predicate
only on `b` would normally require a full scan.  Skip-scan instead
enumerates the distinct values of the leading column `a` and performs
a bounded `b`-range probe within each -- effectively a sequence of
`bt_startkey` seeks, each one re-descending to the next distinct `a`
prefix rather than scanning every leaf.  Prefix compression helps
directly here: the distinct leading-column values are exactly the
prefixes the nodes already group on, so the "next distinct `a`" seek
lands cheaply.  Skip-scan is chosen by the planner only when the
leading column has low cardinality; otherwise a full scan is cheaper.

**Prefix compression as storage reduction.**  Beyond search speed,
section 3.2's prefix compression is the primary key-storage reduction:
a leaf of timestamp-prefixed or tenant-prefixed keys stores the shared
prefix once per page instead of once per row.

## 4. Concurrency model

### 4.1 The hybrid page latch

Each frame's `HybridLatch` is an optimistic version latch.  A reader
does not lock at all in the common case: it reads the version, reads
the page, then rechecks the version.  If the version is unchanged and
no exclusive bit is set, the read was consistent and the reader
proceeds wait-free.  If it changed, the read restarts.  This is
LeanStore's `BMOptimisticGuard` / `recheck()` pattern.

When optimism fails repeatedly, or when a writer needs exclusivity,
the latch falls back to a real lock.  In the xtc port that fallback is
an `xtc_lwlock` embedded in the header: `xtc_lwlock_acquire` in
`XTC_LW_SHARED` for a pessimistic reader, `XTC_LW_EXCLUSIVE` for a
writer.  The version word and the `xtc_lwlock` state are the same
"hybrid" latch LeanStore describes -- optimistic first, pessimistic on
contention.  The exclusive bit corresponds to LeanStore incrementing
the version under the lock; a reader that observes it set restarts.

### 4.2 Page-table reads

The page table (page id to frame) is read on every fault and every
sibling-by-id lookup, and written only by the provider proc when it
installs or evicts.  This is a textbook read-mostly structure, so it
sits behind an `xtc_lrlock` in COW mode (`XTC_LRLOCK_COW`): readers
take `xtc_lrlock_read_begin` and traverse a stable snapshot wait-free;
the provider is the single writer, mutating the off-side copy and
calling `xtc_lrlock_publish`.  COW keeps the steady-state memory at
roughly one copy and pays an mmap+copy only on the first write after
idle.  This is the same substitution `M_SQLXTC_GREENFIELD.md` names as
the single highest-value step, applied to our own page table rather
than SQLite's pcache.

### 4.3 Reclaiming old versions

When the buffer manager evicts a frame or rebuilds a structure, an old
version cannot be freed until every reader that might still hold a
pointer into it has finished.  `xtc_rcu` provides this: readers wrap
traversals in `xtc_rcu_read_lock` / `xtc_rcu_read_unlock`, and the
writer hands retired objects to `xtc_rcu_retire`, which frees them only
after a grace period drains.  This is the deferred-reclaim safety net
for the page-table publish (and complements the freed-page-id
quarantine the B-tree uses for reissued pids).

### 4.4 Table-level transaction locks and deadlock detection

Above the page latches sits the transaction lock layer.  A transaction
acquires an `xtc_lockmgr` lock on each table it touches -- `XTC_LOCK_S`
for read, `XTC_LOCK_X` for write, with the intent modes
(`XTC_LOCK_IS`, `XTC_LOCK_IX`, `XTC_LOCK_IWR`) where the access path
warrants them.  The lock object key is the table (or index) id.  The
lock manager runs its deadlock detector periodically
(`XTC_LOCK_DETECT_PERIODIC`) and aborts a victim on a cycle, so a
lock-order inversion between two transactions touching the same tables
in opposite order is resolved automatically rather than hanging.  A
faulted session's locks are released through `xtc_lock_release_all`
registered with `xtc_proc_at_exit`, matching the recovery contract the
proc layer documents.

### 4.5 The single-writer-per-page invariant, honestly

Each page has at most one exclusive latch holder at a time.  That is
the invariant the cooling evictor, the optimistic readers, and the
split logic all depend on.  What it buys, and what it does not:

  * Writers to **different subtrees** proceed in **parallel**.  Two
    transactions inserting into disjoint key ranges latch disjoint
    leaves and never meet; the B-link structure and per-page latches
    let them commit concurrently.  This is genuine write concurrency.
  * Writers to the **same leaf** serialize on that leaf's exclusive
    latch.  Two transactions appending adjacent keys to the same page
    take turns.  The window is short -- the latch is held only for the
    mutation, not for the whole statement -- but it is a real
    serialization point.
  * There is **no row-level concurrency and no MVCC**.  A reader and a
    writer on the same page do not see independent snapshots beyond the
    optimistic-read retry; a long write blocks pessimistic readers of
    that page for its duration.

This is real write concurrency, short of full MVCC, and the document
states it plainly so no one mistakes B-link page latching for snapshot
isolation.

## 5. Integration and staging

### 5.1 Where it slots behind the facade

`engine.h` publishes the `sx_` surface; `engine.c` is "the single
boundary between sqlxtc and the embedded SQL engine."  Today that
boundary forwards to SQLite.  The storage engine slots in underneath
the cursor contract: the SQL parser, the planner, and the VDBE keep
their shape (the example already uses the Lime parser as a pure
function on the session proc's stack), and `engine.c` is rewritten to
drive our btree cursors instead of SQLite's `sqlite3_stmt` stepping.
The `sx_step` / `sx_column_*` cursor API in `engine.h` is the contract
that does not change; everything below it does.

`vfs.c` and `pcache.c` are subsumed rather than extended.  The
buffer manager replaces the pcache entirely -- our frames are the
resident set, so there is no separate `sqlite3_pcache`.  The async
read/write path replaces `vfs.c`'s blocking-offload shim with direct
`xtc_io` submission, keeping `xtc_blocking_run` only as the fallback
for backends that cannot poll a regular file.  The instrumentation
those files established (`sqlxtc.vfs.*`, `sqlxtc.pcache.*`) carries
over as the engine's own `xtc_stats` metrics.

### 5.2 Phased plan

The work is staged so each phase is independently testable and leaves
a working tree.

  1. **Buffer manager -- foundational, in progress this milestone.**
     Frame pool on `xtc_slab`, the striped-lock page table, the free
     list, and the page-provider proc with the cooling cycle and
     `xtc_io` writeback.  Tests
     drive synthetic page faults and assert the cooling-then-flush
     ordering and zero-leak reclamation.
  2. **Slotted node plus prefix compression -- next.**  Port
     `BTreeNode` layout, `lowerBound` with heads and hints,
     `setFences` prefix computation, `storeKeyValue`, `compactify`,
     `findSep`.  Single-threaded node-level property tests
     (round-trip insert/lookup/delete, split/merge invariants).
  3. **B-link tree.**  Right/left sibling pointers, `bt_loadpage`
     lock coupling with slide-right, half-split-then-post, the four
     per-page latch roles.  Concurrent stress tests with disjoint and
     overlapping key ranges.
  4. **Cursor, prefetch, skip-scan.**  `bt_startkey` / `bt_nextkey`
     range cursors, `xtc_io` sibling/child readahead, and planner
     skip-scan for low-cardinality leading columns.
  5. **WAL and recovery.**  Write-ahead log with a single writer
     proc, page LSNs (`PLSN`), redo replay on restart.  This is the
     hard phase (section 6).
  6. **SQL / VDBE integration.**  Rewrite `engine.c` to drive the
     btree cursors; map the record format and the `sx_` cursor API
     onto the new storage.  This is the largest remaining piece.

Phase 1 is foundational and is the work of this milestone.  Phases 2
through 4 are the bulk of the storage engine proper.  Phases 5 and 6
are the multi-quarter remainder: durability and SQL integration are
each a project in their own right.  This staging is the storage-layer
counterpart to the stages in `M_SQLXTC_HARDFORK.md` -- where the
hard-fork plan retrofits SQLite's own btree under `xtc_lockmgr` and
`xtc_lwlock` (its Stage 4), this plan replaces the btree outright; the
two converge on the same primitives.

### 5.3 Implementation status (what is built and tested)

Landed and tested in `examples/06_sqlxtc/` (each with an in-process
test, no daemon; ASan/UBSan clean):

  * `bufmgr.c` -- Phase 1.  A page-id-addressed frame pool with a
    striped-lock page table (`bm_fix_pid`/`bm_alloc_pid`), cooling-stage
    CLOCK eviction with a page-provider `xtc_proc` that proactively
    flushes dirty COOL pages ahead of demand, per-frame content
    latches, and scan resistance (2Q probation).  `test_bufmgr` cycles
    200 pages through a 16-frame pool; `test_bufmgr_mt` drives it from
    a 4-thread `xtc_exec` (16 workers + the provider) with 32000
    verified reads and zero mismatches -- the buffer manager is
    thread-safe.
  * `btnode.c` -- Phase 2.  The prefix-compressed slotted node
    (common fence prefix stored once, per-slot 4-byte head, split /
    search / insert / remove).  `test_btnode`: 3834 checks.
  * `btree.c` -- Phase 3 (serial core + async-I/O proven).  A
    multi-level B-tree on the
    page-table path: insert with split propagation, lookup, delete,
    and a forward range cursor.  `test_btree`: 5000 keys built
    shuffled through a 24-frame pool (far over memory, so the tree
    pages through eviction), all looked up correct, a full ascending
    cursor scan returns every key in order (proving the split
    separators), plus binary keys -- 40890 checks.

### 5.4 Concurrency: parallel writers via latch coupling

Update: parallel writers are now implemented.  The B-tree runs under
concurrent, genuinely parallel writers and concurrent readers on a
cooperative loop and on a multi-loop executor.  The model is
fine-grained latch coupling (crabbing) on a fiber-yielding
shared/exclusive page latch.

  * The missing primitive was built first: `xtc_arwlock`, a
    shared/exclusive latch whose contended waiters PARK the fiber
    (yield to the loop) rather than blocking the OS thread.  It is the
    reader/writer analogue of `xtc_amutex` (FIFO direct hand-off,
    condvar fallback off-loop).  Because it yields, a holder may park
    across a child fix or a page-allocation park without wedging the
    loop -- the property latch coupling requires.  The buffer
    manager's per-frame content latch is now an `xtc_arwlock`.
  * There is NO per-tree writer lock.  Writers on disjoint subtrees
    proceed in parallel, serializing only on the per-page latches they
    actually collide on (and parking when they do).
  * Writer descent (`bt_insert`): exclusive latch-couple from the
    root, keeping a stack of held frames.  When a latched internal
    node is "safe" (a third of the page free, so a child split cannot
    cascade into it) release every ancestor above it; the retained
    stack is then [deepest safe node .. leaf].  A leaf split
    propagates the separator UP through that already-held stack --
    never acquiring a latch upward -- and grows a new root if the held
    root splits.  All latches are taken top-down in the same order by
    every writer and reader, and propagation only touches held frames,
    so the scheme is deadlock-free.  With the yielding latch the
    sibling allocation may park while the node stays latched, so the
    old drop-relatch dance is gone.
  * Reads shared latch-couple (latch the child before releasing the
    parent), so a reader can no longer be split out from under -- a
    miss is conclusive and the old re-confirm pass is gone.  Delete
    exclusive latch-couples to the owning leaf.  The range cursor
    holds one leaf shared at a time and follows the right-sibling
    chain.  This still avoids the Lehman-Yao internal-level B-link
    follow, whose interaction with per-node prefix compression made it
    the costlier option here.
  * `test_btree_mt`: four writer processes insert fully INTERLEAVED
    keys (writer w owns keys congruent to w), so adjacent keys share
    leaves and several writers split the same nodes and the same root
    concurrently -- the direct test of the coupling -- alongside four
    reader processes, on a four-loop executor (four OS threads) with
    the page-provider live and I/O offloaded.  Every key is present
    and correct, concurrent reads see zero wrong values, and the tree
    pages through eviction throughout.  Validated to height 4 with
    255+ concurrent splits (three root splits under contention) on a
    512-byte-page, 20-frame pool; deterministic across repeated runs,
    ASan + UBSan clean.

What remains (a further optimization, not a correctness gap): the
writer takes the root exclusive briefly on every insert before it can
release it at a safe node.  An optimistic two-pass descent (shared
crab to the leaf, exclusive only the leaf for the common non-splitting
insert, fall back to the pessimistic exclusive crab on a split) would
remove even that brief root contention.  MVCC remains out of scope
(section 6): this is page-granular concurrency, not snapshot
isolation.

## 6. Risks and honest limitations

  * **Full MVCC is out of scope.**  The engine gives page-granular
    concurrency, not snapshot isolation.  Readers and writers on the
    same page contend; there is no version chain.  Adding MVCC later
    would mean per-row version records and a visibility map, which is
    a separate, larger design.  Section 4.5 states the bound plainly
    so callers do not over-claim the concurrency.

  * **Recovery is the hard part.**  A buffer manager that evicts dirty
    pages proactively (section 2.4) needs a write-ahead log and
    correct LSN ordering so that a crash mid-flush is recoverable: no
    page may reach disk ahead of the log records that explain it.  The
    cooling evictor must honor the WAL flush-before-page-write rule,
    which couples phase 2 of the provider to the log writer.  threadskv
    carries a redo log and `logseqno` per page for exactly this; the
    `PLSN` / `last_written_plsn` machinery in the frame header is the
    hook.  Getting this provably correct under concurrent eviction is
    the single highest-risk item, and it is why WAL/recovery is its
    own late phase rather than folded into the buffer manager.

  * **SQL-execution integration is the largest remaining piece.**  The
    storage engine can be built and tested in isolation as a key/value
    store, but wiring it under the VDBE -- record encoding, the cursor
    contract, schema and catalog storage, the optimizer's awareness of
    prefix compression and skip-scan -- touches the whole upper half
    of the engine.  Until phase 6 lands, the storage engine stands
    alone behind the `sx_` seam and the example continues to run on
    SQLite.

  * **The page table writer is single.**  The `xtc_lrlock` gives
    wait-free reads but one writer; if install/evict throughput ever
    becomes the bottleneck, the table must be partitioned (one lrlock
    per partition, as the free list already is) before adding writer
    parallelism.  LeanStore's partitioning is the precedent and the
    mitigation.

## Remaining arc: making xstore the default backend (sequenced plan)

Status at this checkpoint: the storage engine (bufmgr + B-link tree +
WAL + recovery + checkpoint) and the xstore MVCC virtual table are
built and tested, and xstore is reachable via `CREATE VIRTUAL TABLE t
USING xstore`.  Snapshot MVCC, transaction-level isolation
(xBegin/xCommit write buffering + one commit timestamp), and Cahill SSI
serializability with predicate (range) locking are implemented and
enforced (xs_sync returns SQLITE_BUSY on a pivot) -- see test_xstore.c.
Quack supports int/float/text/blob/null bind params, multi-statement
batches, and a per-connection prepared-statement cache.

What remains to make sqlxtc fully xtc-native, in dependency order.
Each step is self-contained and independently testable; do them as
separate commits.

1. **Multi-column rowstore (xstore.c).**  DONE.  xConnect parses the
   column list from `USING xstore(col1, col2, ...)` (no args keeps the
   k/v default); a type-preserving record codec
   `[ncol:u8][type:u8,payload]...` (0 null, 1 int64, 2 double, 3 text,
   4 blob) encodes the non-key columns into the version blob; xColumn
   decodes column i, xUpdate encodes argv[3..].  The MVCC key keeps
   (rowid, commit_ts); only the payload shape changed.
   Tested: test_xstore scenario_multicol.

2. **Multi-table + on-disk catalog (xstore.c).**  DONE.  The version key
   gained a 4-byte table-id prefix -- (table-id, rowid, commit_ts) --
   so one B-tree holds many tables; the write buffer, serializable read
   set, SSI read/range sets, GC grouping, and the WAL record are all
   table-id aware, so a cross-table BEGIN..COMMIT is still atomic and
   recovery rebuilds every table.  Each table's id is allocated from a
   persisted on-disk catalog kept at reserved table-id 0 (a catalog row
   is keyed (0, id, name)): ids are dense and sequential, so distinct
   names never collide -- unlike the earlier FNV name hash, which could
   silently alias two tables that hashed equal.  The catalog is
   authoritative and recovery-safe: every CREATE writes its (0, id,
   name) row through xs_put, so it is WAL-logged and replayed into a
   fresh B-tree, and a reconnecting vtab finds its id by name; the next
   allocation continues past the recovered high-water id.  ALTER TABLE
   RENAME writes a new version of the same id's catalog row.  A within-
   process cache (B-tree + name) shortcuts the catalog scan and reserves
   a just-allocated id against a concurrent creator; the catalog write
   happens outside that lock, since xs_put can park on the WAL ack.
   Tested: test_xstore scenario_multitable + scenario_catalog (30
   tables, no collision), test_wal_recover (two tables with overlapping
   rowids, both ids restored by name after total pool loss).

3. **Transparent CREATE TABLE -> xstore.**  DONE.  The Quack db layer
   (db.c db_rewrite_create_table, hooked in db_exec_params) rewrites a
   plain `CREATE TABLE t (cols)` into `CREATE VIRTUAL TABLE t USING
   xstore(cols)` when the native engine is open, so ordinary DDL lands
   in the xtc-native engine with no opt-in.  Conservative: only a
   single well-formed CREATE TABLE is rewritten; a batch, table options,
   trailing statements, or an already-virtual table pass through to
   SQLite's native B-tree (the escape hatch).  Tested:
   test_server_storage transparent_proc.

4. **connection-per-proc parallelism (main.c).**  DONE.  --threads N
   builds a supervised N-loop executor; the listener spawns each
   connection's proc round-robin across loops, each with its own
   MULTITHREAD handle.  With steps 1-3, per-connection handles sharing
   one process-global xstore now agree on table-ids (name-derived), so
   plain CREATE TABLE + reads/writes in a parallel server land in the
   one shared B-tree -- VDBE runs across cores, writers serialize at the
   B-tree latch, not a global handle mutex.  Tested: test_server_storage
   cpp_driver (8 procs x 200 rows over one shared store).

5. **Crash recovery: clean/unclean restart (engine.c, wal.c).**  DONE.
   A clean shutdown checkpoints the tree durable and drops a marker
   (`<data>.clean`); the next open trusts the checkpointed base.  After
   a crash (no marker) the on-disk tree may be structurally torn by
   partial mid-SMO eviction -- a parent page flushed before its split
   child -- which logical redo cannot repair, so the torn tree is
   DISCARDED and rebuilt from scratch by replaying the whole log onto a
   fresh page file.  The log is the source of truth and is truncated
   only at a clean shutdown, so a crash can always rebuild the full
   history; the immutable append-only version keys make that redo
   idempotent.  (Torn pages within a single page are handled by the
   double-write buffer.)  Tested: test_torn_smo (1000-row multi-level
   tree, constant mid-SMO eviction under a tiny pool, crash, full
   ordered-scan verification), test_server_storage's crash cycle
   (engine-level abandon + rebuild), test_wal_recover, test_persist.
   (A physiological in-place REPAIR of a torn SMO -- XL_PAGE images
   applied to the trusted base instead of a rebuild -- is now wired and
   tested as a mechanism, xstore_recover_inplace + test_inplace_redo,
   but is not yet the live crash default; see the "physiological
   in-place redo" update below and M_SQLXTC_BDB.md S3.)

6. **In-WAL checkpoint: bounded log (engine.c, wal.c, xstore.c).**  DONE.
   The log is the source of truth; recovery rebuilds the tree by
   replaying it.  To keep it from growing with the write history, the
   checkpoint lives IN the log: xstore_checkpoint_wal atomically
   rewrites the log as a CHECKPOINT record (carrying the commit clock)
   plus a dump of the LIVE row set -- newest committed version per key,
   superseded versions and tombstones dropped -- via a temp file +
   fsync + atomic rename (crash-atomic: the old log survives a crash
   before the rename, the compacted log after).  This bounds the log
   and replay to the live data, not the history.  A long-running server
   calls sx_storage_checkpoint periodically at a quiescent point.
   Tested: test_wal_compact (200 rows x 10 updates churn the log, the
   checkpoint shrinks it ~10x, recovery from the compacted log + a
   post-checkpoint tail restores every row).

Steps 1-6 are landed and tested.  A fuller ARIES (physiological page
logging + page LSNs, for an in-place-trusted base and faster cold
restart of a very large database) is a performance refinement, not a
correctness gap; see M_SQLXTC_WAL.md sec 3.

Update (physiological in-place redo -- mechanism wired and tested).
The physiological-redo path named above is now built and proven end to
end, though it is not yet the LIVE crash default:

  * Page LSNs are stamped (bm_opts.lsn_off; the first field of every
    btnode), and the buffer manager enforces write-ahead before
    flushing a dirty page (bm_set_wal_flush).
  * The B-tree split / root-growth path logs each page it writes as an
    XL_PAGE full-page after-image (xstore_register_smo installs the
    hook; btree.c stays WAL-agnostic via bt_set_smo_hook), bracketed as
    a nested top action closed by a dummy CLR -- the Stasis device that
    makes a structure modification crash-atomic (redone if it finished,
    never half-undone).
  * bm_apply_page_image writes an image onto its page only when the
    on-disk page LSN is older (page-LSN gated, hence idempotent), and
    xstore_recover_inplace drives that apply per XL_PAGE record while
    trusting a non-truncated base, repairing a torn structure
    modification in place rather than discarding it.
  * bm_min_rec_lsn returns the true recLSN truncation horizon (the
    oldest dirty page's first-dirty LSN); the trickler writes
    oldest-recLSN-first.

  test_inplace_redo proves the loop: drive REAL splits through SQL with
  SMO logging on, then (a) recover in place over the current base with
  every row intact, and (b) ZERO the split pages on disk and recover,
  watching the XL_PAGE images repair each torn page so every committed
  row reappears and the ordered scan holds.  test_redo_page covers the
  page-LSN gate and the recLSN horizon in isolation.

  STILL the LIVE crash default (sx_storage_open) is the proven LOGICAL
  rebuild onto a fresh page file (step 5, test_torn_smo), NOT in-place
  redo.  The reason, confirmed by experiment: logical XL_UPDATE redo
  cannot reliably navigate an ARBITRARILY torn base -- when a non-split
  row write's leaf was lost (only its logical record survives), a
  replayed bt_insert over the partially-repaired torn structure can
  fail to insert the row.  A fully in-place crash restart additionally
  needs physiological logging of non-split row writes (so logical redo
  never descends torn structure); that, plus mid-log truncation to the
  recLSN horizon, is the remaining work (see M_SQLXTC_BDB.md S3/S4/S5).

## Delete merge / page reclaim, and the concurrent-merge race

Deleting keys reclaims space: when a leaf falls below a quarter full,
a right-merge structure modification (bt_merge, under the per-tree SMO
lock) pulls the right sibling into the left, drops the parent
separator, cascades the underflow upward, collapses a one-child root,
and returns the emptied page to the buffer-manager freelist.  Freed
page ids pass through a one-epoch quarantine before reissue.

Single-mutator merge is correct and proven (test_shrink_and_reclaim,
test_collapse_to_root, test_no_bloat_churn, and a dedicated
single-threaded interleaved-churn stress that builds, empties, and
rebuilds a multi-level tree while validating that every surviving key
stays reachable by a from-root DESCENT, not just by a scan).

Concurrent merge is NOT yet correct and is DISABLED by default
(bt_set_merge_enabled, default off).  With it off, concurrent deletes
are fully correct -- every key is removed, the tree stays valid -- and
pages simply stay underfull rather than being reclaimed.

The race, as far as it has been localized: under a concurrent
insert/delete storm with merge enabled, a churn key whose delete
returned success can still survive, reachable by a scan (so it is in a
live, linked leaf -- not corruption of the page itself).  The merge
algorithm is single-threaded-correct, so the fault is purely an
interleaving.  Two latch-discipline guards were added and help but do
NOT close it: a node carries a `dead` flag that bt_merge sets while it
still holds the unlinked right node's latch, and the latch-free
descents (bt_insert_fast, bt_delete, descend_shared) check both that
flag and the node's lower fence (btnode_below_lo_fence) after latching
a leaf -- a key at or below the lower fence, or a dead node, means a
concurrent merge moved the key's range LEFT (which move_right cannot
follow), so the operation restarts from the root (bounded retries,
then an SMO-locked authoritative pass).  These correctly catch a
descent that LANDS on a merged-away leaf, but the surviving-key case
shows a remaining interleaving they do not cover -- most likely in the
internal-node merge cascade (a latch-free descent caching an internal
child pointer across a parent merge) or an insert/merge interaction
that leaves a duplicate in a sibling.  Closing it needs move-right +
dead/fence validation at EVERY internal level of every latch-free
descent, and is left as open work; the guards and the dead flag are
kept because they are correct improvements to the descent paths
regardless.
