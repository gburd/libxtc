#-
# Copyright (c) 2026, The XTC Project -- All rights reserved.
# Use of this source code is governed by the ISC License.
#
# examples/06_sqlxtc/Makefile -- builds sqlxtc-server.
#
# Targets:
#   make            build sqlxtc-server (native, no SQLite)
#   make lime       build the bundled Lime parser generator (Phase 2)
#   make sql_parse  generate sql_parse_gen.c via Lime
#   make clean      remove objects + binary
#   make distclean  also remove Lime artifacts
#   make test       run the basic smoke test (requires python3)
#
# Override XTC_BUILD to point at a different libxtc build dir.

XTC_BUILD ?= ../../build_unix
XTC_INC   ?= ../../src/inc

CC       = cc
CFLAGS   = -O2 -g -std=c11 -Wall -Wextra -Wpedantic \
           -D_GNU_SOURCE \
           -I$(XTC_INC) -I.

# Pull liburing flags from the parent build.
LIBURING_INC := $(shell \
    [ -f $(XTC_BUILD)/Makefile ] && \
    awk '/^CFLAGS[[:space:]]*=/{print}' $(XTC_BUILD)/Makefile 2>/dev/null \
    | grep -o '\-I[^ ]*liburing[^ ]*' \
    | head -1)

LIBURING_LDFLAGS := $(shell \
    [ -f $(XTC_BUILD)/Makefile ] && \
    awk '/^LIBS[[:space:]]*=/{sub(/^LIBS[[:space:]]*=[[:space:]]*/,""); print}' \
    $(XTC_BUILD)/Makefile 2>/dev/null)

XTC_LDFLAGS := $(shell \
    [ -f $(XTC_BUILD)/Makefile ] && \
    awk '/^LDFLAGS[[:space:]]*=/{sub(/^LDFLAGS[[:space:]]*=[[:space:]]*/,""); print}' \
    $(XTC_BUILD)/Makefile 2>/dev/null)

CFLAGS  += $(LIBURING_INC)

LDFLAGS  = $(XTC_LDFLAGS) $(XTC_BUILD)/libxtc.a -pthread -ldl -lm \
           $(LIBURING_LDFLAGS)

SRCS = main.c quack.c db.c conn.c engine.c metrics.c sql_parse.c admin.c xstore.c xlog.c wal.c bufmgr.c btree.c btnode.c
OBJS = $(SRCS:.c=.o)

# Lime-generated parser + vectorized executor.  The engine is native
# (no SQLite), so the parser and vexec are MANDATORY: the server, the
# parser oracles, and the storage tests all link them.  The build
# requires the committed sql_parse_gen.c (regenerate with 'make
# sql_parse' only when the grammar changes).
LIME_OBJ := sql_parse_gen.o sql_parse_drv.o sql_ast.o
VEXEC_OBJ := vexec.o vxfunc.o
CFLAGS  += -DSQLXTC_HAVE_LIME=1

# The full native SQL engine, for storage-engine tests that drive the
# B-tree through the native sx_* SQL API (sx_open_bt).  No SQLite.
ENGINE_NATIVE = engine.c db.c quack.c metrics.c \
    vexec.c vxfunc.c sql_parse.c sql_parse_drv.c sql_ast.c sql_parse_gen.c \
    xstore.c xlog.c wal.c btree.c btnode.c bufmgr.o

TARGET = sqlxtc-server

.PHONY: all clean distclean test lime sql_parse

all: $(TARGET)

$(TARGET): $(OBJS) $(LIME_OBJ) $(VEXEC_OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LIME_OBJ) $(VEXEC_OBJ) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# ---- Lime parser generator ----
lime: lime/bin/lime
lime/bin/lime:
	@if [ -d lime ]; then \
	    $(MAKE) -C lime JIT=0 bin/lime; \
	else \
	    echo "lime/ submodule missing; skipping" 1>&2; \
	fi

# Regenerate sql_parse_gen.c (and .h) from sql_grammar.lime via Lime.
# This is an EXPLICIT target ('make sql_parse'), not a build dependency:
# the generated parser is committed to the tree, so an ordinary build
# (and CI, and a source tarball) compiles it without needing the Lime
# binary or the submodule.  Regenerate only when the grammar changes,
# then commit the result.  Lime names outputs after the input basename;
# we rename to *_gen.* so they don't collide with the hand-written
# sql_parse.[ch].  Lime exits non-zero if the conflict count differs
# from the grammar's %expect, so a new conflict fails this target.
#
# The recipe is gated on the Lime binary being present: on a fresh
# checkout the submodule is not built, so the committed sql_parse_gen.c
# is used as-is and never spuriously "regenerated" stale.
ifneq ($(wildcard lime/bin/lime),)
sql_parse_gen.c sql_parse_gen.h: sql_grammar.lime lime/bin/lime
	./lime/bin/lime -T./lime/limpar.c sql_grammar.lime
	@test -f sql_grammar.c || { echo "lime failed to produce sql_grammar.c"; exit 1; }
	@mv -f sql_grammar.c sql_parse_gen.c
	@mv -f sql_grammar.h sql_parse_gen.h
	@rm -f sql_grammar.out
endif

sql_parse: sql_parse_gen.c

# Dependencies
main.o: main.c conn.h db.h
quack.o: quack.c quack.h
db.o: db.c db.h quack.h engine.h
conn.o: conn.c conn.h db.h quack.h sql_parse.h engine.h admin.h
admin.o: admin.c admin.h quack.h
engine.o: engine.c engine.h xstore.h btree.h bufmgr.h
metrics.o: metrics.c
sql_parse.o: sql_parse.c sql_parse.h
sql_parse_drv.o: sql_parse_drv.c sql_parse.h sql_parse_lime.h sql_parse_gen.h

