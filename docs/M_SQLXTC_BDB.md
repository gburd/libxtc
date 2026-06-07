# sqlxtc vs. Berkeley DB: a maturation plan toward ARIES UNDO/REDO

This compares the sqlxtc storage engine (examples/06_sqlxtc) with Berkeley
DB (`~/ws/libdb`), a mature, fully transactional, WAL-logged ARIES system,
component by component -- ignoring the async/libxtc plumbing, which is
orthogonal.  It calls out where sqlxtc is LESS mature (and should adopt a
known technique), where it is MORE mature (MVCC+SSI, the buffer pool, the
log pipeline), and from that lays out the plan to finish sqlxtc with the
best known techniques.  The headline decision, driven by review: sqlxtc
moves from its current redo-only model to a full **UNDO/REDO** system with
page LSNs, physiological logging, and compensation log records (CLRs).

Berkeley DB references below are concrete (file:function); sqlxtc
references are to examples/06_sqlxtc.


## 1. Component map

| Concern              | sqlxtc                          | Berkeley DB                         |
|----------------------|---------------------------------|-------------------------------------|
| Log                  | wal.c                           | src/log/, src/dbinc/log.h           |
| Recovery             | xstore.c `xstore_recover`       | src/env/env_recover.c `__db_apprec` |
| Checkpoint           | wal.c `wal_checkpoint` (compact)| src/txn/txn_chkpt.c                 |
| Buffer manager       | bufmgr.c                        | src/mp/, src/dbinc/mp.h             |
| B-tree               | btree.c, btnode.c               | src/btree/, src/dbinc/btree.h       |
| Transactions/isolation | xstore.c (MVCC + Cahill SSI)  | src/txn/, src/lock/ (2PL [+ MVCC SI]) |
| Page format          | btnode_hdr (no LSN)             | PAGE.lsn (db_page.h:238)            |
| Recovery record gen  | hand-coded payloads             | .src files + dist/gen_rec.awk       |


## 2. Where the engines differ (and what to adopt)

### 2.1 Logging and the recovery model -- the central gap

**BDB** is STEAL + NO-FORCE with full ARIES.  Every page header carries a
`DB_LSN lsn` (db_page.h:238).  The mpool enforces write-ahead at the point
of eviction: `__memp_pgwrite` (mp_bh.c) does `__log_flush(env, &page_lsn)`
before writing any dirty page, so a page never reaches disk before its log.
Each operation logs a *physiological* record carrying both REDO and UNDO
images and the affected pages' prior LSNs (e.g. the `__bam_split` record in
btree.src carries `llsn/rlsn/plsn` and `pg` -- the full before-image of the
split page).  Recovery (`__db_apprec`) runs three passes: OPENFILES
(forward from the last checkpoint, build the transaction list), BACKWARD
(undo losers), FORWARD (redo committed work), each dispatched op gated by
comparing the on-disk page LSN against the record's LSN (`CHECK_LSN`).  The
checkpoint writes a `__txn_ckp` record whose `ckp_lsn = min(oldest active
txn begin_lsn, oldest dirty-page recLSN)` and truncates the log behind it.

**sqlxtc** is redo-only.  Versions are immutable and append-only and a
transaction's writes are buffered in memory (`xstore_ctx.wbuf`) and applied
to the tree only at commit (xstore.c:1463), so **uncommitted data never
reaches the tree or disk** -- there is nothing to UNDO for a transaction.
The log records are logical version inserts `[ts][n]{rowid,tableid,flags,
value}` (xs_wal_put).  There are no page LSNs.  Recovery (`xstore_recover`)
rebuilds the whole tree by replaying the log onto a FRESH page file;
idempotence comes from the immutable version keys, not page-LSN gating.
The page file is a rebuildable cache, not a trusted recovery base.  The
in-WAL checkpoint (`wal_checkpoint`) bounds the log by rewriting it as a
CHECKPOINT record plus a dump of the live row set.

