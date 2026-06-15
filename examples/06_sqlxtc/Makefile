#-
# Copyright (c) 2026, The XTC Project -- All rights reserved.
# Use of this source code is governed by the ISC License.
#
# examples/06_sqlxtc/Makefile -- builds sqlxtc-server.
#
# Targets:
#   make            build sqlxtc-server (sqlite3.o is built once, ~30s)
#   make lime       build the bundled Lime parser generator (Phase 2)
#   make sql_parse  generate sql_parse_gen.c via Lime
#   make clean      remove objects + binary
#   make distclean  also remove sqlite3.o and Lime artifacts
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

# Vendored-SQL-engine symbol map (docs/M_SQLXTC_NAMING.md): force-include
# xsql.h into every translation unit so the public sqlite3_* API the
# seams use resolves to project-native xsql_* names.  Harmless in units
# that touch no SQL.
CFLAGS  += -include xsql.h

LDFLAGS  = $(XTC_LDFLAGS) $(XTC_BUILD)/libxtc.a -pthread -ldl -lm \
           $(LIBURING_LDFLAGS)

# SQLite amalgamation flags: we want the recommended set plus a few
# size/perf knobs.  Keep this list consistent with sqlite3.h-aligned
# defaults.
SQLITE_CFLAGS = -O2 -g -std=c11 -D_GNU_SOURCE \
    -include xsql.h \
    -DSQLITE_THREADSAFE=1 \
    -DSQLITE_DEFAULT_MEMSTATUS=0 \
    -DSQLITE_DEFAULT_WAL_SYNCHRONOUS=1 \
    -DSQLITE_LIKE_DOESNT_MATCH_BLOBS \
    -DSQLITE_MAX_EXPR_DEPTH=0 \
    -DSQLITE_OMIT_DEPRECATED \
    -DSQLITE_OMIT_PROGRESS_CALLBACK \
    -DSQLITE_OMIT_SHARED_CACHE \
    -DSQLITE_USE_ALLOCA \
    -DSQLITE_ENABLE_JSON1 \
    -Wno-unused-but-set-variable \
    -Wno-unused-function \
    -Wno-unused-parameter \
    -Wno-implicit-fallthrough

SRCS = main.c quack.c db.c conn.c engine.c mutex.c mem.c vfs.c pcache.c metrics.c sql_parse.c admin.c xstore.c xlog.c wal.c bufmgr.c btree.c btnode.c
OBJS = $(SRCS:.c=.o)

# Lime-generated parser (Phase 2).  Compiled in if the file exists.
LIME_OBJ :=
ifneq ($(wildcard sql_parse_gen.c),)
LIME_OBJ := sql_parse_gen.o sql_parse_drv.o sql_ast.o
CFLAGS  += -DSQLXTC_HAVE_LIME=1
endif

TARGET = sqlxtc-server

.PHONY: all clean distclean test lime sql_parse

all: $(TARGET)

$(TARGET): $(OBJS) sqlite3.o $(LIME_OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJS) sqlite3.o $(LIME_OBJ) $(LDFLAGS)

# SQLite amalgamation: build once, slow.
sqlite3.o: sqlite3.c sqlite3.h
	$(CC) $(SQLITE_CFLAGS) -c -o $@ $<

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
engine.o: engine.c engine.h vfs.h xstore.h btree.h bufmgr.h sqlite3.h
mutex.o: mutex.c
mem.o: mem.c mem.h sqlite3.h
vfs.o: vfs.c vfs.h sqlite3.h
pcache.o: pcache.c pcache.h sqlite3.h
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
	rm -f test_mem test_mem.o
	rm -f test_wal test_wal.o wal.o
	rm -f test_wal_recover
	rm -f test_torn_smo
	rm -f test_wal_compact
	rm -f test_shard test_shard.o
	rm -f test_mvcc test_mvcc.o mvcc.o
	rm -f test_xstore test_xstore.o xstore.o
	rm -f test_xlog test_xlog.o xlog.o
	rm -f test_recover_undo
	rm -f test_redo_page
	rm -f test_inplace_redo
	rm -f test_clean_restart
	rm -f test_steal
	rm -f test_parallel test_parallel.o
	rm -f test_bufmgr test_bufmgr.o bufmgr.o test_bufmgr_dio test_bufmgr_dio.o
	rm -f test_bufmgr_mt
	rm -f test_scan_resist
	rm -f test_persist
	rm -f test_cursor_resume
	rm -f test_server_storage
	rm -f test_btnode
	rm -f test_btree
	rm -f test_btree_loop
	rm -f test_btree_mt
	rm -f test_btree_delete_merge test_savepoint test_isolation
	rm -f test_vfs_loop test_vfs_loop.o
	rm -rf $(AMALG_DIR)
	rm -f sql_parse_gen.o sql_parse_drv.o
	rm -f /tmp/sqlxtc-bench.db /tmp/sqlxtc-bench.db-*

