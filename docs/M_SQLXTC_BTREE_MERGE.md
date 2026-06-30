# M_SQLXTC_BTREE_MERGE.md -- Concurrent B-link Node Merge

## 0. Status and scope

The merge structure-modification (SMO) is **already implemented** in
`examples/06_sqlxtc/btree.c` and `btnode.c`, and is **single-mutator
correct** (proven by `test_btree_delete_merge.c` scenarios 1-3). It is
**disabled by default** (`bt_set_merge_enabled`, default off;
`btree.c:171-178`, `btree.c:1745-1759`) because there is a **known,
localized structural race under concurrent latch-free deletes**
(`docs/M_SQLXTC_STORAGE.md:800-842`, `test_btree_delete_merge.c`
scenario 4 runs with merge OFF).

This document does three things:

1. Records the merge SMO protocol **as built** -- latching order, the
   B-link `dead`/fence invariants, underflow policy, WAL/ARIES
   interaction, parent-key deletion, right-link repair, and page
   reclamation through the buffer-manager quarantine.
2. Diagnoses the **concurrent-merge race** precisely and designs the
   fix: move-right + dead/fence revalidation at **every** internal
   level of every latch-free descent (plus a tightening of the
   `child_for_key` re-read window).
3. Gives a test plan, risk assessment, and a phased plan to flip
   `merge_on` to default-on with confidence.

All file:line references are against the tree as read on 2026-06-30.

---

## 1. Background: what exists today

### 1.1 The node layer (`btnode.c`, `btnode.h`)

A page is a slotted node (`struct btnode_hdr` at
`btnode.h:78-95`, packed `struct btnode_slot` at
`btnode.h:101-108`). The header already carries the merge machinery:

- `uint32_t right_sibling` -- the Lehman-Yao right-link (0 == none).
- `uint8_t dead` -- "unlinked by a merge" tombstone (`btnode.h:88-92`).
- `lo_fence_off/len`, `hi_fence_off/len` -- the fence keys that bound
  every key on the page. The node owns the half-open-on-the-left range
  `(lo_fence, hi_fence]`.

The node owns a complete, in-place, read-before-write merge:

- `btnode_merge(page, right_page)` (`btnode.c:418-470`) rebuilds
  `page` with fences `[page.lo, right.hi]`, appends all of right's
  slots after page's, inherits right's right-link, recomputes the
  merged prefix, and re-stores every cell. It reads both pages fully
  into a stack scratch buffer before the single write-back, so it is
  safe in place.
- `btnode_merge_fits(page, right_page)` (`btnode.c:380-416`) is a
  conservative pre-check (assumes the worst-case empty merged prefix).
- `btnode_mark_dead` / `btnode_is_dead` (`btnode.c:642-653`).
- `btnode_below_lo_fence` (`btnode.c:626-637`) -- the LEFT-direction
  analogue of the B-link follow test: returns 1 when `key <= lo_fence`,
  meaning the key's range was absorbed leftward and this is a stale
  landing that `move_right` **cannot** recover.
- `btnode_beyond_hi_fence` (`btnode.c:610-624`) -- the classic
  Lehman-Yao right-follow test.

### 1.2 The split SMO (the model the merge inverts)

Split is the reference protocol the merge must compose with safely.

- **Fast path** (`bt_insert_fast`, `btree.c:464-540`): latch-free
  descent, one node latched at a time, no coupling, `move_right` at
  each level; takes only the leaf exclusive. Disjoint leaves run fully
  in parallel.
- **SMO path** (`bt_insert` -> `post_separator`, `btree.c:614-756`):
  taken only on a full leaf or an upsert; serializes on the per-tree
  `bt->smo` (`xtc_arwlock`, `btree.c:163-169`). "Half-split then post":
  `btnode_split` sets the left's hi-fence == right's lo-fence and makes
  the new right inherit the old right-link, so the half-split is
  immediately **chase-consistent** before the separator reaches the
  parent. `post_separator` walks up the captured pid path, latching
  each level independently with a `move_right` re-find, splitting full
  internals, and growing a new root off the top.
- **Latching order, universally top-down (root->leaf) and
  left->right.** Every descent latches the child before releasing the
  parent's latch (coupling) or releases-then-latches-deeper (no
  coupling), and `move_right` (`btree.c:386-414`) latches the right
  sibling before releasing the left. No thread ever holds two latches
  in the opposite order. This is what makes splits and merges
  deadlock-free against each other and against readers.