**Consequences of the difference (why BDB's model is more mature here):**

  1. **Cold restart cost.**  sqlxtc replays the entire live data set into a
     fresh tree on every crash recovery.  BDB redoes only from the last
     checkpoint's `ckp_lsn` onto the *existing* page file -- O(log tail),
     not O(database).  For a large database this is the difference between
     seconds and minutes/hours.

  2. **Checkpoint cost.**  sqlxtc's checkpoint rewrites the live set
     (O(live data) per checkpoint).  BDB's fuzzy checkpoint just flushes
     the current dirty set (the trickler is already doing this
     incrementally) and advances the truncation horizon -- O(dirty), and
     decoupled from log size.

  3. **Transaction size.**  sqlxtc must hold a whole transaction's writes
     in memory until commit (the write buffer).  A million-row UPDATE
     would exhaust memory.  A STEAL engine writes uncommitted changes to
     disk and UNDOes them on abort -- arbitrarily large transactions.

  4. **Torn structure.**  We already hit this (test_torn_smo): logical
     redo onto a partially-flushed tree corrupts.  We worked around it by
     discarding the base and rebuilding.  ARIES *repairs* a torn page in
     place by redoing physiological records gated by the page LSN -- no
     rebuild, and it composes with (1)-(3).

**Adopt:** page LSNs, physiological logging, and ARIES 3-pass recovery.
This is the bulk of the plan (Section 4).

### 2.2 Buffer manager -- sqlxtc is more modern, but missing the LSN hook

sqlxtc's pool is LeanStore-shaped (Leis 2018): pointer **swizzling** (a
resident child is a tagged pointer -- resolving it is a load, no hash
lookup), a **cooling** stage (2Q-style probationary FIFO, Johnson/Shasha
1994), CLOCK second-chance, a double-write buffer for torn-page atomicity,
and a trickler that writes dirty pages oldest-first (a recLSN proxy).
BDB's mpool is an older generation-priority LRU (mp.h `MPOOL_LRU_*`) with a
hash-bucket lookup on every page access and a 10% priority boost for dirty
buffers (mp_fput.c).

So sqlxtc's *replacement policy and resident-page path are more advanced*.
What BDB has that sqlxtc lacks is the **write-ahead enforcement hook**:
`__memp_pgwrite` flushes the log to the page's LSN before writing it.
sqlxtc relies instead on "the commit that produced a version is durable
before the version's page is dirtied" plus the double-write buffer.  Once
sqlxtc has page LSNs (Section 4), `flush_frame` must gain the same
log-flush-to-page-LSN gate, and the trickler's "oldest dirty first"
becomes a true recLSN ordering driving checkpoint truncation.

### 2.3 B-tree -- different SMO recovery, similar latching idea

Both use latch/lock coupling down the tree.  sqlxtc couples fiber-yielding
arwlocks (lighter than BDB's database-level page locks via the lock
manager).  The real difference is **SMO recovery**.  BDB logs a split as
one physiological record with the before-image and the children's prior
LSNs, and `__bam_split_recover` redoes or undoes it idempotently by LSN.
sqlxtc does not log SMOs at all -- recovery rebuilds the tree logically, so
a split is implicit in re-inserting the rows.  To trust the page file,
sqlxtc must log SMOs physiologically and treat each as an ARIES **nested
top action** (a dummy CLR at the end so a completed SMO is never undone,
only redone to completion).  This is the standard fix for the torn-SMO
problem and removes the rebuild-from-log fallback.

### 2.4 Concurrency / isolation -- sqlxtc is MORE mature

BDB's concurrency is two-phase locking with a deadlock detector
(src/lock/); snapshot isolation was bolted on later via `DB_MULTIVERSION`
(per-page version copies in the mpool).  It does **not** provide
serializable snapshot isolation.

sqlxtc implements **Cahill SSI** (Cahill/Roehm/Fekete, SIGMOD 2008/TODS
2009): PostgreSQL-style MVCC visibility plus rw-antidependency tracking and
the "dangerous structure" (pivot) abort test, giving true SERIALIZABLE
without read locks.  This is a generation ahead of BDB's concurrency and
should be preserved as-is; the ARIES work below sits underneath it (ARIES
recovers the physical B-tree; MVCC+SSI provides isolation on top).  Note
the clean split of duties: MVCC means there is no per-transaction *data*
undo (an abort just never makes versions visible); ARIES UNDO is for the
*physical* page operations of stolen/incomplete work, not for visibility.

