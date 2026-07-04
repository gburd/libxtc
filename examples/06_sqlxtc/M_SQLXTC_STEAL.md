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
| A | Uncommitted data may reach pages | HAVE (design I) | Real MVCC versions are not applied to the tree until commit (xstore.c:xstore_ctx_t.wbuf -> xs_commit_ctx), but an OVERSIZED txn's uncommitted payloads DO reach pages and ARE evicted to disk under pool pressure: xstore.c:xs_spill_payloads (`bt_insert(... XS_STAGE_TID)`) from xstore.c:xs_buf_write when `wbuf_bytes > XS_SPILL_HI`.  PROVEN to reach disk by test_steal_page (evict_flushes=609 under a 16-frame pool), invisible to readers (staging table-id), undone in place via CLRs on crash.  Design II (apply the REAL versions incrementally) is still not done -- see Increment 3 status. |
| B | Write-ahead-log enforcement on eviction | HAVE | Hook defined bufmgr.c:211 (`wal_flush`), installed engine.c:361 (`bm_set_wal_flush(g_xbm, sx_wal_flush_cb, g_xwal)`), and CALLED on every dirty-page write path: bufmgr.c:447 in `flush_frame` (eviction/cooling/checkpoint via lines 596, 1504, 2009) and bufmgr.c:1631 in `tr_prepare` (the trickler).  The callback is engine.c:262 `sx_wal_flush_cb` -> `wal_flush_through`. |
| C | Page-LSN redo gating on a torn base | HAVE (mechanism) | Mechanism built and correct: bufmgr.c:1140 `bm_apply_page_image` (and `bm_apply_page_image_at`, record-LSN gated) compares on-disk page LSN vs image LSN and applies only the newer; recovery calls it from xstore.c:xs_recover_cb (XL_PAGE arm) via xstore.c:xstore_recover_inplace, now a TWO-pass driver (images first, logical redo second).  Torn NON-split leaves ARE now covered: every plain in-leaf insert logs an XL_PAGE after-image (xstore.c:xs_leaf_page via the bt_smo_hook `leaf` callback), proven by test_steal_leaf (the 5144-lost-rows repro recovers fully in place).  NOT the live default: engine.c:sx_storage_open still uses xstore_recover (logical rebuild) -- the mechanism is proven but the live flip is deliberately deferred (Increment 3 status). |
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
## Increment 1 -- DONE (per-record checksum makes recovery torn-tail-safe)

The ROOT of the OUTCOME B failure (below) was that recovery could not
tell a COMPLETE log record from a torn one, so a partially written tail
record was decoded and its length/id fields drove unbounded allocations.
That is now fixed at the source with the standard ARIES/Stasis technique
-- a self-checking log record:

  - Record format bumped (wal.c).  On-disk layout is now
    `[u64 lsn][u32 len][body:len][u64 crc]`: an 8-byte trailer holding a
    64-bit FNV-1a hash over the header bytes (lsn+len) and the body.
    The example engine has no external on-disk-format compat
    requirement, so no version negotiation is needed.
  - Every write path stamps the trailer: batch_add (group commit),
    wal_commit_sync (synchronous off-loop append), wal_cmp_emit
    (checkpoint compaction re-emit).  The checksum is over each LOGICAL
    record, NOT the fsync'd batch -- so a batch torn mid-write fails the
    checksum of exactly the first incomplete record.
  - Both readers VERIFY it: wal_scan and wal_scan_tail recompute the
    hash and treat a MISMATCH (or a short read of the trailer) exactly
    as end-of-log.  A torn tail record is dropped before it ever reaches
    xl_parse_*.
  - Both recovery drivers run through the verified scan:
    xstore_recover (logical rebuild, the safe default) AND
    xstore_recover_inplace (the in-place path that trusts the base).
    Neither can decode a torn record, so none of the unbounded-alloc
    sites listed under OUTCOME B is reachable from a torn record.

