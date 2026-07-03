# M_SQLXTC_STEAL -- making sqlxtc STEAL + NO-FORCE (verified plan)

Scope: examples/06_sqlxtc.  Read-only audit of the ACTUAL code plus a
bounded implementation plan.  Every claim cites file:function.  ASCII only.

------------------------------------------------------------------------------
## HEADLINE

The write-ahead-log enforcement hook and the page-LSN-gated redo path are
ALREADY WIRED.  The undo/CLR machinery is ALREADY BUILT and ALREADY FIRES
on a real workload.  A form of STEAL (payload spill of an oversized
transaction, logged and undone with CLRs) is ALREADY LANDED and tested by
test_steal.c.  What does NOT exist is *page-level* STEAL of the real MVCC
version records: the live crash path (engine.c:sx_storage_open) still
rebuilds the tree LOGICALLY onto a freshly-truncated page file
(engine.c:344 calls xstore_recover, not xstore_recover_inplace), so
whatever uncommitted version pages a dirty frame flushed to the base
before a crash are simply thrown away and rebuilt from the bounded log.
The base is never trusted after a crash, so page-level STEAL is a no-op
for correctness today.

Recommendation (Section 5): do NOT pursue full page-level STEAL.  The
engine is MVCC append-only with a bounded per-transaction write buffer,
and the ONE case page-level STEAL exists to serve -- a single transaction
whose dirty set exceeds the buffer pool -- is already handled by the
spill-to-staging mechanism (xstore.c:xs_spill_payloads), which bounds
resident footprint AND logs undo.  Full STEAL would require trusting the
torn base in place (xstore_recover_inplace) for ALL crashes, which the
BDB doc (docs/M_SQLXTC_BDB.md, S3) shows needs physiological logging of
every non-split row write (the measured "13% missing rows" problem) --
a large, data-integrity-critical change with little payoff for this
data model.  Keep NO-STEAL-of-versions + spill; the full page-STEAL
plan is given in Section 2/6 in case it is required.

------------------------------------------------------------------------------
## 1. PRESENT vs MISSING table

Requirement columns: HAVE = built and on the live path; PARTIAL = built
but not the live default, or built for a narrower case; MISSING = absent.