- **WAL/ARIES** (`bt_set_smo_hook`, `btree.c:308-320`;
  `smo_begin/page/end`, `btree.c:330-358`): each finished SMO page's
  full after-image is logged inside a nested top action (NTA). The
  engine implements the hooks over the log as `XL_PAGE` images plus a
  dummy `XL_CLR` (`xstore.c:2096-2182`). Recovery either replays the
  page images in place, page-LSN gated and idempotent
  (`xstore.c:3871-3889`), or -- the live default -- discards a torn
  base and rebuilds logically from the `XL_UPDATE` log
  (`test_torn_smo.c`, reopen=0).

### 1.3 The merge SMO as built (`bt_merge`, `merge_level`, `collapse_root`)

`bt_delete` (`btree.c:1372-1530`) runs the common delete latch-free and
leaf-local. Only if the leaf then underflows, and `merge_on` is set,
does it call `bt_merge` (`btree.c:1280-1370`):

1. `xtc_arwlock_wrlock(bt->smo)` -- the merge serializes against splits
   and other merges on the **same** lock the split path uses.
2. `bm_reclaim_quarantine(bm)` -- **epoch boundary**: drains pids freed
   by the *previous* merge onto the reusable freelist. Pids freed by
   *this* pass go to quarantine and become reusable only at the *next*
   merge (`bufmgr.c:108-133`, `1056-1095`). This is the RCU-like grace
   period for reissued page ids.
3. Latch-free descent collecting the ancestor pid path (`path[]`),
   with `move_right` at every level.
4. Drop the leaf latch, then `merge_level(path, depth-1, key, klen)`.

`merge_level` (`btree.c:1067-1278`) is the core, latching strictly
top-down:

- Fix + exclusive-latch the **parent**; `move_right` (no-op under the
  SMO lock but keeps the discipline uniform).
- Choose the merge pair `(L, R)` of adjacent children under **this**
  parent: prefer C's right sibling (`lslot=cslot, rslot=cslot+1`),
  else C's left sibling. R is always the one unlinked. Never cross to a
  cousin under a different parent (would need a second parent latch and
  break the order).
- Fix + exclusive-latch **L**, then **R** (left then right == the same
  order a descent and `move_right` take).
- Revalidate under the latches: same leaf-ness, `L.right_sibling == R`,
  pair fits, at least one underflows.
- `btnode_merge(L, R)` (internals rebuild R's slot-0 empty child keyed
  by the parent separator first). L now owns R's keys and R's
  right-link.
- `btnode_remove(pp, rslot)` -- drop R's separator from the parent.
- Commit L (dirty), commit parent (dirty), `btnode_mark_dead(R)`,
  unfix R **not dirty**, `bm_free_pid(R)`.
- If the parent now underflows, recurse `merge_level(level-1)`; at
  level 0, `collapse_root` (`btree.c:982-1024`) makes a one-child root's
  sole child the new root and frees the old root.

