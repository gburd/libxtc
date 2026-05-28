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

SRCS = main.c quack.c db.c conn.c xtc_mutex.c metrics.c sql_parse.c
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

$(TARGET): $(OBJS) sqlite/sqlite3.o $(LIME_OBJ)
	$(CC) $(CFLAGS) -o $@ $(OBJS) sqlite/sqlite3.o $(LIME_OBJ) $(LDFLAGS)

# SQLite amalgamation: build once, slow.
sqlite/sqlite3.o: sqlite/sqlite3.c sqlite/sqlite3.h
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
db.o: db.c db.h quack.h
conn.o: conn.c conn.h db.h quack.h sql_parse.h
xtc_mutex.o: xtc_mutex.c
metrics.o: metrics.c
sql_parse.o: sql_parse.c sql_parse.h
sql_parse_drv.o: sql_parse_drv.c sql_parse.h sql_parse_lime.h sql_parse_gen.h

clean:
	rm -f $(OBJS) $(TARGET)
	rm -f sql_parse_gen.o sql_parse_drv.o
	rm -f /tmp/sqlxtc-bench.db /tmp/sqlxtc-bench.db-*

distclean: clean
	rm -f sqlite/sqlite3.o
	-$(MAKE) -C lime clean 2>/dev/null || true
	rm -f sql_parse_gen.c sql_parse_gen.h sql_grammar.out

test: $(TARGET) test-quack test-sql-parse
	@bash ../../test/sqlxtc/test_sqlxtc_smoke.sh

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