### 2.5 Log-record engineering -- adopt BDB's codegen discipline

BDB defines each log record in a `.src` file (field name, type, REDO/UNDO
role) and `dist/gen_rec.awk` generates the read/write/print/recover
scaffolding plus a standard header (type, txnid, prev_lsn).  sqlxtc
hand-codes record payloads and discriminates types with an `n ==
0xFFFFFFFF` sentinel.  As sqlxtc grows record types (CHECKPOINT, page
insert/delete, split, merge, txn begin/commit/abort, CLR), a small
generator + a standard header (type, txn id, prev_lsn) is worth adopting --
it makes adding a record type a one-line change and keeps read/write/redo/
undo in lockstep.

### 2.6 Log pipeline -- sqlxtc is AETHER-shaped (more modern)

sqlxtc's WAL is a decoupled committer/writer: committers hand records to a
group-commit writer proc that batches many commits into one fsync -- the
shape of Aether (Johnson et al., VLDB 2010: decoupled log insert from
flush, consolidate concurrent committers).  BDB's group commit (the
`lp->commits` wait queue in log_put.c) is the older "everyone waits on the
flush mutex" style.  sqlxtc's logging path is the more scalable design;
keep it.  (When STEAL arrives, the writer also carries the physiological
page records, not just commit records.)

### 2.7 Stasis -- the textbook reference, and what we borrow from it

Stasis (Sears & Brewer, OSDI 2006; the tree under
`/home/gburd/src/stasis-aries-wal`) is a pedagogically pure ARIES.  Where
BDB is the production reference, Stasis is the clean-room one, and two of
its mechanisms shape our plan directly:

  - **CLRs written during undo, and replayed during redo.**
    `recovery2.c` writes a compensation record for every reversed update
    (the CLR's `prevLSN` skips the compensated record, so undo is bounded
    and resumable across repeated crashes), and during the redo pass a CLR
    re-applies its compensating action.  BDB instead omits CLRs and makes
    undo idempotent via page-LSN gating.  We follow Stasis here -- our
    XL_CLR carries `undo_next_lsn`, is redo-only, and is closed by an
    XL_END (S3, implemented; see test_recover_undo).

  - **Nested Top Actions + a dummy CLR for structure modification.**
    Stasis brackets a multi-page structural change (a hash/B-tree split)
    in `TbeginNestedTopAction` / `TendNestedTopAction`; the end writes a
    dummy CLR whose redo re-runs the operation and whose presence makes
    undo skip the interior physical records.  The whole SMO is therefore
    atomic with respect to recovery -- redone if it finished, never half
    undone.  This is exactly the device sqlxtc needs for crash-atomic
    B-tree splits, and is the mechanism named in S3's remaining work.

Stasis also uses STEAL/NO-FORCE with page-LSN-gated redo, a dirty-page
table + transaction table rebuilt by analysis, fuzzy checkpointing via a
continuous `min(applied, page recLSN, xact recLSN, flushed)`, and
`lsnFree` / reorderable pages for updates whose final state -- not their
intermediate order -- is what matters.  We adopt the first three (S3/S4)
and skip lsnFree: our MVCC keyspace makes superseded versions GC-able
instead.  Stasis's log manager is a simple timeout-batched group commit
(`groupForce.c`) with no Aether-style buffer scaling -- our pipeline
(below) is more modern than Stasis's here.

### 2.8 Three-lineage scorecard: element -> who has it -> sqlxtc status