The **B-link safety invariant** is documented inline at
`btree.c:1230-1278`: a page is unlinked and freed only while the pass
holds, at once and exclusively, the latches on the parent, L, and R.
Any concurrent reader/writer that can reach R either already holds R's
latch (so the pass blocks acquiring R until that reader leaves, and
that reader read only valid pre-merge bytes) or reaches R through a
live link (the parent separator or L's right-link) -- **both of which
the pass rewires away before releasing the parent and L latches.**
Direction matters: merging R **into** L keeps L's identity and its
place in the right-sibling chain, so a reader on L that walks right now
finds R's keys absorbed into L and stops, never chasing a dangling
right-link.

---

## 2. The merge SMO protocol (exact, as the design fixes it)

### 2.1 Latching order and deadlock freedom

This is the classic hard part: **splits propagate bottom-up
(leaf->root) and left->right; merges want to reach right-to-left and
top-down.** The resolution used here, and the one this design keeps, is:

> **All structure modification serializes on a single per-tree lock
> (`bt->smo`), and within an SMO every latch is taken strictly
> top-down (parent before child) and left-before-right.**

Because split and merge take the *same* `bt->smo` writer lock
(`btree.c:625` for split, `btree.c:1289` for merge), they **never run
concurrently**. The "splits go one way, merges go the other" deadlock
cannot arise between two SMOs: there is only ever one SMO in flight per
tree. The remaining concurrency is SMO-vs-latch-free-descent (readers,
fast-path inserts, common-path deletes), and there the descent never
holds two latches in opposite order, so it cannot deadlock the SMO.

Concretely the merge holds at most three latches, acquired in this
order and released in the reverse-safe order shown:

```
  acquire: parent (X)  ->  L (X)  ->  R (X)
  release: L (commit dirty) -> R (mark dead, commit not-dirty)
           -> parent (commit dirty)
```

`move_right` is never invoked while holding a second latch (it always
hops one node at a time), so it cannot invert order against the SMO
either (`btree.c:386-414`).

Rationale for keeping the single SMO lock rather than fine-grained
merge latching: the split path already pays this serialization and the
common insert/delete/lookup paths stay fully parallel and latch-free
(they never take `bt->smo`). A delete only reaches `bt_merge` when a
node genuinely underflows -- a rare event under the hysteresis policy of
section 4 -- so the SMO lock is off the hot path. Fine-grained merge
latching (e.g. lock-coupling pairs of siblings) would buy parallel
merges in disjoint subtrees but reintroduce exactly the bidirectional
deadlock the single lock avoids, and is not justified at this layer's
scale.

### 2.2 B-link semantics during and after merge

Two directions, two recovery tests:

- **Right-follow (split direction).** A reader/writer that descended
  before a split sees a node whose hi-fence is now below its key; it
  follows `right_sibling` (`btnode_beyond_hi_fence`,
  `move_right`). Merge preserves this: L's hi-fence *widens* to R's
  when R is absorbed, so a chaser on L's left neighbor that walks right
  lands on L and `btnode_beyond_hi_fence` correctly says "key belongs
  here" -- it never needs to reach the freed R.

- **Left-absorb (merge direction).** A descent that *landed on R* (or
  was about to read R's child pointer) before the merge finds, after
  re-latching, either `btnode_is_dead(R)` true or `key <= R.lo_fence`
  (`btnode_below_lo_fence`). `move_right` cannot recover this (the key
  moved LEFT). The descent must **restart from the root**. The parent
  separators now route to L, which absorbed R.

The **"mark deleted, defer reclaim" approach is exactly what is
used and what this design keeps.** R is marked `dead` while still
exclusively latched, its parent separator and L's right-link are
rewired away under the same latch set, and only then is its id freed.
The freed id is quarantined for one SMO epoch before reissue. This is
the tombstone + grace-period discipline; there is no attempt to do
optimistic lock-free reclamation of the page itself.

### 2.3 The concurrent-merge race -- diagnosis and fix

**Symptom** (`docs/M_SQLXTC_STORAGE.md:820-842`): under a concurrent
insert/delete storm with merge ENABLED, a churn key whose delete
returned success can still survive, reachable by a scan -- so it sits in
a live, linked leaf. The page is not corrupted; the fault is purely an
interleaving. The leaf-landing guards (dead flag + lo-fence check,
applied in `bt_insert_fast` `btree.c:521-525`, `bt_delete`
`btree.c:1437-1452`, `descend_shared` `btree.c:838-845`) correctly
catch a descent that *lands on a merged-away leaf*. They do **not**
cover the surviving-key case.

**Root cause.** The latch-free descents validate dead/fence only at the
**leaf**, never at the **internal levels they pass through.** The
descent reads an internal node's child pointer (`child_for_key`,
`btree.c:267-275`), releases the internal latch, then fixes the child.
Two windows are open:

1. **Stale internal child pointer across a parent merge.** Between
   reading `child = child_for_key(internal_page, key)` and latching
   `child`, a concurrent merge can collapse/cascade through that
   internal node: the chosen `child` may be R (about to be freed) or
   the separator the descent routed on may have been removed, so
   `child_for_key` returns the *wrong* subtree -- one that does not (yet)
   own `key`. The descent then lands on a leaf that move_right cannot
   bridge to (the correct leaf is to the *left*, absorbed into L). A
   delete on that wrong leaf misses; an insert on it can create a
   duplicate in a sibling. The leaf-level dead/lo-fence check fires
   only if the landed leaf happens to be dead or excludes the key --
   but the descent can land on a perfectly live leaf that simply is not
   the owner, and the check passes.

2. **`child_for_key` re-read window.** Even at a single internal node,
   the child pointer is read once and the internal latch dropped; a
   merge that runs entirely after that read but before the child fix
   leaves the descent chasing a freed/relocated child id. Quarantine
   protects the *id* from reissue-under-a-stale-reader for one epoch,
   but does not make a *logically wrong* child choice correct.

**Fix: dead/fence revalidation + move_right at every internal level.**
The descent invariant becomes: *after latching any node (internal or
leaf) and after every `move_right`, check `btnode_is_dead` and
`btnode_below_lo_fence` before trusting the node.* If either fires, the
node was merged away under the descent and the descent **restarts from
the root** (bounded retries, then the SMO-locked authoritative pass --
the same escalation `bt_delete` already does at `btree.c:1483-1530`).
Specifically:

```
  descend(key):
    f = fix(root); latch(f); move_right(f, key)
    if dead(f) or below_lo_fence(f, key): restart
    while not leaf(f):
        child = child_for_key(page(f), key)         # read child pid
        cf = fix(child); latch(cf)
        unlatch(f); unfix(f)                         # release parent
        f = cf
        move_right(f, key)                           # right-direction repair
        if dead(f) or below_lo_fence(f, key):        # NEW: left-direction
            unlatch(f); unfix(f); restart            #      check at EVERY level
    if dead(f) or below_lo_fence(f, key): restart
    return f
```

This closes window 1: a child that was R (or any node absorbed
leftward) is now caught at the internal level the instant the descent
latches it, exactly as the leaf case already is. It closes window 2 in
combination with the merge always marking R `dead` and narrowing
nothing on L that would falsely claim R's old keys (L's lo-fence is
unchanged, its hi-fence widens, so a key that belongs in R now satisfies
`below_lo_fence(L) == false` and `beyond_hi_fence(L) == false` -- it is
correctly claimed by L, which holds R's keys).

This is precisely the residual work the storage doc names
(`M_SQLXTC_STORAGE.md:838-840`): "Closing it needs move-right +
dead/fence validation at EVERY internal level of every latch-free
descent." The three latch-free descents to update are `bt_insert_fast`
(`btree.c:480-512`), `bt_delete`'s descent loop (`btree.c:1411-1434`),
and `descend_shared` (`btree.c:803-848`). `cursor_descend`
(`btree.c:1591-1640`) needs the same treatment for correctness of a
cursor opened during a merge storm.

**Why this is sufficient.** The merge's own B-link invariant
(`btree.c:1230-1278`) guarantees that once R is unlinked and freed, no
live link reaches it; a descent can therefore reach R only if it read
R's id *before* the unlink, in which case it now holds a stale id whose
page is either (a) still resident and `dead`, caught by `is_dead`, or
(b) reissued in a *later* epoch (quarantine guarantees not this epoch),
in which case the descent that read it this epoch has already finished
(the quarantine grace period). The added internal-level check turns the
"reached a node whose range moved left" condition into a restart at the
*first* such node rather than only at the leaf, eliminating the
land-on-a-live-wrong-leaf interleaving.

### 2.4 How racing search/insert/delete behave after the fix

- **Search (`bt_lookup` via `descend_shared`).** Shared-coupling
  descent with the internal-level dead/lo-fence check. A search that
  raced into a doomed subtree restarts; a miss remains conclusive
  because the authoritative owner is always reached.
- **Fast insert (`bt_insert_fast`).** On any dead/lo-fence hit at any
  level, returns 0 (no change), and `bt_insert` falls to the SMO path,
  which re-descends under `bt->smo` -- no merge runs concurrently there,
  so it is authoritative.
- **Common delete (`bt_delete`).** Bounded latch-free retries on
  dead/lo-fence/not-found, then the SMO-locked authoritative descent
  (`btree.c:1483-1530`) that cannot race a merge.
- **Cursor.** `bt_cursor_resume` (`btree.c:1700-1740`) already
  re-fixes the parked leaf and falls back to a full re-descent when the
  parked page no longer covers the resume key; the same fresh descent
  must carry the internal-level checks (it goes through
  `cursor_descend`).

---

## 3. Right-link repair and parent-key deletion

- **Right-link repair.** `btnode_merge` makes L inherit R's
  `right_sibling` (`btnode.c:464`, `btnode_merge` sets
  `th->right_sibling = rh->right_sibling`). After commit, L's
  right-link points *past* the merged-away R to R's old right neighbor.
  No separate "fix the left sibling of R" step is needed because the
  merge pair is always *adjacent* (`L.right_sibling == R`, revalidated
  at `btree.c:1180`), so L *is* R's left sibling and the inheritance is
  the repair. A cursor that was on L and walks right skips R entirely.
- **Parent-key deletion.** `btnode_remove(pp, rslot)` drops R's
  separator from the parent (`btree.c:1224`). This is what makes R
  unreachable from above. It may leave the parent underfull, which
  cascades: `merge_level` recurses up (`btree.c:1270-1276`), and a
  one-child root collapses (`collapse_root`, `btree.c:982-1024`,
  shrinking `st_height` and freeing the old root). The cascade
  terminates at the root (`level == 0`).

The internal-node merge is subtler than the leaf merge because of the
slot-0 `-infinity` empty child. `merge_level` rebuilds R into scratch
with its slot-0 re-keyed by the real separator (the parent's rslot key
== R's lo-fence) before calling `btnode_merge`, carrying R's *real*
hi-fence so a finite hi-fence from an earlier split survives
(`btree.c:1190-1222`). This is essential: dropping R's hi-fence would
let the merged node wrongly claim a key that belongs further right
instead of chasing it.

---

## 4. Underflow threshold and hysteresis

Policy (`btree.c:140-154`):

- `BT_MERGE_NUM/BT_MERGE_DEN = 1/4`. A node underflows when
  `used_bytes * 4 < page_capacity` (below a quarter full), or when it
  is structurally empty/one-child.
- An internal node also underflows at `count <= 1` (only the slot-0
  child left).

Hysteresis: insert splits a node near **full** and merge fires near a
**quarter** full, so a key oscillating around the boundary does not
thrash split<->merge. The 1/4 bound is the classic B-tree underflow
threshold tuned conservatively. The merge additionally fires only when
`btnode_merge_fits` says the pair fits one page, so a merge never
immediately re-splits.

Two refinements this design recommends keeping/adding:

1. **Best-effort, never mandatory.** Any obstacle (no sibling, different
   parent, does not fit, alloc failure) ends the pass with the tree
   intact and correct, just less compact (`btree.c:1262-1268`). This is
   already the behavior and is the right policy -- correctness never
   depends on a merge completing.
2. **Underflow re-check under latch.** The merge pre-check on the
   delete path is advisory; `merge_level` re-reads `used_bytes`/`count`
   under the exclusive latches and bails if neither member underflows
   (`btree.c:1166-1185`), so a node that filled back up between the
   delete and the SMO is left alone. Keep this.

---

## 5. WAL logging, redo, and undo (ARIES)

### 5.1 The merge as a nested top action

A merge touches up to three pages per level (parent, L, R) plus, on
cascade, more parents and possibly the root. Like the split, it must be
crash-atomic: a crash that flushed L's merged image but not the parent's
separator-removal (or vice versa) leaves a torn structure.

**The merge currently logs nothing** -- `bt_merge`/`merge_level` do not
call `smo_begin/page/end`. The split path does (`btree.c:709-744`).
This design adds the same NTA bracket to the merge:

```
  bt_merge:
    nta = smo_begin()                 # begin marker = durable LSN
    merge_level(...)                  # logs each finished page image
    smo_end(nta)                      # dummy CLR closes the NTA
```

and inside `merge_level`, for every page it finalizes, **before
unlatch**:

```
  bm_predirty(bm, lf); smo_log_page(bt, lpid, page(lf));   # merged L
  bm_predirty(bm, pf); smo_log_page(bt, path[level], page(pf)); # parent
  # R is dead -> its image is logged as a dead/empty page OR not logged;
  # see 5.2
```

`collapse_root` must also bracket and log the new-root selection (it
already calls `bt_write_super`; it must `smo_log_page` the surviving
child if its header changes, and -- critically -- order the superblock
root-pid update after the page images, matching `post_separator`'s
root-growth at `btree.c:920-942`).

### 5.2 Redo: physiological, page-LSN gated, idempotent

The engine logs each finished page's **full after-image** as `XL_PAGE`
(`xstore.c:2119-2141`), redo-only, with the page-LSN at a fixed offset.
Recovery applies it only if the on-disk page-LSN is older
(`bm_apply_page_image`, page-LSN gated -> idempotent,
`xstore.c:3871-3889`). For the merge:

- **L's after-image** carries the merged contents and the inherited
  right-link. Redo restores L even if L's pre-merge image was flushed.
- **Parent's after-image** carries the separator-removed slots. Redo
  restores the parent even if its pre-merge image was flushed.
- **R (the freed page)**: log a **dead-marked after-image** (header
  with `dead=1`, count 0). On redo this stamps R dead so a torn replay
  that re-applies an older live R image is overridden by the
  higher-LSN dead image. Equivalently, because the live default is
  *logical rebuild* (section 5.4), R's physical image is moot. The
  conservative choice for the in-place path is to log R's dead image so
  the page-LSN gating makes "R is dead" win.

The closing dummy `XL_CLR` (`xstore.c:2143-2163`, `undo_next_lsn ==
the begin LSN`) makes a *completed* merge redo to completion and a
*loser* undo skip the interior `XL_PAGE` records -- the Stasis NTA
model. A crash *mid-merge* (NTA open) is handled by redo of whatever
images reached the log plus the fact that the merge made no logical
change to user data: merge only **moves** keys between sibling pages and
**removes** a redundant separator. No row is created or destroyed.

### 5.3 Undo: there is nothing to undo

A merge belongs to **no transaction** (`txn_id 0`, like the split,
`xstore.c:2136`, `2158`). It is a structure modification, not a data
change. ARIES never undoes an NTA's interior; the dummy CLR's
`undo_next_lsn` jumps an enclosing loser's undo *over* the merge's
physical records. So merge undo is a no-op by construction -- the same
property the split relies on. This is why the merge must not be folded
into a user transaction's redo/undo chain.

### 5.4 Idempotence on the live (logical-rebuild) recovery path

The **live** crash default is **not** in-place physiological redo; it
discards a torn base (`reopen=0`) and **rebuilds the tree logically**
by replaying the `XL_UPDATE` log (`test_torn_smo.c`;
`xstore.c:3931-3952`). Under logical rebuild a merge is **invisible**:
the rebuild replays inserts/deletes onto a fresh tree, and merges
re-happen (or not) as a function of the replayed delete sequence and
the live `merge_on` setting during recovery. So the **simplest correct
recovery story for the merge is: keep the live logical-rebuild default,
and the merge needs no recovery support at all on that path** -- it is
reconstructed from the logical log like every other structural state.

The physiological in-place path (`xstore_recover_inplace`,
`xstore.c:3931+`, exercised by `test_inplace_redo`) is where the
`XL_PAGE` merge images matter, and section 5.2 gives the atomicity
recipe for it. This document recommends:

- **Phase A (ship merge-on concurrent):** rely on logical rebuild for
  crash recovery (no merge WAL needed); merges are reconstructed from
  the delete log. This is the lowest-risk path to enabling concurrent
  merge.
- **Phase B (optional):** add the NTA bracket + dead-image logging to
  `merge_level`/`collapse_root` so the in-place physiological path also
  survives a torn merge, gated by `test_torn_smo`-style tests with
  merge enabled.

---

## 6. Page reclamation and the free-list grace period

- `bm_free_pid(R)` (`bufmgr.c:1022-1053`) first **drops any resident
  frame** for R (so a stale resident copy cannot be re-fixed), then
  records R's id. Critically, `merge_level` only calls it *after* R is
  unlinked from both the parent and L's right-link and marked dead, so
  the contract "no live pointer reaches the id" holds
  (`bufmgr.h:158-166`).
- Freed ids do **not** go straight to the reusable freelist; they go to
  a **quarantine** (`bufmgr.c:121-131`). `bm_reclaim_quarantine`
  (`bufmgr.c:1056-1095`), called at the **start of the next merge**
  (`btree.c:1300`), drains the previous epoch's quarantine to the
  freelist. This is the RCU-like grace period: a latch-free chaser that
  read R's id this epoch is guaranteed to have finished (or restarted)
  by the next merge epoch -- chasers never park indefinitely on a freed
  page (`drop_resident` waits out an in-flight pin, `bufmgr.c:819-828`).
- A page unlinked *now* is therefore never reissued for fresh contents
  while a chaser that read its id *this* epoch is still in flight. This
  is the design's substitute for full epoch/RCU on the page itself and
  is the key reason the "deleted but linked" tombstone state plus a
  one-epoch quarantine suffices.

**Interaction with the section-2.3 fix.** The internal-level dead/fence
check is what makes the quarantine *enough*: a chaser that read R's id
will, on latching R, see `dead` (R still resident, this epoch) or, if R
was already dropped and the id not yet reissued, fail the fix and
restart. The id cannot be reissued *and* refilled with unrelated
contents until the next epoch, by when the chaser is gone.

---

## 7. Test plan

### 7.1 Unit / single-threaded (exists, keep and extend)

`test_btree_delete_merge.c`:
- **Scenario 1** `test_shrink_and_reclaim`: delete ~94% (sparse
  survivors), assert height shrank, merges>0, reclaimed>0, bufmgr
  freed>0, every survivor found, scan exactly the survivors ordered.
- **Scenario 2** `test_collapse_to_root`: delete every key, assert
  collapse to height 1, interior pages reclaimed, then rebuild reusing
  freed ids without aliasing.
- **Scenario 3** `test_no_bloat_churn`: sliding window insert+delete,
  assert footprint bounded (freed>0, reissued>0, merges grew).
- **Add**: an explicit **cascade** test that forces a multi-level
  parent underflow (delete to leave a single internal child at two
  levels) and asserts `collapse_root` runs more than once and height
  drops by >=2; assert from-root **descent** reachability of every
  survivor (not just scan), since the storage doc notes the
  single-threaded stress already validates descent reachability
  (`M_SQLXTC_STORAGE.md:809-813`).

### 7.2 MT stress (the gate that must go green with merge ON)

`test_btree_delete_merge.c` **scenario 4** `test_concurrent_merge`
currently runs with `bt_set_merge_enabled(.., 0)` (`btree.c` test line
sets it off) and asserts `merges == 0`. After the section-2.3 fix:

- Flip it to `bt_set_merge_enabled(.., 1)`.
- Keep the **anchor set** invariant: 500 untouched keys must all be
  present with correct values after the storm (the gate against a merge
  dropping/aliasing live data).
- Add a **churn-key-gone** assertion: after the last round every churn
  key must be **absent** (the exact bug the doc reports -- a deleted key
  surviving). Today the test cannot assert this with merge on; after
  the fix it must.
- Assert the final scan is strictly ascending and yields exactly the
  anchors.
- Add a parallel **reader** cohort (like `test_btree_mt.c`) probing
  anchors during the storm: 0 wrong values.

`test_btree_mt.c` extension: add deleter procs alongside the writers so
inserts (splits) and deletes (merges) race continuously, with the
anchor-survival and ordered-scan gates.

Run all MT tests under the executor with the page-provider live and a
**tiny pool** (forces eviction/reload mid-SMO) and **tiny pages**
(forces frequent merges) -- the existing scenario-4 parameters
(`PAGE_SZ 512`, `n_frames 48`) are good.

### 7.3 Crash recovery (torn merge SMO)

- **Phase A (logical rebuild default):** extend `test_torn_smo.c` with
  a delete-heavy workload (insert N, delete most, with merge enabled on
  the live tree), crash without checkpoint, recover by logical rebuild
  (reopen=0), assert exactly the survivors reappear, ordered. Because
  merge is invisible to logical rebuild, this proves the merge does not
  corrupt the *log* (it must not -- a merge writes no `XL_UPDATE`).
- **Phase B (in-place physiological redo):** mirror `test_inplace_redo`
  for the merge: build, delete-to-merge with the NTA bracket logging
  `XL_PAGE` images for L/parent and a dead-image for R, crash
  mid-merge (drop the pool with the NTA open), recover **in place**
  (`xstore_recover_inplace`), assert the structure is repaired
  (page-LSN gated apply count > 0) and a from-root descent + scan are
  correct.

### 7.4 DST simulation harness

If the project's deterministic-simulation harness (the
forced-fcontext / musl coroutine CI job in `AGENTS.md`, and the
`xtc_exec` multi-loop executor the MT tests already use) supports
seeded interleavings, drive a **schedule-fuzzed** version of scenario 4:
a fixed seed selects the interleaving of insert/delete/merge/move_right
steps, so a failing race is reproducible. The merge's correctness rests
entirely on interleavings, so a deterministic replay of the
`xtc_exec`/`xtc_proc` cooperative schedule (already used by
`test_btree_mt.c` and `test_concurrent_merge`) is the right harness:
run thousands of seeds asserting the anchor + churn-gone + ordered-scan
invariants. Wire it the same way `test_concurrent_merge` spawns
churners on `xtc_exec` loops, but with a seeded yield schedule.

### 7.5 Sanitizers

Per `AGENTS.md`, run the full suite under ASan and UBSan
(`make check` with the sanitizer `CFLAGS`), and the forced-fcontext
job. The merge's stack scratch buffers (`btree.c:1078-1080`,
`btnode.c` merge/split scratch) and the over-aligned-frame allocation
rule are the UBSan-sensitive spots; the merge adds no new over-aligned
struct, so the existing `__os_aligned_alloc` discipline is unaffected.

