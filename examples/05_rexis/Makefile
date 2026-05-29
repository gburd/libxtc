#-
# Copyright (c) 2026, The XTC Project -- All rights reserved.
# Use of this source code is governed by the ISC License.
#
# examples/05_rexis/Makefile
#	Builds the rexis (Redis-protocol-compatible) server.
#
# Usage:
#   make                         # builds ./rexis-server-xtc
#   make clean
#   make test                    # run basic sanity test
#   XTC_BUILD=/path/to/build make   # override build dir

XTC_BUILD ?= ../../build_unix
XTC_INC   ?= ../../src/inc

CC       = cc
CFLAGS   = -O2 -g -std=c11 -Wall -Wextra -Wpedantic \
           -D_GNU_SOURCE \
           -I$(XTC_INC) -I.

# Extract the liburing flags from the parent build if available
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
LDFLAGS  = $(XTC_LDFLAGS) $(XTC_BUILD)/libxtc.a -pthread $(LIBURING_LDFLAGS)

SRCS = main.c proto.c db.c cmd.c conn.c expire.c metrics.c bitcask.c
OBJS = $(SRCS:.c=.o)
TARGET = rexis-server-xtc

.PHONY: all clean test

all: $(TARGET) test_bitcask test_db_persist

test_bitcask: test_bitcask.c bitcask.c bitcask.h
	$(CC) $(CFLAGS) -o $@ test_bitcask.c bitcask.c -lpthread

test_db_persist: test_db_persist.c db.c bitcask.c db.h bitcask.h
	$(CC) $(CFLAGS) -o $@ test_db_persist.c db.c proto.c expire.c bitcask.c $(LDFLAGS)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $(OBJS) $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Dependencies
main.o: main.c proto.h db.h cmd.h conn.h
proto.o: proto.c proto.h
db.o: db.c db.h bitcask.h
cmd.o: cmd.c cmd.h proto.h db.h
conn.o: conn.c conn.h proto.h cmd.h db.h
expire.o: expire.c db.h
metrics.o: metrics.c db.h

clean:
	rm -f $(OBJS) $(TARGET) test_bitcask test_db_persist

# Basic sanity test: start server, ping, stop
test: $(TARGET)
	@echo "=== Starting server in background ==="
	./$(TARGET) -p 16379 &
	@sleep 1
	@echo "=== Sending PING ==="
	echo -e '*1\r\n$$4\r\nPING\r\n' | nc -q1 localhost 16379 || true
	@echo ""
	@echo "=== Sending SET/GET ==="
	echo -e '*3\r\n$$3\r\nSET\r\n$$3\r\nfoo\r\n$$3\r\nbar\r\n*2\r\n$$3\r\nGET\r\n$$3\r\nfoo\r\n' | nc -q1 localhost 16379 || true
	@echo ""
	@echo "=== Stopping server ==="
	-pkill -f './$(TARGET) -p 16379' 2>/dev/null || true
	@echo "=== Test complete ==="