| # | STEAL / NO-FORCE requirement | Status | Evidence (file:function) |
|---|------------------------------|--------|--------------------------|
| A | Uncommitted data may reach pages | PARTIAL | Real MVCC versions do NOT: they buffer in xstore.c:xstore_ctx_t.wbuf and apply only in xstore.c:xs_commit_ctx (line ~2735, `bt_insert` loop after `xs_wal_log`).  A COPY of an oversized txn's payloads DOES reach pages under a reserved staging table-id: xstore.c:xs_spill_payloads (`bt_insert(cx->bt, key ... XS_STAGE_TID)`), triggered from xstore.c:xs_buf_write when `wbuf_bytes > XS_SPILL_HI`. |
| B | Write-ahead-log enforcement on eviction | HAVE | Hook defined bufmgr.c:211 (`wal_flush`), installed engine.c:361 (`bm_set_wal_flush(g_xbm, sx_wal_flush_cb, g_xwal)`), and CALLED on every dirty-page write path: bufmgr.c:447 in `flush_frame` (eviction/cooling/checkpoint via lines 596, 1504, 2009) and bufmgr.c:1631 in `tr_prepare` (the trickler).  The callback is engine.c:262 `sx_wal_flush_cb` -> `wal_flush_through`. |
| C | Page-LSN redo gating on a torn base | PARTIAL | Mechanism built and correct: bufmgr.c:1140 `bm_apply_page_image` compares on-disk page LSN vs image LSN (`in_image > on_disk`) and applies only if newer; recovery calls it from xstore.c:xs_recover_cb (XL_PAGE arm, line ~3875) via xstore.c:xstore_recover_inplace (line 3958).  NOT the live default: engine.c:344 uses xstore_recover (logical rebuild on a fresh file), where redo idempotence comes from version-key immutability, not page-LSN gating.  Torn NON-split leaves are not covered (docs/M_SQLXTC_BDB.md S3, "13% missing rows"). |
| D | Undo images logged for every update | PARTIAL | The redo image is logged before apply (xstore.c:xs_wal_log for a committing txn; xstore.c:xs_wal_put for autocommit; xstore.c:xs_wal_emit_stage for a spilled payload).  There is NO explicit before-image: undo of a versioned insert needs none -- reversing it is `bt_delete` of the immutable version key (tableid,rowid,commit_ts).  See xstore.c:xs_undo_loser (line 3788).  So "undo image" = the version key, which every XL_UPDATE carries.  Adequate for the append-only version model; a true before-image would be needed only if updates mutated a row in place, which they do not. |
| E | Undo pass reverses losers via CLRs | HAVE | xstore.c:xs_undo_loser (line 3788): for each loser update newest-first, `bt_delete` the version key, emit XL_CLR with `undo_next_lsn`, then XL_END.  Driven from xstore.c:xstore_recover (line 3899) and xstore_recover_inplace (line 3958) over the ACTIVE (uncommitted) txn table (`r->n_txn`).  Proven to FIRE on a real workload by test_steal.c (asserts `xstore_undo_clrs()` increments) and by test_recover_undo.c. |
| F | recLSN-based redo start / dirty-page table | PARTIAL | Per-page recLSN stamped on the clean->dirty edge: bufmgr.c:1067 (`frame->rec_lsn`).  Horizon query bufmgr.c:1169 `bm_min_rec_lsn` (oldest dirty recLSN), tested in test_redo_page.c.  The trickler writes oldest-recLSN-first (bufmgr.c:1567 comment; dirty_seq/rec_lsn stamped together).  NOT used to START redo: recovery scans the whole (bounded, compacted) log from the front (xstore.c:xstore_recover -> wal_scan).  Live checkpoint is O(live-data) compaction (engine.c:sx_storage_checkpoint -> xstore_checkpoint_wal), not an O(dirty) fuzzy checkpoint with mid-log truncation to the recLSN horizon (docs/M_SQLXTC_BDB.md S4, deferred). |
| G | Torn-page atomicity (double-write) | HAVE | bufmgr.c:329 `dw_protect` writes+fdatasyncs the page to the double-write ring before overwriting home (called from flush_frame bufmgr.c and tr_prepare bufmgr.c:1642); bufmgr.c:363 `dw_recover` replays the ring on reopen (bufmgr.c:774, single-threaded before any proc).  Enabled for the live store: engine.c:311 `o.double_write = 1`.  NOTE: this only protects the in-place base, which the live crash path discards -- so today it is insurance for a path (in-place recovery) not yet the default. |

Summary: B, E, G are HAVE.  A, C, D, F are PARTIAL, and in every PARTIAL
case the mechanism is BUILT and the gap is the same single fact: the live
crash path rebuilds logically from the log instead of trusting the torn
base in place.  Nothing is fully MISSING.

------------------------------------------------------------------------------
## 2. What actually holds STEAL back (and what it would take)

There is NO "NO-STEAL gate" that merely blocks uncommitted data from the
buffer manager.  The buffer manager already evicts any dirty page under
pressure, WAL-gated (requirement B).  The reason uncommitted MVCC
versions do not reach pages is upstream of the buffer manager: xstore
does not PUT a version into the tree until commit.  The version-buffering
in wbuf (xstore.c:xstore_ctx_t.wbuf, applied in xs_commit_ctx) is a
correctness-and-atomicity device (one commit timestamp for the whole
transaction, clean rollback, SSI validation before any write), not a
buffer-pool STEAL policy.  So "let uncommitted data reach pages" means
"apply versions to the tree incrementally, before commit" -- which
changes the commit-atomicity model, not the buffer manager.