---

## 8. Risk assessment (honest)

**High-confidence, low-risk:**
- The merge is single-mutator proven (scenarios 1-3 pass today).
- The node-level `btnode_merge`/`merge_fits` are pure, read-before-
  write, and unit-testable in isolation.
- The quarantine grace period is implemented and exercised by the
  reuse-without-aliasing gate in scenario 2.

**Medium-risk:**
- The section-2.3 fix (internal-level dead/fence revalidation) is the
  crux. It is a small, local change to four descent loops, but the
  *argument* that it closes the race depends on the merge's B-link
  invariant holding under every interleaving. The bounded-retry +
  SMO-locked authoritative fallback (already present for delete) is the
  safety net: even if a rare interleaving slips past the latch-free
  checks, the authoritative pass under `bt->smo` is deterministic.
- `child_for_key` returning a wrong subtree mid-cascade is the subtle
  case; the fix must be validated by the schedule-fuzzed DST runs, not
  just the unhinted MT stress (which found the bug but cannot prove
  absence).

**Higher-risk / deferred:**
- In-place physiological redo of a torn merge (Phase B) is genuinely
  hard to get idempotent across the cascade (multiple parents, root
  collapse, superblock root-pid ordering). It is **not** required to
  enable concurrent merge, because the live recovery default is logical
  rebuild. Defer it behind its own test and keep merge WAL logging
  off the critical path until then.