distclean: clean
	rm -f sqlite3.o
	rm -rf *.dSYM
	rm -rf ../../test/sqlxtc/test_quack ../../test/sqlxtc/*.dSYM
	-$(MAKE) -C lime clean 2>/dev/null || true
	rm -f sql_parse_gen.c sql_parse_gen.h sql_grammar.out

test: $(TARGET) test-quack test-sql-parse test-sql-ast test-sqlrec test-vfs test-vfs-loop test-pcache test-concurrency test-admin test-mem test-wal test-wal-recover test-torn-smo test-wal-compact test-shard test-mvcc test-xstore test-savepoint test-isolation test-xlog test-recover-undo test-parallel test-bufmgr test-redo-page test-inplace-redo test-bufmgr-mt test-scan-resist test-persist test-cursor-resume test-server-storage test-clean-restart test-steal test-btnode test-btree test-btree-loop test-btree-mt test-btree-delete-merge
	@bash ../../test/sqlxtc/test_sqlxtc_smoke.sh

# ---- amalgamation build ----
# Builds sqlxtc against the single-file xtc amalgamation instead of
# libxtc.a, which exercises the amalgamation as a real, broad
# consumer (app, io, log, lrlock, lwlock, proc, res, stats, net, plus
# the __os_* helpers).  The sqlxtc sources are compiled in a separate
# amalgamation mode (-Iamalg/include resolves the forwarding stubs to
# the single xtc.h); SQLite is reused as-is.  No libxtc.a, no
# liburing: the amalgamation auto-selects the epoll backend.
AMALG_DIR    = amalg
AMALG_CFLAGS = -O2 -g -std=c11 -Wall -Wextra -Wpedantic -D_GNU_SOURCE \
               -include xsql.h \
               -I$(AMALG_DIR)/include -I.
AMALG_OBJS   = $(SRCS:.c=.amalg.o)

.PHONY: amalg amalg-test

$(AMALG_DIR)/xtc.c:
	python3 ../../dist/mkamalgamation.py --root ../.. --out $(AMALG_DIR)

$(AMALG_DIR)/xtc.o: $(AMALG_DIR)/xtc.c
	$(CC) $(AMALG_CFLAGS) -c -o $@ $(AMALG_DIR)/xtc.c

%.amalg.o: %.c $(AMALG_DIR)/xtc.c
	$(CC) $(AMALG_CFLAGS) -c -o $@ $<

sqlxtc-server-amalg: $(AMALG_OBJS) sqlite3.o $(AMALG_DIR)/xtc.o
	$(CC) $(AMALG_CFLAGS) -o $@ $(AMALG_OBJS) sqlite3.o \
		$(AMALG_DIR)/xtc.o -pthread -ldl -lm

amalg: sqlxtc-server-amalg

amalg-test: sqlxtc-server-amalg
	@SQLXTC_SERVER=$(CURDIR)/sqlxtc-server-amalg \
		bash ../../test/sqlxtc/test_sqlxtc_smoke.sh

# ---- xtc VFS test (in-process, no daemon) ----
# Builds the VFS test against both libxtc.a and the amalgamation, so
# the custom sqlite3_vfs is exercised in both link configurations.
.PHONY: test-vfs test-vfs-amalg

test_vfs: test_vfs.c vfs.o sqlite3.o
	$(CC) $(CFLAGS) -o $@ test_vfs.c vfs.o sqlite3.o $(LDFLAGS)

test-vfs: test_vfs
	./test_vfs

# ---- xtc VFS on-loop blocking-offload test (in-process, no daemon) ----
test_vfs_loop: test_vfs_loop.c vfs.o sqlite3.o
	$(CC) $(CFLAGS) -o $@ test_vfs_loop.c vfs.o sqlite3.o $(LDFLAGS)

test-vfs-loop: test_vfs_loop
	./test_vfs_loop

test_vfs_amalg: test_vfs.c vfs.amalg.o sqlite3.o \
        $(AMALG_DIR)/xtc.o
	$(CC) $(AMALG_CFLAGS) -o $@ test_vfs.c vfs.amalg.o \
		sqlite3.o $(AMALG_DIR)/xtc.o -pthread -ldl -lm

test-vfs-amalg: test_vfs_amalg
	./test_vfs_amalg

# ---- xtc page-cache test (in-process, no daemon) ----
.PHONY: test-pcache test-pcache-amalg

test_pcache: test_pcache.c pcache.o sqlite3.o
	$(CC) $(CFLAGS) -o $@ test_pcache.c pcache.o sqlite3.o $(LDFLAGS)

test-pcache: test_pcache
	./test_pcache

# ---- xtc_amutex SQLite-mutex concurrency test (in-process, no daemon) ----
test_concurrency: test_concurrency.c mutex.o sqlite3.o
	$(CC) $(CFLAGS) -o $@ test_concurrency.c mutex.o sqlite3.o $(LDFLAGS)

test-concurrency: test_concurrency
	./test_concurrency

# ---- SHOW PROCESSES admin command test (in-process, no daemon) ----
# Links only admin.o + quack.o against libxtc.a; no SQLite needed --
# admin walks the live proc set via xtc_inspect_procs.
test_admin: test_admin.c admin.o quack.o
	$(CC) $(CFLAGS) -o $@ test_admin.c admin.o quack.o $(LDFLAGS)

test-admin: test_admin
	./test_admin

# ---- xtc-allocator SQLite-memory seam test (in-process, no daemon) ----
# Installs the xtc allocator under an __os_alloc counting hook, then
# drives a real SQL workload: proves the engine's allocations flow
# through xtc (hook count > 0) and that the workload is correct.
test_mem: test_mem.c mem.o mutex.o engine.o vfs.o xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o sqlite3.o
	$(CC) $(CFLAGS) -o $@ test_mem.c mem.o mutex.o engine.o vfs.o xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o sqlite3.o $(LDFLAGS)

test-mem: test_mem
	./test_mem

# ---- group-commit WAL writer process (in-process, no daemon) ----
# Many committer procs drive concurrent commits at one writer; proves
# fsync coalescing, durable monotonic LSNs, and log replay -- on a
# single loop and on a 4-loop executor.  Storage-engine stage 1
# (docs/M_SQLXTC_SCALEOUT.md).
test_wal: test_wal.c wal.c wal.h
	$(CC) $(CFLAGS) -o $@ test_wal.c wal.c $(LDFLAGS)

test-wal: test_wal
	./test_wal

# ---- WAL durability + crash recovery for the xstore SQL engine ----
test_wal_recover: test_wal_recover.c xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o sqlite3.o
	$(CC) $(CFLAGS) -o $@ test_wal_recover.c xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o sqlite3.o $(LDFLAGS)

test-wal-recover: test_wal_recover
	./test_wal_recover

test_torn_smo: test_torn_smo.c xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o sqlite3.o
	$(CC) $(CFLAGS) -o $@ test_torn_smo.c xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o sqlite3.o $(LDFLAGS)

test-torn-smo: test_torn_smo
	./test_torn_smo

test_wal_compact: test_wal_compact.c xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o sqlite3.o
	$(CC) $(CFLAGS) -o $@ test_wal_compact.c xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o sqlite3.o $(LDFLAGS)

test-wal-compact: test_wal_compact
	./test_wal_compact

# ---- share-nothing sharded store + control plane (executor; no daemon) ----
# N shard servers (one xtc_svr each, share-nothing) + a global
# timestamp allocator; clients route by key-hash.  Scale-out stage 2/3
# (docs/M_SQLXTC_SCALEOUT.md): single loop and 4-loop executor.
test_shard: test_shard.c
	$(CC) $(CFLAGS) -o $@ test_shard.c $(LDFLAGS)

test-shard: test_shard
	./test_shard

# ---- MVCC + cross-shard 2PC (executor; no daemon) ----
# Snapshot-isolation versioned shards + a deferred-reply 2PC
# coordinator + per-shard HLC.  Scale-out stage 4
# (docs/M_SQLXTC_STAGE4.md): snapshot isolation, cross-shard
# atomicity, and write-write conflict detection on a 4-loop executor.
test_mvcc: test_mvcc.c mvcc.c mvcc.h
	$(CC) $(CFLAGS) -o $@ test_mvcc.c mvcc.c $(LDFLAGS)

test-mvcc: test_mvcc
	./test_mvcc

# ---- SQL on the libxtc-native storage via the xstore vtable ----
# SQLite's VDBE runs SQL; rows live in our btree/bufmgr (larger than
# RAM).  Off-loop and on-loop (VDBE parks on offloaded page I/O
# mid-statement).  See docs/M_SQLXTC_MVCC_SQL.md.
test_xstore: test_xstore.c xstore.c xlog.c wal.c btree.c btnode.c bufmgr.o sqlite3.o
	$(CC) $(CFLAGS) -o $@ test_xstore.c xstore.c xlog.c wal.c btree.c btnode.c bufmgr.o sqlite3.o $(LDFLAGS)

test-xstore: test_xstore
	./test_xstore

# ---- Lime SQL parser: differential AST oracle vs the reference engine ----
test_sql_ast: test_sql_ast.c sql_parse_drv.c sql_ast.c sql_parse_gen.c sqlite3.o
	$(CC) $(CFLAGS) -o $@ test_sql_ast.c sql_parse_drv.c sql_ast.c \
		sql_parse_gen.c sqlite3.o $(LDFLAGS)

test-sql-ast: test_sql_ast
	./test_sql_ast

# ---- record codec: byte-for-byte SQLite-format encode/decode ----
test_sqlrec: test_sqlrec.c sqlrec.c sqlite3.o
	$(CC) $(CFLAGS) -o $@ test_sqlrec.c sqlrec.c sqlite3.o $(LDFLAGS)

test-sqlrec: test_sqlrec
	./test_sqlrec

test_savepoint: test_savepoint.c xstore.c xlog.c wal.c btree.c btnode.c bufmgr.o sqlite3.o
	$(CC) $(CFLAGS) -o $@ test_savepoint.c xstore.c xlog.c wal.c btree.c btnode.c bufmgr.o sqlite3.o $(LDFLAGS)

test-savepoint: test_savepoint
	./test_savepoint

test_isolation: test_isolation.c xstore.c xlog.c wal.c btree.c btnode.c bufmgr.o sqlite3.o
	$(CC) $(CFLAGS) -o $@ test_isolation.c xstore.c xlog.c wal.c btree.c btnode.c bufmgr.o sqlite3.o $(LDFLAGS)

test-isolation: test_isolation
	./test_isolation

# ---- ARIES log-record codec unit test (pure, no I/O) ----
test_xlog: test_xlog.c xlog.c
	$(CC) $(CFLAGS) -o $@ test_xlog.c xlog.c $(LDFLAGS)

test-xlog: test_xlog
	./test_xlog

# ---- recovery undo pass + CLR writing (synthetic loser log) ----
test_recover_undo: test_recover_undo.c xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o sqlite3.o
	$(CC) $(CFLAGS) -o $@ test_recover_undo.c xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o sqlite3.o $(LDFLAGS)

test-recover-undo: test_recover_undo
	./test_recover_undo

# ---- sx_ engine parallel-execution test (in-process, no daemon) ----
test_parallel: test_parallel.c engine.o vfs.o xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o sqlite3.o
	$(CC) $(CFLAGS) -o $@ test_parallel.c engine.o vfs.o xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o sqlite3.o $(LDFLAGS)

test-parallel: test_parallel
	./test_parallel

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

# ---- physiological redo apply (page-LSN gated image write) ----
test_redo_page: test_redo_page.c bufmgr.o
	$(CC) $(CFLAGS) -o $@ test_redo_page.c bufmgr.o $(LDFLAGS)

test-redo-page: test_redo_page
	./test_redo_page

# ---- end-to-end in-place physiological crash recovery (XL_PAGE images) ----
test_inplace_redo: test_inplace_redo.c xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o sqlite3.o
	$(CC) $(CFLAGS) -o $@ test_inplace_redo.c xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o sqlite3.o $(LDFLAGS)

test-inplace-redo: test_inplace_redo
	./test_inplace_redo

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

# ---- server storage wiring + connection-per-proc (in-process) ----
test_server_storage: test_server_storage.c engine.o db.o quack.o vfs.o xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o sqlite3.o
	$(CC) $(CFLAGS) -o $@ test_server_storage.c engine.o db.o quack.o vfs.o xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o sqlite3.o $(LDFLAGS)

test-server-storage: test_server_storage
	./test_server_storage

# ---- clean-restart fast path: trust a cleanly shut-down base ----
test_clean_restart: test_clean_restart.c engine.o db.o quack.o vfs.o xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o sqlite3.o
	$(CC) $(CFLAGS) -o $@ test_clean_restart.c engine.o db.o quack.o vfs.o xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o sqlite3.o $(LDFLAGS)

test-clean-restart: test_clean_restart
	./test_clean_restart

# ---- STEAL: large transaction spills to disk; undo on crash with CLRs ----
test_steal: test_steal.c engine.o db.o quack.o vfs.o xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o sqlite3.o
	$(CC) $(CFLAGS) -o $@ test_steal.c engine.o db.o quack.o vfs.o xstore.o xlog.o wal.o btree.o btnode.o bufmgr.o sqlite3.o $(LDFLAGS)

test-steal: test_steal
	./test_steal

# ---- B-tree node (prefix compression) unit test (standalone) ----
test_btnode: test_btnode.c btnode.c btnode.h
	$(CC) $(CFLAGS) -o $@ test_btnode.c btnode.c

test-btnode: test_btnode
	./test_btnode

# ---- B-tree (on bufmgr + btnode) test (in-process, no daemon) ----
test_btree: test_btree.c btree.c bufmgr.o btnode.c
	$(CC) $(CFLAGS) -o $@ test_btree.c btree.c bufmgr.o btnode.c $(LDFLAGS)

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

test_pcache_amalg: test_pcache.c pcache.amalg.o sqlite3.o \
        $(AMALG_DIR)/xtc.o
	$(CC) $(AMALG_CFLAGS) -o $@ test_pcache.c pcache.amalg.o \
		sqlite3.o $(AMALG_DIR)/xtc.o -pthread -ldl -lm

test-pcache-amalg: test_pcache_amalg
	./test_pcache_amalg

test-quack: ../../test/sqlxtc/test_quack
	../../test/sqlxtc/test_quack

../../test/sqlxtc/test_quack: ../../test/sqlxtc/test_quack.c quack.o
	$(CC) $(CFLAGS) -o $@ $< quack.o

test-sql-parse: ../../test/sqlxtc/test_sql_parse
	../../test/sqlxtc/test_sql_parse

../../test/sqlxtc/test_sql_parse: ../../test/sqlxtc/test_sql_parse.c \
        sql_parse.o $(LIME_OBJ)
	$(CC) $(CFLAGS) -o $@ $< sql_parse.o $(LIME_OBJ)

.PHONY: test-quack test-sql-parse test-sql-ast test-sqlrec