| Element | BDB | Stasis | Aether | sqlxtc |
| --- | --- | --- | --- | --- |
| Page LSN at fixed offset | yes | yes | -- | in code (S1) |
| Physiological full-page redo (XL_PAGE) + gated apply | -- | -- | -- | in code (mechanism: xl_enc_page + bm_apply_page_image) |
| In-place recovery, clean case (trust clean base) | yes | yes | -- | in code (clean-restart fast path) |
| Write-ahead enforce (flush log before dirty page) | yes | yes | -- | in code (S1) |
| Log header type/txn/prev_lsn | gen'd | yes | -- | in code (S2) |
| 3-pass recovery | yes | yes | -- | in code, reshaped (S3): redo-all + undo-losers |
| CLRs + UndoNextLSN | skipped | yes | -- | **in code (S3), Stasis-style** |
| Nested Top Action + dummy CLR (atomic SMO) | n/a | yes | -- | planned (S3 remainder) |
| Physiological page/SMO logging | yes | yes | -- | planned (deferred from S2) |
| STEAL/NO-FORCE | yes | yes | -- | diverged: NO-STEAL today; STEAL = S5 |
| Dirty-page table + true recLSN | yes | yes | -- | partial (dirty_seq proxy); S4 |
| Fuzzy checkpoint + min-recLSN truncation | yes | yes | -- | planned (S4) |
| Log-record codegen from .src | yes | no | -- | planned (deferred from S2) |
| Group commit | yes | yes | -- | in code |
| Flush pipelining (no ctx-switch) | -- | -- | yes | in code, via fibers (committer yields, loop runs peers) |
| Early Lock Release | -- | -- | yes | not adopted (candidate) |
| Consolidation array / decoupled buffer fill | -- | -- | yes | not needed: message-passing single-writer has no shared-buffer mutex |
| lsnFree / reorderable pages | -- | yes | -- | not adopted (MVCC GC instead) |
| Double-write (torn-page atomicity) | no | no | -- | **in code, beyond all three** |

Reading of the table: on physical recovery we are converging on the
BDB/Stasis ARIES core (page LSNs done, CLRs done, physiological logging +
NTA + STEAL remaining); on the log *pipeline* and the *isolation* model we
are ahead of all three (fiber flush-pipelining, MVCC+SSI, double-write).
Aether contributes throughput ideas, of which flush pipelining we get for
free from the runtime and the buffer-contention fixes we do not need;
early lock release is the one Aether idea still on the table.


## 3. Scorecard

More mature in sqlxtc (keep, do not regress):
  - Isolation: Cahill SSI (serializable) vs BDB 2PL/snapshot.
  - Buffer pool: LeanStore swizzling + cooling + CLOCK vs priority-LRU.
  - Log pipeline: Aether-style decoupled group commit.
  - Torn-page atomicity: InnoDB-style double-write buffer (BDB has none;
    it relies on redo, which needs page LSNs to be safe).

More mature in BDB (adopt):
  - STEAL/NO-FORCE with page LSNs + physiological redo/UNDO + CLRs.
  - ARIES 3-pass recovery from a trusted, checkpointed page-file base.
  - Fuzzy checkpoint with dirty-page-table / min-recLSN log truncation.
  - Log-record code generation from declarative `.src` files.
  - dbreg-style file registration in the log (for multi-file recovery).


## 4. The plan: move sqlxtc to ARIES UNDO/REDO

Goal: the B-tree becomes a recoverable, STEAL/NO-FORCE physical structure
with page LSNs and physiological logging; MVCC+SSI stays on top as the
isolation model.  CLRs are written during the UNDO pass so undo is logged,
idempotent, and bounded across repeated crashes (the classic ARIES device;
note BDB instead made undo idempotent via page-LSN gating and skipped CLRs
-- we choose CLRs for restart-bounded undo, the more robust textbook form).