Test: examples/06_sqlxtc/test_steal_torn.c (new, in the Makefile).  It
builds a log of several committed transactions, then torns the tail
THREE ways -- (1) a plausible-but-wrong appended record with a corrupt
trailer, (2) a record torn before its trailer (crash mid-record), (3) a
bit flipped in the last good record's body -- and for EACH runs BOTH
xstore_recover and xstore_recover_inplace, asserting: recovery completes
without ballooning; every complete committed row before the tear
survives; the torn tail's row never leaks in.  The process caps its own
address space with setrlimit(RLIMIT_AS, 256 MB) BEFORE any recovery
(relaxed only under AddressSanitizer, which needs a large shadow
mapping), so a regression that reintroduced the unbounded allocation
fails a malloc cleanly instead of OOM-killing the box -- measured peak
RSS ~4.5 MB, far under the cap.  Verified: the test FAILS its
torn-tail-excluded assertion when the checksum verify is disabled (the
regression is genuinely caught), and PASSES with it on.  All existing
recovery tests still pass: test_recover_undo, test_redo_page,
test_wal_recover, test_inplace_redo, test_clean_restart, test_wal,
test_xlog, test_steal, test_bufmgr, test_bufmgr_mt (plus test_wal_compact
and test_persist, which exercise the compaction re-emit and the
resume/rebind paths).  ASan-clean on test_steal_torn and
test_recover_undo.

What REMAINS for full page-level STEAL (NOT done here): physiological
XL_PAGE after-image logging of every dirtied NON-split leaf (the
remainder of the original Increment 1 scope), so logical XL_UPDATE redo
over a trusted base never descends a torn non-split leaf; and Increments
2-4.  Those stay deferred per Section 5 -- the checksum work closes the
OOM / unbounded-alloc hole, which was the data-integrity-critical part.

------------------------------------------------------------------------------
## Increments 2-4 -- STATUS (2026-07): 2 DONE, 3 DONE (mechanism, not
## live default), 4 DEFERRED; live default NOT flipped (deliberate)

Work done under the "implement Increments 2-4" pass.  Test-driven; every
recovery here runs under setrlimit(RLIMIT_AS, 256 MB) so a regression
fails cleanly instead of OOMing the box.  New native tests:
test_steal_page.c and test_steal_leaf.c (both in the Makefile and both
CI examples-job lists).

### Increment 2 -- real page-level STEAL of uncommitted data: DONE