clean:
	rm -f $(OBJS) $(TARGET)
	rm -f $(AMALG_OBJS) sqlxtc-server-amalg
	rm -f test_vfs test_vfs_amalg test_vfs.o
	rm -f test_pcache test_pcache_amalg test_pcache.o
	rm -f test_concurrency test_concurrency.o
	rm -f test_admin test_admin.o admin.o
	rm -f test_quack_range
	rm -f test_mem test_mem.o
	rm -f test_wal test_wal.o wal.o
	rm -f test_wal_recover test_wal_fail
	rm -f test_torn_smo
	rm -f test_wal_compact test_wal_lsn_monotone
	rm -f test_shard test_shard.o
	rm -f test_mvcc test_mvcc.o mvcc.o
	rm -f test_xstore test_xstore.o xstore.o
	rm -f test_xlog test_xlog.o xlog.o
	rm -f test_recover_undo
	rm -f test_steal_torn
	rm -f test_redo_page
	rm -f test_inplace_redo
	rm -f test_fuzzy_checkpoint
	rm -f test_clean_restart test_dw_fail
	rm -f test_steal
	rm -f test_steal_page
	rm -f test_steal_leaf test_cat_reopen test_ckpt_online test_wal_group_sync test_intents_thread test_torn_base test_snap_registry test_rowid_minmax test_btree_fill test_wal_bigtxn test_steal_reclaim test_dio_restart test_wal_prealloc
	rm -f test_bufmgr test_bufmgr.o bufmgr.o test_bufmgr_dio test_bufmgr_dio.o
	rm -f test_bufmgr_mt
	rm -f test_drop_reserved test_evict_revalidate_sampled
	rm -f test_scan_resist test_sweep_bound test_evict_revalidate test_btree_late_load test_flush_pin test_trickler_select test_pp_slice test_tr_slice test_publication_pin test_private_frame test_pid_writeback bm_sweep_bench
	rm -f test_persist test_btree_prefix_split sx_size test_rec_codec test_idx_elem
	rm -f test_cursor_resume
	rm -f test_btnode
	rm -f test_btree
	rm -f test_btree_loop
	rm -f test_btree_mt
	rm -f test_btree_stale_descent test_btree_limits test_btree_sample bench_morsel_balance
	rm -f test_tpcc_seek test_dup_composite test_bulk_insert test_plan_cache
	rm -f test_btree_delete_merge test_savepoint test_isolation test_ww_conflict test_row_intents test_commit_visibility test_ssi_skew test_ckpt_bound test_dup_insert test_recover_fast
	rm -f test_vfs_loop test_vfs_loop.o
	rm -rf $(AMALG_DIR)
	rm -f sql_parse_gen.o sql_parse_drv.o
	rm -f /tmp/sqlxtc-bench.db /tmp/sqlxtc-bench.db-*