Staged so each step is independently testable and leaves the tree green:

  **S1 -- Page LSNs + WAL enforcement at the buffer manager.  DONE.**
  Add `uint64_t page_lsn` to `btnode_hdr`.  Give the WAL a real monotonic
  LSN per record (it has lsn framing already).  In `flush_frame`, before
  writing a dirty page, flush the log up to that page's LSN (BDB
  `__memp_pgwrite`).  No behavior change yet; this is the substrate.
  Test: a page is never written while its log tail is unflushed (assert in
  flush_frame under a fault probe).

  **S2 -- Log record format + transaction-structured logging.  DONE
  (logical records; physiological page/SMO logging deferred).**
  Define a standard log header (type, txn id, prev_lsn) and records for
  page insert, page delete, and the SMO (split/merge) carrying redo + undo
  images and the affected pages' prior LSNs.  Stamp each modified page's
  LSN with the record LSN.  Add a tiny generator (`.src` style) so the
  record set is declarative.  Keep the existing logical records during the
  transition.  Test: a single page op round-trips through write/redo/undo.

  Landed as two commits: (1) the xlog codec -- a common header plus
  XL_BEGIN/UPDATE/COMMIT/ABORT/CLR/CHECKPOINT/END records, bounds-checked
  serialize/parse, unit-tested (test_xlog); (2) wiring the live WAL commit,
  recovery, and checkpoint paths onto it.  A committed transaction is now
  one WAL frame of [XL_UPDATE...][XL_COMMIT] (atomic durability, group
  commit preserved); the checkpoint record is XL_CHECKPOINT, owned by the
  storage layer (the generic WAL no longer knows the format).  The records
  are LOGICAL (a version's value), not yet physiological page images: that
  -- and the redo `.src` generator -- is a later refinement needed only for
  trusting a torn page file in place (S3's "recover in place" goal).  The
  UPDATE undo image and CLR record type are defined and tested but unused
  until losers can persist (S5).

  **S3 -- Recovery driver: redo-all + undo losers with CLRs.  DONE
  (undo dormant under NO-STEAL; in-place torn-SMO repair deferred).**
  Replace `xstore_recover`'s rebuild-onto-fresh-tree with analysis (rebuild
  the dirty-page table + transaction table from the last checkpoint), redo
  (from min-recLSN, gated by page LSN), and undo (roll back losers, writing
  CLRs with UndoNxtLSN).  Treat each SMO as a nested top action (dummy CLR
  so a finished SMO is redone, never undone).  This makes the page file the
  trusted base and retires test_torn_smo's "discard + rebuild" path.
  Test: torn-SMO crash recovers in place (no rebuild); a partial UNDO that
  is itself interrupted re-recovers correctly (CLR/UndoNxtLSN bound).

  Landed: xstore_recover now redoes every update while tracking the
  still-active transactions, then undoes whatever is still active at end of
  log -- the losers -- deleting each version newest-first and writing one
  XL_CLR (undo_next_lsn = next LSN to undo, 0 when done) plus a closing
  XL_END.  The active-transaction table is O(1) on the NO-STEAL path (a
  frame opens and closes its own transaction), so the structure imposes no
  per-history memory.  Because NO-STEAL never logs a loser, the undo pass
  is dormant in the live engine (behavior unchanged, every storage test
  passes); test_recover_undo synthesizes the loser log a STEAL engine would
  leave and proves redo-winner / undo-loser / CLR + END.  STILL DEFERRED:
  redo is not yet page-LSN-gated against a trusted torn base, so recovery
  keeps rebuilding onto a fresh page file; "recover in place" needs the
  physiological page/SMO logging held back from S2.  The undo + CLR path
  this delivers is exactly what S5 (STEAL) plugs real losers into.

  Progress since: the physiological-redo mechanism is built and tested on
  both ends -- XL_PAGE (a full-page after-image record) and
  bm_apply_page_image (page-LSN-gated, idempotent apply).  The CLEAN case
  of in-place recovery is wired: a clean shutdown flushes the base and
  marks its superblock clean + records the commit clock, and the next
  open trusts that base and skips replay (test_clean_restart proves it by
  deleting the WAL).  A crash still rebuilds from the log.  REMAINING for
  the crash case: emit XL_PAGE from the B-tree SMO path inside a
  nested-top-action bracket and run a physiological redo pass so a torn
  base is repaired in place rather than discarded -- this couples with
  the STEAL write-ordering (log changes physiologically as applied,
  commit last), so it is best done together with S5.

  **S4 -- Fuzzy checkpoint + min-recLSN truncation.**
  Checkpoint writes the dirty-page table + active-txn table + `ckp_lsn =
  min(oldest active txn LSN, oldest dirty-page recLSN)` and flushes the
  dirty set incrementally (the trickler already orders by dirty_seq -- make
  that a true recLSN).  Truncate the log behind `ckp_lsn`.  This replaces
  the O(live-data) compaction with an O(dirty) checkpoint, decoupling
  checkpoint cost from database size.  Test: checkpoint cost is
  proportional to dirty pages, not live rows; log truncates to the recLSN
  horizon; recovery starts at ckp_lsn.

  **S5 -- STEAL for large transactions.**
  Allow the per-transaction write buffer to spill uncommitted versions into
  the tree (marked uncommitted) when it grows past a threshold, and UNDO
  them on abort/crash via the S2/S3 machinery (CLRs).  This removes the
  in-memory transaction-size ceiling.  MVCC visibility already hides
  uncommitted versions from other transactions, so spilling is invisible to
  readers.  Test: a transaction larger than the write-buffer cap commits
  and aborts correctly; crash mid-large-transaction undoes the spill.

Sequencing note: S1-S4 deliver the mature recovery + bounded-log story
(fast restart, cheap checkpoints) without changing the data model.  S5 is
the one that needs UNDO of data and is the clearest justification for CLRs;
it can follow once S1-S3 are solid.  Throughout, MVCC+SSI and the
LeanStore buffer pool are preserved -- ARIES is added underneath, not in
place of them.


## 5. Portability lessons from Berkeley DB for libxtc

BDB ran on every Unix, Windows, VxWorks, QNX, mainframes -- decades of
portability discipline.  libxtc already uses the structurally cleaner
approach BDB's evolution argues for (C11 `_Atomic` instead of per-CPU
test-and-set asm; per-backend `.c` files chosen at configure time instead
of `#ifdef` thickets).  The worthwhile, concrete adoptions:

  1. **A `RETRY_CHK`-style macro for transient syscall failures.**  BDB
     wraps every raw syscall in a bounded retry on EINTR/EAGAIN/EBUSY/EIO
     (os.h `RETRY_CHK`).  libxtc handles EINTR in ~16 spots ad hoc; a
     single macro in the os/io layer (and the blocking-offload pool) would
     make the blocking paths uniformly robust.  Highest-value, lowest-cost.

  2. **A CPU-relax / PAUSE hint in spin loops.**  libxtc's CAS spin loops
     (bufmgr `claim_frame`, the lock_lr fast paths) only have
     `sched_yield`.  A `__os_cpu_relax()` (x86 `pause`, arm `yield`, a
     no-op elsewhere) reduces spin power/contention.  BDB carries these per
     arch (mutex_int.h `MUTEX_PAUSE`); a one-macro C11-friendly version is
     enough.

  3. **Override callback hooks (BDB `DB_GLOBAL(j_*)`).**  BDB lets an
     embedder replace `pread`/`pwrite`/`fsync`/`malloc`.  A small optional
     hook table in libxtc's os layer would help testing (fault injection
     without ptrace) and embedding (e.g. PG-on-xtc supplying its own I/O).

  4. **Configure-time CPU feature detection.**  Extend the existing
     configure backend-selection to probe cache-line size, the PAUSE
     instruction, `O_DIRECT`/`F_FULLFSYNC`, etc., rather than assuming.

  5. **A handful of safe libc fallbacks.**  Not the 30-function clib BDB
     carries, but `snprintf` bounds discipline and an internal getopt for
     the examples would help on minimal toolchains.  (libxtc depends only
     on libc by design; keep that.)

  6. **Alignment macros for shared-memory regions.**  Not needed today
     (libxtc is single-process), but the PG-on-xtc direction will put data
     structures in cross-process shared memory; BDB's per-arch alignment
     rules (mutex_int.h `MUTEX_ALIGN`) are the cautionary precedent.

Items 1 and 2 are small and worth doing soon; 3-6 are situational.


## 6. Decision

sqlxtc adopts the Berkeley DB / ARIES recovery architecture -- STEAL/
NO-FORCE, page LSNs, physiological redo + UNDO, CLRs, fuzzy checkpoints,
recLSN log truncation -- via the staged plan in Section 4, while keeping
the places it is already ahead of BDB: Cahill serializable SSI, the
LeanStore cooling/swizzling buffer pool, the Aether-style log pipeline,
and double-write torn-page protection.  The result is a from-scratch,
xtc-native engine that matches a mature transactional store on recovery
and exceeds it on concurrency and cache management.