Two distinct designs satisfy "STEAL":

  (I) Value-staging STEAL (ALREADY DONE).  Keep versions out of the tree
      until commit, but when a single transaction's buffered payloads
      exceed XS_SPILL_HI (xstore.c:235, 256 KB) spill the oldest payloads
      to a reserved staging table-id and free them from RAM
      (xstore.c:xs_spill_payloads).  Each spill is logged as an XL_UPDATE
      under a fake steal_txn id in a high range that NEVER commits
      (xstore.c:xs_wal_emit_stage, g_steal_txn = 1<<62 at xstore.c:168),
      so recovery redoes it then undoes it with a CLR whether the real
      SQL transaction committed or not.  On commit the payloads are
      re-materialized (xstore.c:xs_commit_ctx calls xs_wrec_ensure per
      entry) and applied at the real commit timestamp; the staging rows
      are left as garbage that recovery/GC removes (staging table-id is
      skipped by checkpoint dump and GC, xstore.c:~2311).  This bounds a
      large transaction's resident footprint AND logs undo -- exactly the
      "spill-to-disk-under-pressure that still logs undo" alternative.
      Proven by test_steal.c (commit / rollback / crash-undone-with-CLRs).

 (II) Page-level STEAL (NOT done, the "full ARIES" reading).  Apply the
      real MVCC versions to the tree incrementally, let the buffer
      manager evict those dirty pages, and on crash TRUST the torn base
      in place, page-LSN-gated redo repairing it and the undo pass
      reversing losers.  This is what BDB/Stasis do.  It requires the
      live crash default to become xstore_recover_inplace AND
      physiological (XL_PAGE) logging of every dirtied page, not just SMO
      pages -- see Section 6.

------------------------------------------------------------------------------
## 3. Change set for FULL page-level STEAL (design II), ordered + bounded

Only pursue this if a hard requirement demands page-level STEAL of real
versions (see Section 5 -- it is likely NOT wanted).  Each increment lists
files, the invariant it must preserve, and its test.

### Increment 0 -- confirm the plumbing (no code)
The hook (B), page-LSN apply (C mechanism), CLR undo (E), double-write (G)
are wired.  Verify with the existing suite: test_inplace_redo,
test_recover_undo, test_redo_page, test_steal, test_torn_smo.
Invariant: nothing regresses.  This increment is documentation only.

### Increment 1 -- physiological logging of every dirtied page
Files: btree.c (row-write path bt_insert_fast), xstore.c
(xs_smo_page-style emit generalized), xlog.c/xlog.h (reuse XL_PAGE).
Today XL_PAGE is emitted ONLY on the SMO/split path (xstore.c:xs_smo_page
via xstore_register_smo).  Emit an XL_PAGE after-image for EVERY dirtied
leaf, not just split pages, so logical XL_UPDATE redo never has to descend
a torn non-split leaf (the documented 13%-missing-rows failure,
docs/M_SQLXTC_BDB.md S3).
Invariant: for every page the crash could tear, the log carries a
page-LSN-stamped after-image at least as new as the page's on-disk state.
Test: extend test_inplace_redo.c to zero a NON-split leaf (not just split
pages) and assert every committed row reappears after
xstore_recover_inplace.