- Performance: enabling merge adds `bm_reclaim_quarantine` + a
  re-descent per underflowing delete. Under a delete-heavy storm this
  is more `bt->smo` contention. Mitigation: the 1/4 hysteresis keeps
  merges rare, and merge stays best-effort. Measure with the existing
  `bm_writeback_bench`-style harness before flipping the default.

**Known non-goals (kept simple):**
- No left-merge (only right-merge into L); no cousin merge across
  different parents. This trades maximal compaction for a simple
  top-down latch order. Documented at `btree.c:1145-1150`.
- No rebalancing/key-redistribution (borrow from sibling) -- only full
  merge. Simpler, and the underflow threshold makes borrow's marginal
  benefit small.

---

## 9. Phased implementation plan

**Phase 0 -- instrument and reproduce (no behavior change).**
Add the schedule-fuzzed DST harness (section 7.4) around the existing
`test_concurrent_merge` body, with `merge_on` ENABLED, asserting the
churn-key-gone invariant so the current race **fails reproducibly**
under a fixed seed. This is the regression oracle.

**Phase 1 -- the descent fix (closes the race).**
Add `btnode_is_dead` + `btnode_below_lo_fence` + `move_right`
revalidation at **every internal level** of `bt_insert_fast`
(`btree.c:480-512`), `bt_delete`'s descent (`btree.c:1411-1434`),
`descend_shared` (`btree.c:803-848`), and `cursor_descend`
(`btree.c:1591-1640`). On a hit: restart from root (bounded retries),
then escalate to the SMO-locked authoritative pass (insert: fall to
`bt_insert` SMO path; delete: existing fallback; lookup: SMO-locked
re-descent). Re-run Phase 0 seeds until green; run ASan/UBSan/fcontext.

