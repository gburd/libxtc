# sqlxtc -- an xtc-native storage engine

This document specifies the storage engine that replaces SQLite's
btree / pager / buffer-pool triad for the `examples/06_sqlxtc`
example.  It is a design and planning document.  It synthesizes two
reference engines -- LeanStore's pointer-swizzling buffer manager and
the threadskv B-link tree -- onto xtc's concurrency primitives, behind
the `sx_` facade that `engine.h` already publishes.

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
  * **Old versions** of swizzled structures are reclaimed with
    `xtc_rcu`.
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

The buffer manager owns a fixed DRAM pool of frames and a backing
file of pages.  It is modeled on LeanStore's design in
`_/leanstore/backend/leanstore/storage/buffer-manager/`.

### 2.1 Pointer swizzling -- the Swip

LeanStore's central trick is that a reference to a child page is not a
page id that must be hashed to find the frame; it is a tagged 64-bit
word that is *either* a direct pointer to the resident `BufferFrame`
*or* an on-disk page id, distinguished by the two most significant
bits.  `Swip.hpp` encodes this:

```
  1xxxxxxx...   EVICTED  -- low 63 bits are the page id (evicted_mask)
  01xxxxxx...   COOL     -- pointer with the cool_bit set
  00xxxxxx...   HOT      -- a bare BufferFrame* (hot_mask)
```

with `evicted_bit = 1<<63`, `cool_bit = 1<<62`, and the predicates
`isHOT()`, `isCOOL()`, `isEVICTED()`.  A HOT swip is dereferenced with
zero indirection -- `asBufferFrame()` is just the pointer.  The state
transitions are `warm()` (COOL to HOT, clears the cool bit), `cool()`
(HOT to COOL, sets it), and `evict(pid)` (to EVICTED, stores the page
id).  This is what makes a warm working set effectively pointer-chase
fast: most traversals never touch the page table at all.

In our engine the swip is the child reference stored in a B-tree inner
node's slot payload, exactly as `BTreeNode::getChild` casts the
payload to a `SwipType`.  Resolving a swip during descent mirrors
LeanStore's `resolveSwip`: HOT returns immediately; COOL warms it back
to HOT under the parent and child latches; EVICTED triggers a fault
that reads the page through `xtc_io`.

### 2.2 The frame -- header plus page

A frame is LeanStore's `BufferFrame`: a `Header` followed by a
512-byte-aligned `Page`.  The header carries the latch and the
bookkeeping the provider needs; the page carries the persisted bytes.
The fields we keep, named as in `BufferFrame.hpp`:

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

  * The **page table** maps an EVICTED page id to nothing (it is not
    resident) and a resident page id to its frame.  Because swizzling
    means most references are direct pointers, the page table is
    consulted only on a fault or when a sibling must be located by id.
    It is read through an `xtc_lrlock` (section 4.2).
  * The **free list** is a stack of `FREE` frames threaded through
    `next_free_bf`.  `allocatePage` pops one and returns it
    exclusively latched and marked `HOT`; `reclaimPage` pushes one
    back.  LeanStore partitions both the free list and the page-id
    space to cut contention; we keep that partitioning, one partition
    per provider shard.

### 2.4 Cooling-stage eviction, and how cooling drives writeback

This is the part the user asked to be explicit about.  LeanStore does
not evict a hot page directly.  It runs a three-phase cycle in
`pageProviderThread` (`PageProviderThread.cpp`); the engine runs the
same cycle inside an `xtc_proc`.

**Phase 1 -- cool.**  When a partition's free count drops below
`free_bfs_limit`, the provider draws a random batch of frames
(`randomBufferFrame`).  For each HOT candidate whose children are all
evicted, it finds the parent, takes the child and parent latches, sets
`state = COOL`, and calls `swip.cool()` on the parent's pointer to it.
The page is now in the **cooling stage**: still resident, still
correct to read, but marked as a future eviction candidate, and
reachable only through the page table rather than a HOT pointer.  No
data has moved and nothing has been written.

