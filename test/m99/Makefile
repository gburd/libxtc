#-
# Copyright (c) 2026, The XTC Project -- All rights reserved.
# Use of this source code is governed by the ISC License.
#
# test/m99/Makefile
#	Build tests for the Redis-compatible server example.

XTC_BUILD ?= ../../build_unix
XTC_INC   ?= ../../src/inc
REXIS_DIR ?= ../../examples/05_rexis

CC       = cc
CFLAGS   = -O2 -g -std=c11 -Wall -Wextra -Wpedantic \
           -D_GNU_SOURCE \
           -I$(XTC_INC) -I$(REXIS_DIR) -I.

# Extract liburing flags
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

# Proto object file from redis example
PROTO_OBJ = $(REXIS_DIR)/proto.o

TESTS = test_resp_parser test_rexis_loopback test_rexis_budgets test_rexis_pbt

.PHONY: all clean check FORCE

all: $(TESTS)

test_resp_parser: test_resp_parser.o munit.o $(PROTO_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_rexis_loopback: test_rexis_loopback.o munit.o
	$(CC) $(CFLAGS) -o $@ $^ -lpthread

test_rexis_budgets: test_rexis_budgets.o munit.o
	$(CC) $(CFLAGS) -o $@ $^ -lpthread

test_rexis_pbt: test_rexis_pbt.o munit.o
	$(CC) $(CFLAGS) -o $@ $^ -lpthread

test_rexis_loopback.o test_rexis_budgets.o test_rexis_pbt.o: rexis_harness.h

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Always rebuilt through the example's own Makefile (so an edit to
# proto.c is never tested against a stale proto.o).
$(PROTO_OBJ): FORCE
	$(MAKE) -C $(REXIS_DIR) XTC_BUILD=$(abspath $(XTC_BUILD)) proto.o
FORCE:

clean:
	rm -f *.o $(TESTS)

# Build the server, then run every m99 test.  Any failure fails the
# target.  (It used to append `|| echo "(some tests may fail...)"` to
# each server test, so the budget tests could fail -- or hang -- and
# `make check` still reported success.)  Each server test is bounded by
# `timeout` as defence in depth; the harness already bounds every wait.
TIMEOUT ?= timeout 300

check: all
	$(MAKE) -C $(REXIS_DIR) XTC_BUILD=$(abspath $(XTC_BUILD))
	cd $(REXIS_DIR) && ./test_bitcask && ./test_metrics
	./test_resp_parser
	$(TIMEOUT) ./test_rexis_loopback
	$(TIMEOUT) ./test_rexis_budgets
	$(TIMEOUT) ./test_rexis_pbt
	XTC_SRC_DIR=$(abspath ../..) sh ./test_rexis_persist.sh
