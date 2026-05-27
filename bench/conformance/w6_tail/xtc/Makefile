#-
# Copyright (c) 2026, The XTC Project
# Use of this source code is governed by the ISC License.
#
# bench/conformance/w6_tail/xtc/Makefile
#   Build the W6 tail-latency-under-backpressure benchmark for the xtc
#   runtime.
#
# Paths (override on the command line if the build tree is elsewhere):
#   XTC_BUILD  -- directory containing libxtc.a and the generated Makefile
#   HIST_DIR   -- directory containing hist.h and hist.c
#
# Usage:
#   make                       # build ./bench with defaults
#   make clean                 # remove ./bench
#   XTC_BUILD=/other/path make # custom build directory

CC ?= cc

XTC_ROOT  ?= ../../../..
XTC_BUILD ?= $(XTC_ROOT)/build_unix
HIST_DIR  ?= ../../include

# ---------------------------------------------------------------------------
# Extract the liburing include path and link flags from the xtc build
# Makefile so we do not duplicate Nix store paths here.  Skip -lssl and
# -lcrypto: the backpressure benchmark uses no TLS, and those libraries
# may not be on the linker path in all build environments.
# ---------------------------------------------------------------------------
_URING_INC  := $(shell grep -m1 '^CFLAGS' '$(XTC_BUILD)/Makefile' 2>/dev/null \
                    | tr ' ' '\n' | grep -m1 '^-I.*uring' || true)
_URING_LIBS := $(shell grep -m1 '^LIBS' '$(XTC_BUILD)/Makefile' 2>/dev/null \
                    | sed 's/^[^=]*=[[:space:]]*//' \
                    | tr ' ' '\n' | grep 'uring' | tr '\n' ' ' || true)

CFLAGS += -O3 -std=c11 -Wall -Wextra \
           -I$(XTC_ROOT)/src/inc \
           -I$(HIST_DIR) \
           $(_URING_INC)

# ---------------------------------------------------------------------------
# Targets
# ---------------------------------------------------------------------------
.PHONY: all clean

all: bench

# Compile main.c and hist.c together; hist.c provides HIST_IMPLEMENTATION
# so main.c need not define it itself.
bench: main.c $(HIST_DIR)/hist.c $(XTC_BUILD)/libxtc.a
	$(CC) $(CFLAGS) $(CPPFLAGS) \
	    -o bench \
	    main.c \
	    $(HIST_DIR)/hist.c \
	    $(XTC_BUILD)/libxtc.a \
	    -pthread \
	    $(_URING_LIBS)

clean:
	rm -f bench
