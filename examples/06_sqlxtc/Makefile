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

LDFLAGS  = $(XTC_LDFLAGS) $(XTC_BUILD)/libxtc.a -pthread -ldl -lm \
           $(LIBURING_LDFLAGS)

# SQLite amalgamation flags: we want the recommended set plus a few
# size/perf knobs.  Keep this list consistent with sqlite3.h-aligned
# defaults.
SQLITE_CFLAGS = -O2 -g -std=c11 -D_GNU_SOURCE \
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

SRCS = main.c quack.c db.c conn.c engine.c mutex.c mem.c vfs.c pcache.c metrics.c sql_parse.c
OBJS = $(SRCS:.c=.o)

# Lime-generated parser (Phase 2).  Compiled in if the file exists.
LIME_OBJ :=
ifneq ($(wildcard sql_parse_gen.c),)
LIME_OBJ := sql_parse_gen.o sql_parse_drv.o
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

# Generate sql_parse_gen.c (and .h) from sql_grammar.lime via Lime.
# Lime names outputs after the input basename; we rename to *_gen.*
# so they don't collide with the hand-written sql_parse.[ch].
sql_parse_gen.c sql_parse_gen.h: sql_grammar.lime lime/bin/lime
	-./lime/bin/lime -T./lime/limpar.c sql_grammar.lime
	@test -f sql_grammar.c || { echo "lime failed to produce sql_grammar.c"; exit 1; }
	@mv -f sql_grammar.c sql_parse_gen.c
	@mv -f sql_grammar.h sql_parse_gen.h
	@rm -f sql_grammar.out

sql_parse: sql_parse_gen.c

# Dependencies
main.o: main.c conn.h db.h
quack.o: quack.c quack.h
db.o: db.c db.h quack.h engine.h
conn.o: conn.c conn.h db.h quack.h sql_parse.h engine.h
engine.o: engine.c engine.h vfs.h sqlite3.h
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
	rm -f test_mem test_mem.o
	rm -f test_wal test_wal.o wal.o
	rm -f test_parallel test_parallel.o
	rm -f test_bufmgr test_bufmgr.o bufmgr.o
	rm -f test_bufmgr_mt
	rm -f test_btnode
	rm -f test_btree
	rm -f test_btree_loop
	rm -f test_btree_mt
	rm -f test_vfs_loop test_vfs_loop.o
	rm -rf $(AMALG_DIR)
	rm -f sql_parse_gen.o sql_parse_drv.o
	rm -f /tmp/sqlxtc-bench.db /tmp/sqlxtc-bench.db-*

distclean: clean
	rm -f sqlite3.o
	-$(MAKE) -C lime clean 2>/dev/null || true
	rm -f sql_parse_gen.c sql_parse_gen.h sql_grammar.out

test: $(TARGET) test-quack test-sql-parse test-vfs test-vfs-loop test-pcache test-concurrency test-mem test-wal test-parallel test-bufmgr test-bufmgr-mt test-btnode test-btree test-btree-loop test-btree-mt
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

# ---- xtc-allocator SQLite-memory seam test (in-process, no daemon) ----
# Installs the xtc allocator under an __os_alloc counting hook, then
# drives a real SQL workload: proves the engine's allocations flow
# through xtc (hook count > 0) and that the workload is correct.
test_mem: test_mem.c mem.o mutex.o engine.o vfs.o sqlite3.o
	$(CC) $(CFLAGS) -o $@ test_mem.c mem.o mutex.o engine.o vfs.o sqlite3.o $(LDFLAGS)

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

# ---- sx_ engine parallel-execution test (in-process, no daemon) ----
test_parallel: test_parallel.c engine.o vfs.o sqlite3.o
	$(CC) $(CFLAGS) -o $@ test_parallel.c engine.o vfs.o sqlite3.o $(LDFLAGS)

test-parallel: test_parallel
	./test_parallel

# ---- LeanStore-style buffer manager test (in-process, no daemon) ----
test_bufmgr: test_bufmgr.c bufmgr.o
	$(CC) $(CFLAGS) -o $@ test_bufmgr.c bufmgr.o $(LDFLAGS)

test-bufmgr: test_bufmgr
	./test_bufmgr

bufmgr.o: bufmgr.c bufmgr.h

# ---- buffer-manager multi-threaded stress test (executor; no daemon) ----
test_bufmgr_mt: test_bufmgr_mt.c bufmgr.o
	$(CC) $(CFLAGS) -o $@ test_bufmgr_mt.c bufmgr.o $(LDFLAGS)

test-bufmgr-mt: test_bufmgr_mt
	./test_bufmgr_mt

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

.PHONY: test-quack test-sql-parse