### Increment 2 -- make xstore_recover_inplace the live crash default
Files: engine.c:sx_storage_open (line ~344).  On an untrusted (torn)
base, call xstore_recover_inplace(g_xbt, g_xbm, path, &pages) instead of
destroying the base and calling xstore_recover.  Keep the clean-restart
fast path (engine.c ~330, trusted==1) unchanged.
Invariant: recovery from ANY crash restores exactly the durable-commit
set with zero missing/torn/leaked rows -- the property test_sim_crash_
recover.c asserts.  This is the data-integrity crux; do not land it until
Increment 1's test is green on torn non-split leaves.
Test: this is where test/sim/test_sim_crash_recover.c must change (see
Section 5's acceptance test) -- crash with a SMALL pool so real version
pages are STOLEN to the base, and assert losers are undone in place.

### Increment 3 -- apply real versions incrementally (true STEAL of versions)
Files: xstore.c:xs_buf_write / xs_commit_ctx.  Instead of (or in addition
to) staging payloads, PUT the real version into the tree under an
uncommitted marker as it is written, logged as XL_UPDATE under the real
(never-yet-committed) txn id, and let it be evicted.  Commit then only
writes XL_COMMIT (the versions are already in the tree); rollback/crash
undoes via the existing xs_undo_loser path.
Invariant: an uncommitted version in the tree is INVISIBLE to every other
transaction's snapshot (MVCC visibility) and REMOVABLE by undo.  This is
the MVCC-interaction risk in Section 4.
Test: extend test_steal.c so a concurrent reader on a second connection
never sees the large uncommitted txn's rows mid-flight, and crash still
undoes them.

### Increment 4 -- fuzzy checkpoint + mid-log truncation to recLSN horizon
Files: engine.c:sx_storage_checkpoint, xstore.c:xstore_checkpoint_wal.
Replace O(live-data) compaction with an O(dirty) checkpoint that writes
the dirty-page table + active-txn table + ckp_lsn = min(oldest active txn
LSN, bm_min_rec_lsn) and truncates the log behind ckp_lsn.  Only sound
once Increment 2 trusts the base in place from that horizon.
Invariant: every change before ckp_lsn is already on the data file, so
truncating the log behind it loses nothing.
Test: checkpoint cost proportional to dirty pages, not live rows;
recovery starts at ckp_lsn and still recovers the durable set.

------------------------------------------------------------------------------
## 4. Hard risks

### 4.1 Torn pages
Handled for the in-place base: double-write (G, bufmgr.c:dw_protect /
dw_recover) makes every home-location overwrite atomic -- a crash
mid-write leaves either the old page or a durable double-write copy that
dw_recover re-applies on reopen.  Combined with page-LSN-gated redo
(bm_apply_page_image), a torn STRUCTURE modification is repaired.  The
gap is torn NON-split leaves with no XL_PAGE image (Increment 1): NO-FORCE
+ page-LSN redo does NOT handle them today because there is no after-image
to gate on, and logical bt_insert over the hole loses rows.  Atomic page
write (double-write) is present and sufficient; the missing piece is
COVERAGE (log an image for every dirtied page), not the atomic-write
mechanism.

### 4.2 MVCC interaction (Increment 3 only)
If real uncommitted versions enter the tree, they must be invisible to
other snapshots.  The visibility rule already keys on commit_ts vs
snapshot (xstore.c scan/visibility, e.g. enc_vkey uses ~commit_ts and the
cursor filters by snap), but an UNCOMMITTED version has no real commit_ts
yet.  The value-staging design (I) sidesteps this entirely by keeping
uncommitted payloads under a reserved table-id that no user scan reads.
Design II must assign uncommitted versions a marker the visibility check
treats as invisible-to-all-but-self and that undo can delete -- a genuine
new invariant, and the primary reason design II is risky.

### 4.3 The write-ahead hook must never be bypassed
This is the one invariant whose violation silently loses data: any dirty
page written to disk before the log is durable past its LSN can, on crash,
present a committed-looking change the log cannot replay, or an
uncommitted change the log cannot undo.  Today the hook gates EVERY write
path (requirement B: flush_frame bufmgr.c:447, tr_prepare bufmgr.c:1631).
The risk is a FUTURE write path added without the gate.  Mitigation:
route every device write for a dirty frame through the two existing
choke points (flush_frame, tr_prepare); never add a third that calls
do_io directly on a dirty frame.  A cheap guard: assert in do_io that a
dirty page's rec_lsn is <= wal_durable_lsn when lsn_off >= 0.

------------------------------------------------------------------------------
## 5. Recommendation

Keep NO-STEAL-of-versions + NO-FORCE, plus the existing spill-to-staging
mechanism.  Do NOT make xstore_recover_inplace the live default and do NOT
pursue design II (page-level STEAL of real versions).

Why:
  - STEAL earns its keep only when a SINGLE transaction's dirty set
    exceeds the buffer pool.  For an MVCC append-only engine with a
    bounded per-transaction write buffer, that case is already covered by
    xs_spill_payloads (design I): resident footprint is bounded, undo is
    logged, commit/rollback/crash are correct and tested (test_steal.c).
  - Design II's cost is concentrated exactly where the code is most
    dangerous to change: the live crash-recovery default and MVCC
    visibility of uncommitted data.  The BDB doc (docs/M_SQLXTC_BDB.md,
    S3) already measured the trap -- ~13% of rows lost when logical redo
    descends a torn non-split leaf -- and shows the fix (physiological
    logging of every dirtied page) is a large, log-volume-heavy change
    whose payoff (cheap O(dirty) checkpoints) is a performance
    refinement, not a correctness need.
  - The append-only version keyspace makes superseded versions GC-able,
    so the classic STEAL motivation (in-place update needs the before-
    image on disk) does not apply here.

If a hard external requirement later demands page-level STEAL, Section 3
(Increments 1-4) is the ordered, bounded path, gated on the torn-non-split
-leaf test before the live default flips.

------------------------------------------------------------------------------
## 6. Bounded FIRST increment + acceptance test

Two honest choices for "first increment", depending on the goal:

### 6a. If the goal is to HARDEN what exists (recommended)
First increment = Increment 0 above: add ONE acceptance test that crashes
with real version pages STOLEN to the base under memory pressure, proving
the existing spill+undo already bounds and recovers correctly.

Acceptance test (extend test/sim/test_sim_crash_recover.c):
  - Change the phase-1 pool from 1024 frames to a SMALL pool (e.g. 8
    frames) so dirty pages are genuinely evicted to btA during the
    workload -- real STEAL of committed AND in-flight pages to disk.
    (Today the test comment at bo.n_frames=1024 explicitly says "nothing
    is ever evicted -> no page reaches the data file"; shrinking it is
    the single change that makes STEAL real in the test.)
  - Keep the write-ahead hook installed (xstore_set_wal is set; the bm is
    the one bt runs on) so eviction is WAL-gated.
  - On crash, DO NOT discard btA: recover in place from btA via
    xstore_recover_inplace(bt_on_btA, bm_on_btA, logp, &pages) instead of
    into a fresh btB.
  - Assert, unchanged: every acked commit present (durability), both rows
    or neither (atomicity), no unattempted row (no leak), replay-identical
    hash across two runs of a seed.
  - Additionally assert `pages > 0` -- proof that page images actually
    repaired stolen/torn pages rather than a silent full rebuild.

Expected outcome given the current code: this test will EXPOSE the
torn-non-split-leaf gap (Section 4.1) if the small pool evicts a
non-split leaf that is then torn -- i.e. it is the precise regression
gate that must pass before Increment 2 flips the live default.  If it
passes, in-place recovery is safe for the exercised crash schedules; if
it fails on missing rows, it reproduces the documented 13% problem and
tells you Increment 1 is required first.

### 6b. If the goal is to SHIP page-level STEAL
First increment = Increment 1 (physiological logging of every dirtied
page), with its acceptance test being the zero-a-non-split-leaf extension
of test_inplace_redo.c described in Section 3.  Do not touch the live
default (Increment 2) until 6a's small-pool sim test is green.

------------------------------------------------------------------------------
## Appendix: exact evidence map

  - NO-STEAL commit apply: xstore.c:xs_commit_ctx (~2735), gated by
    xstore.c:xs_wal_log (~2680, comment: "exactly the NO-STEAL discipline").
  - Spill (design I STEAL): xstore.c:xs_spill_payloads (~2191),
    xstore.c:xs_buf_write (~2451), xstore.c:xs_wal_emit_stage (~2081),
    re-materialize on commit xstore.c:xs_wrec_ensure (~700).
  - WAL-gate hook: bufmgr.c:211 (field), engine.c:361 (install),
    bufmgr.c:447 (flush_frame gate), bufmgr.c:1631 (tr_prepare gate),
    callback engine.c:262 sx_wal_flush_cb.
  - Page-LSN redo: bufmgr.c:1140 bm_apply_page_image, used
    xstore.c:xs_recover_cb XL_PAGE arm (~3875).
  - Undo/CLR: xstore.c:xs_undo_loser (3788), drivers xstore_recover
    (3899) and xstore_recover_inplace (3958).
  - recLSN: bufmgr.c:1067 (stamp), bufmgr.c:1169 bm_min_rec_lsn.
  - Double-write: bufmgr.c:329 dw_protect, bufmgr.c:363 dw_recover,
    engine.c:311 enable.
  - Live crash default (the thing to change for design II): engine.c:344
    xstore_recover (logical rebuild), NOT xstore_recover_inplace.
  - Crash-recovery property test: test/sim/test_sim_crash_recover.c
    (pool = 1024 frames = no eviction = NO-STEAL/NO-FORCE loss model).
  - Design rationale: docs/M_SQLXTC_BDB.md sections S3, S4, S5 and the
    2.8 scorecard.