**Phase 2 -- write dirty cooling pages ahead of eviction.**  The
provider walks the cooling candidates.  For each:

  * If the page is **clean** (`!isDirty()`), it is evicted now:
    `swip.evict(pid)` rewrites the parent pointer to the page id, the
    frame is `reset()` and returned to the free list.
  * If the page is **dirty**, it is *not* evicted yet.  The provider
    sets `is_being_written_back` and submits the page to the async
    write buffer.  The write is issued now, while the page is cooling,
    not at the moment eviction is needed.

This is the mechanism the user named: cooling informs the buffer
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

## 3. The B-tree

The on-disk index is a B-link tree (Lehman and Yao) with LeanStore's
slotted node layout and prefix compression.  threadskv
(`threadskv10g.c`) is the structural reference; `BTreeNode.hpp/.cpp`
is the node-layout reference.

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
sibling is EVICTED, the cursor submits an `xtc_io` read for it while
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

When the provider swizzles a structure or rebuilds the off-side page
table copy, the old version cannot be freed until every reader that
might still hold a pointer into it has finished.  `xtc_rcu` provides
this: readers wrap traversals in `xtc_rcu_read_lock` /
`xtc_rcu_read_unlock`, and the writer hands retired objects to
`xtc_rcu_retire`, which frees them only after a grace period drains.
This is the deferred-reclaim safety net under both the swizzling and
the lrlock publish.

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
     Frame pool on `xtc_slab`, swip encoding, page table behind
     `xtc_lrlock`, free-list partitions, and the page-provider proc
     with the three-phase cooling cycle and `xtc_io` writeback.  Tests
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

  * `bufmgr.c` -- Phase 1.  Swip swizzling (HOT/COOL/EVICTED), the
    frame pool, the cooling-stage eviction with a page-provider
    `xtc_proc` that proactively flushes dirty COOL pages ahead of
    demand, the swizzle path (`bm_fix`) and the page-table path
    (`bm_fix_pid`), per-frame content latches, and the child-aware
    cooling invariant.  `test_bufmgr` cycles 200 pages through a
    16-frame pool; `test_bufmgr_mt` drives it from a 4-thread
    `xtc_exec` (16 workers + the provider) with 32000 verified reads
    and zero mismatches -- the buffer manager is thread-safe.
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

### 5.4 The remaining concurrency gap, precisely

Update: the two hazards below are now CLOSED, and the B-tree runs
under concurrent writers and readers on a cooperative loop (and a
multi-loop executor).  The work:

  * The per-tree writer lock is a fiber-yielding `xtc_amutex`, so a
    writer that parks on I/O does not wedge the loop and contending
    writers park rather than thread-block.  Writers serialize on it
    (one structural modification at a time).
  * No content latch is held across an I/O park: the split paths
    allocate the sibling frame with the node UNLATCHED (the alloc may
    park on eviction writeback), then re-latch to mutate.
  * Reads are lock-free and non-coupling: the descent releases each
    node's shared latch before fixing the child, so no latch is held
    across a child fix.  A hit is always correct (a concurrent split
    moves keys to a right sibling, never deletes them); a miss is
    confirmed by one descent under the writer lock, where the tree is
    stable.  This avoids the Lehman-Yao internal-level B-link follow,
    whose interaction with per-node prefix compression (re-fencing a
    node would re-encode its keys) made it the costlier option here.
  * `test_btree_mt`: four writer processes (disjoint key ranges) and
    four reader processes drive one tree on a four-loop executor (four
    OS threads) with the page-provider live and I/O offloaded.  Every
    key is present and correct after the concurrent build, thousands
    of concurrent lock-free reads see zero wrong values, and the tree
    pages through eviction throughout.  Deterministic across repeated
    runs, ASan-clean.

What remains is PARALLEL writers: today writers serialize on the
single `xtc_amutex`, which is the one-writer-many-readers model (as in
WAL-mode SQLite).  Letting writers on disjoint subtrees proceed truly
in parallel needs fine-grained latch coupling (crabbing) with
fiber-yielding shared/exclusive page latches -- a primitive xtc does
not yet have (xtc_lwlock is thread-blocking, xtc_amutex is exclusive
only) -- or full optimistic descent with version validation and
restart.  That is the next stage, tracked against `xtc_lockmgr`
(Stage 4 of `M_SQLXTC_HARDFORK.md`).

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