The requirement is: dirty pages carrying uncommitted data may be evicted
to disk under buffer-pool pressure (WAL-gated), those versions stay
INVISIBLE to other readers, and are REMOVABLE by undo on abort/crash.
All three now hold on the LIVE spill path and are PROVEN with
buffer-manager evidence, not merely asserted:

  - STEAL actually happens.  test_steal_page drives a single
    uncommitted transaction whose dirty set (~1.2 MB of staged version
    pages) far exceeds a 16-frame (64 KB) pool, then reads
    bm_get_stats(): evict_flushes = 609 (measured) -- the foreground
    eviction path found no clean victim and WROTE 609 dirty pages
    carrying the uncommitted transaction's staged version data to the
    base file, through the write-ahead hook (bufmgr.c:597, gated by
    wal_flush at bufmgr.c:447).  10121 evictions total.  This is
    genuine page-level STEAL of uncommitted data to disk, WAL-before-
    data preserved.  (The prior test_steal.c ran on a 64-frame pool
    where nothing is ever evicted, so it never demonstrated STEAL
    reached disk; test_steal_page closes exactly that evidence gap.)
  - Invisible to other readers.  The stolen uncommitted data lives
    under the reserved staging table-id XS_STAGE_TID, which no user scan
    reads (xs_advance filters by the caller's tableid, xstore.c:1436).
    test_steal_page opens a concurrent scan on the same base while the
    big transaction is mid-flight with pages already on disk and sees
    exactly the committed set (32 rows), none of the 6000 stolen
    uncommitted rows.  MVCC visibility already excludes them -- verified.
  - Removable by undo.  On crash mid-transaction the staged rows are
    logged as XL_UPDATE under a never-committing steal_txn
    (xstore.c:xs_wal_emit_stage), so recovery redoes then UNDOES them
    with CLRs.  test_steal_page: after xstore_recover_inplace on the
    torn/stolen base, xstore_undo_clrs() rose by 4775 (measured) and the
    base holds exactly the 32 committed rows -- no stolen uncommitted row
    leaked.  This is in-place undo of a genuinely stolen loser.

Why this is Increment 2 DONE and not design II: Section 2 established
that "let uncommitted data reach pages" is satisfied by EITHER design.
The spill path (design I) puts real uncommitted payloads into the tree
(under the staging table-id) and lets the buffer manager evict them to
disk -- which is page-level STEAL of uncommitted data by the plain
definition, now measured.  It sidesteps the design-II MVCC hazard
(Section 4.2) because the staged rows are never keyed with a
commit timestamp a user snapshot could accept; they are invisible by
construction, not by a new visibility rule.

### Increment 3 -- physiological non-split-leaf logging: DONE (mechanism);
### live default NOT flipped (deliberate, per Section 5 + the mandate)

Increment 3 asks to (a) apply real MVCC versions incrementally under an
uncommitted marker and (b) flip the live crash default to
xstore_recover_inplace so STEAL is the live path -- ONLY once the tests
prove it correct.  The PREREQUISITE the plan named for a torn-base-safe
in-place path is now BUILT and PROVEN: physiological XL_PAGE after-image
logging of every dirtied NON-split leaf, plus a redo pass that applies
images strictly before any logical redo descends the repaired base.

WHAT WAS BUILT (files):

  - btree.c / btree.h.  The SMO hook gained a `leaf` callback fired from
    the two PLAIN (non-split) in-leaf insert sites -- bt_insert_fast (the
    latch-free fast path) and the non-split branch of bt_insert (the SMO
    path's upsert/re-run).  Both now bm_predirty the leaf and log its
    full after-image, exactly as the split path already logs its pages.
    The `page` (split) and `leaf` callbacks now RETURN the image record's
    own WAL LSN; btree.c stamps that LSN onto the live page (bm_stamp_lsn),
    so the on-disk page LSN equals the LSN recovery gates the apply by.
  - xstore.c.  xs_leaf_page emits the leaf after-image as an XL_PAGE
    (txn_id 0, no NTA -- a single-leaf change is atomic under the
    double-write buffer) and returns its emit LSN; xstore_register_smo
    installs it alongside the split hooks.  xs_wal_log now returns the
    commit LSN and xs_commit_ctx stamps it (bt_set_lsn) before the apply
    loop, so leaf images in a group-committed transaction carry a
    well-defined page LSN.
  - bufmgr.c / bufmgr.h.  bm_apply_page_image_at gates a page image on an
    EXTERNAL LSN (the log record's own, monotonically-numbered LSN)
    instead of the LSN embedded in the image bytes -- so when several
    after-images of one leaf share an embedded commit LSN (many in-leaf
    inserts in one group-committed txn), the LAST image (highest record
    LSN) wins.  bm_stamp_lsn stamps a live page's LSN field in place.
  - xstore.c: xstore_recover_inplace now runs TWO forward passes over the
    (checksum-verified) log.  Pass 1 applies every XL_PAGE image
    (record-LSN gated) so every torn page -- a partly-flushed SMO OR a
    torn NON-split leaf -- is repaired from its final image BEFORE pass 2.
    Pass 2 does the logical XL_UPDATE redo / winner-retire over the
    repaired base (idempotent upsert on well-formed leaves), then undoes
    losers with CLRs exactly as before.  Because pass 1 repairs the torn
    leaf first, pass 2's logical redo never navigates torn structure --
    which is precisely what made the earlier single-pass logical redo
    lose a torn non-split leaf's whole key range.

EVIDENCE (test_steal_leaf.c, new; under the 256 MB RLIMIT_AS cap;
ASan-clean):

  A. The 5144-lost-rows repro now RECOVERS FULLY.  Commit a large txn
     (6000 rows, double_write OFF so a torn leaf is not auto-repaired),
     hand-tear several committed NON-split leaves on disk (zero them,
     LSN -> 0), recover in place.  BEFORE this increment a throwaway
     probe on the same setup lost ~3000 of 6000 rows (pages_redone == 0:
     no leaf image existed to repair from).  AFTER: all 6000 rows
     reappear, a full ordered scan returns exactly the keys (no missing,
     torn, duplicated, or misordered row), and pages_redone > 0 proves
     the torn leaves were repaired from their images, not silently
     rebuilt.  The torn-non-split-leaf hole named in docs/M_SQLXTC_BDB.md
     S3 is closed for in-place recovery.
  B. Atomicity + no-leak of a torn loser.  A committed baseline plus a
     large uncommitted txn that SPILLS to disk (STEAL), with committed
     leaves ALSO torn: in-place recovery repairs the committed baseline
     whole from its images AND undoes the stolen loser via CLRs (4775
     measured); the base holds exactly the committed baseline, no
     uncommitted row leaks.

The SMO-image-clobber hazard (the earlier repro #2, where stale mid-split
images reverted committed leaves) is closed by the two-pass ordering and
the record-LSN gate: images apply LAST-image-wins in pass 1, and logical
redo runs only afterward.  test_inplace_redo (torn split pages) still
passes -- 433 images applied, all rows recovered ordered.

### Was the live default flipped?  NO -- deliberately.

engine.c:sx_storage_open is UNCHANGED: the live crash default stays
xstore_recover (logical rebuild onto a fresh page file), and
xstore_register_smo is NOT called on the live path.  The MECHANISM for a
torn-base-safe in-place restart is now proven correct, but flipping the
live default is deliberately NOT done, for three reasons the plan's
Section 5 and the mandate both call out:

  1. Per-insert log volume.  Logging an XL_PAGE after-image on EVERY
     non-split leaf insert (a full page per row write) is a large,
     WAL-volume-heavy cost.  It earns its keep only for page-level STEAL
     of REAL versions -- which this MVCC append-only engine does not do
     (the spill path, Increment 2, already bounds a single oversized
     transaction with far less overhead).
  2. The payoff is already delivered.  Increment 2's spill + in-place
     UNDO (test_steal_page: 609 evict_flushes, 4775 CLRs) already handles
     the one case page-level STEAL exists to serve, without trusting a
     torn base for EVERY crash.
  3. A live-default flip needs the DST seeded-crash sweep
     (test/sim/test_sim_crash_recover.c, Section 6a) to prove no
     lost/torn/leaked row across many crash schedules -- that sweep is
     owned separately and is the gate the mandate names before any flip.
     Landing the proven mechanism without forcing the flip is exactly
     "an honest partial that is crash-safe beats a forced flip."

So: Increment 3's PREREQUISITE (physiological non-split-leaf logging) and
its recovery path (two-pass, record-LSN-gated xstore_recover_inplace) are
DONE and proven for the torn-non-split-leaf case; Increment 3(b) (the
live-default flip) stays deferred by choice; Increment 3(a) (apply real
versions incrementally under an invisible marker) stays deferred -- the
spill path delivers its payoff without the MVCC-visibility risk of
Section 4.2.

### Increment 4 -- fuzzy checkpoint + recLSN-horizon truncation: DEFERRED

Increment 4 (Section 3) is "only sound once [the live in-place default]
trusts the base in place from that horizon."  Since the in-place default
is NOT flipped (Increment 3(b), deliberate), the recLSN-horizon fuzzy
checkpoint has no safe base to truncate behind, so it stays deferred by
dependency.  The live checkpoint stays O(live-data) compaction
(engine.c:sx_storage_checkpoint), correct for the logical-rebuild
default.  The recLSN plumbing Increment 4 would consume already exists
(bufmgr.c:bm_min_rec_lsn, tested by test_redo_page); only the checkpoint
policy change is deferred.

### Recovery-test pass list (all PASS).

test_recover_undo, test_redo_page, test_wal_recover, test_inplace_redo,
test_clean_restart, test_wal, test_xlog, test_steal, test_steal_torn,
test_steal_page, test_steal_leaf (new), test_bufmgr, test_bufmgr_mt,
test_wal_compact, test_persist, test_torn_smo.  build_unix `make check`
stays green.  ASan-clean on test_steal_leaf, test_recover_undo, and
test_inplace_redo.  The new test_steal_leaf is wired into the Makefile
`test:` target and both CI examples-job lists
(.github/workflows/ci.yml, .forgejo/workflows/ci.yml).


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

**STATUS (2026-07): the torn-record ROOT is DONE (per-record checksum,
see the "Increment 1 -- DONE" section below).  Physiological per-page
after-image logging of every NON-split leaf is now ALSO DONE (btree.c
leaf hook -> xstore.c:xs_leaf_page -> two-pass xstore_recover_inplace),
proven by test_steal_leaf: the torn-non-split-leaf repro recovers the
FULL committed set in place.  The live default flip stays deferred by
choice (see the Increment 3 status above).**

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

## Verification outcome (2026-07): OUTCOME B -- in-place STEAL recovery WAS NOT torn-base-safe; Increment 1 (per-record checksum) now closes the torn-record hole

The bounded first-increment probe was built (a small-pool DST test that
forces genuine page-level STEAL, then recovers the torn base in place
via xstore_recover_inplace and asserts the durability/atomicity/no-leak
invariants).  It reproduced the boundary decisively -- more severely
than the documented ~13%-rows-lost torn-non-split-leaf case:

  xstore_recover_inplace on a genuinely TORN STEAL base has MULTIPLE
  unbounded-allocation vulnerabilities.  It trusts length/id fields
  read from possibly-torn records and allocates on them, so a partially
  written tail record drives a multi-GB allocation (observed: a sudden
  ~1.3-2.1 GB balloon after a flat ~5 MB workload, OOM-killing the
  process).  At least three distinct sites participate:
    1. wal_scan / wal_scan_tail realloc(len) on the 4-byte record length
       (a torn tail reads up to ~4 GiB).  FIXED (see below).
    2. bm_apply_page_image -> bm_fix_pid on a page id parsed from a torn
       XL_PAGE record extends the base file to pid*page_size for a
       garbage pid.  (A naive "reject pid past EOF" guard is WRONG -- it
       breaks the legitimate page-EXTEND redo that test_redo_page
       exercises, where redo validly creates a page past the current
       EOF.  A correct guard needs a physiological "this record extends
       the file by one page" contract, i.e. Increment 1's per-page
       logging, not an EOF check.)
    3. at least one further site downstream (same balloon signature
       after the first two are bounded).

UPDATE (2026-07, Increment 1 DONE): all three sites are now UNREACHABLE
from a torn record.  The root cause was that recovery could not tell a
complete record from a torn one; the per-record checksum (see the
"Increment 1 -- DONE" section above) fixes exactly that.  wal_scan /
wal_scan_tail recompute an FNV-1a trailer over each record and stop at
the first mismatch (or short-read trailer) -- the torn tail is dropped
BEFORE decode, so no length/id from a torn record ever reaches
xl_record_len, bm_apply_page_image/bm_fix_pid, or any downstream site.
Both xstore_recover and xstore_recover_inplace run through the verified
scan, so neither can be driven into the unbounded allocation.  This is
verified safe under a 256 MB RLIMIT_AS cap by test_steal_torn (both
recovery paths, three distinct tears), which was also shown to FAIL when
the checksum verify is removed -- i.e. it genuinely guards the hole.
(The WAL_MAX_REC bound remains as defense in depth on the outer length,
but the checksum, not the bound, is what makes recovery torn-safe: a
torn body whose length is < WAL_MAX_REC is now caught by the trailer.)

This is why the default crash path uses xstore_recover (LOGICAL rebuild
from the clean-durable-frontier WAL), never xstore_recover_inplace, and
why the crash-recovery capstone (test_sim_crash_recover) runs with a
pool large enough that NO eviction occurs (pure NO-STEAL/NO-FORCE).
The default path is torn-base-safe precisely because it never trusts a
torn base.

CONCLUSION (confirms the recommendation above): do NOT enable page-level
STEAL / flip the live default to in-place recovery.  Full page-STEAL
would require hardening every torn-record-trusting site in
xstore_recover_inplace (Increment 1's physiological per-page logging is
the prerequisite, not an ad-hoc bound), and the payoff -- surviving a
single transaction whose dirty set exceeds the buffer pool -- is
already delivered by the spill-to-staging path (test_steal.c).
NO-STEAL-of-versions + NO-FORCE + spill is the correct design.

UPDATE (2026-07): Increment 1's ROOT fix -- the per-record checksum --
IS now landed (see the "Increment 1 -- DONE" section).  It does NOT flip
the live default; it makes BOTH recovery paths torn-tail-safe by
excluding torn records before decode, closing the OOM / unbounded-alloc
hole that made the earlier probe unsafe to keep.  The safe probe that
was kept is now a committed test (test_steal_torn), running under a
256 MB RLIMIT_AS cap so it can never balloon the machine.  What stays
deferred is the REST of full page-STEAL (per-non-split-leaf XL_PAGE
logging and Increments 2-4), for the same payoff reasons above.

The checksum-verified scan replaces the earlier note that "the OOM-ing
probe was NOT added to the committed test suite": the OOM is closed, and
a safe torn-tail test now lives in the suite.  The WAL_MAX_REC (16 MiB)
bound in wal_scan / wal_scan_tail remains as defense in depth (a garbage
outer length still terminates the scan without a giant realloc), verified
against all existing recovery tests (test_recover_undo, test_redo_page,
test_wal_recover, test_inplace_redo, test_clean_restart, test_wal,
test_xlog, test_steal all pass), now joined by the per-record checksum as
the primary torn-record defense.

## Transition status: COMPLETE (2026-07)

The STEAL transition for sqlxtc is DONE.  "Transition to STEAL" meant
gaining STEAL where it earns its keep and hardening recovery to be
torn-safe -- NOT making page-level STEAL the live recovery default,
which this append-only MVCC engine does not benefit from.  Settled
state:

  - Increment 1 (per-record WAL checksum -> recovery excludes torn tail
    records before decode): DONE, live, test_steal_torn.
  - Increment 2 (value-staging STEAL: an oversized transaction's
    uncommitted payloads reach disk under pool pressure, invisible to
    readers, undone via CLRs on crash): DONE, live, test_steal /
    test_steal_page (proven reaching disk, evict_flushes>0).
  - Increment 3 (physiological per-non-split-leaf XL_PAGE after-image
    logging so in-place recovery of a torn leaf is exact): mechanism
    DONE + proven (test_steal_leaf: the 5144-lost-rows torn-leaf repro
    recovers fully in place).  KEPT READY, NOT the live default.
  - Increment 4 (fuzzy checkpoint / recLSN-horizon log truncation):
    DEFERRED -- it depends on trusting an in-place base from the recLSN
    horizon, i.e. on the live flip, which we deliberately do not do.

The LIVE recovery path stays NO-STEAL-of-versions + NO-FORCE +
value-staging-STEAL + logical-rebuild-from-clean-frontier recovery.
This is the CORRECT final design for an append-only MVCC engine (see
Section 5): full page-level STEAL only helps when a SINGLE transaction's
dirty set exceeds the buffer pool, which value-staging already handles;
its cost (per-insert page-image WAL volume + the MVCC visibility of
uncommitted versions in the tree + a torn-base-trusting live recovery
default) buys nothing here.  The page-level mechanism is built, tested,
and kept ready SHOULD a future workload (a non-MVCC / update-in-place
table type) ever need it -- but enabling it is a deliberate future
decision, not unfinished transition work.

No further STEAL work is planned for sqlxtc.  This closes the STEAL
milestone.