**Phase 2 -- enable concurrent merge by default.**
Flip `merge_on` default to ON in `bt_open` (`btree.c:330`-area init) or
have `xstore` enable it, and change `test_concurrent_merge` to
`bt_set_merge_enabled(.., 1)` with the new churn-gone + reader gates.
Update `btree.h` / `btree.c` comments and `M_SQLXTC_STORAGE.md:815-842`
to record the race as closed.

**Phase 3 -- cascade and recovery hardening.**
Add the multi-level cascade unit test (7.1) and the Phase-A logical-
rebuild crash test (7.3). Keep WAL-merge logging off; rely on logical
rebuild.

**Phase 4 (optional) -- in-place physiological merge redo.**
Add the NTA bracket + `XL_PAGE` (L, parent) + dead-image (R) logging to
`merge_level`/`collapse_root`, ordered before the superblock root-pid
update; add the Phase-B torn-merge in-place recovery test. Only pursue
if the in-place restart path is promoted to the live default.

**Phase 5 -- measure and tune.**
Benchmark delete-heavy and churn workloads with merge on vs off;
confirm bounded footprint and acceptable `bt->smo` contention; tune
`BT_MERGE_NUM/DEN` only if thrashing is observed.

---

## 10. References

- Lehman & Yao, "Efficient Locking for Concurrent Operations on B-Trees"
  (1981): right-link + high-key, half-split-then-post, move-right
  recovery. The merge here is the inverse, with the symmetric
  left-absorb recovery (`btnode_below_lo_fence`).