distclean: clean
	rm -rf *.dSYM
	rm -rf ../../test/sqlxtc/test_quack ../../test/sqlxtc/*.dSYM
	-$(MAKE) -C lime clean 2>/dev/null || true
	rm -f sql_parse_gen.c sql_parse_gen.h sql_grammar.out

test: $(TARGET) test-quack test-quack-range test-sql-parse test-admin test-wal test-wal-group-sync test-wal-bigtxn test-wal-prealloc test-wal-recover test-wal-fail test-torn-smo test-wal-compact test-wal-lsn-monotone test-shard test-mvcc test-xstore test-xstore-scan test-index test-tpcc-seek test-bulk-insert test-plan-cache test-rowid-minmax test-cat-reopen test-savepoint test-isolation test-ww-conflict test-row-intents test-intents-thread test-snap-registry test-commit-visibility test-ssi-skew test-dup-insert test-dup-composite test-ckpt-bound test-ckpt-online test-torn-base test-recover-fast test-native-txn test-xlog test-recover-undo test-bufmgr test-redo-page test-inplace-redo test-fuzzy-checkpoint test-bufmgr-mt test-scan-resist test-sweep-bound test-persist test-cursor-resume test-clean-restart test-dw-fail test-steal test-steal-reclaim test-dio-restart test-steal-torn test-steal-page test-steal-leaf test-btnode test-btree test-btree-fill test-btree-loop test-btree-mt test-btree-delete-merge
	@bash ../../test/sqlxtc/test_sqlxtc_smoke.sh

# ---- amalgamation build ----
# Builds sqlxtc against the single-file xtc amalgamation instead of
# libxtc.a, which exercises the amalgamation as a real, broad
# consumer (app, io, log, lrlock, lwlock, proc, res, stats, net, plus
# the __os_* helpers).  The sqlxtc sources are compiled in a separate
# amalgamation mode (-Iamalg/include resolves the forwarding stubs to
# the single xtc.h).  The engine is native: no SQLite, no libxtc.a,
# no liburing (the amalgamation auto-selects the epoll backend).
AMALG_DIR    = amalg
AMALG_CFLAGS = -O2 -g -std=c11 -Wall -Wextra -Wpedantic -D_GNU_SOURCE \
               -DSQLXTC_HAVE_LIME=1 \
               -I$(AMALG_DIR)/include -I.
# Native engine sources for the amalgamation link (server set + the
# parser + vexec; no SQLite).
AMALG_SRCS   = $(SRCS) vexec.c vxfunc.c sql_parse_drv.c sql_ast.c sql_parse_gen.c
AMALG_OBJS   = $(AMALG_SRCS:.c=.amalg.o)

.PHONY: amalg amalg-test

$(AMALG_DIR)/xtc.c:
	python3 ../../dist/mkamalgamation.py --root ../.. --out $(AMALG_DIR)

$(AMALG_DIR)/xtc.o: $(AMALG_DIR)/xtc.c
	$(CC) $(AMALG_CFLAGS) -c -o $@ $(AMALG_DIR)/xtc.c

%.amalg.o: %.c $(AMALG_DIR)/xtc.c
	$(CC) $(AMALG_CFLAGS) -c -o $@ $<

sqlxtc-server-amalg: $(AMALG_OBJS) $(AMALG_DIR)/xtc.o
	$(CC) $(AMALG_CFLAGS) -o $@ $(AMALG_OBJS) \
		$(AMALG_DIR)/xtc.o -pthread -ldl -lm

amalg: sqlxtc-server-amalg

amalg-test: sqlxtc-server-amalg
	@SQLXTC_SERVER=$(CURDIR)/sqlxtc-server-amalg \
		bash ../../test/sqlxtc/test_sqlxtc_smoke.sh

# ---- xtc VFS test (in-process, no daemon) ----
# Builds the VFS test against both libxtc.a and the amalgamation, so
# the custom sqlite3_vfs is exercised in both link configurations.
.PHONY:-amalg



# ---- xtc VFS on-loop blocking-offload test (in-process, no daemon) ----




# ---- xtc page-cache test (in-process, no daemon) ----
.PHONY:-amalg



# ---- xtc_amutex SQLite-mutex concurrency test (in-process, no daemon) ----


# ---- SHOW PROCESSES admin command test (in-process, no daemon) ----
# Links only admin.o + quack.o against libxtc.a; no SQLite needed --
# admin walks the live proc set via xtc_inspect_procs.
test_admin: test_admin.c admin.o quack.o
	$(CC) $(CFLAGS) -o $@ test_admin.c admin.o quack.o $(LDFLAGS)

test-admin: test_admin
	./test_admin

# ---- Quack integer range test (PLAN 19.27.23; in-process, no daemon) ----
test_quack_range: test_quack_range.c quack.o
	$(CC) $(CFLAGS) -o $@ test_quack_range.c quack.o

test-quack-range: test_quack_range
	./test_quack_range

# ---- xtc-allocator SQLite-memory seam test (in-process, no daemon) ----
# Installs the xtc allocator under an __os_alloc counting hook, then
# drives a real SQL workload: proves the engine's allocations flow
# through xtc (hook count > 0) and that the workload is correct.


# ---- group-commit WAL writer process (in-process, no daemon) ----
# Many committer procs drive concurrent commits at one writer; proves
# fsync coalescing, durable monotonic LSNs, and log replay -- on a
# single loop and on a 4-loop executor.  Storage-engine stage 1
#.
test_wal: test_wal.c wal.c wal.h
	$(CC) $(CFLAGS) -o $@ test_wal.c wal.c $(LDFLAGS)

test-wal: test_wal
	./test_wal

# ---- WAL durability + crash recovery for the xstore SQL engine ----
test_wal_recover: test_wal_recover.c $(ENGINE_NATIVE)
	$(CC) $(CFLAGS) -o $@ test_wal_recover.c $(ENGINE_NATIVE) $(LDFLAGS)

test-wal-recover: test_wal_recover
	./test_wal_recover

# ---- WAL write/fsync failure: fail-stop, never ack a lost commit ----
test_wal_fail: test_wal_fail.c $(ENGINE_NATIVE)
	$(CC) $(CFLAGS) -o $@ test_wal_fail.c $(ENGINE_NATIVE) $(LDFLAGS)

test-wal-fail: test_wal_fail
	./test_wal_fail

test_torn_smo: test_torn_smo.c $(ENGINE_NATIVE)
	$(CC) $(CFLAGS) -o $@ test_torn_smo.c $(ENGINE_NATIVE) $(LDFLAGS)

test-torn-smo: test_torn_smo
	./test_torn_smo

test_wal_compact: test_wal_compact.c $(ENGINE_NATIVE)
	$(CC) $(CFLAGS) -o $@ test_wal_compact.c $(ENGINE_NATIVE) $(LDFLAGS)

test-wal-compact: test_wal_compact
	./test_wal_compact

# LSNs keep rising across a log checkpoint (ARIES page-LSN gate).
test_wal_lsn_monotone: test_wal_lsn_monotone.c wal.c wal.h
	$(CC) $(CFLAGS) -o $@ test_wal_lsn_monotone.c wal.c $(LDFLAGS)

test-wal-lsn-monotone: test_wal_lsn_monotone
	./test_wal_lsn_monotone

# ---- share-nothing sharded store + control plane (executor; no daemon) ----
# N shard servers (one xtc_svr each, share-nothing) + a global
# timestamp allocator; clients route by key-hash.  Scale-out stage 2/3
#: single loop and 4-loop executor.
test_shard: test_shard.c
	$(CC) $(CFLAGS) -o $@ test_shard.c $(LDFLAGS)

test-shard: test_shard
	./test_shard

# ---- MVCC + cross-shard 2PC (executor; no daemon) ----
# Snapshot-isolation versioned shards + a deferred-reply 2PC
# coordinator + per-shard HLC.  Scale-out stage 4
#: snapshot isolation, cross-shard
# atomicity, and write-write conflict detection on a 4-loop executor.
test_mvcc: test_mvcc.c mvcc.c mvcc.h
	$(CC) $(CFLAGS) -o $@ test_mvcc.c mvcc.c $(LDFLAGS)

test-mvcc: test_mvcc
	./test_mvcc

# ---- SQL on the libxtc-native storage via the xstore vtable ----
# SQLite's VDBE runs SQL; rows live in our btree/bufmgr (larger than
# RAM).  Off-loop and on-loop (VDBE parks on offloaded page I/O
# mid-statement).
test_xstore: test_xstore.c $(ENGINE_NATIVE)
	$(CC) $(CFLAGS) -o $@ test_xstore.c $(ENGINE_NATIVE) $(LDFLAGS)

test-xstore: test_xstore
	./test_xstore

# ---- storage-native scan (xstore_scan_*) vs the VDBE under MVCC ----
test_xstore_scan: test_xstore_scan.c $(ENGINE_NATIVE)
	$(CC) $(CFLAGS) -o $@ test_xstore_scan.c $(ENGINE_NATIVE) $(LDFLAGS)

test-xstore-scan: test_xstore_scan
	./test_xstore_scan

# ---- secondary indexes (CREATE INDEX): build, DML maintenance, seek ----
test_index: test_index.c $(ENGINE_NATIVE)
	$(CC) $(CFLAGS) -o $@ test_index.c $(ENGINE_NATIVE) $(LDFLAGS)

test-index: test_index
	./test_index

# ---- TPROC-C point statements are O(1)-row seeks (rows-visited metric) ----
test_tpcc_seek: test_tpcc_seek.c $(ENGINE_NATIVE)
	$(CC) $(CFLAGS) -o $@ test_tpcc_seek.c $(ENGINE_NATIVE) $(LDFLAGS)

test-tpcc-seek: test_tpcc_seek
	./test_tpcc_seek

# ---- in-txn bulk INSERT is O(1) per row (no write-buffer rescan) ----
test_bulk_insert: test_bulk_insert.c $(ENGINE_NATIVE)
	$(CC) $(CFLAGS) -o $@ test_bulk_insert.c $(ENGINE_NATIVE) $(LDFLAGS)

test-bulk-insert: test_bulk_insert
	./test_bulk_insert

# ---- a prepared statement is parsed and planned once ----
test_plan_cache: test_plan_cache.c $(ENGINE_NATIVE)
	$(CC) $(CFLAGS) -o $@ test_plan_cache.c $(ENGINE_NATIVE) $(LDFLAGS)

test-plan-cache: test_plan_cache
	./test_plan_cache

# ---- Lime SQL parser: differential AST oracle vs the reference engine ----


# ---- record codec: byte-for-byte SQLite-format encode/decode ----


# ---- vectorized executor V0: recognizer + fallback + differential oracle ----


# ---- vectorized executor V2: morsel-parallel + differential/scaling ----


# ---- vectorized executor V4: ORDER BY / LIMIT positional differential ----


# ---- vectorized executor: unified dispatcher (vx_run) ----


# ---- live query path: vexec fast path differential (db_exec_cached) ----


# ---- vectorized executor V5: INNER hash join differential ----


test_savepoint: test_savepoint.c $(ENGINE_NATIVE)
	$(CC) $(CFLAGS) -o $@ test_savepoint.c $(ENGINE_NATIVE) $(LDFLAGS)

test-savepoint: test_savepoint
	./test_savepoint

test_native_txn: test_native_txn.c $(ENGINE_NATIVE)
	$(CC) $(CFLAGS) -o $@ test_native_txn.c $(ENGINE_NATIVE) $(LDFLAGS)

test-native-txn: test_native_txn
	./test_native_txn



test_ww_conflict: test_ww_conflict.c $(ENGINE_NATIVE)
	$(CC) $(CFLAGS) -o $@ test_ww_conflict.c $(ENGINE_NATIVE) $(LDFLAGS)

test-ww-conflict: test_ww_conflict
	./test_ww_conflict

test_row_intents: test_row_intents.c $(ENGINE_NATIVE)
	$(CC) $(CFLAGS) -o $@ test_row_intents.c $(ENGINE_NATIVE) $(LDFLAGS)

test-row-intents: test_row_intents
	./test_row_intents

test_commit_visibility: test_commit_visibility.c $(ENGINE_NATIVE)
	$(CC) $(CFLAGS) -o $@ test_commit_visibility.c $(ENGINE_NATIVE) $(LDFLAGS)

test-commit-visibility: test_commit_visibility
	./test_commit_visibility

test_ssi_skew: test_ssi_skew.c $(ENGINE_NATIVE)
	$(CC) $(CFLAGS) -o $@ test_ssi_skew.c $(ENGINE_NATIVE) $(LDFLAGS)

test-ssi-skew: test_ssi_skew
	./test_ssi_skew

test_dup_composite: test_dup_composite.c $(ENGINE_NATIVE)
	$(CC) $(CFLAGS) -o $@ test_dup_composite.c $(ENGINE_NATIVE) $(LDFLAGS)

test-dup-composite: test_dup_composite
	./test_dup_composite

test_dup_insert: test_dup_insert.c $(ENGINE_NATIVE)
	$(CC) $(CFLAGS) -o $@ test_dup_insert.c $(ENGINE_NATIVE) $(LDFLAGS)

test-dup-insert: test_dup_insert
	./test_dup_insert

test_recover_fast: test_recover_fast.c $(ENGINE_NATIVE)
	$(CC) $(CFLAGS) -o $@ test_recover_fast.c $(ENGINE_NATIVE) $(LDFLAGS)

test-recover-fast: test_recover_fast
	./test_recover_fast

test_ckpt_bound: test_ckpt_bound.c $(ENGINE_NATIVE)
	$(CC) $(CFLAGS) -o $@ test_ckpt_bound.c $(ENGINE_NATIVE) $(LDFLAGS)

test-ckpt-bound: test_ckpt_bound
	./test_ckpt_bound

test_isolation: test_isolation.c $(ENGINE_NATIVE)
	$(CC) $(CFLAGS) -o $@ test_isolation.c $(ENGINE_NATIVE) $(LDFLAGS)

test-isolation: test_isolation
	./test_isolation

# ---- ARIES log-record codec unit test (pure, no I/O) ----
test_xlog: test_xlog.c xlog.c
	$(CC) $(CFLAGS) -o $@ test_xlog.c xlog.c $(LDFLAGS)

test-xlog: test_xlog
	./test_xlog

# ---- recovery undo pass + CLR writing (synthetic loser log) ----
test_recover_undo: test_recover_undo.c xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o
	$(CC) $(CFLAGS) -o $@ test_recover_undo.c xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o $(LDFLAGS)

test-recover-undo: test_recover_undo
	./test_recover_undo

# ---- torn-tail recovery safety (per-record checksum drops a torn tail) ----
# Both recovery paths must survive a torn tail without ballooning memory
# (caps its own RLIMIT_AS at 256 MB internally so a regression fails
# cleanly instead of OOMing the box).  Increment 1 of the design notes.
test_steal_torn: test_steal_torn.c xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o
	$(CC) $(CFLAGS) -o $@ test_steal_torn.c xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o $(LDFLAGS)

test-steal-torn: test_steal_torn
	./test_steal_torn

# ---- LeanStore-style buffer manager test (in-process, no daemon) ----
test_bufmgr: test_bufmgr.c bufmgr.o
	$(CC) $(CFLAGS) -o $@ test_bufmgr.c bufmgr.o $(LDFLAGS)

test-bufmgr: test_bufmgr
	./test_bufmgr

test_bufmgr_dio: test_bufmgr_dio.c bufmgr.o
	$(CC) $(CFLAGS) -o $@ test_bufmgr_dio.c bufmgr.o $(LDFLAGS)

test-bufmgr-dio: test_bufmgr_dio
	./test_bufmgr_dio

bm_writeback_bench: bm_writeback_bench.c bufmgr.o
	$(CC) $(CFLAGS) -o $@ bm_writeback_bench.c bufmgr.o $(LDFLAGS)

# A/B benchmark for the eviction-sweep bound (not part of make test).
bm_sweep_bench: bm_sweep_bench.c bufmgr.h bufmgr.o
	$(CC) $(CFLAGS) -o $@ bm_sweep_bench.c bufmgr.o $(LDFLAGS)

bt_merge_bench: bt_merge_bench.c btree.c bufmgr.o btnode.c
	$(CC) $(CFLAGS) -o $@ bt_merge_bench.c btree.c bufmgr.o btnode.c $(LDFLAGS)

bench_btree_concurrent: bench_btree_concurrent.c btree.c bufmgr.o btnode.c
	$(CC) $(CFLAGS) -o $@ bench_btree_concurrent.c btree.c bufmgr.o btnode.c $(LDFLAGS)

# Read latency (p50/p99/p99.9) under a concurrent delete+insert stream.
bench_btree_mixed: bench_btree_mixed.c btree.c bufmgr.o btnode.c
	$(CC) $(CFLAGS) -o $@ bench_btree_mixed.c btree.c bufmgr.o btnode.c $(LDFLAGS)

# Morsel balance of the parallel scan on skewed rowids (sampled vs uniform
# split; rows per morsel and wall time).
bench_morsel_balance: bench_morsel_balance.c $(ENGINE_NATIVE)
	$(CC) $(CFLAGS) -o $@ bench_morsel_balance.c $(ENGINE_NATIVE) $(LDFLAGS)

# ---- physiological redo apply (page-LSN gated image write) ----
test_redo_page: test_redo_page.c bufmgr.o
	$(CC) $(CFLAGS) -o $@ test_redo_page.c bufmgr.o $(LDFLAGS)

test-redo-page: test_redo_page
	./test_redo_page

# ---- end-to-end in-place physiological crash recovery (XL_PAGE images) ----
test_inplace_redo: test_inplace_redo.c $(ENGINE_NATIVE)
	$(CC) $(CFLAGS) -o $@ test_inplace_redo.c $(ENGINE_NATIVE) $(LDFLAGS)

test-inplace-redo: test_inplace_redo
	./test_inplace_redo

# ---- Increment 4: fuzzy (O(dirty)) checkpoint + recLSN-horizon log
# truncation; in-place recovery from the horizon matches a full-scan
# recovery exactly ----
test_fuzzy_checkpoint: test_fuzzy_checkpoint.c $(ENGINE_NATIVE)
	$(CC) $(CFLAGS) -o $@ test_fuzzy_checkpoint.c $(ENGINE_NATIVE) $(LDFLAGS)

test-fuzzy-checkpoint: test_fuzzy_checkpoint
	./test_fuzzy_checkpoint

bufmgr.o: bufmgr.c bufmgr.h

# ---- buffer-manager multi-threaded stress test (executor; no daemon) ----
test_bufmgr_mt: test_bufmgr_mt.c bufmgr.o
	$(CC) $(CFLAGS) -o $@ test_bufmgr_mt.c bufmgr.o $(LDFLAGS)

test-bufmgr-mt: test_bufmgr_mt
	./test_bufmgr_mt

# ---- buffer-pool scan resistance (probationary admission) ----
test_scan_resist: test_scan_resist.c bufmgr.o
	$(CC) $(CFLAGS) -o $@ test_scan_resist.c bufmgr.o $(LDFLAGS)

test-scan-resist: test_scan_resist
	./test_scan_resist

test: test-drop-reserved

test_drop_reserved: test_drop_reserved.c bufmgr.c bufmgr.h
	$(CC) $(CFLAGS) -o $@ test_drop_reserved.c $(LDFLAGS)

test-drop-reserved: test_drop_reserved
	./test_drop_reserved control
	./test_drop_reserved

test: test-evict-revalidate test-btree-late-load test-flush-pin test-trickler-select test-pp-slice test-tr-slice test-publication-pin test-private-frame test-pid-writeback test-btree-stale-descent

# A descent that keeps hitting stale nodes still returns a held leaf
# (bufmgr.c and btree.c are #included for the test-only stale seam).
test_btree_stale_descent: test_btree_stale_descent.c bufmgr.c bufmgr.h btree.c btnode.c
	$(CC) $(CFLAGS) -o $@ test_btree_stale_descent.c btnode.c $(LDFLAGS)

test-btree-stale-descent: test_btree_stale_descent
	./test_btree_stale_descent

test_pid_writeback: test_pid_writeback.c bufmgr.c bufmgr.h
	$(CC) $(CFLAGS) -o $@ test_pid_writeback.c $(LDFLAGS)

test-pid-writeback: test_pid_writeback
	./test_pid_writeback

test_private_frame: test_private_frame.c bufmgr.c bufmgr.h
	$(CC) $(CFLAGS) -o $@ test_private_frame.c $(LDFLAGS)

test-private-frame: test_private_frame
	./test_private_frame

test_publication_pin: test_publication_pin.c bufmgr.c bufmgr.h
	$(CC) $(CFLAGS) -o $@ test_publication_pin.c $(LDFLAGS)

test-publication-pin: test_publication_pin
	./test_publication_pin

# Double-write slot + clean-shutdown I/O failures propagate (bufmgr.c is
# #included for its internals; the engine minus bufmgr.o is linked).
test_dw_fail: test_dw_fail.c bufmgr.c bufmgr.h engine.o db.o quack.o xstore.o xlog.o wal.o btree.o btnode.o $(LIME_OBJ) $(VEXEC_OBJ)
	$(CC) $(CFLAGS) -o $@ test_dw_fail.c engine.o db.o quack.o xstore.o xlog.o wal.o btree.o btnode.o $(LIME_OBJ) $(VEXEC_OBJ) $(LDFLAGS)

test-dw-fail: test_dw_fail
	./test_dw_fail

test_flush_pin: test_flush_pin.c bufmgr.c bufmgr.h
	$(CC) $(CFLAGS) -o $@ test_flush_pin.c $(LDFLAGS)

test-flush-pin: test_flush_pin
	./test_flush_pin

test_trickler_select: test_trickler_select.c bufmgr.c bufmgr.h
	$(CC) $(CFLAGS) -o $@ test_trickler_select.c $(LDFLAGS)

test-trickler-select: test_trickler_select
	./test_trickler_select

test_pp_slice: test_pp_slice.c bufmgr.c bufmgr.h
	$(CC) $(CFLAGS) -o $@ test_pp_slice.c $(LDFLAGS)

test_tr_slice: test_tr_slice.c bufmgr.c bufmgr.h
	$(CC) $(CFLAGS) -o $@ test_tr_slice.c $(LDFLAGS)

test-tr-slice: test_tr_slice
	TMPDIR=$${TMPDIR:-/tmp} ./test_tr_slice

test-pp-slice: test_pp_slice
	./test_pp_slice

test_btree_late_load: test_btree_late_load.c bufmgr.c bufmgr.h btree.c btnode.c
	$(CC) $(CFLAGS) -o $@ test_btree_late_load.c btree.c btnode.c $(LDFLAGS)

test-btree-late-load: test_btree_late_load
	./test_btree_late_load

test_evict_revalidate: test_evict_revalidate.c bufmgr.c bufmgr.h
	$(CC) $(CFLAGS) -o $@ test_evict_revalidate.c $(LDFLAGS)

test_evict_revalidate_sampled: test_evict_revalidate.c bufmgr.c bufmgr.h
	$(CC) $(CFLAGS) -DBM_SAMPLED_EVICT -o $@ test_evict_revalidate.c $(LDFLAGS)

test-evict-revalidate: test_evict_revalidate test_evict_revalidate_sampled
	./test_evict_revalidate
	./test_evict_revalidate_sampled

# Protection-work bound in a controlled all-hot, reclaimable pool.
# This does not bound total allocation latency under pins or I/O.
test_sweep_bound: test_sweep_bound.c bufmgr.o
	$(CC) $(CFLAGS) -o $@ test_sweep_bound.c bufmgr.o $(LDFLAGS)

test-sweep-bound: test_sweep_bound
	./test_sweep_bound

# ---- persistence: superblock reopen + WAL truncation ----
test_persist: test_persist.c bufmgr.o btree.o btnode.o wal.o
	$(CC) $(CFLAGS) -o $@ test_persist.c bufmgr.o btree.o btnode.o wal.o $(LDFLAGS)

test-persist: test_persist
	./test_persist

# ---- latch-releasing, position-revalidating cursor (O(1) scans) ----
test_cursor_resume: test_cursor_resume.c bufmgr.o btree.o btnode.o
	$(CC) $(CFLAGS) -o $@ test_cursor_resume.c bufmgr.o btree.o btnode.o $(LDFLAGS)

test-cursor-resume: test_cursor_resume
	./test_cursor_resume

# ---- clean-restart fast path: trust a cleanly shut-down base ----
test_clean_restart: test_clean_restart.c engine.o db.o quack.o xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o $(LIME_OBJ) $(VEXEC_OBJ)
	$(CC) $(CFLAGS) -o $@ test_clean_restart.c engine.o db.o quack.o xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o $(LIME_OBJ) $(VEXEC_OBJ) $(LDFLAGS)

test-clean-restart: test_clean_restart
	./test_clean_restart

# ---- STEAL: large transaction spills to disk; undo on crash with CLRs ----
test_steal: test_steal.c engine.o db.o quack.o xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o $(LIME_OBJ) $(VEXEC_OBJ)
	$(CC) $(CFLAGS) -o $@ test_steal.c engine.o db.o quack.o xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o $(LIME_OBJ) $(VEXEC_OBJ) $(LDFLAGS)

test-steal: test_steal
	./test_steal

test_steal_page: test_steal_page.c engine.o db.o quack.o metrics.o xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o $(LIME_OBJ) $(VEXEC_OBJ)
	$(CC) $(CFLAGS) -o $@ test_steal_page.c engine.o db.o quack.o metrics.o xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o $(LIME_OBJ) $(VEXEC_OBJ) $(LDFLAGS)

test-steal-page: test_steal_page
	./test_steal_page

# ---- Increment 3: physiological non-split-leaf logging -> torn-leaf
# in-place recovery is durable + atomic ----
test_steal_leaf: test_steal_leaf.c engine.o db.o quack.o metrics.o xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o $(LIME_OBJ) $(VEXEC_OBJ)
	$(CC) $(CFLAGS) -o $@ test_steal_leaf.c engine.o db.o quack.o metrics.o xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o $(LIME_OBJ) $(VEXEC_OBJ) $(LDFLAGS)

test-steal-leaf: test_steal_leaf
	./test_steal_leaf

test_ckpt_online: test_ckpt_online.c engine.o db.o quack.o metrics.o xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o $(LIME_OBJ) $(VEXEC_OBJ)
	$(CC) $(CFLAGS) -o $@ test_ckpt_online.c engine.o db.o quack.o metrics.o xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o $(LIME_OBJ) $(VEXEC_OBJ) $(LDFLAGS)

test-ckpt-online: test_ckpt_online
	TMPDIR=$${TMPDIR:-/tmp} ./test_ckpt_online

test_wal_group_sync: test_wal_group_sync.c wal.o
	$(CC) $(CFLAGS) -o $@ test_wal_group_sync.c wal.o $(LDFLAGS)

test-wal-group-sync: test_wal_group_sync
	TMPDIR=$${TMPDIR:-/tmp} ./test_wal_group_sync

test_intents_thread: test_intents_thread.c engine.o db.o quack.o metrics.o xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o $(LIME_OBJ) $(VEXEC_OBJ)
	$(CC) $(CFLAGS) -o $@ test_intents_thread.c engine.o db.o quack.o metrics.o xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o $(LIME_OBJ) $(VEXEC_OBJ) $(LDFLAGS)

test-intents-thread: test_intents_thread
	TMPDIR=$${TMPDIR:-/tmp} ./test_intents_thread

test_wal_prealloc: test_wal_prealloc.c wal.c wal.h
	$(CC) $(CFLAGS) -o $@ test_wal_prealloc.c wal.c $(LDFLAGS)

test-wal-prealloc: test_wal_prealloc
	TMPDIR=$${TMPDIR:-/tmp} ./test_wal_prealloc

test_cat_reopen: test_cat_reopen.c engine.o db.o quack.o metrics.o xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o $(LIME_OBJ) $(VEXEC_OBJ)
	$(CC) $(CFLAGS) -o $@ test_cat_reopen.c engine.o db.o quack.o metrics.o xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o $(LIME_OBJ) $(VEXEC_OBJ) $(LDFLAGS)

test-cat-reopen: test_cat_reopen
	TMPDIR=$${TMPDIR:-/tmp} ./test_cat_reopen

test_torn_base: test_torn_base.c engine.o db.o quack.o metrics.o xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o $(LIME_OBJ) $(VEXEC_OBJ)
	$(CC) $(CFLAGS) -o $@ test_torn_base.c engine.o db.o quack.o metrics.o xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o $(LIME_OBJ) $(VEXEC_OBJ) $(LDFLAGS)

test-torn-base: test_torn_base
	TMPDIR=$${TMPDIR:-/tmp} ./test_torn_base

test_snap_registry: test_snap_registry.c engine.o db.o quack.o metrics.o xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o $(LIME_OBJ) $(VEXEC_OBJ)
	$(CC) $(CFLAGS) -o $@ test_snap_registry.c engine.o db.o quack.o metrics.o xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o $(LIME_OBJ) $(VEXEC_OBJ) $(LDFLAGS)

test-snap-registry: test_snap_registry
	TMPDIR=$${TMPDIR:-/tmp} ./test_snap_registry

test_rowid_minmax: test_rowid_minmax.c engine.o db.o quack.o metrics.o xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o $(LIME_OBJ) $(VEXEC_OBJ)
	$(CC) $(CFLAGS) -o $@ test_rowid_minmax.c engine.o db.o quack.o metrics.o xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o $(LIME_OBJ) $(VEXEC_OBJ) $(LDFLAGS)

test-rowid-minmax: test_rowid_minmax
	TMPDIR=$${TMPDIR:-/tmp} ./test_rowid_minmax

test_wal_bigtxn: test_wal_bigtxn.c engine.o db.o quack.o metrics.o xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o $(LIME_OBJ) $(VEXEC_OBJ)
	$(CC) $(CFLAGS) -o $@ test_wal_bigtxn.c engine.o db.o quack.o metrics.o xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o $(LIME_OBJ) $(VEXEC_OBJ) $(LDFLAGS)

test-wal-bigtxn: test_wal_bigtxn
	TMPDIR=$${TMPDIR:-/tmp} ./test_wal_bigtxn

test_steal_reclaim: test_steal_reclaim.c engine.o db.o quack.o metrics.o xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o $(LIME_OBJ) $(VEXEC_OBJ)
	$(CC) $(CFLAGS) -o $@ test_steal_reclaim.c engine.o db.o quack.o metrics.o xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o $(LIME_OBJ) $(VEXEC_OBJ) $(LDFLAGS)

test-steal-reclaim: test_steal_reclaim
	TMPDIR=$${TMPDIR:-/tmp} ./test_steal_reclaim

test_dio_restart: test_dio_restart.c engine.o db.o quack.o metrics.o xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o $(LIME_OBJ) $(VEXEC_OBJ)
	$(CC) $(CFLAGS) -o $@ test_dio_restart.c engine.o db.o quack.o metrics.o xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o $(LIME_OBJ) $(VEXEC_OBJ) $(LDFLAGS)

test-dio-restart: test_dio_restart
	TMPDIR=$${TMPDIR:-/tmp} ./test_dio_restart

# ---- B-tree node (prefix compression) unit test (standalone) ----
test_btnode: test_btnode.c btnode.c btnode.h
	$(CC) $(CFLAGS) -o $@ test_btnode.c btnode.c

test-btnode: test_btnode
	./test_btnode

# ---- B-tree (on bufmgr + btnode) test (in-process, no daemon) ----
test_btree: test_btree.c btree.c bufmgr.o btnode.c
	$(CC) $(CFLAGS) -o $@ test_btree.c btree.c bufmgr.o btnode.c $(LDFLAGS)

test_btree_fill: test_btree_fill.c btree.c bufmgr.o btnode.c
	$(CC) $(CFLAGS) -o $@ test_btree_fill.c btree.c bufmgr.o btnode.c $(LDFLAGS)

test-btree-fill: test_btree_fill
	TMPDIR=$${TMPDIR:-/tmp} ./test_btree_fill

test-btree: test_btree
	./test_btree

# ---- B-tree on a loop with offloaded async I/O + live page-provider ----
test_btree_loop: test_btree_loop.c btree.c bufmgr.o btnode.c
	$(CC) $(CFLAGS) -o $@ test_btree_loop.c btree.c bufmgr.o btnode.c $(LDFLAGS)

test-btree-loop: test_btree_loop
	./test_btree_loop

# ---- concurrent multi-writer/multi-reader B-tree on a 4-thread executor ----
test_btree_mt: test_btree_mt.c btree.c bufmgr.o btnode.c
	$(CC) $(CFLAGS) -o $@ test_btree_mt.c btree.c bufmgr.o btnode.c $(LDFLAGS)

test-btree-mt: test_btree_mt
	./test_btree_mt

# ---- leaf merge + page reclaim on delete (height shrink, no bloat) ----
test_btree_delete_merge: test_btree_delete_merge.c btree.c bufmgr.o btnode.c
	$(CC) $(CFLAGS) -o $@ test_btree_delete_merge.c btree.c bufmgr.o btnode.c $(LDFLAGS)

test-btree-delete-merge: test_btree_delete_merge
	./test_btree_delete_merge

# Entry-size bounds + byte-balanced split (RethinkDB leaf_epsilon rule):
# L1-L5 regressions of 1.56 and a mixed-size property run.
test: test-btree-limits
test_btree_limits: test_btree_limits.c btree.c bufmgr.o btnode.c
	$(CC) $(CFLAGS) -o $@ test_btree_limits.c btree.c bufmgr.o btnode.c $(LDFLAGS)

test-btree-limits: test_btree_limits
	./test_btree_limits

# Key-distribution sample (RethinkDB get_distribution): order, range,
# O(fanout^depth) page bound, equi-depth balance on skewed keys.
test: test-btree-sample
test_btree_sample: test_btree_sample.c btree.c bufmgr.o btnode.c
	$(CC) $(CFLAGS) -o $@ test_btree_sample.c btree.c bufmgr.o btnode.c $(LDFLAGS)

test-btree-sample: test_btree_sample
	./test_btree_sample



test-quack: ../../test/sqlxtc/test_quack
	../../test/sqlxtc/test_quack

test-oracle: $(TARGET)
	bash ../../test/sqlxtc/test_sqlxtc_oracle.sh

test-compat: $(TARGET)
	bash ../../test/sqlxtc/test_sqlxtc_compat.sh

../../test/sqlxtc/test_quack: ../../test/sqlxtc/test_quack.c quack.o
	$(CC) $(CFLAGS) -o $@ $< quack.o

test-sql-parse: ../../test/sqlxtc/test_sql_parse
	../../test/sqlxtc/test_sql_parse

../../test/sqlxtc/test_sql_parse: ../../test/sqlxtc/test_sql_parse.c \
        sql_parse.o $(LIME_OBJ)
	$(CC) $(CFLAGS) -o $@ $< sql_parse.o $(LIME_OBJ)

.PHONY: test-quack test-quack-range test-sql-parse-par-ord-join-run test-oracle test-compat

# ---- NUMA buffer-pool sharing-degree PREMISE PROBE (research spike) ----
# Builds bufmgr with the BM_ACCESS_PROBE hook enabled and links the probe.
bufmgr_probe.o: bufmgr.c bufmgr.h
	$(CC) $(CFLAGS) -DBM_ACCESS_PROBE=1 -c -o $@ bufmgr.c

# numa_claim_probe is a RESEARCH SPIKE, not shipped example code, and it
# uses the internal __os_* topology surface deliberately -- so it lives in
# bench/, not in examples/ (which is the consumer exemplar and must stay
# clean under test/dist/test_api_discipline.sh).
numa_claim_probe: ../../bench/sqlxtc/numa_claim_probe.c btree.c bufmgr_probe.o btnode.c
	$(CC) $(CFLAGS) -o $@ ../../bench/sqlxtc/numa_claim_probe.c btree.c bufmgr_probe.o btnode.c $(LDFLAGS)

# ---- eviction-policy research prototype (standalone, no libxtc/bufmgr) ----
# Self-contained proving ground for the NUMA buffer-pool eviction policy
# (sampled power-of-D victim selection, reversible cooling ring, W-TinyLFU
# admission + doorkeeper).  Pure policy module over a synthetic trace;
# self-checks the design's key properties with plain assert().
evict_policy_proto: evict_policy_proto.c
	$(CC) $(CFLAGS) -o $@ evict_policy_proto.c

test: test-btree-prefix-split

# Prefix-compressed leaves (index keys) must split, fill, and keep every
# entry: the split used to cost entries with their full key.
test_btree_prefix_split: test_btree_prefix_split.c engine.o db.o quack.o metrics.o xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o $(LIME_OBJ) $(VEXEC_OBJ)
	$(CC) $(CFLAGS) -o $@ test_btree_prefix_split.c engine.o db.o quack.o metrics.o xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o $(LIME_OBJ) $(VEXEC_OBJ) $(LDFLAGS)

test-btree-prefix-split: test_btree_prefix_split
	TMPDIR=$${TMPDIR:-/tmp} ./test_btree_prefix_split

test: test-rec-codec

# The row record codec: round trip of every class and width edge, the
# size of a TPC-C row, 1.59 records still decode, truncations in bounds.
test_rec_codec: test_rec_codec.c engine.o db.o quack.o metrics.o xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o $(LIME_OBJ) $(VEXEC_OBJ)
	$(CC) $(CFLAGS) -o $@ test_rec_codec.c engine.o db.o quack.o metrics.o xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o $(LIME_OBJ) $(VEXEC_OBJ) $(LDFLAGS)

test-rec-codec: test_rec_codec
	./test_rec_codec

test: test-idx-elem

# Index key elements of numbers: memcmp order == SQLite's numeric order
# (INT and REAL, every width edge, asc and DESC), the element sizes, a
# UNIQUE index keeping INT64_MAX and 2^63 apart, seeks == SQLite's rows.
test_idx_elem: test_idx_elem.c engine.o db.o quack.o metrics.o xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o $(LIME_OBJ) $(VEXEC_OBJ)
	$(CC) $(CFLAGS) -o $@ test_idx_elem.c engine.o db.o quack.o metrics.o xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o $(LIME_OBJ) $(VEXEC_OBJ) $(LDFLAGS)

test-idx-elem: test_idx_elem
	TMPDIR=$${TMPDIR:-/tmp} ./test_idx_elem

# ---- store size attribution (offline, read-only) ----
# sx_size STORE.xstore [STORE.xstore-wal]: bytes per table / index /
# out-of-line page / unreachable page, fill factors, record encodings,
# and the log by record type.  Run it on a closed store.
sx_size: sx_size.c xstore.o btnode.o wal.o xlog.o engine.o db.o quack.o metrics.o btree.o bufmgr.o $(LIME_OBJ) $(VEXEC_OBJ)
	$(CC) $(CFLAGS) -o $@ sx_size.c xstore.o btnode.o wal.o xlog.o engine.o db.o quack.o metrics.o btree.o bufmgr.o $(LIME_OBJ) $(VEXEC_OBJ) $(LDFLAGS)
