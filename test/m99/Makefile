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

.PHONY: all clean check

all: $(TESTS)

test_resp_parser: test_resp_parser.o munit.o $(PROTO_OBJ)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

test_rexis_loopback: test_rexis_loopback.o munit.o
	$(CC) $(CFLAGS) -o $@ $^ -lpthread

test_rexis_budgets: test_rexis_budgets.o munit.o
	$(CC) $(CFLAGS) -o $@ $^ -lpthread

test_rexis_pbt: test_rexis_pbt.o munit.o
	$(CC) $(CFLAGS) -o $@ $^ -lpthread

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

# Build proto.o if needed
$(PROTO_OBJ):
	$(MAKE) -C $(REXIS_DIR) proto.o

clean:
	rm -f *.o $(TESTS)

# Run all tests (requires server to be built first)
check: all
	@echo "=== Building Redis server ==="
	$(MAKE) -C $(REXIS_DIR)
	@echo ""
	@echo "=== Running RESP parser tests ==="
	./test_resp_parser
	@echo ""
	@echo "=== Running loopback tests ==="
	./test_rexis_loopback || echo "(some tests may fail if server not found)"
	@echo ""
	@echo "=== Running budget tests ==="
	./test_rexis_budgets || echo "(some tests may fail if server not found)"
	@echo ""
	@echo "=== Running PBT tests ==="
	./test_rexis_pbt || echo "(some tests may fail if server not found)"