- Mohan et al., ARIES; Stasis nested-top-action model
  (`M_SQLXTC_BDB.md` sec 2.7): physiological redo + dummy CLR for
  crash-atomic multi-page SMOs.
- LeanStore `BTreeNode` (cited in `btnode.c:11-15`): slotted layout,
  fence-derived prefix, per-slot 4-byte head.
- In-tree: `docs/M_SQLXTC_STORAGE.md` sec 3-4 and the "Delete merge /
  page reclaim, and the concurrent-merge race" section (lines 800-842);
  `docs/M_SQLXTC_WAL.md`.

### Critical Files for Implementation
- /home/gburd/ws/xtc/examples/06_sqlxtc/btree.c - The merge SMO lives here (`bt_merge` 1280-1370, `merge_level` 1067-1278, `collapse_root` 982-1024); the four latch-free descents that need internal-level dead/fence revalidation (`bt_insert_fast`, `bt_delete`, `descend_shared`, `cursor_descend`); `merge_on` default flag.
- /home/gburd/ws/xtc/examples/06_sqlxtc/btnode.c - Node-level merge primitives (`btnode_merge` 418-470, `btnode_merge_fits` 380-416, `btnode_mark_dead`/`is_dead`, `btnode_below_lo_fence` 626-637, `btnode_beyond_hi_fence`) that the descent checks call.
- /home/gburd/ws/xtc/examples/06_sqlxtc/test_btree_delete_merge.c - Scenario 4 `test_concurrent_merge` is the gate to flip to merge-ON and extend with the churn-gone + reader invariants; the DST/schedule-fuzz harness wraps this body.
- /home/gburd/ws/xtc/examples/06_sqlxtc/xstore.c - SMO WAL hooks (`xs_smo_begin/page/end` 2096-2182) and the recovery paths (logical rebuild vs in-place `XL_PAGE` redo, 3871-3952) that any Phase-4 merge WAL logging plugs into.
- /home/gburd/ws/xtc/examples/06_sqlxtc/bufmgr.c - `bm_free_pid` (1022-1053), `bm_reclaim_quarantine` (1056-1095), and the quarantine grace-period machinery that makes tombstone reclamation safe for reissued page ids.
