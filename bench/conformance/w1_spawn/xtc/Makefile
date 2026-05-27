#-
# Copyright (c) 2026, The XTC Project — All rights reserved.
# Use of this source code is governed by the ISC License.
#
# bench/conformance/w1_spawn/xtc/Makefile
#   Builds the W1 xtc benchmark binary ("bench").
#
# Usage:
#   make                     # builds ./bench
#   make clean
#   XTC_BUILD=/path/to/build make   # override build dir (default: ../../../../build_unix)
#
# Dependency on liburing is discovered by parsing the liburing flags
# from the parent build's Makefile.

XTC_BUILD ?= ../../../../build_unix
XTC_INC   ?= ../../../../src/inc
CONF_INC  ?= ../../include

CC       = cc
CFLAGS   = -O3 -std=c11 -Wall -Wextra -Wpedantic \
           -I$(XTC_INC) -I$(CONF_INC)

# Extract the liburing -I flag from the parent Makefile's CFLAGS line.
LIBURING_INC := $(shell \
    awk '/^CFLAGS[[:space:]]*=/{print}' $(XTC_BUILD)/Makefile \
    | grep -o '\-I[^ ]*liburing[^ ]*' \
    | head -1)

# Extract the liburing -L / -luring / -Wl,-rpath flags from LIBS.
LIBURING_LDFLAGS := $(shell \
    awk '/^LIBS[[:space:]]*=/{sub(/^LIBS[[:space:]]*=[[:space:]]*/,""); print}' \
    $(XTC_BUILD)/Makefile)

CFLAGS  += $(LIBURING_INC)
LDFLAGS  = $(XTC_BUILD)/libxtc.a -pthread $(LIBURING_LDFLAGS)

.PHONY: all clean

all: bench

bench: main.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f bench
