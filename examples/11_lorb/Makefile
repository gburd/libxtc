#-
# Copyright (c) 2026, The XTC Project
# Use of this source code is governed by the ISC License.
#
# examples/11_lorb/Makefile -- builds the lorb limit order book.
#
# Targets:
#   make            build the lorb driver + unit tests
#   make test       build and run the unit tests
#   make clean      remove objects + binaries
#
# Override XTC_BUILD to point at a different libxtc build dir.

XTC_BUILD ?= ../../build_unix
XTC_INC   ?= ../../src/inc

CC       = cc
CFLAGS   = -O2 -g -std=c11 -Wall -Wextra -Wpedantic \
           -D_GNU_SOURCE \
           -I$(XTC_INC) -I.

LIBURING_INC := $(shell \
    [ -f $(XTC_BUILD)/Makefile ] && \
    awk '/^CFLAGS[[:space:]]*=/{print}' $(XTC_BUILD)/Makefile 2>/dev/null \
    | grep -o '\-I[^ ]*liburing[^ ]*' | head -1)

XTC_LDFLAGS := $(shell \
    [ -f $(XTC_BUILD)/Makefile ] && \
    awk '/^LDFLAGS[[:space:]]*=/{sub(/^LDFLAGS[[:space:]]*=[[:space:]]*/,""); print}' \
    $(XTC_BUILD)/Makefile 2>/dev/null)

LIBS := $(shell \
    [ -f $(XTC_BUILD)/Makefile ] && \
    awk '/^LIBS[[:space:]]*=/{sub(/^LIBS[[:space:]]*=[[:space:]]*/,""); print}' \
    $(XTC_BUILD)/Makefile 2>/dev/null)

CFLAGS  += $(LIBURING_INC)
LDFLAGS  = -pthread $(XTC_BUILD)/libxtc.a $(XTC_LDFLAGS) $(LIBS)

TARGET  = lorb
TESTS   = test_lob

.PHONY: all clean test

all: $(TARGET) $(TESTS)

$(TARGET): driver.c lob.c lob.h
	$(CC) $(CFLAGS) -o $@ driver.c lob.c $(LDFLAGS) -lm

test_lob: test_lob.c lob.c lob.h t_assert.h
	$(CC) $(CFLAGS) -o $@ test_lob.c lob.c $(LDFLAGS)

test: $(TESTS)
	./test_lob

clean:
	rm -f $(TARGET) $(TESTS) *.o
	rm -rf *.dSYM
