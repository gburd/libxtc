#-
# Copyright (c) 2026, The XTC Project -- All rights reserved.
# Use of this source code is governed by the ISC License.
#
# examples/08_tnt/Makefile -- builds the canonical tnt echo server.
#
# tnt is now a SUPPORTED part of libxtc (src/orc/tnt.c, public header
# src/inc/xtc_tnt.h).  This directory keeps only the example: a TCP
# echo server that links the in-library tnt L4 layer.
#
# Targets:
#   make            build the echo server (tnt_echo)
#   make echo-test  build and run the TCP echo round-trip test
#   make clean      remove the binary
#
# Override XTC_BUILD to point at a different libxtc build dir.

XTC_BUILD ?= ../../build_unix
XTC_INC   ?= ../../src/inc

CC       = cc
CFLAGS   = -O2 -g -std=c11 -Wall -Wextra -Wpedantic \
           -D_GNU_SOURCE \
           -I$(XTC_INC) -I.

# Pull liburing include flags from the parent build.
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

.PHONY: all clean echo-test

all: tnt_echo

# The canonical TCP echo server.  Links the in-library tnt L4 layer.
tnt_echo: echo.c
	$(CC) $(CFLAGS) -o $@ echo.c $(LDFLAGS)

# End-to-end TCP echo round-trip test (spawns the server, connects,
# verifies echo).  Driven by the in-tree shell harness.
echo-test: tnt_echo
	XTC_BUILD_DIR=. bash ../../test/tnt/test_tnt_echo.sh

clean:
	rm -f tnt_echo
